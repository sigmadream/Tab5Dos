#pragma once

#include "pc/pc_text_renderer.h"

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#include <stdint.h>

class Tab5LcdText {
public:
  Tab5LcdText() = default;
  ~Tab5LcdText();

  Tab5LcdText(Tab5LcdText const &) = delete;
  Tab5LcdText & operator=(Tab5LcdText const &) = delete;

  esp_err_t init();
  esp_err_t showStatus(char const * line1, char const * line2 = nullptr);
  esp_err_t blitText80(tabdos::PcTextRenderer const & renderer, uint8_t const * text80Buffer);
  esp_err_t blitRgb565(uint16_t const * source, int sourceWidth, int sourceHeight);

private:
  static constexpr int LcdWidth = 720;
  static constexpr int LcdHeight = 1280;
  static constexpr int TextSourceWidth = tabdos::PcTextRenderer::Width9;
  static constexpr int TextSourceHeight = tabdos::PcTextRenderer::Height;
  static constexpr int HerculesSourceWidth = 720;
  static constexpr int HerculesSourceHeight = 348;
  static constexpr int VgaSourceWidth = 640;
  static constexpr int VgaSourceHeight = 480;
  static constexpr int TextOrHerculesPixels = TextSourceWidth * TextSourceHeight > HerculesSourceWidth * HerculesSourceHeight
                                              ? TextSourceWidth * TextSourceHeight
                                              : HerculesSourceWidth * HerculesSourceHeight;
  static constexpr int SourcePixels = TextOrHerculesPixels > VgaSourceWidth * VgaSourceHeight
                                      ? TextOrHerculesPixels
                                      : VgaSourceWidth * VgaSourceHeight;
  static constexpr int OutputWidth = LcdWidth;
  static constexpr int OutputHeight = LcdHeight;
  static constexpr int OutputX = 0;
  static constexpr int OutputY = 0;

  esp_err_t clear(uint16_t color);
  esp_err_t drawOutput();
  void drawFrame(uint16_t const * source, int sourceWidth, int sourceHeight);
  static void rotateScaleCounterClockwise(uint16_t const * source,
                                          int sourceWidth,
                                          int sourceHeight,
                                          uint16_t * dest);
  static void writeStatusLine(uint8_t * text, char const * line, int row, uint8_t attribute);

  esp_lcd_panel_handle_t m_panel = nullptr;
  uint16_t * m_sourceFrame = nullptr;
  uint16_t * m_frame = nullptr;
  uint8_t * m_statusText = nullptr;
};
