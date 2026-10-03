#pragma once
#if !defined(USE_CARDKB2_UNIT)
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#if defined(CONFIG_BLUEDROID_ENABLED)
#include <esp_gap_ble_api.h>
#else
#include <host/ble_store.h>
#endif
#include <atomic>

// Cardputer adapter: uses Arduino's ESP32 BLE stack; CardKB2 retains OEM HID code.
class CardputerHid : public BLEServerCallbacks {
public:
    std::atomic<bool> connected{false};
    std::atomic<bool> enabled{false};
    void onConnect(BLEServer* server) override {
        if (!enabled) { server->disconnect(server->getConnId()); return; }
        connected = true;
    }
    void onDisconnect(BLEServer*) override {
        connected = false;
        if (enabled) BLEDevice::startAdvertising();
    }
    void start() {
        if (!hid_) {
            char name[32];
            snprintf(name, sizeof(name), "Jot-Cardputer-%04x", unsigned(ESP.getEfuseMac() & 0xffff));
            BLEDevice::init(name);
            server_ = BLEDevice::createServer(); server_->setCallbacks(this);
            hid_ = new BLEHIDDevice(server_);
            input_ = hid_->inputReport(1);
            hid_->outputReport(1);
            hid_->manufacturer()->setValue("Jot");
            hid_->pnp(2, 0x16c0, 0x05df, 0x0100);
            hid_->hidInfo(0, 1);
            static const uint8_t map[] = {
                0x05,0x01,0x09,0x06,0xA1,0x01,0x85,0x01,
                0x05,0x07,0x19,0xE0,0x29,0xE7,0x15,0x00,0x25,0x01,
                0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x01,
                0x95,0x05,0x75,0x01,0x05,0x08,0x19,0x01,0x29,0x05,0x91,0x02,
                0x95,0x01,0x75,0x03,0x91,0x01,0x95,0x06,0x75,0x08,
                0x15,0x00,0x25,0x65,0x05,0x07,0x19,0x00,0x29,0x65,0x81,0x00,0xC0
            };
            hid_->reportMap(const_cast<uint8_t*>(map), sizeof(map));
            security_.setAuthenticationMode(ESP_LE_AUTH_BOND);
            security_.setCapability(ESP_IO_CAP_NONE);
            security_.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
            security_.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
            hid_->startServices(); hid_->setBatteryLevel(100);
            auto* advertising = server_->getAdvertising();
            advertising->setAppearance(0x03c1);
            advertising->addServiceUUID(hid_->hidService()->getUUID());
            advertising->setScanResponse(true);
        }
        enabled = true;
        BLEDevice::startAdvertising();
    }
    void report(const uint8_t (&keys)[8]) {
        if (enabled && connected && input_) {
            input_->setValue(const_cast<uint8_t*>(keys), sizeof(keys)); input_->notify();
        }
    }
    void release() { const uint8_t empty[8] = {}; report(empty); }
    void stop() {
        if (!hid_) return;
        release(); enabled = false;
        BLEDevice::stopAdvertising();
        if (connected) {
            server_->disconnect(server_->getConnId());
            uint32_t start = millis();
            while (connected && millis() - start < 1000) delay(5);
        }
        // Keep controller memory allocated so repeated Jot/BLE transitions remain supported.
    }
    bool clearPairing() {
        if (!hid_) return false;
        stop();
        if (connected) return false;
#if defined(CONFIG_BLUEDROID_ENABLED)
        int count = esp_ble_get_bond_device_num();
        if (count) {
            std::vector<esp_ble_bond_dev_t> bonds(count);
            if (esp_ble_get_bond_device_list(&count, bonds.data()) != ESP_OK) { start(); return false; }
            for (int i = 0; i < count; ++i) {
                if (esp_ble_remove_bond_device(bonds[i].bd_addr) != ESP_OK) { start(); return false; }
            }
        }
#else
        if (ble_store_clear() != 0) { start(); return false; }
#endif
        start(); return true;
    }
private:
    BLEServer* server_ = nullptr;
    BLEHIDDevice* hid_ = nullptr;
    BLECharacteristic* input_ = nullptr;
    BLESecurity security_;
};
#endif
