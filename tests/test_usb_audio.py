import unittest

from host_harness import COMMON, defines, function, run_c, source


def usb_program():
    text = source("main/usb_audio.c")
    units = text[text.index("typedef struct {\n    uint8_t id"):text.index("static void client_cb(")]
    code = COMMON + "#include <stdatomic.h>\n" + defines("main/usb_audio.c", "UAC_", "MAX_UNITS", "MAX_SRCS", "MAX_CH")
    code += r"""
typedef void *usb_device_handle_t;
typedef void *usb_host_client_handle_t;
typedef struct __attribute__((packed)) {
    uint8_t bmRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
} usb_setup_packet_t;
typedef struct usb_transfer {
    uint8_t *data_buffer;
    int num_bytes, status, timeout_ms;
    void *device_handle, *context;
    uint8_t bEndpointAddress;
    void (*callback)(struct usb_transfer *);
} usb_transfer_t;
typedef struct __attribute__((packed)) {
    uint8_t bLength, bDescriptorType;
    uint16_t wTotalLength;
} usb_config_desc_t;
#define USB_SETUP_PACKET_SIZE 8
#define USB_BM_REQUEST_TYPE_DIR_OUT 0
#define USB_BM_REQUEST_TYPE_TYPE_CLASS 0x20
#define USB_BM_REQUEST_TYPE_RECIP_INTERFACE 1
#define USB_B_DESCRIPTOR_TYPE_INTERFACE 4
#define USB_TRANSFER_STATUS_COMPLETED 0
#define pdMS_TO_TICKS(ms) (ms)
static bool fail_alloc, fail_transfer_alloc, fail_submit, immediate;
static int live_allocs, allocated, freed, closed;
static unsigned delay_calls;
static uint8_t config[1024];
static size_t config_len;
static usb_transfer_t *pending[16];
static unsigned pending_count;
static uint16_t written_units[32], written_values[32];
static unsigned writes;
static void (*delay_hook)(void);
static void *tracked_calloc(size_t n, size_t size) {
    if (fail_alloc) return NULL;
    void *p = calloc(n, size);
    assert(p); live_allocs++;
    return p;
}
static void tracked_free(void *p) { assert(p && live_allocs > 0); live_allocs--; free(p); }
static esp_err_t usb_host_device_open(void *client, uint8_t addr, void **out) {
    *out = (void *)1;
    return ESP_OK;
}
static esp_err_t usb_host_device_close(void *client, void *dev) {
    assert(dev == (void *)1);
    assert(allocated == freed);
    closed++;
    return ESP_OK;
}
static esp_err_t usb_host_get_active_config_descriptor(void *dev, const usb_config_desc_t **out) {
    *out = (const usb_config_desc_t *)config;
    return ESP_OK;
}
static esp_err_t usb_host_transfer_alloc(size_t n, int flags, usb_transfer_t **out) {
    if (fail_transfer_alloc) return ESP_ERR_NO_MEM;
    *out = calloc(1, sizeof(**out));
    assert(*out);
    (*out)->data_buffer = calloc(1, n);
    assert((*out)->data_buffer);
    allocated++;
    return ESP_OK;
}
static void usb_host_transfer_free(usb_transfer_t *xfer) {
    freed++;
    free(xfer->data_buffer);
    free(xfer);
}
static esp_err_t usb_host_transfer_submit_control(void *client, usb_transfer_t *xfer) {
    if (fail_submit) return ESP_FAIL;
    usb_setup_packet_t *setup = (usb_setup_packet_t *)xfer->data_buffer;
    assert(writes < 32);
    written_units[writes] = setup->wIndex >> 8;
    written_values[writes++] = setup->wValue;
    if (immediate) xfer->callback(xfer);
    else { assert(pending_count < 16); pending[pending_count++] = xfer; }
    return ESP_OK;
}
static void vTaskDelay(unsigned ticks) { delay_calls++; if (delay_hook) delay_hook(); }
static void complete(unsigned index, int status) {
    usb_transfer_t *xfer = pending[index];
    assert(xfer);
    pending[index] = NULL;
    xfer->status = status;
    xfer->callback(xfer);
}
#define calloc tracked_calloc
#define free tracked_free
"""
    code += units
    for signature in (
        "static void ctrl_device_release(", "static void ctrl_release(", "static void ctrl_done_cb(",
        "static const unit_t *find_unit(", "static bool from_physical_input(", "static bool reaches_output(",
        "static int channels_of(", "static esp_err_t set_cur(",
    ):
        code += function("main/usb_audio.c", signature)
    start = text.index("static const uint8_t k_mute_on")
    code += text[start:text.index("static bool has_rate(", start)]
    code += r"""
#undef calloc
#undef free
static ctrl_device_t *device(void) {
    ctrl_device_t *dev = tracked_calloc(1, sizeof(*dev));
    dev->handle = (void *)1;
    atomic_init(&dev->refs, 1);
    return dev;
}
static void descriptor(const uint8_t *data, size_t len) {
    assert(config_len + len <= sizeof(config));
    memcpy(config + config_len, data, len);
    config_len += len;
    ((usb_config_desc_t *)config)->wTotalLength = config_len;
}
#define DESC(...) do { const uint8_t bytes[] = {__VA_ARGS__}; descriptor(bytes, sizeof(bytes)); } while (0)
static void configuration(void) {
    s_client = (void *)1;
    immediate = true;
    config_len = sizeof(usb_config_desc_t);
    config[0] = sizeof(usb_config_desc_t);
    config[1] = 2;
    DESC(9, 4, 0, 0, 0, 1, 1, 0, 0);
    DESC(12, 0x24, 2, 1, 1, 2, 0, 1, 0, 0, 0, 0);  // physical microphone
    DESC(8, 0x24, 6, 2, 1, 1, 3, 0);                // shared mic gain/mute
}
"""
    return code


class USBAudioTests(unittest.TestCase):
    def test_shared_capture_feature_is_preserved_and_only_monitor_mixer_input_is_muted(self):
        run_c(usb_program() + r"""
int main(void) {
    configuration();
    DESC(9, 0x24, 3, 3, 1, 1, 0, 2, 0);              // USB capture from unit 2
    DESC(12, 0x24, 2, 5, 1, 1, 0, 2, 0, 0, 0, 0);   // USB playback
    DESC(12, 0x24, 4, 6, 2, 5, 2, 2, 0, 0, 3, 0);   // playback + mic mixer
    DESC(9, 0x24, 3, 4, 1, 3, 0, 6, 0);              // speaker
    mute_monitor(1);
    assert(writes == 2 && written_units[0] == 6 && written_units[1] == 6);
    assert(written_values[0] == 0x0301 && written_values[1] == 0x0302);
    assert(closed == 1 && !live_allocs && allocated == freed);
}
""")

    def test_monitor_only_feature_can_be_muted_without_touching_capture(self):
        run_c(usb_program() + r"""
int main(void) {
    configuration();
    DESC(9, 0x24, 3, 3, 1, 1, 0, 2, 0);
    DESC(8, 0x24, 6, 7, 2, 1, 1, 0);    // dedicated monitor branch
    DESC(9, 0x24, 3, 4, 1, 3, 0, 7, 0);
    mute_monitor(1);
    assert(writes == 1 && written_units[0] == 7 && written_values[0] == 0x0100);
    assert(closed == 1 && !live_allocs);
}
""")

    def test_shared_mixer_and_unknown_capture_paths_are_not_muted(self):
        for unknown in (False, True):
            with self.subTest(unknown=unknown):
                run_c(usb_program() + r"""
int main(void) {
    configuration();
    DESC(12, 0x24, 2, 5, 1, 1, 0, 2, 0, 0, 0, 0);
    DESC(12, 0x24, 4, 6, 2, 5, 2, 2, 0, 0, 3, 0);
    DESC(9, 0x24, 3, 4, 1, 3, 0, 6, 0);
""" + (
                    "DESC(6, 0x24, 7, 8, 1, 2);\nDESC(9, 0x24, 3, 3, 1, 1, 0, 8, 0);"
                    if unknown else "DESC(9, 0x24, 3, 3, 1, 1, 0, 6, 0);"
                ) + r"""
    mute_monitor(1);
    assert(!writes && closed == 1 && !live_allocs);
}
""")

    def test_failed_allocation_or_submission_releases_its_own_resources(self):
        run_c(usb_program() + r"""
int main(void) {
    ctrl_device_t *dev = device();
    fail_alloc = true;
    assert(set_cur(dev, 0, 2, 0x0100, &k_mute_on, 1) == ESP_ERR_NO_MEM);
    fail_alloc = false;
    fail_transfer_alloc = true;
    assert(set_cur(dev, 0, 2, 0x0100, &k_mute_on, 1) == ESP_ERR_NO_MEM);
    fail_transfer_alloc = false;
    fail_submit = true;
    assert(set_cur(dev, 0, 2, 0x0100, &k_mute_on, 1) == ESP_FAIL);
    assert(live_allocs == 1 && allocated == freed && !delay_calls && !closed);
    ctrl_device_release(dev);
    assert(!live_allocs && closed == 1);
}
""")

    def test_late_completion_cannot_complete_or_free_a_newer_request(self):
        run_c(usb_program() + r"""
static void old_completion(void) {
    if (pending[0]) complete(0, USB_TRANSFER_STATUS_COMPLETED);
}
static void new_completion(void) {
    complete(2, 1);
    delay_hook = NULL;
}
int main(void) {
    ctrl_device_t *dev = device();
    assert(set_cur(dev, 0, 2, 0x0100, &k_mute_on, 1) == ESP_ERR_TIMEOUT);
    assert(allocated == 1 && !freed);
    delay_hook = old_completion;
    assert(set_cur(dev, 0, 2, 0x0100, &k_mute_on, 1) == ESP_ERR_TIMEOUT);
    assert(allocated == 2 && freed == 1 && pending[1]);
    delay_hook = new_completion;
    assert(set_cur(dev, 0, 2, 0x0100, &k_mute_on, 1) == ESP_FAIL);
    assert(allocated == 3 && freed == 2);
    ctrl_device_release(dev);
    assert(!closed);  // request 1 still owns the open device
    complete(1, USB_TRANSFER_STATUS_COMPLETED);
    assert(allocated == freed && !live_allocs && closed == 1);
}
""")


if __name__ == "__main__":
    unittest.main()
