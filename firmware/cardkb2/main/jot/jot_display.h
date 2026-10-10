#pragma once
#include <array>
#include <string>

// Called exclusively by the app task. Owns LVGL and the Grove master bus;
// stop() releases both before factory wired transports take ownership.
namespace jot::display {
void start();
void stop();
void update(const std::array<std::string, 8>& rows);
}
