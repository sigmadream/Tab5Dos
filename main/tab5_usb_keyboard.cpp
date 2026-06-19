#include "tab5_usb_keyboard.h"

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "usb/hid_host.h"

#include <string.h>

namespace {
static char const * TAG = "tab5_usb_kbd";

struct HidEvent {
  hid_host_device_handle_t handle;
  hid_host_driver_event_t event;
  void * arg;
};

tabdos::PcKeyboardController * s_keyboard = nullptr;
SemaphoreHandle_t s_machineMutex = nullptr;
QueueHandle_t s_hidEventQueue = nullptr;
bool s_loggedFirstInputReport = false;
void (*s_eventLog)(char const * message) = nullptr;

static void withKeyboard(void (*fn)(tabdos::PcKeyboardController &))
{
  if (!s_keyboard)
    return;
  if (s_machineMutex)
    xSemaphoreTake(s_machineMutex, portMAX_DELAY);
  fn(*s_keyboard);
  if (s_machineMutex)
    xSemaphoreGive(s_machineMutex);
}

static void forwardBootKeyboardReport(uint8_t const * data, size_t length)
{
  if (length < 8)
    return;

  if (!s_loggedFirstInputReport) {
    s_loggedFirstInputReport = true;
    ESP_LOGI(TAG, "first USB keyboard input report received");
    if (s_eventLog)
      s_eventLog("first USB keyboard input report received");
  }

  uint8_t keys[6] = {};
  memcpy(keys, data + 2, sizeof(keys));
  uint8_t modifiers = data[0];
  if (s_machineMutex)
    xSemaphoreTake(s_machineMutex, portMAX_DELAY);
  if (s_keyboard)
    s_keyboard->onHidBootKeyboardReport(modifiers, keys);
  if (s_machineMutex)
    xSemaphoreGive(s_machineMutex);
}

static void hidInterfaceCallback(hid_host_device_handle_t hidDeviceHandle,
                                 hid_host_interface_event_t event,
                                 void * arg)
{
  (void)arg;
  uint8_t data[64] = {};
  size_t dataLength = 0;
  hid_host_dev_params_t params = {};
  esp_err_t err = hid_host_device_get_params(hidDeviceHandle, &params);
  if (err != ESP_OK)
    return;

  switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
      err = hid_host_device_get_raw_input_report_data(hidDeviceHandle, data, sizeof(data), &dataLength);
      if (err == ESP_OK && params.proto == HID_PROTOCOL_KEYBOARD &&
          (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE || dataLength == 8))
        forwardBootKeyboardReport(data, dataLength);
      break;

    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "keyboard disconnected");
      withKeyboard([](tabdos::PcKeyboardController & keyboard) { keyboard.onUsbDisconnected(); });
      hid_host_device_close(hidDeviceHandle);
      break;

    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
      ESP_LOGW(TAG, "keyboard transfer error");
      break;

    default:
      break;
  }
}

static void hidDriverCallback(hid_host_device_handle_t hidDeviceHandle,
                              hid_host_driver_event_t event,
                              void * arg)
{
  if (!s_hidEventQueue)
    return;
  HidEvent hidEvent = {hidDeviceHandle, event, arg};
  xQueueSend(s_hidEventQueue, &hidEvent, 0);
}

static void hidEventsTask(void * arg)
{
  (void)arg;
  HidEvent event = {};
  while (xQueueReceive(s_hidEventQueue, &event, portMAX_DELAY) == pdTRUE) {
    if (event.event != HID_HOST_DRIVER_EVENT_CONNECTED)
      continue;

    hid_host_dev_params_t params = {};
    ESP_ERROR_CHECK(hid_host_device_get_params(event.handle, &params));
    if (params.proto != HID_PROTOCOL_KEYBOARD) {
      ESP_LOGI(TAG, "ignoring HID proto=%d", params.proto);
      continue;
    }

    hid_host_device_config_t deviceConfig = {};
    deviceConfig.callback = hidInterfaceCallback;
    deviceConfig.callback_arg = nullptr;
    ESP_ERROR_CHECK(hid_host_device_open(event.handle, &deviceConfig));
    if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
      ESP_ERROR_CHECK(hid_class_request_set_protocol(event.handle, HID_REPORT_PROTOCOL_BOOT));
      ESP_ERROR_CHECK(hid_class_request_set_idle(event.handle, 0, 0));
    } else {
      ESP_LOGI(TAG, "USB HID keyboard uses report protocol; accepting 8-byte boot-compatible reports");
    }
    ESP_ERROR_CHECK(hid_host_device_start(event.handle));
    ESP_LOGI(TAG, "USB boot keyboard connected");
    if (s_eventLog)
      s_eventLog("USB boot keyboard connected");
  }
}

} // namespace

esp_err_t Tab5UsbKeyboard::start(tabdos::PcKeyboardController * keyboard, SemaphoreHandle_t machineMutex, void (*eventLog)(char const * message))
{
  ESP_RETURN_ON_FALSE(keyboard, ESP_ERR_INVALID_ARG, TAG, "keyboard controller is null");
  s_keyboard = keyboard;
  s_machineMutex = machineMutex;
  s_eventLog = eventLog;

  ESP_RETURN_ON_ERROR(bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true), TAG, "USB host start failed");

  s_hidEventQueue = xQueueCreate(8, sizeof(HidEvent));
  ESP_RETURN_ON_FALSE(s_hidEventQueue, ESP_ERR_NO_MEM, TAG, "HID event queue allocation failed");

  hid_host_driver_config_t driverConfig = {};
  driverConfig.create_background_task = true;
  driverConfig.task_priority = 5;
  driverConfig.stack_size = 4096;
  driverConfig.core_id = 0;
  driverConfig.callback = hidDriverCallback;
  driverConfig.callback_arg = nullptr;
  ESP_RETURN_ON_ERROR(hid_host_install(&driverConfig), TAG, "HID host install failed");

  BaseType_t ok = xTaskCreatePinnedToCore(hidEventsTask, "hid_events", 4096, nullptr, 4, nullptr, 0);
  ESP_RETURN_ON_FALSE(ok == pdTRUE, ESP_ERR_NO_MEM, TAG, "HID event task allocation failed");
  return ESP_OK;
}
