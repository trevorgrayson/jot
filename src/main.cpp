#if defined(USE_CARDKB2_UNIT)
#include <M5Unified.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedKEYBOARD.h>
#include <Wire.h>
#define APP_DISPLAY M5.Display
#else
#include <M5Cardputer.h>
#define APP_DISPLAY M5Cardputer.Display
#endif
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_partition.h>
#include <JotCore.h>
#include <JotConfig.h>
#include "CardputerHid.h"
#include <atomic>
#include <cstring>

namespace {
class FlashStorage : public jot::Storage {
public:
    bool mounted = false;
    jot::Read read(const char* path, std::string& data) override {
        if (!mounted) return jot::Read::Error;
        if (!LittleFS.exists(path)) return jot::Read::Missing;
        File file = LittleFS.open(path, "r");
        if (!file || file.size() > 70000) return jot::Read::Error;
        data.resize(file.size());
        const size_t n = file.readBytes(&data[0], data.size());
        file.close();
        return n == data.size() ? jot::Read::Ok : jot::Read::Error;
    }
    bool write(const char* path, const std::string& data) override {
        if (!mounted) return false;
        File file = LittleFS.open(path, "w");
        if (!file) return false;
        bool ok = file.write(reinterpret_cast<const uint8_t*>(data.data()), data.size()) == data.size();
        file.flush(); file.close(); return ok;
    }
    bool verify(const char* path, const std::string& expected) override {
        File file = LittleFS.open(path, "r");
        if (!file || file.size() != expected.size()) return false;
        char buffer[512]; size_t offset = 0;
        while (offset < expected.size()) {
            const size_t length = std::min(sizeof(buffer), expected.size() - offset);
            if (file.readBytes(buffer, length) != length ||
                memcmp(buffer, expected.data() + offset, length) != 0) { file.close(); return false; }
            offset += length;
        }
        file.close(); return true;
    }
} storage;
jot::Core core(storage);
Preferences preferences;
SemaphoreHandle_t coreMutex, radioMutex;
TaskHandle_t worker;
std::string deviceId;
std::atomic<bool> bleMode{false}, switching{false}, connected{false}, sending{false};
std::atomic<bool> dirty{true};
char notice[48] = {};
uint32_t noticeUntil = 0;

#if defined(USE_CARDKB2_UNIT)
m5::unit::UnitUnified units;
m5::unit::UnitCardKB2 keyboard;
bool keyboardReady = false;
#else
CardputerHid hid;
jot::Hold toggleHold, pairingHold;
bool waitForRelease = true, wasConnected = false;
#endif

void feedback(const char* message) {
    snprintf(notice, sizeof(notice), "%s", message);
    noticeUntil = millis() + 2500; dirty = true;
}

bool blankFilesystem() {
    const auto* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                     ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (!partition) return false;
    uint8_t buffer[256];
    for (size_t offset = 0; offset < partition->size; offset += sizeof(buffer)) {
        size_t length = std::min<size_t>(sizeof(buffer), partition->size - offset);
        if (esp_partition_read(partition, offset, buffer, length) != ESP_OK) return false;
        for (size_t i = 0; i < length; ++i) if (buffer[i] != 0xff) return false;
    }
    return true;
}

std::string nextId() {
    uint32_t seq = preferences.getUInt("seq", 0);
    if (seq == UINT32_MAX || preferences.putUInt("seq", seq + 1) != sizeof(seq)) return {};
    char id[48];
    snprintf(id, sizeof(id), "%012llx-%lu", ESP.getEfuseMac() & 0xffffffffffffULL,
             static_cast<unsigned long>(seq + 1));
    return id;
}

void key(char c) {
    xSemaphoreTake(coreMutex, portMAX_DELAY);
    jot::Result result = jot::Result::Ok;
    if (c == '\r' || c == '\n') result = core.submit(nextId());
    else if (c == '\b' || c == 0x7f) core.backspace();
    else result = core.type(c);
    xSemaphoreGive(coreMutex);
    if (result == jot::Result::Full) feedback("Queue full; draft retained");
    else if (result == jot::Result::StorageError) feedback("Save failed; draft retained");
    else if (result == jot::Result::Limit) feedback("512-byte limit");
    else if ((c == '\r' || c == '\n') && result == jot::Result::Ok) {
        feedback("Saved"); xTaskNotifyGive(worker);
    }
    dirty = true;
}

bool connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    for (const auto& network : jot::networks) {
        if (!network.ssid || switching || bleMode) break;
        WiFi.disconnect(); WiFi.begin(network.ssid, network.password);
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 5000 && !switching)
            vTaskDelay(pdMS_TO_TICKS(50));
        if (WiFi.status() == WL_CONNECTED) return true;
    }
    return false;
}

bool post(const std::vector<jot::Message>& batch) {
    WiFiClient plain;
    WiFiClientSecure secure;
    const bool tls = std::string(JOT_API_URL).find("https://") == 0;
    if (tls && !strlen(JOT_CA_CERT)) return false;
    if (tls) secure.setCACert(JOT_CA_CERT);
    WiFiClient& client = tls ? static_cast<WiFiClient&>(secure) : plain;
    HTTPClient http;
    if (!http.begin(client, JOT_API_URL)) return false;
    http.setConnectTimeout(5000); http.setTimeout(5000);
    http.addHeader("Content-Type", "application/json");
    if (strlen(JOT_API_TOKEN)) http.addHeader("Authorization", String("Bearer ") + JOT_API_TOKEN);
    const auto body = jot::payload(deviceId, batch);
    const int status = http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(body.data())), body.size());
    http.end();
    Serial.printf("POST status: %d\n", status);
    return status >= 200 && status < 300;
}

void networkTask(void*) {
    for (;;) {
        bool successful = false;
        if (!bleMode && !switching) {
            xSemaphoreTake(radioMutex, portMAX_DELAY);
            if (!bleMode && !switching) {
                xSemaphoreTake(coreMutex, portMAX_DELAY);
                const auto& pending = core.pending();
                std::vector<jot::Message> batch(pending.begin(), pending.begin() + std::min<size_t>(5, pending.size()));
                const bool ready = core.ready();
                xSemaphoreGive(coreMutex);
                if (ready && !batch.empty()) {
                    connected = connectWiFi(); dirty = true;
                    if (connected && !switching) {
                        sending = true; dirty = true;
                        successful = post(batch);
                        if (successful) {
                            xSemaphoreTake(coreMutex, portMAX_DELAY);
                            successful = core.acknowledge(batch);
                            xSemaphoreGive(coreMutex);
                            if (!successful) Serial.println("Queue acknowledgement save failed; retry retained");
                        }
                        sending = false;
                    }
                    connected = WiFi.status() == WL_CONNECTED; dirty = true;
                }
            }
            xSemaphoreGive(radioMutex);
        }
        if (!successful) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
    }
}

#if !defined(USE_CARDKB2_UNIT)
void selectMode(bool useBle) {
    // The worker checks switching and yields its radio lock after settling an in-flight POST.
    switching = true; dirty = true;
    APP_DISPLAY.fillScreen(BLACK); APP_DISPLAY.setCursor(4, 4);
    APP_DISPLAY.println("Switching mode...");
    xSemaphoreTake(radioMutex, portMAX_DELAY);
    hid.stop();
    WiFi.disconnect(true); connected = false;
    bleMode = useBle;
    if (useBle) hid.start(); else WiFi.mode(WIFI_STA);
    waitForRelease = true; wasConnected = false;
    if (preferences.putBool("ble", useBle) != 1) feedback("Mode save failed");
    xSemaphoreGive(radioMutex);
    switching = false; dirty = true;
    xTaskNotifyGive(worker);
}

void handleKeyboard() {
    const auto keys = M5Cardputer.Keyboard.keysState();
    const bool b = std::find(keys.hid_keys.begin(), keys.hid_keys.end(), 0x05) != keys.hid_keys.end();
    const bool reset = keys.fn && keys.shift && b;
    const bool toggle = keys.fn && !keys.shift && b;
    if (toggleHold.update(toggle, millis(), 0)) { selectMode(!bleMode); return; }
    if (pairingHold.update(bleMode && reset, millis(), 3000)) {
        feedback(hid.clearPairing() ? "Pairing cleared" : "Pairing reset failed");
        waitForRelease = true;
    }
    if (toggle || reset) { hid.release(); waitForRelease = true; return; }
    bool allUp = keys.hid_keys.empty() && keys.modifiers == 0 && !keys.fn;
    bool link = hid.connected;
    if (link != wasConnected) { wasConnected = link; waitForRelease = true; hid.release(); dirty = true; }
    if (waitForRelease) { if (allUp) waitForRelease = false; return; }
    if (!M5Cardputer.Keyboard.isChange()) return;
    if (bleMode) {
        uint8_t report[8] = {}; report[0] = keys.modifiers;
        size_t n = 0;
        for (uint8_t code : keys.hid_keys) {
            if (keys.fn) {
                if (code == 0x35) code = 0x29; // Fn+` = Escape
                else if (code == 0x33) code = 0x52; // Fn+; = Up
                else if (code == 0x36) code = 0x50; // Fn+, = Left
                else if (code == 0x37) code = 0x51; // Fn+. = Down
                else if (code == 0x38) code = 0x4f; // Fn+/ = Right
            }
            if (n < 6) report[2 + n++] = code;
        }
        if (keys.hid_keys.size() > 6) memset(report + 2, 1, 6); // HID rollover
        hid.report(report);
    } else if (M5Cardputer.Keyboard.isPressed()) {
        for (char c : keys.word) key(c);
        if (keys.del) key('\b');
        if (keys.enter) key('\n');
    }
}
#else
void handleKeyboard() {
    if (!keyboardReady || !keyboard.updated()) return;
    while (keyboard.available()) { char c = keyboard.getchar(); keyboard.discard(); key(c); }
}
#endif

void drawUI() {
    xSemaphoreTake(coreMutex, portMAX_DELAY);
    const auto draft = core.draft(); const bool ready = core.ready();
    const auto& all = core.pending(); const auto count = all.size();
    const std::vector<jot::Message> pending(all.end() - std::min<size_t>(3, count), all.end());
    xSemaphoreGive(coreMutex);
    auto& display = APP_DISPLAY;
    display.fillScreen(BLACK); display.setTextSize(1); display.setTextColor(WHITE);
    display.setCursor(4, 4);
    display.printf("Jot %s  queued: %u", bleMode ? "BLE" : "notes", unsigned(count));
    display.setCursor(4, 18);
#if !defined(USE_CARDKB2_UNIT)
    if (bleMode) {
        display.println(hid.connected ? "BLE connected" : "Advertising / not connected");
        display.setCursor(4, 45); display.println("Fn+B: return to notes");
        display.println("Fn+Shift+B (3s): clear pairing");
    } else
#endif
    {
        display.println(sending ? "wifi: sending" : connected ? "wifi: connected" : "wifi: offline");
        int y = 45;
        size_t start = pending.size() > 3 ? pending.size() - 3 : 0;
        for (size_t i = start; i < pending.size(); ++i, y += 13) {
            display.setCursor(4, y); display.printf("> %.28s", pending[i].text.c_str());
        }
        display.drawFastHLine(0, 101, display.width(), DARKGREY);
        display.setCursor(4, 110);
        const auto visible = draft.size() > 34 ? draft.substr(draft.size() - 34) : draft;
        display.printf("> %s", visible.c_str());
    }
    display.setCursor(4, 31);
    if (!ready) display.print("Storage unavailable; notes kept");
    else if (notice[0]) display.print(notice);
    dirty = false;
}
} // namespace

void setup() {
    Serial.begin(115200);
#if defined(USE_CARDKB2_UNIT)
    M5.begin(M5.config()); M5.Display.setRotation(1);
    const int sda = M5.getPin(m5::pin_name_t::port_a_sda), scl = M5.getPin(m5::pin_name_t::port_a_scl);
    if (sda >= 0 && scl >= 0) {
        Wire.end(); Wire.begin(sda, scl, 100000U);
        keyboardReady = units.add(keyboard, Wire) && units.begin();
    }
    if (!keyboardReady) Serial.println("CardKB2 not found; use Fn+Sym+1 for I2C mode");
#else
    M5Cardputer.begin(M5.config(), true); M5Cardputer.Display.setRotation(1);
#endif
    coreMutex = xSemaphoreCreateMutex(); radioMutex = xSemaphoreCreateMutex();
    configASSERT(coreMutex && radioMutex);
    const bool preferencesReady = preferences.begin("txtings", false);
    storage.mounted = LittleFS.begin(false);
    // Only fully erased storage can be initialized automatically. Never erase a failed mount.
    if (!storage.mounted && blankFilesystem() && LittleFS.format()) storage.mounted = LittleFS.begin(false);
    if (!core.load()) feedback("Storage unavailable");
    char id[32]; snprintf(id, sizeof(id), "cardputer-%012llx", ESP.getEfuseMac() & 0xffffffffffffULL);
    deviceId = id; // Preserve existing host identities.
    WiFi.mode(WIFI_STA);
    const BaseType_t task = xTaskCreate(networkTask, "jot-network", 8192, nullptr, 1, &worker);
    configASSERT(task == pdPASS);
#if !defined(USE_CARDKB2_UNIT)
    if (preferencesReady && preferences.getBool("ble", false)) selectMode(true);
#else
    (void)preferencesReady;
#endif
    drawUI();
}

void loop() {
#if defined(USE_CARDKB2_UNIT)
    M5.update(); units.update();
#else
    M5Cardputer.update();
#endif
    handleKeyboard();
    if (notice[0] && int32_t(millis() - noticeUntil) >= 0) { notice[0] = 0; dirty = true; }
    if (dirty) drawUI();
    delay(5);
}
