#include "tab5_lcd_text.h"

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <string.h>

namespace {
static char const * TAG = "tab5_lcd";
static constexpr int DrawRetries = 20;
static constexpr TickType_t DrawRetryDelay = pdMS_TO_TICKS(10);
}

Tab5LcdText::~Tab5LcdText()
{
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

  m_sourceFrame = static_cast<uint16_t *>(heap_caps_malloc(SourcePixels * sizeof(uint16_t),
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!m_sourceFrame) {
    m_sourceFrame = static_cast<uint16_t *>(heap_caps_malloc(SourcePixels * sizeof(uint16_t),
                                                            MALLOC_CAP_8BIT));
  }
  ESP_RETURN_ON_FALSE(m_sourceFrame, ESP_ERR_NO_MEM, TAG, "text80 source framebuffer allocation failed");

  m_frame = static_cast<uint16_t *>(heap_caps_malloc(OutputWidth * OutputHeight * sizeof(uint16_t),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!m_frame) {
    m_frame = static_cast<uint16_t *>(heap_caps_malloc(OutputWidth * OutputHeight * sizeof(uint16_t),
                                                       MALLOC_CAP_8BIT));
  }
  ESP_RETURN_ON_FALSE(m_frame, ESP_ERR_NO_MEM, TAG, "rotated LCD framebuffer allocation failed");

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
  return drawOutput();
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
  rotateScaleCounterClockwise(source, sourceWidth, sourceHeight, m_frame);
}

esp_err_t Tab5LcdText::blitText80(tabdos::PcTextRenderer const & renderer, uint8_t const * text80Buffer)
{
  ESP_RETURN_ON_FALSE(m_panel && m_sourceFrame && m_frame, ESP_ERR_INVALID_STATE, TAG, "display not initialized");
  ESP_RETURN_ON_FALSE(text80Buffer, ESP_ERR_INVALID_ARG, TAG, "missing text buffer");
  renderer.renderFrame9Dot(text80Buffer, m_sourceFrame, TextSourceWidth);
  drawFrame(m_sourceFrame, TextSourceWidth, TextSourceHeight);
  return drawOutput();
}

esp_err_t Tab5LcdText::blitRgb565(uint16_t const * source, int sourceWidth, int sourceHeight)
{
  ESP_RETURN_ON_FALSE(m_panel && m_frame, ESP_ERR_INVALID_STATE, TAG, "display not initialized");
  ESP_RETURN_ON_FALSE(source && sourceWidth > 0 && sourceHeight > 0, ESP_ERR_INVALID_ARG, TAG, "invalid RGB565 source");
  drawFrame(source, sourceWidth, sourceHeight);
  return drawOutput();
}
