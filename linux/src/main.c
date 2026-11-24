// The CardputerZero entry point: makes ~/.local/share/scheff-echolink the
// working directory (main/'s SDCARD_MOUNT "."), runs main/'s app_main(), and
// waits for SIGINT/SIGTERM (the launcher's long Esc) or a long Esc of ours.
// Started from a desktop session it opens a window instead of drawing on the
// LCD (SCHEFF_DISPLAY=window|lcd overrides the choice), and runs the window
// here until it is closed.

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cp0.h"
#include "el_directory.h"
#include "el_qso.h"

#define DESKTOP_ENTRY_SRC "/usr/share/scheff-echolink/scheff-echolink.desktop"

void app_main(void);

static volatile sig_atomic_t s_quit;

void cp0_request_quit(void)
{
    s_quit = 1;
}

static void on_signal(int sig)
{
    s_quit = 1;
}

static bool should_quit(void)
{
    return s_quit;
}

static int mkdirs(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    return mkdir(tmp, 0700) != 0 && errno != EEXIST ? -1 : 0;
}

// The desktop session's menu entry, copied in at each start so the app can
// be started from the desktop too.
static void install_desktop_entry(void)
{
    const char *home = getenv("HOME");
    FILE *in = home ? fopen(DESKTOP_ENTRY_SRC, "r") : NULL;
    if (!in) return;
    char dir[512], path[600];
    snprintf(dir, sizeof(dir), "%s/.local/share/applications", home);
    snprintf(path, sizeof(path), "%s/scheff-echolink.desktop", dir);
    FILE *out = mkdirs(dir) == 0 ? fopen(path, "w") : NULL;
    if (out) {
        char buf[1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
        fclose(out);
    }
    fclose(in);
}

// A window when started from a desktop session, unless SCHEFF_DISPLAY says
// otherwise.
static bool want_window(void)
{
    const char *d = getenv("SCHEFF_DISPLAY");
    if (d && !strcmp(d, "window")) return true;
    if (d && !strcmp(d, "lcd")) return false;
    const char *w = getenv("WAYLAND_DISPLAY"), *x = getenv("DISPLAY");
    return (w && *w) || (x && *x);
}

int main(int argc, char **argv)
{
    char dir[512];
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) {
        snprintf(dir, sizeof(dir), "%s/scheff-echolink", xdg);
    } else {
        snprintf(dir, sizeof(dir), "%s/.local/share/scheff-echolink", home && *home ? home : "/tmp");
    }
    if (mkdirs(dir) != 0 || chdir(dir) != 0) {
        fprintf(stderr, "scheff-echolink: can't use %s: %s\n", dir, strerror(errno));
        return 1;
    }

    // One copy at a time: a second would fail to bind the EchoLink ports.
    int lock = open("instance.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "scheff-echolink: already running\n");
        return 1;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    install_desktop_entry();

    // SDL's video must stay on this thread.
    bool window = want_window();
    cp0_choose_window(window);
    if (window && !sdlwin_open("Scheff EchoLink")) return 1;

    printf("Scheff EchoLink %s for the CardputerZero, data in %s\n", EL_VERSION, dir);
    app_main();

    if (window) {
        sdlwin_run(should_quit);
        sdlwin_close(getenv("SCHEFF_SCREENSHOT"));
    } else {
        while (!s_quit) usleep(50 * 1000);
    }

    // Hang up politely and leave the directory before going.
    el_qso_disconnect();
    if (el_dir_logged_in()) el_dir_login(EL_DIR_OFFLINE);
    return 0;
}
