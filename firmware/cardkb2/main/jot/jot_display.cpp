#include "jot_display.h"
#include "JotConfig.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <cstdio>
#include <cstring>

namespace jot::display {
namespace {
i2c_master_bus_handle_t bus = nullptr;
i2c_master_dev_handle_t oled = nullptr;
lv_display_t* display = nullptr;
lv_obj_t* labels[8] = {};
lv_font_t editorFont;
// I1 is row-major, MSB first, with an 8-byte palette prefix.
alignas(4) uint8_t pixels[128 * 64 / 8 + 8];
uint8_t frame[128 * 64 / 8 + 1];
bool initialized = false, flushFailed = false;
uint32_t tick() { return uint32_t(esp_timer_get_time() / 1000); }
void flush(lv_display_t* disp, const lv_area_t*, uint8_t* data) {
    data += 8;
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x40;
    // Full rendering always supplies 128x64 pixels. SSD1306 stores vertical
    // columns in eight pages rather than LVGL's horizontal rows.
    for (unsigned y = 0; y < 64; ++y)
        for (unsigned x = 0; x < 128; ++x)
            if (data[y * 16 + x / 8] & (0x80u >> (x % 8)))
                frame[1 + (y / 8) * 128 + x] |= 1u << (y % 8);
    const uint8_t window[] = {0x00, 0x21, 0, 127, 0x22, 0, 7};
    if (!flushFailed && oled &&
        (i2c_master_transmit(oled, window, sizeof(window), 50) != ESP_OK ||
         i2c_master_transmit(oled, frame, sizeof(frame), 100) != ESP_OK))
        flushFailed = true;
    // Always complete the flush, including NACKs. Delete the display only
    // after LVGL has returned from rendering.
    lv_display_flush_ready(disp);
}
} // namespace
void stop() {
    if (display) { lv_display_delete(display); display = nullptr; }
    for (auto& label : labels) label = nullptr;
    flushFailed = false;
    if (oled || bus) ESP_LOGI("jot", "OLED stop");
    if (oled || bus) printf("JOT: OLED stop\r\n");
    if (oled) { i2c_master_bus_rm_device(oled); oled = nullptr; }
    if (bus) { i2c_del_master_bus(bus); bus = nullptr; }
}
void start() {
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
        stop();
        return;
    }
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7; dev.device_address = JOT_OLED_ADDRESS;
    dev.scl_speed_hz = 400000;
    err = i2c_master_bus_add_device(bus, &dev, &oled);
    if (err != ESP_OK) {
        ESP_LOGE("jot", "OLED add device failed: %s", esp_err_to_name(err));
        printf("JOT: OLED add device failed: %s\r\n", esp_err_to_name(err));
        stop();
        return;
    }
    const uint8_t init[] = {0x00, 0xae, 0xd5, 0x80, 0xa8, 0x3f, 0xd3, 0x00, 0x40,
        0x8d, 0x14, 0x20, 0x00, 0xa1, 0xc8, 0xda, 0x12, 0x81, 0x7f,
        0xd9, 0xf1, 0xdb, 0x40, 0xa4, 0xa6, 0xaf};
    err = i2c_master_transmit(oled, init, sizeof(init), 50);
    if (err != ESP_OK) {
        ESP_LOGE("jot", "OLED init transmit failed: %s", esp_err_to_name(err));
        printf("JOT: OLED init transmit failed: %s\r\n", esp_err_to_name(err));
        stop();
        return;
    }
    if (!initialized) {
        lv_init();
        lv_tick_set_cb(tick);
        editorFont = lv_font_unscii_8;
        // UNSCII glyphs occupy eight pixels; remove its extra top leading
        // so the cursor and descenders fit in each eight-pixel OLED row.
        editorFont.line_height = 8;
        initialized = true;
    }
    display = lv_display_create(128, 64);
    if (!display) { stop(); return; }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(display, pixels, nullptr, sizeof(pixels), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    auto* screen = lv_display_get_screen_active(display);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    for (unsigned row = 0; row < 8; ++row) {
        auto* label = labels[row] = lv_label_create(screen);
        lv_obj_remove_style_all(label);
        lv_obj_set_pos(label, 0, row * 8);
        lv_obj_set_size(label, 128, 8);
        lv_obj_set_style_text_font(label, &editorFont, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(label, "");
    }
    ESP_LOGI("jot", "OLED ready (LVGL)");
    printf("JOT: OLED ready\r\n");
}
void update(const std::array<std::string, 8>& rows) {
    if (!display) return;
    for (unsigned row = 0; row < rows.size(); ++row)
        if (rows[row] != lv_label_get_text(labels[row]))
            lv_label_set_text(labels[row], rows[row].c_str());
    lv_timer_handler();
    if (flushFailed) {
        ESP_LOGW("jot", "OLED removed or unresponsive; keyboard continues");
        stop();
    }
}
} // namespace jot::display
