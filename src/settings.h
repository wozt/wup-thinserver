#pragma once
#include <json-glib/json-glib.h>

/* Source kinds */
enum { SRC_SCREEN, SRC_WINDOW, SRC_REGION };

typedef struct {
    char output[64];       /* xrandr output name for SRC_SCREEN */
    char window_class[256];/* WM_CLASS for SRC_WINDOW (first of list match) */
    unsigned long window_id;/* X window id for SRC_WINDOW */
    int x, y, w, h;        /* geometry: region sel, or last known window geom */
} source_t;

typedef struct {
    char name[64];
    int crf;
    int kbps;              /* max bitrate, 0 = lib default */
    int gop;               /* frames between keyframes, 0 = auto */
} quality_preset_t;

enum { ASPECT_STRETCH, ASPECT_LETTERBOX, ASPECT_CROP };
enum { INMODE_GAMEPAD, INMODE_KBMOUSE };
enum { GYRO_OFF, GYRO_MOUSE, GYRO_STICK };

typedef struct {
    char name[64];
    char keys[18][32];
    char stick_keys[8][32];
} cmd_preset_t;

typedef struct {
    int mode;              /* INMODE_* */
    int deadzone;          /* 0..500 raw stick units (~+-1100 full) */
    int sensitivity;       /* percent, mouse/stick speed */
    int invert_y;          /* sticks / mouse Y */
    int gyro_mode;         /* GYRO_* */
    int gyro_sens;         /* percent */
    double gyro_bias[3];   /* raw gyro offset measured at rest (calibration) */
    /* kbmouse: per-button X11 keysym names; empty = unmapped.
       Index order: A B X Y DLEFT DRIGHT DUP DOWN ZL ZR L R PLUS MINUS HOME L3 R3 TV */
    char keys[18][32];
    /* stick direction -> key (kb/mouse): LX-,LX+,LY-,LY+, RX-,RX+,RY-,RY+ */
    char stick_keys[8][32];
    /* WUP_THIN_CMDS: optional shell command run on each button PRESS edge
       (kb/mouse mode). Empty = unmapped. Used e.g. to toggle Onboard. */
    char commands[18][160];
    /* WUP_THIN_CMDS: X11 mouse button (1=left,2=middle,3=right,4/5=wheel
       click) injected on press/release edges; 0 = unmapped. Takes
       precedence over keys[] like commands[]. */
    int mouse_buttons[18];
    /* named command presets (per-app keymaps), saved in settings.json */
    cmd_preset_t cmd_presets[8];
    int ncmd;
    char cmd_preset[64];      /* active preset name, "" = custom */
    int touch_as_mouse;
    int left_stick_mouse;    /* WUP_THIN_DESKTOP: left stick = relative pointer */
    int right_stick_mouse;   /* right stick = relative pointer (legacy) */
    int right_stick_scroll;  /* WUP_THIN_DESKTOP: right stick = wheel scroll */
} input_cfg_t;

typedef struct {
    char pad_ip[64];       /* empty = first pad found */
    int listen_port;       /* UDP port for 'B' discovery/direct beacons (1-65535) */
    char listen_address[64]; /* numeric IPv4 to bind; "" = all interfaces */
    source_t source;
    int source_type;
    int aspect;            /* ASPECT_* */
    char preset_name[64];
    int crf, kbps, gop;    /* resolved quality (preset or custom) */
    int capture_backend; /* 0 = MIT-SHM, 1 = XCB GetImage, 2 = KMS CPU, 3 = KMS EGL */
    int cursor_visible;  /* composite the X11 cursor into captured video */
    int custom_quality;    /* 1 = don't follow preset_name */
    int low_latency;       /* x264 zerolatency: no lookahead, no B-frames */
    quality_preset_t presets[8];
    int npresets;
    input_cfg_t input;
    int pad_brightness; /* -1: leave client setting alone; 1..6: last selected level */
    unsigned long pad_brightness_revision; /* runtime only: explicit GTK commands */
    int camera_announce; /* expose GamePad camera through a dedicated V4L2 output */
    int camera_preview;  /* publish latest frame for the protected Settings preview */
    char camera_device[128]; /* dedicated v4l2loopback node; exact card label is verified */
    int audio_pc_to_pad;
    int audio_pad_to_pc;
    int pad_volume_controls_pc; /* WUP_V96_PAD_VOLUME */
    char audio_monitor[128];
    int ui_theme; /* V4 numeric setting kept for migration */
    char theme_id[64]; /* WUP_V5_JSON_THEMES */
    char language_id[64]; /* WUP_V6_JSON_LANGUAGES: en is compiled in */
    int autostart_stream;  /* start streaming when pad connects */
    int auto_reconnect; /* retry after unexpected session loss */
    int gamepad_only;      /* stream a static test image instead of capture (calibration) */
    int gamepad_only_black;/* gamepad-only sends black frames instead of test image */
} settings_t;

settings_t *settings_load(void);
void settings_save(settings_t *s);
/* Atomic same-directory replacement. The old file remains intact on error. */
gboolean settings_save_checked(const settings_t *s, GError **error);
void settings_defaults(settings_t *s);
const char *settings_path(void);
