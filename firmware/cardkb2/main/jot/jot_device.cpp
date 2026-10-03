#include "jot_device.h"
#include "font8x8_basic.h"
#include "JotCore.h"
#include "JotConfig.h"
#include "driver/i2c_master.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {
uint32_t now() { return uint32_t(esp_timer_get_time() / 1000); }
class Files : public jot::Storage {
public:
    bool mounted = false;
    jot::Read read(const char* path, std::string& data) override {
        if (!mounted) return jot::Read::Error;
        const auto name = std::string("/jot") + path;
        FILE* f = fopen(name.c_str(), "rb");
        if (!f) return errno == ENOENT ? jot::Read::Missing : jot::Read::Error;
        data.clear(); char buffer[1024]; size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), f))) {
            data.append(buffer, n);
            if (data.size() > 70000) { fclose(f); return jot::Read::Error; }
        }
        const bool ok = !ferror(f); fclose(f);
        return ok ? jot::Read::Ok : jot::Read::Error;
    }
    bool write(const char* path, const std::string& data) override {
        if (!mounted) return false;
        const auto name = std::string("/jot") + path;
        FILE* f = fopen(name.c_str(), "wb");
        if (!f) return false;
        bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
        if (fflush(f) != 0 || fsync(fileno(f)) != 0) ok = false;
        if (fclose(f) != 0) ok = false;
        return ok;
    }
    bool verify(const char* path, const std::string& expected) override {
        const auto name = std::string("/jot") + path;
        FILE* f = fopen(name.c_str(), "rb");
        if (!f) return false;
        char buffer[512]; size_t offset = 0;
        while (offset < expected.size()) {
            const auto length = std::min(sizeof(buffer), expected.size() - offset);
            if (fread(buffer, 1, length, f) != length ||
                memcmp(buffer, expected.data() + offset, length) != 0) { fclose(f); return false; }
            offset += length;
        }
        const bool ok = fgetc(f) == EOF && !ferror(f);
        fclose(f); return ok;
    }
} files;
jot::Core core(files);
SemaphoreHandle_t mutex, radio;
TaskHandle_t worker;
std::string device;
std::atomic<bool> active{false}, online{false}, sending{false};
std::atomic<int> bleState{0};
int currentMode = 0;
char notice[48] = {};
uint32_t noticeUntil = 0, lastDraw = 0;
jot::Hold pairing;
i2c_master_bus_handle_t bus = nullptr;
i2c_master_dev_handle_t oled = nullptr;
esp_netif_t* netif = nullptr;
esp_event_handler_instance_t wifiHandler, ipHandler;
bool wifiOwned = false;

bool blankPartition() {
    const auto* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "jot");
    if (!p) return false;
    uint8_t data[256];
    for (size_t offset = 0; offset < p->size; offset += sizeof(data)) {
        const auto length = std::min<size_t>(sizeof(data), p->size - offset);
        if (esp_partition_read(p, offset, data, length) != ESP_OK) return false;
        for (size_t i = 0; i < length; ++i) if (data[i] != 0xff) return false;
    }
    return true;
}
void oledStop() {
    if (oled || bus) ESP_LOGI("jot", "OLED stop");
    if (oled || bus) printf("JOT: OLED stop\r\n");
    if (oled) { i2c_master_bus_rm_device(oled); oled = nullptr; }
    if (bus) { i2c_del_master_bus(bus); bus = nullptr; }
}
void oledStart() {
    ESP_LOGI("jot", "OLED start: SDA=26 SCL=25 address=0x%02x", JOT_OLED_ADDRESS);
    printf("JOT: OLED start SDA=26 SCL=25 address=0x%02x\r\n", JOT_OLED_ADDRESS);
    i2c_master_bus_config_t config = {};
    config.i2c_port = I2C_NUM_0; config.sda_io_num = GPIO_NUM_26; config.scl_io_num = GPIO_NUM_25;
    config.clk_source = I2C_CLK_SRC_DEFAULT; config.glitch_ignore_cnt = 7;
    config.flags.enable_internal_pullup = true;
    esp_err_t err = i2c_new_master_bus(&config, &bus);
    if (err != ESP_OK) {
        ESP_LOGE("jot", "OLED I2C bus init failed: %s", esp_err_to_name(err));
        printf("JOT: OLED I2C bus init failed: %s\r\n", esp_err_to_name(err));
        bus = nullptr;
        return;
    }
    err = i2c_master_probe(bus, JOT_OLED_ADDRESS, 50);
    if (err != ESP_OK) {
        ESP_LOGW("jot", "OLED probe failed at 0x%02x: %s", JOT_OLED_ADDRESS, esp_err_to_name(err));
        printf("JOT: OLED probe failed at 0x%02x: %s\r\n", JOT_OLED_ADDRESS, esp_err_to_name(err));
        oledStop();
        return;
    }
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7; dev.device_address = JOT_OLED_ADDRESS;
    dev.scl_speed_hz = 400000;
    err = i2c_master_bus_add_device(bus, &dev, &oled);
    if (err != ESP_OK) {
        ESP_LOGE("jot", "OLED add device failed: %s", esp_err_to_name(err));
        printf("JOT: OLED add device failed: %s\r\n", esp_err_to_name(err));
        oledStop();
        return;
    }
    const uint8_t init[] = {0x00, 0xae, 0xd5, 0x80, 0xa8, 0x3f, 0xd3, 0x00, 0x40,
        0x8d, 0x14, 0x20, 0x00, 0xa1, 0xc8, 0xda, 0x12, 0x81, 0x7f,
        0xd9, 0xf1, 0xdb, 0x40, 0xa4, 0xa6, 0xaf};
    err = i2c_master_transmit(oled, init, sizeof(init), 50);
    if (err != ESP_OK) {
        ESP_LOGE("jot", "OLED init transmit failed: %s", esp_err_to_name(err));
        printf("JOT: OLED init transmit failed: %s\r\n", esp_err_to_name(err));
        oledStop();
        return;
    }
    ESP_LOGI("jot", "OLED ready");
    printf("JOT: OLED ready\r\n");
}
void text(uint8_t* frame, unsigned row, const std::string& line) {
    for (unsigned column = 0; column < std::min(size_t(16), line.size()); ++column) {
        unsigned char c = line[column]; if (c > 127) c = '?';
        for (unsigned x = 0; x < 8; ++x) {
            uint8_t bits = 0;
            for (unsigned y = 0; y < 8; ++y) if (font8x8_basic[c][y] & (1u << x)) bits |= 1u << y;
            frame[1 + row * 128 + column * 8 + x] = bits;
        }
    }
}
void draw() {
    if (!oled) return;
    uint8_t frame[1025] = {}; frame[0] = 0x40;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const auto draft = core.draft(); const auto count = core.pending().size(); const bool ready = core.ready();
    xSemaphoreGive(mutex);
    char status[32];
    if (currentMode == 6) {
        snprintf(status, sizeof(status), "JOT %s Q:%u", sending ? "SEND" : online ? "WIFI" : "OFF", unsigned(count));
        text(frame, 0, status);
        std::string editor = draft + '_';
        const size_t rows = (editor.size() + 15) / 16;
        const size_t first = rows > 6 ? (rows - 6) * 16 : 0;
        for (unsigned r = 0; r < 6 && first + r * 16 < editor.size(); ++r)
            text(frame, r + 2, editor.substr(first + r * 16, 16));
    } else {
        text(frame, 0, "BLE keyboard");
        text(frame, 2, bleState == 2 ? "Connected" : "Not connected");
        text(frame, 3, bleState == 1 ? "Advertising" : "");
        text(frame, 5, "Fn+Sym+5: Jot");
        text(frame, 6, "Fn+Sym+0 hold:"); text(frame, 7, "Clear pairing");
    }
    if (!ready && currentMode == 6) text(frame, 1, "Storage error");
    else if (notice[0]) text(frame, 1, notice);
    const uint8_t window[] = {0x00, 0x21, 0, 127, 0x22, 0, 7};
    if (i2c_master_transmit(oled, window, sizeof(window), 50) != ESP_OK ||
        i2c_master_transmit(oled, frame, sizeof(frame), 100) != ESP_OK) {
        ESP_LOGW("jot", "OLED removed or unresponsive; keyboard continues"); oledStop();
    }
}
void wifiEvent(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) online = true;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) online = false;
}
bool wifiStart() {
    if (wifiOwned) return true;
    netif = esp_netif_create_default_wifi_sta();
    if (!netif) return false;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) { esp_netif_destroy_default_wifi(netif); netif = nullptr; return false; }
    wifiOwned = true;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEvent, nullptr, &wifiHandler);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifiEvent, nullptr, &ipHandler);
    esp_wifi_set_storage(WIFI_STORAGE_RAM); esp_wifi_set_mode(WIFI_MODE_STA);
    return esp_wifi_start() == ESP_OK;
}
void wifiStop() {
    if (!wifiOwned) return;
    esp_wifi_stop();
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiHandler);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ipHandler);
    esp_wifi_deinit(); esp_netif_destroy_default_wifi(netif);
    netif = nullptr; wifiOwned = false; online = false;
}
bool connect() {
    if (online) return true;
    if (!wifiStart()) return false;
    for (const auto& n : jot::networks) {
        if (!n.ssid || !active) break;
        if (strlen(n.ssid) > 32 || strlen(n.password) > 64) continue;
        online = false; esp_wifi_disconnect();
        wifi_config_t cfg = {};
        memcpy(cfg.sta.ssid, n.ssid, strlen(n.ssid)); memcpy(cfg.sta.password, n.password, strlen(n.password));
        if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK || esp_wifi_connect() != ESP_OK) continue;
        uint32_t start = now();
        while (!online && active && now() - start < 5000) vTaskDelay(pdMS_TO_TICKS(50));
        if (online) return true;
    }
    return false;
}
bool post(const std::vector<jot::Message>& batch) {
    const bool tls = std::string(JOT_API_URL).find("https://") == 0;
    if (tls && !strlen(JOT_CA_CERT)) return false;
    esp_http_client_config_t cfg = {};
    cfg.url = JOT_API_URL; cfg.timeout_ms = 5000;
    cfg.cert_pem = tls ? JOT_CA_CERT : nullptr;
    auto client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (strlen(JOT_API_TOKEN)) {
        const auto auth = std::string("Bearer ") + JOT_API_TOKEN;
        esp_http_client_set_header(client, "Authorization", auth.c_str());
    }
    const auto body = jot::payload(device, batch);
    esp_http_client_set_post_field(client, body.data(), body.size());
    const auto err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    ESP_LOGI("jot", "POST: %s, status %d", esp_err_to_name(err), status);
    return err == ESP_OK && status >= 200 && status < 300;
}
void networkTask(void*) {
    for (;;) {
        bool success = false;
        if (active) {
            xSemaphoreTake(radio, portMAX_DELAY);
            if (active) {
                xSemaphoreTake(mutex, portMAX_DELAY);
                const auto& pending = core.pending();
                const std::vector<jot::Message> batch(pending.begin(), pending.begin() + std::min<size_t>(5, pending.size()));
                const bool ready = core.ready();
                xSemaphoreGive(mutex);
                if (ready && !batch.empty() && connect() && active) {
                    sending = true; success = post(batch);
                    if (success) {
                        xSemaphoreTake(mutex, portMAX_DELAY);
                        success = core.acknowledge(batch);
                        xSemaphoreGive(mutex);
                        if (!success) ESP_LOGE("jot", "Acknowledgement write failed; retry retained");
                    }
                    sending = false;
                }
            }
            xSemaphoreGive(radio);
        }
        if (!success) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
    }
}
std::string nextId() {
    nvs_handle_t handle;
    if (nvs_open("jot", NVS_READWRITE, &handle) != ESP_OK) return {};
    uint32_t seq = 0; auto err = nvs_get_u32(handle, "seq", &seq);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) { nvs_close(handle); return {}; }
    if (seq == UINT32_MAX || nvs_set_u32(handle, "seq", seq + 1) != ESP_OK || nvs_commit(handle) != ESP_OK) {
        nvs_close(handle); return {};
    }
    nvs_close(handle);
    return device + '-' + std::to_string(seq + 1);
}
} // namespace

extern "C" void jot_device_init() {
    ESP_LOGI("jot", "Jot device init");
    printf("JOT: device init\r\n");
    mutex = xSemaphoreCreateMutex(); radio = xSemaphoreCreateMutex(); configASSERT(mutex && radio);
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/jot"; conf.partition_label = "jot"; conf.format_if_mount_failed = false;
    auto err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) ESP_LOGW("jot", "LittleFS mount failed: %s", esp_err_to_name(err));
    if (err != ESP_OK) printf("JOT: LittleFS mount failed: %s\r\n", esp_err_to_name(err));
    if (err != ESP_OK && blankPartition() && esp_littlefs_format("jot") == ESP_OK) {
        ESP_LOGI("jot", "Formatted blank Jot partition");
        printf("JOT: formatted blank partition\r\n");
        err = esp_vfs_littlefs_register(&conf);
    }
    files.mounted = err == ESP_OK;
    ESP_LOGI("jot", "LittleFS mounted: %s", files.mounted ? "yes" : "no");
    printf("JOT: LittleFS mounted: %s\r\n", files.mounted ? "yes" : "no");
    if (!core.load()) ESP_LOGE("jot", "Storage unavailable; existing data will not be erased");
    uint8_t mac[6]; ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    char id[40]; snprintf(id, sizeof(id), "cardkb2-%02x%02x%02x%02x%02x%02x", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    device = id;
    ESP_LOGI("jot", "Device id: %s", device.c_str());
    printf("JOT: device id %s\r\n", device.c_str());
    ESP_ERROR_CHECK(esp_netif_init());
    const auto loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(loop);
    configASSERT(xTaskCreate(networkTask, "jot-network", 8192, nullptr, 1, &worker) == pdPASS);
    ESP_LOGI("jot", "Jot network worker started");
    printf("JOT: network worker started\r\n");
}
extern "C" void jot_device_leave_mode() {
    ESP_LOGI("jot", "Leave mode: current=%d active=%s", currentMode, active ? "true" : "false");
    printf("JOT: leave mode current=%d active=%s\r\n", currentMode, active ? "true" : "false");
    active = false;
    xSemaphoreTake(radio, portMAX_DELAY); wifiStop(); xSemaphoreGive(radio);
    oledStop(); currentMode = 0;
}
extern "C" void jot_device_enter_mode(int mode) {
    ESP_LOGI("jot", "Enter mode: %d", mode);
    printf("JOT: enter mode %d\r\n", mode);
    currentMode = mode;
    if (mode == 6 || mode == 4) oledStart();
    active = mode == 6;
    ESP_LOGI("jot", "Jot active: %s", active ? "true" : "false");
    printf("JOT: active %s\r\n", active ? "true" : "false");
    if (active) xTaskNotifyGive(worker);
    lastDraw = now() - 100;
}
extern "C" void jot_device_feedback(const char* message) {
    snprintf(notice, sizeof(notice), "%s", message); noticeUntil = now() + 2500;
}
extern "C" void jot_device_key(char c) {
    if (currentMode != 6) return;
    if (c == '\n' || c == '\r') ESP_LOGI("jot", "Jot key: Enter");
    else if (c == '\b' || c == 0x7f) ESP_LOGI("jot", "Jot key: Backspace");
    else if (c >= 32 && c <= 126) ESP_LOGI("jot", "Jot key: '%c'", c);
    else ESP_LOGI("jot", "Jot key: 0x%02x", (unsigned char)c);
    if (c == '\n' || c == '\r') printf("JOT: key Enter\r\n");
    else if (c == '\b' || c == 0x7f) printf("JOT: key Backspace\r\n");
    else if (c >= 32 && c <= 126) printf("JOT: key '%c'\r\n", c);
    else printf("JOT: key 0x%02x\r\n", (unsigned char)c);
    xSemaphoreTake(mutex, portMAX_DELAY);
    jot::Result r = jot::Result::Ok;
    if (c == '\n' || c == '\r') r = core.submit(nextId());
    else if (c == '\b' || c == 0x7f) core.backspace();
    else r = core.type(c);
    xSemaphoreGive(mutex);
    if (r == jot::Result::Full) jot_device_feedback("Queue full");
    else if (r == jot::Result::Limit) jot_device_feedback("512-byte limit");
    else if (r == jot::Result::StorageError) jot_device_feedback("Save failed");
    else if ((c == '\n' || c == '\r') && r == jot::Result::Ok) { jot_device_feedback("Saved"); xTaskNotifyGive(worker); }
    lastDraw = now() - 100;
}
extern "C" void jot_device_tick() {
    if (notice[0] && int32_t(now() - noticeUntil) >= 0) notice[0] = 0;
    if (now() - lastDraw >= 100) { draw(); lastDraw = now(); }
}
extern "C" void jot_device_ble_state(int state) { bleState = state; }
extern "C" bool jot_device_pairing_hold(bool held) { return pairing.update(held, now(), 3000); }
