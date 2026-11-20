#pragma once

#include <stddef.h>
#include "esp_err.h"

// esp_console's command registry and REPL, reading stdin (only when it's a
// terminal: under the launcher there is no console).

typedef int (*esp_console_cmd_func_t)(int argc, char **argv);

typedef struct {
    const char *command;
    const char *help;
    const char *hint;
    esp_console_cmd_func_t func;
    void *argtable;
} esp_console_cmd_t;

typedef struct {
    size_t max_history_len;
    const char *history_save_path;
    size_t task_stack_size;
    unsigned task_priority;
    const char *prompt;
    size_t max_cmdline_length;
} esp_console_repl_config_t;

typedef struct esp_console_repl_s esp_console_repl_t;

#define ESP_CONSOLE_REPL_CONFIG_DEFAULT() \
    {.max_history_len = 32, .task_stack_size = 4096, .task_priority = 2, .prompt = NULL, .max_cmdline_length = 256}

#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_console_cmd_register(const esp_console_cmd_t *cmd);
esp_err_t esp_console_register_help_command(void);
// Linux only: a REPL on stdin/stdout.
esp_err_t esp_console_new_repl_stdio(const esp_console_repl_config_t *cfg, esp_console_repl_t **out);
esp_err_t esp_console_start_repl(esp_console_repl_t *repl);
#ifdef __cplusplus
}
#endif
