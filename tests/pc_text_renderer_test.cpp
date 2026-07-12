#include "pc/pc_text_renderer.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

using tabdos::PcTextRenderer;

int main()
{
  static uint8_t font[256 * PcTextRenderer::CellHeight] = {};
  font['A' * PcTextRenderer::CellHeight + 0] = 0x80; // leftmost pixel
  font['A' * PcTextRenderer::CellHeight + 1] = 0x01; // rightmost pixel

  PcTextRenderer renderer;
  renderer.setFont({font, 256});
  renderer.setBlinkEnabled(true);
  renderer.setFrameCounter(0);

  std::vector<uint8_t> text(PcTextRenderer::TextBufferBytes, 0);
  text[0] = 'A';
  text[1] = 0x1e; // yellow on blue

  uint16_t line[PcTextRenderer::Width] = {};
  renderer.renderLine(text.data(), 0, line);

  uint16_t yellow = PcTextRenderer::cgaColorRgb565(0x0e);
  uint16_t blue = PcTextRenderer::cgaColorRgb565(0x01);
  assert(line[0] == yellow);
  for (int x = 1; x < PcTextRenderer::CellWidth; ++x)
    assert(line[x] == blue);

  renderer.setColorRgb565(0x0e, 0xf800);
  renderer.renderLine(text.data(), 0, line);
  assert(line[0] == 0xf800);
  renderer.setColorRgb565(0x0e, yellow);

  renderer.setDisplayEnabled(false);
  renderer.renderLine(text.data(), 0, line);
  for (int x = 0; x < PcTextRenderer::Width; ++x)
    assert(line[x] == 0x0000);
  uint16_t disabledWideLine[PcTextRenderer::Width9] = {};
  renderer.renderLine9Dot(text.data(), 0, disabledWideLine);
  for (int x = 0; x < PcTextRenderer::Width9; ++x)
    assert(disabledWideLine[x] == 0x0000);
  renderer.setDisplayEnabled(true);

  renderer.renderLine(text.data(), 1, line);
  for (int x = 0; x < PcTextRenderer::CellWidth - 1; ++x)
    assert(line[x] == blue);
  assert(line[PcTextRenderer::CellWidth - 1] == yellow);

  renderer.setHorizontalPanning(1);
  renderer.setOverscanColorRgb565(0x07e0);
  renderer.renderLine(text.data(), 0, line);
  assert(line[0] == blue); // AC horizontal PEL panning shifts text pixels left
  assert(line[PcTextRenderer::Width - 1] == 0x07e0);
  renderer.setHorizontalPanning(8);
  renderer.setOverscanColorRgb565(PcTextRenderer::cgaColorRgb565(0x00));

  text[1] = 0x9e; // blinking yellow on blue
  renderer.setFrameCounter(0x20);
  renderer.renderLine(text.data(), 0, line);
  for (int x = 0; x < PcTextRenderer::CellWidth; ++x)
    assert(line[x] == blue);

  renderer.setBlinkEnabled(false);
  renderer.renderLine(text.data(), 0, line);
  uint16_t brightBlue = PcTextRenderer::cgaColorRgb565(0x09);
  assert(line[0] == yellow);
  for (int x = 1; x < PcTextRenderer::CellWidth; ++x)
    assert(line[x] == brightBlue);

  text[1] = 0x1e;
  renderer.setBlinkEnabled(true);
  renderer.setFrameCounter(0);
  renderer.setCursor(0, 0, true);
  renderer.renderLine(text.data(), 13, line);
  for (int x = 0; x < PcTextRenderer::CellWidth; ++x)
    assert(line[x] == yellow);
  text[0] = ' ';
  renderer.setCursorShape(2, 4);
  renderer.renderLine(text.data(), 1, line);
  assert(line[0] == blue);
  renderer.renderLine(text.data(), 2, line);
  for (int x = 0; x < PcTextRenderer::CellWidth; ++x)
    assert(line[x] == yellow);
  renderer.renderLine(text.data(), 5, line);
  assert(line[0] == blue);
  renderer.setCursorShape(0x20, 4);
  renderer.renderLine(text.data(), 2, line);
  assert(line[0] == blue);
  renderer.setCursorShape(13, 15);

  renderer.setColumns(40);
  assert(renderer.columns() == 40);
  renderer.setCursor(0, 0, false);
  memset(text.data(), 0, text.size());
  text[0] = 'A';
  text[1] = 0x1e;
  renderer.renderLine(text.data(), 0, line);
  assert(line[0] == yellow);
  assert(line[1] == yellow); // 40-column text doubles each glyph pixel horizontally
  for (int x = 2; x < PcTextRenderer::CellWidth * 2; ++x)
    assert(line[x] == blue);
  renderer.setColumns(80);
  assert(renderer.columns() == 80);
  renderer.setCursor(0, 0, false);

  memset(text.data(), 0, text.size());
  font[0xc4 * PcTextRenderer::CellHeight + 0] = 0x01; // rightmost/eighth column only
  font['B' * PcTextRenderer::CellHeight + 0] = 0x01;
  text[0] = 0xc4;
  text[1] = 0x1e;
  uint16_t wideLine[PcTextRenderer::Width9] = {};
  renderer.setLineGraphicsEnabled(false);
  renderer.renderLine9Dot(text.data(), 0, wideLine);
  assert(wideLine[7] == yellow);
  assert(wideLine[8] == blue);
  assert(renderer.sampleColorIndex(text.data(), 7, 0) == 0x0e);
  assert(renderer.sampleColorIndex(text.data(), 8, 0) == 0x01);
  renderer.setLineGraphicsEnabled(true);
  renderer.renderLine9Dot(text.data(), 0, wideLine);
  assert(wideLine[7] == yellow);
  assert(wideLine[8] == yellow);
  assert(renderer.sampleColorIndex(text.data(), 8, 0) == 0x0e);
  renderer.setHorizontalPanning(7);
  renderer.setOverscanColorRgb565(0x001f);
  renderer.renderLine9Dot(text.data(), 0, wideLine);
  assert(wideLine[0] == yellow); // 9-dot text raw pan 7 shifts to the repeated ninth dot
  assert(renderer.sampleColorIndex(text.data(), 0, 0) == 0x0e);
  assert(wideLine[PcTextRenderer::Width9 - 1] == 0x001f);
  renderer.setHorizontalPanning(8);
  renderer.renderLine9Dot(text.data(), 0, wideLine);
  assert(wideLine[0] == blue); // 9-dot text raw pan 8 means zero effective panning
  assert(wideLine[7] == yellow);
  assert(wideLine[8] == yellow);
  renderer.setHorizontalPanning(8);
  renderer.setOverscanColorRgb565(PcTextRenderer::cgaColorRgb565(0x00));
  text[0] = 'B';
  renderer.renderLine9Dot(text.data(), 0, wideLine);
  assert(wideLine[7] == yellow);
  assert(wideLine[8] == blue);
  renderer.setCursor(0, 0, true);
  renderer.setCursorShape(0, 0);
  renderer.renderLine9Dot(text.data(), 0, wideLine);
  for (int x = 0; x < PcTextRenderer::CellWidth9; ++x)
    assert(wideLine[x] == yellow);
  renderer.setCursorShape(13, 15);
  renderer.setLineGraphicsEnabled(false);
  renderer.setCursor(0, 0, true);

  std::vector<uint16_t> frame(PcTextRenderer::Width * PcTextRenderer::Height, 0);
  renderer.renderFrame(text.data(), frame.data(), PcTextRenderer::Width);
  assert(frame[13 * PcTextRenderer::Width] == yellow);

  printf("pc_text_renderer_test passed\n");
  return 0;
}
