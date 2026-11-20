// The CardputerZero entry point: makes ~/.local/share/scheff-echolink the
// working directory (main/'s SDCARD_MOUNT "."), runs main/'s app_main(), and
// waits for SIGINT/SIGTERM (the launcher's long Esc) or a long Esc of ours.

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

    printf("Scheff EchoLink %s for the CardputerZero, data in %s\n", EL_VERSION, dir);
    app_main();

    while (!s_quit) usleep(50 * 1000);

    // Hang up politely and leave the directory before going.
    el_qso_disconnect();
    if (el_dir_logged_in()) el_dir_login(EL_DIR_OFFLINE);
    return 0;
}
