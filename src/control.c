#define _GNU_SOURCE
#include "control.h"
#include "config.h"
#include "input.h"
#include <X11/Xlib.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define CONTROL_MAX 65536

struct control_t {
    int fd;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    settings_t *settings;
    server_t *server;
};

static void set_protocol_error(GError **error, const char *message) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
}

static gboolean bool_member(JsonObject *o, const char *name, int *out,
                            GError **error) {
    if (!json_object_has_member(o, name)) return TRUE;
    JsonNode *node = json_object_get_member(o, name);
    if (!JSON_NODE_HOLDS_VALUE(node)) {
        set_protocol_error(error, "boolean setting has the wrong type");
        return FALSE;
    }
    GType type = json_node_get_value_type(node);
    if (type == G_TYPE_BOOLEAN) *out = json_node_get_boolean(node);
    else if (type == G_TYPE_INT64) {
        gint64 value = json_node_get_int(node);
        if (value != 0 && value != 1) {
            set_protocol_error(error, "boolean setting must be 0 or 1");
            return FALSE;
        }
        *out = (int)value;
    } else {
        set_protocol_error(error, "boolean setting has the wrong type");
        return FALSE;
    }
    return TRUE;
}

static gboolean int_member(JsonObject *o, const char *name, int *out,
                           GError **error) {
    if (!json_object_has_member(o, name)) return TRUE;
    JsonNode *node = json_object_get_member(o, name);
    if (!JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_INT64) {
        set_protocol_error(error, "integer setting has the wrong type");
        return FALSE;
    }
    gint64 value = json_node_get_int(node);
    if (value < G_MININT || value > G_MAXINT) {
        set_protocol_error(error, "integer setting is out of range");
        return FALSE;
    }
    *out = (int)value;
    return TRUE;
}

static gboolean chord_valid(const char *value) {
    if (!value || !value[0]) return TRUE;
    gchar **parts = g_strsplit(value, "+", 0);
    int count = g_strv_length(parts);
    gboolean valid = count >= 1 && count <= 5;
    for (int i = 0; valid && i < count; i++) {
        char *part = g_strstrip(parts[i]);
        if (!part[0]) { valid = FALSE; break; }
        if (i + 1 < count) {
            valid = !g_ascii_strcasecmp(part, "ctrl") ||
                    !g_ascii_strcasecmp(part, "control") ||
                    !g_ascii_strcasecmp(part, "alt") ||
                    !g_ascii_strcasecmp(part, "shift") ||
                    !g_ascii_strcasecmp(part, "super") ||
                    !g_ascii_strcasecmp(part, "meta");
        } else {
            valid = XStringToKeysym(part) != NoSymbol;
        }
    }
    g_strfreev(parts);
    return valid;
}

static gboolean string_array(JsonObject *o, const char *name, char *dst,
                             int count, size_t stride, const input_cfg_t *old,
                             gboolean commands, GError **error) {
    if (!json_object_has_member(o, name)) return TRUE;
    JsonNode *node = json_object_get_member(o, name);
    if (!JSON_NODE_HOLDS_ARRAY(node)) {
        set_protocol_error(error, "binding list has the wrong type");
        return FALSE;
    }
    JsonArray *a = json_node_get_array(node);
    if ((int)json_array_get_length(a) != count) {
        set_protocol_error(error, "binding list has the wrong length");
        return FALSE;
    }
    for (int i = 0; i < count; i++) {
        JsonNode *entry = json_array_get_element(a, i);
        if (!JSON_NODE_HOLDS_VALUE(entry) ||
            json_node_get_value_type(entry) != G_TYPE_STRING) {
            set_protocol_error(error, "binding must be a string");
            return FALSE;
        }
        const char *value = json_node_get_string(entry);
        if (!value || strlen(value) >= stride) {
            set_protocol_error(error, "binding is too long");
            return FALSE;
        }
        if (commands) {
            const char *previous = old ? old->commands[i] : "";
            if (strcmp(value, "") && strcmp(value, "onboard-toggle") &&
                strcmp(value, previous)) {
                set_protocol_error(error, "only defined built-in commands may be changed over IPC");
                return FALSE;
            }
        } else if (!chord_valid(value)) {
            set_protocol_error(error, "unknown key or keyboard shortcut");
            return FALSE;
        }
        g_strlcpy(dst + (size_t)i * stride, value, stride);
    }
    return TRUE;
}

static gboolean mouse_array(JsonObject *o, input_cfg_t *input, GError **error) {
    if (!json_object_has_member(o, "mouse_buttons")) return TRUE;
    JsonNode *node = json_object_get_member(o, "mouse_buttons");
    if (!JSON_NODE_HOLDS_ARRAY(node)) {
        set_protocol_error(error, "mouse button list has the wrong type");
        return FALSE;
    }
    JsonArray *a = json_node_get_array(node);
    if (json_array_get_length(a) != WBTN_COUNT) {
        set_protocol_error(error, "mouse button list has the wrong length");
        return FALSE;
    }
    for (int i = 0; i < WBTN_COUNT; i++) {
        JsonNode *entry = json_array_get_element(a, i);
        if (!JSON_NODE_HOLDS_VALUE(entry) ||
            json_node_get_value_type(entry) != G_TYPE_INT64) {
            set_protocol_error(error, "mouse button binding must be an integer");
            return FALSE;
        }
        int value = (int)json_node_get_int(entry);
        if (value < 0 || value > 5) {
            set_protocol_error(error, "mouse button binding must be between 0 and 5");
            return FALSE;
        }
        input->mouse_buttons[i] = value;
    }
    return TRUE;
}

static gboolean parse_input(JsonObject *object, const input_cfg_t *old,
                            input_cfg_t *input, GError **error) {
    if (!int_member(object, "mode", &input->mode, error) ||
        !int_member(object, "deadzone", &input->deadzone, error) ||
        !int_member(object, "sensitivity", &input->sensitivity, error) ||
        !int_member(object, "gyro_mode", &input->gyro_mode, error) ||
        !int_member(object, "gyro_sens", &input->gyro_sens, error) ||
        !bool_member(object, "invert_y", &input->invert_y, error) ||
        !bool_member(object, "touch_as_mouse", &input->touch_as_mouse, error) ||
        !bool_member(object, "left_stick_mouse", &input->left_stick_mouse, error) ||
        !bool_member(object, "right_stick_mouse", &input->right_stick_mouse, error) ||
        !bool_member(object, "right_stick_scroll", &input->right_stick_scroll, error))
        return FALSE;
    if (!string_array(object, "keys", (char *)input->keys, WBTN_COUNT,
                      sizeof input->keys[0], old, FALSE, error) ||
        !string_array(object, "stick_keys", (char *)input->stick_keys, 8,
                      sizeof input->stick_keys[0], old, FALSE, error) ||
        !string_array(object, "commands", (char *)input->commands, WBTN_COUNT,
                      sizeof input->commands[0], old, TRUE, error) ||
        !mouse_array(object, input, error)) return FALSE;
    if (input->right_stick_mouse && input->right_stick_scroll) {
        set_protocol_error(error, "right stick mouse and scrolling modes are mutually exclusive");
        return FALSE;
    }
    for (int i = 0; i < 4; i++) {
        if (input->left_stick_mouse && input->stick_keys[i][0]) {
            set_protocol_error(error, "left stick mouse and keyboard modes are mutually exclusive");
            return FALSE;
        }
        if ((input->right_stick_mouse || input->right_stick_scroll) &&
            input->stick_keys[i + 4][0]) {
            set_protocol_error(error, "right stick pointer and keyboard modes are mutually exclusive");
            return FALSE;
        }
    }
    return TRUE;
}

static void add_strings(JsonBuilder *b, char *base, int count, size_t stride) {
    json_builder_begin_array(b);
    for (int i = 0; i < count; i++)
        json_builder_add_string_value(b, base + (size_t)i * stride);
    json_builder_end_array(b);
}

static void add_input(JsonBuilder *b, const input_cfg_t *in) {
#define ADD_INT(name) json_builder_set_member_name(b, #name); json_builder_add_int_value(b, in->name)
    json_builder_begin_object(b);
    ADD_INT(mode); ADD_INT(deadzone); ADD_INT(sensitivity); ADD_INT(invert_y);
    ADD_INT(gyro_mode); ADD_INT(gyro_sens); ADD_INT(touch_as_mouse);
    ADD_INT(left_stick_mouse); ADD_INT(right_stick_mouse);
    ADD_INT(right_stick_scroll);
    json_builder_set_member_name(b, "keys");
    add_strings(b, (char *)in->keys, WBTN_COUNT, sizeof in->keys[0]);
    json_builder_set_member_name(b, "stick_keys");
    add_strings(b, (char *)in->stick_keys, 8, sizeof in->stick_keys[0]);
    json_builder_set_member_name(b, "commands");
    add_strings(b, (char *)in->commands, WBTN_COUNT, sizeof in->commands[0]);
    json_builder_set_member_name(b, "mouse_buttons");
    json_builder_begin_array(b);
    for (int i = 0; i < WBTN_COUNT; i++)
        json_builder_add_int_value(b, in->mouse_buttons[i]);
    json_builder_end_array(b);
    json_builder_end_object(b);
#undef ADD_INT
}

static const char *state_name(int state) {
    static const char *names[] = {"idle", "waiting", "streaming", "stopped"};
    return state >= 0 && state < 4 ? names[state] : "unknown";
}

static char *make_response(control_t *control, gboolean ok, const char *message,
                           gboolean include_config) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "ok"); json_builder_add_boolean_value(b, ok);
    if (message) {
        json_builder_set_member_name(b, ok ? "message" : "error");
        json_builder_add_string_value(b, message);
    }
    if (include_config) {
        json_builder_set_member_name(b, "config"); json_builder_begin_object(b);
        json_builder_set_member_name(b, "input"); add_input(b, &control->settings->input);
        json_builder_set_member_name(b, "audio_pad_to_pc");
        json_builder_add_int_value(b, control->settings->audio_pad_to_pc);
        json_builder_set_member_name(b, "pad_volume_controls_pc");
        json_builder_add_int_value(b, control->settings->pad_volume_controls_pc);
        json_builder_end_object(b);
    }
    json_builder_set_member_name(b, "state"); json_builder_begin_object(b);
    int state = server_state(control->server);
    json_builder_set_member_name(b, "connection");
    json_builder_add_string_value(b, state_name(state));
    json_builder_set_member_name(b, "status");
    json_builder_add_string_value(b, server_status(control->server));
    wupstream_input_t input;
    gboolean connected = server_input_snapshot(control->server, &input);
    json_builder_set_member_name(b, "connected");
    json_builder_add_boolean_value(b, connected);
    int input_mode = INMODE_KBMOUSE, gamepad_available = 0;
    char input_status[160];
    server_input_mode_snapshot(control->server, &input_mode, &gamepad_available,
                               input_status, sizeof input_status);
    json_builder_set_member_name(b, "input_mode");
    json_builder_add_string_value(b, input_mode == INMODE_GAMEPAD ? "gamepad" : "desktop");
    json_builder_set_member_name(b, "gamepad_available");
    json_builder_add_boolean_value(b, gamepad_available);
    json_builder_set_member_name(b, "input_status");
    json_builder_add_string_value(b, input_status);
    if (connected) {
#define ADD_REPORT(name) json_builder_set_member_name(b, #name); json_builder_add_int_value(b, input.name)
        ADD_REPORT(buttons); ADD_REPORT(lx); ADD_REPORT(ly); ADD_REPORT(rx);
        ADD_REPORT(ry); ADD_REPORT(touched); ADD_REPORT(tx); ADD_REPORT(ty);
        ADD_REPORT(volume); ADD_REPORT(battery); ADD_REPORT(charging);
#undef ADD_REPORT
    }
    json_builder_end_object(b);
    json_builder_end_object(b);
    JsonGenerator *g = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(g, root);
    char *data = json_generator_to_data(g, NULL);
    json_node_free(root);
    g_object_unref(g);
    g_object_unref(b);
    return data;
}

static char *handle_request(control_t *control, const char *data, gssize length) {
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    if (!json_parser_load_from_data(parser, data, length, &error)) {
        char *reply = make_response(control, FALSE, error->message, FALSE);
        g_clear_error(&error); g_object_unref(parser); return reply;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return make_response(control, FALSE, "request must be a JSON object", FALSE);
    }
    JsonObject *request = json_node_get_object(root);
    const char *op = json_object_get_string_member_with_default(request, "op", "");
    if (!strcmp(op, "get") || !strcmp(op, "ping")) {
        char *reply = make_response(control, TRUE, NULL, !strcmp(op, "get"));
        g_object_unref(parser); return reply;
    }

    settings_t candidate = *control->settings;
    if (!strcmp(op, "restore_defaults")) {
        settings_t defaults;
        settings_defaults(&defaults);
        candidate.input = defaults.input;
        candidate.audio_pad_to_pc = defaults.audio_pad_to_pc;
        candidate.pad_volume_controls_pc = defaults.pad_volume_controls_pc;
    } else if (!strcmp(op, "validate_input") || !strcmp(op, "apply_input")) {
        JsonNode *input_node = json_object_get_member(request, "input");
        if (!input_node || !JSON_NODE_HOLDS_OBJECT(input_node) ||
            !parse_input(json_node_get_object(input_node), &control->settings->input,
                         &candidate.input, &error) ||
            !bool_member(request, "audio_pad_to_pc", &candidate.audio_pad_to_pc, &error) ||
            !bool_member(request, "pad_volume_controls_pc", &candidate.pad_volume_controls_pc, &error)) {
            char *reply = make_response(control, FALSE,
                error ? error->message : "input object is required", FALSE);
            g_clear_error(&error); g_object_unref(parser); return reply;
        }
    } else {
        g_object_unref(parser);
        return make_response(control, FALSE, "unknown operation", FALSE);
    }

    const char *validation = config_validate(&candidate);
    if (validation) {
        g_object_unref(parser);
        return make_response(control, FALSE, validation, FALSE);
    }
    if (!strcmp(op, "validate_input")) {
        g_object_unref(parser);
        return make_response(control, TRUE, "configuration is valid", FALSE);
    }
    if (candidate.input.mode == INMODE_GAMEPAD &&
        control->settings->input.mode != INMODE_GAMEPAD) {
        char probe_error[160];
        if (input_probe_gamepad(probe_error, sizeof probe_error) != 0) {
            char message[224];
            snprintf(message, sizeof message, "Gamepad mode unavailable: %s", probe_error);
            g_object_unref(parser);
            return make_response(control, FALSE, message, FALSE);
        }
    }
    if (!settings_save_checked(&candidate, &error)) {
        char *reply = make_response(control, FALSE, error->message, FALSE);
        g_clear_error(&error); g_object_unref(parser); return reply;
    }
    *control->settings = candidate;
    server_apply_input_keys(control->server);
    char *reply = make_response(control, TRUE,
        !strcmp(op, "restore_defaults") ? "defaults restored" : "input settings applied",
        TRUE);
    g_object_unref(parser);
    return reply;
}

static void send_reply(int fd, const char *reply) {
    size_t len = strlen(reply), off = 0;
    while (off < len) {
        ssize_t n = send(fd, reply + off, len - off, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        off += (size_t)n;
    }
    send(fd, "\n", 1, MSG_NOSIGNAL);
}

control_t *control_start(settings_t *settings, server_t *server, GError **error) {
    control_t *control = g_new0(control_t, 1);
    control->fd = -1;
    control->settings = settings;
    control->server = server;
    const char *runtime = g_getenv("XDG_RUNTIME_DIR");
    char *fallback = NULL;
    if (!runtime || !runtime[0]) {
        fallback = g_strdup_printf("/tmp/wup-thinserver-%u", (unsigned)getuid());
        runtime = fallback;
    }
    char *dir = g_build_filename(runtime, "wup-thinserver", NULL);
    if (g_mkdir_with_parents(dir, 0700) != 0 || chmod(dir, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "cannot secure runtime directory %s: %s", dir, g_strerror(errno));
        g_free(fallback); g_free(dir); g_free(control); return NULL;
    }
    char *path = g_build_filename(dir, "control.sock", NULL);
    if (strlen(path) >= sizeof control->path) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                            "control socket path is too long");
        g_free(fallback); g_free(dir); g_free(path); g_free(control); return NULL;
    }
    g_strlcpy(control->path, path, sizeof control->path);
    control->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (control->fd < 0) goto system_error;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    g_strlcpy(addr.sun_path, control->path, sizeof addr.sun_path);
    unlink(control->path);
    if (bind(control->fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        chmod(control->path, 0600) != 0 || listen(control->fd, 8) != 0)
        goto system_error;
    g_free(fallback); g_free(dir); g_free(path);
    return control;

system_error:
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "cannot create control socket %s: %s", control->path,
                g_strerror(errno));
    if (control->fd >= 0) close(control->fd);
    unlink(control->path);
    g_free(fallback); g_free(dir); g_free(path); g_free(control);
    return NULL;
}

void control_poll(control_t *control) {
    if (!control) return;
    for (;;) {
        int fd = accept4(control->fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            return;
        }
        struct ucred peer;
        socklen_t peer_len = sizeof peer;
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) != 0 ||
            peer.uid != getuid()) {
            send_reply(fd, "{\"ok\":false,\"error\":\"same-user access required\"}");
            close(fd); continue;
        }
        struct timeval timeout = { .tv_sec = 0, .tv_usec = 200000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        char *buffer = g_malloc(CONTROL_MAX + 1);
        size_t used = 0;
        while (used < CONTROL_MAX) {
            ssize_t n = recv(fd, buffer + used, CONTROL_MAX - used, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            used += (size_t)n;
            if (memchr(buffer, '\n', used)) break;
        }
        if (used == CONTROL_MAX && !memchr(buffer, '\n', used)) {
            send_reply(fd, "{\"ok\":false,\"error\":\"request exceeds 64 KiB\"}");
        } else {
            char *newline = memchr(buffer, '\n', used);
            if (newline) used = (size_t)(newline - buffer);
            buffer[used] = 0;
            char *reply = handle_request(control, buffer, (gssize)used);
            send_reply(fd, reply);
            g_free(reply);
        }
        g_free(buffer);
        close(fd);
    }
}

void control_stop(control_t *control) {
    if (!control) return;
    if (control->fd >= 0) close(control->fd);
    unlink(control->path);
    g_free(control);
}

const char *control_path(const control_t *control) {
    return control ? control->path : "";
}
