import unittest

from host_harness import COMMON, ROOT, defines, function, header, implementation, run_c, source


STORAGE = r"""
typedef struct { bool format_if_mount_failed; int max_files; int allocation_unit_size; } esp_vfs_fat_mount_config_t;
typedef int wl_handle_t;
#define CONFIG_WL_SECTOR_SIZE 512
static esp_err_t flash_mount_result = ESP_OK, sd_mount_result = ESP_OK;
static int esp_vfs_fat_spiflash_mount_rw_wl(const char *p, const char *l, const void *c, void *w) { return flash_mount_result; }
#define SDCARD_MOUNT "."
static esp_err_t sdcard_mount(void) { return sd_mount_result; }
#undef SERVER_LIST_PATH
#undef SERVER_LIST_DEFAULT_PATH
#define SERVER_LIST_PATH "servers.xml"
#define SERVER_LIST_DEFAULT_PATH "default.xml"
static bool fail_writes, fail_replacement, fail_reads, fail_backup_writes;
static bool short_reads;
static FILE *open_server_read;
static FILE *test_fopen(const char *p, const char *m) {
    if ((*m == 'w' && fail_writes) || (*m == 'r' && fail_reads && !strcmp(p, SERVER_LIST_PATH))) {
        errno = EIO;
        return NULL;
    }
    FILE *file = fopen(p, m);
    if (*m == 'r' && !strcmp(p, SERVER_LIST_PATH)) open_server_read = file;
    return file;
}
static int test_fclose(FILE *file) {
    if (file == open_server_read) open_server_read = NULL;
    return fclose(file);
}
static int test_rename(const char *from, const char *to) {
    if (open_server_read && !strcmp(from, SERVER_LIST_PATH)) {
        errno = EBUSY;
        return -1;
    }
    if (fail_replacement && strstr(from, ".tmp") && !strcmp(to, SERVER_LIST_PATH)) {
        errno = EIO;
        return -1;
    }
    /* FAT rename, unlike POSIX rename, cannot overwrite an existing file. */
    struct stat st;
    if (stat(to, &st) == 0) { errno = EEXIST; return -1; }
    return rename(from, to);
}
static size_t test_fwrite(const void *p, size_t size, size_t count, FILE *file) {
    if (fail_backup_writes) { errno = ENOSPC; return 0; }
    return fwrite(p, size, count, file);
}
static size_t test_fread(void *p, size_t size, size_t count, FILE *file) {
    return fread(p, size, short_reads && count ? count - 1 : count, file);
}
#define fopen test_fopen
#define fclose test_fclose
#define rename test_rename
#define fwrite test_fwrite
#define fread test_fread
"""

FIXTURES = r"""
static const char settings_xml[] =
    "<settings version=\"1\" callsign=\"N0TEST\" password=\"demo-password\" "
    "name=\"Operator\" location=\"Test\" dir_server=\"example.invalid\" "
    "wifi_ssid=\"network\" wifi_pass=\"demo-wifi-password\" "
    "accept_incoming=\"true\" volume=\"70\" mic_gain=\"30\"/>";
static void seed(void) {
    assert(server_list_init() == ESP_OK);
    s_list = calloc(1, sizeof(*s_list));
    assert(s_list);
    s_cap = s_count = 1;
    strcpy(s_list[0].call, "KEEP-R");
    s_list[0].node = 10;
    s_list[0].type = SERVER_REPEATER;
    assert(server_list_export_file(SERVER_LIST_PATH) == ESP_OK);
}
static void assert_original(void) {
    server_entry_t entry;
    assert(server_list_search("KEEP-R", &entry, 1) == 1 && entry.node == 10);
    assert(server_list_count() == 1);
    char xml[2048] = {0};
    FILE *file = fopen(SERVER_LIST_PATH, "r");
    assert(file);
    assert(fread(xml, 1, sizeof(xml) - 1, file) > 0);
    fclose(file);
    assert(strstr(xml, "KEEP-R") && !strstr(xml, "NEW-R"));
}
"""


def storage_program():
    code = COMMON + '#include <expat.h>\n'
    code += header("main/el_directory.h") + header("main/server_list.h")
    code += header("main/settings.h") + "\nsettings_t g_settings;\n" + STORAGE
    for module in ("xml", "file_store"):
        if (ROOT / f"main/{module}.c").exists():
            code += header(f"main/{module}.h") + implementation(f"main/{module}.c")
    code += implementation("main/server_list.c")
    web = source("main/web_config.c")
    start = web.index("typedef esp_err_t (*backup_write_cb_t)")
    end = web.index("static void restored_wifi_task")
    line = web.count("\n", 0, start) + 1
    code += f'\n#line {line} "{ROOT / "main/web_config.c"}"\n' + web[start:end]
    return code + FIXTURES


class StorageTests(unittest.TestCase):
    def run_storage(self, body):
        run_c(storage_program() + body, extra=["-lexpat"])

    def test_without_flash_storage_or_sd_list_is_kept_in_ram(self):
        self.run_storage(r"""
int main(void) {
    flash_mount_result = ESP_ERR_NOT_FOUND;
    sd_mount_result = ESP_FAIL;
    assert(server_list_init() == ESP_OK);
    el_station_t st = {0};
    strcpy(st.call, "KEEP-R");
    st.node = 10;
    server_list_on_directory(&st, 1);
    assert(server_list_count() == 1);
    struct stat sb;
    assert(stat(SERVER_LIST_PATH, &sb) != 0 && stat(SERVER_LIST_DEFAULT_PATH, &sb) != 0);
    server_list_on_directory(&st, 1);
    assert(server_list_count() == 1);
    free(s_list);
}
""")

    def test_headerless_legacy_xml_is_safe_and_preserves_attributes(self):
        self.run_storage(r"""
int main(void) {
    seed();
    char *xml = strdup("<server call=\"NEW-R\" node=\"42\" desc=\"A &amp; B\" favorite=\"true\" version=\"2\" extra=\"ignored\"/>");
    size_t count;
    assert(server_list_import_buffer(xml, &count) == ESP_OK && count == 1);
    server_entry_t entry;
    assert(server_list_search("NEW-R", &entry, 1) == 1);
    assert(entry.node == 42 && entry.favorite && !strcmp(entry.desc, "A & B"));
    free(xml);
    free(s_list);
}
""")

    def test_truncated_or_malformed_xml_does_not_replace_list(self):
        self.run_storage(r"""
int main(void) {
    seed();
    const char *bad[] = {
        "<?xml version=\"1.0\"?><echolink-servers><server call=\"NEW-R\" node=\"1\"/><server call=\"unfinished\"",
        "<echolink-servers><server call=\"NEW-R\" node=\"1\"/></wrong-root>",
        "<echolink-servers><server call=\"NEW-R\" node=\"1\"/><server node=\"2\"/></echolink-servers>",
        "<!DOCTYPE echolink-servers [<!ENTITY call \"NEW-R\">]><echolink-servers><server call=\"&call;\"/></echolink-servers>",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        assert(server_list_import_buffer(bad[i], NULL) != ESP_OK);
        assert_original();
    }
    char backup[2048];
    snprintf(backup, sizeof(backup), "<echolink-backup version=\"1\">%s<servers><server call=\"NEW-R\"", settings_xml);
    assert(server_list_import_buffer(backup, NULL) != ESP_OK);
    assert_original();
    settings_t restored;
    bool found;
    assert(backup_parse_settings(backup, &restored, &found) != ESP_OK);
    free(s_list);
}
""")

    def test_complete_backup_can_intentionally_restore_empty_list(self):
        self.run_storage(r"""
int main(void) {
    seed();
    char backup[2048];
    snprintf(backup, sizeof(backup),
             "<echolink-backup version=\"1\">%s<servers/></echolink-backup>", settings_xml);
    settings_t restored;
    bool found;
    assert(backup_parse_settings(backup, &restored, &found) == ESP_OK && found);
    assert(!strcmp(restored.callsign, "N0TEST") && restored.volume == 70);
    size_t count = 99;
    assert(server_list_import_buffer(backup, &count) == ESP_OK && count == 0);
    assert(server_list_count() == 0);
    assert(server_list_import_buffer("<echolink-servers/>", NULL) == ESP_ERR_NOT_FOUND);
    free(s_list);
}
""")

    def test_xml_attributes_allow_standard_whitespace_and_entities(self):
        self.run_storage(r"""
int main(void) {
    seed();
    const char *xml = "<echolink-servers><!-- <server call='IGNORE-R'/> -->"
        "<server call = 'NEW-R' node = '42' desc='A &#38; B' favorite='true'/>"
        "</echolink-servers>";
    size_t count;
    assert(server_list_import_buffer(xml, &count) == ESP_OK && count == 1);
    server_entry_t entry;
    assert(server_list_search("NEW-R", &entry, 1) == 1);
    assert(entry.node == 42 && entry.favorite && !strcmp(entry.desc, "A & B"));
    free(s_list);
}
""")

    def test_failed_import_preserves_active_and_persisted_list(self):
        for failure in ("fail_writes", "fail_replacement"):
            with self.subTest(failure=failure):
                self.run_storage(r"""
int main(void) {
    seed();
""" + f"{failure} = true;\n" + r"""
    int err = server_list_import_buffer("\n<server call=\"NEW-R\" node=\"42\"/>", NULL);
    assert(err != ESP_OK);
    assert_original();
    free(s_list);
}
""")

    def test_backup_cannot_silently_drop_unreadable_servers(self):
        self.run_storage(r"""
static char exported[8192];
static esp_err_t collect(void *ctx, const char *data, size_t n) {
    size_t used = strlen(exported);
    assert(used + n < sizeof(exported));
    memcpy(exported + used, data, n);
    exported[used + n] = 0;
    return ESP_OK;
}
int main(void) {
    seed();
    fail_reads = true;
    esp_err_t err = backup_write(collect, NULL);
    /* Either report the read failure, or export the intact in-memory snapshot. */
    assert(err != ESP_OK || strstr(exported, "KEEP-R"));
    free(s_list);
}
""")

    def test_interrupted_replacement_recovers_previous_list(self):
        self.run_storage(r"""
int main(void) {
    seed();
    assert(rename(SERVER_LIST_PATH, SERVER_LIST_PATH ".bak") == 0);
    FILE *tmp = fopen(SERVER_LIST_PATH ".tmp", "w");
    assert(tmp);
    fputs("<echolink-servers><server call=\"NEW-R\"", tmp);
    fclose(tmp);
    free(s_list);
    s_list = NULL;
    s_count = s_cap = 0;
    assert(server_list_init() == ESP_OK);
    assert_original();
    free(s_list);
}
""")

    def test_backup_snapshot_survives_list_changes_during_export(self):
        self.run_storage(r"""
static char exported[8192];
static bool changed;
static esp_err_t collect(void *ctx, const char *data, size_t n) {
    size_t used = strlen(exported);
    assert(used + n < sizeof(exported));
    memcpy(exported + used, data, n);
    exported[used + n] = 0;
    if (!changed && strstr(exported, "KEEP-R")) {
        changed = true;
        assert(server_list_import_buffer("<server call='NEW-R' node='42'/>", NULL) == ESP_OK);
    }
    return ESP_OK;
}
int main(void) {
    seed();
    assert(server_list_import_buffer(
        "<echolink-servers><server call='KEEP-R' node='10'/>"
        "<server call='OLD-L' node='11'/></echolink-servers>", NULL) == ESP_OK);
    assert(backup_write(collect, NULL) == ESP_OK);
    assert(strstr(exported, "KEEP-R") && strstr(exported, "OLD-L") && !strstr(exported, "NEW-R"));
    server_entry_t entry;
    assert(server_list_search("NEW-R", &entry, 1) == 1 && server_list_count() == 1);
    free(s_list);
}
""")

    def test_backup_round_trips_long_escaped_entries_and_settings(self):
        self.run_storage(r"""
static char exported[8192];
static esp_err_t collect(void *ctx, const char *data, size_t n) {
    size_t used = strlen(exported);
    assert(used + n < sizeof(exported));
    memcpy(exported + used, data, n);
    exported[used + n] = 0;
    return ESP_OK;
}
int main(void) {
    seed();
    memset(s_list[0].desc, '"', sizeof(s_list[0].desc) - 1);
    memset(s_list[0].host, '&', sizeof(s_list[0].host) - 1);
    s_list[0].manual = s_list[0].favorite = true;
    s_list[0].ip = inet_addr("10.20.30.40");
    server_entry_t original = s_list[0];
    assert(server_list_export_file(SERVER_LIST_PATH) == ESP_OK);
    strcpy(g_settings.wifi_ssid, "network & devices");
    strcpy(g_settings.wifi_pass, "line1\nline2\r\t\"<&");
    g_settings.volume = 70;
    g_settings.mic_gain = 30;
    assert(backup_write(collect, NULL) == ESP_OK);
    settings_t restored;
    bool found;
    assert(backup_parse_settings(exported, &restored, &found) == ESP_OK && found);
    assert(!strcmp(restored.wifi_ssid, g_settings.wifi_ssid));
    assert(!strcmp(restored.wifi_pass, g_settings.wifi_pass));
    assert(server_list_import_buffer(exported, NULL) == ESP_OK);
    server_entry_t entry;
    assert(server_list_search("KEEP-R", &entry, 1) == 1);
    assert(!strcmp(entry.desc, original.desc) && !strcmp(entry.host, original.host));
    assert(entry.ip == original.ip && entry.manual && entry.favorite);
    free(s_list);
}
""")

    def test_failed_sd_export_preserves_last_good_backup(self):
        code = storage_program() + r"""
typedef void httpd_req_t;
typedef void cJSON;
#define SD_BACKUP_PATH "backup.xml"
#define BACKUP_FILE_NAME "backup.xml"
static esp_err_t sd_error(httpd_req_t *r) { return ESP_FAIL; }
static esp_err_t send_error(httpd_req_t *r, const char *status, const char *msg) { return ESP_FAIL; }
static esp_err_t send_json(httpd_req_t *r, cJSON *j) { return ESP_OK; }
static cJSON *cJSON_CreateObject(void) { return (void *)1; }
static void cJSON_AddStringToObject(cJSON *j, const char *k, const char *v) { }
static void cJSON_AddNumberToObject(cJSON *j, const char *k, size_t v) { }
static void cJSON_AddBoolToObject(cJSON *j, const char *k, bool v) { }
"""
        web = source("main/web_config.c")
        # Keep the actual file-writer callback and any complete-backup wrapper together.
        start = web.index("static esp_err_t backup_file_write(")
        end = web.index("// GET /api/servers/export", start)
        code += web[start:end]
        code += function("main/web_config.c", "static esp_err_t h_sd_export(")
        code += r"""
int main(void) {
    seed();
    FILE *previous = fopen(SD_BACKUP_PATH, "w");
    assert(previous);
    fputs("last known good backup", previous);
    fclose(previous);
    fail_backup_writes = true;
    assert(h_sd_export(NULL) != ESP_OK);
    FILE *preserved = fopen(SD_BACKUP_PATH, "r");
    assert(preserved);
    char text[64] = {0};
    fread(text, 1, sizeof(text) - 1, preserved);
    fclose(preserved);
    assert(!strcmp(text, "last known good backup"));
    free(s_list);
}
"""
        run_c(code, extra=["-lexpat"])

    def test_file_import_rejects_nul_and_incomplete_reads(self):
        code = storage_program() + defines("main/web_config.c", "IMPORT_MAX")
        code += function("main/web_config.c", "static esp_err_t backup_read_file(")
        code += r"""
int main(void) {
    seed();
    const char *name = "input.xml";
    const char nul_data[] = "<server call='NEW-R' node='42'/>\0ignored suffix";
    FILE *file = fopen(name, "w");
    assert(file && fwrite(nul_data, 1, sizeof(nul_data) - 1, file) == sizeof(nul_data) - 1);
    fclose(file);
    char *data = NULL;
    assert(server_list_import_file(name, NULL) == ESP_ERR_INVALID_ARG);
    assert(backup_read_file(name, &data) == ESP_ERR_INVALID_ARG);
    assert_original();
    file = fopen(name, "w");
    assert(file);
    fputs("<server call='NEW-R' node='42'/>\n", file);
    fclose(file);
    short_reads = true;
    assert(server_list_import_file(name, NULL) != ESP_OK);
    assert(backup_read_file(name, &data) != ESP_OK);
    short_reads = false;
    assert_original();
    free(s_list);
}
"""
        run_c(code, extra=["-lexpat"])


if __name__ == "__main__":
    unittest.main()
