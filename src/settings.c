#define _GNU_SOURCE
#include "settings.h"
#include "input.h" /* WBTN_* indices for the default command map */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define CFG_DIR ".config/wup-thinserver"
#define CFG_FILE "settings.json"
/* WUP_V913_GOP_ZERO: legacy JSON GOP entries are ignored and removed
 * on save. Keep the struct field for ABI/source compatibility. */

const char *settings_path(void) {
    static char path[512];
    /* WUP_THIN_CONFIG: explicit config file path (container / native installs). */
    const char *env = getenv("WUP_THIN_CONFIG");
    if (env && env[0]) {
        snprintf(path, sizeof path, "%s", env);
        return path;
    }
    const char *home = g_get_home_dir();
    snprintf(path, sizeof path, "%s/" CFG_DIR "/" CFG_FILE, home);
    return path;
}

/* default kbmouse mapping: A B X Y -> z x c v, dpad -> arrows,
   L R -> shift ctrl, ZL ZR -> a s, +/- -> -, =, sticks->mouse, L3/R3 -> e q.
   HOME is bindable like any button (Super_L = the XFCE app menu); the
   ThinClient only reserves its local shortcuts (5 s hold, HOME+X/Up/Down/B). */
/* WUP_THIN_DESKTOP: GamePad -> XFCE desktop defaults (see
   config/settings.example.json and docs/CONFIGURATION.md). X is left
   unmapped here because input.commands[X] runs onboard-toggle instead;
   ZL/ZR/L3/R3 are mouse buttons via input.mouse_buttons. */
static const char *defkeys[18] = {
    "Return", "Escape", "", "Alt+Tab",       /* A B X Y */
    "Left", "Right", "Up", "Down",           /* dpad */
    "", "",                                   /* ZL ZR (mouse buttons) */
    "Ctrl+Page_Up", "Ctrl+Page_Down",         /* L R: browser tabs */
    "Ctrl+plus", "Ctrl+minus",                /* +/-: zoom */
    "Super_L", "", "",                        /* HOME L3 R3 */
    "F11",                                    /* TV: fullscreen */
};

void settings_defaults(settings_t *s) {
    memset(s, 0, sizeof *s);
    s->source_type = SRC_SCREEN;
    s->aspect = ASPECT_LETTERBOX;
    s->capture_backend = 0;
    s->cursor_visible = 1;
    s->crf = 18;
    s->kbps = 10000;
    s->gop = 0;
    s->pad_brightness = -1;
    s->camera_announce = 0;
    s->camera_preview = 0;
    snprintf(s->camera_device, sizeof s->camera_device, "/dev/video42");
    s->listen_port = 50002;   /* (WUP ThinClient beacons here; WUP Stream keeps 50001) */
    s->listen_address[0] = 0;
    s->audio_pc_to_pad = 1;
    s->audio_pad_to_pc = 0;
    s->pad_volume_controls_pc = 1; /* Preserve previous default behavior */
    s->low_latency = 1;
    snprintf(s->audio_monitor, sizeof s->audio_monitor, "default");
    s->ui_theme = 2; /* V4 default, for migration only */
    g_strlcpy(s->theme_id, "ocean", sizeof(s->theme_id)); /* WUP_V5_JSON_THEMES */
    g_strlcpy(s->language_id, "en", sizeof(s->language_id)); /* WUP_V6_JSON_LANGUAGES */
    /* WUP_THINSERVER: XTest kb/mouse is the default — it is what the remote
       desktop needs (Onboard typing, chords, mouse buttons). The uinput
       virtual gamepad remains available via input.mode = 0. */
    s->input.mode = INMODE_KBMOUSE;
    s->input.deadzone = 120;
    s->input.sensitivity = 100;
    s->input.gyro_mode = GYRO_OFF;
    s->input.gyro_sens = 100;
    s->input.touch_as_mouse = 1;
    /* WUP_THIN_DESKTOP: desktop-oriented sticks — left = pointer,
       right = scroll; no stick->key defaults (stick_keys stays
       configurable but empty by default). */
    s->input.left_stick_mouse = 1;
    s->input.right_stick_mouse = 0;
    s->input.right_stick_scroll = 1;
    /* WUP_THINSERVER: daemon defaults — stream on client connect, wait for
       reconnection after a drop instead of stopping the session. */
    s->autostart_stream = 1;
    s->auto_reconnect = 1;
    for (int i = 0; i < 18; i++)
        snprintf(s->input.keys[i], sizeof s->input.keys[i], "%s", defkeys[i]);
    /* WUP_THINSERVER: X button toggles the Onboard on-screen keyboard.
       The `onboard-toggle` helper ships in scripts/ and is installed on
       PATH by the container image and the native installer. */
    snprintf(s->input.commands[WBTN_X], sizeof s->input.commands[WBTN_X],
             "onboard-toggle");
    /* WUP_THIN_DESKTOP: ZL/L3 = left click, ZR/R3 = right click. */
    s->input.mouse_buttons[WBTN_ZL] = 1;
    s->input.mouse_buttons[WBTN_ZR] = 3;
    s->input.mouse_buttons[WBTN_L3] = 1;
    s->input.mouse_buttons[WBTN_R3] = 3;
    /* WUP_THIN_DESKTOP: sticks drive pointer/scroll, not keys — the
       upstream WASD/arrows defaults conflicted with mouse control. */
    s->npresets = 3;
    struct { const char *n; int crf, kbps, gop; } dp[] = {
        {"Economy", 23, 4000, 0},
        {"Balanced", 18, 10000, 0},
        {"Max", 14, 20000, 0},
    };
    for (int i = 0; i < 3; i++) {
        snprintf(s->presets[i].name, 64, "%s", dp[i].n);
        s->presets[i].crf = dp[i].crf;
        s->presets[i].kbps = dp[i].kbps;
        s->presets[i].gop = dp[i].gop;
    }
    snprintf(s->preset_name, sizeof s->preset_name, "Balanced");
}

static void json_str(JsonObject *o, const char *k, char *dst, size_t n) {
    const char *v = json_object_get_string_member_with_default(o, k, "");
    if (v) snprintf(dst, n, "%s", v);
}
static int json_int(JsonObject *o, const char *k, int d) {
    return json_object_has_member(o, k)
        ? (int)json_object_get_int_member(o, k) : d;
}
static JsonObject *json_obj(JsonObject *o, const char *k) {
    return json_object_has_member(o, k) ? json_object_get_object_member(o, k) : NULL;
}
static JsonArray *json_arr(JsonObject *o, const char *k) {
    return json_object_has_member(o, k) ? json_object_get_array_member(o, k) : NULL;
}

settings_t *settings_load(void) {
    settings_t *s = g_new0(settings_t, 1);
    settings_defaults(s);
    char *data = NULL;
    gsize len = 0;
    if (!g_file_get_contents(settings_path(), &data, &len, NULL))
        return s;
    JsonParser *p = json_parser_new();
    if (!json_parser_load_from_data(p, data, len, NULL)) {
        g_object_unref(p); g_free(data); return s;
    }
    JsonNode *root_node = json_parser_get_root(p);
    if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node)) { g_object_unref(p); g_free(data); return s; }
    JsonObject *root = json_node_get_object(root_node);

    json_str(root, "pad_ip", s->pad_ip, sizeof s->pad_ip);
    s->listen_port = json_int(root, "listen_port", s->listen_port);
    json_str(root, "listen_address", s->listen_address, sizeof s->listen_address);
    s->source_type = json_int(root, "source_type", s->source_type);
    s->aspect = json_int(root, "aspect", s->aspect);
    json_str(root, "preset_name", s->preset_name, sizeof s->preset_name);
    s->custom_quality = json_int(root, "custom_quality", 0);
    s->low_latency = json_int(root, "low_latency", 1);
    s->capture_backend = json_int(root, "capture_backend", 0);
    s->cursor_visible = json_int(root, "cursor_visible", s->cursor_visible);
    /* WUP_THINSERVER: out-of-range values are rejected by config_validate()
       (--check / daemon startup) instead of being silently clamped here. */
    s->crf = json_int(root, "crf", s->crf);
    s->kbps = json_int(root, "kbps", s->kbps);
    s->gop = 0; /* Ignore old saved values. */
    s->pad_brightness = json_int(root, "pad_brightness", -1);
    s->camera_announce = json_int(root, "camera_announce", 0);
    s->camera_preview = json_int(root, "camera_preview", 0);
    if (json_object_has_member(root, "camera_device"))
        json_str(root, "camera_device", s->camera_device, sizeof s->camera_device);
    s->audio_pc_to_pad = json_int(root, "audio_pc_to_pad", s->audio_pc_to_pad);
    s->audio_pad_to_pc = json_int(root, "audio_pad_to_pc", s->audio_pad_to_pc);
    s->pad_volume_controls_pc = json_int(root, "pad_volume_controls_pc", s->pad_volume_controls_pc);
    s->gamepad_only = json_int(root, "gamepad_only", 0);
    s->gamepad_only_black = json_int(root, "gamepad_only_black", 0);
    json_str(root, "audio_monitor", s->audio_monitor, sizeof s->audio_monitor);
    s->autostart_stream = json_int(root, "autostart_stream", 0);
    s->auto_reconnect = !!json_int(root, "auto_reconnect", 0);
    s->ui_theme = json_int(root, "ui_theme", s->ui_theme);
    if (s->ui_theme < 0 || s->ui_theme >= 5) s->ui_theme = 2;
    /* Existing users keep their selected V4 color on first V5 launch. */
    static const char *old_themes[] = {"graphite","light","ocean","emerald","rose"};
    g_strlcpy(s->theme_id,old_themes[s->ui_theme],sizeof(s->theme_id));
    if (json_object_has_member(root,"theme_id"))
        json_str(root,"theme_id",s->theme_id,sizeof(s->theme_id));
    if (json_object_has_member(root,"language_id"))
        json_str(root,"language_id",s->language_id,sizeof(s->language_id));

    JsonObject *src = json_obj(root, "source");
    if (src) {
        json_str(src, "output", s->source.output, sizeof s->source.output);
        json_str(src, "window_class", s->source.window_class, sizeof s->source.window_class);
        s->source.window_id = json_int(src, "window_id", 0);
        s->source.x = json_int(src, "x", 0);
        s->source.y = json_int(src, "y", 0);
        s->source.w = json_int(src, "w", 0);
        s->source.h = json_int(src, "h", 0);
    }
    JsonObject *in = json_obj(root, "input");
    if (in) {
        s->input.mode = json_int(in, "mode", s->input.mode);
        s->input.deadzone = json_int(in, "deadzone", s->input.deadzone);
        s->input.sensitivity = json_int(in, "sensitivity", s->input.sensitivity);
        s->input.invert_y = json_int(in, "invert_y", 0);
        s->input.gyro_mode = json_int(in, "gyro_mode", s->input.gyro_mode);
        s->input.gyro_sens = json_int(in, "gyro_sens", s->input.gyro_sens);
        if (json_object_has_member(in, "gyro_bias")) {
            JsonArray *gb = json_arr(in, "gyro_bias");
            if (gb)
                for (guint i = 0; i < 3 && i < json_array_get_length(gb); i++)
                    s->input.gyro_bias[i] = json_array_get_double_element(gb, i);
        }
        s->input.touch_as_mouse = json_int(in, "touch_as_mouse", s->input.touch_as_mouse);
        s->input.left_stick_mouse = json_int(in, "left_stick_mouse", s->input.left_stick_mouse);
        s->input.right_stick_mouse = json_int(in, "right_stick_mouse", s->input.right_stick_mouse);
        s->input.right_stick_scroll = json_int(in, "right_stick_scroll", s->input.right_stick_scroll);
        JsonArray *keys = json_arr(in, "keys");
        if (keys) {
            for (guint i = 0; i < 18 && i < json_array_get_length(keys); i++) {
                const char *v = json_array_get_string_element(keys, i);
                if (v) snprintf(s->input.keys[i], 32, "%s", v);
            }
        }
        JsonArray *skeys = json_arr(in, "stick_keys");
        if (skeys) {
            for (guint i = 0; i < 8 && i < json_array_get_length(skeys); i++) {
                const char *v = json_array_get_string_element(skeys, i);
                if (v) snprintf(s->input.stick_keys[i], 32, "%s", v);
            }
        }
        /* WUP_THIN_CMDS: per-button shell commands (press edge, kb/mouse mode) */
        JsonArray *cmds = json_arr(in, "commands");
        if (cmds) {
            for (guint i = 0; i < 18 && i < json_array_get_length(cmds); i++) {
                const char *v = json_array_get_string_element(cmds, i);
                if (v) snprintf(s->input.commands[i], sizeof s->input.commands[i], "%s", v);
            }
        }
        /* WUP_THIN_CMDS: per-button X11 mouse buttons (0 = unmapped) */
        JsonArray *mbs = json_arr(in, "mouse_buttons");
        if (mbs) {
            for (guint i = 0; i < 18 && i < json_array_get_length(mbs); i++) {
                int v = (int)json_array_get_int_element(mbs, i);
                s->input.mouse_buttons[i] = (v >= 0 && v <= 5) ? v : 0;
            }
        }
        json_str(in, "cmd_preset", s->input.cmd_preset, sizeof s->input.cmd_preset);
        JsonArray *cps = json_arr(in, "cmd_presets");
        if (cps) {
            int n = json_array_get_length(cps);
            if (n > 8) n = 8;
            for (int i = 0; i < n; i++) {
                JsonObject *po = json_array_get_object_element(cps, i);
                if (!po) continue;
                cmd_preset_t *cp = &s->input.cmd_presets[i];
                json_str(po, "name", cp->name, sizeof cp->name);
                JsonArray *pk = json_arr(po, "keys");
                if (pk)
                    for (guint j = 0; j < 18 && j < json_array_get_length(pk); j++) {
                        const char *v = json_array_get_string_element(pk, j);
                        if (v) snprintf(cp->keys[j], 32, "%s", v);
                    }
                JsonArray *ps = json_arr(po, "stick_keys");
                if (ps)
                    for (guint j = 0; j < 8 && j < json_array_get_length(ps); j++) {
                        const char *v = json_array_get_string_element(ps, j);
                        if (v) snprintf(cp->stick_keys[j], 32, "%s", v);
                    }
            }
            s->input.ncmd = n;
        }
    }
    JsonArray *pres = json_arr(root, "presets");
    if (pres) {
        int n = json_array_get_length(pres);
        if (n > 8) n = 8;
        for (int i = 0; i < n; i++) {
            JsonObject *po = json_array_get_object_element(pres, i);
            snprintf(s->presets[i].name, 64, "%s",
                     json_object_get_string_member_with_default(po, "name", ""));
            s->presets[i].crf = json_int(po, "crf", 18);
            s->presets[i].kbps = json_int(po, "kbps", 10000);
            s->presets[i].gop = 0; /* Ignore old preset values. */
        }
        s->npresets = n;
    }
    g_object_unref(p); g_free(data);
    return s;
}

static char *settings_json(const settings_t *s) {
    JsonObject *root = json_object_new();
    if (s->pad_ip[0]) json_object_set_string_member(root, "pad_ip", s->pad_ip);
    json_object_set_int_member(root, "listen_port", s->listen_port);
    if (s->listen_address[0])
        json_object_set_string_member(root, "listen_address", s->listen_address);
    json_object_set_int_member(root, "source_type", s->source_type);
    json_object_set_int_member(root, "aspect", s->aspect);
    json_object_set_string_member(root, "preset_name", s->preset_name);
    json_object_set_int_member(root, "custom_quality", s->custom_quality);
    json_object_set_int_member(root, "low_latency", s->low_latency);
    json_object_set_int_member(root, "capture_backend", s->capture_backend);
    json_object_set_int_member(root, "cursor_visible", s->cursor_visible);
    json_object_set_int_member(root, "crf", s->crf);
    json_object_set_int_member(root, "kbps", s->kbps);
    json_object_set_int_member(root, "pad_brightness", s->pad_brightness);
    json_object_set_int_member(root, "camera_announce", s->camera_announce);
    json_object_set_int_member(root, "camera_preview", s->camera_preview);
    json_object_set_string_member(root, "camera_device", s->camera_device);
    json_object_set_int_member(root, "audio_pc_to_pad", s->audio_pc_to_pad);
    json_object_set_int_member(root, "audio_pad_to_pc", s->audio_pad_to_pc);
    json_object_set_int_member(root, "pad_volume_controls_pc", s->pad_volume_controls_pc);
    json_object_set_int_member(root, "gamepad_only", s->gamepad_only);
    json_object_set_int_member(root, "gamepad_only_black", s->gamepad_only_black);
    json_object_set_string_member(root, "audio_monitor", s->audio_monitor);
    json_object_set_int_member(root, "autostart_stream", s->autostart_stream);
    json_object_set_int_member(root, "auto_reconnect", s->auto_reconnect);
    json_object_set_string_member(root,"theme_id",s->theme_id);
    json_object_set_string_member(root,"language_id",s->language_id);

    JsonObject *src = json_object_new();
    json_object_set_string_member(src, "output", s->source.output);
    json_object_set_string_member(src, "window_class", s->source.window_class);
    if (s->source.window_id) json_object_set_int_member(src, "window_id", s->source.window_id);
    json_object_set_int_member(src, "x", s->source.x);
    json_object_set_int_member(src, "y", s->source.y);
    json_object_set_int_member(src, "w", s->source.w);
    json_object_set_int_member(src, "h", s->source.h);
    json_object_set_object_member(root, "source", src);

    JsonObject *in = json_object_new();
    json_object_set_int_member(in, "mode", s->input.mode);
    json_object_set_int_member(in, "deadzone", s->input.deadzone);
    json_object_set_int_member(in, "sensitivity", s->input.sensitivity);
    json_object_set_int_member(in, "invert_y", s->input.invert_y);
    json_object_set_int_member(in, "gyro_mode", s->input.gyro_mode);
    json_object_set_int_member(in, "gyro_sens", s->input.gyro_sens);
    JsonArray *gb = json_array_new();
    for (int i = 0; i < 3; i++)
        json_array_add_double_element(gb, s->input.gyro_bias[i]);
    json_object_set_array_member(in, "gyro_bias", gb);
    json_object_set_int_member(in, "touch_as_mouse", s->input.touch_as_mouse);
    json_object_set_int_member(in, "left_stick_mouse", s->input.left_stick_mouse);
    json_object_set_int_member(in, "right_stick_mouse", s->input.right_stick_mouse);
    json_object_set_int_member(in, "right_stick_scroll", s->input.right_stick_scroll);
    JsonArray *keys = json_array_new();
    for (int i = 0; i < 18; i++)
        json_array_add_string_element(keys, s->input.keys[i]);
    json_object_set_array_member(in, "keys", keys);
    JsonArray *skeys = json_array_new();
    for (int i = 0; i < 8; i++)
        json_array_add_string_element(skeys, s->input.stick_keys[i]);
    json_object_set_array_member(in, "stick_keys", skeys);
    /* WUP_THIN_CMDS */
    JsonArray *cmds = json_array_new();
    for (int i = 0; i < 18; i++)
        json_array_add_string_element(cmds, s->input.commands[i]);
    json_object_set_array_member(in, "commands", cmds);
    JsonArray *mbs = json_array_new();
    for (int i = 0; i < 18; i++)
        json_array_add_int_element(mbs, s->input.mouse_buttons[i]);
    json_object_set_array_member(in, "mouse_buttons", mbs);
    json_object_set_string_member(in, "cmd_preset", s->input.cmd_preset);
    JsonArray *cps = json_array_new();
    for (int i = 0; i < s->input.ncmd; i++) {
        JsonObject *po = json_object_new();
        json_object_set_string_member(po, "name", s->input.cmd_presets[i].name);
        JsonArray *pk = json_array_new();
        for (int j = 0; j < 18; j++)
            json_array_add_string_element(pk, s->input.cmd_presets[i].keys[j]);
        json_object_set_array_member(po, "keys", pk);
        JsonArray *ps = json_array_new();
        for (int j = 0; j < 8; j++)
            json_array_add_string_element(ps, s->input.cmd_presets[i].stick_keys[j]);
        json_object_set_array_member(po, "stick_keys", ps);
        json_array_add_object_element(cps, po);
    }
    json_object_set_array_member(in, "cmd_presets", cps);
    json_object_set_object_member(root, "input", in);

    JsonArray *pres = json_array_new();
    for (int i = 0; i < s->npresets; i++) {
        JsonObject *po = json_object_new();
        json_object_set_string_member(po, "name", s->presets[i].name);
        json_object_set_int_member(po, "crf", s->presets[i].crf);
        json_object_set_int_member(po, "kbps", s->presets[i].kbps);
        json_array_add_object_element(pres, po);
    }
    json_object_set_array_member(root, "presets", pres);

    JsonGenerator *g = json_generator_new();
    JsonNode *root_node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root_node, root);
    /* JSON-GLib 1.6 (Ubuntu 22.04) does not have take_root().
     * set_root() copies the node; free the original afterward. */
    json_generator_set_root(g, root_node);
    json_node_free(root_node);
    char *out = json_generator_to_data(g, NULL);
    g_object_unref(g);
    return out;
}

gboolean settings_save_checked(const settings_t *s, GError **error) {
    const char *path = settings_path();
    char *dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "cannot create configuration directory %s: %s",
                    dir, g_strerror(errno));
        g_free(dir);
        return FALSE;
    }
    char *tmp = g_strdup_printf("%s/.settings.json.XXXXXX", dir);
    g_free(dir);
    int fd = g_mkstemp_full(tmp, O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "cannot create temporary configuration: %s", g_strerror(errno));
        g_free(tmp);
        return FALSE;
    }
    char *out = settings_json(s);
    gsize len = strlen(out), off = 0;
    gboolean ok = TRUE;
    while (off < len) {
        ssize_t n = write(fd, out + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = FALSE; break; }
        off += (gsize)n;
    }
    if (ok && fsync(fd) != 0) ok = FALSE;
    int saved_errno = errno;
    if (close(fd) != 0 && ok) { ok = FALSE; saved_errno = errno; }
    if (ok && rename(tmp, path) != 0) { ok = FALSE; saved_errno = errno; }
    if (!ok) {
        unlink(tmp);
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "cannot replace configuration %s: %s", path,
                    g_strerror(saved_errno));
    }
    g_free(out);
    g_free(tmp);
    return ok;
}

void settings_save(settings_t *s) {
    GError *error = NULL;
    if (!settings_save_checked(s, &error)) {
        g_warning("Cannot save settings: %s", error->message);
        g_clear_error(&error);
    }
}
