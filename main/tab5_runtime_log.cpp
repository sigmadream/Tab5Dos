#include "tab5_runtime_log.h"

#include "esp_log.h"

#include <unistd.h>

namespace {
static char const * TAG = "tab5_runtime_log";
}

Tab5RuntimeLog::~Tab5RuntimeLog()
{
  close();
  if (m_mutex) {
    vSemaphoreDelete(m_mutex);
    m_mutex = nullptr;
  }
}

esp_err_t Tab5RuntimeLog::open()
{
  if (!m_mutex) {
    m_mutex = xSemaphoreCreateMutex();
    if (!m_mutex) {
      ESP_LOGW(TAG, "failed to allocate log mutex");
      return ESP_ERR_NO_MEM;
    }
  }

  close();
  if (!lock())
    return ESP_ERR_TIMEOUT;
  m_file = fopen(Path, "a");
  if (!m_file) {
    unlock();
    ESP_LOGW(TAG, "failed to open %s", Path);
    return ESP_FAIL;
  }
  unlock();

  line("--- TabDOS boot ---");
  return ESP_OK;
}

void Tab5RuntimeLog::close()
{
  if (!m_mutex) {
    if (m_file) {
      fflush(m_file);
      fclose(m_file);
      m_file = nullptr;
    }
    return;
  }

  if (!lock())
    return;
  if (m_file) {
    syncLocked();
    fclose(m_file);
  }
  m_file = nullptr;
  unlock();
}

void Tab5RuntimeLog::line(char const * fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vline(fmt, args);
  va_end(args);
}

void Tab5RuntimeLog::vline(char const * fmt, va_list args)
{
  if (!fmt || !m_mutex || !lock())
    return;
  if (m_file) {
    vfprintf(m_file, fmt, args);
    fputc('\n', m_file);
    syncLocked();
  }
  unlock();
}

bool Tab5RuntimeLog::lock()
{
  return m_mutex && xSemaphoreTake(m_mutex, portMAX_DELAY) == pdTRUE;
}

void Tab5RuntimeLog::unlock()
{
  if (m_mutex)
    xSemaphoreGive(m_mutex);
}

void Tab5RuntimeLog::syncLocked()
{
  if (!m_file)
    return;
  fflush(m_file);
  int fd = fileno(m_file);
  if (fd >= 0)
    fsync(fd);
}
