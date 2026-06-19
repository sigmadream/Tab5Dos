#include "pc_text_renderer.h"

#include <string.h>

namespace tabdos {

extern const uint8_t PcFont8x16Data[4096];

PcTextRenderer::PcTextRenderer()
  : m_font(defaultFont()),
    m_vgaFontPlane(nullptr),
    m_vgaCharacterMapClear(0),
    m_vgaCharacterMapSet(0),
    m_underlineLocation(0x1f),
    m_vgaFontSelectionEnabled(false),
    m_columns(Columns),
    m_cellHeight(CellHeight),
    m_rows(Rows),
    m_cursorRow(0),
    m_cursorColumn(0),
    m_cursorStart(CellHeight - 3),
    m_cursorEnd(CellHeight - 1),
    m_cursorVisible(false),
    m_blinkEnabled(true),
    m_displayEnabled(true),
    m_lineGraphicsEnabled(true),
    m_nineDotTextMode(true),
    m_horizontalPanning(8),
    m_frameCounter(0),
    m_overscanColor(cgaColorRgb565(0))
{
  for (int i = 0; i < 16; ++i)
    m_palette[i] = cgaColorRgb565(static_cast<uint8_t>(i));
}

void PcTextRenderer::setFont(Font8x16 font)
{
  if (font.data && font.glyphCount > 0) {
    m_font = font;
    m_vgaFontSelectionEnabled = false;
  }
}

uint8_t PcTextRenderer::characterMapIfAttributeClear(uint8_t characterMapSelect)
{
  return static_cast<uint8_t>((characterMapSelect & 0x03) | ((characterMapSelect >> 2) & 0x04));
}

uint8_t PcTextRenderer::characterMapIfAttributeSet(uint8_t characterMapSelect)
{
  return static_cast<uint8_t>(((characterMapSelect >> 2) & 0x03) | ((characterMapSelect >> 3) & 0x04));
}

uint16_t PcTextRenderer::characterMapOffset(uint8_t map)
{
  static constexpr uint16_t Offsets[8] = {
    0x0000, 0x4000, 0x8000, 0xc000, 0x2000, 0x6000, 0xa000, 0xe000,
  };
  return Offsets[map & 0x07];
}

void PcTextRenderer::setVgaCharacterMap(uint8_t const * plane2, uint8_t characterMapSelect)
{
  m_vgaFontPlane = plane2;
  m_vgaCharacterMapClear = characterMapIfAttributeClear(characterMapSelect);
  m_vgaCharacterMapSet = characterMapIfAttributeSet(characterMapSelect);
  m_vgaFontSelectionEnabled = m_vgaFontPlane && m_vgaCharacterMapClear != m_vgaCharacterMapSet;
}

uint8_t PcTextRenderer::glyphBits(uint8_t character, uint8_t rawAttribute, int glyphRow, int sourceGlyphRow) const
{
  if (m_vgaFontSelectionEnabled && glyphRow >= 0 && glyphRow < 32) {
    uint8_t const map = (rawAttribute & 0x08) ? m_vgaCharacterMapSet : m_vgaCharacterMapClear;
    uint32_t const offset = static_cast<uint32_t>(characterMapOffset(map)) +
                            static_cast<uint32_t>(character) * 32u +
                            static_cast<uint32_t>(glyphRow);
    return m_vgaFontPlane[offset & 0xffff];
  }
  if (character < m_font.glyphCount)
    return m_font.data[static_cast<size_t>(character) * CellHeight + sourceGlyphRow];
  return 0;
}

bool PcTextRenderer::shouldRepeatNinthDot(uint8_t character, uint8_t bits) const
{
  // VGA 9-dot text mode repeats the eighth glyph column for box/line
  // drawing characters C0h-DFh when Attribute Controller mode-control
  // bit 2 enables line-graphics expansion.  The eighth displayed column is
  // the low bit because glyph bits are emitted high-to-low.
  return m_nineDotTextMode &&
         m_lineGraphicsEnabled &&
         character >= 0xc0 &&
         character <= 0xdf &&
         (bits & 0x01) != 0;
}

void PcTextRenderer::setCursor(int row, int column, bool visible)
{
  m_cursorRow = row;
  m_cursorColumn = column;
  m_cursorVisible = visible;
}

void PcTextRenderer::setCursorShape(uint8_t start, uint8_t end)
{
  m_cursorStart = start;
  m_cursorEnd = end;
}

void PcTextRenderer::setColumns(int columns)
{
  if (columns != 40 && columns != 80)
    columns = Columns;
  m_columns = columns;
  if (m_cursorColumn >= m_columns)
    m_cursorColumn = m_columns - 1;
}

void PcTextRenderer::setCellHeight(int cellHeight)
{
  if (cellHeight != 8 && cellHeight != 14 && cellHeight != 16)
    cellHeight = CellHeight;
  m_cellHeight = cellHeight;
  setRows(Height / m_cellHeight);
}

void PcTextRenderer::setRows(int rows)
{
  if (rows < 1 || rows > MaxRows)
    rows = Rows;
  m_rows = rows;
  if (m_cursorRow >= m_rows)
    m_cursorRow = m_rows - 1;
}

void PcTextRenderer::setColorRgb565(uint8_t index, uint16_t color)
{
  m_palette[index & 0x0f] = color;
}

uint8_t PcTextRenderer::effectiveHorizontalPanning(bool nineDotOutput) const
{
  uint8_t const pan = m_horizontalPanning & 0x0f;
  if (nineDotOutput) {
    if (pan <= 7)
      return static_cast<uint8_t>(pan + 1);
    if (pan == 8)
      return 0;
  }
  return pan;
}

uint8_t PcTextRenderer::sampleColorIndex(uint8_t const * textBuffer, int x, int y) const
{
  if (!textBuffer || !m_displayEnabled || x < 0 || y < 0 || y >= Height)
    return 0;

  int const columns = m_columns == 40 ? 40 : Columns;
  int const pixelScale = Columns / columns;
  int const glyphCellWidth = m_nineDotTextMode ? CellWidth9 : CellWidth;
  int const cellWidth = glyphCellWidth * pixelScale;
  int const displayWidth = columns * cellWidth;

  x += effectiveHorizontalPanning(m_nineDotTextMode);
  if (x >= displayWidth)
    return 0;

  int const sourceColumn = x / cellWidth;
  if (sourceColumn < 0 || sourceColumn >= columns)
    return 0;

  int const textRow = y / m_cellHeight;
  int const glyphRow = y % m_cellHeight;
  if (textRow < 0 || textRow >= m_rows)
    return 0;

  uint8_t const * cell = textBuffer + (textRow * columns + sourceColumn) * 2;
  uint8_t const character = cell[0];
  uint8_t attribute = cell[1];
  uint8_t const rawAttribute = attribute;
  bool blink = false;
  if (m_blinkEnabled) {
    blink = (attribute & 0x80) != 0;
    attribute &= 0x7f;
  }

  uint8_t const fgIndex = m_vgaFontSelectionEnabled ? (attribute & 0x07) : (attribute & 0x0f);
  uint8_t const bgIndex = (attribute >> 4) & 0x0f;
  if (blink && ((m_frameCounter & 0x3f) >= 0x20))
    return bgIndex;

  uint8_t bits = glyphBits(character, rawAttribute, glyphRow, glyphRow * CellHeight / m_cellHeight);
  bool const underlineEligible = (rawAttribute & 0x01) != 0 && (rawAttribute & 0x76) == 0;
  if (underlineEligible && glyphRow == m_underlineLocation && glyphRow < m_cellHeight)
    bits = 0xff;

  bool const cursorDisabled = (m_cursorStart & 0x20) != 0;
  uint8_t const cursorStart = m_cursorStart & 0x1f;
  uint8_t const cursorEnd = m_cursorEnd & 0x1f;
  bool const cursorCell = m_cursorVisible && ((m_frameCounter & 0x1f) < 0x10) && !cursorDisabled &&
                          cursorStart <= cursorEnd && textRow == m_cursorRow && sourceColumn == m_cursorColumn &&
                          glyphRow >= cursorStart && glyphRow <= cursorEnd;
  if (cursorCell)
    bits = 0xff;

  int const cellX = (x % cellWidth) / pixelScale;
  if (cellX < CellWidth)
    return (bits & (0x80 >> cellX)) ? fgIndex : bgIndex;
  return (cursorCell || shouldRepeatNinthDot(character, bits)) ? fgIndex : bgIndex;
}

void PcTextRenderer::renderLine(uint8_t const * textBuffer, int y, uint16_t * dest) const
{
  if (!textBuffer || !dest || y < 0 || y >= Height)
    return;
  uint16_t * const lineStart = dest;

  if (!m_displayEnabled) {
    for (int x = 0; x < Width; ++x)
      lineStart[x] = 0;
    return;
  }

  int textRow = y / m_cellHeight;
  int glyphRow = y % m_cellHeight;
  if (textRow >= m_rows) {
    for (int x = 0; x < Width; ++x)
      lineStart[x] = m_palette[0];
    return;
  }
  int sourceGlyphRow = glyphRow * CellHeight / m_cellHeight;
  bool blinkPhaseOff = m_blinkEnabled && ((m_frameCounter & 0x3f) >= 0x20);
  bool cursorPhaseOn = m_cursorVisible && ((m_frameCounter & 0x1f) < 0x10);

  int const columns = m_columns == 40 ? 40 : Columns;
  int const pixelScale = Columns / columns;
  uint8_t const * cell = textBuffer + textRow * columns * 2;
  for (int column = 0; column < columns; ++column) {
    uint8_t character = *cell++;
    uint8_t attribute = *cell++;
    uint8_t const rawAttribute = attribute;
    bool blink = false;
    if (m_blinkEnabled) {
      blink = (attribute & 0x80) != 0;
      attribute &= 0x7f;
    }

    uint8_t fgIndex = m_vgaFontSelectionEnabled ? (attribute & 0x07) : (attribute & 0x0f);
    uint8_t bgIndex = (attribute >> 4) & 0x0f;
    uint16_t fg = (blink && blinkPhaseOff) ? m_palette[bgIndex] : m_palette[fgIndex];
    uint16_t bg = m_palette[bgIndex];

    uint8_t bits = glyphBits(character, rawAttribute, glyphRow, sourceGlyphRow);
    bool const underlineEligible = (rawAttribute & 0x01) != 0 && (rawAttribute & 0x76) == 0;
    if (underlineEligible && glyphRow == m_underlineLocation && glyphRow < m_cellHeight)
      bits = 0xff;

    bool const cursorDisabled = (m_cursorStart & 0x20) != 0;
    uint8_t const cursorStart = m_cursorStart & 0x1f;
    uint8_t const cursorEnd = m_cursorEnd & 0x1f;
    if (cursorPhaseOn && !cursorDisabled && cursorStart <= cursorEnd &&
        textRow == m_cursorRow && column == m_cursorColumn &&
        glyphRow >= cursorStart && glyphRow <= cursorEnd)
      bits = 0xff;

    for (int x = 0; x < CellWidth; ++x) {
      uint16_t const pixel = (bits & (0x80 >> x)) ? fg : bg;
      for (int repeat = 0; repeat < pixelScale; ++repeat)
        *dest++ = pixel;
    }
  }

  uint8_t const pan = effectiveHorizontalPanning(m_nineDotTextMode);
  if (pan != 0) {
    memmove(lineStart, lineStart + pan, static_cast<size_t>(Width - pan) * sizeof(uint16_t));
    for (int x = Width - pan; x < Width; ++x)
      lineStart[x] = m_overscanColor;
  }
}

void PcTextRenderer::renderLine9Dot(uint8_t const * textBuffer, int y, uint16_t * dest) const
{
  if (!textBuffer || !dest || y < 0 || y >= Height)
    return;
  uint16_t * const lineStart = dest;

  if (!m_displayEnabled) {
    for (int x = 0; x < Width9; ++x)
      lineStart[x] = 0;
    return;
  }

  int textRow = y / m_cellHeight;
  int glyphRow = y % m_cellHeight;
  if (textRow >= m_rows) {
    for (int x = 0; x < Width9; ++x)
      lineStart[x] = m_palette[0];
    return;
  }
  int sourceGlyphRow = glyphRow * CellHeight / m_cellHeight;
  bool blinkPhaseOff = m_blinkEnabled && ((m_frameCounter & 0x3f) >= 0x20);
  bool cursorPhaseOn = m_cursorVisible && ((m_frameCounter & 0x1f) < 0x10);

  int const columns = m_columns == 40 ? 40 : Columns;
  int const pixelScale = Columns / columns;
  int const cellWidth = CellWidth9 * pixelScale;
  uint8_t const * cell = textBuffer + textRow * columns * 2;
  for (int column = 0; column < columns; ++column) {
    uint8_t character = *cell++;
    uint8_t attribute = *cell++;
    uint8_t const rawAttribute = attribute;
    bool blink = false;
    if (m_blinkEnabled) {
      blink = (attribute & 0x80) != 0;
      attribute &= 0x7f;
    }

    uint8_t fgIndex = m_vgaFontSelectionEnabled ? (attribute & 0x07) : (attribute & 0x0f);
    uint8_t bgIndex = (attribute >> 4) & 0x0f;
    uint16_t fg = (blink && blinkPhaseOff) ? m_palette[bgIndex] : m_palette[fgIndex];
    uint16_t bg = m_palette[bgIndex];

    uint8_t bits = glyphBits(character, rawAttribute, glyphRow, sourceGlyphRow);
    bool const underlineEligible = (rawAttribute & 0x01) != 0 && (rawAttribute & 0x76) == 0;
    if (underlineEligible && glyphRow == m_underlineLocation && glyphRow < m_cellHeight)
      bits = 0xff;

    bool const cursorDisabled = (m_cursorStart & 0x20) != 0;
    uint8_t const cursorStart = m_cursorStart & 0x1f;
    uint8_t const cursorEnd = m_cursorEnd & 0x1f;
    bool const cursorCell = cursorPhaseOn && !cursorDisabled && cursorStart <= cursorEnd &&
                            textRow == m_cursorRow && column == m_cursorColumn &&
                            glyphRow >= cursorStart && glyphRow <= cursorEnd;
    if (cursorCell)
      bits = 0xff;

    for (int x = 0; x < CellWidth; ++x) {
      uint16_t const pixel = (bits & (0x80 >> x)) ? fg : bg;
      for (int repeat = 0; repeat < pixelScale; ++repeat)
        *dest++ = pixel;
    }
    uint16_t const ninthPixel = (cursorCell || shouldRepeatNinthDot(character, bits)) ? fg : bg;
    for (int repeat = 0; repeat < pixelScale; ++repeat)
      *dest++ = ninthPixel;
  }

  // If a future mode clamps columns unexpectedly, leave no stale pixels.
  int const written = columns * cellWidth;
  for (int x = written; x < Width9; ++x)
    lineStart[x] = m_palette[0];

  uint8_t const pan = effectiveHorizontalPanning(true);
  if (pan != 0) {
    memmove(lineStart, lineStart + pan, static_cast<size_t>(Width9 - pan) * sizeof(uint16_t));
    for (int x = Width9 - pan; x < Width9; ++x)
      lineStart[x] = m_overscanColor;
  }
}

void PcTextRenderer::renderFrame(uint8_t const * textBuffer, uint16_t * dest, int destPitchPixels) const
{
  if (!textBuffer || !dest || destPitchPixels < Width)
    return;

  for (int y = 0; y < Height; ++y)
    renderLine(textBuffer, y, dest + y * destPitchPixels);
}

void PcTextRenderer::renderFrame9Dot(uint8_t const * textBuffer, uint16_t * dest, int destPitchPixels) const
{
  if (!textBuffer || !dest || destPitchPixels < Width9)
    return;

  for (int y = 0; y < Height; ++y)
    renderLine9Dot(textBuffer, y, dest + y * destPitchPixels);
}

uint16_t PcTextRenderer::cgaColorRgb565(uint8_t colorIndex)
{
  static constexpr uint8_t Rgb[16][3] = {
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xaa}, {0x00, 0xaa, 0x00}, {0x00, 0xaa, 0xaa},
    {0xaa, 0x00, 0x00}, {0xaa, 0x00, 0xaa}, {0xaa, 0x55, 0x00}, {0xaa, 0xaa, 0xaa},
    {0x55, 0x55, 0x55}, {0x55, 0x55, 0xff}, {0x55, 0xff, 0x55}, {0x55, 0xff, 0xff},
    {0xff, 0x55, 0x55}, {0xff, 0x55, 0xff}, {0xff, 0xff, 0x55}, {0xff, 0xff, 0xff},
  };
  auto const & color = Rgb[colorIndex & 0x0f];
  return static_cast<uint16_t>(((color[0] & 0xf8) << 8) | ((color[1] & 0xfc) << 3) | (color[2] >> 3));
}

PcTextRenderer::Font8x16 PcTextRenderer::defaultFont()
{
  return { PcFont8x16Data, 256 };
}

} // namespace tabdos
