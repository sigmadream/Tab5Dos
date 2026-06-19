#pragma once

#include "pc/pc_keyboard_controller.h"

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

class Tab5UsbKeyboard {
public:
  esp_err_t start(tabdos::PcKeyboardController * keyboard, SemaphoreHandle_t machineMutex, void (*eventLog)(char const * message) = nullptr);
};
