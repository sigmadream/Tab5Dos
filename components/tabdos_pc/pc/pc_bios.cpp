#include "pc_bios.h"

#include "pc_i8086.h"
#include "pc_machine.h"

#include <string.h>
#include <time.h>
#include <algorithm>
#include <chrono>
#include <vector>

namespace tabdos {

extern const uint8_t PcFont8x16Data[4096];

namespace {

constexpr uint16_t VideoStateSaveBlocks = 16; // 16 * 64 = 1024 bytes
constexpr size_t VideoStateSaveBytes = VideoStateSaveBlocks * 64;
constexpr size_t VideoStateDacOffset = 128;
constexpr size_t VideoStateDacBytes = 256 * 3;
constexpr size_t VideoStateDacCursorOffset = VideoStateDacOffset + VideoStateDacBytes;
constexpr size_t VideoStateDacCursorBytes = 8;
constexpr size_t VideoStateAttributeCursorOffset = VideoStateDacCursorOffset + VideoStateDacCursorBytes;
constexpr size_t VideoStateLatchOffset = VideoStateAttributeCursorOffset + 3;
constexpr size_t VideoStateLatchBytes = 4;
constexpr uint16_t VideoStateHardwareMask = 0x0001;
constexpr uint16_t VideoStateBiosDataMask = 0x0002;
constexpr uint16_t VideoStateDacMask = 0x0004;

constexpr uint8_t VideoStateMagic0 = 'T';
constexpr uint8_t VideoStateMagic1 = 'D';
constexpr uint8_t VideoStateMagic2 = 'V';
constexpr uint8_t VideoStateMagic3 = 'S';
constexpr uint8_t VideoStateVersion = 1;
constexpr uint32_t BiosTicksPerDay = 0x001800b0;
constexpr uint16_t BiosFont8x8Segment = 0xf000;
constexpr uint16_t BiosFont8x8Offset = 0xc800;
constexpr uint32_t BiosFont8x8Linear = (static_cast<uint32_t>(BiosFont8x8Segment) << 4) + BiosFont8x8Offset;
constexpr uint16_t BiosFont8x14Segment = 0xf000;
constexpr uint16_t BiosFont8x14Offset = 0xd000;
constexpr uint32_t BiosFont8x14Linear = (static_cast<uint32_t>(BiosFont8x14Segment) << 4) + BiosFont8x14Offset;
constexpr uint16_t BiosFont8x16Segment = 0xf000;
constexpr uint16_t BiosFont8x16Offset = 0xe000;
constexpr uint32_t BiosFont8x16Linear = (static_cast<uint32_t>(BiosFont8x16Segment) << 4) + BiosFont8x16Offset;
constexpr uint16_t BiosKeyboardIrqSegment = 0xf000;
constexpr uint16_t BiosKeyboardIrqOffset = 0xf100;
constexpr uint8_t BiosKeyboardInternalInterrupt = 0x79;
constexpr uint16_t BiosKeyboardServiceSegment = 0xf000;
constexpr uint16_t BiosKeyboardServiceOffset = 0xf120;
constexpr uint8_t BiosKeyboardServiceInterrupt = 0x7a;
constexpr uint16_t BiosTimerIrqSegment = 0xf000;
constexpr uint16_t BiosTimerIrqOffset = 0xf140;
constexpr uint8_t BiosTimerInternalInterrupt = 0x78;
constexpr uint16_t BiosUserTimerSegment = 0xf000;
constexpr uint16_t BiosUserTimerOffset = 0xf160;
constexpr uint16_t BiosVideoServiceSegment = 0xf000;
constexpr uint16_t BiosVideoServiceOffset = 0xf170;
constexpr uint8_t BiosVideoServiceInterrupt = 0x7b;
constexpr uint16_t VideoStaticTableSegment = 0xf000;
constexpr uint16_t VideoStaticTableOffset = 0xf180;
constexpr uint32_t VideoStaticTableLinear = (static_cast<uint32_t>(VideoStaticTableSegment) << 4) + VideoStaticTableOffset;

static constexpr uint8_t VideoStaticTable[] = {
  0xff,       // supported modes 00h-07h: text/CGA-compatible modes
  0xe0,       // supported modes 08h-0fh: 0dh, 0eh, 0fh
  0x0f,       // supported modes 10h-13h
  0x00, 0x00, 0x00, 0x00,
  0x0b,       // scan lines supported: 200, 350, and 480
  0x08, 0x08, // total/maximum character blocks in text mode
  0x6c, 0x04, // miscellaneous support flags
  0x00, 0x00,
  0x08,       // save pointer function flags
  0x00,
};

struct DisplayCombinationEntry {
  uint8_t inactive;
  uint8_t active;
};

static constexpr DisplayCombinationEntry DisplayCombinationTable[] = {
  {0x00, 0x00}, //  0: no displays
  {0x00, 0x01}, //  1: MDPA
  {0x00, 0x02}, //  2: CGA
  {0x02, 0x01}, //  3: MDPA + CGA
  {0x00, 0x04}, //  4: EGA color
  {0x04, 0x01}, //  5: EGA color + MDPA
  {0x00, 0x05}, //  6: EGA mono
  {0x02, 0x05}, //  7: EGA mono + CGA
  {0x00, 0x06}, //  8: PGC
  {0x01, 0x06}, //  9: PGC + MDPA
  {0x05, 0x06}, // 10: PGC + EGA mono
  {0x00, 0x08}, // 11: VGA with analog color display
  {0x01, 0x08}, // 12: VGA color + MDPA
  {0x00, 0x07}, // 13: VGA with analog monochrome display
  {0x02, 0x07}, // 14: VGA mono + CGA
  {0x02, 0x06}, // 15: VGA mono + PGC
};

static int displayCombinationIndex(uint8_t inactive, uint8_t active)
{
  for (size_t i = 0; i < sizeof(DisplayCombinationTable) / sizeof(DisplayCombinationTable[0]); ++i) {
    if (DisplayCombinationTable[i].inactive == inactive && DisplayCombinationTable[i].active == active)
      return static_cast<int>(i);
  }
  return -1;
}

static uint32_t linearAddress(uint16_t segment, uint16_t offset)
{
  return (static_cast<uint32_t>(segment) << 4) + offset;
}


static void writeMemory32(PcMachine & machine, uint32_t address, uint32_t value)
{
  machine.writeMemory16(address + 0, static_cast<uint16_t>(value & 0xffff));
  machine.writeMemory16(address + 2, static_cast<uint16_t>(value >> 16));
}

static void writeMemory64(PcMachine & machine, uint32_t address, uint32_t low, uint32_t high = 0)
{
  writeMemory32(machine, address + 0, low);
  writeMemory32(machine, address + 4, high);
}

static void writeE820Entry(PcMachine & machine, uint32_t address, uint32_t base, uint32_t length, uint32_t type)
{
  writeMemory64(machine, address + 0x00, base);
  writeMemory64(machine, address + 0x08, length);
  writeMemory32(machine, address + 0x10, type);
}

static uint64_t monotonicMilliseconds()
{
  using clock = std::chrono::steady_clock;
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

static uint64_t monotonicMicroseconds()
{
  using clock = std::chrono::steady_clock;
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(clock::now().time_since_epoch()).count());
}

static void writeIndexedRegister(PcMachine & machine, uint16_t indexPort, uint16_t dataPort, uint8_t index, uint8_t value)
{
  machine.writePort(indexPort, index);
  machine.writePort(dataPort, value);
}

static uint8_t readIndexedRegister(PcMachine & machine, uint16_t indexPort, uint16_t dataPort, uint8_t index)
{
  machine.writePort(indexPort, index);
  return machine.readPort(dataPort);
}

static void writeAttributeRegister(PcMachine & machine, uint8_t index, uint8_t value)
{
  PcMachine::VgaAttributeState const state = machine.vgaAttributeState();
  machine.readPort(0x03da); // reset VGA attribute controller flip-flop to index phase
  machine.writePort(0x03c0, index | 0x20); // keep PAS/video enabled while BIOS updates AC registers
  machine.writePort(0x03c0, value);
  machine.setVgaAttributeState(state);
}

static uint8_t readAttributeRegister(PcMachine & machine, uint8_t index)
{
  PcMachine::VgaAttributeState const state = machine.vgaAttributeState();
  machine.readPort(0x03da); // reset VGA attribute controller flip-flop to index phase
  machine.writePort(0x03c0, index | 0x20);
  uint8_t const value = machine.readPort(0x03c1);
  machine.setVgaAttributeState(state);
  return value;
}

static uint8_t grayScaleSum(uint8_t r, uint8_t g, uint8_t b)
{
  return static_cast<uint8_t>((30 * (r & 0x3f) + 59 * (g & 0x3f) + 11 * (b & 0x3f) + 50) / 100);
}

static uint8_t textRowsMinusOneForCellHeight(uint8_t cellHeight)
{
  if (cellHeight == 8)
    return 49; // 80x50 text in the fixed 400-line LCD source frame
  return 24;   // 8x14 and 8x16 retain 25 text rows
}

static uint32_t biosFontLinearForCellHeight(uint8_t cellHeight)
{
  switch (cellHeight) {
    case 8: return BiosFont8x8Linear;
    case 14: return BiosFont8x14Linear;
    case 16: return BiosFont8x16Linear;
    default: return BiosFont8x16Linear;
  }
}

static uint16_t biosFontOffsetForCellHeight(uint8_t cellHeight)
{
  switch (cellHeight) {
    case 8: return BiosFont8x8Offset;
    case 14: return BiosFont8x14Offset;
    case 16: return BiosFont8x16Offset;
    default: return BiosFont8x16Offset;
  }
}

static void writeBiosFontImage(PcMachine & machine, uint8_t cellHeight)
{
  if (cellHeight != 8 && cellHeight != 14 && cellHeight != 16)
    cellHeight = PcTextRenderer::CellHeight;
  uint32_t const fontLinear = biosFontLinearForCellHeight(cellHeight);
  if (!machine.isRamRangeValid(fontLinear, static_cast<size_t>(cellHeight) * 256))
    return;

  for (uint16_t ch = 0; ch < 256; ++ch) {
    for (uint8_t row = 0; row < cellHeight; ++row) {
      uint8_t const sourceRow = static_cast<uint8_t>(row * PcTextRenderer::CellHeight / cellHeight);
      machine.writeMemory8(fontLinear + static_cast<uint32_t>(ch) * cellHeight + row,
                           PcFont8x16Data[ch * PcTextRenderer::CellHeight + sourceRow]);
    }
  }
}

static void setTextCellHeight(PcMachine & machine, uint8_t cellHeight, uint8_t rowsMinusOne = 0xff)
{
  if (cellHeight != 8 && cellHeight != 14 && cellHeight != 16)
    cellHeight = PcTextRenderer::CellHeight;
  if (rowsMinusOne == 0xff)
    rowsMinusOne = textRowsMinusOneForCellHeight(cellHeight);
  machine.writeMemory8(0x0484, rowsMinusOne);
  machine.writeMemory16(0x0485, cellHeight);
  machine.textRenderer().setCellHeight(cellHeight);
  machine.textRenderer().setRows(static_cast<int>(rowsMinusOne) + 1);

  uint8_t const oldMaxScanLine = readIndexedRegister(machine, 0x03d4, 0x03d5, 0x09);
  writeIndexedRegister(machine,
                       0x03d4,
                       0x03d5,
                       0x09,
                       static_cast<uint8_t>((oldMaxScanLine & 0xe0) | ((cellHeight - 1) & 0x1f)));
  machine.writeMemory8(0x0484, rowsMinusOne);
  machine.writeMemory16(0x0485, cellHeight);
  machine.textRenderer().setCellHeight(cellHeight);
  machine.textRenderer().setRows(static_cast<int>(rowsMinusOne) + 1);
}

static void setGraphicsTextMetrics(PcMachine & machine, uint8_t cellHeight)
{
  if (cellHeight != 8 && cellHeight != 14 && cellHeight != 16)
    cellHeight = PcTextRenderer::CellHeight;
  int rows = cellHeight > 0 ? machine.graphicsHeight() / cellHeight : PcTextRenderer::Rows;
  if (rows < 1)
    rows = 1;
  if (rows > PcTextRenderer::MaxRows)
    rows = PcTextRenderer::MaxRows;
  machine.writeMemory8(0x0484, static_cast<uint8_t>(rows - 1));
  machine.writeMemory16(0x0485, cellHeight);
}

static uint8_t fontHeightForInfoBlock(PcMachine const & machine, uint8_t block)
{
  switch (block) {
    case 0x01:
    case 0x05:
      return 14;
    case 0x04:
    case 0x06:
      return 16;
    case 0x00:
    case 0x02:
    case 0x03:
      return 8;
    default:
    {
      uint16_t const current = machine.readMemory16(0x0485);
      return current == 8 || current == 14 || current == 16 ? static_cast<uint8_t>(current) : PcTextRenderer::CellHeight;
    }
  }
}

static uint8_t cgaModeControlForBiosMode(uint8_t mode)
{
  switch (mode & 0x7f) {
    case 0x00: return 0x2c; // 40x25 BW text: blink + video + BW/color-burst disable
    case 0x01: return 0x28; // 40x25 color text: blink + video
    case 0x02: return 0x2d; // 80x25 BW text
    case 0x03: return 0x29; // 80x25 color text
    case 0x04: return 0x0a; // 320x200 4-color graphics
    case 0x05: return 0x0e; // 320x200 gray/composite-suppressed graphics
    case 0x06: return 0x1a; // 640x200 2-color graphics
    default: return 0x29;
  }
}

static void grayScaleDacBlock(PcMachine & machine, uint8_t firstIndex, uint16_t count)
{
  PcMachine::VgaDacState const dacState = machine.vgaDacState();
  uint8_t index = firstIndex;
  for (uint16_t i = 0; i < count; ++i, ++index) {
    machine.writePort(0x03c7, index);
    uint8_t const r = machine.readPort(0x03c9);
    uint8_t const g = machine.readPort(0x03c9);
    uint8_t const b = machine.readPort(0x03c9);
    uint8_t const gray = grayScaleSum(r, g, b);
    machine.writePort(0x03c8, index);
    machine.writePort(0x03c9, gray);
    machine.writePort(0x03c9, gray);
    machine.writePort(0x03c9, gray);
  }
  machine.setVgaDacState(dacState);
}

static bool isVgaGraphicsMode(PcMachine::VideoMode mode)
{
  switch (mode) {
    case PcMachine::VideoMode::VgaGraphics320x200x16:
    case PcMachine::VideoMode::VgaGraphics640x200x16:
    case PcMachine::VideoMode::VgaGraphics640x350x2:
    case PcMachine::VideoMode::VgaGraphics640x350x16:
    case PcMachine::VideoMode::VgaGraphics640x480x2:
    case PcMachine::VideoMode::VgaGraphics640x480x16:
    case PcMachine::VideoMode::VgaGraphics320x200x256:
      return true;
    default:
      return false;
  }
}

static uint16_t vgaCrtcStartAddressForPageOffset(PcMachine & machine, uint16_t pageOffset)
{
  if (!isVgaGraphicsMode(machine.videoMode()))
    return pageOffset;

  uint8_t const underlineLocation = readIndexedRegister(machine, 0x03d4, 0x03d5, 0x14);
  uint8_t const crtcModeControl = readIndexedRegister(machine, 0x03d4, 0x03d5, 0x17);
  uint16_t const addressUnitBytes = (underlineLocation & 0x40) ? 4 : ((crtcModeControl & 0x40) ? 1 : 2);
  return static_cast<uint16_t>(pageOffset / addressUnitBytes);
}

static PcMachine::VideoMode videoModeFromBiosMode(uint8_t biosMode)
{
  switch (biosMode & 0x7f) {
    case 0x04:
    case 0x05: return PcMachine::VideoMode::CgaGraphics320x200x4;
    case 0x06: return PcMachine::VideoMode::CgaGraphics640x200x2;
    case 0x0d: return PcMachine::VideoMode::VgaGraphics320x200x16;
    case 0x0e: return PcMachine::VideoMode::VgaGraphics640x200x16;
    case 0x0f: return PcMachine::VideoMode::VgaGraphics640x350x2;
    case 0x10: return PcMachine::VideoMode::VgaGraphics640x350x16;
    case 0x11: return PcMachine::VideoMode::VgaGraphics640x480x2;
    case 0x12: return PcMachine::VideoMode::VgaGraphics640x480x16;
    case 0x13: return PcMachine::VideoMode::VgaGraphics320x200x256;
    case 0x03:
    case 0x07:
    default: return PcMachine::VideoMode::Text80;
  }
}

static uint16_t colorsForVideoMode(uint8_t biosMode)
{
  switch (biosMode & 0x7f) {
    case 0x04:
    case 0x05: return 4;
    case 0x06:
    case 0x0f:
    case 0x11: return 2;
    case 0x13: return 256;
    case 0x03:
    case 0x07:
    case 0x0d:
    case 0x0e:
    case 0x10:
    case 0x12:
    default: return 16;
  }
}

static uint8_t displayPagesForVideoMode(uint8_t biosMode, uint16_t pageSize)
{
  switch (biosMode & 0x7f) {
    case 0x03:
    case 0x07:
      return 8;
    case 0x0d:
      return 8;
    case 0x0e:
    case 0x0f:
    case 0x10:
      return pageSize ? static_cast<uint8_t>(0x10000 / pageSize) : 1;
    default:
      return 1;
  }
}

static uint8_t rasterCodeForVideoMode(uint8_t biosMode)
{
  switch (biosMode & 0x7f) {
    case 0x0f:
    case 0x10:
      return 1; // 350 scan lines
    case 0x11:
    case 0x12:
      return 3; // 480 scan lines
    default:
      return 0; // 200 text/CGA/VGA scan lines
  }
}

static bool usesVgaAttributeController(PcMachine::VideoMode mode)
{
  return mode == PcMachine::VideoMode::Text80 ||
         mode == PcMachine::VideoMode::VgaGraphics320x200x16 ||
         mode == PcMachine::VideoMode::VgaGraphics640x200x16 ||
         mode == PcMachine::VideoMode::VgaGraphics640x350x2 ||
         mode == PcMachine::VideoMode::VgaGraphics640x350x16 ||
         mode == PcMachine::VideoMode::VgaGraphics640x480x2 ||
         mode == PcMachine::VideoMode::VgaGraphics640x480x16 ||
         mode == PcMachine::VideoMode::VgaGraphics320x200x256;
}

static bool isCgaGraphicsMode(PcMachine::VideoMode mode)
{
  return mode == PcMachine::VideoMode::CgaGraphics320x200x4 ||
         mode == PcMachine::VideoMode::CgaGraphics640x200x2;
}

static int16_t saturatedMouseCounter(int value)
{
  if (value > 32767)
    return 32767;
  if (value < -32768)
    return -32768;
  return static_cast<int16_t>(value);
}

static uint16_t mousePressEventMask(int button)
{
  static uint16_t const masks[3] = {0x0002, 0x0008, 0x0020};
  return button >= 0 && button < 3 ? masks[button] : 0;
}

static uint16_t mouseReleaseEventMask(int button)
{
  static uint16_t const masks[3] = {0x0004, 0x0010, 0x0040};
  return button >= 0 && button < 3 ? masks[button] : 0;
}

} // namespace

bool PcBios::loadUserFont(PcMachine & machine,
                          uint8_t bytesPerCharacter,
                          uint16_t firstCharacter,
                          uint16_t characterCount,
                          uint32_t source,
                          bool updateTextRenderer)
{
  if (bytesPerCharacter == 0 || bytesPerCharacter > PcTextRenderer::CellHeight || firstCharacter >= 256 || characterCount == 0)
    return false;

  uint16_t const clampedCount = static_cast<uint16_t>(
    std::min<uint16_t>(characterCount, static_cast<uint16_t>(256 - firstCharacter)));
  size_t const sourceBytes = static_cast<size_t>(clampedCount) * bytesPerCharacter;
  if (!machine.isRamRangeValid(source, sourceBytes))
    return false;

  for (uint16_t i = 0; i < clampedCount; ++i) {
    uint8_t sourceGlyph[PcTextRenderer::CellHeight] = {};
    uint8_t rendererGlyph[PcTextRenderer::CellHeight] = {};
    if (!machine.readMemoryBlock(source + static_cast<uint32_t>(i) * bytesPerCharacter, sourceGlyph, bytesPerCharacter))
      return false;
    for (uint8_t row = 0; row < bytesPerCharacter; ++row) {
      uint8_t const rendererRow = static_cast<uint8_t>(row * PcTextRenderer::CellHeight / bytesPerCharacter);
      rendererGlyph[rendererRow] = sourceGlyph[row];
    }
    memcpy(m_userFont + static_cast<size_t>(firstCharacter + i) * PcTextRenderer::CellHeight,
           rendererGlyph,
           PcTextRenderer::CellHeight);
  }

  if (updateTextRenderer)
    machine.textRenderer().setFont({ m_userFont, 256 });
  return true;
}

void PcBios::loadRomGraphicsFont(PcMachine & machine, uint8_t cellHeight)
{
  if (cellHeight != 8 && cellHeight != 14 && cellHeight != 16)
    cellHeight = graphicsTextCellHeight(machine);
  for (uint16_t ch = 0; ch < 256; ++ch) {
    uint8_t * glyph = m_userFont + static_cast<size_t>(ch) * PcTextRenderer::CellHeight;
    memset(glyph, 0, PcTextRenderer::CellHeight);
    for (uint8_t row = 0; row < cellHeight; ++row) {
      uint8_t const sourceRow = static_cast<uint8_t>(row * PcTextRenderer::CellHeight / cellHeight);
      glyph[sourceRow] = PcFont8x16Data[ch * PcTextRenderer::CellHeight + sourceRow];
    }
  }
  setGraphicsTextMetrics(machine, cellHeight);
}

static uint8_t asciiForScancode(uint8_t scan, bool shifted)
{
  switch (scan) {
    case 0x02: return shifted ? '!' : '1';
    case 0x03: return shifted ? '@' : '2';
    case 0x04: return shifted ? '#' : '3';
    case 0x05: return shifted ? '$' : '4';
    case 0x06: return shifted ? '%' : '5';
    case 0x07: return shifted ? '^' : '6';
    case 0x08: return shifted ? '&' : '7';
    case 0x09: return shifted ? '*' : '8';
    case 0x0a: return shifted ? '(' : '9';
    case 0x0b: return shifted ? ')' : '0';
    case 0x0c: return shifted ? '_' : '-';
    case 0x0d: return shifted ? '+' : '=';
    case 0x01: return 0x1b;
    case 0x0e: return '\b';
    case 0x0f: return '\t';
    case 0x10: return shifted ? 'Q' : 'q';
    case 0x11: return shifted ? 'W' : 'w';
    case 0x12: return shifted ? 'E' : 'e';
    case 0x13: return shifted ? 'R' : 'r';
    case 0x14: return shifted ? 'T' : 't';
    case 0x15: return shifted ? 'Y' : 'y';
    case 0x16: return shifted ? 'U' : 'u';
    case 0x17: return shifted ? 'I' : 'i';
    case 0x18: return shifted ? 'O' : 'o';
    case 0x19: return shifted ? 'P' : 'p';
    case 0x1a: return shifted ? '{' : '[';
    case 0x1b: return shifted ? '}' : ']';
    case 0x1c: return '\r';
    case 0x1e: return shifted ? 'A' : 'a';
    case 0x1f: return shifted ? 'S' : 's';
    case 0x20: return shifted ? 'D' : 'd';
    case 0x21: return shifted ? 'F' : 'f';
    case 0x22: return shifted ? 'G' : 'g';
    case 0x23: return shifted ? 'H' : 'h';
    case 0x24: return shifted ? 'J' : 'j';
    case 0x25: return shifted ? 'K' : 'k';
    case 0x26: return shifted ? 'L' : 'l';
    case 0x27: return shifted ? ':' : ';';
    case 0x28: return shifted ? '"' : '\'';
    case 0x29: return shifted ? '~' : '`';
    case 0x2b: return shifted ? '|' : '\\';
    case 0x2c: return shifted ? 'Z' : 'z';
    case 0x2d: return shifted ? 'X' : 'x';
    case 0x2e: return shifted ? 'C' : 'c';
    case 0x2f: return shifted ? 'V' : 'v';
    case 0x30: return shifted ? 'B' : 'b';
    case 0x31: return shifted ? 'N' : 'n';
    case 0x32: return shifted ? 'M' : 'm';
    case 0x33: return shifted ? '<' : ',';
    case 0x34: return shifted ? '>' : '.';
    case 0x35: return shifted ? '?' : '/';
    case 0x39: return ' ';
    default: return 0;
  }
}

void PcBios::reset()
{
  m_cursorRow = 0;
  m_cursorColumn = 0;
  m_activeDisplayPage = 0;
  m_leftShift = false;
  m_rightShift = false;
  m_ctrl = false;
  m_alt = false;
  m_scrollLock = false;
  m_numLock = false;
  m_capsLock = false;
  m_insert = false;
  m_dacColorPageMode = 0;
  m_defaultPaletteLoadingEnabled = true;
  m_grayScaleSummingEnabled = false;
  m_cursorEmulationEnabled = true;
  memcpy(m_userFont, PcFont8x16Data, sizeof(m_userFont));
  m_mouseX = 0;
  m_mouseY = 0;
  m_mouseMinX = 0;
  m_mouseMaxX = 639;
  m_mouseMinY = 0;
  m_mouseMaxY = 199;
  m_mouseVisibleCount = 0;
  m_mouseButtons = 0;
  clearMouseEventState();
  loadDefaultGraphicsCursor();
  m_mouseExcludeActive = false;
  m_mouseInstalled = false;
  m_shadowKeyPending = false;
  m_shadowKey = 0;
  m_shadowTailAtObserve = 0;
  for (int i = 0; i < WaitSlots; ++i)
    m_waits[i] = BiosWait{};
  m_timerBaseTicks = 0;
  m_timerBaseMillis = monotonicMilliseconds();
}

void PcBios::loadDefaultGraphicsCursor()
{
  // Standard Microsoft-compatible arrow pointer (hotspot at the top-left tip).
  // screenMask is ANDed with the background, cursorMask is XORed afterwards.
  static const uint16_t kArrowScreenMask[16] = {
    0x3fff, 0x1fff, 0x0fff, 0x07ff, 0x03ff, 0x01ff, 0x00ff, 0x007f,
    0x003f, 0x001f, 0x01ff, 0x10ff, 0x30ff, 0xf87f, 0xf87f, 0xfc7f,
  };
  static const uint16_t kArrowCursorMask[16] = {
    0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7c00, 0x7e00, 0x7f00,
    0x7f80, 0x7c00, 0x6c00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000,
  };
  memcpy(m_mouseScreenMask, kArrowScreenMask, sizeof(m_mouseScreenMask));
  memcpy(m_mouseCursorMask, kArrowCursorMask, sizeof(m_mouseCursorMask));
  m_mouseHotspotX = 0;
  m_mouseHotspotY = 0;
  m_mouseGraphicsCursorDefined = false;
}

PcBios::MouseRenderInfo PcBios::mouseRenderInfo() const
{
  MouseRenderInfo info = {};
  info.visible = m_mouseInstalled && m_mouseVisibleCount > 0;
  info.x = m_mouseX;
  info.y = m_mouseY;
  info.minX = m_mouseMinX;
  info.maxX = m_mouseMaxX;
  info.minY = m_mouseMinY;
  info.maxY = m_mouseMaxY;
  info.graphicsCursorDefined = m_mouseGraphicsCursorDefined;
  info.hotspotX = m_mouseHotspotX;
  info.hotspotY = m_mouseHotspotY;
  memcpy(info.screenMask, m_mouseScreenMask, sizeof(info.screenMask));
  memcpy(info.cursorMask, m_mouseCursorMask, sizeof(info.cursorMask));
  info.excludeActive = m_mouseExcludeActive;
  info.excludeLeft = m_mouseExcludeLeft;
  info.excludeTop = m_mouseExcludeTop;
  info.excludeRight = m_mouseExcludeRight;
  info.excludeBottom = m_mouseExcludeBottom;
  return info;
}

void PcBios::setMouseInstalled(bool installed)
{
  m_mouseInstalled = installed;
  if (!installed) {
    m_mouseButtons = 0;
    clearMouseEventState();
  }
}

void PcBios::clampMousePosition()
{
  if (m_mouseMinX > m_mouseMaxX)
    m_mouseMinX = m_mouseMaxX;
  if (m_mouseMinY > m_mouseMaxY)
    m_mouseMinY = m_mouseMaxY;
  if (m_mouseX < m_mouseMinX)
    m_mouseX = m_mouseMinX;
  if (m_mouseX > m_mouseMaxX)
    m_mouseX = m_mouseMaxX;
  if (m_mouseY < m_mouseMinY)
    m_mouseY = m_mouseMinY;
  if (m_mouseY > m_mouseMaxY)
    m_mouseY = m_mouseMaxY;
}

void PcBios::clearMouseEventState()
{
  for (int i = 0; i < 3; ++i) {
    m_mousePressCount[i] = 0;
    m_mouseReleaseCount[i] = 0;
    m_mousePressX[i] = m_mouseX;
    m_mousePressY[i] = m_mouseY;
    m_mouseReleaseX[i] = m_mouseX;
    m_mouseReleaseY[i] = m_mouseY;
  }
  m_mouseMotionX = 0;
  m_mouseMotionY = 0;
  m_mouseCallbackMask = 0;
  m_mouseCallbackSegment = 0;
  m_mouseCallbackOffset = 0;
  m_mousePendingCallbackMask = 0;
  m_mousePendingCallbackMotionX = 0;
  m_mousePendingCallbackMotionY = 0;
}

void PcBios::updateMouseState(uint16_t x, uint16_t y, uint16_t buttons, bool recordMotion)
{
  uint16_t const previousX = m_mouseX;
  uint16_t const previousY = m_mouseY;
  uint16_t const previousButtons = m_mouseButtons;

  m_mouseX = x;
  m_mouseY = y;
  clampMousePosition();
  m_mouseButtons = buttons & 0x0007;

  int16_t motionX = 0;
  int16_t motionY = 0;
  if (recordMotion) {
    motionX = saturatedMouseCounter(static_cast<int>(m_mouseX) - static_cast<int>(previousX));
    motionY = saturatedMouseCounter(static_cast<int>(m_mouseY) - static_cast<int>(previousY));
    m_mouseMotionX = saturatedMouseCounter(static_cast<int>(m_mouseMotionX) + motionX);
    m_mouseMotionY = saturatedMouseCounter(static_cast<int>(m_mouseMotionY) + motionY);
    if (motionX != 0 || motionY != 0) {
      m_mousePendingCallbackMask |= 0x0001;
      m_mousePendingCallbackMotionX = saturatedMouseCounter(static_cast<int>(m_mousePendingCallbackMotionX) + motionX);
      m_mousePendingCallbackMotionY = saturatedMouseCounter(static_cast<int>(m_mousePendingCallbackMotionY) + motionY);
    }
  }

  for (int i = 0; i < 3; ++i) {
    uint16_t const mask = static_cast<uint16_t>(1u << i);
    if ((previousButtons & mask) == 0 && (m_mouseButtons & mask) != 0) {
      if (m_mousePressCount[i] != 0xffff)
        ++m_mousePressCount[i];
      m_mousePressX[i] = m_mouseX;
      m_mousePressY[i] = m_mouseY;
      m_mousePendingCallbackMask |= mousePressEventMask(i);
    }
    if ((previousButtons & mask) != 0 && (m_mouseButtons & mask) == 0) {
      if (m_mouseReleaseCount[i] != 0xffff)
        ++m_mouseReleaseCount[i];
      m_mouseReleaseX[i] = m_mouseX;
      m_mouseReleaseY[i] = m_mouseY;
      m_mousePendingCallbackMask |= mouseReleaseEventMask(i);
    }
  }
}

bool PcBios::dispatchMouseCallback(PcMachine & machine)
{
  uint16_t const eventMask = m_mousePendingCallbackMask & m_mouseCallbackMask;
  if (!m_mouseInstalled || eventMask == 0 || (m_mouseCallbackSegment == 0 && m_mouseCallbackOffset == 0))
    return false;

  uint32_t const callbackLinear = linearAddress(m_mouseCallbackSegment, m_mouseCallbackOffset);
  if (!machine.isRamRangeValid(callbackLinear, 1))
    return false;
  if (machine.readMemory8(callbackLinear) == 0xcf) {
    // Some DOS programs leave the mouse callback pointed at a harmless IRET
    // stub while still enabling event masks. The Microsoft-compatible callback
    // convention is a far CALL/RETF; jumping into an IRET-only stub would pop
    // one extra word and corrupt the interrupted program's stack. Treat this
    // common stub as an installed-but-empty callback.
    m_mousePendingCallbackMask &= static_cast<uint16_t>(~eventMask);
    m_mousePendingCallbackMotionX = 0;
    m_mousePendingCallbackMotionY = 0;
    return true;
  }

  uint16_t const oldSp = PcI8086::SP();
  uint16_t const returnIpAddress = static_cast<uint16_t>(oldSp - 4);
  uint16_t const returnCsAddress = static_cast<uint16_t>(oldSp - 2);
  uint32_t const returnIpLinear = linearAddress(PcI8086::SS(), returnIpAddress);
  uint32_t const returnCsLinear = linearAddress(PcI8086::SS(), returnCsAddress);
  if (!machine.isRamRangeValid(returnIpLinear, 4))
    return false;

  machine.writeMemory16(returnIpLinear, PcI8086::IP());
  machine.writeMemory16(returnCsLinear, PcI8086::CS());
  PcI8086::setSP(returnIpAddress);
  PcI8086::setAX(eventMask);
  PcI8086::setBX(m_mouseButtons);
  PcI8086::setCX(m_mouseX);
  PcI8086::setDX(m_mouseY);
  PcI8086::setSI(static_cast<uint16_t>(m_mousePendingCallbackMotionX));
  PcI8086::setDI(static_cast<uint16_t>(m_mousePendingCallbackMotionY));
  PcI8086::setCS(m_mouseCallbackSegment);
  PcI8086::setIP(m_mouseCallbackOffset);

  m_mousePendingCallbackMask &= static_cast<uint16_t>(~eventMask);
  m_mousePendingCallbackMotionX = 0;
  m_mousePendingCallbackMotionY = 0;
  return true;
}

void PcBios::setMouseState(uint16_t x, uint16_t y, uint16_t buttons)
{
  m_mouseInstalled = true;
  updateMouseState(x, y, buttons, true);
}

void PcBios::setMouseSourceState(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t buttons)
{
  m_mouseInstalled = true;
  uint32_t const xSpan = m_mouseMaxX >= m_mouseMinX ? (m_mouseMaxX - m_mouseMinX) : 0;
  uint32_t const ySpan = m_mouseMaxY >= m_mouseMinY ? (m_mouseMaxY - m_mouseMinY) : 0;
  uint16_t const mappedX = static_cast<uint16_t>(m_mouseMinX + (width > 1 ? (static_cast<uint32_t>(x) * xSpan) / (width - 1) : 0));
  uint16_t const mappedY = static_cast<uint16_t>(m_mouseMinY + (height > 1 ? (static_cast<uint32_t>(y) * ySpan) / (height - 1) : 0));
  updateMouseState(mappedX, mappedY, buttons, true);
}

uint64_t PcBios::absoluteTimerTicks() const
{
  uint64_t const elapsedMillis = monotonicMilliseconds() - m_timerBaseMillis;
  uint64_t const elapsedTicks = (elapsedMillis * 182 + 5000) / 10000; // 18.2 Hz BIOS-compatible tick
  return static_cast<uint64_t>(m_timerBaseTicks) + elapsedTicks;
}

uint32_t PcBios::timerDayTicks() const
{
  return static_cast<uint32_t>(absoluteTimerTicks() % BiosTicksPerDay);
}

void PcBios::updateTimerBda(PcMachine & machine) const
{
  uint64_t const ticks = absoluteTimerTicks();
  uint32_t const dayTicks = static_cast<uint32_t>(ticks % BiosTicksPerDay);
  machine.writeMemory16(0x046c, static_cast<uint16_t>(dayTicks & 0xffff));
  machine.writeMemory16(0x046e, static_cast<uint16_t>(dayTicks >> 16));
  machine.writeMemory8(0x0470, static_cast<uint8_t>((ticks / BiosTicksPerDay) & 0xff));
}

bool PcBios::handleTimerInterrupt(PcMachine & machine)
{
  updateTimerBda(machine);

  uint16_t const vectorOffset = machine.readMemory16(0x001c * 4 + 0);
  uint16_t const vectorSegment = machine.readMemory16(0x001c * 4 + 2);
  if ((vectorOffset != 0 || vectorSegment != 0) &&
      (vectorOffset != BiosUserTimerOffset || vectorSegment != BiosUserTimerSegment)) {
    PcI8086::triggerInterrupt(0x1c);
  }
  return true;
}

void PcBios::observeKeyboardScancode(PcMachine & machine, uint8_t rawScancode)
{
  // This shadow translation runs while a guest INT 09h handler reads port 0x60
  // (the BIOS-default path never gets here). Two handler styles exist and we
  // cannot tell them apart up front:
  //   * "chaining" handlers read the scancode then jump to the BIOS; the BIOS
  //     can no longer re-read the consumed byte, so the BDA relies on us.
  //   * "complete" handlers (e.g. QBASIC's editor) translate and fill the BDA
  //     themselves; shadowing them too would double every key.
  // So we DEFER: remember the translated key and the BDA tail now, then flush it
  // only when the key is actually consumed (flushShadowKey) and only if the
  // guest handler did not advance the tail itself. This gives exactly one BDA
  // entry per key for both handler styles.
  flushShadowKey(machine); // resolve any previously observed key first
  uint16_t key = 0;
  if (translateKeyboardScancode(machine, rawScancode, &key)) {
    m_shadowKey = key;
    m_shadowKeyPending = true;
    m_shadowTailAtObserve = machine.readMemory16(0x041c); // BDA keyboard buffer tail
  }
}

void PcBios::flushShadowKey(PcMachine & machine)
{
  if (!m_shadowKeyPending)
    return;
  m_shadowKeyPending = false;
  // If the tail moved, the guest handler already stored this key; don't double it.
  if (machine.readMemory16(0x041c) == m_shadowTailAtObserve)
    storeBdaKey(machine, m_shadowKey);
}

bool PcBios::handleInterrupt(PcMachine & machine, int interruptNumber)
{
  switch (interruptNumber) {
    case 0x08:
    {
      uint16_t const vectorOffset = machine.readMemory16(0x0008 * 4 + 0);
      uint16_t const vectorSegment = machine.readMemory16(0x0008 * 4 + 2);
      if ((vectorOffset != 0 || vectorSegment != 0) &&
          (vectorOffset != BiosTimerIrqOffset || vectorSegment != BiosTimerIrqSegment)) {
        return false; // a DOS program installed its own timer IRQ handler
      }
      return handleTimerInterrupt(machine);
    }
    case 0x10:
    {
      uint16_t const vectorOffset = machine.readMemory16(0x0010 * 4 + 0);
      uint16_t const vectorSegment = machine.readMemory16(0x0010 * 4 + 2);
      if ((vectorOffset != 0 || vectorSegment != 0) &&
          (vectorOffset != BiosVideoServiceOffset || vectorSegment != BiosVideoServiceSegment)) {
        return false; // a DOS program installed its own INT 10h handler; deliver to it
      }
      return handleVideoInterrupt(machine);
    }
    case 0x11:
      PcI8086::setAX(machine.readMemory16(0x0410)); // BIOS equipment word
      return true;
    case 0x12:
      PcI8086::setAX(640);
      return true;
    case 0x13:
      return handleDiskInterrupt(machine);
    case 0x14:
      return handleSerialInterrupt();
    case 0x15:
      return handleSystemInterrupt(machine);
    case 0x09:
    {
      uint16_t const vectorOffset = machine.readMemory16(0x0009 * 4 + 0);
      uint16_t const vectorSegment = machine.readMemory16(0x0009 * 4 + 2);
      if ((vectorOffset != 0 || vectorSegment != 0) &&
          (vectorOffset != BiosKeyboardIrqOffset || vectorSegment != BiosKeyboardIrqSegment)) {
        return false; // a DOS program installed its own INT 09h handler; deliver the raw IRQ to it
      }
      uint16_t key = 0;
      if (fetchKey(machine, &key))
        storeBdaKey(machine, key);
      return true;
    }
    case BiosKeyboardInternalInterrupt:
    {
      uint16_t key = 0;
      if (fetchKey(machine, &key))
        storeBdaKey(machine, key);
      // The ROM INT 09h handler sends an EOI to the master PIC before it
      // returns. DOS programs such as QBasic hook INT 09h and chain to this
      // BIOS entry with a FAR CALL, relying on the old handler to complete
      // IRQ1. Mirror that side effect for the compatibility stub so later USB
      // keyboard reports are not suppressed as nested IRQs.
      machine.completeKeyboardIrqService();
      return true;
    }
    case BiosTimerInternalInterrupt:
      return handleTimerInterrupt(machine);
    case BiosKeyboardServiceInterrupt:
      if (PcI8086::AH() == 0x00 || PcI8086::AH() == 0x10) {
        uint16_t key = 0;
        if (!popBdaKey(machine, &key)) {
          if (fetchKey(machine, &key))
            storeBdaKey(machine, key);
          if (!popBdaKey(machine, &key)) {
            PcI8086::setAX(0x0000);
            PcI8086::setFlagZF(true);
            return true;
          }
        }
        PcI8086::setAX(key);
        PcI8086::setFlagZF(false);
        return true;
      }
      return handleKeyboardInterrupt(machine);
    case BiosVideoServiceInterrupt:
      return handleVideoInterrupt(machine);
    case 0x16:
    {
      uint16_t const vectorOffset = machine.readMemory16(0x0016 * 4 + 0);
      uint16_t const vectorSegment = machine.readMemory16(0x0016 * 4 + 2);
      if ((vectorOffset != 0 || vectorSegment != 0) &&
          (vectorOffset != BiosKeyboardServiceOffset || vectorSegment != BiosKeyboardServiceSegment)) {
        return false; // a DOS program installed its own INT 16h handler; deliver to it
      }
      return handleKeyboardInterrupt(machine);
    }
    case 0x17: // printer services: no LPT device is attached, but answer BIOS probes cleanly
      switch (PcI8086::AH()) {
        case 0x00: // print character
        case 0x01: // initialize printer
        case 0x02: // get printer status
          PcI8086::setAH(0x01); // timeout/no printer present
          return true;
        default:
          PcI8086::setAH(0x01);
          return true;
      }
    case 0x1a:
      return handleClockInterrupt();
    case 0x1c:
    {
      uint16_t const vectorOffset = machine.readMemory16(0x001c * 4 + 0);
      uint16_t const vectorSegment = machine.readMemory16(0x001c * 4 + 2);
      if ((vectorOffset != 0 || vectorSegment != 0) &&
          (vectorOffset != BiosUserTimerOffset || vectorSegment != BiosUserTimerSegment)) {
        return false;
      }
      return true;
    }
    case 0x29:
      writeTeletype(machine, PcI8086::AL(), 0x07);
      return true;
    case 0x33:
      return handleMouseInterrupt(machine);
    case 0x67:
      return machine.handleEmsInterrupt();
    default:
      return false;
  }
}

bool PcBios::handleVideoInterrupt(PcMachine & machine)
{
  uint8_t const function = PcI8086::AH();
  auto writeAttributeRegister = [&machine](uint8_t index, uint8_t value) {
    tabdos::writeAttributeRegister(machine, index, value);
  };
  auto readAttributeRegister = [&machine](uint8_t index) {
    return tabdos::readAttributeRegister(machine, index);
  };
  switch (function) {
    case 0x00: // set video mode
    {
      uint8_t const requestedMode = PcI8086::AL() & 0x7f;
      bool const preserveDisplayMemory = (PcI8086::AL() & 0x80) != 0;
      uint8_t savedAttributePalette[16] = {};
      uint8_t savedOverscan = 0;
      uint8_t savedColorSelect = 0;
      uint8_t savedDacColorPageMode = m_dacColorPageMode;
      uint8_t savedDacPalette[256][3] = {};
      PcMachine::VgaDacState savedDacState = {};
      PcMachine::VgaAttributeState savedAttributeState = {};
      if (!m_defaultPaletteLoadingEnabled) {
        savedDacState = machine.vgaDacState();
        savedAttributeState = machine.vgaAttributeState();
        savedDacColorPageMode = (readAttributeRegister(0x10) & 0x80) ? 1 : 0;
        for (uint8_t i = 0; i < 16; ++i)
          savedAttributePalette[i] = readAttributeRegister(i);
        savedOverscan = readAttributeRegister(0x11);
        savedColorSelect = readAttributeRegister(0x14);
        machine.writePort(0x03c7, 0x00);
        for (int i = 0; i < 256; ++i) {
          savedDacPalette[i][0] = machine.readPort(0x03c9);
          savedDacPalette[i][1] = machine.readPort(0x03c9);
          savedDacPalette[i][2] = machine.readPort(0x03c9);
        }
      }
      m_dacColorPageMode = 0;
      if (requestedMode <= 0x03 || requestedMode == 0x07) {
        uint16_t const columns = requestedMode <= 0x01 ? 40 : 80;
        uint16_t const pageSize = columns == 40 ? 0x0800 : 0x1000;
        m_activeDisplayPage = 0;
        machine.setVideoMode(PcMachine::VideoMode::Text80);
        machine.textRenderer().setColumns(columns);
        machine.writeMemory8(0x0484, PcTextRenderer::Rows - 1);
        machine.writeMemory16(0x0485, PcTextRenderer::CellHeight);
        machine.textRenderer().setCellHeight(PcTextRenderer::CellHeight);
        machine.textRenderer().setRows(PcTextRenderer::Rows);
        machine.writeMemory8(0x0449, requestedMode);
        machine.writeMemory16(0x044a, columns);
        machine.writeMemory16(0x044c, pageSize);
        machine.writeMemory16(0x044e, 0x0000);
        machine.writeMemory8(0x0462, 0x00);
        machine.writeMemory16(0x0463, requestedMode == 0x07 ? 0x03b4 : 0x03d4);
        machine.writeMemory16(0x0410, static_cast<uint16_t>((machine.readMemory16(0x0410) & ~0x0030) |
                                                            (requestedMode == 0x07 ? 0x0030 : 0x0020)));
        if (requestedMode != 0x07)
          machine.setCgaCompatibilityState(cgaModeControlForBiosMode(requestedMode), 0x00);
        if (!preserveDisplayMemory)
          clearTextScreen(machine, 0x07);
      } else {
        PcMachine::VideoMode newMode = PcMachine::VideoMode::Text80;
        uint16_t pageSize = 0x4000;
        uint16_t columns = 40;
        switch (requestedMode) {
          case 0x04:
          case 0x05:
            newMode = PcMachine::VideoMode::CgaGraphics320x200x4;
            columns = 40;
            break;
          case 0x06:
            newMode = PcMachine::VideoMode::CgaGraphics640x200x2;
            columns = 80;
            break;
          case 0x0d:
            newMode = PcMachine::VideoMode::VgaGraphics320x200x16;
            pageSize = 0x2000;
            columns = 40;
            break;
          case 0x0e:
            newMode = PcMachine::VideoMode::VgaGraphics640x200x16;
            pageSize = 0x4000;
            columns = 80;
            break;
          case 0x0f:
            newMode = PcMachine::VideoMode::VgaGraphics640x350x2;
            pageSize = 0x8000;
            columns = 80;
            break;
          case 0x10:
            newMode = PcMachine::VideoMode::VgaGraphics640x350x16;
            pageSize = 0x8000;
            columns = 80;
            break;
          case 0x11:
            newMode = PcMachine::VideoMode::VgaGraphics640x480x2;
            pageSize = 0x0000; // 64 KiB wraps in the BDA word
            columns = 80;
            break;
          case 0x12:
            newMode = PcMachine::VideoMode::VgaGraphics640x480x16;
            pageSize = 0x0000; // 64 KiB does not fit in the BDA word; VGA BIOSes commonly wrap here
            columns = 80;
            break;
          case 0x13:
            newMode = PcMachine::VideoMode::VgaGraphics320x200x256;
            pageSize = 0xfa00;
            columns = 40;
            break;
          default:
            return true;
        }
        machine.setVideoMode(newMode, true, !preserveDisplayMemory);
        machine.writeMemory8(0x0449, requestedMode);
        machine.writeMemory16(0x044a, columns);
        machine.writeMemory16(0x044c, pageSize);
        machine.writeMemory16(0x044e, 0x0000);
        machine.writeMemory8(0x0462, 0x00);
        machine.writeMemory16(0x0463, 0x03d4);
        machine.writeMemory16(0x0410, static_cast<uint16_t>((machine.readMemory16(0x0410) & ~0x0030) | 0x0020));
        if (requestedMode <= 0x06)
          machine.setCgaCompatibilityState(cgaModeControlForBiosMode(requestedMode),
                                           requestedMode == 0x06 ? 0x0f : 0x00);
        setGraphicsTextMetrics(machine, this->graphicsTextCellHeight(machine));
        if (!preserveDisplayMemory) {
          for (uint32_t address = PcMachine::VgaGraphicsMemoryBase; address < PcMachine::VgaGraphicsMemoryBase + 0x10000; ++address)
            machine.writeVideoMemory8(address, 0x00);
        }
      }
      if (!m_defaultPaletteLoadingEnabled) {
        for (uint8_t i = 0; i < 16; ++i)
          writeAttributeRegister(i, savedAttributePalette[i]);
        writeAttributeRegister(0x11, savedOverscan);
        writeAttributeRegister(0x14, savedColorSelect);
        m_dacColorPageMode = savedDacColorPageMode;
        uint8_t restoredModeControl = readAttributeRegister(0x10);
        if (m_dacColorPageMode)
          restoredModeControl |= 0x80;
        else
          restoredModeControl &= static_cast<uint8_t>(~0x80);
        writeAttributeRegister(0x10, restoredModeControl);
        machine.writePort(0x03c6, savedDacState.pelMask);
        machine.writePort(0x03c8, 0x00);
        for (int i = 0; i < 256; ++i) {
          machine.writePort(0x03c9, savedDacPalette[i][0]);
          machine.writePort(0x03c9, savedDacPalette[i][1]);
          machine.writePort(0x03c9, savedDacPalette[i][2]);
        }
        machine.setVgaDacState(savedDacState);
        machine.setVgaAttributeState(savedAttributeState);
      } else if (m_grayScaleSummingEnabled) {
        grayScaleDacBlock(machine, 0x00, 256);
      }
      return true;
    }
    case 0x01: // set cursor shape
    {
      machine.writeMemory16(0x0460, static_cast<uint16_t>(PcI8086::CH()) << 8 | PcI8086::CL());
      uint16_t crtcBase = machine.readMemory16(0x0463);
      if (crtcBase != 0x03b4 && crtcBase != 0x03d4)
        crtcBase = 0x03d4;
      writeIndexedRegister(machine, crtcBase, static_cast<uint16_t>(crtcBase + 1), 0x0a, PcI8086::CH());
      writeIndexedRegister(machine, crtcBase, static_cast<uint16_t>(crtcBase + 1), 0x0b, PcI8086::CL());
      return true;
    }
    case 0x02: // set cursor position
      setCursor(machine, PcI8086::BH(), PcI8086::DH(), PcI8086::DL());
      return true;
    case 0x03: // get cursor position
    {
      uint8_t const page = PcI8086::BH();
      uint16_t const packedCursor = page < 8 ? machine.readMemory16(0x0450 + page * 2) : cursorOffset(machine);
      uint16_t const cursorShape = machine.readMemory16(0x0460);
      PcI8086::setCH(static_cast<uint8_t>(cursorShape >> 8));
      PcI8086::setCL(static_cast<uint8_t>(cursorShape & 0x00ff));
      PcI8086::setDH(static_cast<uint8_t>(packedCursor >> 8));
      PcI8086::setDL(static_cast<uint8_t>(packedCursor & 0x00ff));
      return true;
    }
    case 0x05: // set active display page
    {
      uint8_t const page = PcI8086::AL();
      if (page < 8) {
        m_activeDisplayPage = page;
        machine.writeMemory8(0x0462, page);
        if (machine.isGraphicsMode()) {
          uint16_t const pageOffset = this->graphicsPageOffset(machine, page);
          uint16_t const crtcStartAddress = vgaCrtcStartAddressForPageOffset(machine, pageOffset);
          machine.writeMemory16(0x044e, pageOffset);
          writeIndexedRegister(machine, 0x03d4, 0x03d5, 0x0c, static_cast<uint8_t>(crtcStartAddress >> 8));
          writeIndexedRegister(machine, 0x03d4, 0x03d5, 0x0d, static_cast<uint8_t>(crtcStartAddress & 0xff));
        } else {
          uint16_t const pageSize = machine.readMemory16(0x044c) ? machine.readMemory16(0x044c) : 0x1000;
          uint16_t const pageOffset = static_cast<uint16_t>(page) * pageSize;
          machine.writeMemory16(0x044e, pageOffset);
          uint16_t const crtcStartAddress = static_cast<uint16_t>(pageOffset / 2);
          writeIndexedRegister(machine, 0x03d4, 0x03d5, 0x0c, static_cast<uint8_t>(crtcStartAddress >> 8));
          writeIndexedRegister(machine, 0x03d4, 0x03d5, 0x0d, static_cast<uint8_t>(crtcStartAddress & 0xff));
          uint16_t const packedCursor = machine.readMemory16(0x0450 + page * 2);
          m_cursorRow = static_cast<uint8_t>(packedCursor >> 8);
          m_cursorColumn = static_cast<uint8_t>(packedCursor & 0x00ff);
          uint8_t const rows = activeTextRows(machine);
          if (m_cursorRow >= rows)
            m_cursorRow = rows - 1;
          uint16_t const columns = activeTextColumns(machine);
          if (m_cursorColumn >= columns)
            m_cursorColumn = static_cast<uint8_t>(columns - 1);
          machine.textRenderer().setCursor(m_cursorRow, m_cursorColumn, true);
        }
      }
      return true;
    }
    case 0x06: // scroll up / clear window
    case 0x07: // scroll down / clear window
      scrollTextWindow(machine,
                       PcI8086::AL(),
                       PcI8086::BH(),
                       PcI8086::CH(),
                       PcI8086::CL(),
                       PcI8086::DH(),
                       PcI8086::DL(),
                       function == 0x07);
      return true;
    case 0x08: // read character and attribute at cursor
    {
      uint32_t const base = textPageMemoryBase(machine, PcI8086::BH());
      uint16_t const cell = machine.readVideoMemory16(base + cursorOffsetForPage(machine, PcI8086::BH()));
      PcI8086::setAL(static_cast<uint8_t>(cell & 0x00ff));
      PcI8086::setAH(static_cast<uint8_t>(cell >> 8));
      return true;
    }
    case 0x09: // write character and attribute at cursor
    case 0x0a: // write character only at cursor
    {
      uint8_t const ch = PcI8086::AL();
      uint16_t count = PcI8086::CX();
      uint8_t const page = PcI8086::BH() < 8 ? PcI8086::BH() : activeDisplayPage(machine);
      uint16_t const packedCursor = machine.readMemory16(0x0450 + page * 2);
      uint8_t row = static_cast<uint8_t>(packedCursor >> 8);
      uint8_t column = static_cast<uint8_t>(packedCursor & 0x00ff);
      uint16_t const columns = activeTextColumns(machine);
      uint8_t const rows = activeTextRows(machine);
      if (row >= rows)
        row = rows - 1;
      if (column >= columns)
        column = static_cast<uint8_t>(columns - 1);
      if (machine.isGraphicsMode()) {
        uint8_t const color = PcI8086::BL();
        bool const xorMode = (color & 0x80) != 0 && machine.videoMode() != PcMachine::VideoMode::VgaGraphics320x200x256;
        while (count--) {
          writeGraphicsCharacter(machine, ch, color & (xorMode ? 0x7f : 0xff), row, column, page, xorMode);
          if (++column >= columns) {
            column = 0;
            if (++row >= rows)
              row = rows - 1;
          }
        }
      } else {
        uint32_t const base = textPageMemoryBase(machine, page);
        while (count--) {
          uint32_t const address = base + 2 * (row * columns + column);
          uint8_t const attribute = function == 0x09
                                    ? PcI8086::BL()
                                    : static_cast<uint8_t>(machine.readVideoMemory16(address) >> 8);
          machine.writeVideoMemory16(address, static_cast<uint16_t>(attribute) << 8 | ch);
          if (++column >= columns) {
            column = 0;
            if (++row >= rows)
              row = rows - 1;
          }
        }
      }
      return true;
    }
    case 0x0e: // teletype output
      writeTeletype(machine, PcI8086::AL(), PcI8086::BL() ? PcI8086::BL() : 0x07);
      return true;
    case 0x0f: // get current video mode
      PcI8086::setAL(machine.readMemory8(0x0449));
      PcI8086::setAH(static_cast<uint8_t>(machine.readMemory16(0x044a)));
      PcI8086::setBH(machine.readMemory8(0x0462));
      return true;
    case 0x0b: // set background/border or palette
      if (PcI8086::BH() == 0x00) {
        if (usesVgaAttributeController(machine.videoMode()))
          writeAttributeRegister(0x11, PcI8086::BL() & 0x3f);
        else
          machine.writePort(0x03d9, static_cast<uint8_t>((machine.readPort(0x03d9) & 0xf0) | (PcI8086::BL() & 0x0f)));
      } else if (PcI8086::BH() == 0x01 && isCgaGraphicsMode(machine.videoMode())) {
        machine.writePort(0x03d9, static_cast<uint8_t>((machine.readPort(0x03d9) & ~0x20) | ((PcI8086::BL() & 1) ? 0x20 : 0)));
      }
      return true;
    case 0x0c: // write graphics pixel
    {
      uint8_t const rawColor = PcI8086::AL();
      bool const xorMode = (rawColor & 0x80) != 0 && machine.videoMode() != PcMachine::VideoMode::VgaGraphics320x200x256;
      machine.writeGraphicsPixel(PcI8086::CX(),
                                 PcI8086::DX(),
                                 rawColor & (xorMode ? 0x7f : 0xff),
                                 xorMode,
                                 this->graphicsPageOffset(machine, PcI8086::BH()));
      return true;
    }
    case 0x0d: // read graphics pixel
    {
      uint8_t color = 0;
      if (machine.readGraphicsPixel(PcI8086::CX(), PcI8086::DX(), &color, this->graphicsPageOffset(machine, PcI8086::BH())))
        PcI8086::setAL(color);
      else
        PcI8086::setAL(0);
      return true;
    }
    case 0x13: // write string
    {
      uint8_t const mode = PcI8086::AL();
      bool const updateCursor = (mode & 0x01) != 0;
      bool const stringHasAttributes = (mode & 0x02) != 0;
      uint8_t const page = PcI8086::BH();
      uint8_t const fallbackAttribute = PcI8086::BL();
      uint16_t count = PcI8086::CX();
      uint8_t row = PcI8086::DH();
      uint8_t column = PcI8086::DL();
      uint32_t source = linearAddress(PcI8086::ES(), PcI8086::BP());
      size_t const bytes = static_cast<size_t>(count) * (stringHasAttributes ? 2 : 1);
      if (machine.isRamRangeValid(source, bytes)) {
        uint16_t const columns = activeTextColumns(machine);
        uint8_t const rows = activeTextRows(machine);
        while (count-- && row < rows) {
          uint8_t const ch = machine.readMemory8(source++);
          uint8_t const attribute = stringHasAttributes ? machine.readMemory8(source++) : fallbackAttribute;
          if (column < columns) {
            if (machine.isGraphicsMode()) {
              bool const xorMode = (attribute & 0x80) != 0 &&
                                   machine.videoMode() != PcMachine::VideoMode::VgaGraphics320x200x256;
              writeGraphicsCharacter(machine, ch, attribute & (xorMode ? 0x7f : 0xff), row, column, page, xorMode);
            } else {
              uint32_t const base = textPageMemoryBase(machine, page);
              uint32_t const address = base + 2 * (row * columns + column);
              machine.writeVideoMemory16(address, static_cast<uint16_t>(attribute) << 8 | ch);
            }
          }
          if (++column >= columns) {
            column = 0;
            ++row;
          }
        }
        if (updateCursor)
          setCursor(machine, page, row >= rows ? rows - 1 : row, column);
      }
      return true;
    }
    case 0x10: // palette/color register services
      switch (PcI8086::AL()) {
        case 0x00: // set single palette register
          writeAttributeRegister(PcI8086::BL() & 0x1f, PcI8086::BH());
          break;
        case 0x01: // set border/overscan
          writeAttributeRegister(0x11, PcI8086::BH());
          break;
        case 0x02: // set all palette registers: ES:DX points to 17 bytes
        {
          uint32_t const table = (static_cast<uint32_t>(PcI8086::ES()) << 4) + PcI8086::DX();
          if (machine.isRamRangeValid(table, 17)) {
            for (uint8_t i = 0; i < 16; ++i) {
              writeAttributeRegister(i, machine.readMemory8(table + i));
            }
            writeAttributeRegister(0x11, machine.readMemory8(table + 16));
          }
          break;
        }
        case 0x03: // toggle intensity/blinking bit
        {
          uint8_t modeControl = readAttributeRegister(0x10);
          if (PcI8086::BL() & 0x01)
            modeControl |= 0x08; // enable blink
          else
            modeControl &= static_cast<uint8_t>(~0x08); // enable intensive background
          writeAttributeRegister(0x10, modeControl);
          break;
        }
        case 0x07: // read single palette register
          PcI8086::setBH(readAttributeRegister(PcI8086::BL() & 0x1f));
          break;
        case 0x08: // read overscan
          PcI8086::setBH(readAttributeRegister(0x11));
          break;
        case 0x09: // read all palette registers: ES:DX points to 17 bytes
        {
          uint32_t const table = (static_cast<uint32_t>(PcI8086::ES()) << 4) + PcI8086::DX();
          if (machine.isRamRangeValid(table, 17)) {
            for (uint8_t i = 0; i < 16; ++i)
              machine.writeMemory8(table + i, readAttributeRegister(i));
            machine.writeMemory8(table + 16, readAttributeRegister(0x11));
          }
          break;
        }
        case 0x10: // set individual DAC register
          machine.writePort(0x03c8, PcI8086::BX() & 0xff);
          machine.writePort(0x03c9, PcI8086::DH());
          machine.writePort(0x03c9, PcI8086::CH());
          machine.writePort(0x03c9, PcI8086::CL());
          break;
        case 0x12: // set block of DAC registers
        {
          uint32_t const table = (static_cast<uint32_t>(PcI8086::ES()) << 4) + PcI8086::DX();
          uint16_t const count = PcI8086::CX();
          size_t const bytes = static_cast<size_t>(count) * 3;
          if (machine.isRamRangeValid(table, bytes)) {
            machine.writePort(0x03c8, PcI8086::BX() & 0xff);
            for (size_t i = 0; i < bytes; ++i)
              machine.writePort(0x03c9, machine.readMemory8(table + static_cast<uint32_t>(i)));
          }
          break;
        }
        case 0x13: // select video DAC color page
          if (PcI8086::BL() == 0x00) {
            m_dacColorPageMode = PcI8086::BH() & 0x01;
            uint8_t modeControl = readAttributeRegister(0x10);
            if (m_dacColorPageMode)
              modeControl |= 0x80; // 16 pages of 16 colors: Color Select supplies DAC bits 4-5
            else
              modeControl &= static_cast<uint8_t>(~0x80); // 4 pages of 64 colors
            writeAttributeRegister(0x10, modeControl);
          } else if (PcI8086::BL() == 0x01) {
            m_dacColorPageMode = (readAttributeRegister(0x10) & 0x80) ? 1 : 0;
            uint8_t colorSelect = readAttributeRegister(0x14);
            if (m_dacColorPageMode)
              colorSelect = static_cast<uint8_t>((colorSelect & 0xf0) | (PcI8086::BH() & 0x0f));
            else
              colorSelect = static_cast<uint8_t>((colorSelect & 0xf3) | ((PcI8086::BH() & 0x03) << 2));
            writeAttributeRegister(0x14, colorSelect);
          }
          break;
        case 0x15: // read individual DAC register
          machine.writePort(0x03c7, PcI8086::BX() & 0xff);
          PcI8086::setDH(machine.readPort(0x03c9));
          PcI8086::setCH(machine.readPort(0x03c9));
          PcI8086::setCL(machine.readPort(0x03c9));
          break;
        case 0x17: // read block of DAC registers
        {
          uint32_t const table = (static_cast<uint32_t>(PcI8086::ES()) << 4) + PcI8086::DX();
          uint16_t const count = PcI8086::CX();
          size_t const bytes = static_cast<size_t>(count) * 3;
          if (machine.isRamRangeValid(table, bytes)) {
            machine.writePort(0x03c7, PcI8086::BX() & 0xff);
            for (size_t i = 0; i < bytes; ++i)
              machine.writeMemory8(table + static_cast<uint32_t>(i), machine.readPort(0x03c9));
          }
          break;
        }
        case 0x18: // set PEL mask
        {
          PcMachine::VgaDacState dacState = machine.vgaDacState();
          dacState.pelMaskReadCount = 0;
          machine.setVgaDacState(dacState);
          machine.writePort(0x03c6, PcI8086::BL());
          break;
        }
        case 0x19: // read PEL mask
          PcI8086::setBL(machine.vgaDacState().pelMask);
          break;
        case 0x1a: // read video DAC color page state
        {
          m_dacColorPageMode = (readAttributeRegister(0x10) & 0x80) ? 1 : 0;
          uint8_t const colorSelect = readAttributeRegister(0x14);
          PcI8086::setBL(m_dacColorPageMode);
          PcI8086::setBH(m_dacColorPageMode ? (colorSelect & 0x0f) : ((colorSelect >> 2) & 0x03));
          break;
        }
        case 0x1b: // perform gray-scale summing on a block of DAC registers
          grayScaleDacBlock(machine, static_cast<uint8_t>(PcI8086::BX()), PcI8086::CX());
          break;
        default:
          break;
      }
      return true;
    case 0x11: // font/character generator services
      if (PcI8086::AL() == 0x00 || PcI8086::AL() == 0x10) { // load user font
        uint8_t const cellHeight = PcI8086::BH();
        if (loadUserFont(machine, cellHeight, PcI8086::DX(), PcI8086::CX(), linearAddress(PcI8086::ES(), PcI8086::BP())))
          setTextCellHeight(machine, cellHeight == 8 || cellHeight == 14 ? cellHeight : PcTextRenderer::CellHeight);
      } else if (PcI8086::AL() == 0x01 || PcI8086::AL() == 0x11) { // load ROM 8x14 font
        machine.textRenderer().setFont(PcTextRenderer::defaultFont());
        writeBiosFontImage(machine, 14);
        setTextCellHeight(machine, 14);
      } else if (PcI8086::AL() == 0x02 || PcI8086::AL() == 0x12) { // load ROM 8x8 font
        machine.textRenderer().setFont(PcTextRenderer::defaultFont());
        writeBiosFontImage(machine, 8);
        setTextCellHeight(machine, 8);
      } else if (PcI8086::AL() == 0x03) { // set block specifier
        writeIndexedRegister(machine, 0x03c4, 0x03c5, 0x03, PcI8086::BL() & 0x3f);
      } else if (PcI8086::AL() == 0x04 || PcI8086::AL() == 0x14) { // load ROM 8x16 font
        machine.textRenderer().setFont(PcTextRenderer::defaultFont());
        writeBiosFontImage(machine, 16);
        setTextCellHeight(machine, 16);
      } else if (PcI8086::AL() == 0x20) { // graphics-mode user 8x8 font
        if (loadUserFont(machine, 8, PcI8086::DX(), PcI8086::CX(), linearAddress(PcI8086::ES(), PcI8086::BP()), false))
          setGraphicsTextMetrics(machine, 8);
      } else if (PcI8086::AL() == 0x21) { // graphics-mode user font
        uint8_t const cellHeight = PcI8086::BH();
        if (loadUserFont(machine, cellHeight, PcI8086::DX(), PcI8086::CX(), linearAddress(PcI8086::ES(), PcI8086::BP()), false))
          setGraphicsTextMetrics(machine, cellHeight == 8 || cellHeight == 14 || cellHeight == 16 ? cellHeight : graphicsTextCellHeight(machine));
      } else if (PcI8086::AL() == 0x22) { // graphics-mode ROM 8x14 font
        loadRomGraphicsFont(machine, 14);
      } else if (PcI8086::AL() == 0x23) { // graphics-mode ROM 8x8 font
        loadRomGraphicsFont(machine, 8);
      } else if (PcI8086::AL() == 0x24) { // graphics-mode ROM 8x16 font
        loadRomGraphicsFont(machine, 16);
      } else if (PcI8086::AL() == 0x30) { // get font information
        uint8_t const cellHeight = fontHeightForInfoBlock(machine, PcI8086::BH());
        uint32_t const fontLinear = biosFontLinearForCellHeight(cellHeight);
        if (cellHeight == 16 && machine.isRamRangeValid(fontLinear, sizeof(PcFont8x16Data)))
          machine.writeMemoryBlock(fontLinear, PcFont8x16Data, sizeof(PcFont8x16Data));
        else
          writeBiosFontImage(machine, cellHeight);
        PcI8086::setES(BiosFont8x16Segment);
        PcI8086::setBP(biosFontOffsetForCellHeight(cellHeight));
        PcI8086::setCX(cellHeight);
        PcI8086::setDL(machine.readMemory8(0x0484));
      }
      return true;
    case 0x12: // EGA/VGA alternate select
      if (PcI8086::BL() == 0x10) {
        uint8_t const egaVgaControl = machine.readMemory8(0x0487);
        uint16_t const crtcBase = machine.readMemory16(0x0463);
        PcI8086::setBH((crtcBase == 0x03b4 || (egaVgaControl & 0x02)) ? 0x01 : 0x00);
        PcI8086::setBL(static_cast<uint8_t>((egaVgaControl >> 5) & 0x03));
        PcI8086::setCH(0x00);
        PcI8086::setCL(static_cast<uint8_t>(machine.readMemory8(0x0488) & 0x0f));
      } else if (PcI8086::BL() == 0x30) { // select vertical scan-line count
        uint8_t characterHeight = PcTextRenderer::CellHeight;
        uint8_t maxScanLine = PcTextRenderer::CellHeight - 1;
        switch (PcI8086::AL()) {
          case 0x00: // 200 scan lines, normally 8x8 text cells
            characterHeight = 8;
            maxScanLine = 7;
            break;
          case 0x01: // 350 scan lines, normally 8x14 text cells
            characterHeight = 14;
            maxScanLine = 13;
            break;
          case 0x02: // 400 scan lines, normally 8x16 text cells
            characterHeight = 16;
            maxScanLine = 15;
            break;
          default:
            PcI8086::setAL(0x00);
            return true;
        }
        (void) maxScanLine;
        setTextCellHeight(machine, characterHeight, PcTextRenderer::Rows - 1);
        PcI8086::setAL(0x12);
      } else if (PcI8086::BL() == 0x31) { // enable/disable default palette loading on mode set
        if (PcI8086::AL() <= 0x01) {
          m_defaultPaletteLoadingEnabled = PcI8086::AL() == 0x00;
          PcI8086::setAL(0x12);
        } else {
          PcI8086::setAL(0x00);
        }
      } else if (PcI8086::BL() == 0x32) { // enable/disable CPU video memory addressing
        if (PcI8086::AL() <= 0x01) {
          uint8_t miscOutput = machine.readPort(0x03cc);
          if (PcI8086::AL() == 0x00)
            miscOutput |= 0x02; // ERAM: enable CPU access to display memory
          else
            miscOutput &= static_cast<uint8_t>(~0x02);
          machine.writePort(0x03c2, miscOutput);
          PcI8086::setAL(0x12);
        } else {
          PcI8086::setAL(0x00);
        }
      } else if (PcI8086::BL() == 0x33) { // enable/disable default gray-scale summing on palette load
        if (PcI8086::AL() <= 0x01) {
          m_grayScaleSummingEnabled = PcI8086::AL() == 0x00;
          PcI8086::setAL(0x12);
        } else {
          PcI8086::setAL(0x00);
        }
      } else if (PcI8086::BL() == 0x34) { // enable/disable cursor emulation
        if (PcI8086::AL() <= 0x01) {
          // EGA/VGA BIOSes expose this as a compatibility switch for software
          // cursor handling. TabDOS renders a software cursor already, but DOS
          // applications still use the success return as a VGA capability
          // probe, so preserve the requested state and acknowledge it.
          m_cursorEmulationEnabled = PcI8086::AL() == 0x00;
          uint8_t egaVgaControl = machine.readMemory8(0x0487);
          if (m_cursorEmulationEnabled)
            egaVgaControl &= static_cast<uint8_t>(~0x01);
          else
            egaVgaControl |= 0x01;
          machine.writeMemory8(0x0487, egaVgaControl);
          PcI8086::setAL(0x12);
        } else {
          PcI8086::setAL(0x00);
        }
      } else if (PcI8086::BL() == 0x36) { // enable/disable screen refresh
        if (PcI8086::AL() <= 0x01) {
          uint8_t clockingMode = readIndexedRegister(machine, 0x03c4, 0x03c5, 0x01);
          if (PcI8086::AL() == 0x00)
            clockingMode &= static_cast<uint8_t>(~0x20); // enable display output
          else
            clockingMode |= 0x20; // screen off
          writeIndexedRegister(machine, 0x03c4, 0x03c5, 0x01, clockingMode);
          PcI8086::setAL(0x12);
        } else {
          PcI8086::setAL(0x00);
        }
      }
      return true;
    case 0x1a: // video display combination code
      if (PcI8086::AL() == 0x00) {
        uint8_t const index = machine.readMemory8(0x048a);
        DisplayCombinationEntry const entry = index < sizeof(DisplayCombinationTable) / sizeof(DisplayCombinationTable[0])
                                              ? DisplayCombinationTable[index]
                                              : DisplayCombinationTable[0x0b];
        PcI8086::setAL(0x1a); // function supported
        PcI8086::setBL(entry.active);
        PcI8086::setBH(entry.inactive);
      } else if (PcI8086::AL() == 0x01) {
        int const index = displayCombinationIndex(PcI8086::BH(), PcI8086::BL());
        if (index >= 0) {
          machine.writeMemory8(0x048a, static_cast<uint8_t>(index));
          PcI8086::setAL(0x1a);
        } else {
          PcI8086::setAL(0x00);
        }
      } else {
        PcI8086::setAL(0x00);
      }
      return true;
    case 0x1b: // VGA functionality/state information
    {
      uint32_t const stateInfo = (static_cast<uint32_t>(PcI8086::ES()) << 4) + PcI8086::DI();
      if (PcI8086::BX() == 0 && machine.isRamRangeValid(stateInfo, 64)) {
        for (int i = 0; i < 64; ++i)
          machine.writeMemory8(stateInfo + i, 0);
        if (machine.isRamRangeValid(VideoStaticTableLinear, sizeof(VideoStaticTable)))
          machine.writeMemoryBlock(VideoStaticTableLinear, VideoStaticTable, sizeof(VideoStaticTable));
        machine.writeMemory16(stateInfo + 0x00, VideoStaticTableOffset);
        machine.writeMemory16(stateInfo + 0x02, VideoStaticTableSegment);
        for (uint8_t i = 0; i < 30; ++i)
          machine.writeMemory8(stateInfo + 0x04 + i, machine.readMemory8(0x0449 + i));
        uint8_t const biosMode = machine.readMemory8(0x0449);
        uint16_t const pageSize = machine.readMemory16(0x044c);
        machine.writeMemory8(stateInfo + 0x22, machine.readMemory8(0x0484)); // rows minus one
        machine.writeMemory16(stateInfo + 0x23, machine.readMemory16(0x0485)); // character height
        machine.writeMemory8(stateInfo + 0x25, machine.videoMode() == PcMachine::VideoMode::HerculesGraphics ? 0x01 : 0x08);
        machine.writeMemory8(stateInfo + 0x26, 0x00);
        machine.writeMemory16(stateInfo + 0x27, colorsForVideoMode(biosMode));
        machine.writeMemory8(stateInfo + 0x29, displayPagesForVideoMode(biosMode, pageSize));
        machine.writeMemory8(stateInfo + 0x2a, rasterCodeForVideoMode(biosMode));
        machine.writeMemory8(stateInfo + 0x2b, 0x00);
        machine.writeMemory8(stateInfo + 0x2c, 0x00);
        uint8_t stateFlags = 0x01; // all video modes active
        if (m_grayScaleSummingEnabled)
          stateFlags |= 0x02;
        if (machine.readMemory8(0x0487) & 0x02)
          stateFlags |= 0x04; // monochrome display attached
        if (!m_defaultPaletteLoadingEnabled)
          stateFlags |= 0x08;
        if (m_cursorEmulationEnabled)
          stateFlags |= 0x10; // cursor emulation enabled
        if (readAttributeRegister(0x10) & 0x08)
          stateFlags |= 0x20; // blinking attribute enabled
        machine.writeMemory8(stateInfo + 0x2d, stateFlags);
        machine.writeMemory8(stateInfo + 0x31, static_cast<uint8_t>((machine.readMemory8(0x0487) >> 5) & 0x03));
        machine.writeMemory8(stateInfo + 0x32, 0x00);
        PcI8086::setAL(0x1b);
      } else {
        PcI8086::setAL(0x00);
      }
      return true;
    }
    case 0x1c: // VGA save/restore video state
    {
      uint8_t const subfunction = PcI8086::AL();
      uint16_t const stateMask = PcI8086::CX();
      uint32_t const buffer = linearAddress(PcI8086::ES(), PcI8086::BX());
      if (subfunction == 0x00) {
        PcI8086::setAL(0x1c);
        PcI8086::setBX(VideoStateSaveBlocks);
        PcI8086::setFlagCF(false);
      } else if (subfunction == 0x01 && saveVideoState(machine, buffer, stateMask)) {
        PcI8086::setAL(0x1c);
        PcI8086::setFlagCF(false);
      } else if (subfunction == 0x02 && restoreVideoState(machine, buffer, stateMask)) {
        PcI8086::setAL(0x1c);
        PcI8086::setFlagCF(false);
      } else {
        PcI8086::setAL(0x00);
        PcI8086::setFlagCF(true);
      }
      return true;
    }
    case 0x4e: // VESA/XGA extension probe. TabDOS exposes VGA, not XGA; fail cleanly.
      PcI8086::setAX(0x014e);
      PcI8086::setFlagCF(true);
      return true;
    case 0x4f: // VESA/VBE probe. TabDOS exposes VGA, not VBE; fail cleanly.
      PcI8086::setAX(0x014f);
      PcI8086::setFlagCF(true);
      return true;
    case 0x30: // vendor/video-BIOS extension probe; no extension installed on TabDOS
      PcI8086::setAL(0x00);
      return true;
    case 0xef: // video BIOS extension probe used by some DOS runtimes
      PcI8086::setAL(0x00);
      return true;
    case 0xfa: // OEM/video BIOS extension probe; not present on TabDOS
      PcI8086::setAL(0x00);
      return true;
    case 0xfe: // OEM/video BIOS extension probe; not present on TabDOS
      PcI8086::setAL(0x00);
      return true;
    case 0xff: // OEM/video BIOS extension probe; not present on TabDOS
      PcI8086::setAL(0x00);
      return true;
    default:
      return false;
  }
}

bool PcBios::saveVideoState(PcMachine & machine, uint32_t buffer, uint16_t stateMask)
{
  (void)stateMask;
  if (!machine.isRamRangeValid(buffer, VideoStateSaveBytes))
    return false;

  PcMachine::VgaDacState const dacState = machine.vgaDacState();
  PcMachine::VgaAttributeState const attributeState = machine.vgaAttributeState();
  PcMachine::VgaLatchState const latchState = machine.vgaLatchState();

  for (size_t i = 0; i < VideoStateSaveBytes; ++i)
    machine.writeMemory8(buffer + i, 0);

  machine.writeMemory8(buffer + 0, VideoStateMagic0);
  machine.writeMemory8(buffer + 1, VideoStateMagic1);
  machine.writeMemory8(buffer + 2, VideoStateMagic2);
  machine.writeMemory8(buffer + 3, VideoStateMagic3);
  machine.writeMemory8(buffer + 4, VideoStateVersion);
  machine.writeMemory8(buffer + 5, machine.readMemory8(0x0449)); // current video mode
  machine.writeMemory8(buffer + 6, machine.readMemory8(0x0462)); // active display page
  machine.writeMemory8(buffer + 7, m_dacColorPageMode);
  machine.writeMemory16(buffer + 8, machine.readMemory16(0x044a));  // columns
  machine.writeMemory16(buffer + 10, machine.readMemory16(0x044c)); // page size
  machine.writeMemory16(buffer + 12, machine.readMemory16(0x044e)); // page offset
  machine.writeMemory16(buffer + 14, machine.readMemory16(0x0463)); // CRTC base
  for (uint8_t i = 0; i < 8; ++i)
    machine.writeMemory16(buffer + 16 + i * 2, machine.readMemory16(0x0450 + i * 2));

  machine.writeMemory8(buffer + 32, machine.readPort(0x03cc)); // misc output
  machine.writeMemory8(buffer + 33, machine.readPort(0x03ca)); // feature control
  machine.writeMemory8(buffer + 34, machine.readPort(0x03c3)); // VGA enable
  machine.writeMemory8(buffer + 35, dacState.pelMask); // PEL mask
  machine.writeMemory8(buffer + 36, machine.readPort(0x03c4)); // sequencer index
  for (uint8_t i = 0; i < 8; ++i)
    machine.writeMemory8(buffer + 37 + i, readIndexedRegister(machine, 0x03c4, 0x03c5, i));
  machine.writeMemory8(buffer + 45, machine.readPort(0x03ce)); // graphics controller index
  for (uint8_t i = 0; i < 16; ++i)
    machine.writeMemory8(buffer + 46 + i, readIndexedRegister(machine, 0x03ce, 0x03cf, i));
  machine.writeMemory8(buffer + 62, machine.readPort(0x03d4)); // CRTC index
  for (uint8_t i = 0; i < 32; ++i)
    machine.writeMemory8(buffer + 63 + i, readIndexedRegister(machine, 0x03d4, 0x03d5, i));
  machine.writeMemory8(buffer + 95, machine.readPort(0x03c0)); // attribute index/PAS state
  for (uint8_t i = 0; i < 32; ++i)
    machine.writeMemory8(buffer + 96 + i, readAttributeRegister(machine, i));

  machine.writePort(0x03c7, 0x00);
  for (size_t i = 0; i < VideoStateDacBytes; ++i)
    machine.writeMemory8(buffer + VideoStateDacOffset + i, machine.readPort(0x03c9));

  machine.writeMemory8(buffer + VideoStateDacCursorOffset + 0, dacState.readIndex);
  machine.writeMemory8(buffer + VideoStateDacCursorOffset + 1, dacState.writeIndex);
  machine.writeMemory8(buffer + VideoStateDacCursorOffset + 2, dacState.readComponent);
  machine.writeMemory8(buffer + VideoStateDacCursorOffset + 3, dacState.writeComponent);
  machine.writeMemory8(buffer + VideoStateDacCursorOffset + 4, dacState.pelMask);
  machine.writeMemory8(buffer + VideoStateDacCursorOffset + 5, dacState.state);
  machine.writeMemory8(buffer + VideoStateDacCursorOffset + 6, dacState.pelMaskReadCount);
  machine.writeMemory8(buffer + VideoStateDacCursorOffset + 7, dacState.command);
  machine.writeMemory8(buffer + VideoStateAttributeCursorOffset + 0, attributeState.index);
  machine.writeMemory8(buffer + VideoStateAttributeCursorOffset + 1, attributeState.flipFlop ? 1 : 0);
  machine.writeMemory8(buffer + VideoStateAttributeCursorOffset + 2, attributeState.videoEnabled ? 1 : 0);
  for (size_t i = 0; i < VideoStateLatchBytes; ++i)
    machine.writeMemory8(buffer + VideoStateLatchOffset + i, latchState.plane[i]);

  machine.writePort(0x03c4, machine.readMemory8(buffer + 36));
  machine.writePort(0x03ce, machine.readMemory8(buffer + 45));
  machine.writePort(0x03d4, machine.readMemory8(buffer + 62));
  machine.setVgaDacState(dacState);
  machine.setVgaAttributeState(attributeState);
  return true;
}

bool PcBios::restoreVideoState(PcMachine & machine, uint32_t buffer, uint16_t stateMask)
{
  if (!machine.isRamRangeValid(buffer, VideoStateSaveBytes))
    return false;
  if (machine.readMemory8(buffer + 0) != VideoStateMagic0 ||
      machine.readMemory8(buffer + 1) != VideoStateMagic1 ||
      machine.readMemory8(buffer + 2) != VideoStateMagic2 ||
      machine.readMemory8(buffer + 3) != VideoStateMagic3)
    return false;

  bool const restoreHardware = (stateMask & VideoStateHardwareMask) != 0;
  bool const restoreBiosData = (stateMask & VideoStateBiosDataMask) != 0;
  bool const restoreDac = (stateMask & VideoStateDacMask) != 0;
  uint8_t const restoredBiosMode = machine.readMemory8(buffer + 5);
  if (restoreHardware || restoreBiosData)
    machine.setVideoMode(videoModeFromBiosMode(restoredBiosMode), false);

  if (restoreBiosData) {
    machine.writeMemory8(0x0449, restoredBiosMode);
    m_activeDisplayPage = machine.readMemory8(buffer + 6);
    if (m_activeDisplayPage >= 8)
      m_activeDisplayPage = 0;
    m_dacColorPageMode = machine.readMemory8(buffer + 7);
    machine.writeMemory16(0x044a, machine.readMemory16(buffer + 8));
    machine.textRenderer().setColumns(activeTextColumns(machine));
    machine.writeMemory16(0x044c, machine.readMemory16(buffer + 10));
    machine.writeMemory16(0x044e, machine.readMemory16(buffer + 12));
    machine.writeMemory16(0x0463, machine.readMemory16(buffer + 14));
    for (uint8_t i = 0; i < 8; ++i)
      machine.writeMemory16(0x0450 + i * 2, machine.readMemory16(buffer + 16 + i * 2));
    machine.writeMemory8(0x0462, m_activeDisplayPage);
  }

  if (restoreHardware) {
    machine.writePort(0x03c2, machine.readMemory8(buffer + 32));
    machine.writePort(0x03da, machine.readMemory8(buffer + 33));
    machine.writePort(0x03c3, machine.readMemory8(buffer + 34));
    for (uint8_t i = 0; i < 8; ++i)
      writeIndexedRegister(machine, 0x03c4, 0x03c5, i, machine.readMemory8(buffer + 37 + i));
    for (uint8_t i = 0; i < 16; ++i)
      writeIndexedRegister(machine, 0x03ce, 0x03cf, i, machine.readMemory8(buffer + 46 + i));
    writeIndexedRegister(machine, 0x03d4, 0x03d5, 0x11, static_cast<uint8_t>(machine.readMemory8(buffer + 63 + 0x11) & 0x7f));
    for (uint8_t i = 0; i < 32; ++i)
      writeIndexedRegister(machine, 0x03d4, 0x03d5, i, machine.readMemory8(buffer + 63 + i));
    for (uint8_t i = 0; i < 32; ++i)
      writeAttributeRegister(machine, i, machine.readMemory8(buffer + 96 + i));

    machine.writePort(0x03c4, machine.readMemory8(buffer + 36));
    machine.writePort(0x03ce, machine.readMemory8(buffer + 45));
    machine.writePort(0x03d4, machine.readMemory8(buffer + 62));
    PcMachine::VgaAttributeState const attributeState = {
      machine.readMemory8(buffer + VideoStateAttributeCursorOffset + 0),
      machine.readMemory8(buffer + VideoStateAttributeCursorOffset + 1) != 0,
      machine.readMemory8(buffer + VideoStateAttributeCursorOffset + 2) != 0,
    };
    PcMachine::VgaLatchState latchState = {};
    for (size_t i = 0; i < VideoStateLatchBytes; ++i)
      latchState.plane[i] = machine.readMemory8(buffer + VideoStateLatchOffset + i);
    machine.setVgaAttributeState(attributeState);
    machine.setVgaLatchState(latchState);
    m_dacColorPageMode = (readAttributeRegister(machine, 0x10) & 0x80) ? 1 : 0;
  }

  if (restoreDac) {
    machine.writePort(0x03c6, machine.readMemory8(buffer + 35));
    machine.writePort(0x03c8, 0x00);
    for (size_t i = 0; i < VideoStateDacBytes; ++i)
      machine.writePort(0x03c9, machine.readMemory8(buffer + VideoStateDacOffset + i));

    PcMachine::VgaDacState const dacState = {
      machine.readMemory8(buffer + VideoStateDacCursorOffset + 0),
      machine.readMemory8(buffer + VideoStateDacCursorOffset + 1),
      machine.readMemory8(buffer + VideoStateDacCursorOffset + 2),
      machine.readMemory8(buffer + VideoStateDacCursorOffset + 3),
      machine.readMemory8(buffer + VideoStateDacCursorOffset + 4),
      machine.readMemory8(buffer + VideoStateDacCursorOffset + 5),
      machine.readMemory8(buffer + VideoStateDacCursorOffset + 6),
      machine.readMemory8(buffer + VideoStateDacCursorOffset + 7),
    };
    machine.setVgaDacState(dacState);
  }

  if (restoreBiosData) {
    uint16_t const packedCursor = machine.readMemory16(0x0450 + m_activeDisplayPage * 2);
    setCursor(machine,
              m_activeDisplayPage,
              static_cast<uint8_t>(packedCursor >> 8),
              static_cast<uint8_t>(packedCursor & 0x00ff));
  }
  return true;
}

bool PcBios::handleDiskInterrupt(PcMachine & machine)
{
  uint8_t const function = PcI8086::AH();
  uint8_t const drive = PcI8086::DL();
  int const diskIndex = diskIndexForBiosDrive(drive);
  PcDiskImage * image = machine.disk(diskIndex);

  auto fail = [](uint8_t status) {
    PcI8086::setAH(status);
    PcI8086::setFlagCF(true);
    return true;
  };
  auto ok = []() {
    PcI8086::setAH(0x00);
    PcI8086::setFlagCF(false);
    return true;
  };

  switch (function) {
    case 0x00: // reset disk
      return image ? ok() : fail(0x01);
    case 0x10: // check drive ready
      return image ? ok() : fail(0x01);
    case 0x0c: // seek to cylinder: no head movement in image-backed storage
      return image ? ok() : fail(0x01);
    case 0x02: // read sectors
    case 0x03: // write sectors
    {
      if (!image)
        return fail(0x01);

      uint8_t const count = PcI8086::AL();
      if (count == 0)
        return fail(0x01);

      uint16_t const cylinder = static_cast<uint16_t>(PcI8086::CH()) |
                                (static_cast<uint16_t>(PcI8086::CL() & 0xc0) << 2);
      uint8_t const sector = PcI8086::CL() & 0x3f;
      uint8_t const head = PcI8086::DH();
      uint64_t lba = 0;
      if (!PcDiskImage::chsToLba(image->geometry(), cylinder, head, sector, &lba))
        return fail(0x04);

      uint32_t const transferAddress = (static_cast<uint32_t>(PcI8086::ES()) << 4) + PcI8086::BX();
      size_t const transferBytes = static_cast<size_t>(count) * PcDiskImage::SectorSize;
      if (!machine.isRamRangeValid(transferAddress, transferBytes))
        return fail(0x09);

      std::vector<uint8_t> buffer(transferBytes);
      if (function == 0x02) {
        if (!image->readSectors(lba, count, buffer.data()))
          return fail(0x04);
        machine.writeMemoryBlock(transferAddress, buffer.data(), buffer.size());
      } else {
        machine.readMemoryBlock(transferAddress, buffer.data(), buffer.size());
        if (!image->writeSectors(lba, count, buffer.data()))
          return fail(0x04);
        machine.recordDiskWrite(drive, lba, count);
      }
      PcI8086::setAL(count);
      return ok();
    }
    case 0x41: // extensions installation check
      if (!image || PcI8086::BX() != 0x55aa)
        return fail(0x01);
      PcI8086::setAH(0x21);   // EDD 2.1 compatible subset
      PcI8086::setBX(0xaa55); // installation signature
      PcI8086::setCX(0x0001); // extended read/write/check/params subset
      PcI8086::setFlagCF(false);
      return true;
    case 0x42: // extended read sectors
    case 0x43: // extended write sectors
    {
      if (!image)
        return fail(0x01);

      uint32_t const dap = linearAddress(PcI8086::DS(), PcI8086::SI());
      if (!machine.isRamRangeValid(dap, 16))
        return fail(0x09);

      uint8_t const packetSize = machine.readMemory8(dap);
      if (packetSize < 16)
        return fail(0x01);

      uint16_t const count = machine.readMemory16(dap + 2);
      uint16_t const offset = machine.readMemory16(dap + 4);
      uint16_t const segment = machine.readMemory16(dap + 6);
      uint64_t lba = 0;
      for (int i = 0; i < 4; ++i)
        lba |= static_cast<uint64_t>(machine.readMemory16(dap + 8 + i * 2)) << (i * 16);

      if (count == 0 || count > 127)
        return fail(0x01);
      if (lba + count > image->sectorCount())
        return fail(0x04);

      uint32_t const transferAddress = linearAddress(segment, offset);
      size_t const transferBytes = static_cast<size_t>(count) * PcDiskImage::SectorSize;
      if (!machine.isRamRangeValid(transferAddress, transferBytes))
        return fail(0x09);

      std::vector<uint8_t> buffer(transferBytes);
      if (function == 0x42) {
        if (!image->readSectors(lba, static_cast<uint8_t>(count), buffer.data()))
          return fail(0x04);
        machine.writeMemoryBlock(transferAddress, buffer.data(), buffer.size());
      } else {
        machine.readMemoryBlock(transferAddress, buffer.data(), buffer.size());
        if (!image->writeSectors(lba, static_cast<uint8_t>(count), buffer.data()))
          return fail(0x04);
        machine.recordDiskWrite(drive, lba, static_cast<uint8_t>(count));
      }
      return ok();
    }
    case 0x44: // extended verify sectors
    case 0x47: // extended seek
      return image ? ok() : fail(0x01);
    case 0x48: // get extended drive parameters
    {
      if (!image)
        return fail(0x01);

      uint32_t const buffer = linearAddress(PcI8086::DS(), PcI8086::SI());
      if (!machine.isRamRangeValid(buffer, 26))
        return fail(0x09);

      uint16_t const requestedSize = machine.readMemory16(buffer);
      if (requestedSize < 26)
        return fail(0x01);

      PcDiskImage::Geometry const geometry = image->geometry();
      uint64_t const totalSectors = image->sectorCount();
      machine.writeMemory16(buffer + 0, requestedSize >= 30 ? 30 : 26);
      machine.writeMemory16(buffer + 2, 0x0000); // no DMA boundary/errors flags
      machine.writeMemory16(buffer + 4, static_cast<uint16_t>(geometry.cylinders & 0xffff));
      machine.writeMemory16(buffer + 6, static_cast<uint16_t>((geometry.cylinders >> 16) & 0xffff));
      machine.writeMemory16(buffer + 8, static_cast<uint16_t>(geometry.heads & 0xffff));
      machine.writeMemory16(buffer + 10, static_cast<uint16_t>((geometry.heads >> 16) & 0xffff));
      machine.writeMemory16(buffer + 12, static_cast<uint16_t>(geometry.sectors & 0xffff));
      machine.writeMemory16(buffer + 14, static_cast<uint16_t>((geometry.sectors >> 16) & 0xffff));
      for (int i = 0; i < 4; ++i)
        machine.writeMemory16(buffer + 16 + i * 2, static_cast<uint16_t>((totalSectors >> (i * 16)) & 0xffff));
      machine.writeMemory16(buffer + 24, PcDiskImage::SectorSize);
      if (requestedSize >= 30) {
        machine.writeMemory16(buffer + 26, 0xffff);
        machine.writeMemory16(buffer + 28, 0xffff);
      }
      return ok();
    }
    case 0x08: // get drive parameters
      if (!image)
        return fail(0x01);
      PcI8086::setCH(static_cast<uint8_t>((image->geometry().cylinders - 1) & 0xff));
      PcI8086::setCL(static_cast<uint8_t>(image->geometry().sectors |
                                          (((image->geometry().cylinders - 1) >> 2) & 0xc0)));
      PcI8086::setDH(static_cast<uint8_t>(image->geometry().heads - 1));
      PcI8086::setDL(1);
      return ok();
    case 0x15: // get disk type
      if (!image)
        return fail(0x01);
      if (drive < 0x80) {
        PcI8086::setAH(0x01); // diskette, no change-line support
        PcI8086::setFlagCF(false);
        return true;
      }
      PcI8086::setAH(0x03); // fixed disk
      PcI8086::setCX(static_cast<uint16_t>((image->sectorCount() >> 16) & 0xffff));
      PcI8086::setDX(static_cast<uint16_t>(image->sectorCount() & 0xffff));
      PcI8086::setFlagCF(false);
      return true;
    default:
      return false;
  }
}

bool PcBios::handleKeyboardInterrupt(PcMachine & machine)
{
  uint8_t const function = PcI8086::AH();
  if (function == 0x01 || function == 0x11) { // check keystroke; do not consume it
    uint16_t key = 0;
    if (!peekBdaKey(machine, &key)) {
      if (fetchKey(machine, &key))
        storeBdaKey(machine, key);
    }
    bool const hasKey = peekBdaKey(machine, &key);
    PcI8086::setFlagZF(!hasKey);
    if (hasKey)
      PcI8086::setAX(key);
    return true;
  }
  if (function == 0x02 || function == 0x12) { // get shift flags / enhanced shift flags
    uint16_t queuedKey = 0;
    while (fetchKey(machine, &queuedKey))
      storeBdaKey(machine, queuedKey);
    updateKeyboardFlags(machine);
    if (function == 0x02)
      PcI8086::setAL(machine.readMemory8(0x0417));
    else
      PcI8086::setAX(static_cast<uint16_t>(machine.readMemory8(0x0418)) << 8 | machine.readMemory8(0x0417));
    return true;
  }
  if (function == 0x03) { // set typematic rate/delay; accept as a no-op
    PcI8086::setFlagCF(false);
    return true;
  }
  if (function == 0x05) { // store keystroke in BIOS keyboard buffer
    // Several text UIs probe the BIOS buffer path with CX=0000 while their
    // own menu loop is active. A literal NUL keystroke is not useful to DOS
    // applications and can leave menu bars drawn but apparently unresponsive.
    if (PcI8086::CX() != 0)
      storeBdaKey(machine, PcI8086::CX());
    PcI8086::setAL(0x00);
    PcI8086::setFlagCF(false);
    return true;
  }
  if (function == 0x55) { // keyboard extension/private probe
    PcI8086::setFlagCF(true);
    return true;
  }
  if (function != 0x00 && function != 0x10)
    return false;

  uint16_t key = 0;
  if (!popBdaKey(machine, &key)) {
    if (fetchKey(machine, &key))
      storeBdaKey(machine, key);
    if (!popBdaKey(machine, &key)) {
      // BIOS INT 16h AH=00/10 is blocking. Re-run the INT instruction until
      // the keyboard IRQ path or a later poll has placed a key in the BDA
      // buffer. Returning AX=0 makes menu loops consume a fake NUL key.
      PcI8086::setIP(static_cast<uint16_t>(PcI8086::IP() - 2));
      return true;
    }
  }

  PcI8086::setAX(key);
  PcI8086::setFlagZF(false);
  return true;
}

bool PcBios::handleSerialInterrupt()
{
  // Tab5 has no emulated PC COM device. Still implement the IBM BIOS serial
  // entry point so DOS boot scripts and hardware probes can conclude
  // "no/timeout" without tripping the unsupported-interrupt diagnostics.
  switch (PcI8086::AH()) {
    case 0x00: // initialize port
    case 0x01: // send character
    case 0x02: // receive character
    case 0x03: // get port status
      PcI8086::setAH(0x80); // timeout/no device
      PcI8086::setAL(0x00); // no modem-status bits
      return true;
    default:
      PcI8086::setAH(0x80);
      PcI8086::setAL(0x00);
      return true;
  }
}

bool PcBios::handleSystemInterrupt(PcMachine & machine)
{
  switch (PcI8086::AH()) {
    case 0x06: // legacy/private BIOS probes (for example Amstrad ROS); not present
    case 0x11: // legacy/system-extension probe; no extension installed
    case 0x41: // wait-on-external-event/system service probe; no event device installed
    case 0x64: // OEM/system-extension probe; no extension installed
      PcI8086::setAH(0x86);
      PcI8086::setFlagCF(true);
      return true;
    case 0x88: // get extended memory size in KiB above 1 MiB
      PcI8086::setAX(0x0000); // PcMachine exposes a 1 MiB real-mode address space only
      PcI8086::setFlagCF(false);
      return true;
    case 0x86: // wait CX:DX microseconds
    {
      uint16_t const cs = PcI8086::CS();
      uint16_t const ip = PcI8086::IP();
      uint16_t const sp = PcI8086::SP();
      uint64_t const now = monotonicMicroseconds();

      // Find the wait belonging to this exact call context, or start a new one.
      // Keying on {CS, IP, SP} keeps a wait re-entered from a nested interrupt
      // handler independent of the outer wait that it interrupted.
      int slot = -1;
      for (int i = 0; i < WaitSlots; ++i) {
        if (m_waits[i].active && m_waits[i].cs == cs && m_waits[i].ip == ip && m_waits[i].sp == sp) {
          slot = i;
          break;
        }
      }
      if (slot < 0) {
        uint64_t const micros = (static_cast<uint64_t>(PcI8086::CX()) << 16) | PcI8086::DX();
        for (int i = 0; i < WaitSlots; ++i) {
          if (!m_waits[i].active) {
            slot = i;
            break;
          }
        }
        if (slot < 0)
          slot = 0; // all slots busy (deeper nesting than expected): reuse slot 0
        m_waits[slot] = BiosWait{true, cs, ip, sp, now + micros};
      }

      if (now < m_waits[slot].targetMicros) {
        // Re-execute the INT 15h so the wait yields back to the emulator loop
        // (letting the timer IRQ and other tasks run) rather than spinning with
        // the machine mutex held for the whole delay.
        PcI8086::setIP(static_cast<uint16_t>(PcI8086::IP() - 2));
        return true;
      }
      m_waits[slot].active = false;
      PcI8086::setAH(0x00);
      PcI8086::setFlagCF(false); // CF clear: the wait completed
      return true;
    }
    case 0x24: // A20 gate services
      switch (PcI8086::AL()) {
        case 0x00: // disable A20
        case 0x01: // enable A20
          // TabDOS exposes only the 1 MiB real-mode aperture, so there is no
          // high-memory alias to switch. Report success to keep DOS memory
          // managers on the BIOS path without advertising usable XMS/HMA.
          PcI8086::setAH(0x00);
          PcI8086::setFlagCF(false);
          return true;
        case 0x02: // query A20 state
          PcI8086::setAX(0x0001); // AH=success, AL=A20 enabled/no wrap
          PcI8086::setFlagCF(false);
          return true;
        case 0x03: // query A20 support
          PcI8086::setAH(0x00);
          PcI8086::setBX(0x0003); // keyboard-controller and fast-A20 style paths are harmless no-ops
          PcI8086::setFlagCF(false);
          return true;
        default:
          PcI8086::setAH(0x86);
          PcI8086::setFlagCF(true);
          return true;
      }
    case 0xe8: // extended memory-size services (for example AX=E801h/E820h)
      if (PcI8086::AX() == 0xe801) {
        // Report no memory above the 1 MiB real-mode aperture. Returning a
        // successful zero-sized map keeps FreeDOS MEM-style probes on the
        // supported BIOS path without advertising inaccessible RAM.
        PcI8086::setAX(0x0000);
        PcI8086::setBX(0x0000);
        PcI8086::setCX(0x0000);
        PcI8086::setDX(0x0000);
        PcI8086::setFlagCF(false);
      } else if (PcI8086::AX() == 0xe820 && PcI8086::DX() == 0x4150 && PcI8086::CX() >= 20) {
        uint32_t const buffer = linearAddress(PcI8086::ES(), PcI8086::DI());
        if (!machine.isRamRangeValid(buffer, 20)) {
          PcI8086::setAH(0x86);
          PcI8086::setFlagCF(true);
          return true;
        }

        switch (PcI8086::BX()) {
          case 0x0000:
            writeE820Entry(machine, buffer, 0x00000000, 0x000a0000, 0x00000001); // usable conventional RAM
            PcI8086::setBX(0x0001);
            PcI8086::setCX(20);
            PcI8086::setAX(0x4150); // low word of SMAP signature
            PcI8086::setFlagCF(false);
            break;
          case 0x0001:
            writeE820Entry(machine, buffer, 0x000a0000, 0x00060000, 0x00000002); // VGA/BIOS reserved area
            PcI8086::setBX(0x0000);
            PcI8086::setCX(20);
            PcI8086::setAX(0x4150);
            PcI8086::setFlagCF(false);
            break;
          default:
            PcI8086::setAH(0x86);
            PcI8086::setFlagCF(true);
            break;
        }
      } else {
        PcI8086::setAH(0x86);
        PcI8086::setFlagCF(true);
      }
      return true;
    case 0xc0: // get system configuration
    {
      static constexpr uint32_t tableAddress = 0x000fe6f5;
      machine.writeMemory16(tableAddress + 0, 0x0008); // bytes following length
      machine.writeMemory8(tableAddress + 2, 0xfc);    // IBM PC/AT compatible
      machine.writeMemory8(tableAddress + 3, 0x00);    // submodel
      machine.writeMemory8(tableAddress + 4, 0x00);    // BIOS revision
      machine.writeMemory8(tableAddress + 5, 0x00);    // feature byte 1
      machine.writeMemory8(tableAddress + 6, 0x00);
      machine.writeMemory8(tableAddress + 7, 0x00);
      machine.writeMemory8(tableAddress + 8, 0x00);
      machine.writeMemory8(tableAddress + 9, 0x00);
      PcI8086::setES(0xf000);
      PcI8086::setBX(0xe6f5);
      PcI8086::setAH(0x00);
      PcI8086::setFlagCF(false);
      return true;
    }
    case 0xc1: // get EBDA segment; no separate EBDA is installed
      PcI8086::setAH(0x86);
      PcI8086::setFlagCF(true);
      return true;
    case 0xc2: // PS/2 pointing-device BIOS interface; INT 33h mouse is used instead
      PcI8086::setAH(0x86);
      PcI8086::setFlagCF(true);
      return true;
    case 0xd8: // system/bus extension probe; not implemented on TabDOS
      PcI8086::setAH(0x86);
      PcI8086::setFlagCF(true);
      return true;
    default:
      return false;
  }
}

bool PcBios::handleClockInterrupt()
{
  time_t now = time(nullptr);
  struct tm tmNow;
#if defined(_WIN32)
  tmNow = *localtime(&now);
#else
  localtime_r(&now, &tmNow);
#endif

  switch (PcI8086::AH()) {
    case 0x00: // get system timer ticks
    {
      uint64_t const absoluteTicks = absoluteTimerTicks();
      uint32_t const dayTicks = static_cast<uint32_t>(absoluteTicks % BiosTicksPerDay);
      PcI8086::setCX(static_cast<uint16_t>(dayTicks >> 16));
      PcI8086::setDX(static_cast<uint16_t>(dayTicks & 0xffff));
      PcI8086::setAL(static_cast<uint8_t>((absoluteTicks / BiosTicksPerDay) & 0xff));
      PcI8086::setFlagCF(false);
      return true;
    }
    case 0x01: // set system timer ticks
      m_timerBaseTicks = (static_cast<uint32_t>(PcI8086::CX()) << 16) | PcI8086::DX();
      m_timerBaseMillis = monotonicMilliseconds();
      PcI8086::setFlagCF(false);
      return true;
    case 0x02: // get RTC time
      PcI8086::setCH(toBcd(tmNow.tm_hour));
      PcI8086::setCL(toBcd(tmNow.tm_min));
      PcI8086::setDH(toBcd(tmNow.tm_sec));
      PcI8086::setDL(0);
      PcI8086::setFlagCF(false);
      return true;
    case 0x04: // get RTC date
    {
      int const year = tmNow.tm_year + 1900;
      PcI8086::setCH(toBcd(year / 100));
      PcI8086::setCL(toBcd(year % 100));
      PcI8086::setDH(toBcd(tmNow.tm_mon + 1));
      PcI8086::setDL(toBcd(tmNow.tm_mday));
      PcI8086::setFlagCF(false);
      return true;
    }
    case 0x03: // set RTC time
    case 0x05: // set RTC date
      PcI8086::setFlagCF(false);
      return true;
    case 0x35: // OEM/BIOS clock-extension probe; no extension installed
    case 0x36: // OEM/BIOS clock-extension probe; no extension installed
    case 0x60: // OEM/BIOS clock-extension probe; no extension installed
      PcI8086::setAH(0x86);
      PcI8086::setFlagCF(true);
      return true;
    default:
      return false;
  }
}

bool PcBios::handleMouseInterrupt(PcMachine & machine)
{
  switch (PcI8086::AX()) {
    case 0x0000: // reset driver and read installed flag
      m_mouseX = 0;
      m_mouseY = 0;
      m_mouseVisibleCount = 0;
      m_mouseButtons = 0;
      m_mouseMinX = 0;
      m_mouseMaxX = 639;
      m_mouseMinY = 0;
      m_mouseMaxY = 199;
      clearMouseEventState();
      loadDefaultGraphicsCursor();
      m_mouseExcludeActive = false;
      PcI8086::setAX(m_mouseInstalled ? 0xffff : 0x0000);
      PcI8086::setBX(m_mouseInstalled ? 0x0002 : 0x0000);
      return true;
    case 0x0001: // show cursor
      // A real Microsoft driver caps the internal show counter at the visible
      // state, so redundant show calls do not stack; a single hide then always
      // removes the pointer. Here visibility is m_mouseVisibleCount > 0, so cap
      // the counter at 1 instead of letting it grow without bound.
      if (m_mouseVisibleCount < 1)
        ++m_mouseVisibleCount;
      m_mouseExcludeActive = false; // showing the cursor clears any conditional-off region
      return true;
    case 0x0002: // hide cursor
      if (m_mouseVisibleCount > 0)
        --m_mouseVisibleCount;
      return true;
    case 0x0003: // get buttons and cursor position
      PcI8086::setBX(m_mouseInstalled ? m_mouseButtons : 0x0000);
      PcI8086::setCX(m_mouseX);
      PcI8086::setDX(m_mouseY);
      return true;
    case 0x0004: // set cursor position
      updateMouseState(PcI8086::CX(), PcI8086::DX(), m_mouseButtons, false);
      return true;
    case 0x0005: // get button press information
    {
      uint16_t const button = PcI8086::BX();
      uint16_t const index = button < 3 ? button : 0;
      PcI8086::setAX(m_mouseInstalled ? m_mouseButtons : 0x0000);
      PcI8086::setBX(m_mousePressCount[index]);
      PcI8086::setCX(m_mousePressX[index]);
      PcI8086::setDX(m_mousePressY[index]);
      m_mousePressCount[index] = 0;
      return true;
    }
    case 0x0006: // get button release information
    {
      uint16_t const button = PcI8086::BX();
      uint16_t const index = button < 3 ? button : 0;
      PcI8086::setAX(m_mouseInstalled ? m_mouseButtons : 0x0000);
      PcI8086::setBX(m_mouseReleaseCount[index]);
      PcI8086::setCX(m_mouseReleaseX[index]);
      PcI8086::setDX(m_mouseReleaseY[index]);
      m_mouseReleaseCount[index] = 0;
      return true;
    }
    case 0x0007: // set horizontal range
      m_mouseMinX = PcI8086::CX();
      m_mouseMaxX = PcI8086::DX();
      clampMousePosition();
      return true;
    case 0x0008: // set vertical range
      m_mouseMinY = PcI8086::CX();
      m_mouseMaxY = PcI8086::DX();
      clampMousePosition();
      return true;
    case 0x000b: // read motion counters
      PcI8086::setCX(static_cast<uint16_t>(m_mouseMotionX));
      PcI8086::setDX(static_cast<uint16_t>(m_mouseMotionY));
      m_mouseMotionX = 0;
      m_mouseMotionY = 0;
      return true;
    case 0x000c: // define interrupt subroutine and event mask
      m_mouseCallbackMask = PcI8086::CX();
      m_mouseCallbackSegment = PcI8086::ES();
      m_mouseCallbackOffset = PcI8086::DX();
      return true;
    case 0x0009: // define graphics cursor (BX/CX = hotspot, ES:DX = 16 words screen mask + 16 words cursor mask)
    {
      m_mouseHotspotX = static_cast<int16_t>(PcI8086::BX());
      m_mouseHotspotY = static_cast<int16_t>(PcI8086::CX());
      uint32_t const source = linearAddress(PcI8086::ES(), PcI8086::DX());
      if (machine.isRamRangeValid(source, 64)) {
        for (int i = 0; i < 16; ++i)
          m_mouseScreenMask[i] = machine.readMemory16(source + i * 2);
        for (int i = 0; i < 16; ++i)
          m_mouseCursorMask[i] = machine.readMemory16(source + 32 + i * 2);
        m_mouseGraphicsCursorDefined = true;
      }
      return true;
    }
    case 0x0010: // set conditional off (exclusion) region: CX,DX = upper-left, SI,DI = lower-right
      m_mouseExcludeLeft = PcI8086::CX();
      m_mouseExcludeTop = PcI8086::DX();
      m_mouseExcludeRight = PcI8086::SI();
      m_mouseExcludeBottom = PcI8086::DI();
      m_mouseExcludeActive = true;
      return true;
    case 0x000a: // define text cursor
    case 0x000f: // set mickey/pixel ratio
    case 0x0013: // set double-speed threshold
    case 0x001a: // set sensitivity
    case 0x001d: // set display page
      return true;
    case 0x0014: // exchange interrupt subroutines
    {
      uint16_t const oldMask = m_mouseCallbackMask;
      uint16_t const oldSegment = m_mouseCallbackSegment;
      uint16_t const oldOffset = m_mouseCallbackOffset;
      m_mouseCallbackMask = PcI8086::CX();
      m_mouseCallbackSegment = PcI8086::ES();
      m_mouseCallbackOffset = PcI8086::DX();
      PcI8086::setCX(oldMask);
      PcI8086::setES(oldSegment);
      PcI8086::setDX(oldOffset);
      return true;
    }
    case 0x0015: // get driver state storage requirements
      PcI8086::setBX(0x0000);
      return true;
    case 0x0016: // save driver state
    case 0x0017: // restore driver state
      return true;
    case 0x0018: // set alternate user handler
      return true;
    case 0x0019: // get alternate user handler
      PcI8086::setBX(0x0000);
      PcI8086::setCX(0x0000);
      PcI8086::setDX(0x0000);
      PcI8086::setES(0x0000);
      return true;
    case 0x001b: // get sensitivity
      PcI8086::setBX(8);
      PcI8086::setCX(16);
      PcI8086::setDX(64);
      return true;
    case 0x001e: // get display page
      PcI8086::setBX(0x0000);
      return true;
    case 0x0024: // get software version, type, and IRQ
      PcI8086::setBX(0x0800);
      PcI8086::setCH(0x04);
      PcI8086::setCL(0x00);
      return true;
    default:
      return true;
  }
}

uint8_t PcBios::toBcd(int value)
{
  return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

void PcBios::updateKeyboardFlags(PcMachine & machine)
{
  uint8_t flags = 0;
  if (m_rightShift)
    flags |= 0x01;
  if (m_leftShift)
    flags |= 0x02;
  if (m_ctrl)
    flags |= 0x04;
  if (m_alt)
    flags |= 0x08;
  if (m_scrollLock)
    flags |= 0x10;
  if (m_numLock)
    flags |= 0x20;
  if (m_capsLock)
    flags |= 0x40;
  if (m_insert)
    flags |= 0x80;
  machine.writeMemory8(0x0417, flags);

  uint8_t extendedFlags = 0;
  if (m_ctrl)
    extendedFlags |= 0x01; // left/control-active compatibility bit
  if (m_alt)
    extendedFlags |= 0x02; // left/alt-active compatibility bit
  if (m_scrollLock)
    extendedFlags |= 0x10;
  if (m_numLock)
    extendedFlags |= 0x20;
  if (m_capsLock)
    extendedFlags |= 0x40;
  if (m_insert)
    extendedFlags |= 0x80;
  machine.writeMemory8(0x0418, extendedFlags);
}

bool PcBios::translateKeyboardScancode(PcMachine & machine, uint8_t raw, uint16_t * key)
{
  if (raw == 0xe0)
    return false;

  bool const released = (raw & 0x80) != 0;
  uint8_t const scan = raw & 0x7f;
  switch (scan) {
    case 0x2a: m_leftShift = !released; updateKeyboardFlags(machine); return false;
    case 0x36: m_rightShift = !released; updateKeyboardFlags(machine); return false;
    case 0x1d: m_ctrl = !released; updateKeyboardFlags(machine); return false;
    case 0x38: m_alt = !released; updateKeyboardFlags(machine); return false;
    case 0x3a:
      if (!released)
        m_capsLock = !m_capsLock;
      updateKeyboardFlags(machine);
      return false;
    case 0x45:
      if (!released)
        m_numLock = !m_numLock;
      updateKeyboardFlags(machine);
      return false;
    case 0x46:
      if (!released)
        m_scrollLock = !m_scrollLock;
      updateKeyboardFlags(machine);
      return false;
    case 0x52:
      if (!released)
        m_insert = !m_insert;
      updateKeyboardFlags(machine);
      break;
    default: break;
  }
  if (released) {
    updateKeyboardFlags(machine);
    return false;
  }

  uint8_t ascii = asciiForScancode(scan, m_leftShift || m_rightShift);
  if (m_alt && ((ascii >= 'a' && ascii <= 'z') || (ascii >= 'A' && ascii <= 'Z')))
    ascii = 0;
  else if (m_ctrl && ascii >= 'a' && ascii <= 'z')
    ascii = ascii - 'a' + 1;
  else if (m_ctrl && ascii >= 'A' && ascii <= 'Z')
    ascii = ascii - 'A' + 1;

  if (key)
    *key = static_cast<uint16_t>(scan) << 8 | ascii;
  return true;
}

bool PcBios::fetchKey(PcMachine & machine, uint16_t * key)
{
  while (machine.keyboard().readStatusPort() & PcKeyboardController::StatusOutputBufferFull) {
    uint8_t const raw = machine.keyboard().readDataPort();
    if (translateKeyboardScancode(machine, raw, key))
      return true;
  }
  return false;
}

bool PcBios::peekBdaKey(PcMachine & machine, uint16_t * key)
{
  // Resolve any deferred shadow key at the moment it is about to be consumed,
  // now that the guest INT 09h handler has had its chance to fill the BDA.
  flushShadowKey(machine);
  uint16_t head = machine.readMemory16(0x041a);
  uint16_t tail = machine.readMemory16(0x041c);
  if (head < 0x001e || head >= 0x003e) {
    head = 0x001e;
    machine.writeMemory16(0x041a, head);
  }
  if (tail < 0x001e || tail >= 0x003e) {
    tail = 0x001e;
    machine.writeMemory16(0x041c, tail);
  }
  if (head == tail)
    return false;

  if (key)
    *key = machine.readMemory16(0x0400 + head);
  return true;
}

bool PcBios::popBdaKey(PcMachine & machine, uint16_t * key)
{
  uint16_t head = machine.readMemory16(0x041a);
  if (!peekBdaKey(machine, key))
    return false;

  head += 2;
  if (head >= 0x003e)
    head = 0x001e;
  machine.writeMemory16(0x041a, head);
  return true;
}

void PcBios::storeBdaKey(PcMachine & machine, uint16_t key)
{
  if (key == 0)
    return;

  uint16_t head = machine.readMemory16(0x041a);
  uint16_t tail = machine.readMemory16(0x041c);
  if (head < 0x001e || head >= 0x003e) {
    head = 0x001e;
    machine.writeMemory16(0x041a, head);
  }
  if (tail < 0x001e || tail >= 0x003e)
    tail = 0x001e;

  uint16_t nextTail = tail + 2;
  if (nextTail >= 0x003e)
    nextTail = 0x001e;
  if (nextTail == head)
    return;

  machine.writeMemory16(0x0400 + tail, key);
  machine.writeMemory16(0x041c, nextTail);
}

void PcBios::clearTextScreen(PcMachine & machine, uint8_t attribute)
{
  scrollTextWindow(machine, 0, attribute, 0, 0, activeTextRows(machine) - 1, static_cast<uint8_t>(activeTextColumns(machine) - 1), false);
  setCursor(machine, activeDisplayPage(machine), 0, 0);
}

void PcBios::scrollTextWindow(PcMachine & machine,
                              uint8_t lines,
                              uint8_t attribute,
                              uint8_t top,
                              uint8_t left,
                              uint8_t bottom,
                              uint8_t right,
                              bool down)
{
  uint8_t const rows = activeTextRows(machine);
  if (top >= rows)
    top = rows - 1;
  if (bottom >= rows)
    bottom = rows - 1;
  uint16_t const columns = activeTextColumns(machine);
  if (left >= columns)
    left = static_cast<uint8_t>(columns - 1);
  if (right >= columns)
    right = static_cast<uint8_t>(columns - 1);
  if (top > bottom || left > right)
    return;

  if (machine.isGraphicsMode()) {
    scrollGraphicsWindow(machine, lines, attribute, top, left, bottom, right, down);
    return;
  }

  uint32_t const base = activeTextMemoryBase(machine);
  uint8_t const height = static_cast<uint8_t>(bottom - top + 1);
  if (lines == 0 || lines >= height) {
    for (uint8_t row = top; row <= bottom; ++row) {
      for (uint8_t col = left; col <= right; ++col) {
        uint32_t const address = base + 2 * (row * columns + col);
        machine.writeVideoMemory16(address, static_cast<uint16_t>(attribute) << 8 | ' ');
      }
    }
    return;
  }

  if (down) {
    for (int row = bottom; row >= static_cast<int>(top) + lines; --row) {
      for (uint8_t col = left; col <= right; ++col) {
        uint32_t const from = base + 2 * ((row - lines) * columns + col);
        uint32_t const to = base + 2 * (row * columns + col);
        machine.writeVideoMemory16(to, machine.readVideoMemory16(from));
      }
    }
    for (uint8_t row = top; row < top + lines; ++row) {
      for (uint8_t col = left; col <= right; ++col) {
        uint32_t const address = base + 2 * (row * columns + col);
        machine.writeVideoMemory16(address, static_cast<uint16_t>(attribute) << 8 | ' ');
      }
    }
  } else {
    for (uint8_t row = top; row + lines <= bottom; ++row) {
      for (uint8_t col = left; col <= right; ++col) {
        uint32_t const from = base + 2 * ((row + lines) * columns + col);
        uint32_t const to = base + 2 * (row * columns + col);
        machine.writeVideoMemory16(to, machine.readVideoMemory16(from));
      }
    }
    for (uint8_t row = bottom - lines + 1; row <= bottom; ++row) {
      for (uint8_t col = left; col <= right; ++col) {
        uint32_t const address = base + 2 * (row * columns + col);
        machine.writeVideoMemory16(address, static_cast<uint16_t>(attribute) << 8 | ' ');
      }
    }
  }
}

void PcBios::scrollGraphicsWindow(PcMachine & machine,
                                  uint8_t lines,
                                  uint8_t color,
                                  uint8_t top,
                                  uint8_t left,
                                  uint8_t bottom,
                                  uint8_t right,
                                  bool down)
{
  uint8_t const cellHeight = graphicsTextCellHeight(machine);
  int const x0 = static_cast<int>(left) * 8;
  int const y0 = static_cast<int>(top) * cellHeight;
  int x1 = (static_cast<int>(right) + 1) * 8 - 1;
  int y1 = (static_cast<int>(bottom) + 1) * cellHeight - 1;
  int const width = machine.graphicsWidth();
  int const heightPixels = machine.graphicsHeight();
  if (width <= 0 || heightPixels <= 0 || x0 >= width || y0 >= heightPixels)
    return;
  if (x1 >= width)
    x1 = width - 1;
  if (y1 >= heightPixels)
    y1 = heightPixels - 1;
  if (x0 > x1 || y0 > y1)
    return;

  uint8_t const windowRows = static_cast<uint8_t>(bottom - top + 1);
  uint16_t const pageOffset = graphicsPageOffset(machine, activeDisplayPage(machine));
  auto fillRect = [&](int fillY0, int fillY1) {
    if (fillY0 < y0)
      fillY0 = y0;
    if (fillY1 > y1)
      fillY1 = y1;
    for (int y = fillY0; y <= fillY1; ++y) {
      for (int x = x0; x <= x1; ++x)
        machine.writeGraphicsPixel(x, y, color, false, pageOffset);
    }
  };

  if (lines == 0 || lines >= windowRows) {
    fillRect(y0, y1);
    return;
  }

  int const pixelLines = static_cast<int>(lines) * cellHeight;
  if (pixelLines <= 0 || pixelLines > y1 - y0 + 1) {
    fillRect(y0, y1);
    return;
  }

  if (down) {
    for (int y = y1; y >= y0 + pixelLines; --y) {
      for (int x = x0; x <= x1; ++x) {
        uint8_t sourceColor = 0;
        machine.readGraphicsPixel(x, y - pixelLines, &sourceColor, pageOffset);
        machine.writeGraphicsPixel(x, y, sourceColor, false, pageOffset);
      }
    }
    fillRect(y0, y0 + pixelLines - 1);
  } else {
    for (int y = y0; y + pixelLines <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) {
        uint8_t sourceColor = 0;
        machine.readGraphicsPixel(x, y + pixelLines, &sourceColor, pageOffset);
        machine.writeGraphicsPixel(x, y, sourceColor, false, pageOffset);
      }
    }
    fillRect(y1 - pixelLines + 1, y1);
  }
}

void PcBios::scrollTextUp(PcMachine & machine, uint8_t attribute)
{
  scrollTextWindow(machine, 1, attribute, 0, 0, activeTextRows(machine) - 1, static_cast<uint8_t>(activeTextColumns(machine) - 1), false);
  if (m_cursorRow > 0)
    --m_cursorRow;
  setCursor(machine, activeDisplayPage(machine), m_cursorRow, m_cursorColumn);
}

uint16_t PcBios::graphicsPageOffset(PcMachine const & machine, uint8_t page) const
{
  if (!machine.isGraphicsMode())
    return 0;
  uint16_t const pageSize = machine.readMemory16(0x044c);
  if (pageSize == 0)
    return 0;
  return static_cast<uint16_t>(static_cast<uint32_t>(page) * pageSize);
}

uint8_t PcBios::graphicsTextCellHeight(PcMachine const & machine) const
{
  switch (machine.readMemory8(0x0449) & 0x7f) {
    case 0x04:
    case 0x05:
    case 0x06:
    case 0x0d:
    case 0x0e:
    case 0x13:
      return 8;
    case 0x0f:
    case 0x10:
      return 14;
    case 0x11:
    case 0x12:
      return 16;
    default:
      return 8;
  }
}

void PcBios::writeGraphicsCharacter(PcMachine & machine, uint8_t ch, uint8_t color, uint8_t row, uint8_t column, uint8_t page, bool xorMode)
{
  int const x0 = static_cast<int>(column) * 8;
  int const y0 = static_cast<int>(row) * graphicsTextCellHeight(machine);
  uint8_t const cellHeight = graphicsTextCellHeight(machine);
  uint16_t const pageOffset = graphicsPageOffset(machine, page);
  for (uint8_t y = 0; y < cellHeight; ++y) {
    uint8_t const sourceRow = static_cast<uint8_t>(y * PcTextRenderer::CellHeight / cellHeight);
    uint8_t const bits = m_userFont[static_cast<uint16_t>(ch) * PcTextRenderer::CellHeight + sourceRow];
    for (uint8_t x = 0; x < 8; ++x) {
      if (bits & (0x80 >> x))
        machine.writeGraphicsPixel(x0 + x, y0 + y, color, xorMode, pageOffset);
    }
  }
}

void PcBios::writeTeletype(PcMachine & machine, uint8_t ch, uint8_t attribute)
{
  uint8_t const rows = activeTextRows(machine);
  if (ch == '\r') {
    m_cursorColumn = 0;
  } else if (ch == '\n') {
    ++m_cursorRow;
  } else if (ch == '\b') {
    if (m_cursorColumn > 0)
      --m_cursorColumn;
  } else {
    uint16_t const columns = activeTextColumns(machine);
    if (machine.isGraphicsMode()) {
      bool const xorMode = (attribute & 0x80) != 0 && machine.videoMode() != PcMachine::VideoMode::VgaGraphics320x200x256;
      writeGraphicsCharacter(machine, ch, attribute & (xorMode ? 0x7f : 0xff), m_cursorRow, m_cursorColumn, activeDisplayPage(machine), xorMode);
    } else {
      machine.writeVideoMemory16(activeTextMemoryBase(machine) + cursorOffset(machine),
                                 static_cast<uint16_t>(attribute) << 8 | ch);
    }
    ++m_cursorColumn;
    if (m_cursorColumn >= columns) {
      m_cursorColumn = 0;
      ++m_cursorRow;
    }
  }

  if (m_cursorRow >= rows) {
    scrollTextUp(machine, attribute);
    m_cursorRow = rows - 1;
  }
  setCursor(machine, activeDisplayPage(machine), m_cursorRow, m_cursorColumn);
}

uint8_t PcBios::activeDisplayPage(PcMachine const & machine) const
{
  uint8_t const page = machine.readMemory8(0x0462);
  return page < 8 ? page : m_activeDisplayPage;
}

uint32_t PcBios::textPageMemoryBase(PcMachine const & machine, uint8_t page) const
{
  if (page >= 8)
    page = activeDisplayPage(machine);
  uint16_t const pageSize = machine.readMemory16(0x044c) ? machine.readMemory16(0x044c) : 0x1000;
  uint32_t const textBase = (machine.readMemory8(0x0449) == 0x07 || machine.readMemory16(0x0463) == 0x03b4)
                              ? PcMachine::HerculesGraphicsMemoryBase
                              : PcMachine::TextColorMemoryBase;
  return textBase + static_cast<uint32_t>(page) * pageSize;
}

uint32_t PcBios::activeTextMemoryBase(PcMachine const & machine) const
{
  return textPageMemoryBase(machine, activeDisplayPage(machine));
}

uint16_t PcBios::activeTextColumns(PcMachine const & machine) const
{
  uint16_t columns = machine.readMemory16(0x044a);
  return columns == 40 ? 40 : 80;
}

uint8_t PcBios::activeTextRows(PcMachine const & machine) const
{
  if (machine.isGraphicsMode()) {
    int const cellHeight = graphicsTextCellHeight(machine);
    int rows = cellHeight > 0 ? machine.graphicsHeight() / cellHeight : PcTextRenderer::Rows;
    if (rows < 1)
      rows = 1;
    if (rows > PcTextRenderer::MaxRows)
      rows = PcTextRenderer::MaxRows;
    return static_cast<uint8_t>(rows);
  }

  uint8_t rows = static_cast<uint8_t>(machine.readMemory8(0x0484) + 1);
  if (rows < 1 || rows > PcTextRenderer::MaxRows)
    rows = PcTextRenderer::Rows;
  return rows;
}

void PcBios::setCursor(PcMachine & machine, uint8_t page, uint8_t row, uint8_t column)
{
  if (page >= 8)
    page = activeDisplayPage(machine);
  uint8_t const rows = activeTextRows(machine);
  if (row >= rows)
    row = rows - 1;
  uint16_t const columns = activeTextColumns(machine);
  if (column >= columns)
    column = static_cast<uint8_t>(columns - 1);

  machine.writeMemory16(0x0450 + page * 2, static_cast<uint16_t>(row) << 8 | column);
  if (page == activeDisplayPage(machine)) {
    m_activeDisplayPage = page;
    m_cursorRow = row;
    m_cursorColumn = column;
    machine.textRenderer().setCursor(row, column, true);
  }
}

int PcBios::diskIndexForBiosDrive(uint8_t drive) const
{
  if (drive < 0x80)
    return drive;
  return 2 + (drive - 0x80);
}

uint16_t PcBios::cursorOffset(PcMachine const & machine) const
{
  return 2 * (m_cursorRow * activeTextColumns(machine) + m_cursorColumn);
}

uint16_t PcBios::cursorOffsetForPage(PcMachine const & machine, uint8_t page) const
{
  if (page >= 8)
    return cursorOffset(machine);

  uint16_t const packedCursor = machine.readMemory16(0x0450 + page * 2);
  uint8_t row = static_cast<uint8_t>(packedCursor >> 8);
  uint8_t column = static_cast<uint8_t>(packedCursor & 0x00ff);
  uint8_t const rows = activeTextRows(machine);
  uint16_t const columns = activeTextColumns(machine);
  if (row >= rows)
    row = rows - 1;
  if (column >= columns)
    column = static_cast<uint8_t>(columns - 1);
  return static_cast<uint16_t>(2 * (row * columns + column));
}

} // namespace tabdos
