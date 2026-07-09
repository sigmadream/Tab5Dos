#pragma once

#include <stddef.h>
#include <stdint.h>

namespace tabdos {

class PcTextRenderer {
public:
  struct Font8x16 {
    uint8_t const * data;
    size_t glyphCount;
  };

  static constexpr int Columns = 80;
  static constexpr int Rows = 25;
  static constexpr int MaxRows = 50;
  static constexpr int CellWidth = 8;
  static constexpr int CellWidth9 = 9;
  static constexpr int CellHeight = 16;
  static constexpr int Width = Columns * CellWidth;
  static constexpr int Width9 = Columns * CellWidth9;
  static constexpr int Height = Rows * CellHeight;
  static constexpr int TextBufferBytes = Columns * MaxRows * 2;

  PcTextRenderer();

  void setFont(Font8x16 font);
  void setVgaCharacterMap(uint8_t const * plane2, uint8_t characterMapSelect);
  void setCursor(int row, int column, bool visible);
  void setCursorShape(uint8_t start, uint8_t end);
  void setColumns(int columns);
  void setCellHeight(int cellHeight);
  void setRows(int rows);
  void setUnderlineLocation(uint8_t location) { m_underlineLocation = location & 0x1f; }
  void setBlinkEnabled(bool enabled) { m_blinkEnabled = enabled; }
  void setDisplayEnabled(bool enabled) { m_displayEnabled = enabled; }
  void setLineGraphicsEnabled(bool enabled) { m_lineGraphicsEnabled = enabled; }
  void setNineDotTextMode(bool enabled) { m_nineDotTextMode = enabled; }
  void setHorizontalPanning(uint8_t pixels) { m_horizontalPanning = pixels & 0x0f; }
  void setFrameCounter(uint32_t value) { m_frameCounter = value; }
  void setColorRgb565(uint8_t index, uint16_t color);
  void setOverscanColorRgb565(uint16_t color) { m_overscanColor = color; }
  int columns() const { return m_columns; }
  int cellHeight() const { return m_cellHeight; }
  int rows() const { return m_rows; }
  // Hash of all state that affects rendered output, so the display layer can
  // detect "nothing changed" from the inputs (text buffer + this) instead of
  // re-rendering the frame and hashing the pixels.
  uint64_t stateHash() const;
  uint8_t sampleColorIndex(uint8_t const * textBuffer, int x, int y) const;

  void renderLine(uint8_t const * textBuffer, int y, uint16_t * dest) const;
  void renderLine9Dot(uint8_t const * textBuffer, int y, uint16_t * dest) const;
  void renderFrame(uint8_t const * textBuffer, uint16_t * dest, int destPitchPixels) const;
  void renderFrame9Dot(uint8_t const * textBuffer, uint16_t * dest, int destPitchPixels) const;

  static uint16_t cgaColorRgb565(uint8_t colorIndex);
  static Font8x16 defaultFont();

private:
  uint8_t effectiveHorizontalPanning(bool nineDotOutput) const;
  uint8_t glyphBits(uint8_t character, uint8_t rawAttribute, int glyphRow, int sourceGlyphRow) const;
  bool shouldRepeatNinthDot(uint8_t character, uint8_t bits) const;
  static uint8_t characterMapIfAttributeClear(uint8_t characterMapSelect);
  static uint8_t characterMapIfAttributeSet(uint8_t characterMapSelect);
  static uint16_t characterMapOffset(uint8_t map);

  Font8x16 m_font;
  uint8_t const * m_vgaFontPlane;
  uint8_t m_vgaCharacterMapClear;
  uint8_t m_vgaCharacterMapSet;
  uint8_t m_underlineLocation;
  bool m_vgaFontSelectionEnabled;
  int m_columns;
  int m_cellHeight;
  int m_rows;
  int m_cursorRow;
  int m_cursorColumn;
  uint8_t m_cursorStart;
  uint8_t m_cursorEnd;
  bool m_cursorVisible;
  bool m_blinkEnabled;
  bool m_displayEnabled;
  bool m_lineGraphicsEnabled;
  bool m_nineDotTextMode;
  uint8_t m_horizontalPanning;
  uint32_t m_frameCounter;
  uint16_t m_palette[16];
  uint16_t m_overscanColor;
};

} // namespace tabdos
