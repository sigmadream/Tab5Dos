#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdarg.h>
#include <stdio.h>

class Tab5RuntimeLog {
public:
  static constexpr char const * Path = "/sdcard/dos/tabdos.log";

  ~Tab5RuntimeLog();

  esp_err_t open();
  void close();
  bool isOpen() const { return m_file != nullptr; }
  void line(char const * fmt, ...);
  void vline(char const * fmt, va_list args);

private:
  bool lock();
  void unlock();
  void syncLocked();

  FILE * m_file = nullptr;
  SemaphoreHandle_t m_mutex = nullptr;
};
