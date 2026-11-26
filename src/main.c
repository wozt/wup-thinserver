/* wup-thinserver: headless WUP ThinServer daemon.
 *
 * Streams the dedicated Linux desktop session (its own X11 display, e.g.
 * Xvfb) to a WUP ThinClient on a Wii U GamePad, using the WUP Stream
 * protocol (libwupstream). No GTK UI, no physical monitor, no host desktop
 * access: the daemon only talks to the X display named by DISPLAY.
 *
 * Usage:
 *   wup-thinserver [--config PATH] [--check] [--version]
 *
 * The desktop session itself (Xvfb, XFCE, audio) is owned by the session
 * launcher (docker entrypoint or the native installer), not by this daemon.
 * When the daemon exits the desktop keeps running; when it restarts it
 * reconnects to the same session.
 */
#define _DEFAULT_SOURCE
#include "settings.h"
#include "server.h"
#include "config.h"
#include "input.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/file.h>
#include <syslog.h>

static const char *prog = "wup-thinserver";
static volatile sig_atomic_t got_signal = 0;

static void on_signal(int sig) {
    got_signal = sig;
}

static void usage(FILE *f) {
    fprintf(f,
        "WUP ThinServer " WUP_THIN_VERSION "\n"
        "Usage: %s [options]\n"
        "\n"
        "  --config PATH   JSON configuration file\n"
        "                  (default: $WUP_THIN_CONFIG or ~/.config/wup-thinserver/settings.json)\n"
        "  --check         validate the configuration and exit\n"
        "  --version       print the version and exit\n"
        "  --help          this help\n"
        "\n"
        "Environment:\n"
        "  WUP_THIN_CONFIG   configuration file path\n"
        "  DISPLAY           X11 display of the session to capture (required)\n"
        "  WUPSTREAM_CRF / WUPSTREAM_KBPS / WUPSTREAM_X264 / WUPSTREAM_DEBUG\n"
        "                    libwupstream encoder overrides (see docs/CONFIGURATION.md)\n",
        prog);
}

/* refuse to run twice: flock on a lock file next to the config */
static int acquire_instance_lock(const char *cfgpath) {
    char path[600];
    const char *slash = strrchr(cfgpath, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - cfgpath) + 1;
        if (dlen >= sizeof path) dlen = sizeof path - 1;
        memcpy(path, cfgpath, dlen);
        path[dlen] = 0;
        strncat(path, "wup-thinserver.lock", sizeof path - strlen(path) - 1);
    } else {
        snprintf(path, sizeof path, "%s.lock", cfgpath);
    }
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        fprintf(stderr, "%s: cannot open lock file %s: %s\n",
                prog, path, strerror(errno));
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "%s: another instance is already running\n", prog);
        close(fd);
        return -1;
    }
    return fd;
}

static int parse_args(int argc, char **argv, const char **cfg) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(stdout);
            exit(0);
        } else if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-V")) {
            printf("%s %s\n", prog, WUP_THIN_VERSION);
            exit(0);
        } else if (!strcmp(argv[i], "--check")) {
            *cfg = "__check__";
        } else if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            setenv("WUP_THIN_CONFIG", argv[++i], 1);
        } else {
            fprintf(stderr, "%s: unknown argument '%s'\n", prog, argv[i]);
            usage(stderr);
            return -1;
        }
    }
    (void)cfg;
    return 0;
}

int main(int argc, char **argv) {
    prog = argv[0] ? argv[0] : "wup-thinserver";
    const char *cfg = NULL;
    int check_only = 0;
    if (parse_args(argc, argv, &cfg) != 0) return 2;
    check_only = cfg && !strcmp(cfg, "__check__");

    openlog("wup-thinserver", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    if (!getenv("DISPLAY") || !getenv("DISPLAY")[0]) {
        fprintf(stderr, "%s: DISPLAY is not set; point it at the session's X11 display\n", prog);
        fprintf(stderr, "       (e.g. DISPLAY=:99 for the container's Xvfb)\n");
        return 2;
    }

    settings_t *s = settings_load();
    const char *err = config_validate(s);
    if (err) {
        fprintf(stderr, "%s: invalid configuration: %s\n", prog, err);
        fprintf(stderr, "       config: %s\n", settings_path());
        return 2;
    }
    if (check_only) {
        printf("configuration OK: %s\n", settings_path());
        printf("  source: type=%d output='%s' backend=%d aspect=%d\n",
               s->source_type, s->source.output, s->capture_backend, s->aspect);
        printf("  quality: crf=%d kbps=%d low_latency=%d\n",
               s->crf, s->kbps, s->low_latency);
        printf("  input: mode=%d deadzone=%d commands[X]=%s\n",
               s->input.mode, s->input.deadzone, s->input.commands[WBTN_X]);
        return 0;
    }

    int lockfd = acquire_instance_lock(settings_path());
    if (lockfd < 0) return 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    server_t *srv = server_new(s);
    if (s->autostart_stream)
        server_request_stream(srv);

    syslog(LOG_INFO, "started (config %s, DISPLAY %s)",
           settings_path(), getenv("DISPLAY"));
    fprintf(stderr, "%s: streaming daemon started; config %s; DISPLAY=%s\n",
            prog, settings_path(), getenv("DISPLAY"));

    /* Status heartbeat: one line per state change / every 30 s. */
    int last_state = -1;
    for (;;) {
        int st = server_state(srv);
        if (st != last_state) {
            fprintf(stderr, "%s: state %d - %s\n", prog, st, server_status(srv));
            syslog(LOG_INFO, "state %d: %s", st, server_status(srv));
            last_state = st;
        }
        if (got_signal) break;
        sleep(1);
    }

    int sig = got_signal;
    fprintf(stderr, "%s: signal %d, shutting down\n", prog, sig);
    server_request_stop(srv);
    server_destroy(srv);       /* joins the stream thread, releases inputs */
    settings_save(s);
    syslog(LOG_INFO, "stopped");
    closelog();
    (void)lockfd;              /* released on exit */
    return 0;
}
