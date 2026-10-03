#pragma once
#if __has_include("jot_config.h")
#include "jot_config.h"
#endif
#ifndef JOT_WIFI_NETWORKS
#define JOT_WIFI_NETWORKS
#endif
#ifndef JOT_API_URL
#define JOT_API_URL "http://txtin.gs/api/v1/messages"
#endif
#ifndef JOT_API_TOKEN
#define JOT_API_TOKEN ""
#endif
#ifndef JOT_CA_CERT
#define JOT_CA_CERT ""
#endif
#ifndef JOT_OLED_ADDRESS
#define JOT_OLED_ADDRESS 0x3C
#endif
namespace jot {
struct Network { const char* ssid; const char* password; };
// Sentinel keeps an unconfigured build valid C++.
static const Network networks[] = { JOT_WIFI_NETWORKS {nullptr, nullptr} };
}
