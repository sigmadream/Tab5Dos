#include "tab5_lcd_text.h"

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>

namespace {
static char const * TAG = "tab5_lcd";
static constexpr int DrawRetries = 20;
static constexpr TickType_t DrawRetryDelay = pdMS_TO_TICKS(10);
}

Tab5LcdText::~Tab5LcdText()
{
  if (m_ppa)
    ppa_unregister_client(m_ppa);
  if (m_sourceFrame)
    heap_caps_free(m_sourceFrame);
  if (m_frame)
    heap_caps_free(m_frame);
  if (m_statusText)
    heap_caps_free(m_statusText);
}

esp_err_t Tab5LcdText::init()
{
  bsp_display_config_t config = {};
  config.dsi_bus.phy_clk_src = static_cast<mipi_dsi_phy_clock_source_t>(0);
  config.dsi_bus.lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS;
  esp_lcd_panel_io_handle_t io = nullptr;

  ESP_RETURN_ON_ERROR(bsp_display_new(&config, &m_panel, &io), TAG, "display init failed");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(m_panel, true), TAG, "display on failed");
  esp_err_t backlight = bsp_display_brightness_set(80);
  if (backlight != ESP_OK)
    ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG, "backlight failed");

  // Cache-line aligned so the PPA (2D-DMA) can read the source and write the
  // rotated output directly; the driver handles the cache writeback/invalidate.
  constexpr size_t Align = 128;
  m_sourceFrame = static_cast<uint16_t *>(heap_caps_aligned_alloc(Align, SourcePixels * sizeof(uint16_t),
                                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  ESP_RETURN_ON_FALSE(m_sourceFrame, ESP_ERR_NO_MEM, TAG, "text80 source framebuffer allocation failed");

  m_frame = static_cast<uint16_t *>(heap_caps_aligned_alloc(Align, OutputWidth * OutputHeight * sizeof(uint16_t),
                                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  ESP_RETURN_ON_FALSE(m_frame, ESP_ERR_NO_MEM, TAG, "rotated LCD framebuffer allocation failed");

  // Register the Pixel Processing Accelerator for hardware rotate/scale. Falls
  // back to the software transform if unavailable.
  ppa_client_config_t ppaConfig = {};
  ppaConfig.oper_type = PPA_OPERATION_SRM;
  if (ppa_register_client(&ppaConfig, &m_ppa) != ESP_OK) {
    m_ppa = nullptr;
    ESP_LOGW(TAG, "PPA unavailable; using software rotate (slower)");
  } else {
    ESP_LOGI(TAG, "PPA hardware rotate/scale enabled");
  }

  m_statusText = static_cast<uint8_t *>(heap_caps_malloc(tabdos::PcTextRenderer::TextBufferBytes,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!m_statusText) {
    m_statusText = static_cast<uint8_t *>(heap_caps_malloc(tabdos::PcTextRenderer::TextBufferBytes,
                                                           MALLOC_CAP_8BIT));
  }
  ESP_RETURN_ON_FALSE(m_statusText, ESP_ERR_NO_MEM, TAG, "status text buffer allocation failed");

  int const textRotatedWidth = TextSourceHeight;
  int const textRotatedHeight = TextSourceWidth;
  int textContentWidth = LcdWidth;
  int textContentHeight = textRotatedHeight * textContentWidth / textRotatedWidth;
  if (textContentHeight > LcdHeight) {
    textContentHeight = LcdHeight;
    textContentWidth = textRotatedWidth * textContentHeight / textRotatedHeight;
  }
  ESP_LOGI(TAG,
           "text80 transform: %dx%d -> CCW90 scaled to %dx%d at (%d,%d)",
           TextSourceWidth,
           TextSourceHeight,
           textContentWidth,
           textContentHeight,
           (LcdWidth - textContentWidth) / 2,
           (LcdHeight - textContentHeight) / 2);
  ESP_LOGI(TAG, "hercules transform: %dx%d -> CCW90 aspect-fit LCD", HerculesSourceWidth, HerculesSourceHeight);
  ESP_LOGI(TAG, "vga transform: up to %dx%d -> CCW90 aspect-fit LCD", VgaSourceWidth, VgaSourceHeight);
  std::fill(m_frame, m_frame + OutputWidth * OutputHeight, 0x0000);
  return ESP_OK;
}

esp_err_t Tab5LcdText::clear(uint16_t color)
{
  ESP_RETURN_ON_FALSE(m_panel, ESP_ERR_INVALID_STATE, TAG, "display not initialized");
  std::fill(m_frame, m_frame + OutputWidth * OutputHeight, color);
  m_haveFrameHash = false;
  m_haveTextInputHash = false;
  return drawOutput();
}

uint64_t Tab5LcdText::hashSource(uint16_t const * source, int width, int height)
{
  // FNV-1a over the RGB565 source frame, seeded with the dimensions so a mode
  // change that happens to share pixel values still forces a redraw.
  uint64_t hash = 1469598103934665603ull;
  hash = (hash ^ static_cast<uint64_t>(width)) * 1099511628211ull;
  hash = (hash ^ static_cast<uint64_t>(height)) * 1099511628211ull;
  int const count = width * height;
  for (int i = 0; i < count; ++i)
    hash = (hash ^ source[i]) * 1099511628211ull;
  return hash;
}

bool Tab5LcdText::frameUnchanged(uint16_t const * source, int width, int height, uint64_t * hashOut)
{
  uint64_t const hash = hashSource(source, width, height);
  if (hashOut)
    *hashOut = hash;
  return m_haveFrameHash && hash == m_lastFrameHash;
}

void Tab5LcdText::commitFrameHash(uint64_t hash)
{
  m_lastFrameHash = hash;
  m_haveFrameHash = true;
}

esp_err_t Tab5LcdText::showStatus(char const * line1, char const * line2)
{
  ESP_RETURN_ON_FALSE(m_panel && m_sourceFrame && m_frame && m_statusText, ESP_ERR_INVALID_STATE, TAG, "display not initialized");
  for (int i = 0; i < tabdos::PcTextRenderer::Rows * tabdos::PcTextRenderer::Columns; ++i) {
    m_statusText[i * 2] = ' ';
    m_statusText[i * 2 + 1] = 0x17;
  }

  writeStatusLine(m_statusText, line1, 10, 0x1f);
  writeStatusLine(m_statusText, line2, 12, 0x1e);

  tabdos::PcTextRenderer renderer;
  renderer.setCursor(0, 0, false);
  renderer.renderFrame9Dot(m_statusText, m_sourceFrame, TextSourceWidth);
  drawFrame(m_sourceFrame, TextSourceWidth, TextSourceHeight);
  m_haveFrameHash = false;
  m_haveTextInputHash = false;
  return drawOutput();
}

void Tab5LcdText::writeStatusLine(uint8_t * text, char const * line, int row, uint8_t attribute)
{
  if (!text || !line || row < 0 || row >= tabdos::PcTextRenderer::Rows)
    return;

  int len = static_cast<int>(strlen(line));
  int col = std::max(0, (tabdos::PcTextRenderer::Columns - len) / 2);
  for (int i = 0; line[i] && col + i < tabdos::PcTextRenderer::Columns; ++i) {
    text[2 * (row * tabdos::PcTextRenderer::Columns + col + i)] = static_cast<uint8_t>(line[i]);
    text[2 * (row * tabdos::PcTextRenderer::Columns + col + i) + 1] = attribute;
  }
}

void Tab5LcdText::rotateScaleCounterClockwise(uint16_t const * source,
                                              int sourceWidth,
                                              int sourceHeight,
                                              uint16_t * dest)
{
  if (!source || !dest)
    return;

  std::fill(dest, dest + OutputWidth * OutputHeight, 0x0000);
  int const rotatedWidth = sourceHeight;
  int const rotatedHeight = sourceWidth;
  int contentWidth = OutputWidth;
  int contentHeight = rotatedHeight * contentWidth / rotatedWidth;
  if (contentHeight > OutputHeight) {
    contentHeight = OutputHeight;
    contentWidth = rotatedWidth * contentHeight / rotatedHeight;
  }
  int const contentX = (OutputWidth - contentWidth) / 2;
  int const contentY = (OutputHeight - contentHeight) / 2;

  for (int y = 0; y < contentHeight; ++y) {
    int const rotatedY = y * rotatedHeight / contentHeight;
    int const sourceX = sourceWidth - 1 - rotatedY;
    uint16_t * row = dest + (contentY + y) * OutputWidth + contentX;
    for (int x = 0; x < contentWidth; ++x) {
      int const rotatedX = x * rotatedWidth / contentWidth;
      int const sourceY = rotatedX;
      row[x] = source[sourceY * sourceWidth + sourceX];
    }
  }
}

esp_err_t Tab5LcdText::drawOutput()
{
  esp_err_t err = ESP_OK;
  for (int attempt = 0; attempt <= DrawRetries; ++attempt) {
    err = esp_lcd_panel_draw_bitmap(m_panel, OutputX, OutputY, OutputX + OutputWidth, OutputY + OutputHeight, m_frame);
    if (err == ESP_OK)
      return ESP_OK;
    if (err != ESP_ERR_INVALID_STATE)
      return err;
    vTaskDelay(DrawRetryDelay);
  }
  return err;
}

void Tab5LcdText::drawFrame(uint16_t const * source, int sourceWidth, int sourceHeight)
{
  if (!source || sourceWidth <= 0 || sourceHeight <= 0)
    return;

  // Remember the original PC source so a screenshot can dump it later.
  m_lastSource = source;
  m_lastSourceWidth = sourceWidth;
  m_lastSourceHeight = sourceHeight;

  if (!m_ppa) {
    rotateScaleCounterClockwise(source, sourceWidth, sourceHeight, m_frame);
    return;
  }

  // 90 deg CCW then aspect-fit into the portrait panel. The PPA scale factor has
  // 1/16 precision, so quantize the uniform scale down to 1/16 and size/center
  // the output block with the same integer math the driver uses (out_w =
  // scale*in_h, out_h = scale*in_w for a 90/270 rotation).
  int const rotatedWidth = sourceHeight;  // width after rotation
  int const rotatedHeight = sourceWidth;  // height after rotation
  int scale16 = 16 * OutputWidth / rotatedWidth;
  int const scale16h = 16 * OutputHeight / rotatedHeight;
  if (scale16h < scale16)
    scale16 = scale16h;
  if (scale16 < 1)
    scale16 = 1;
  int const outW = scale16 * sourceHeight / 16;
  int const outH = scale16 * sourceWidth / 16;
  int const contentX = (OutputWidth - outW) / 2;
  int const contentY = (OutputHeight - outH) / 2;

  // Clear the black margins only when the content rectangle changes size.
  if (sourceWidth != m_lastRotSourceW || sourceHeight != m_lastRotSourceH) {
    std::fill(m_frame, m_frame + OutputWidth * OutputHeight, 0x0000);
    m_lastRotSourceW = sourceWidth;
    m_lastRotSourceH = sourceHeight;
  }

  float const scale = static_cast<float>(scale16) / 16.0f;
  ppa_srm_oper_config_t op = {};
  op.in.buffer = source;
  op.in.pic_w = static_cast<uint32_t>(sourceWidth);
  op.in.pic_h = static_cast<uint32_t>(sourceHeight);
  op.in.block_w = static_cast<uint32_t>(sourceWidth);
  op.in.block_h = static_cast<uint32_t>(sourceHeight);
  op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  op.out.buffer = m_frame;
  op.out.buffer_size = OutputWidth * OutputHeight * sizeof(uint16_t);
  op.out.pic_w = OutputWidth;
  op.out.pic_h = OutputHeight;
  op.out.block_offset_x = static_cast<uint32_t>(contentX);
  op.out.block_offset_y = static_cast<uint32_t>(contentY);
  op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  op.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
  op.scale_x = scale;
  op.scale_y = scale;
  op.mode = PPA_TRANS_MODE_BLOCKING;

  if (ppa_do_scale_rotate_mirror(m_ppa, &op) != ESP_OK) {
    // Any PPA rejection (e.g. an unaligned external source) falls back to the
    // slower software transform so the frame is still shown.
    rotateScaleCounterClockwise(source, sourceWidth, sourceHeight, m_frame);
  }
}

void Tab5LcdText::applyMouseCursor(uint16_t * buffer,
                                  int width,
                                  int height,
                                  tabdos::PcMachine::MouseCursorOverlay const & cursor)
{
  if (!buffer || !cursor.visible || width <= 0 || height <= 0)
    return;

  if (cursor.textCell) {
    // Reverse-video block over the character cell (matches the driver default).
    for (int dy = 0; dy < cursor.cellHeight; ++dy) {
      int const py = cursor.y + dy;
      if (py < 0 || py >= height)
        continue;
      uint16_t * row = buffer + py * width;
      for (int dx = 0; dx < cursor.cellWidth; ++dx) {
        int const px = cursor.x + dx;
        if (px < 0 || px >= width)
          continue;
        row[px] = static_cast<uint16_t>(~row[px]);
      }
    }
    return;
  }

  // 16x16 AND/XOR mask arrow. screenMask AND background, then XOR cursorMask.
  for (int dy = 0; dy < 16; ++dy) {
    int const py = cursor.y + dy;
    if (py < 0 || py >= height)
      continue;
    uint16_t * row = buffer + py * width;
    uint16_t const screen = cursor.screenMask[dy];
    uint16_t const shape = cursor.cursorMask[dy];
    for (int dx = 0; dx < 16; ++dx) {
      int const px = cursor.x + dx;
      if (px < 0 || px >= width)
        continue;
      int const bit = 15 - dx;
      int const s = (screen >> bit) & 1;
      int const c = (shape >> bit) & 1;
      if (s && !c)
        continue; // transparent
      if (!s && !c)
        row[px] = 0x0000; // black
      else if (!s && c)
        row[px] = 0xffff; // white
      else
        row[px] = static_cast<uint16_t>(~row[px]); // invert background
    }
  }
}

esp_err_t Tab5LcdText::blitText80(tabdos::PcTextRenderer const & renderer,
                                  uint8_t const * text80Buffer,
                                  tabdos::PcMachine::MouseCursorOverlay const & cursor)
{
  ESP_RETURN_ON_FALSE(m_panel && m_sourceFrame && m_frame, ESP_ERR_INVALID_STATE, TAG, "display not initialized");
  ESP_RETURN_ON_FALSE(text80Buffer, ESP_ERR_INVALID_ARG, TAG, "missing text buffer");

  // Input-based dirty detection: hash the visible text plus the renderer state
  // and cursor overlay. If nothing that affects the output changed, skip the
  // whole render/rotate/present chain. Hashing ~4 KB of text is far cheaper than
  // rendering the 720x400 frame and hashing its ~576 KB of pixels every time.
  int const visibleBytes = renderer.columns() * renderer.rows() * 2;
  uint64_t inputHash = 1469598103934665603ull;
  for (int i = 0; i < visibleBytes; ++i)
    inputHash = (inputHash ^ text80Buffer[i]) * 1099511628211ull;
  inputHash ^= renderer.stateHash();
  uint8_t const * cursorBytes = reinterpret_cast<uint8_t const *>(&cursor);
  for (size_t i = 0; i < sizeof(cursor); ++i)
    inputHash = (inputHash ^ cursorBytes[i]) * 1099511628211ull;
  if (m_haveTextInputHash && inputHash == m_lastTextInputHash)
    return ESP_OK;

  renderer.renderFrame9Dot(text80Buffer, m_sourceFrame, TextSourceWidth);
  applyMouseCursor(m_sourceFrame, TextSourceWidth, TextSourceHeight, cursor);
  drawFrame(m_sourceFrame, TextSourceWidth, TextSourceHeight);
  esp_err_t const err = drawOutput();
  if (err == ESP_OK) {
    ++m_drawnFrames;
    m_lastTextInputHash = inputHash; // only skip identical inputs after a successful draw
    m_haveTextInputHash = true;
  }
  return err;
}

esp_err_t Tab5LcdText::blitRgb565(uint16_t const * source, int sourceWidth, int sourceHeight)
{
  ESP_RETURN_ON_FALSE(m_panel && m_frame, ESP_ERR_INVALID_STATE, TAG, "display not initialized");
  ESP_RETURN_ON_FALSE(source && sourceWidth > 0 && sourceHeight > 0, ESP_ERR_INVALID_ARG, TAG, "invalid RGB565 source");
  uint64_t hash = 0;
  if (frameUnchanged(source, sourceWidth, sourceHeight, &hash))
    return ESP_OK;
  drawFrame(source, sourceWidth, sourceHeight);
  esp_err_t const err = drawOutput();
  if (err == ESP_OK) {
    ++m_drawnFrames;
    commitFrameHash(hash); // only skip identical frames after a successful draw
  }
  return err;
}

namespace {
// Emit `len` bytes of `buf` as base64 to the serial console, 54 input bytes (72
// chars) per line. Each line carries an "SS:" prefix so a host filter can
// recover the payload even if other tasks interleave console log lines, and the
// loop yields periodically so the idle task and watchdogs are serviced while the
// data drains through the (slow) UART.
void emitScreenshotBase64(uint8_t const * buf, size_t len)
{
  static char const b64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  constexpr size_t BytesPerLine = 54;
  char line[3 + 72 + 2];
  size_t lineCount = 0;
  for (size_t i = 0; i < len; i += BytesPerLine) {
    size_t const chunk = len - i < BytesPerLine ? len - i : BytesPerLine;
    char * p = line;
    *p++ = 'S';
    *p++ = 'S';
    *p++ = ':';
    for (size_t j = 0; j < chunk; j += 3) {
      size_t const rem = chunk - j;
      uint32_t n = static_cast<uint32_t>(buf[i + j]) << 16;
      if (rem > 1)
        n |= static_cast<uint32_t>(buf[i + j + 1]) << 8;
      if (rem > 2)
        n |= static_cast<uint32_t>(buf[i + j + 2]);
      *p++ = b64[(n >> 18) & 0x3f];
      *p++ = b64[(n >> 12) & 0x3f];
      *p++ = rem > 1 ? b64[(n >> 6) & 0x3f] : '=';
      *p++ = rem > 2 ? b64[n & 0x3f] : '=';
    }
    *p++ = '\n';
    *p = '\0';
    fputs(line, stdout);
    if ((++lineCount & 0x3f) == 0)
      vTaskDelay(1);
  }
}
} // namespace

esp_err_t Tab5LcdText::dumpSourceFrameBase64() const
{
  if (!m_lastSource || m_lastSourceWidth <= 0 || m_lastSourceHeight <= 0)
    return ESP_ERR_INVALID_STATE;

  int const w = m_lastSourceWidth;
  int const h = m_lastSourceHeight;
  size_t const pixelCount = static_cast<size_t>(w) * h;
  size_t const rawBytes = pixelCount * sizeof(uint16_t);

  // Run-length encode the RGB565 pixels: DOS screens are mostly flat color, so
  // this typically shrinks the serial payload ~10x. Each run is {u16 count, u16
  // value} little-endian. Falls back to raw pixels if the scratch buffer can't
  // be allocated or the frame is too noisy for RLE to help.
  size_t const rleCap = pixelCount * 4; // worst case: every pixel its own run
  uint8_t * rle = static_cast<uint8_t *>(heap_caps_malloc(rleCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  size_t rleLen = 0;
  if (rle) {
    for (size_t i = 0; i < pixelCount;) {
      uint16_t const v = m_lastSource[i];
      size_t run = 1;
      while (i + run < pixelCount && m_lastSource[i + run] == v && run < 0xffff)
        ++run;
      rle[rleLen++] = static_cast<uint8_t>(run & 0xff);
      rle[rleLen++] = static_cast<uint8_t>((run >> 8) & 0xff);
      rle[rleLen++] = static_cast<uint8_t>(v & 0xff);
      rle[rleLen++] = static_cast<uint8_t>((v >> 8) & 0xff);
      i += run;
    }
  }

  bool const useRle = rle && rleLen < rawBytes;
  char const * const fmt = useRle ? "rgb565le-rle" : "rgb565le";
  uint8_t const * const payload = useRle ? rle : reinterpret_cast<uint8_t const *>(m_lastSource);
  size_t const payloadBytes = useRle ? rleLen : rawBytes;

  ESP_LOGI(TAG, "screenshot %dx%d fmt=%s payload=%u raw=%u bytes", w, h, fmt,
           static_cast<unsigned>(payloadBytes), static_cast<unsigned>(rawBytes));
  // bytes= is the base64-encoded payload size; raw= is the decoded RGB565 size
  // (w*h*2) so the host can validate the expanded frame.
  printf("\n=== TABDOS SCREENSHOT BEGIN w=%d h=%d fmt=%s bytes=%u raw=%u ===\n",
         w, h, fmt, static_cast<unsigned>(payloadBytes), static_cast<unsigned>(rawBytes));

  emitScreenshotBase64(payload, payloadBytes);

  printf("=== TABDOS SCREENSHOT END ===\n");
  fflush(stdout);
  if (rle)
    heap_caps_free(rle);
  ESP_LOGI(TAG, "screenshot dump complete");
  return ESP_OK;
}
