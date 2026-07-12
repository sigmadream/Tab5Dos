#pragma once

#include "presented_frame_cache.h"
#include "pc/pc_mouse_cursor_overlay.h"
#include "pc/pc_text_renderer.h"

#include "driver/ppa.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#include <stddef.h>
#include <stdint.h>

class Tab5LcdText {
public:
  Tab5LcdText() = default;
  ~Tab5LcdText();

  Tab5LcdText(Tab5LcdText const &) = delete;
  Tab5LcdText & operator=(Tab5LcdText const &) = delete;

  esp_err_t init();
  esp_err_t showStatus(char const * line1, char const * line2 = nullptr);
  esp_err_t blitText80(tabdos::PcTextRenderer const & renderer,
                       uint8_t const * text80Buffer,
                       tabdos::MouseCursorOverlay const & cursor = {});
  esp_err_t blitRgb565(uint16_t const * source, int sourceWidth, int sourceHeight);

  // Composite the INT 33h pointer onto an RGB565 source frame in place, before
  // the rotate/scale to the panel. Safe to call with an invisible overlay.
  static void applyMouseCursor(uint16_t * buffer,
                               int width,
                               int height,
                               tabdos::MouseCursorOverlay const & cursor);

  // Count of frames actually rotated and pushed to the panel (skipped identical
  // frames are not counted). Used by the display task to report effective fps.
  uint32_t drawnFrameCount() const { return m_drawnFrames; }

  static constexpr size_t ScreenshotMaxPixels = 640 * 480;

  // Copy the last source frame while called from the display-owner task. The
  // returned snapshot can then be encoded by another task without blocking
  // presentation or racing the next frame.
  esp_err_t copySourceFrame(uint16_t * dest, size_t capacityPixels, int * width, int * height) const;
  static esp_err_t dumpRgb565Base64(uint16_t const * source, int width, int height);

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
  static uint64_t hashSource(uint16_t const * source, int width, int height);
  static void rotateScaleCounterClockwise(uint16_t const * source,
                                          int sourceWidth,
                                          int sourceHeight,
                                          uint16_t * dest);
  static void writeStatusLine(uint8_t * text, char const * line, int row, uint8_t attribute);

  esp_lcd_panel_handle_t m_panel = nullptr;
  ppa_client_handle_t m_ppa = nullptr; // hardware scale/rotate/mirror; null => software fallback
  uint16_t * m_sourceFrame = nullptr;
  uint16_t * m_frame = nullptr;
  uint8_t * m_statusText = nullptr;
  PresentedFrameCache m_presentedFrame;
  uint32_t m_drawnFrames = 0;
  int m_lastRotSourceW = -1; // last source size, to know when to reclear output margins
  int m_lastRotSourceH = -1;
  // Last PC source frame handed to drawFrame (text => m_sourceFrame, graphics =>
  // the caller's buffer). Both stay valid between frames, so a screenshot dump
  // can read them; the content is the pre-rotation original PC screen.
  uint16_t const * m_lastSource = nullptr;
  int m_lastSourceWidth = 0;
  int m_lastSourceHeight = 0;
};
