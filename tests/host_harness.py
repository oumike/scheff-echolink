"""Compile firmware logic on the host with hardware/RTOS boundaries replaced."""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def source(path):
    return (ROOT / path).read_text()


def header(path):
    return re.sub(r"(?m)^#(?:include|pragma)[^\n]*", "", source(path))


def implementation(path):
    text = re.sub(r"(?m)^#include[^\n]*", "", source(path))
    tag = Path(path).stem + "_tag"
    return f'\n#define TAG {tag}\n#line 1 "{ROOT / path}"\n{text}\n#undef TAG\n'


def function(path, signature):
    text = source(path)
    start = text.index(signature)
    brace = text.index("{", start)
    depth, quote, comment = 0, None, None
    i = brace
    while i < len(text):
        ch, pair = text[i], text[i : i + 2]
        if comment == "line":
            if ch == "\n":
                comment = None
        elif comment == "block":
            if pair == "*/":
                comment = None
                i += 1
        elif quote:
            if ch == "\\":
                i += 1
            elif ch == quote:
                quote = None
        elif pair in ("//", "/*"):
            comment = "line" if pair == "//" else "block"
            i += 1
        elif ch in ('"', "'"):
            quote = ch
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                line = text.count("\n", 0, start) + 1
                return f'\n#line {line} "{ROOT / path}"\n{text[start : i + 1]}\n'
        i += 1
    raise ValueError(f"Unclosed function: {signature}")


def defines(path, *prefixes):
    return "\n".join(
        line for line in source(path).splitlines()
        if any(line.startswith("#define " + prefix) for prefix in prefixes)
    ) + "\n"


COMMON = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/stat.h>
#include <arpa/inet.h>
typedef int esp_err_t;
enum { ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = 2,
       ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM, ESP_ERR_NOT_FOUND,
       ESP_ERR_INVALID_SIZE, ESP_ERR_INVALID_VERSION,
       ESP_ERR_TIMEOUT, ESP_ERR_INVALID_RESPONSE, ESP_ERR_NOT_SUPPORTED };
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_RETURN_ON_FALSE(test, err, ...) do { if (!(test)) return (err); } while (0)
#define ESP_RETURN_ON_ERROR(expr, ...) do { int e_ = (expr); if (e_) return e_; } while (0)
static const char *esp_err_to_name(int e) { return e ? "FAIL" : "OK"; }
#define MALLOC_CAP_SPIRAM 0
#define heap_caps_malloc(n, caps) malloc(n)
#define heap_caps_calloc(n, s, caps) calloc(n, s)
#define heap_caps_realloc(p, n, caps) realloc(p, n)
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY UINT32_MAX
static void *xSemaphoreCreateMutex(void) { return (void *)1; }
static int xSemaphoreTake(void *p, unsigned t) { return 1; }
static int xSemaphoreGive(void *p) { return 1; }
#ifndef __APPLE__
static size_t host_strlcpy(char *out, const char *in, size_t size) {
    size_t n = strlen(in);
    if (size) {
        size_t used = n < size - 1 ? n : size - 1;
        memcpy(out, in, used);
        out[used] = 0;
    }
    return n;
}
#define strlcpy host_strlcpy
#endif
"""


def run_c(text, *, args=(), extra=(), sanitize="address,undefined"):
    with tempfile.TemporaryDirectory(prefix="echolink-test-") as directory:
        directory = Path(directory)
        c_path, executable = directory / "case.c", directory / "case"
        c_path.write_text(text)
        command = [
            *shlex.split(os.environ.get("CC", "cc")),
            "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g",
            f"-fsanitize={sanitize}", "-fno-sanitize-recover=all",
            "-fno-omit-frame-pointer", "-Werror=implicit-function-declaration",
            "-Wno-unused-function", "-Wno-unused-variable",
            str(c_path), "-o", str(executable), *map(str, extra),
        ]
        built = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if built.returncode:
            raise AssertionError("Host compilation failed:\n" + built.stdout + built.stderr)
        result = subprocess.run(
            [str(executable), *map(str, args)], cwd=directory,
            env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"),
            capture_output=True, text=True, timeout=15,
        )
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result.stdout
