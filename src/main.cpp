#include "M5Cardputer.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <Preferences.h>

#include <cstring>
#include <vector>

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

#ifndef JOT_API_URL
#define JOT_API_URL "http://txtin.gs/api/v1/messages"
#endif

#ifndef JOT_API_TOKEN
#define JOT_API_TOKEN ""
#endif

static const char* API_URL = JOT_API_URL;
static const char* API_TOKEN = JOT_API_TOKEN;

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

String buildPayload(
    const std::vector<Message>& messages
) {
    String json;

    json.reserve(
        256 + messages.size() * 128
    );

    json += "{";

    json += "\"device_id\":\"";
    json += jsonEscape(deviceId);
    json += "\",";

    json += "\"messages\":[";

    for (size_t i = 0;
         i < messages.size();
         i++) {

        if (i)
            json += ",";

        json += "{";

        json += "\"id\":\"";
        json += jsonEscape(messages[i].id);
        json += "\",";

        json += "\"text\":\"";
        json += jsonEscape(messages[i].text);
        json += "\"";

        json += "}";
    }

    json += "]}";

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
        M5Cardputer.Display;

    display.fillScreen(BLACK);

    display.setTextColor(WHITE);
    display.setTextSize(1);

    display.setCursor(4, 4);
    display.println("txtin.gs");

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
// HTTP upload
// -----------------------------------------------------------------------------

bool postMessages(
    const std::vector<Message>& messages
) {
    if (messages.empty())
        return true;

    String payload =
        buildPayload(messages);

    setNetworkStatus(
        true,
        true,
        WiFi.SSID().c_str()
    );

    WiFiClient plainClient;
    WiFiClientSecure secureClient;

    // Development only.
    //
    // Replace with CA verification
    // for production.
    secureClient.setInsecure();

    bool useTls =
        String(API_URL).startsWith("https://");

    WiFiClient& client =
        useTls
            ? static_cast<WiFiClient&>(secureClient)
            : plainClient;

    HTTPClient http;

    if (!http.begin(
        client,
        API_URL
    )) {
        setNetworkStatus(
            true,
            false,
            WiFi.SSID().c_str()
        );

        return false;
    }

    http.setTimeout(5000);

    http.addHeader(
        "Content-Type",
        "application/json"
    );

    if (strlen(API_TOKEN) > 0) {
        String authorization =
            "Bearer " +
            String(API_TOKEN);

        http.addHeader(
            "Authorization",
            authorization
        );
    }

    int status =
        http.POST(
            payload
        );

    Serial.printf(
        "POST status: %d\n",
        status
    );

    http.end();

    setNetworkStatus(
        WiFi.status() ==
            WL_CONNECTED,
        false,
        WiFi.status() ==
            WL_CONNECTED
            ? WiFi.SSID().c_str()
            : ""
    );

    return (
        status >= 200 &&
        status < 300
    );
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
            postMessages(pending);

        if (success) {

            // Remove ONLY what was in the
            // snapshot we successfully sent.
            //
            // Messages typed while HTTP was
            // running stay in the queue.

            removeDeliveredMessages(
                pending
            );

            // Don't sleep.
            //
            // Immediately loop around because
            // more messages may have arrived
            // while the HTTP request was active.

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

    auto cfg = M5.config();

    M5Cardputer.begin(
        cfg,
        true
    );

    M5Cardputer.Display.setRotation(1);

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

    M5Cardputer.update();

    handleKeyboard();

    if (uiDirty)
        drawUI();

    delay(5);
}
