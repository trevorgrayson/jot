#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void jot_device_init(void);
void jot_device_leave_mode(void);
void jot_device_enter_mode(int mode);
void jot_device_key(char c);
void jot_device_tick(void);
void jot_device_ble_state(int state);
void jot_device_feedback(const char* text);
bool jot_device_pairing_hold(bool held);
#ifdef __cplusplus
}
#endif
