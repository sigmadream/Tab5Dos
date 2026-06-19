#include "tab5_lcd_text.h"
#include "tab5_runtime_log.h"
#include "tab5_usb_keyboard.h"

#include "pc/pc_disk_catalog.h"
#include "pc/pc_machine.h"

#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <ctype.h>
#include <string>
#include <string.h>
#include <sys/stat.h>

namespace {
static char const * TAG = "tabdos_tab5";
static constexpr int CpuStepsPerSlice = 25000;
static constexpr int CpuSlicesBeforeDelay = 8;
static constexpr TickType_t CpuRestDelay = 1;
static constexpr TickType_t DisplayDelay = pdMS_TO_TICKS(100);
static constexpr TickType_t DiskFlushInterval = pdMS_TO_TICKS(1000);
static constexpr TickType_t TouchPollDelay = pdMS_TO_TICKS(20);
static constexpr TickType_t SyntheticKeyHoldDelay = pdMS_TO_TICKS(90);
static constexpr TickType_t SyntheticKeyGapDelay = pdMS_TO_TICKS(20);
static constexpr int SyntheticKeyQueueCapacity = 32;
static constexpr int LcdWidth = 720;
static constexpr int LcdHeight = 1280;
#if TABDOS_AUTOTEST_KEYS
static constexpr TickType_t AutotestInitialDelay = pdMS_TO_TICKS(30000);
static constexpr TickType_t AutotestPollDelay = pdMS_TO_TICKS(500);
static constexpr TickType_t AutotestKeyDelay = pdMS_TO_TICKS(250);
static constexpr TickType_t AutotestPostWriteDelay = pdMS_TO_TICKS(10000);
static constexpr int AutotestPromptPolls = 360;
static constexpr int AutotestWritePolls = 480;
#endif

struct ScreenMilestones {
  bool prompt;
  bool dirListing;
  bool writeFile;
};

struct AppState {
  tabdos::PcMachine machine;
  Tab5LcdText display;
  Tab5RuntimeLog runtimeLog;
  Tab5UsbKeyboard usbKeyboard;
  SemaphoreHandle_t mutex = nullptr;
  esp_lcd_touch_handle_t touch = nullptr;
  uint8_t syntheticKeyModifiers[SyntheticKeyQueueCapacity] = {};
  uint8_t syntheticKeyUsages[SyntheticKeyQueueCapacity] = {};
  int syntheticKeyRead = 0;
  int syntheticKeyCount = 0;
  bool syntheticKeyActive = false;
  TickType_t syntheticKeyReleaseAt = 0;
  TickType_t syntheticKeyNextPressAt = 0;
};

struct BootDiskSelection {
  int diskIndex;
  uint8_t biosDrive;
  char const * label;
};

static AppState s_app;

#if TABDOS_AUTOTEST_KEYS
struct HidKey {
  uint8_t modifier;
  uint8_t usage;
};

static HidKey asciiToHid(uint8_t ch)
{
  if (ch >= 'a' && ch <= 'z')
    return {0, static_cast<uint8_t>(0x04 + ch - 'a')};
  if (ch >= 'A' && ch <= 'Z')
    return {0x02, static_cast<uint8_t>(0x04 + ch - 'A')};
  if (ch >= '1' && ch <= '9')
    return {0, static_cast<uint8_t>(0x1e + ch - '1')};
  if (ch == '0')
    return {0, 0x27};

  switch (ch) {
    case '\r':
    case '\n': return {0, 0x28};
    case ' ': return {0, 0x2c};
    case '.': return {0, 0x37};
    case '>': return {0x02, 0x37};
    case ':': return {0x02, 0x33};
    case '\\': return {0, 0x31};
    default: return {0, 0};
  }
}
#endif

static int normalizedTextColumns(int columns)
{
  return columns == 40 ? 40 : tabdos::PcTextRenderer::Columns;
}

static bool textContains(uint8_t const * text80, int columns, char const * needle)
{
  if (!text80 || !needle || !*needle)
    return false;

  columns = normalizedTextColumns(columns);
  char screen[tabdos::PcTextRenderer::Rows * (tabdos::PcTextRenderer::Columns + 1) + 1] = {};
  size_t out = 0;
  for (int row = 0; row < tabdos::PcTextRenderer::Rows; ++row) {
    for (int col = 0; col < columns; ++col) {
      uint8_t ch = text80[2 * (row * columns + col)];
      screen[out++] = ch >= 0x20 && ch < 0x7f ? static_cast<char>(ch) : ' ';
    }
    screen[out++] = '\n';
  }
  screen[out] = '\0';
  return strstr(screen, needle) != nullptr;
}

static bool textContainsAny(uint8_t const * text80, int columns, char const * const * needles, size_t needleCount)
{
  for (size_t i = 0; i < needleCount; ++i) {
    if (textContains(text80, columns, needles[i]))
      return true;
  }
  return false;
}

static void reportScreenMilestones(uint8_t const * text80, int columns, ScreenMilestones * milestones)
{
  if (!milestones)
    return;
  static char const * const PromptNeedles[] = { "A:\\>", "C:\\>" };
  if (!milestones->prompt && textContainsAny(text80, columns, PromptNeedles, sizeof(PromptNeedles) / sizeof(PromptNeedles[0]))) {
    milestones->prompt = true;
    ESP_LOGI(TAG, "TEXT80 milestone: DOS prompt detected");
    s_app.runtimeLog.line("TEXT80 milestone: DOS prompt detected");
  }
  static char const * const DirNeedles[] = { "Directory of A:", "Directory of C:" };
  if (!milestones->dirListing && textContainsAny(text80, columns, DirNeedles, sizeof(DirNeedles) / sizeof(DirNeedles[0]))) {
    milestones->dirListing = true;
    ESP_LOGI(TAG, "TEXT80 milestone: DIR listing detected");
    s_app.runtimeLog.line("TEXT80 milestone: DIR listing detected");
  }
  if (!milestones->writeFile &&
      (textContains(text80, columns, "WRITE    TXT") || textContains(text80, columns, "W        TXT"))) {
    milestones->writeFile = true;
    ESP_LOGI(TAG, "TEXT80 milestone: DOS-created file detected");
    s_app.runtimeLog.line("TEXT80 milestone: DOS-created file detected");
  }
}

static esp_err_t mountSdCard()
{
  esp_err_t err = bsp_sdcard_mount();
  if (err == ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "SD card already mounted; continuing");
    return ESP_OK;
  }
  ESP_RETURN_ON_ERROR(err, TAG, "SD card mount failed");
  mkdir(tabdos::PcDiskCatalog::DefaultDirectory, 0775);
  return ESP_OK;
}

static void logHeapSnapshot(char const * label)
{
  unsigned internal = static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT));
  unsigned psram = static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  ESP_LOGI(TAG, "%s heap: internal/8bit=%u psram=%u", label, internal, psram);
  s_app.runtimeLog.line("%s heap: internal/8bit=%u psram=%u", label, internal, psram);
}

static bool lcdToSourcePoint(uint16_t lcdX,
                             uint16_t lcdY,
                             int sourceWidth,
                             int sourceHeight,
                             uint16_t * sourceX,
                             uint16_t * sourceY)
{
  if (!sourceX || !sourceY || sourceWidth <= 0 || sourceHeight <= 0)
    return false;

  int const rotatedWidth = sourceHeight;
  int const rotatedHeight = sourceWidth;
  int contentWidth = LcdWidth;
  int contentHeight = rotatedHeight * contentWidth / rotatedWidth;
  if (contentHeight > LcdHeight) {
    contentHeight = LcdHeight;
    contentWidth = rotatedWidth * contentHeight / rotatedHeight;
  }
  int const contentX = (LcdWidth - contentWidth) / 2;
  int const contentY = (LcdHeight - contentHeight) / 2;
  int const localX = static_cast<int>(lcdX) - contentX;
  int const localY = static_cast<int>(lcdY) - contentY;
  if (localX < 0 || localY < 0 || localX >= contentWidth || localY >= contentHeight)
    return false;

  int const rotatedX = localX * rotatedWidth / contentWidth;
  int const rotatedY = localY * rotatedHeight / contentHeight;
  int sx = sourceWidth - 1 - rotatedY;
  int sy = rotatedX;
  if (sx < 0)
    sx = 0;
  if (sx >= sourceWidth)
    sx = sourceWidth - 1;
  if (sy < 0)
    sy = 0;
  if (sy >= sourceHeight)
    sy = sourceHeight - 1;
  *sourceX = static_cast<uint16_t>(sx);
  *sourceY = static_cast<uint16_t>(sy);
  return true;
}

static uint8_t letterToHidUsage(uint8_t ch)
{
  if (ch >= 'A' && ch <= 'Z')
    ch = static_cast<uint8_t>(ch - 'A' + 'a');
  if (ch >= 'a' && ch <= 'z')
    return static_cast<uint8_t>(0x04 + ch - 'a');
  return 0;
}

static bool tickReached(TickType_t now, TickType_t deadline)
{
  return static_cast<int32_t>(now - deadline) >= 0;
}

static bool queueSyntheticHidKeyTap(AppState & app, uint8_t modifier, uint8_t usage)
{
  if (usage == 0)
    return false;
  if (app.syntheticKeyCount >= SyntheticKeyQueueCapacity)
    return false;

  int const write = (app.syntheticKeyRead + app.syntheticKeyCount) % SyntheticKeyQueueCapacity;
  app.syntheticKeyModifiers[write] = modifier;
  app.syntheticKeyUsages[write] = usage;
  ++app.syntheticKeyCount;
  return true;
}

static void serviceSyntheticKeyboard(AppState & app, TickType_t now)
{
  uint8_t emptyReport[6] = {};
  if (app.syntheticKeyActive && tickReached(now, app.syntheticKeyReleaseAt)) {
    app.machine.keyboard().onHidBootKeyboardReport(0, emptyReport);
    app.syntheticKeyActive = false;
    app.syntheticKeyNextPressAt = now + SyntheticKeyGapDelay;
  }

  if (app.syntheticKeyActive || app.syntheticKeyCount == 0 || !tickReached(now, app.syntheticKeyNextPressAt))
    return;

  uint8_t const modifier = app.syntheticKeyModifiers[app.syntheticKeyRead];
  uint8_t const usage = app.syntheticKeyUsages[app.syntheticKeyRead];
  app.syntheticKeyRead = (app.syntheticKeyRead + 1) % SyntheticKeyQueueCapacity;
  --app.syntheticKeyCount;

  uint8_t keyReport[6] = {usage, 0, 0, 0, 0, 0};
  app.machine.keyboard().onHidBootKeyboardReport(modifier, keyReport);
  app.syntheticKeyActive = true;
  app.syntheticKeyReleaseAt = now + SyntheticKeyHoldDelay;
}

static uint8_t textCellChar(uint8_t const * text80, int columns, int row, int col)
{
  columns = normalizedTextColumns(columns);
  if (!text80 || row < 0 || row >= tabdos::PcTextRenderer::Rows || col < 0 || col >= columns)
    return 0;
  return text80[2 * (row * columns + col)];
}

static uint8_t textCellAttribute(uint8_t const * text80, int columns, int row, int col)
{
  columns = normalizedTextColumns(columns);
  if (!text80 || row < 0 || row >= tabdos::PcTextRenderer::Rows || col < 0 || col >= columns)
    return 0;
  return text80[2 * (row * columns + col) + 1];
}

static bool isTextMenuTopLeft(uint8_t ch)
{
  return ch == 0xda || ch == 0xc9 || ch == 0xd5 || ch == 0xd6;
}

static bool isTextMenuTopRight(uint8_t ch)
{
  return ch == 0xbf || ch == 0xbb || ch == 0xb8 || ch == 0xb7;
}

static bool isTextMenuBottomLeft(uint8_t ch)
{
  return ch == 0xc0 || ch == 0xc8 || ch == 0xd4 || ch == 0xd3;
}

static bool isTextMenuBottomRight(uint8_t ch)
{
  return ch == 0xd9 || ch == 0xbc || ch == 0xbe || ch == 0xbd;
}

static bool isTextMenuItemRow(uint8_t const * text80, int columns, int row, int left, int right)
{
  for (int col = left + 1; col < right; ++col) {
    uint8_t const ch = textCellChar(text80, columns, row, col);
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
      return true;
  }
  return false;
}

struct TextMenuPane {
  int top;
  int bottom;
  int left;
  int right;
};

static bool findTextMenuPane(uint8_t const * text80, int columns, int tapRow, int tapCol, TextMenuPane * pane)
{
  if (!text80 || !pane || tapRow <= 0 || tapRow >= tabdos::PcTextRenderer::Rows)
    return false;

  columns = normalizedTextColumns(columns);
  for (int top = 1; top < tabdos::PcTextRenderer::Rows - 1; ++top) {
    for (int left = 0; left < columns - 2; ++left) {
      if (!isTextMenuTopLeft(textCellChar(text80, columns, top, left)))
        continue;
      for (int right = left + 2; right < columns; ++right) {
        if (!isTextMenuTopRight(textCellChar(text80, columns, top, right)))
          continue;
        if (tapCol <= left || tapCol >= right || tapRow <= top)
          continue;

        int bottom = tabdos::PcTextRenderer::Rows - 1;
        for (int row = top + 1; row < tabdos::PcTextRenderer::Rows; ++row) {
          if (isTextMenuBottomLeft(textCellChar(text80, columns, row, left)) &&
              isTextMenuBottomRight(textCellChar(text80, columns, row, right))) {
            bottom = row;
            break;
          }
        }
        if (tapRow >= bottom)
          continue;

        pane->top = top;
        pane->bottom = bottom;
        pane->left = left;
        pane->right = right;
        return true;
      }
    }
  }
  return false;
}

static int highlightedTextMenuRow(uint8_t const * text80, int columns, TextMenuPane const & pane)
{
  int bestRow = -1;
  int bestScore = 0;
  for (int row = pane.top + 1; row < pane.bottom; ++row) {
    if (!isTextMenuItemRow(text80, columns, row, pane.left, pane.right))
      continue;
    int score = 0;
    for (int col = pane.left + 1; col < pane.right; ++col) {
      uint8_t const ch = textCellChar(text80, columns, row, col);
      uint8_t const attr = textCellAttribute(text80, columns, row, col);
      if (ch != ' ' && (attr & 0x70) == 0x70)
        ++score;
    }
    if (score > bestScore) {
      bestScore = score;
      bestRow = row;
    }
  }
  return bestRow;
}

static uint8_t textMenuItemMnemonicUsage(uint8_t const * text80, int columns, int row, int left, int right)
{
  uint8_t fallback = 0;
  for (int col = left + 1; col < right; ++col) {
    uint8_t const ch = textCellChar(text80, columns, row, col);
    uint8_t const usage = letterToHidUsage(ch);
    if (usage == 0)
      continue;
    if (fallback == 0)
      fallback = usage;
    uint8_t const attr = textCellAttribute(text80, columns, row, col);
    if ((attr & 0x0f) == 0x0f)
      return usage;
  }
  return fallback;
}

static bool injectTextMenuItemSelection(AppState & app, uint8_t const * text80, int columns, int row, int col)
{
  TextMenuPane pane = {};
  if (!findTextMenuPane(text80, columns, row, col, &pane) ||
      !isTextMenuItemRow(text80, columns, row, pane.left, pane.right))
    return false;

  uint8_t const mnemonic = textMenuItemMnemonicUsage(text80, columns, row, pane.left, pane.right);
  if (mnemonic != 0) {
    // QBasic-style text menus respond more reliably to item mnemonic letters
    // than to synthesized arrow-key walks, especially when they hook INT 09h.
    return queueSyntheticHidKeyTap(app, 0, mnemonic) &&
           queueSyntheticHidKeyTap(app, 0, 0x28); // Enter
  }

  int const highlightedRow = highlightedTextMenuRow(text80, columns, pane);
  if (highlightedRow >= 0) {
    uint8_t const direction = row > highlightedRow ? 0x51 : 0x52; // HID down/up
    int const moves = row > highlightedRow ? row - highlightedRow : highlightedRow - row;
    for (int i = 0; i < moves; ++i) {
      if (!queueSyntheticHidKeyTap(app, 0, direction))
        return false;
    }
  }

  // Text-mode menu frameworks such as QBasic often consume the first Enter
  // while closing the highlighted menu pane, then complete the selected
  // command on the next keyboard poll. Queueing two taps makes direct touch
  // selection behave like a deliberate keyboard selection without relying on
  // DOS mouse support.
  return queueSyntheticHidKeyTap(app, 0, 0x28) && // Enter
         queueSyntheticHidKeyTap(app, 0, 0x28);   // Enter confirmation
}

static bool injectTextMenuShortcut(AppState & app, uint16_t sourceX, uint16_t sourceY, int columns)
{
  columns = normalizedTextColumns(columns);
  int const row = sourceY / tabdos::PcTextRenderer::CellHeight;
  int const col = static_cast<int>(sourceX) * columns / tabdos::PcTextRenderer::Width9;
  if (col < 0 || col >= columns || row < 0 || row >= tabdos::PcTextRenderer::Rows)
    return false;

  uint8_t const * text80 = app.machine.text80Buffer();
  if (!text80)
    return false;

  if (row > 0)
    return injectTextMenuItemSelection(app, text80, columns, row, col);

  auto charAt = [text80, columns](int column) {
    return textCellChar(text80, columns, 0, column);
  };

  int start = col;
  while (start > 0 && charAt(start) != ' ')
    --start;
  if (charAt(start) == ' ')
    ++start;
  if (start >= columns || charAt(start) == ' ')
    return false;

  uint8_t const usage = letterToHidUsage(charAt(start));
  if (usage == 0)
    return false;

  queueSyntheticHidKeyTap(app, 0x04, usage); // Left Alt + menu accelerator
  return true;
}

static bool startsWithIgnoreCase(char const * text, char const * prefix)
{
  if (!text || !prefix)
    return false;
  while (*prefix) {
    if (!*text)
      return false;
    if (tolower(static_cast<unsigned char>(*text)) != tolower(static_cast<unsigned char>(*prefix)))
      return false;
    ++text;
    ++prefix;
  }
  return true;
}

static char const * baseName(char const * path)
{
  if (!path)
    return "";
  char const * slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static bool isLikelyHardDiskImage(std::string const & path)
{
  char const * name = baseName(path.c_str());
  if (startsWithIgnoreCase(name, "hd"))
    return true;

  struct stat st = {};
  if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
    return st.st_size > 2880 * 1024;
  return false;
}

static BootDiskSelection bootDiskSelectionForImage(std::string const & path)
{
  if (isLikelyHardDiskImage(path))
    return {2, 0x80, "hard disk"};
  return {0, 0x00, "floppy"};
}

static esp_err_t bootDefaultImage(AppState & app, std::string * imagePath)
{
  logHeapSnapshot("before PcMachine init");
  ESP_RETURN_ON_FALSE(app.machine.init(), ESP_ERR_NO_MEM, TAG, "PcMachine init failed");
  logHeapSnapshot("after PcMachine init");

  std::string path;
  ESP_RETURN_ON_FALSE(tabdos::PcDiskCatalog::chooseDefaultImage(tabdos::PcDiskCatalog::DefaultDirectory, &path),
                      ESP_ERR_NOT_FOUND,
                      TAG,
                      "no DOS image found in %s",
                      tabdos::PcDiskCatalog::DefaultDirectory);

  BootDiskSelection const bootDisk = bootDiskSelectionForImage(path);
  ESP_LOGI(TAG,
           "Boot image: %s (%s BIOS drive %02x)",
           path.c_str(),
           bootDisk.label,
           bootDisk.biosDrive);
  app.runtimeLog.line("Boot image: %s (%s BIOS drive %02x)", path.c_str(), bootDisk.label, bootDisk.biosDrive);
  ESP_RETURN_ON_FALSE(app.machine.openDisk(bootDisk.diskIndex, path.c_str()), ESP_FAIL, TAG, "open disk image failed");
  if (tabdos::PcDiskImage * disk = app.machine.disk(bootDisk.diskIndex)) {
    disk->setFlushPolicy(tabdos::PcDiskImage::FlushPolicy::Deferred);
    ESP_LOGI(TAG, "Disk write-back enabled: dirty sectors fsync every %u ms", static_cast<unsigned>(DiskFlushInterval * portTICK_PERIOD_MS));
    app.runtimeLog.line("Disk write-back enabled: dirty sectors fsync every %u ms",
                        static_cast<unsigned>(DiskFlushInterval * portTICK_PERIOD_MS));
  }
  ESP_RETURN_ON_FALSE(app.machine.loadBootSector(bootDisk.diskIndex, bootDisk.biosDrive), ESP_FAIL, TAG, "load boot sector failed");
  ESP_RETURN_ON_FALSE(app.machine.prepareBootCpu(), ESP_FAIL, TAG, "prepare CPU failed");

  if (imagePath)
    *imagePath = path;
  return ESP_OK;
}

static void cpuTask(void * arg)
{
  auto * app = static_cast<AppState *>(arg);
  tabdos::PcMachine::Diagnostics lastDiagnostics = {};
  uint64_t flushedDiskWriteCount = 0;
  TickType_t nextDiskFlush = xTaskGetTickCount() + DiskFlushInterval;
  int slicesSinceDelay = 0;
  while (true) {
    xSemaphoreTake(app->mutex, portMAX_DELAY);
    for (int i = 0; i < CpuStepsPerSlice; ++i)
      app->machine.stepCpu();
    tabdos::PcMachine::Diagnostics diagnostics = app->machine.diagnostics();
    TickType_t const now = xTaskGetTickCount();
    if (diagnostics.diskWriteCount != flushedDiskWriteCount && now >= nextDiskFlush) {
      if (!app->machine.flushDisks()) {
        ESP_LOGE(TAG, "disk flush failed");
        app->runtimeLog.line("disk flush failed");
      }
      flushedDiskWriteCount = diagnostics.diskWriteCount;
      nextDiskFlush = now + DiskFlushInterval;
    }
    xSemaphoreGive(app->mutex);

    bool changed = diagnostics.unsupportedInterruptCount != lastDiagnostics.unsupportedInterruptCount ||
                   diagnostics.unsupportedPortReadCount != lastDiagnostics.unsupportedPortReadCount ||
                   diagnostics.unsupportedPortWriteCount != lastDiagnostics.unsupportedPortWriteCount;
    if (diagnostics.diskWriteCount != lastDiagnostics.diskWriteCount) {
      ESP_LOGI(TAG,
               "DISK milestone: write count=%llu drive=%02x lba=%llu sectors=%u",
               static_cast<unsigned long long>(diagnostics.diskWriteCount),
               diagnostics.lastDiskWriteDrive,
               static_cast<unsigned long long>(diagnostics.lastDiskWriteLba),
               diagnostics.lastDiskWriteCount);
      app->runtimeLog.line("DISK milestone: write count=%llu drive=%02x lba=%llu sectors=%u",
                           static_cast<unsigned long long>(diagnostics.diskWriteCount),
                           diagnostics.lastDiskWriteDrive,
                           static_cast<unsigned long long>(diagnostics.lastDiskWriteLba),
                           diagnostics.lastDiskWriteCount);
      lastDiagnostics = diagnostics;
    }
    if (changed && (diagnostics.unsupportedInterruptCount || diagnostics.unsupportedPortReadCount || diagnostics.unsupportedPortWriteCount)) {
      ESP_LOGW(TAG,
               "diagnostic int=%llu portR=%llu portW=%llu lastInt=%02x ah=%02x read=%04x write=%04x",
               static_cast<unsigned long long>(diagnostics.unsupportedInterruptCount),
               static_cast<unsigned long long>(diagnostics.unsupportedPortReadCount),
               static_cast<unsigned long long>(diagnostics.unsupportedPortWriteCount),
               diagnostics.lastUnsupportedInterrupt,
               diagnostics.lastUnsupportedInterruptAh,
               diagnostics.lastUnsupportedPortRead,
               diagnostics.lastUnsupportedPortWrite);
      lastDiagnostics = diagnostics;
    }
    ++slicesSinceDelay;
    if (slicesSinceDelay >= CpuSlicesBeforeDelay) {
      slicesSinceDelay = 0;
      vTaskDelay(CpuRestDelay);
    } else {
      taskYIELD();
    }
  }
}

static void displayTask(void * arg)
{
  auto * app = static_cast<AppState *>(arg);
  ScreenMilestones milestones = {};
  auto * videoSnapshot = static_cast<uint8_t *>(heap_caps_malloc(tabdos::PcMachine::VideoMemorySize,
                                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!videoSnapshot)
    videoSnapshot = static_cast<uint8_t *>(heap_caps_malloc(tabdos::PcMachine::VideoMemorySize, MALLOC_CAP_8BIT));
  if (!videoSnapshot) {
    ESP_LOGE(TAG, "display text snapshot allocation failed");
    app->runtimeLog.line("display text snapshot allocation failed");
    vTaskDelete(nullptr);
    return;
  }
  int constexpr MaxGraphicsPixels = tabdos::PcMachine::VgaGraphics16Width * tabdos::PcMachine::VgaGraphics16Height;
  auto * graphicsFrame = static_cast<uint16_t *>(heap_caps_malloc(MaxGraphicsPixels * sizeof(uint16_t),
                                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!graphicsFrame)
    graphicsFrame = static_cast<uint16_t *>(heap_caps_malloc(MaxGraphicsPixels * sizeof(uint16_t), MALLOC_CAP_8BIT));
  if (!graphicsFrame) {
    ESP_LOGE(TAG, "display graphics framebuffer allocation failed");
    app->runtimeLog.line("display graphics framebuffer allocation failed");
    heap_caps_free(videoSnapshot);
    vTaskDelete(nullptr);
    return;
  }

  while (true) {
    tabdos::PcTextRenderer rendererSnapshot;
    tabdos::PcMachine::VideoMode videoMode = tabdos::PcMachine::VideoMode::Text80;
    int graphicsWidth = 0;
    int graphicsHeight = 0;
    xSemaphoreTake(app->mutex, portMAX_DELAY);
    rendererSnapshot = app->machine.textRenderer();
    videoMode = app->machine.videoMode();
    memcpy(videoSnapshot, app->machine.videoMemory(), tabdos::PcMachine::VideoMemorySize);
    if (app->machine.isGraphicsMode()) {
      graphicsWidth = app->machine.graphicsWidth();
      graphicsHeight = app->machine.graphicsHeight();
      if (graphicsWidth * graphicsHeight <= MaxGraphicsPixels)
        app->machine.renderGraphicsFrameForDisplay(graphicsFrame, graphicsWidth);
    }
    xSemaphoreGive(app->mutex);

    uint8_t const * text80 = videoSnapshot + (tabdos::PcMachine::TextColorMemoryBase - tabdos::PcMachine::VideoMemoryBase);
    reportScreenMilestones(text80, rendererSnapshot.columns(), &milestones);
    if (videoMode != tabdos::PcMachine::VideoMode::Text80 && graphicsWidth > 0 && graphicsHeight > 0)
      app->display.blitRgb565(graphicsFrame, graphicsWidth, graphicsHeight);
    else
      app->display.blitText80(rendererSnapshot, text80);
    vTaskDelay(DisplayDelay);
  }
}

static void touchMouseTask(void * arg)
{
  auto * app = static_cast<AppState *>(arg);
  bool haveLastSource = false;
  uint16_t lastSourceX = 0;
  uint16_t lastSourceY = 0;
  int lastSourceWidth = tabdos::PcTextRenderer::Width9;
  int lastSourceHeight = tabdos::PcTextRenderer::Height;
  bool wasPressed = false;
  bool suppressMouseButtonUntilRelease = false;

  while (true) {
    esp_err_t err = esp_lcd_touch_read_data(app->touch);
    esp_lcd_touch_point_data_t point = {};
    uint8_t pointCount = 0;
    if (err == ESP_OK)
      err = esp_lcd_touch_get_data(app->touch, &point, &pointCount, 1);

    xSemaphoreTake(app->mutex, portMAX_DELAY);
    serviceSyntheticKeyboard(*app, xTaskGetTickCount());
    int sourceWidth = tabdos::PcTextRenderer::Width9;
    int sourceHeight = tabdos::PcTextRenderer::Height;
    int textColumns = app->machine.textRenderer().columns();
    if (app->machine.isGraphicsMode()) {
      sourceWidth = app->machine.graphicsWidth();
      sourceHeight = app->machine.graphicsHeight();
      textColumns = tabdos::PcTextRenderer::Columns;
    }

    uint16_t sourceX = lastSourceX;
    uint16_t sourceY = lastSourceY;
    bool const pressed = err == ESP_OK && pointCount > 0;
    if (pressed && lcdToSourcePoint(point.x, point.y, sourceWidth, sourceHeight, &sourceX, &sourceY)) {
      lastSourceX = sourceX;
      lastSourceY = sourceY;
      lastSourceWidth = sourceWidth;
      lastSourceHeight = sourceHeight;
      haveLastSource = true;
      if (!wasPressed && !app->machine.isGraphicsMode()) {
        suppressMouseButtonUntilRelease = injectTextMenuShortcut(*app, sourceX, sourceY, textColumns);
        if (suppressMouseButtonUntilRelease)
          ESP_LOGI(TAG, "touch text-menu keyboard fallback at source=%u,%u", sourceX, sourceY);
      }
      app->machine.setMouseSourceState(sourceX,
                                       sourceY,
                                       static_cast<uint16_t>(sourceWidth),
                                       static_cast<uint16_t>(sourceHeight),
                                       suppressMouseButtonUntilRelease ? 0x0000 : 0x0001);
    } else if (haveLastSource) {
      suppressMouseButtonUntilRelease = false;
      app->machine.setMouseSourceState(lastSourceX,
                                       lastSourceY,
                                       static_cast<uint16_t>(lastSourceWidth),
                                       static_cast<uint16_t>(lastSourceHeight),
                                       0x0000);
    } else {
      suppressMouseButtonUntilRelease = false;
      app->machine.setMouseState(0, 0, 0);
    }
    wasPressed = pressed;
    xSemaphoreGive(app->mutex);

    vTaskDelay(TouchPollDelay);
  }
}

#if TABDOS_AUTOTEST_KEYS
static bool autotestScreenContains(AppState * app, char const * needle)
{
  bool found = false;
  xSemaphoreTake(app->mutex, portMAX_DELAY);
  found = textContains(app->machine.text80Buffer(), app->machine.textRenderer().columns(), needle);
  xSemaphoreGive(app->mutex);
  return found;
}

static bool autotestWaitForText(AppState * app, char const * needle, int maxPolls)
{
  for (int i = 0; i < maxPolls; ++i) {
    if (autotestScreenContains(app, needle))
      return true;
    vTaskDelay(AutotestPollDelay);
  }
  return false;
}

static bool autotestWaitForAnyText(AppState * app, char const * const * needles, size_t needleCount, int maxPolls)
{
  for (int i = 0; i < maxPolls; ++i) {
    bool found = false;
    xSemaphoreTake(app->mutex, portMAX_DELAY);
    found = textContainsAny(app->machine.text80Buffer(), app->machine.textRenderer().columns(), needles, needleCount);
    xSemaphoreGive(app->mutex);
    if (found)
      return true;
    vTaskDelay(AutotestPollDelay);
  }
  return false;
}

static bool autotestWaitForDiskWrite(AppState * app, uint64_t previousCount, int maxPolls)
{
  for (int i = 0; i < maxPolls; ++i) {
    xSemaphoreTake(app->mutex, portMAX_DELAY);
    uint64_t const count = app->machine.diagnostics().diskWriteCount;
    xSemaphoreGive(app->mutex);
    if (count > previousCount)
      return true;
    vTaskDelay(AutotestPollDelay);
  }
  return false;
}

static uint64_t autotestDiskWriteCount(AppState * app)
{
  xSemaphoreTake(app->mutex, portMAX_DELAY);
  uint64_t const count = app->machine.diagnostics().diskWriteCount;
  xSemaphoreGive(app->mutex);
  return count;
}

static bool autotestInjectChar(AppState * app, char ch)
{
  HidKey key = asciiToHid(static_cast<uint8_t>(ch));
  if (key.usage == 0) {
    ESP_LOGE(TAG, "autotest unsupported key: 0x%02x", static_cast<unsigned>(static_cast<uint8_t>(ch)));
    return false;
  }

  uint8_t report[6] = {key.usage, 0, 0, 0, 0, 0};
  uint8_t empty[6] = {};
  xSemaphoreTake(app->mutex, portMAX_DELAY);
  app->machine.keyboard().onHidBootKeyboardReport(key.modifier, report);
  app->machine.keyboard().onHidBootKeyboardReport(0, empty);
  xSemaphoreGive(app->mutex);
  vTaskDelay(AutotestKeyDelay);
  return true;
}

static bool autotestType(AppState * app, char const * text)
{
  ESP_LOGI(TAG, "AUTOTEST typing: %s", text);
  app->runtimeLog.line("AUTOTEST typing: %s", text);
  for (char const * p = text; p && *p; ++p) {
    if (!autotestInjectChar(app, *p))
      return false;
  }
  return true;
}

static void autotestTask(void * arg)
{
  auto * app = static_cast<AppState *>(arg);
  ESP_LOGI(TAG, "AUTOTEST key script enabled");
  app->runtimeLog.line("AUTOTEST key script enabled");

  vTaskDelay(AutotestInitialDelay);
  autotestType(app, "\r");

  static char const * const PromptNeedles[] = { "A:\\>", "C:\\>" };
  if (!autotestWaitForAnyText(app, PromptNeedles, sizeof(PromptNeedles) / sizeof(PromptNeedles[0]), AutotestPromptPolls)) {
    ESP_LOGE(TAG, "AUTOTEST timeout waiting for DOS prompt");
    app->runtimeLog.line("AUTOTEST timeout waiting for DOS prompt");
    vTaskDelete(nullptr);
    return;
  }

  autotestType(app, "dir\r");
  static char const * const DirNeedles[] = { "Directory of A:", "Directory of C:" };
  if (!autotestWaitForAnyText(app, DirNeedles, sizeof(DirNeedles) / sizeof(DirNeedles[0]), AutotestWritePolls)) {
    ESP_LOGE(TAG, "AUTOTEST timeout waiting for DIR listing");
    app->runtimeLog.line("AUTOTEST timeout waiting for DIR listing");
  }
  uint64_t const beforeWrite = autotestDiskWriteCount(app);
  autotestType(app, "echo X>W.TXT\r");
  if (!autotestWaitForDiskWrite(app, beforeWrite, AutotestWritePolls)) {
    ESP_LOGE(TAG, "AUTOTEST timeout waiting for disk write");
    app->runtimeLog.line("AUTOTEST timeout waiting for disk write");
  }
  vTaskDelay(AutotestPostWriteDelay);
  autotestType(app, "dir W.TXT\r");
  if (!autotestWaitForText(app, "W        TXT", AutotestWritePolls)) {
    ESP_LOGE(TAG, "AUTOTEST timeout waiting for DOS-created file listing");
    app->runtimeLog.line("AUTOTEST timeout waiting for DOS-created file listing");
  }
  ESP_LOGI(TAG, "AUTOTEST key script finished");
  app->runtimeLog.line("AUTOTEST key script finished");
  vTaskDelete(nullptr);
}
#endif

} // namespace

extern "C" void app_main(void)
{
  ESP_LOGI(TAG, "Starting TabDOS for M5Stack Tab5");

  s_app.mutex = xSemaphoreCreateMutex();
  if (!s_app.mutex) {
    ESP_LOGE(TAG, "mutex allocation failed");
    return;
  }

  ESP_ERROR_CHECK(s_app.display.init());
  s_app.display.showStatus("TabDOS", "Mounting /sdcard/dos...");

  esp_err_t err = mountSdCard();
  if (err != ESP_OK) {
    s_app.display.showStatus("SD mount failed", esp_err_to_name(err));
    return;
  }

  s_app.runtimeLog.open();

  std::string imagePath;
  err = bootDefaultImage(s_app, &imagePath);
  if (err != ESP_OK) {
    s_app.runtimeLog.line("No bootable image in /sdcard/dos");
    s_app.display.showStatus("No bootable image", "/sdcard/dos/*.img");
    return;
  }

  s_app.display.showStatus("Booting DOS image", imagePath.c_str());
  ESP_ERROR_CHECK(s_app.usbKeyboard.start(&s_app.machine.keyboard(), s_app.mutex, [](char const * message) {
    s_app.runtimeLog.line("%s", message);
  }));
  bsp_touch_config_t touchConfig = {};
  err = bsp_touch_new(&touchConfig, &s_app.touch);
  if (err == ESP_OK) {
    s_app.machine.setMouseInstalled(true);
    ESP_LOGI(TAG, "Tab5 touch mapped to DOS INT 33h mouse");
    s_app.runtimeLog.line("Tab5 touch mapped to DOS INT 33h mouse");
  } else {
    ESP_LOGW(TAG, "touch init failed: %s; DOS mouse remains unavailable", esp_err_to_name(err));
    s_app.runtimeLog.line("touch init failed: %s; DOS mouse remains unavailable", esp_err_to_name(err));
  }

  xTaskCreatePinnedToCore(cpuTask, "pc_cpu", 8192, &s_app, 6, nullptr, 1);
  xTaskCreatePinnedToCore(displayTask, "pc_lcd", 4096, &s_app, 4, nullptr, 0);
  if (s_app.touch)
    xTaskCreatePinnedToCore(touchMouseTask, "pc_touch_mouse", 4096, &s_app, 3, nullptr, 0);
#if TABDOS_AUTOTEST_KEYS
  xTaskCreatePinnedToCore(autotestTask, "pc_autotest", 4096, &s_app, 3, nullptr, 0);
#endif
}
