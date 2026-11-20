// esp_console over stdin: commands are split into words (double quotes group
// words) and dispatched by name.

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "esp_console.h"

#define MAX_CMDS 48
#define MAX_ARGS 16

struct esp_console_repl_s {
    char prompt[32];
    size_t max_line;
};

static esp_console_cmd_t s_cmds[MAX_CMDS];
static int s_count;

esp_err_t esp_console_cmd_register(const esp_console_cmd_t *cmd)
{
    if (!cmd || !cmd->command || !cmd->func) return ESP_ERR_INVALID_ARG;
    if (s_count >= MAX_CMDS) return ESP_ERR_NO_MEM;
    s_cmds[s_count++] = *cmd;
    return ESP_OK;
}

static int cmd_help(int argc, char **argv)
{
    for (int i = 0; i < s_count; i++) {
        printf("%s %s\n  %s\n\n", s_cmds[i].command, s_cmds[i].hint ? s_cmds[i].hint : "",
               s_cmds[i].help ? s_cmds[i].help : "");
    }
    return 0;
}

esp_err_t esp_console_register_help_command(void)
{
    const esp_console_cmd_t c = {.command = "help", .help = "Print the list of commands", .func = cmd_help};
    return esp_console_cmd_register(&c);
}

esp_err_t esp_console_new_repl_stdio(const esp_console_repl_config_t *cfg, esp_console_repl_t **out)
{
    struct esp_console_repl_s *r = calloc(1, sizeof(*r));
    if (!r) return ESP_ERR_NO_MEM;
    snprintf(r->prompt, sizeof(r->prompt), "%s", cfg && cfg->prompt ? cfg->prompt : ">");
    r->max_line = cfg && cfg->max_cmdline_length ? cfg->max_cmdline_length : 256;
    *out = r;
    return ESP_OK;
}

static int split(char *line, char **argv)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < MAX_ARGS) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *w = p, *o = p;
        bool quoted = false;
        for (; *p && (quoted || (*p != ' ' && *p != '\t')); p++) {
            if (*p == '"') quoted = !quoted;
            else *o++ = *p;
        }
        if (*p) p++;
        *o = 0;
        argv[argc++] = w;
    }
    return argc;
}

static void *repl_thread(void *arg)
{
    struct esp_console_repl_s *r = arg;
    char *line = malloc(r->max_line + 2);
    if (!line) return NULL;
    for (;;) {
        printf("%s ", r->prompt);
        fflush(stdout);
        if (!fgets(line, (int)r->max_line + 2, stdin)) break;
        line[strcspn(line, "\r\n")] = 0;
        char *argv[MAX_ARGS + 1] = {0};
        int argc = split(line, argv);
        if (!argc) continue;
        int i;
        for (i = 0; i < s_count; i++) {
            if (!strcmp(s_cmds[i].command, argv[0])) {
                s_cmds[i].func(argc, argv);
                break;
            }
        }
        if (i == s_count) printf("Unrecognized command\n");
    }
    free(line);
    return NULL;
}

esp_err_t esp_console_start_repl(esp_console_repl_t *repl)
{
    if (!isatty(STDIN_FILENO)) return ESP_OK;
    pthread_t th;
    if (pthread_create(&th, NULL, repl_thread, repl) != 0) return ESP_FAIL;
    pthread_detach(th);
    return ESP_OK;
}
