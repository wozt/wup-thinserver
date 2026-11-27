#define _GNU_SOURCE
#include "settings.h"
#include "input.h"
#include "camera_frame.h"
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <json-glib/json-glib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

enum {
    ACTION_DISABLED, ACTION_KEY, ACTION_MOUSE_LEFT, ACTION_MOUSE_MIDDLE,
    ACTION_MOUSE_RIGHT, ACTION_WHEEL_UP, ACTION_WHEEL_DOWN,
    ACTION_KEYBOARD_TOGGLE, ACTION_ALT_TAB, ACTION_PREV_TAB, ACTION_NEXT_TAB,
    ACTION_ZOOM_IN, ACTION_ZOOM_OUT, ACTION_SUPER, ACTION_FULLSCREEN,
    ACTION_ENTER, ACTION_ESCAPE, ACTION_LEFT, ACTION_RIGHT, ACTION_UP,
    ACTION_DOWN, ACTION_F1, ACTION_F2, ACTION_F3, ACTION_F4, ACTION_F5,
    ACTION_F6, ACTION_F7, ACTION_F8, ACTION_F9, ACTION_F10, ACTION_F11,
    ACTION_F12, ACTION_ADVANCED
};

static const char *button_names[WBTN_COUNT] = {
    "A", "B", "X", "Y", "D-pad left", "D-pad right", "D-pad up",
    "D-pad down", "ZL", "ZR", "L", "R", "Plus", "Minus", "HOME",
    "L3", "R3", "TV"
};

static const uint32_t button_bits[WBTN_COUNT] = {
    WUPSTREAM_A, WUPSTREAM_B, WUPSTREAM_X, WUPSTREAM_Y,
    WUPSTREAM_LEFT, WUPSTREAM_RIGHT, WUPSTREAM_UP, WUPSTREAM_DOWN,
    WUPSTREAM_ZL, WUPSTREAM_ZR, WUPSTREAM_L, WUPSTREAM_R,
    WUPSTREAM_PLUS, WUPSTREAM_MINUS, WUPSTREAM_HOME,
    WUPSTREAM_L3, WUPSTREAM_R3, WUPSTREAM_TV
};

typedef struct {
    GtkWidget *combo;
    GtkWidget *entry;
} BindingRow;

typedef struct {
    GtkWidget *window, *status, *connection, *monitor;
    GtkWidget *mode, *mode_status, *conn_status, *deadzone, *sensitivity, *invert_y, *touch_mouse;
    GtkWidget *gyro_mode, *gyro_sens, *left_mode, *right_mode, *brightness;
    GtkWidget *brightness_value, *left_keys_box, *right_keys_box;
    GtkWidget *stick_entries[8];
    GtkWidget *microphone, *volume;
    GtkWidget *camera_announce, *camera_preview, *camera_device;
    GtkWidget *camera_picture, *camera_status;
    BindingRow bindings[WBTN_COUNT];
    input_cfg_t input;
    int audio_pad_to_pc, pad_volume_controls_pc;
    int pad_brightness;      /* -1 auto, 1..6 discrete levels */
    int camera_announce_enabled, camera_preview_enabled;
    char camera_device_path[128];
    char camera_preview_path[256];
    int camera_frame_fd;
    camera_frame_shared_t *camera_frame;
    unsigned char *camera_pixels;
    uint32_t camera_sequence;
    gboolean camera_pixels_valid;
    guint brightness_flush;  /* debounce id for live slider changes */
    gboolean loading;
} App;

static const char *action_key(int action) {
    static const char *keys[] = {
        [ACTION_ALT_TAB] = "Alt+Tab", [ACTION_PREV_TAB] = "Ctrl+Page_Up",
        [ACTION_NEXT_TAB] = "Ctrl+Page_Down", [ACTION_ZOOM_IN] = "Ctrl+plus",
        [ACTION_ZOOM_OUT] = "Ctrl+minus", [ACTION_SUPER] = "Super_L",
        [ACTION_FULLSCREEN] = "F11", [ACTION_ENTER] = "Return",
        [ACTION_ESCAPE] = "Escape", [ACTION_LEFT] = "Left",
        [ACTION_RIGHT] = "Right", [ACTION_UP] = "Up", [ACTION_DOWN] = "Down",
        [ACTION_F1] = "F1", [ACTION_F2] = "F2", [ACTION_F3] = "F3",
        [ACTION_F4] = "F4", [ACTION_F5] = "F5", [ACTION_F6] = "F6",
        [ACTION_F7] = "F7", [ACTION_F8] = "F8", [ACTION_F9] = "F9",
        [ACTION_F10] = "F10", [ACTION_F11] = "F11", [ACTION_F12] = "F12"
    };
    return action >= 0 && action < (int)G_N_ELEMENTS(keys) ? keys[action] : NULL;
}

static int key_action(const char *key) {
    if (!key || !key[0]) return ACTION_DISABLED;
    for (int i = ACTION_ALT_TAB; i <= ACTION_F12; i++)
        if (action_key(i) && !strcmp(key, action_key(i))) return i;
    return ACTION_KEY;
}

static char *socket_path(void) {
    const char *runtime = g_getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0])
        return g_build_filename(runtime, "wup-thinserver", "control.sock", NULL);
    char *base = g_strdup_printf("/tmp/wup-thinserver-%u", (unsigned)getuid());
    char *path = g_build_filename(base, "wup-thinserver", "control.sock", NULL);
    g_free(base);
    return path;
}

static char *ipc_request(const char *request, GError **error) {
    char *path = socket_path();
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot create local socket: %s", g_strerror(errno));
        g_free(path); return NULL;
    }
    struct timeval timeout = { .tv_sec = 2 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof addr.sun_path) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                            "control socket path is too long");
        close(fd); g_free(path); return NULL;
    }
    g_strlcpy(addr.sun_path, path, sizeof addr.sun_path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot connect to ThinServer at %s: %s", path,
                    g_strerror(errno));
        close(fd); g_free(path); return NULL;
    }
    g_free(path);
    size_t len = strlen(request), off = 0;
    while (off < len) {
        ssize_t n = send(fd, request + off, len - off, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        off += (size_t)n;
    }
    send(fd, "\n", 1, MSG_NOSIGNAL);
    shutdown(fd, SHUT_WR);
    GString *reply = g_string_sized_new(4096);
    char buffer[4096];
    while (reply->len < 65536) {
        ssize_t n = recv(fd, buffer, sizeof buffer, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        g_string_append_len(reply, buffer, n);
        if (memchr(buffer, '\n', n)) break;
    }
    close(fd);
    if (!reply->len) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                            "ThinServer did not answer the local request");
        g_string_free(reply, TRUE); return NULL;
    }
    return g_string_free(reply, FALSE);
}

static gboolean response_root(const char *data, JsonParser **parser_out,
                              JsonObject **root_out, GError **error) {
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, data, -1, error)) {
        g_object_unref(parser); return FALSE;
    }
    JsonNode *node = json_parser_get_root(parser);
    if (!node || !JSON_NODE_HOLDS_OBJECT(node)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "ThinServer returned an invalid response");
        g_object_unref(parser); return FALSE;
    }
    JsonObject *root = json_node_get_object(node);
    if (!json_object_get_boolean_member_with_default(root, "ok", FALSE)) {
        const char *message = json_object_get_string_member_with_default(
            root, "error", "ThinServer rejected the request");
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
        g_object_unref(parser); return FALSE;
    }
    *parser_out = parser; *root_out = root;
    return TRUE;
}

static void set_status(App *app, const char *text, gboolean error) {
    char *markup = g_markup_printf_escaped("<span foreground=\"%s\">%s</span>",
                                           error ? "#ff9a76" : "#98c379", text);
    gtk_label_set_markup(GTK_LABEL(app->status), markup);
    g_free(markup);
}

static void parse_input_object(App *app, JsonObject *in) {
#define READ_INT(name) app->input.name = (int)json_object_get_int_member_with_default(in, #name, app->input.name)
    READ_INT(mode); READ_INT(deadzone); READ_INT(sensitivity); READ_INT(invert_y);
    READ_INT(gyro_mode); READ_INT(gyro_sens); READ_INT(touch_as_mouse);
    READ_INT(left_stick_mouse); READ_INT(right_stick_mouse);
    READ_INT(right_stick_scroll);
#undef READ_INT
    struct { const char *name; char *base; int count; size_t stride; } strings[] = {
        {"keys", (char *)app->input.keys, WBTN_COUNT, sizeof app->input.keys[0]},
        {"stick_keys", (char *)app->input.stick_keys, 8, sizeof app->input.stick_keys[0]},
        {"commands", (char *)app->input.commands, WBTN_COUNT, sizeof app->input.commands[0]}
    };
    for (guint n = 0; n < G_N_ELEMENTS(strings); n++) {
        JsonArray *a = json_object_get_array_member(in, strings[n].name);
        if (!a) continue;
        for (int i = 0; i < strings[n].count && i < (int)json_array_get_length(a); i++)
            g_strlcpy(strings[n].base + (size_t)i * strings[n].stride,
                      json_array_get_string_element(a, i), strings[n].stride);
    }
    JsonArray *mouse = json_object_get_array_member(in, "mouse_buttons");
    if (mouse)
        for (int i = 0; i < WBTN_COUNT && i < (int)json_array_get_length(mouse); i++)
            app->input.mouse_buttons[i] = (int)json_array_get_int_element(mouse, i);
}

static int binding_action(const input_cfg_t *input, int i) {
    if (input->commands[i][0])
        return !strcmp(input->commands[i], "onboard-toggle")
            ? ACTION_KEYBOARD_TOGGLE : ACTION_ADVANCED;
    switch (input->mouse_buttons[i]) {
    case 1: return ACTION_MOUSE_LEFT;
    case 2: return ACTION_MOUSE_MIDDLE;
    case 3: return ACTION_MOUSE_RIGHT;
    case 4: return ACTION_WHEEL_UP;
    case 5: return ACTION_WHEEL_DOWN;
    default: return key_action(input->keys[i]);
    }
}

static void action_changed(GtkComboBox *combo, gpointer data) {
    BindingRow *row = data;
    int action = gtk_combo_box_get_active(combo);
    gboolean key = action == ACTION_KEY;
    gtk_widget_set_sensitive(row->entry, key);
    if (!key) {
        const char *value = action_key(action);
        gtk_entry_set_text(GTK_ENTRY(row->entry), value ? value : "");
    }
}

static void stick_keys_visibility(GtkComboBox *combo, App *app);

static void widgets_from_config(App *app) {
    app->loading = TRUE;
    for (int i = 0; i < WBTN_COUNT; i++) {
        int action = binding_action(&app->input, i);
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->bindings[i].combo), action);
        gtk_entry_set_text(GTK_ENTRY(app->bindings[i].entry),
            action == ACTION_ADVANCED ? "Managed in settings.json" : app->input.keys[i]);
        action_changed(GTK_COMBO_BOX(app->bindings[i].combo), &app->bindings[i]);
    }
    gtk_range_set_value(GTK_RANGE(app->deadzone), app->input.deadzone);
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->mode), app->input.mode);
    gtk_range_set_value(GTK_RANGE(app->sensitivity), app->input.sensitivity);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->invert_y), app->input.invert_y);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->touch_mouse), app->input.touch_as_mouse);
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->gyro_mode), app->input.gyro_mode);
    gtk_range_set_value(GTK_RANGE(app->gyro_sens), app->input.gyro_sens);
    int left = app->input.left_stick_mouse ? 1 :
        (app->input.stick_keys[0][0] || app->input.stick_keys[1][0] ||
         app->input.stick_keys[2][0] || app->input.stick_keys[3][0]) ? 2 : 0;
    int right = app->input.right_stick_mouse ? 1 : app->input.right_stick_scroll ? 2 :
        (app->input.stick_keys[4][0] || app->input.stick_keys[5][0] ||
         app->input.stick_keys[6][0] || app->input.stick_keys[7][0]) ? 3 : 0;
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->left_mode), left);
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->right_mode), right);
    for (int i = 0; i < 8; i++)
        gtk_entry_set_text(GTK_ENTRY(app->stick_entries[i]), app->input.stick_keys[i]);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->microphone), app->audio_pad_to_pc);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->volume), app->pad_volume_controls_pc);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->camera_announce),
                                 app->camera_announce_enabled);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->camera_preview),
                                 app->camera_preview_enabled);
    gtk_entry_set_text(GTK_ENTRY(app->camera_device), app->camera_device_path);
    stick_keys_visibility(NULL, app);
    int brightness = app->pad_brightness < 0 ? 0 : app->pad_brightness;
    gtk_range_set_value(GTK_RANGE(app->brightness), brightness);
    char brightness_text[48];
    if (brightness == 0) g_strlcpy(brightness_text, "Auto (pad default)", sizeof brightness_text);
    else snprintf(brightness_text, sizeof brightness_text, "Level %d of 6", brightness);
    gtk_label_set_text(GTK_LABEL(app->brightness_value), brightness_text);
    app->loading = FALSE;
}

static gboolean load_current(App *app, gboolean quiet) {
    GError *error = NULL;
    char *reply = ipc_request("{\"op\":\"get\"}", &error);
    if (!reply) {
        if (!quiet) set_status(app, error->message, TRUE);
        g_clear_error(&error); return FALSE;
    }
    JsonParser *parser; JsonObject *root;
    if (!response_root(reply, &parser, &root, &error)) {
        if (!quiet) set_status(app, error->message, TRUE);
        g_clear_error(&error); g_free(reply); return FALSE;
    }
    JsonObject *config = json_object_get_object_member(root, "config");
    JsonObject *input = config ? json_object_get_object_member(config, "input") : NULL;
    if (input) parse_input_object(app, input);
    if (config) {
        app->audio_pad_to_pc = (int)json_object_get_int_member_with_default(config, "audio_pad_to_pc", 0);
        app->pad_volume_controls_pc = (int)json_object_get_int_member_with_default(config, "pad_volume_controls_pc", 1);
        app->pad_brightness = (int)json_object_get_int_member_with_default(config, "pad_brightness", -1);
        if (app->pad_brightness < -1 || app->pad_brightness > 6) app->pad_brightness = -1;
        app->camera_announce_enabled = json_object_get_boolean_member_with_default(
            config, "camera_announce", FALSE);
        app->camera_preview_enabled = json_object_get_boolean_member_with_default(
            config, "camera_preview", FALSE);
        g_strlcpy(app->camera_device_path,
            json_object_get_string_member_with_default(config, "camera_device", "/dev/video42"),
            sizeof app->camera_device_path);
    }
    widgets_from_config(app);
    if (!quiet) set_status(app, "Current settings loaded", FALSE);
    g_object_unref(parser); g_free(reply); return TRUE;
}

static void config_from_widgets(App *app) {
    for (int i = 0; i < WBTN_COUNT; i++) {
        int action = gtk_combo_box_get_active(GTK_COMBO_BOX(app->bindings[i].combo));
        if (action == ACTION_ADVANCED) continue;
        app->input.keys[i][0] = 0;
        app->input.commands[i][0] = 0;
        app->input.mouse_buttons[i] = 0;
        if (action == ACTION_KEY)
            g_strlcpy(app->input.keys[i], gtk_entry_get_text(GTK_ENTRY(app->bindings[i].entry)),
                      sizeof app->input.keys[i]);
        else if (action == ACTION_KEYBOARD_TOGGLE)
            g_strlcpy(app->input.commands[i], "onboard-toggle", sizeof app->input.commands[i]);
        else if (action >= ACTION_MOUSE_LEFT && action <= ACTION_WHEEL_DOWN)
            app->input.mouse_buttons[i] = action - ACTION_MOUSE_LEFT + 1;
        else if (action_key(action))
            g_strlcpy(app->input.keys[i], action_key(action), sizeof app->input.keys[i]);
    }
    app->input.deadzone = (int)gtk_range_get_value(GTK_RANGE(app->deadzone));
    app->input.mode = gtk_combo_box_get_active(GTK_COMBO_BOX(app->mode));
    app->input.sensitivity = (int)gtk_range_get_value(GTK_RANGE(app->sensitivity));
    app->input.invert_y = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->invert_y));
    app->input.touch_as_mouse = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->touch_mouse));
    app->input.gyro_mode = gtk_combo_box_get_active(GTK_COMBO_BOX(app->gyro_mode));
    app->input.gyro_sens = (int)gtk_range_get_value(GTK_RANGE(app->gyro_sens));
    int left = gtk_combo_box_get_active(GTK_COMBO_BOX(app->left_mode));
    int right = gtk_combo_box_get_active(GTK_COMBO_BOX(app->right_mode));
    app->input.left_stick_mouse = left == 1;
    app->input.right_stick_mouse = right == 1;
    app->input.right_stick_scroll = right == 2;
    for (int i = 0; i < 8; i++) app->input.stick_keys[i][0] = 0;
    if (left == 2) {
        static const char *fallback[] = {"a", "d", "w", "s"};
        for (int i = 0; i < 4; i++)
            g_strlcpy(app->input.stick_keys[i],
                gtk_entry_get_text(GTK_ENTRY(app->stick_entries[i]))[0]
                    ? gtk_entry_get_text(GTK_ENTRY(app->stick_entries[i])) : fallback[i],
                sizeof app->input.stick_keys[i]);
    }
    if (right == 3) {
        static const char *fallback[] = {"Left", "Right", "Up", "Down"};
        for (int i = 0; i < 4; i++)
            g_strlcpy(app->input.stick_keys[i + 4],
                gtk_entry_get_text(GTK_ENTRY(app->stick_entries[i + 4]))[0]
                    ? gtk_entry_get_text(GTK_ENTRY(app->stick_entries[i + 4])) : fallback[i],
                sizeof app->input.stick_keys[i + 4]);
    }
    app->audio_pad_to_pc = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->microphone));
    app->pad_volume_controls_pc = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->volume));
    app->camera_announce_enabled = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(app->camera_announce));
    app->camera_preview_enabled = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(app->camera_preview));
    g_strlcpy(app->camera_device_path,
              gtk_entry_get_text(GTK_ENTRY(app->camera_device)),
              sizeof app->camera_device_path);
}

static char *apply_request_json(App *app) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "op"); json_builder_add_string_value(b, "apply_input");
    json_builder_set_member_name(b, "audio_pad_to_pc"); json_builder_add_int_value(b, app->audio_pad_to_pc);
    json_builder_set_member_name(b, "pad_volume_controls_pc"); json_builder_add_int_value(b, app->pad_volume_controls_pc);
    json_builder_set_member_name(b, "camera_announce"); json_builder_add_boolean_value(b, app->camera_announce_enabled);
    json_builder_set_member_name(b, "camera_preview"); json_builder_add_boolean_value(b, app->camera_preview_enabled);
    json_builder_set_member_name(b, "camera_device"); json_builder_add_string_value(b, app->camera_device_path);
    json_builder_set_member_name(b, "input"); json_builder_begin_object(b);
#define ADD_INT(name) json_builder_set_member_name(b, #name); json_builder_add_int_value(b, app->input.name)
    ADD_INT(mode); ADD_INT(deadzone); ADD_INT(sensitivity); ADD_INT(invert_y); ADD_INT(gyro_mode);
    ADD_INT(gyro_sens); ADD_INT(touch_as_mouse); ADD_INT(left_stick_mouse);
    ADD_INT(right_stick_mouse); ADD_INT(right_stick_scroll);
#undef ADD_INT
    struct { const char *name; char *base; int count; size_t stride; } arrays[] = {
        {"keys", (char *)app->input.keys, WBTN_COUNT, sizeof app->input.keys[0]},
        {"stick_keys", (char *)app->input.stick_keys, 8, sizeof app->input.stick_keys[0]},
        {"commands", (char *)app->input.commands, WBTN_COUNT, sizeof app->input.commands[0]}
    };
    for (guint a = 0; a < G_N_ELEMENTS(arrays); a++) {
        json_builder_set_member_name(b, arrays[a].name); json_builder_begin_array(b);
        for (int i = 0; i < arrays[a].count; i++)
            json_builder_add_string_value(b, arrays[a].base + (size_t)i * arrays[a].stride);
        json_builder_end_array(b);
    }
    json_builder_set_member_name(b, "mouse_buttons"); json_builder_begin_array(b);
    for (int i = 0; i < WBTN_COUNT; i++) json_builder_add_int_value(b, app->input.mouse_buttons[i]);
    json_builder_end_array(b); json_builder_end_object(b); json_builder_end_object(b);
    JsonGenerator *g = json_generator_new(); JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(g, root); char *json = json_generator_to_data(g, NULL);
    json_node_free(root); g_object_unref(g); g_object_unref(b); return json;
}

static void apply_clicked(GtkButton *button, gpointer data) {
    (void)button; App *app = data; config_from_widgets(app);
    char *request = apply_request_json(app); GError *error = NULL;
    char *reply = ipc_request(request, &error); g_free(request);
    if (!reply) { set_status(app, error->message, TRUE); g_clear_error(&error); return; }
    JsonParser *parser; JsonObject *root;
    if (!response_root(reply, &parser, &root, &error)) {
        set_status(app, error->message, TRUE); g_clear_error(&error); g_free(reply); return;
    }
    set_status(app, "Settings applied live and saved", FALSE);
    g_object_unref(parser); g_free(reply);
}

static void reload_clicked(GtkButton *button, gpointer data) {
    (void)button; load_current(data, FALSE);
}

static void defaults_clicked(GtkButton *button, gpointer data) {
    (void)button; App *app = data; GError *error = NULL;
    char *reply = ipc_request("{\"op\":\"restore_defaults\"}", &error);
    if (!reply) { set_status(app, error->message, TRUE); g_clear_error(&error); return; }
    JsonParser *parser; JsonObject *root;
    if (!response_root(reply, &parser, &root, &error)) {
        set_status(app, error->message, TRUE); g_clear_error(&error); g_free(reply); return;
    }
    g_object_unref(parser); g_free(reply); load_current(app, TRUE);
    set_status(app, "Default input settings restored", FALSE);
}

static gboolean capture_key(GtkWidget *entry, GdkEventKey *event, gpointer data) {
    (void)data;
    if (event->keyval == GDK_KEY_Control_L || event->keyval == GDK_KEY_Control_R ||
        event->keyval == GDK_KEY_Shift_L || event->keyval == GDK_KEY_Shift_R ||
        event->keyval == GDK_KEY_Alt_L || event->keyval == GDK_KEY_Alt_R ||
        event->keyval == GDK_KEY_Super_L || event->keyval == GDK_KEY_Super_R)
        return TRUE;
    const char *name = gdk_keyval_name(event->keyval);
    if (!name) return TRUE;
    GString *value = g_string_new(NULL);
    if (event->state & GDK_CONTROL_MASK) g_string_append(value, "Ctrl+");
    if (event->state & GDK_MOD1_MASK) g_string_append(value, "Alt+");
    if (event->state & GDK_SHIFT_MASK) g_string_append(value, "Shift+");
    if (event->state & GDK_SUPER_MASK) g_string_append(value, "Super+");
    g_string_append(value, name);
    gtk_entry_set_text(GTK_ENTRY(entry), value->str);
    g_string_free(value, TRUE); return TRUE;
}

static void camera_frame_close(App *app) {
    if (app->camera_frame)
        munmap(app->camera_frame, sizeof *app->camera_frame);
    if (app->camera_frame_fd >= 0) close(app->camera_frame_fd);
    app->camera_frame = NULL;
    app->camera_frame_fd = -1;
    app->camera_sequence = 0;
    app->camera_pixels_valid = FALSE;
}

static gboolean camera_frame_open(App *app) {
    if (app->camera_frame || !app->camera_preview_path[0])
        return app->camera_frame != NULL;
    int fd = open(app->camera_preview_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info;
    if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != getuid() || (info.st_mode & 077) != 0 ||
        info.st_size != (off_t)sizeof(camera_frame_shared_t)) {
        if (fd >= 0) close(fd);
        return FALSE;
    }
    camera_frame_shared_t *frame = mmap(NULL, sizeof *frame, PROT_READ,
                                         MAP_SHARED, fd, 0);
    if (frame == MAP_FAILED) { close(fd); return FALSE; }
    if (frame->magic != WUP_CAMERA_FRAME_MAGIC ||
        frame->version != WUP_CAMERA_FRAME_VERSION ||
        frame->width != WUP_CAMERA_WIDTH ||
        frame->height != WUP_CAMERA_HEIGHT ||
        frame->stride != WUP_CAMERA_STRIDE) {
        munmap(frame, sizeof *frame); close(fd); return FALSE;
    }
    app->camera_frame_fd = fd;
    app->camera_frame = frame;
    return TRUE;
}

static gboolean camera_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    App *app = data;
    GtkAllocation area;
    gtk_widget_get_allocation(widget, &area);
    cairo_set_source_rgb(cr, 0.035, 0.065, 0.09);
    cairo_paint(cr);
    if (!app->camera_pixels_valid) {
        cairo_set_source_rgb(cr, 0.55, 0.64, 0.72);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 12);
        const char *message = app->camera_preview &&
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->camera_preview))
            ? "Waiting for GamePad camera" : "Preview is disabled";
        cairo_text_extents_t extents;
        cairo_text_extents(cr, message, &extents);
        cairo_move_to(cr, (area.width - extents.width) / 2.0 - extents.x_bearing,
                      (area.height - extents.height) / 2.0 - extents.y_bearing);
        cairo_show_text(cr, message);
        return FALSE;
    }
    cairo_surface_t *surface = cairo_image_surface_create_for_data(
        app->camera_pixels, CAIRO_FORMAT_ARGB32, WUP_CAMERA_WIDTH,
        WUP_CAMERA_HEIGHT, WUP_CAMERA_STRIDE);
    double scale = MIN((double)area.width / WUP_CAMERA_WIDTH,
                       (double)area.height / WUP_CAMERA_HEIGHT);
    double width = WUP_CAMERA_WIDTH * scale, height = WUP_CAMERA_HEIGHT * scale;
    cairo_save(cr);
    cairo_translate(cr, (area.width - width) / 2.0, (area.height - height) / 2.0);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(surface);
    return FALSE;
}

static gboolean preview_tick(gpointer data) {
    App *app = data;
    GdkWindow *window = app->window ? gtk_widget_get_window(app->window) : NULL;
    GdkWindowState state = window ? gdk_window_get_state(window) : GDK_WINDOW_STATE_WITHDRAWN;
    gboolean visible = app->window && app->camera_picture &&
        gtk_widget_get_mapped(app->window) && gtk_widget_get_mapped(app->camera_picture) &&
        !(state & (GDK_WINDOW_STATE_ICONIFIED | GDK_WINDOW_STATE_WITHDRAWN));
    gboolean enabled = app->camera_preview &&
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app->camera_preview));
    if (!visible || !enabled) {
        if (!enabled && (app->camera_frame || app->camera_pixels_valid)) {
            camera_frame_close(app);
            gtk_widget_queue_draw(app->camera_picture);
        }
        return G_SOURCE_CONTINUE;
    }
    if (!camera_frame_open(app)) return G_SOURCE_CONTINUE;
    camera_frame_shared_t *shared = app->camera_frame;
    uint32_t before = __atomic_load_n(&shared->sequence, __ATOMIC_ACQUIRE);
    if ((before & 1) || before == app->camera_sequence) return G_SOURCE_CONTINUE;
    if (!__atomic_load_n(&shared->valid, __ATOMIC_ACQUIRE)) {
        app->camera_sequence = before;
        app->camera_pixels_valid = FALSE;
        gtk_widget_queue_draw(app->camera_picture);
        return G_SOURCE_CONTINUE;
    }
    memcpy(app->camera_pixels, shared->bgra, WUP_CAMERA_PIXELS);
    uint32_t after = __atomic_load_n(&shared->sequence, __ATOMIC_ACQUIRE);
    if (before != after || (after & 1)) return G_SOURCE_CONTINUE;
    app->camera_sequence = after;
    app->camera_pixels_valid = TRUE;
    gtk_widget_queue_draw(app->camera_picture);
    return G_SOURCE_CONTINUE;
}

static gboolean monitor_tick(gpointer data) {
    App *app = data; GError *error = NULL;
    char *reply = ipc_request("{\"op\":\"ping\"}", &error);
    if (!reply) {
        gtk_label_set_text(GTK_LABEL(app->connection), "Daemon unavailable");
        camera_frame_close(app);
        app->camera_preview_path[0] = 0;
        if (app->camera_status)
            gtk_label_set_text(GTK_LABEL(app->camera_status), "Daemon unavailable");
        g_clear_error(&error); return G_SOURCE_CONTINUE;
    }
    JsonParser *parser; JsonObject *root;
    if (!response_root(reply, &parser, &root, &error)) {
        g_clear_error(&error); g_free(reply); return G_SOURCE_CONTINUE;
    }
    JsonObject *state = json_object_get_object_member(root, "state");
    const char *connection = json_object_get_string_member_with_default(state, "connection", "unknown");
    const char *status = json_object_get_string_member_with_default(state, "status", "");
    const char *input_status = json_object_get_string_member_with_default(
        state, "input_status", "Input mode status unavailable");
    gboolean gamepad_available = json_object_get_boolean_member_with_default(
        state, "gamepad_available", FALSE);
    char *mode_text = g_strdup_printf("%s%s", input_status,
        gamepad_available ? "" : " — /dev/uinput is not available");
    gtk_label_set_text(GTK_LABEL(app->mode_status), mode_text);
    g_free(mode_text);
    char *headline = g_strdup_printf("%s — %s", connection, status);
    gtk_label_set_text(GTK_LABEL(app->connection), headline);
    gtk_label_set_text(GTK_LABEL(app->conn_status), headline);
    g_free(headline);
    JsonObject *camera = json_object_get_object_member(state, "camera");
    if (camera) {
        const char *camera_text = json_object_get_string_member_with_default(
            camera, "error", "");
        if (!camera_text[0])
            camera_text = json_object_get_string_member_with_default(
                camera, "status", "Camera status unavailable");
        gtk_label_set_text(GTK_LABEL(app->camera_status), camera_text);
        const char *path = json_object_get_string_member_with_default(
            camera, "preview_path", "");
        if (strcmp(path, app->camera_preview_path)) {
            camera_frame_close(app);
            g_strlcpy(app->camera_preview_path, path,
                      sizeof app->camera_preview_path);
        }
    }
    if (json_object_get_boolean_member_with_default(state, "connected", FALSE)) {
        guint buttons = (guint)json_object_get_int_member_with_default(state, "buttons", 0);
        GString *pressed = g_string_new(""); gboolean any = FALSE;
        for (int i = 0; i < WBTN_COUNT; i++) if (buttons & button_bits[i]) {
            if (any) g_string_append(pressed, ", ");
            g_string_append(pressed, button_names[i]); any = TRUE;
        }
        if (!any) g_string_append(pressed, "none");
        g_string_append_printf(pressed,
            "  |  L(%d, %d)  R(%d, %d)  |  Touch: %s (%d, %d)  |  Vol %d  |  Mode: %s",
            (int)json_object_get_int_member_with_default(state, "lx", 0),
            (int)json_object_get_int_member_with_default(state, "ly", 0),
            (int)json_object_get_int_member_with_default(state, "rx", 0),
            (int)json_object_get_int_member_with_default(state, "ry", 0),
            json_object_get_int_member_with_default(state, "touched", 0) ? "down" : "up",
            (int)json_object_get_int_member_with_default(state, "tx", 0),
            (int)json_object_get_int_member_with_default(state, "ty", 0),
            (int)json_object_get_int_member_with_default(state, "volume", 0),
            json_object_get_string_member_with_default(state, "input_mode", "?"));
        if (json_object_has_member(state, "backlight"))
            g_string_append_printf(pressed, "  |  Backlight %d",
                (int)json_object_get_int_member(state, "backlight"));
        gtk_label_set_text(GTK_LABEL(app->monitor), pressed->str); g_string_free(pressed, TRUE);
    } else gtk_label_set_text(GTK_LABEL(app->monitor), "No controller input report available.");
    g_object_unref(parser); g_free(reply); return G_SOURCE_CONTINUE;
}

static GtkWidget *frame_new(const char *icon, const char *title) {
    GtkWidget *frame = gtk_frame_new(NULL), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    GtkWidget *image = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
    GtkWidget *label = gtk_label_new(title);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "section-title");
    gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_frame_set_label_widget(GTK_FRAME(frame), box);
    gtk_style_context_add_class(gtk_widget_get_style_context(frame), "settings-section");
    gtk_widget_set_hexpand(frame, TRUE);
    return frame;
}

static GtkWidget *combo_text(const char *const *items, int count) {
    GtkWidget *combo = gtk_combo_box_text_new();
    for (int i = 0; i < count; i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), items[i]);
    return combo;
}

static GtkWidget *check_wrapped(const char *text) {
    GtkWidget *button = gtk_check_button_new_with_label(text);
    GtkWidget *label = gtk_bin_get_child(GTK_BIN(button));
    if (GTK_IS_LABEL(label)) {
        gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 30);
    }
    return button;
}

/* GtkFlowBox ignores hexpand and keeps children at their natural width,
   which left a large empty strip inside the bindings section. Distribute
   the row width evenly over however many groups fit; below the minimum
   the flow box wraps them into a single column automatically. */
#define BINDINGS_MIN_GROUP 430
static void bindings_flow_allocate(GtkWidget *flow, GtkAllocation *alloc, gpointer data) {
    (void)data;
    int spacing = gtk_flow_box_get_column_spacing(GTK_FLOW_BOX(flow));
    GList *children = gtk_container_get_children(GTK_CONTAINER(flow));
    int count = g_list_length(children), each = 0;
    if (count > 0) {
        int candidate = (alloc->width - spacing * (count - 1)) / count;
        if (candidate >= BINDINGS_MIN_GROUP) each = candidate;
    }
    for (GList *l = children; l; l = l->next)
        gtk_widget_set_size_request(GTK_WIDGET(l->data), each > 0 ? each : -1, -1);
    g_list_free(children);
}

/* Bindings live in two sub-columns of nine rows inside one frame; the
   whole dashboard scrolls vertically instead of using tabs. */
static GtkWidget *bindings_frame(App *app) {
    static const char *actions[] = {
        "Disabled", "Keyboard key / shortcut", "Mouse: left button",
        "Mouse: middle button", "Mouse: right button", "Mouse wheel: up",
        "Mouse wheel: down", "Virtual keyboard toggle", "Shortcut: Alt+Tab",
        "Shortcut: previous tab", "Shortcut: next tab", "Shortcut: zoom in",
        "Shortcut: zoom out", "Key: Super", "Desktop: fullscreen", "Key: Enter",
        "Key: Escape", "Key: Left", "Key: Right", "Key: Up", "Key: Down",
        "Key: F1", "Key: F2", "Key: F3", "Key: F4", "Key: F5", "Key: F6",
        "Key: F7", "Key: F8", "Key: F9", "Key: F10", "Key: F11", "Key: F12",
        "Advanced command (settings file)"
    };
    /* Two groups of nine controls side by side inside a GtkFlowBox: at
       narrow widths the flow box stacks them vertically instead of
       clipping or forcing horizontal scrolling. */
    GtkWidget *frame = frame_new("input-gaming-symbolic", "Controller bindings");
    GtkWidget *flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 4);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 10);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), FALSE);
    gtk_widget_set_hexpand(flow, TRUE);
    g_signal_connect(flow, "size-allocate", G_CALLBACK(bindings_flow_allocate), NULL);
    for (int c = 0; c < 2; c++) {
        GtkWidget *grid = gtk_grid_new();
        gtk_widget_set_hexpand(grid, TRUE);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 5);
        gtk_container_set_border_width(GTK_CONTAINER(grid), 2);
        int x = 0;
        gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Control"), x, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Action"), x + 1, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(grid),
            gtk_label_new("Captured key / shortcut"), x + 2, 0, 1, 1);
        for (int i = 0; i < 9; i++) {
            int b = c * 9 + i, y = i + 1;
            GtkWidget *label = gtk_label_new(button_names[b]);
            gtk_widget_set_halign(label, GTK_ALIGN_START);
            app->bindings[b].combo = combo_text(actions, G_N_ELEMENTS(actions));
            app->bindings[b].entry = gtk_entry_new();
            gtk_widget_set_hexpand(app->bindings[b].entry, TRUE);
            gtk_entry_set_width_chars(GTK_ENTRY(app->bindings[b].entry), 12);
            gtk_entry_set_placeholder_text(GTK_ENTRY(app->bindings[b].entry),
                "Select, type, or press a key");
            g_signal_connect(app->bindings[b].combo, "changed",
                G_CALLBACK(action_changed), &app->bindings[b]);
            g_signal_connect(app->bindings[b].entry, "key-press-event",
                G_CALLBACK(capture_key), NULL);
            gtk_grid_attach(GTK_GRID(grid), label, x, y, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), app->bindings[b].combo, x + 1, y, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), app->bindings[b].entry, x + 2, y, 1, 1);
            if (b == WBTN_HOME)
                gtk_widget_set_tooltip_text(label, "HOME: a tap is forwarded to the desktop; "
                    "ThinClient keeps 5 s hold, HOME+X, HOME+Up/Down and HOME+B local.");
        }
        gtk_flow_box_insert(GTK_FLOW_BOX(flow), grid, -1);
    }
    gtk_container_add(GTK_CONTAINER(frame), flow);
    return frame;
}

static void grid_row(GtkGrid *grid, int row, const char *label, GtkWidget *widget) {
    GtkWidget *l = gtk_label_new(label); gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_grid_attach(grid, l, 0, row, 1, 1); gtk_grid_attach(grid, widget, 1, row, 1, 1);
}

static GtkWidget *mode_frame(App *app) {
    GtkWidget *frame = frame_new("video-display-symbolic", "Input mode & device");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 4);
    gtk_widget_set_hexpand(box, TRUE);
    static const char *input_modes[] = {"Gamepad (uinput / evdev)", "Desktop (keyboard and mouse)"};
    app->mode = combo_text(input_modes, 2);
    gtk_box_pack_start(GTK_BOX(box), app->mode, FALSE, FALSE, 0);
    app->mode_status = gtk_label_new("Checking virtual gamepad availability...");
    gtk_label_set_xalign(GTK_LABEL(app->mode_status), 0);
    gtk_label_set_line_wrap(GTK_LABEL(app->mode_status), TRUE);
    gtk_box_pack_start(GTK_BOX(box), app->mode_status, TRUE, TRUE, 0);
    app->conn_status = gtk_label_new("Connecting to daemon...");
    gtk_label_set_xalign(GTK_LABEL(app->conn_status), 1);
    gtk_label_set_ellipsize(GTK_LABEL(app->conn_status), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(box), app->conn_status, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), box);
    return frame;
}

/* Directional-key rows only exist while the matching stick actually uses
   keyboard directions; hidden boxes keep their entries so the values are
   preserved when the mode changes back. */
static void stick_keys_visibility(GtkComboBox *combo, App *app) {
    (void)combo;
    if (!app->left_keys_box || !app->right_keys_box) return;
    gtk_widget_set_visible(app->left_keys_box,
        gtk_combo_box_get_active(GTK_COMBO_BOX(app->left_mode)) == 2);
    gtk_widget_set_visible(app->right_keys_box,
        gtk_combo_box_get_active(GTK_COMBO_BOX(app->right_mode)) == 3);
}

static GtkWidget *stick_keys_row(App *app, int first, const char *label) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    GtkWidget *l = gtk_label_new(label); gtk_widget_set_halign(l, GTK_ALIGN_START);
    GtkWidget *keys = gtk_grid_new(); gtk_grid_set_column_spacing(GTK_GRID(keys), 3);
    static const char *dirs[] = {"L", "R", "U", "D"};
    for (int i = 0; i < 4; i++) {
        int n = first + i;
        app->stick_entries[n] = gtk_entry_new();
        gtk_entry_set_width_chars(GTK_ENTRY(app->stick_entries[n]), 7);
        gtk_entry_set_placeholder_text(GTK_ENTRY(app->stick_entries[n]), dirs[i]);
        g_signal_connect(app->stick_entries[n], "key-press-event",
            G_CALLBACK(capture_key), NULL);
        gtk_grid_attach(GTK_GRID(keys), app->stick_entries[n], i, 0, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), keys, FALSE, FALSE, 0);
    return box;
}

static GtkWidget *sticks_frame(App *app) {
    GtkWidget *frame = frame_new("input-gaming-symbolic", "Analog sticks");
    GtkWidget *grid = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(grid), 3); gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 4);
    static const char *left_modes[] = {"Disabled", "Mouse movement", "Keyboard directions"};
    static const char *right_modes[] = {"Disabled", "Mouse movement", "Scrolling", "Keyboard directions"};
    app->left_mode = combo_text(left_modes, 3); app->right_mode = combo_text(right_modes, 4);
    g_signal_connect(app->left_mode, "changed", G_CALLBACK(stick_keys_visibility), app);
    g_signal_connect(app->right_mode, "changed", G_CALLBACK(stick_keys_visibility), app);
    app->deadzone = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 500, 10);
    app->sensitivity = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 100, 1);
    gtk_scale_set_value_pos(GTK_SCALE(app->deadzone), GTK_POS_RIGHT);
    gtk_scale_set_value_pos(GTK_SCALE(app->sensitivity), GTK_POS_RIGHT);
    app->invert_y = gtk_check_button_new_with_label("Invert vertical axes");
    gtk_widget_set_hexpand(grid, TRUE);
    grid_row(GTK_GRID(grid), 0, "Left stick", app->left_mode);
    grid_row(GTK_GRID(grid), 1, "Right stick", app->right_mode);
    grid_row(GTK_GRID(grid), 2, "Deadzone", app->deadzone);
    grid_row(GTK_GRID(grid), 3, "Sensitivity", app->sensitivity);
    gtk_grid_attach(GTK_GRID(grid), app->invert_y, 1, 4, 1, 1);
    app->left_keys_box = stick_keys_row(app, 0, "Left stick keys:");
    app->right_keys_box = stick_keys_row(app, 4, "Right stick keys:");
    gtk_grid_attach(GTK_GRID(grid), app->left_keys_box, 0, 5, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), app->right_keys_box, 0, 6, 2, 1);
    gtk_widget_set_hexpand(frame, TRUE);
    gtk_container_add(GTK_CONTAINER(frame), grid);
    return frame;
}

/* The slider is discrete: 0 = leave the pad's level alone, 1..6 = the
   hardware backlight levels. Changes are debounced and sent live through
   the control socket as set_pad_brightness (persisted by the daemon). */
static gboolean brightness_flush(gpointer data) {
    App *app = data; app->brightness_flush = 0;
    int value = (int)gtk_range_get_value(GTK_RANGE(app->brightness));
    int level = value == 0 ? -1 : value;
    if (level == app->pad_brightness) return G_SOURCE_REMOVE;
    char request[96];
    snprintf(request, sizeof request,
        "{\"op\":\"set_pad_brightness\",\"level\":%d}", level);
    GError *error = NULL;
    char *reply = ipc_request(request, &error);
    if (!reply) {
        set_status(app, error->message, TRUE); g_clear_error(&error);
        gtk_range_set_value(GTK_RANGE(app->brightness),
            app->pad_brightness < 0 ? 0 : app->pad_brightness);
        return G_SOURCE_REMOVE;
    }
    JsonParser *parser; JsonObject *root;
    if (!response_root(reply, &parser, &root, &error)) {
        set_status(app, error->message, TRUE); g_clear_error(&error);
        g_object_unref(parser); g_free(reply); return G_SOURCE_REMOVE;
    }
    app->pad_brightness = level;
    set_status(app, "Pad brightness updated", FALSE);
    g_object_unref(parser); g_free(reply);
    return G_SOURCE_REMOVE;
}

static void brightness_changed(GtkRange *range, gpointer data) {
    App *app = data;
    if (app->loading) return;
    int value = (int)gtk_range_get_value(range);
    char text[48];
    if (value == 0) g_strlcpy(text, "Auto (pad default)", sizeof text);
    else snprintf(text, sizeof text, "Level %d of 6", value);
    gtk_label_set_text(GTK_LABEL(app->brightness_value), text);
    if (app->brightness_flush) g_source_remove(app->brightness_flush);
    app->brightness_flush = g_timeout_add(400, brightness_flush, app);
}

static GtkWidget *display_frame(App *app) {
    GtkWidget *frame = frame_new("video-display-symbolic", "Display & audio");
    GtkWidget *grid = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(grid), 3); gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 4);
    app->brightness = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 6, 1);
    gtk_scale_set_draw_value(GTK_SCALE(app->brightness), FALSE);
    gtk_scale_add_mark(GTK_SCALE(app->brightness), 0, GTK_POS_BOTTOM, "Auto");
    for (int i = 1; i <= 6; i++)
        gtk_scale_add_mark(GTK_SCALE(app->brightness), i, GTK_POS_BOTTOM, NULL);
    app->brightness_value = gtk_label_new("Auto (pad default)");
    gtk_widget_set_halign(app->brightness_value, GTK_ALIGN_START);
    app->microphone = check_wrapped("Forward GamePad microphone");
    app->volume = check_wrapped("Physical volume slider controls session audio");
    gtk_widget_set_hexpand(grid, TRUE);
    g_signal_connect(app->brightness, "value-changed", G_CALLBACK(brightness_changed), app);
    grid_row(GTK_GRID(grid), 0, "Screen brightness", app->brightness);
    gtk_grid_attach(GTK_GRID(grid), app->brightness_value, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->microphone, 0, 2, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), app->volume, 0, 3, 2, 1);
    gtk_container_add(GTK_CONTAINER(frame), grid);
    return frame;
}

static GtkWidget *touch_frame(App *app) {
    GtkWidget *frame = frame_new("input-touchpad-symbolic", "Touch & motion");
    GtkWidget *grid = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(grid), 3); gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 4);
    app->touch_mouse = check_wrapped("Touchscreen controls the pointer");
    static const char *gyro_modes[] = {"Disabled", "Mouse movement", "Virtual stick"};
    app->gyro_mode = combo_text(gyro_modes, 3);
    app->gyro_sens = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 400, 5);
    gtk_scale_set_value_pos(GTK_SCALE(app->gyro_sens), GTK_POS_RIGHT);
    gtk_grid_attach(GTK_GRID(grid), app->touch_mouse, 0, 0, 2, 1);
    grid_row(GTK_GRID(grid), 1, "Gyroscope", app->gyro_mode);
    grid_row(GTK_GRID(grid), 2, "Gyroscope sensitivity", app->gyro_sens);
    gtk_widget_set_hexpand(grid, TRUE);
    gtk_container_add(GTK_CONTAINER(frame), grid);
    return frame;
}

static GtkWidget *camera_section(App *app) {
    GtkWidget *frame = frame_new("camera-web-symbolic", "Camera");
    gtk_widget_set_size_request(frame, 270, -1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box), 4);
    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->camera_announce = check_wrapped("Announce");
    app->camera_preview = check_wrapped("Preview");
    gtk_widget_set_tooltip_text(app->camera_announce,
        "Publish the GamePad camera through the dedicated virtual webcam.");
    gtk_widget_set_tooltip_text(app->camera_preview,
        "Show the latest GamePad camera frame in this section.");
    gtk_box_pack_start(GTK_BOX(controls), app->camera_announce, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(controls), app->camera_preview, TRUE, TRUE, 0);
    app->camera_picture = gtk_drawing_area_new();
    gtk_widget_set_size_request(app->camera_picture, 240, 180);
    gtk_widget_set_hexpand(app->camera_picture, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(app->camera_picture),
                                "camera-preview");
    g_signal_connect(app->camera_picture, "draw", G_CALLBACK(camera_draw), app);
    app->camera_status = gtk_label_new("Camera disabled");
    gtk_label_set_xalign(GTK_LABEL(app->camera_status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(app->camera_status), PANGO_ELLIPSIZE_END);
    app->camera_device = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->camera_device), "/dev/video42");
    gtk_widget_set_tooltip_text(app->camera_device,
        "Dedicated v4l2loopback node named exactly Wii U GamePad Camera.");
    gtk_box_pack_start(GTK_BOX(box), controls, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), app->camera_picture, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), app->camera_status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), app->camera_device, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), box);
    return frame;
}

static GtkWidget *monitor_frame(App *app) {
    GtkWidget *frame = frame_new("utilities-system-monitor-symbolic", "Live input monitor");
    app->monitor = gtk_label_new("No controller input report available.");
    gtk_label_set_xalign(GTK_LABEL(app->monitor), 0);
    gtk_label_set_ellipsize(GTK_LABEL(app->monitor), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(app->monitor, TRUE);
    gtk_widget_set_margin_start(app->monitor, 5); gtk_widget_set_margin_end(app->monitor, 5);
    gtk_widget_set_margin_top(app->monitor, 3); gtk_widget_set_margin_bottom(app->monitor, 3);
    gtk_container_add(GTK_CONTAINER(frame), app->monitor);
    return frame;
}

/* Vertical dashboard rows: compact mode strip, full-width bindings,
   four secondary sections that wrap via GtkFlowBox, then the monitor.
   Each row keeps its natural height; the scroller is only a fallback. */
static GtkWidget *dashboard_page(App *app) {
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    GtkWidget *rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(rows), 6);
    gtk_box_pack_start(GTK_BOX(rows), mode_frame(app), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rows), bindings_frame(app), FALSE, FALSE, 0);
    GtkWidget *secondary = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(secondary), GTK_SELECTION_NONE);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(secondary), 6);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(secondary), 8);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(secondary), FALSE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(secondary), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(secondary), 4);
    gtk_widget_set_hexpand(secondary, TRUE);
    GtkWidget *sticks = sticks_frame(app);
    GtkWidget *display = display_frame(app);
    GtkWidget *touch = touch_frame(app);
    gtk_widget_set_size_request(sticks, 270, -1);
    gtk_widget_set_size_request(display, 270, -1);
    gtk_widget_set_size_request(touch, 270, -1);
    gtk_flow_box_insert(GTK_FLOW_BOX(secondary), sticks, -1);
    gtk_flow_box_insert(GTK_FLOW_BOX(secondary), display, -1);
    gtk_flow_box_insert(GTK_FLOW_BOX(secondary), touch, -1);
    gtk_flow_box_insert(GTK_FLOW_BOX(secondary), camera_section(app), -1);
    gtk_box_pack_start(GTK_BOX(rows), secondary, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rows), monitor_frame(app), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(scroll), rows);
    return scroll;
}

static gboolean title_press(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    (void)widget; GtkWindow *window = data;
    if (event->button != 1) return FALSE;
    if (event->type == GDK_2BUTTON_PRESS) {
        if (gtk_window_is_maximized(window)) gtk_window_unmaximize(window);
        else gtk_window_maximize(window);
    } else gtk_window_begin_move_drag(window, event->button, event->x_root, event->y_root, event->time);
    return TRUE;
}

static void maximize_clicked(GtkButton *button, gpointer data) {
    (void)button; GtkWindow *window = data;
    if (gtk_window_is_maximized(window)) gtk_window_unmaximize(window); else gtk_window_maximize(window);
}

static gboolean resize_press(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    App *app = g_object_get_data(G_OBJECT(widget), "app");
    if (event->button == 1 && !gtk_window_is_maximized(GTK_WINDOW(app->window)))
        gtk_window_begin_resize_drag(GTK_WINDOW(app->window), GPOINTER_TO_INT(data),
            event->button, event->x_root, event->y_root, event->time);
    return TRUE;
}

static void add_resize_zones(GtkOverlay *overlay, App *app) {
    static const GdkWindowEdge edges[] = {GDK_WINDOW_EDGE_NORTH_WEST, GDK_WINDOW_EDGE_NORTH,
        GDK_WINDOW_EDGE_NORTH_EAST, GDK_WINDOW_EDGE_WEST, GDK_WINDOW_EDGE_EAST,
        GDK_WINDOW_EDGE_SOUTH_WEST, GDK_WINDOW_EDGE_SOUTH, GDK_WINDOW_EDGE_SOUTH_EAST};
    for (int i = 0; i < 8; i++) {
        GtkWidget *zone = gtk_event_box_new(); gtk_event_box_set_visible_window(GTK_EVENT_BOX(zone), FALSE);
        gboolean left = i == 0 || i == 3 || i == 5, right = i == 2 || i == 4 || i == 7;
        gboolean top = i < 3, bottom = i > 4, corner = i == 0 || i == 2 || i == 5 || i == 7;
        gtk_widget_set_halign(zone, left ? GTK_ALIGN_START : right ? GTK_ALIGN_END : GTK_ALIGN_FILL);
        gtk_widget_set_valign(zone, top ? GTK_ALIGN_START : bottom ? GTK_ALIGN_END : GTK_ALIGN_FILL);
        gtk_widget_set_size_request(zone, corner ? 10 : (left || right ? 4 : -1),
            corner ? 10 : (top || bottom ? 4 : -1));
        g_object_set_data(G_OBJECT(zone), "app", app);
        g_signal_connect(zone, "button-press-event", G_CALLBACK(resize_press), GINT_TO_POINTER(edges[i]));
        gtk_overlay_add_overlay(overlay, zone);
    }
}

static void apply_css(void) {
    const char *css =
        ".wup-settings, .wup-settings box, .wup-settings grid,"
        ".wup-settings scrolledwindow, .wup-settings viewport {"
        "background:#111923;color:#dce7f1;font-size:13px;}"
        ".wup-settings .titlebar {background:#192532;border-bottom:1px solid #35485b;}"
        ".wup-settings .brand {font-size:14px;font-weight:bold;color:#96cdf9;}"
        ".wup-settings frame.settings-section>border {border:1px solid #35485b;border-radius:0;}"
        ".wup-settings .camera-preview {border:1px solid #35485b;background:#09111a;}"
        ".wup-settings .section-title {font-size:14px;font-weight:bold;color:#96cdf9;}"
        ".wup-settings button,.wup-settings entry,.wup-settings combobox button {min-height:22px;padding:0 6px;border-radius:0;}"
        ".wup-settings entry {background:#0f1b27;color:#dce7f1;border:1px solid #35485b;}"
        ".wup-settings button,.wup-settings combobox button {background:#253547;color:#dce7f1;border:1px solid #35485b;}"
        ".wup-settings button:hover {background:#334c66;} .wup-settings button:active {background:#285277;}"
        ".wup-settings scrollbar {background:#192532;}"
        ".wup-settings scale slider {min-width:14px;min-height:14px;border-radius:0;}"
        "tooltip,menu {border-radius:0;}";
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* Default to the window manager's real work area (_NET_WORKAREA), so the
   XFCE panel stays visible and no taskbar height is hardcoded. Falls back
   to the previous default when the property is unavailable. */
static void size_to_workarea(GtkWindow *window) {
    GdkScreen *screen = gtk_window_get_screen(window);
    if (!GDK_IS_X11_SCREEN(screen)) return;
    Display *dpy = gdk_x11_display_get_xdisplay(gdk_display_get_default());
    Window root = gdk_x11_window_get_xid(gdk_screen_get_root_window(screen));
    Atom atom = XInternAtom(dpy, "_NET_WORKAREA", False);
    Atom type = None; int format = 0;
    unsigned long nitems = 0, after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, root, atom, 0, 4, False, XA_CARDINAL,
            &type, &format, &nitems, &after, &data) != Success ||
        !data || nitems < 4 || format != 32) {
        if (data) XFree(data);
        return;
    }
    long *area = (long *)data;
    gtk_window_move(window, (int)area[0], (int)area[1]);
    gtk_window_set_default_size(window, (int)area[2], (int)area[3]);
    XFree(data);
}

static void app_activate(GtkApplication *application, gpointer data) {
    App *app = data; settings_t defaults; settings_defaults(&defaults); app->input = defaults.input;
    app->audio_pad_to_pc = defaults.audio_pad_to_pc; app->pad_volume_controls_pc = defaults.pad_volume_controls_pc;
    app->pad_brightness = -1;
    app->camera_announce_enabled = defaults.camera_announce;
    app->camera_preview_enabled = defaults.camera_preview;
    g_strlcpy(app->camera_device_path, defaults.camera_device,
              sizeof app->camera_device_path);
    app->camera_frame_fd = -1;
    if (!app->camera_pixels) app->camera_pixels = g_malloc(WUP_CAMERA_PIXELS);
    app->window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(app->window), "WUP ThinServer Settings");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 820, 450);
    gtk_widget_set_size_request(app->window, 620, 380);
    gtk_window_set_decorated(GTK_WINDOW(app->window), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(app->window), "wup-thinserver-settings");
    const char *icon_paths[] = {
        "data/wup-thinserver-settings.svg",
        "/usr/local/share/icons/hicolor/scalable/apps/wup-thinserver-settings.svg",
        "/usr/share/icons/hicolor/scalable/apps/wup-thinserver-settings.svg"
    };
    for (guint i = 0; i < G_N_ELEMENTS(icon_paths); i++) {
        if (g_file_test(icon_paths[i], G_FILE_TEST_IS_REGULAR)) {
            gtk_window_set_icon_from_file(GTK_WINDOW(app->window), icon_paths[i], NULL);
            break;
        }
    }
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_window_set_wmclass(GTK_WINDOW(app->window), "wup-thinserver-settings", "wup-thinserver-settings");
    G_GNUC_END_IGNORE_DEPRECATIONS
    gtk_style_context_add_class(gtk_widget_get_style_context(app->window), "wup-settings");
    GtkWidget *overlay = gtk_overlay_new(), *main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *title = gtk_event_box_new(), *title_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "titlebar");
    gtk_container_set_border_width(GTK_CONTAINER(title_box), 3);
    GtkWidget *icon = gtk_image_new_from_icon_name("wup-thinserver-settings", GTK_ICON_SIZE_MENU);
    GtkWidget *brand = gtk_label_new("WUP ThinServer Settings");
    gtk_style_context_add_class(gtk_widget_get_style_context(brand), "brand");
    app->connection = gtk_label_new("Connecting to daemon..."); gtk_label_set_ellipsize(GTK_LABEL(app->connection), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(title_box), icon, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(title_box), brand, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(title_box), app->connection, TRUE, TRUE, 8);
    GtkWidget *minimize = gtk_button_new_from_icon_name("window-minimize-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget *maximize = gtk_button_new_from_icon_name("window-maximize-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_tooltip_text(minimize, "Minimize"); gtk_widget_set_tooltip_text(maximize, "Maximize or restore");
    gtk_widget_set_tooltip_text(close, "Close");
    gtk_box_pack_start(GTK_BOX(title_box), minimize, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(title_box), maximize, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(title_box), close, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(title), title_box);
    g_signal_connect(title, "button-press-event", G_CALLBACK(title_press), app->window);
    g_signal_connect_swapped(minimize, "clicked", G_CALLBACK(gtk_window_iconify), app->window);
    g_signal_connect(maximize, "clicked", G_CALLBACK(maximize_clicked), app->window);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_close), app->window);
    GtkWidget *dashboard = dashboard_page(app);
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5); gtk_container_set_border_width(GTK_CONTAINER(footer), 3);
    app->status = gtk_label_new("Ready"); gtk_label_set_xalign(GTK_LABEL(app->status), 0);
    GtkWidget *defaults_button = gtk_button_new_with_label("Restore defaults");
    GtkWidget *reload_button = gtk_button_new_with_label("Reload");
    GtkWidget *apply_button = gtk_button_new_with_label("Apply and save");
    gtk_widget_set_tooltip_text(apply_button, "Validate, apply immediately, and persist atomically.");
    gtk_box_pack_start(GTK_BOX(footer), app->status, TRUE, TRUE, 2);
    gtk_box_pack_start(GTK_BOX(footer), defaults_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(footer), reload_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(footer), apply_button, FALSE, FALSE, 0);
    g_signal_connect(defaults_button, "clicked", G_CALLBACK(defaults_clicked), app);
    g_signal_connect(reload_button, "clicked", G_CALLBACK(reload_clicked), app);
    g_signal_connect(apply_button, "clicked", G_CALLBACK(apply_clicked), app);
    gtk_box_pack_start(GTK_BOX(main), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main), dashboard, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(main), footer, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(overlay), main); add_resize_zones(GTK_OVERLAY(overlay), app);
    gtk_container_add(GTK_CONTAINER(app->window), overlay);
    apply_css(); size_to_workarea(GTK_WINDOW(app->window));
    gtk_widget_show_all(app->window);
    load_current(app, FALSE);
    g_timeout_add(250, monitor_tick, app);
    g_timeout_add(67, preview_tick, app);
}

int main(int argc, char **argv) {
    App app = {.camera_frame_fd = -1};
    GtkApplication *application = gtk_application_new("com.wozt.WupThinServerSettings", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(app_activate), &app);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    camera_frame_close(&app);
    g_free(app.camera_pixels);
    g_object_unref(application); return status;
}
