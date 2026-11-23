// CardputerZero networking: NetworkManager owns Wi-Fi, so this reports its
// state and asks nmcli to scan and join. There is no setup hotspot; the web
// config is on the LAN address, port CONFIG_EL_WEB_PORT.

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "esp_log.h"
#include "net_wifi.h"

static const char *TAG = "wifi";

esp_err_t net_wifi_init(void)
{
    return ESP_OK;
}

esp_err_t net_wifi_connect(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (pass && strlen(pass) > 64)) {
        ESP_LOGE(TAG, "invalid Wi-Fi credentials");
        return ESP_ERR_INVALID_ARG;
    }
    return net_wifi_join(ssid, pass, 20000) == NET_JOIN_OK ? ESP_OK : ESP_FAIL;
}

// The first non-loopback IPv4 address, preferring wlan*.
static bool first_ipv4(char *buf, size_t size)
{
    struct ifaddrs *ifs;
    if (getifaddrs(&ifs) != 0) return false;
    bool found = false;
    for (int pass = 0; pass < 2 && !found; pass++) {
        for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if ((i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
            if (pass == 0 && strncmp(i->ifa_name, "wl", 2)) continue;
            if (buf) inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, buf, size);
            found = true;
            break;
        }
    }
    freeifaddrs(ifs);
    return found;
}

bool net_wifi_has_ip(void)
{
    return first_ipv4(NULL, 0);
}

bool net_wifi_sta_ip(char *buf, size_t size)
{
    return first_ipv4(buf, size);
}

bool net_wifi_wait_ip(uint32_t timeout_ms)
{
    for (uint32_t t = 0; t < timeout_ms; t += 200) {
        if (net_wifi_has_ip()) return true;
        usleep(200 * 1000);
    }
    return net_wifi_has_ip();
}

esp_err_t net_wifi_ap_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void net_wifi_ap_stop(void)
{
}

bool net_wifi_ap_active(void)
{
    return false;
}

const char *net_wifi_ap_ssid(void)
{
    return "";
}

// Runs argv (no shell), collecting stdout and stderr into out. Returns the
// exit status, -1 if it couldn't run, or -2 after timeout_ms.
static int run(char *const argv[], char *out, size_t size, uint32_t timeout_ms)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        setenv("LC_ALL", "C", 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    size_t len = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    bool timed_out = false;
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long spent = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (spent >= (long)timeout_ms) {
            timed_out = true;
            break;
        }
        struct pollfd p = {fds[0], POLLIN, 0};
        if (poll(&p, 1, (int)(timeout_ms - spent)) <= 0) continue;
        char tmp[512];
        ssize_t n = read(fds[0], tmp, sizeof(tmp));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n && len + 1 < size; i++) out[len++] = tmp[i];
    }
    if (size) out[len] = 0;
    close(fds[0]);
    if (timed_out) kill(pid, SIGTERM);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (timed_out) return -2;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Splits nmcli's terse line at unescaped ':' and unescapes "\:" and "\\".
static int split_terse(char *line, char **fields, int max)
{
    int n = 0;
    char *w = line;
    fields[n++] = w;
    for (char *r = line; *r; r++) {
        if (*r == '\\' && r[1]) {
            *w++ = *++r;
        } else if (*r == ':' && n < max) {
            *w++ = 0;
            fields[n++] = w;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
    return n;
}

int net_wifi_scan(net_wifi_ap_t *out, int max)
{
    static char buf[16384];
    // A fresh scan needs NetworkManager's permission; listing the cached
    // results doesn't, so a refused rescan still returns what it knows.
    char *const scan[] = {"nmcli", "-t", "-f", "SSID,SIGNAL,SECURITY", "dev", "wifi", "list", "--rescan", "yes", NULL};
    char *const list[] = {"nmcli", "-t", "-f", "SSID,SIGNAL,SECURITY", "dev", "wifi", "list", "--rescan", "no", NULL};
    if (run(scan, buf, sizeof(buf), 20000) != 0 && run(list, buf, sizeof(buf), 10000) != 0) {
        ESP_LOGW(TAG, "nmcli scan failed: %s", buf);
        return -1;
    }
    int n = 0;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char *f[3];
        if (split_terse(line, f, 3) < 3 || !f[0][0]) continue;  // hidden
        int signal = atoi(f[1]);
        int8_t rssi = (int8_t)(signal / 2 - 100);
        bool secure = f[2][0] && strcmp(f[2], "--");
        int at = -1;
        for (int i = 0; i < n; i++) {
            if (!strcmp(out[i].ssid, f[0])) at = i;
        }
        if (at >= 0) {
            if (rssi > out[at].rssi) out[at].rssi = rssi;
            continue;
        }
        if (n >= max) continue;
        snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", f[0]);
        out[n].rssi = rssi;
        out[n].secure = secure;
        n++;
    }
    for (int i = 1; i < n; i++) {  // strongest first
        net_wifi_ap_t k = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].rssi < k.rssi) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = k;
    }
    return n;
}

net_join_result_t net_wifi_join(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    static char buf[2048];
    char wait[12];
    snprintf(wait, sizeof(wait), "%u", (unsigned)(timeout_ms / 1000 ? timeout_ms / 1000 : 1));
    char *argv[12];
    int a = 0;
    argv[a++] = "nmcli";
    argv[a++] = "--wait";
    argv[a++] = wait;
    argv[a++] = "dev";
    argv[a++] = "wifi";
    argv[a++] = "connect";
    argv[a++] = (char *)ssid;
    if (pass && pass[0]) {
        argv[a++] = "password";
        argv[a++] = (char *)pass;
    }
    argv[a] = NULL;
    int r = run(argv, buf, sizeof(buf), timeout_ms + 5000);
    if (r == 0) {
        net_wifi_wait_ip(5000);
        return NET_JOIN_OK;
    }
    ESP_LOGW(TAG, "nmcli connect \"%s\": %s", ssid, buf);
    if (r == -2 || strstr(buf, "Timeout") || strstr(buf, "timed out")) return NET_JOIN_TIMEOUT;
    if (strstr(buf, "No network with SSID")) return NET_JOIN_NOT_FOUND;
    if (strstr(buf, "Secrets were required") || strstr(buf, "psk") || strstr(buf, "password"))
        return NET_JOIN_BAD_PASSWORD;
    return NET_JOIN_FAILED;
}

void net_wifi_print_status(void)
{
    char ip[INET_ADDRSTRLEN];
    if (net_wifi_sta_ip(ip, sizeof(ip))) {
        printf("Network: IP %s (NetworkManager)\n", ip);
    } else {
        printf("Network: not connected\n");
    }
}
