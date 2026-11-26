#define _GNU_SOURCE
#include "input.h"
#include "settings.h"
#include <SDL2/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int find_event(char *path, size_t path_size) {
    for (int attempt = 0; attempt < 100; attempt++) {
        glob_t matches;
        if (glob("/sys/class/input/event*/device/name", 0, NULL, &matches) == 0) {
            for (size_t i = 0; i < matches.gl_pathc; i++) {
                FILE *file = fopen(matches.gl_pathv[i], "r");
                char name[128] = "";
                if (file) { fgets(name, sizeof name, file); fclose(file); }
                name[strcspn(name, "\r\n")] = 0;
                if (strcmp(name, "WUP ThinServer Gamepad")) continue;
                const char *event = strstr(matches.gl_pathv[i], "/event");
                if (!event) continue;
                event++;
                const char *end = strchr(event, '/');
                snprintf(path, path_size, "/dev/input/%.*s",
                         end ? (int)(end - event) : (int)strlen(event), event);
                globfree(&matches);
                return 0;
            }
            globfree(&matches);
        }
        usleep(20000);
    }
    return -1;
}

int main(void) {
    static const uint32_t button_bits[] = {
        WUPSTREAM_A, WUPSTREAM_B, WUPSTREAM_X, WUPSTREAM_Y,
        WUPSTREAM_LEFT, WUPSTREAM_RIGHT, WUPSTREAM_UP, WUPSTREAM_DOWN,
        WUPSTREAM_ZL, WUPSTREAM_ZR, WUPSTREAM_L, WUPSTREAM_R,
        WUPSTREAM_PLUS, WUPSTREAM_MINUS, WUPSTREAM_HOME,
        WUPSTREAM_L3, WUPSTREAM_R3, WUPSTREAM_TV
    };
    static const uint16_t button_codes[] = {
        BTN_EAST, BTN_SOUTH, BTN_NORTH, BTN_WEST,
        BTN_DPAD_LEFT, BTN_DPAD_RIGHT, BTN_DPAD_UP, BTN_DPAD_DOWN,
        BTN_TL2, BTN_TR2, BTN_TL, BTN_TR,
        BTN_START, BTN_SELECT, BTN_MODE,
        BTN_THUMBL, BTN_THUMBR, BTN_TRIGGER_HAPPY1
    };
#define BUTTON_COUNT ((int)(sizeof button_bits / sizeof button_bits[0]))
    settings_t settings;
    settings_defaults(&settings);
    settings.input.mode = INMODE_GAMEPAD;
    settings.input.touch_as_mouse = 0;
    settings.pad_volume_controls_pc = 0;
    input_ctx_t *input = input_init(&settings);
    if (!input || !input_gamepad_ok(input)) {
        fprintf(stderr, "SKIP: %s\n", input_gamepad_error(input));
        input_free(input);
        return 77;
    }

    char event_path[128];
    if (find_event(event_path, sizeof event_path) != 0) {
        fprintf(stderr, "virtual gamepad event node was not created\n");
        input_free(input);
        return 2;
    }
    int event_fd = -1;
    for (int attempt = 0; attempt < 100 && event_fd < 0; attempt++) {
        event_fd = open(event_path, O_RDONLY | O_NONBLOCK);
        if (event_fd < 0) usleep(20000); /* wait for the host udev rule */
    }
    if (event_fd < 0) {
        fprintf(stderr, "cannot open %s: %s\n", event_path, strerror(errno));
        input_free(input);
        return 3;
    }

    if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        close(event_fd); input_free(input); return 4;
    }
    int found = -1;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        const char *name = SDL_JoystickNameForIndex(i);
        if (name && !strcmp(name, "WUP ThinServer Gamepad")) { found = i; break; }
    }
    if (found < 0) {
        fprintf(stderr, "SDL did not enumerate WUP ThinServer Gamepad\n");
        SDL_Quit(); close(event_fd); input_free(input); return 5;
    }
    SDL_Joystick *joystick = SDL_JoystickOpen(found);
    if (!joystick || SDL_JoystickNumAxes(joystick) < 4 ||
        SDL_JoystickNumButtons(joystick) < 17) {
        fprintf(stderr, "SDL mapping incomplete: axes=%d buttons=%d\n",
                joystick ? SDL_JoystickNumAxes(joystick) : -1,
                joystick ? SDL_JoystickNumButtons(joystick) : -1);
        if (joystick) SDL_JoystickClose(joystick);
        SDL_Quit(); close(event_fd); input_free(input); return 6;
    }

    struct input_event events[64];
    while (read(event_fd, events, sizeof events) > 0) {}

    wupstream_input_t report;
    memset(&report, 0, sizeof report);
    for (int i = 0; i < BUTTON_COUNT; i++) report.buttons |= button_bits[i];
    report.lx = 800;
    input_dispatch(input, &settings, &report);
    usleep(20000);

    int saw_press[BUTTON_COUNT];
    memset(saw_press, 0, sizeof saw_press);
    int saw_axis = 0, press_count = 0;
    for (int attempt = 0; attempt < 20 && (press_count < BUTTON_COUNT || !saw_axis); attempt++) {
        ssize_t bytes = read(event_fd, events, sizeof events);
        if (bytes > 0) {
            int count = (int)(bytes / sizeof events[0]);
            for (int i = 0; i < count; i++) {
                if (events[i].type == EV_ABS && events[i].code == ABS_X &&
                    events[i].value > 0) saw_axis = 1;
                for (int b = 0; b < BUTTON_COUNT; b++) {
                    if (events[i].type == EV_KEY &&
                        events[i].code == button_codes[b] &&
                        events[i].value == 1 && !saw_press[b]) {
                        saw_press[b] = 1;
                        press_count++;
                    }
                }
            }
        }
        usleep(10000);
    }

    char mode_error[160];
    /* Reapplying the active mode must first release every held input. Keep
     * the device alive here so the neutral report can be inspected. */
    if (input_set_mode(input, &settings, mode_error, sizeof mode_error) != 0 ||
        input_active_mode(input) != INMODE_GAMEPAD || !input_gamepad_ok(input)) {
        fprintf(stderr, "cannot reapply Gamepad mode: %s\n", mode_error);
        SDL_JoystickClose(joystick); SDL_Quit(); close(event_fd); input_free(input);
        return 7;
    }
    int saw_release[BUTTON_COUNT];
    memset(saw_release, 0, sizeof saw_release);
    int release_count = 0, saw_center = 0;
    for (int attempt = 0; attempt < 20 && (release_count < BUTTON_COUNT || !saw_center); attempt++) {
        ssize_t bytes = read(event_fd, events, sizeof events);
        if (bytes > 0) {
            int count = (int)(bytes / sizeof events[0]);
            for (int i = 0; i < count; i++) {
                if (events[i].type == EV_ABS && events[i].code == ABS_X &&
                    events[i].value == 0) saw_center = 1;
                for (int b = 0; b < BUTTON_COUNT; b++) {
                    if (events[i].type == EV_KEY &&
                        events[i].code == button_codes[b] &&
                        events[i].value == 0 && !saw_release[b]) {
                        saw_release[b] = 1;
                        release_count++;
                    }
                }
            }
        }
        usleep(10000);
    }

    settings.input.mode = INMODE_KBMOUSE;
    if (input_set_mode(input, &settings, mode_error, sizeof mode_error) != 0 ||
        input_active_mode(input) != INMODE_KBMOUSE || input_gamepad_ok(input)) {
        fprintf(stderr, "cannot switch to Desktop mode: %s\n", mode_error);
        SDL_JoystickClose(joystick); SDL_Quit(); close(event_fd); input_free(input);
        return 8;
    }
    settings.input.mode = INMODE_GAMEPAD;
    if (input_set_mode(input, &settings, mode_error, sizeof mode_error) != 0 ||
        input_active_mode(input) != INMODE_GAMEPAD || !input_gamepad_ok(input)) {
        fprintf(stderr, "cannot switch back to Gamepad mode: %s\n", mode_error);
        SDL_JoystickClose(joystick); SDL_Quit(); close(event_fd); input_free(input);
        return 9;
    }
    printf("SDL: %s, axes=%d, buttons=%d; evdev=%s\n",
           SDL_JoystickName(joystick), SDL_JoystickNumAxes(joystick),
           SDL_JoystickNumButtons(joystick), event_path);
    SDL_JoystickClose(joystick);
    SDL_Quit();
    close(event_fd);
    input_free(input);
    if (press_count != BUTTON_COUNT || !saw_axis) {
        fprintf(stderr, "incomplete gamepad report: buttons=%d/%d axis=%d\n",
                press_count, BUTTON_COUNT, saw_axis);
        return 10;
    }
    if (release_count != BUTTON_COUNT || !saw_center) {
        fprintf(stderr, "incomplete neutral report: buttons=%d/%d center=%d\n",
                release_count, BUTTON_COUNT, saw_center);
        return 11;
    }
    return 0;
}
