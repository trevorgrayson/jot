#if defined(USE_CARDKB2_UNIT)
#include <M5Unified.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedKEYBOARD.h>
#include <Wire.h>
#else
#include "M5Cardputer.h"
#endif

#include <WiFi.h>
#include <WiFiClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <PubSubClient.h>

#include <cctype>
#include <cstring>
#include <vector>

#if defined(USE_CARDKB2_UNIT)
namespace {
m5::unit::UnitUnified units;
m5::unit::UnitCardKB2 cardkb2;

bool cardkb2Ready = false;

bool setupCardKB2() {
    auto pinSda =
        M5.getPin(m5::pin_name_t::port_a_sda);
    auto pinScl =
        M5.getPin(m5::pin_name_t::port_a_scl);

    if (pinSda < 0 || pinScl < 0) {
        Serial.println(
            "CardKB2 I2C pins unavailable"
        );

        return false;
    }

    Wire.end();
    Wire.begin(pinSda, pinScl, 100000U);

    if (!units.add(cardkb2, Wire) ||
        !units.begin()) {
        Serial.println(
            "CardKB2 not found; set it to I2C mode with Fn+Sym+1"
        );

        return false;
    }

    Serial.printf(
        "CardKB2 firmware: %02x\n",
        cardkb2.firmwareVersion()
    );

    return true;
}
}  // namespace
#endif

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

#ifndef JOT_MQTT_HOST
#define JOT_MQTT_HOST ""
#endif

#ifndef JOT_MQTT_PORT
#define JOT_MQTT_PORT 1883
#endif

#ifndef JOT_MQTT_TOPIC
#define JOT_MQTT_TOPIC "jot/messages"
#endif

#ifndef JOT_MQTT_USER
#define JOT_MQTT_USER ""
#endif

#ifndef JOT_MQTT_PASSWORD
#define JOT_MQTT_PASSWORD ""
#endif

static const char* MQTT_HOST     = JOT_MQTT_HOST;
static const uint16_t MQTT_PORT  = JOT_MQTT_PORT;
static const char* MQTT_TOPIC    = JOT_MQTT_TOPIC;
static const char* MQTT_USER     = JOT_MQTT_USER;
static const char* MQTT_PASSWORD = JOT_MQTT_PASSWORD;

struct FriendlyNetwork {
    const char* ssid;
    const char* password;
};

FriendlyNetwork networks[] = {
// {"office-wifi",   "password"},
};

constexpr size_t NETWORK_COUNT =
    sizeof(networks) / sizeof(networks[0]);

constexpr size_t MAX_MESSAGE_LENGTH = 512;
constexpr size_t MAX_QUEUE_SIZE     = 100;

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 5000;
constexpr uint32_t OFFLINE_RETRY_MS        = 8000;
constexpr uint32_t SEND_RETRY_MS           = 5000;

static const char* QUEUE_FILE = "/messages.tsv";

// -----------------------------------------------------------------------------
// Message state
// -----------------------------------------------------------------------------

struct Message {
    String id;
    String text;
};

std::vector<Message> queue;

String input;
String deviceId;

Preferences preferences;

WiFiClient mqttNetworkClient;
PubSubClient mqttClient(mqttNetworkClient);

// Protects queue + queue file operations.
SemaphoreHandle_t queueMutex;

// Wakes network task immediately when a message is queued.
TaskHandle_t networkTaskHandle = nullptr;

// -----------------------------------------------------------------------------
// UI/network status
// -----------------------------------------------------------------------------

SemaphoreHandle_t statusMutex;

struct NetworkStatus {
    bool connected = false;
    bool sending = false;

    char ssid[33] = {0};
};

NetworkStatus networkStatus;

volatile bool uiDirty = true;

#if defined(USE_CARDKB2_UNIT)
#define APP_DISPLAY M5.Display
#else
#define APP_DISPLAY M5Cardputer.Display
#endif

// -----------------------------------------------------------------------------
// Device identity
// -----------------------------------------------------------------------------

String getDeviceId() {
    uint64_t chip = ESP.getEfuseMac();

    char buffer[32];

    snprintf(
        buffer,
        sizeof(buffer),
        "cardputer-%012llx",
        chip & 0xFFFFFFFFFFFFULL
    );

    return String(buffer);
}

String newMessageId() {
    uint32_t seq =
        preferences.getUInt("seq", 0) + 1;

    preferences.putUInt("seq", seq);

    uint64_t chip = ESP.getEfuseMac();

    char buffer[48];

    snprintf(
        buffer,
        sizeof(buffer),
        "%012llx-%lu",
        chip & 0xFFFFFFFFFFFFULL,
        (unsigned long)seq
    );

    return String(buffer);
}

// -----------------------------------------------------------------------------
// Persistent queue
// -----------------------------------------------------------------------------

void saveQueueLocked() {
    File f = LittleFS.open(QUEUE_FILE, "w");

    if (!f) {
        Serial.println("Unable to save queue");
        return;
    }

    for (const Message& message : queue) {
        String text = message.text;

        text.replace("\t", " ");
        text.replace("\r", " ");
        text.replace("\n", " ");

        f.print(message.id);
        f.print('\t');
        f.println(text);
    }

    f.close();
}

void loadQueue() {
    xSemaphoreTake(queueMutex, portMAX_DELAY);

    queue.clear();

    if (LittleFS.exists(QUEUE_FILE)) {
        File f = LittleFS.open(QUEUE_FILE, "r");

        if (f) {
            while (f.available()) {
                String line =
                    f.readStringUntil('\n');

                line.trim();

                if (line.length() == 0)
                    continue;

                int tab = line.indexOf('\t');

                if (tab < 0)
                    continue;

                Message message;

                message.id =
                    line.substring(0, tab);

                message.text =
                    line.substring(tab + 1);

                queue.push_back(message);
            }

            f.close();
        }
    }

    xSemaphoreGive(queueMutex);
}

// -----------------------------------------------------------------------------
// JSON
// -----------------------------------------------------------------------------

String jsonEscape(const String& value) {
    String out;

    out.reserve(value.length() + 16);

    for (size_t i = 0; i < value.length(); i++) {
        char c = value[i];

        switch (c) {
            case '"':
                out += "\\\"";
                break;

            case '\\':
                out += "\\\\";
                break;

            case '\n':
                out += "\\n";
                break;

            case '\r':
                out += "\\r";
                break;

            case '\t':
                out += "\\t";
                break;

            default:
                if ((uint8_t)c >= 0x20)
                    out += c;

                break;
        }
    }

    return out;
}

String buildMessagePayload(
    const Message& message
) {
    String json;

    json.reserve(
        128 + message.text.length()
    );

    json += "{";

    json += "\"device_id\":\"";
    json += jsonEscape(deviceId);
    json += "\",";

    json += "\"id\":\"";
    json += jsonEscape(message.id);
    json += "\",";

    json += "\"text\":\"";
    json += jsonEscape(message.text);
    json += "\"";

    json += "}";

    return json;
}

// -----------------------------------------------------------------------------
// Status helpers
// -----------------------------------------------------------------------------

void setNetworkStatus(
    bool connected,
    bool sending,
    const char* ssid = ""
) {
    xSemaphoreTake(
        statusMutex,
        portMAX_DELAY
    );

    networkStatus.connected = connected;
    networkStatus.sending = sending;

    strncpy(
        networkStatus.ssid,
        ssid,
        sizeof(networkStatus.ssid) - 1
    );

    networkStatus.ssid[
        sizeof(networkStatus.ssid) - 1
    ] = '\0';

    xSemaphoreGive(statusMutex);

    uiDirty = true;
}

// -----------------------------------------------------------------------------
// UI
// -----------------------------------------------------------------------------

void drawUI() {
    NetworkStatus status;

    xSemaphoreTake(
        statusMutex,
        portMAX_DELAY
    );

    status = networkStatus;

    xSemaphoreGive(statusMutex);

    std::vector<Message> queueSnapshot;

    xSemaphoreTake(
        queueMutex,
        portMAX_DELAY
    );

    queueSnapshot = queue;

    xSemaphoreGive(queueMutex);

    auto& display =
        APP_DISPLAY;

    display.fillScreen(BLACK);

    display.setTextColor(WHITE);
    display.setTextSize(1);

    display.setCursor(4, 4);
    display.println("jot mqtt");

    // Network

    display.setCursor(4, 18);

    if (status.connected) {
        display.print("wifi: ");
        display.print(status.ssid);

        if (status.sending)
            display.print(" [sending]");

        display.println();

    } else {
        display.println("wifi: offline");
    }

    // Queue

    display.setCursor(4, 31);

    display.printf(
        "queued: %d\n",
        (int)queueSnapshot.size()
    );

    int start =
        queueSnapshot.size() > 3
        ? queueSnapshot.size() - 3
        : 0;

    int y = 48;

    for (size_t i = start;
         i < queueSnapshot.size();
         i++) {

        String preview =
            queueSnapshot[i].text;

        if (preview.length() > 28) {
            preview =
                preview.substring(0, 25);

            preview += "...";
        }

        display.setCursor(4, y);

        display.print("> ");
        display.println(preview);

        y += 13;
    }

    // Input line

    display.drawFastHLine(
        0,
        103,
        display.width(),
        DARKGREY
    );

    display.setCursor(4, 110);

    display.print("> ");

    String visible = input;

    if (visible.length() > 34) {
        visible =
            visible.substring(
                visible.length() - 34
            );
    }

    display.print(visible);

    uiDirty = false;
}

// -----------------------------------------------------------------------------
// Queue message
// -----------------------------------------------------------------------------

void submitMessage() {
    input.trim();

    if (input.length() == 0)
        return;

    xSemaphoreTake(
        queueMutex,
        portMAX_DELAY
    );

    if (queue.size() >= MAX_QUEUE_SIZE) {
        xSemaphoreGive(queueMutex);
        return;
    }

    Message message;

    message.id = newMessageId();
    message.text = input;

    queue.push_back(message);

    // ENTER means durable.
    saveQueueLocked();

    xSemaphoreGive(queueMutex);

    input = "";

    uiDirty = true;

    // Wake network worker immediately.
    if (networkTaskHandle) {
        xTaskNotifyGive(
            networkTaskHandle
        );
    }
}

// -----------------------------------------------------------------------------
// Keyboard
// -----------------------------------------------------------------------------

void handleKeyboard() {
#if defined(USE_CARDKB2_UNIT)
    if (!cardkb2Ready)
        return;

    if (!cardkb2.updated())
        return;

    while (cardkb2.available()) {
        char c = cardkb2.getchar();
        cardkb2.discard();

        if (c == '\r' || c == '\n') {
            submitMessage();
            continue;
        }

        if (
            (c == '\b' || c == 0x7f) &&
            input.length() > 0
        ) {
            input.remove(
                input.length() - 1
            );

            uiDirty = true;
            continue;
        }

        if (
            isprint((unsigned char)c) &&
            input.length() < MAX_MESSAGE_LENGTH
        ) {
            input += c;
            uiDirty = true;
        }
    }
#else
    if (!M5Cardputer.Keyboard.isChange())
        return;

    if (!M5Cardputer.Keyboard.isPressed())
        return;

    Keyboard_Class::KeysState keys =
        M5Cardputer.Keyboard.keysState();

    for (char c : keys.word) {
        if (
            input.length() <
            MAX_MESSAGE_LENGTH
        ) {
            input += c;
        }
    }

    if (keys.del && input.length() > 0) {
        input.remove(
            input.length() - 1
        );
    }

    if (keys.enter) {
        submitMessage();
        return;
    }

    uiDirty = true;
#endif
}

// -----------------------------------------------------------------------------
// Queue snapshot
// -----------------------------------------------------------------------------

std::vector<Message> getQueueSnapshot() {
    std::vector<Message> snapshot;

    xSemaphoreTake(
        queueMutex,
        portMAX_DELAY
    );

    snapshot = queue;

    xSemaphoreGive(queueMutex);

    return snapshot;
}

// -----------------------------------------------------------------------------
// Remove successfully delivered messages
// -----------------------------------------------------------------------------

void removeDeliveredMessages(
    const std::vector<Message>& delivered
) {
    xSemaphoreTake(
        queueMutex,
        portMAX_DELAY
    );

    for (const Message& sent : delivered) {

        for (auto it = queue.begin();
             it != queue.end();) {

            if (it->id == sent.id) {
                it = queue.erase(it);
            } else {
                ++it;
            }
        }
    }

    saveQueueLocked();

    xSemaphoreGive(queueMutex);

    uiDirty = true;
}

// -----------------------------------------------------------------------------
// WiFi worker helpers
// -----------------------------------------------------------------------------

bool connectFriendlyWiFi() {
    if (WiFi.status() == WL_CONNECTED)
        return true;

    for (size_t i = 0;
         i < NETWORK_COUNT;
         i++) {

        Serial.printf(
            "Trying WiFi: %s\n",
            networks[i].ssid
        );

        WiFi.disconnect(true);

        vTaskDelay(
            pdMS_TO_TICKS(100)
        );

        WiFi.begin(
            networks[i].ssid,
            networks[i].password
        );

        uint32_t start = millis();

        while (
            WiFi.status() != WL_CONNECTED &&
            millis() - start <
                WIFI_CONNECT_TIMEOUT_MS
        ) {
            vTaskDelay(
                pdMS_TO_TICKS(100)
            );
        }

        if (
            WiFi.status() ==
            WL_CONNECTED
        ) {
            Serial.printf(
                "Connected: %s\n",
                WiFi.SSID().c_str()
            );

            setNetworkStatus(
                true,
                false,
                WiFi.SSID().c_str()
            );

            return true;
        }
    }

    setNetworkStatus(
        false,
        false
    );

    return false;
}

// -----------------------------------------------------------------------------
// MQTT publish
// -----------------------------------------------------------------------------

bool connectMqtt() {
    if (mqttClient.connected())
        return true;

    if (strlen(MQTT_HOST) == 0) {
        Serial.println(
            "MQTT host not configured"
        );

        return false;
    }

    mqttClient.setServer(
        MQTT_HOST,
        MQTT_PORT
    );

    mqttClient.setBufferSize(1024);

    Serial.printf(
        "Connecting MQTT: %s:%u\n",
        MQTT_HOST,
        MQTT_PORT
    );

    bool connected = false;

    if (strlen(MQTT_USER) > 0) {
        connected = mqttClient.connect(
            deviceId.c_str(),
            MQTT_USER,
            MQTT_PASSWORD
        );
    } else {
        connected = mqttClient.connect(
            deviceId.c_str()
        );
    }

    if (!connected) {
        Serial.printf(
            "MQTT connect failed: %d\n",
            mqttClient.state()
        );
    }

    return connected;
}

bool publishMessages(
    const std::vector<Message>& messages
) {
    if (messages.empty())
        return true;

    setNetworkStatus(
        true,
        true,
        WiFi.SSID().c_str()
    );

    if (!connectMqtt()) {
        setNetworkStatus(
            true,
            false,
            WiFi.SSID().c_str()
        );

        return false;
    }

    bool success = true;

    for (const Message& message : messages) {
        String payload =
            buildMessagePayload(message);

        bool published =
            mqttClient.publish(
                MQTT_TOPIC,
                payload.c_str()
            );

        mqttClient.loop();

        Serial.printf(
            "MQTT publish %s: %s\n",
            message.id.c_str(),
            published ? "ok" : "failed"
        );

        if (!published) {
            success = false;
            break;
        }
    }

    setNetworkStatus(
        WiFi.status() ==
            WL_CONNECTED,
        false,
        WiFi.status() ==
            WL_CONNECTED
            ? WiFi.SSID().c_str()
            : ""
    );

    return success;
}

// -----------------------------------------------------------------------------
// Network FreeRTOS task
// -----------------------------------------------------------------------------

void networkTask(void* parameter) {
    Serial.println(
        "Network task started"
    );

    while (true) {

        // Nothing queued?
        //
        // Sleep indefinitely until main loop
        // tells us something was added.

        std::vector<Message> pending =
            getQueueSnapshot();

        if (pending.empty()) {

            ulTaskNotifyTake(
                pdTRUE,
                portMAX_DELAY
            );

            continue;
        }

        // Find friendly WiFi.

        if (!connectFriendlyWiFi()) {

            // Wait either for retry interval
            // OR for another notification.

            ulTaskNotifyTake(
                pdTRUE,
                pdMS_TO_TICKS(
                    OFFLINE_RETRY_MS
                )
            );

            continue;
        }

        // IMPORTANT:
        //
        // pending is a snapshot.
        //
        // Keyboard can continue adding new
        // messages to the real queue while
        // this request blocks.

        bool success =
            publishMessages(pending);

        if (success) {

            // Remove ONLY what was in the
            // snapshot we successfully sent.
            //
            // Messages typed while MQTT was
            // running stay in the queue.

            removeDeliveredMessages(
                pending
            );

            // Don't sleep.
            //
            // Immediately loop around because
            // more messages may have arrived
            // while MQTT publishing was active.

            continue;
        }

        if (
            WiFi.status() !=
            WL_CONNECTED
        ) {
            setNetworkStatus(
                false,
                false
            );
        }

        ulTaskNotifyTake(
            pdTRUE,
            pdMS_TO_TICKS(
                SEND_RETRY_MS
            )
        );
    }
}

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);

#if defined(USE_CARDKB2_UNIT)
    auto cfg = M5.config();

    M5.begin(cfg);

    M5.Display.setRotation(1);

    cardkb2Ready =
        setupCardKB2();
#else
    auto cfg = M5.config();

    M5Cardputer.begin(
        cfg,
        true
    );

    M5Cardputer.Display.setRotation(1);
#endif

    LittleFS.begin(true);

    preferences.begin(
        "txtings",
        false
    );

    queueMutex =
        xSemaphoreCreateMutex();

    statusMutex =
        xSemaphoreCreateMutex();

    deviceId =
        getDeviceId();

    loadQueue();

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    // Network worker.
    //
    // Arduino's main loop can remain concerned
    // solely with keyboard/display interaction.

    xTaskCreatePinnedToCore(
        networkTask,
        "txtings-network",
        8192,              // stack bytes
        nullptr,
        1,                 // low priority
        &networkTaskHandle,
        0                  // Core 0
    );

    drawUI();

    // If we booted with queued messages,
    // wake worker immediately.

    xSemaphoreTake(
        queueMutex,
        portMAX_DELAY
    );

    bool hasPending =
        !queue.empty();

    xSemaphoreGive(queueMutex);

    if (hasPending) {
        xTaskNotifyGive(
            networkTaskHandle
        );
    }
}

// -----------------------------------------------------------------------------
// Main loop
// -----------------------------------------------------------------------------

void loop() {
    // This path stays very fast regardless of
    // WiFi or server behavior.

#if defined(USE_CARDKB2_UNIT)
    M5.update();
    units.update();
#else
    M5Cardputer.update();
#endif

    handleKeyboard();

    if (uiDirty)
        drawUI();

    delay(5);
}
