#pragma once
// Copy to jot_config.h (ignored by Git). Shared by Arduino and ESP-IDF builds.
#define JOT_WIFI_NETWORKS {"office-wifi", "replace-me"}, {"home-wifi", "replace-me"},
#define JOT_API_URL "http://txtin.gs/api/v1/messages"
#define JOT_API_TOKEN ""
// HTTPS endpoints require a PEM trust root. No insecure TLS fallback is used.
#define JOT_CA_CERT ""
#define JOT_OLED_ADDRESS 0x3C
