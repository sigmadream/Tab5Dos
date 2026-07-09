#include "pc_machine.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <chrono>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

namespace tabdos {

extern const uint8_t PcFont8x16Data[4096];

namespace {

constexpr uint32_t PitInputHz = 1193182; // 8253/8254 input clock

uint64_t monotonicMicroseconds()
{
  using clock = std::chrono::steady_clock;
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(clock::now().time_since_epoch()).count());
}

constexpr uint16_t BiosKeyboardIrqSegment = 0xf000;
constexpr uint16_t BiosKeyboardIrqOffset = 0xf100;
constexpr uint8_t BiosKeyboardInternalInterrupt = 0x79;
constexpr uint16_t BiosFont8x8Segment = 0xf000;
constexpr uint16_t BiosFont8x8Offset = 0xc800;
constexpr uint32_t BiosFont8x8Linear = (static_cast<uint32_t>(BiosFont8x8Segment) << 4) + BiosFont8x8Offset;
constexpr uint16_t BiosFont8x14Segment = 0xf000;
constexpr uint16_t BiosFont8x14Offset = 0xd000;
constexpr uint32_t BiosFont8x14Linear = (static_cast<uint32_t>(BiosFont8x14Segment) << 4) + BiosFont8x14Offset;
constexpr uint16_t BiosFont8x16Segment = 0xf000;
constexpr uint16_t BiosFont8x16Offset = 0xe000;
constexpr uint32_t BiosFont8x16Linear = (static_cast<uint32_t>(BiosFont8x16Segment) << 4) + BiosFont8x16Offset;
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

static bool isBiosKeyboardVector(uint16_t offset, uint16_t segment)
{
  return offset == BiosKeyboardIrqOffset && segment == BiosKeyboardIrqSegment;
}

static uint8_t * allocateMachineMemory(size_t size)
{
#if defined(ESP_PLATFORM)
  uint8_t * memory = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!memory)
    memory = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
  return memory;
#else
  return static_cast<uint8_t *>(malloc(size));
#endif
}

static void freeMachineMemory(void * memory)
{
#if defined(ESP_PLATFORM)
  heap_caps_free(memory);
#else
  free(memory);
#endif
}

static uint16_t rgb565From6Bit(uint8_t r, uint8_t g, uint8_t b)
{
  uint8_t const r8 = static_cast<uint8_t>((static_cast<unsigned>(r & 0x3f) * 255 + 31) / 63);
  uint8_t const g8 = static_cast<uint8_t>((static_cast<unsigned>(g & 0x3f) * 255 + 31) / 63);
  uint8_t const b8 = static_cast<uint8_t>((static_cast<unsigned>(b & 0x3f) * 255 + 31) / 63);
  return static_cast<uint16_t>(((r8 & 0xf8) << 8) | ((g8 & 0xfc) << 3) | (b8 >> 3));
}

static void writeBiosFontImage(PcMachine & machine, uint32_t linearAddress, uint8_t cellHeight)
{
  if (cellHeight != 8 && cellHeight != 14 && cellHeight != 16)
    return;
  if (!machine.isRamRangeValid(linearAddress, static_cast<size_t>(cellHeight) * 256))
    return;

  for (uint16_t ch = 0; ch < 256; ++ch) {
    for (uint8_t row = 0; row < cellHeight; ++row) {
      uint8_t const sourceRow = static_cast<uint8_t>(row * PcTextRenderer::CellHeight / cellHeight);
      machine.writeMemory8(linearAddress + static_cast<uint32_t>(ch) * cellHeight + row,
                           PcFont8x16Data[ch * PcTextRenderer::CellHeight + sourceRow]);
    }
  }
}

static uint8_t normalizeVgaAttributeRegister(uint8_t index, uint8_t value)
{
  index &= 0x1f;
  if (index <= 0x0f || index == 0x11)
    return value & 0x3f; // VGA AC palette and overscan registers are six-bit DAC selectors.
  if (index == 0x10)
    return value & 0xef; // mode-control bit 4 is reserved on VGA.
  if (index == 0x12)
    return value & 0x3f; // color-plane enable plus status mux bits.
  if (index == 0x13 || index == 0x14)
    return value & 0x0f; // horizontal PEL panning / color select.
  return value;
}

static uint8_t normalizeVgaSequencerRegister(uint8_t index, uint8_t value)
{
  switch (index & 0x07) {
    case 0x00: return value & 0x03; // reset
    case 0x01: return value & 0x3d; // clocking mode: 8/9-dot, load/dot clocks, shift-4, screen off
    case 0x02: return value & 0x0f; // map mask
    case 0x03: return value & 0x3f; // character map select
    case 0x04: return value & 0x0f; // memory mode
    default: return value;
  }
}

static uint8_t normalizeVgaGraphicsRegister(uint8_t index, uint8_t value)
{
  switch (index & 0x0f) {
    case 0x00: // set/reset
    case 0x01: // enable set/reset
    case 0x02: // color compare
    case 0x07: // color don't care
      return value & 0x0f;
    case 0x03: return value & 0x1f; // rotate count + logical operation
    case 0x04: return value & 0x03; // read map select
    case 0x05: return value & 0x7b; // write/read mode, odd/even, and shift mode bits
    case 0x06: return value & 0x0f; // misc graphics / memory map
    default: return value;
  }
}

static uint8_t normalizeVgaCrtcRegister(uint8_t index, uint8_t value)
{
  switch (index & 0x1f) {
    case 0x08: return value & 0x7f; // preset row scan + byte panning; bit 7 is reserved
    case 0x0a: return value & 0x3f; // cursor start/disable; bits 6-7 are reserved
    case 0x0b: return value & 0x7f; // cursor end/skew; bit 7 is reserved
    case 0x14: return value & 0x7f; // underline/count-by-4/dword addressing; bit 7 is reserved
    default: return value;
  }
}

static uint8_t normalizeVgaFeatureControl(uint8_t value)
{
  return value & 0x08; // VGA Feature Control exposes only Vertical Sync Select.
}

static uint8_t nextColorDisplayStatusBits(uint8_t & phase)
{
  // CGA/VGA input status register 1 (3DAh/3BAh) retrace bits, derived from
  // wall-clock time so that guest loops which poll bit 3 to synchronize to the
  // vertical retrace (palette fades, page flips) see a stable ~60 Hz cadence
  // instead of a value that only advances when the port is read.
  constexpr uint64_t kFrameMicros = 16667;      // ~60 Hz refresh
  constexpr uint64_t kVerticalRetraceMicros = 1400; // trailing VBlank window
  constexpr uint64_t kScanlineMicros = 32;      // ~31.77 us per scanline
  constexpr uint64_t kHorizontalBlankMicros = 8; // trailing HBlank per line

  uint64_t const now = monotonicMicroseconds();
  uint64_t const frameOffset = now % kFrameMicros;
  bool const verticalRetrace = frameOffset >= (kFrameMicros - kVerticalRetraceMicros);
  uint64_t const lineOffset = frameOffset % kScanlineMicros;
  bool const horizontalBlank = lineOffset >= (kScanlineMicros - kHorizontalBlankMicros);

  phase = static_cast<uint8_t>((phase + 1) & 0x03); // keep toggling for legacy probes
  uint8_t bits = 0;
  if (verticalRetrace || horizontalBlank)
    bits |= 0x01; // display-enable off (in blanking): safe to touch video RAM
  if (verticalRetrace)
    bits |= 0x08; // vertical retrace active
  return bits;
}

static uint16_t vgaVerticalDisplayEndForMode(PcMachine::VideoMode mode)
{
  switch (mode) {
    case PcMachine::VideoMode::VgaGraphics640x350x2:
    case PcMachine::VideoMode::VgaGraphics640x350x16:
      return PcMachine::VgaGraphics16MediumHeight - 1;
    case PcMachine::VideoMode::VgaGraphics640x480x2:
    case PcMachine::VideoMode::VgaGraphics640x480x16:
      return PcMachine::VgaGraphics16Height - 1;
    default:
      return PcMachine::VgaGraphics256Height - 1;
  }
}

static uint8_t toBcd(int value)
{
  if (value < 0)
    value = 0;
  if (value > 99)
    value %= 100;
  return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

static uint8_t cmosRegisterValue(uint8_t index)
{
  time_t now = time(nullptr);
  struct tm tmNow;
#if defined(_WIN32)
  tmNow = *localtime(&now);
#else
  localtime_r(&now, &tmNow);
#endif

  switch (index & 0x7f) {
    case 0x00: return toBcd(tmNow.tm_sec);
    case 0x02: return toBcd(tmNow.tm_min);
    case 0x04: return toBcd(tmNow.tm_hour);
    case 0x06: return toBcd(tmNow.tm_wday == 0 ? 7 : tmNow.tm_wday);
    case 0x07: return toBcd(tmNow.tm_mday);
    case 0x08: return toBcd(tmNow.tm_mon + 1);
    case 0x09: return toBcd((tmNow.tm_year + 1900) % 100);
    case 0x0a: return 0x26; // 32.768 kHz divider, periodic rate; update not in progress
    case 0x0b: return 0x02; // 24-hour, BCD mode
    case 0x0c: return 0x00; // no pending RTC interrupt
    case 0x0d: return 0x80; // CMOS battery/status valid
    case 0x14: return 0x2d; // equipment byte: FPU absent, keyboard/display/floppy present-ish
    case 0x15: return 0x80; // base memory low byte: 640 KB
    case 0x16: return 0x02; // base memory high byte
    default: return 0x00;
  }
}

static bool isAbsentLegacyProbePort(uint16_t port)
{
  switch (port) {
    case 0x0201: // PC game/joystick adapter
    case 0x008a: // DMA/page-register era probe port; absent on TabDOS
    case 0x03cd: // SVGA bank/extension probe; absent on TabDOS
    case 0x56e0:
    case 0x56e1:
    case 0x92e8:
    case 0x92e9:
    case 0xaae0:
    case 0xaae1:
    case 0xe2e0:
    case 0xe2e1:
    case 0x1ee0:
    case 0x1ee1:
      return true;
    default:
      break;
  }
  if (port >= 0x001c && port <= 0x001f)
    return true;
  if (port >= 0x0230 && port <= 0x023f)
    return true;
  if (port >= 0x0a20 && port <= 0x0a2f)
    return true;
  if ((port >= 0x02e8 && port <= 0x02ef) ||
      (port >= 0x02f0 && port <= 0x02f7) ||
      (port >= 0x02f8 && port <= 0x02ff) ||
      (port >= 0x03e8 && port <= 0x03ef) ||
      (port >= 0x03f8 && port <= 0x03ff))
    return true;
  return port >= 0x2110 && port <= 0x21f1 && (port & 0x000e) == 0x0000;
}

static bool isLegacyPrinterPort(uint16_t port)
{
  return (port >= 0x03bc && port <= 0x03be) ||
         (port >= 0x0378 && port <= 0x037a) ||
         (port >= 0x0278 && port <= 0x027a);
}

static uint8_t legacyPrinterPortRead(uint16_t port)
{
  switch (port & 0x0003) {
    case 0x00: return 0xff; // data bus floats high when no LPT device is attached
    case 0x01: return 0xff; // status: not busy / select-like bits high for probe loops
    case 0x02: return 0x00; // control register defaults low
    default: return 0xff;
  }
}

} // namespace

PcMachine::PcMachine()
  : m_ram(nullptr),
    m_videoMemory(nullptr),
    m_vgaPlaneMemory(nullptr),
    m_emsPool(nullptr),
    m_emsPageOwner(),
    m_emsHandleActive(),
    m_emsHandlePageCount(),
    m_emsPhysMapHandle(),
    m_emsPhysMapLogical(),
    m_emsPhysMapPoolPage(),
    m_emsSaved(),
    m_emsSavedHandle(),
    m_emsSavedLogical(),
    m_keyboard(),
    m_bios(),
    m_textRenderer(),
    m_disks(),
    m_bootState(),
    m_diagnostics(),
    m_videoMode(VideoMode::Text80),
    m_keyboardIrqInService(false),
    m_picMask(0),
    m_picSlaveMask(0),
    m_port61(0),
    m_floppyDigitalOutputRegister(0),
    m_cmosIndex(0),
    m_pit(),
    m_pitChannel0NextIrqMicros(0),
    m_timerUpdateCountdown(0),
    m_herculesCrtcIndex(0),
    m_herculesCrtcRegisters(),
    m_herculesConfigRegister(0),
    m_herculesModeControl(0),
    m_herculesStatus(0),
    m_herculesVideoEnabled(false)
{
}

PcMachine::~PcMachine()
{
  freeMachineMemory(m_ram);
  freeMachineMemory(m_videoMemory);
  freeMachineMemory(m_vgaPlaneMemory);
  freeMachineMemory(m_emsPool);
}

bool PcMachine::init()
{
  if (!m_ram)
    m_ram = allocateMachineMemory(RamSize);
  if (!m_videoMemory)
    m_videoMemory = allocateMachineMemory(VideoMemorySize);
  if (!m_vgaPlaneMemory)
    m_vgaPlaneMemory = allocateMachineMemory(4 * 64 * 1024);
  if (!m_ram || !m_videoMemory || !m_vgaPlaneMemory)
    return false;

  // EMS is optional: if the pool cannot be allocated the machine still runs,
  // just without expanded memory (emsAvailable() stays false).
  if (!m_emsPool)
    m_emsPool = allocateMachineMemory(static_cast<size_t>(EmsTotalPages) * EmsLogicalPageSize);

  reset();
  return true;
}

void PcMachine::reset()
{
  if (m_ram)
    memset(m_ram, 0, RamSize);
  if (m_videoMemory)
    memset(m_videoMemory, 0, VideoMemorySize);
  if (m_vgaPlaneMemory)
    memset(m_vgaPlaneMemory, 0, 4 * 64 * 1024);
  m_keyboard.reset();
  m_bios.reset();
  m_textRenderer.setFont(PcTextRenderer::defaultFont());
  m_textRenderer.setColumns(PcTextRenderer::Columns);
  m_textRenderer.setCellHeight(PcTextRenderer::CellHeight);
  m_textRenderer.setCursor(0, 0, false);
  m_textRenderer.setFrameCounter(0);
  m_bootState = {};
  m_videoMode = VideoMode::Text80;
  m_keyboardIrqInService = false;
  m_picMask = 0;
  m_picSlaveMask = 0;
  m_port61 = 0;
  m_floppyDigitalOutputRegister = 0;
  m_cmosIndex = 0;
  pitReset();
  emsReset();
  m_opl2.reset();
  m_timerUpdateCountdown = 0;
  resetVideoState();
  loadDefaultTextFontPlane();
  updateTextRendererFontMap();
  resetDiagnostics();
}

void PcMachine::resetVideoState()
{
  m_colorCrtcIndex = 0;
  memset(m_colorCrtcRegisters, 0, sizeof(m_colorCrtcRegisters));
  m_colorCrtcRegisters[0x13] = 40; // VGA/CGA offset register: 80 bytes per scanline by default
  m_colorCrtcRegisters[0x0a] = 0x06; // default text cursor start scan line
  m_colorCrtcRegisters[0x0b] = 0x07; // default text cursor end scan line
  m_colorCrtcRegisters[0x18] = 0xff; // line compare low bits; default disables split screen
  m_colorCrtcRegisters[0x07] = 0x10; // line compare bit 8
  m_colorCrtcRegisters[0x09] = 0x40; // line compare bit 9
  m_colorCrtcRegisters[0x14] = 0x1f; // default underline is below the visible VGA text cell
  m_colorCrtcRegisters[0x17] = 0x80; // CRTC mode-control reset bit set: display logic enabled
  m_cgaModeControl = 0x29; // 80-column text, video enabled, blink enabled
  m_cgaColorSelect = 0;
  m_miscOutputRegister = 0x63;
  m_featureControlRegister = 0;
  m_vgaEnableRegister = 1;
  m_attributeIndex = 0;
  memset(m_attributeRegisters, 0, sizeof(m_attributeRegisters));
  for (int i = 0; i < 16; ++i)
    m_attributeRegisters[i] = static_cast<uint8_t>(i);
  m_attributeRegisters[0x10] = 0x0c; // text mode: line graphics + blink enabled
  m_attributeRegisters[0x12] = 0x0f; // color plane enable: all four output bits enabled
  m_attributeRegisters[0x13] = 0x08; // 9-dot text: raw 8 means no horizontal PEL panning
  m_textRenderer.setBlinkEnabled(true);
  m_attributeFlipFlop = false;
  m_attributeVideoEnabled = true;
  m_sequencerIndex = 0;
  memset(m_sequencerRegisters, 0, sizeof(m_sequencerRegisters));
  m_sequencerRegisters[0x00] = 0x03; // sequencer out of async/sync reset
  m_sequencerRegisters[0x02] = 0x0f; // map mask: all planes writable
  m_graphicsIndex = 0;
  memset(m_graphicsRegisters, 0, sizeof(m_graphicsRegisters));
  m_graphicsRegisters[0x06] = 0x05; // graphics mode, A000 window
  m_graphicsRegisters[0x07] = 0x0f; // color don't-care: all planes participate in read mode 1
  m_graphicsRegisters[0x08] = 0xff; // bit mask
  memset(m_vgaLatches, 0, sizeof(m_vgaLatches));
  m_dacReadIndex = 0;
  m_dacWriteIndex = 0;
  m_dacReadComponent = 0;
  m_dacWriteComponent = 0;
  m_dacPelMask = 0xff;
  m_dacPelMaskReadCount = 0;
  m_dacCommandRegister = 0x00;
  m_dacState = 0x00;
  m_herculesCrtcIndex = 0;
  memset(m_herculesCrtcRegisters, 0, sizeof(m_herculesCrtcRegisters));
  m_herculesConfigRegister = 0x01; // Hercules-compatible adapter present; graphics mode disabled until 3B8h enables it
  m_herculesModeControl = 0;
  m_herculesStatus = 0;
  m_herculesVideoEnabled = false;
  resetDacPalette();
  updateTextRendererPalette();
  updateTextRendererFontMap();
  updateTextRendererUnderline();
  updateTextRendererCursorShape();
  updateTextRendererModeControl();
  updateTextRendererHorizontalPanning();
  updateTextRendererDisplayEnabled();
}

void PcMachine::loadDefaultTextFontPlane()
{
  if (!m_vgaPlaneMemory)
    return;

  uint8_t * plane2 = m_vgaPlaneMemory + 2 * 0x10000;
  for (uint16_t ch = 0; ch < 256; ++ch) {
    for (uint8_t row = 0; row < PcTextRenderer::CellHeight; ++row)
      plane2[static_cast<uint32_t>(ch) * 32u + row] =
        PcFont8x16Data[static_cast<uint32_t>(ch) * PcTextRenderer::CellHeight + row];
  }
}

void PcMachine::updateTextRendererFontMap()
{
  m_textRenderer.setVgaCharacterMap(m_vgaPlaneMemory ? m_vgaPlaneMemory + 2 * 0x10000 : nullptr,
                                    m_sequencerRegisters[0x03]);
}

void PcMachine::updateTextRendererUnderline()
{
  m_textRenderer.setUnderlineLocation(m_colorCrtcRegisters[0x14] & 0x1f);
}

void PcMachine::updateTextRendererCursorShape()
{
  m_textRenderer.setCursorShape(m_colorCrtcRegisters[0x0a], m_colorCrtcRegisters[0x0b]);
}

void PcMachine::updateTextRendererCellHeightFromCrtc()
{
  if (m_videoMode != VideoMode::Text80)
    return;

  int const cellHeight = (m_colorCrtcRegisters[0x09] & 0x1f) + 1;
  if (cellHeight != 8 && cellHeight != 14 && cellHeight != 16)
    return;

  m_textRenderer.setCellHeight(cellHeight);
  writeMemory16(0x0485, static_cast<uint16_t>(cellHeight));
  writeMemory8(0x0484, static_cast<uint8_t>(m_textRenderer.rows() - 1));
  updateTextRendererCursorPosition();
}

void PcMachine::updateTextRendererCursorPosition()
{
  if (m_videoMode != VideoMode::Text80)
    return;

  uint16_t columns = readMemory16(0x044a);
  if (columns != 40 && columns != 80)
    columns = PcTextRenderer::Columns;

  uint16_t const cursorAddress = static_cast<uint16_t>((uint16_t(m_colorCrtcRegisters[0x0e]) << 8) |
                                                       m_colorCrtcRegisters[0x0f]);
  uint16_t const startAddress = vgaCrtcStartAddress();
  uint16_t const cellOffset = static_cast<uint16_t>(cursorAddress - startAddress);
  uint8_t const row = static_cast<uint8_t>(cellOffset / columns);
  uint8_t const column = static_cast<uint8_t>(cellOffset % columns);
  m_textRenderer.setCursor(row, column, row < PcTextRenderer::Rows && column < PcTextRenderer::Columns);
}

void PcMachine::updateTextRendererModeControl()
{
  m_textRenderer.setLineGraphicsEnabled((m_attributeRegisters[0x10] & 0x04) != 0);
  m_textRenderer.setNineDotTextMode((m_sequencerRegisters[0x01] & 0x01) == 0);
}

void PcMachine::updateTextRendererHorizontalPanning()
{
  m_textRenderer.setHorizontalPanning(m_attributeRegisters[0x13] & 0x0f);
}

void PcMachine::updateTextRendererDisplayEnabled()
{
  m_textRenderer.setDisplayEnabled(isVgaDisplayEnabled());
}

void PcMachine::resetDacPalette()
{
  int index = 0;
  auto addColor = [this, &index](uint8_t r, uint8_t g, uint8_t b) {
    if (index >= 256)
      return;
    m_dacPalette[index][0] = r;
    m_dacPalette[index][1] = g;
    m_dacPalette[index][2] = b;
    ++index;
  };
  auto addGray = [&addColor](uint8_t value) {
    addColor(value, value, value);
  };
  auto add16Color = [&addColor](uint8_t lo, uint8_t melo, uint8_t mehi, uint8_t hi) {
    for (int color = 0; color < 16; ++color) {
      uint8_t const high = (color & 0x08) ? hi : mehi;
      uint8_t const low = (color & 0x08) ? melo : lo;
      uint8_t r = (color & 0x04) ? high : low;
      uint8_t g = (color & 0x02) ? high : low;
      uint8_t b = (color & 0x01) ? high : low;
      if (color == 0x06)
        g = melo; // IBM VGA brown instead of dark yellow
      addColor(r, g, b);
    }
  };
  auto addRun = [&addColor](int start, int channel, uint8_t lo, uint8_t melo, uint8_t mid, uint8_t mehi, uint8_t hi) {
    uint8_t r = (start & 0x04) ? hi : lo;
    uint8_t g = (start & 0x02) ? hi : lo;
    uint8_t b = (start & 0x01) ? hi : lo;
    addColor(r, g, b);
    bool const rising = (start & channel) == 0;
    uint8_t const steps[3] = {
      rising ? melo : mehi,
      mid,
      rising ? mehi : melo,
    };
    for (uint8_t value : steps) {
      if (channel == 0x04)
        r = value;
      else if (channel == 0x02)
        g = value;
      else
        b = value;
      addColor(r, g, b);
    }
    return start ^ channel;
  };
  auto addCycle = [&addRun](uint8_t lo, uint8_t melo, uint8_t mid, uint8_t mehi, uint8_t hi) {
    int hue = 0x01; // blue high, red/green low
    hue = addRun(hue, 0x04, lo, melo, mid, mehi, hi);
    hue = addRun(hue, 0x01, lo, melo, mid, mehi, hi);
    hue = addRun(hue, 0x02, lo, melo, mid, mehi, hi);
    hue = addRun(hue, 0x04, lo, melo, mid, mehi, hi);
    hue = addRun(hue, 0x01, lo, melo, mid, mehi, hi);
    addRun(hue, 0x02, lo, melo, mid, mehi, hi);
  };

  add16Color(0, 21, 42, 63);
  static uint8_t const gray16[16] = {0, 5, 8, 11, 14, 17, 20, 24, 28, 32, 36, 40, 45, 50, 56, 63};
  for (uint8_t gray : gray16)
    addGray(gray);
  addCycle( 0, 16, 31, 47, 63);
  addCycle(31, 39, 47, 55, 63);
  addCycle(45, 49, 54, 58, 63);
  addCycle( 0,  7, 14, 21, 28);
  addCycle(14, 17, 21, 24, 28);
  addCycle(20, 22, 24, 26, 28);
  addCycle( 0,  4,  8, 12, 16);
  addCycle( 8, 10, 12, 14, 16);
  addCycle(11, 12, 13, 15, 16);
  while (index < 256)
    addGray(0);
}

void PcMachine::setVideoMode(VideoMode mode, bool resetState, bool clearMemory)
{
  m_videoMode = mode;
  m_herculesVideoEnabled = mode == VideoMode::HerculesGraphics;
  if (!resetState)
    return;
  if (mode == VideoMode::Text80 || mode == VideoMode::VgaGraphics320x200x16 ||
      mode == VideoMode::VgaGraphics640x200x16 || mode == VideoMode::VgaGraphics640x350x2 ||
      mode == VideoMode::VgaGraphics640x350x16 || mode == VideoMode::VgaGraphics640x480x2 ||
      mode == VideoMode::VgaGraphics640x480x16 ||
      mode == VideoMode::VgaGraphics320x200x256 || mode == VideoMode::CgaGraphics320x200x4 ||
      mode == VideoMode::CgaGraphics640x200x2) {
    resetDacPalette();
    m_dacReadIndex = 0;
    m_dacWriteIndex = 0;
    m_dacReadComponent = 0;
    m_dacWriteComponent = 0;
    m_dacPelMask = 0xff;
    m_dacPelMaskReadCount = 0;
    m_dacCommandRegister = 0x00;
    m_dacState = 0x00;
    m_miscOutputRegister = 0x63; // color VGA I/O select with CPU access to display memory enabled
    m_featureControlRegister = 0x00;
    m_vgaEnableRegister = 1;
    m_cgaColorSelect = 0x00;
    m_attributeIndex = 0;
    m_attributeFlipFlop = false;
    m_attributeVideoEnabled = true;
    memset(m_attributeRegisters, 0, sizeof(m_attributeRegisters));
    for (int i = 0; i < 16; ++i)
      m_attributeRegisters[i] = static_cast<uint8_t>(i);
    m_attributeRegisters[0x10] = 0x0c; // text mode: line graphics + blink enabled; graphics modes refine this below
    m_attributeRegisters[0x12] = 0x0f;
    m_attributeRegisters[0x13] = 0x00;
    m_attributeRegisters[0x14] = 0x00;
    m_textRenderer.setBlinkEnabled(true);
    m_sequencerIndex = 0;
    memset(m_sequencerRegisters, 0, sizeof(m_sequencerRegisters));
    m_sequencerRegisters[0x00] = 0x03; // sequencer out of async/sync reset
    m_sequencerRegisters[0x02] = 0x0f; // map mask: all planes writable
    m_graphicsIndex = 0;
    memset(m_graphicsRegisters, 0, sizeof(m_graphicsRegisters));
    m_graphicsRegisters[0x06] = 0x05; // graphics mode, A000 window
    m_graphicsRegisters[0x07] = 0x0f; // color don't-care: all planes participate in read mode 1
    m_graphicsRegisters[0x08] = 0xff; // bit mask
    memset(m_vgaLatches, 0, sizeof(m_vgaLatches));
    if (mode == VideoMode::Text80)
      m_textRenderer.setColumns(PcTextRenderer::Columns);
    if (mode == VideoMode::Text80) {
      m_cgaModeControl = 0x29; // 80-column text, video enabled, blink enabled
      m_colorCrtcRegisters[0x14] = 0x1f;
      m_attributeRegisters[0x13] = 0x08; // default 9-dot VGA text has zero effective pan
    }
    if (mode == VideoMode::Text80) {
      m_colorCrtcRegisters[0x0a] = 0x06;
      m_colorCrtcRegisters[0x0b] = 0x07;
      m_colorCrtcRegisters[0x0c] = 0x00;
      m_colorCrtcRegisters[0x0d] = 0x00;
      m_colorCrtcRegisters[0x0e] = 0x00;
      m_colorCrtcRegisters[0x0f] = 0x00;
      m_colorCrtcRegisters[0x17] = 0x80; // CRTC display logic enabled after BIOS text mode set
    }
  }
  if (mode == VideoMode::VgaGraphics320x200x16 || mode == VideoMode::VgaGraphics640x200x16 ||
      mode == VideoMode::VgaGraphics640x350x2 || mode == VideoMode::VgaGraphics640x350x16 ||
      mode == VideoMode::VgaGraphics640x480x2 || mode == VideoMode::VgaGraphics640x480x16 ||
      mode == VideoMode::VgaGraphics320x200x256 ||
      mode == VideoMode::CgaGraphics320x200x4 || mode == VideoMode::CgaGraphics640x200x2) {
    m_sequencerRegisters[0x02] = 0x0f;
    m_sequencerRegisters[0x00] = 0x03;
    m_graphicsRegisters[0x04] = 0x00;
    m_graphicsRegisters[0x05] = mode == VideoMode::VgaGraphics320x200x256 ? 0x40 : 0x00;
    m_graphicsRegisters[0x06] = 0x05;
    m_graphicsRegisters[0x07] = 0x0f;
    m_graphicsRegisters[0x08] = 0xff;
    m_colorCrtcRegisters[0x0c] = 0x00;
    m_colorCrtcRegisters[0x0d] = 0x00;
    m_colorCrtcRegisters[0x13] = (mode == VideoMode::VgaGraphics320x200x16) ? 20 : 40;
    uint16_t const verticalDisplayEnd = vgaVerticalDisplayEndForMode(mode);
    m_colorCrtcRegisters[0x12] = static_cast<uint8_t>(verticalDisplayEnd & 0xff);
    m_colorCrtcRegisters[0x11] = 0x00;
    m_colorCrtcRegisters[0x18] = 0xff;
    m_colorCrtcRegisters[0x07] = 0x10;
    if (verticalDisplayEnd & 0x0100)
      m_colorCrtcRegisters[0x07] = static_cast<uint8_t>(m_colorCrtcRegisters[0x07] | 0x02);
    if (verticalDisplayEnd & 0x0200)
      m_colorCrtcRegisters[0x07] = static_cast<uint8_t>(m_colorCrtcRegisters[0x07] | 0x40);
    m_colorCrtcRegisters[0x08] = 0x00;
    m_colorCrtcRegisters[0x09] = 0x40;
    m_colorCrtcRegisters[0x14] = mode == VideoMode::VgaGraphics320x200x256 ? 0x40 : 0x00;
    m_colorCrtcRegisters[0x17] = mode == VideoMode::VgaGraphics320x200x256 ? 0x80 : 0xc0;
    m_attributeRegisters[0x12] = (mode == VideoMode::VgaGraphics640x350x2 ||
                                  mode == VideoMode::VgaGraphics640x480x2) ? 0x01 : 0x0f;
    m_attributeRegisters[0x13] = 0x00;
    m_attributeRegisters[0x10] = mode == VideoMode::VgaGraphics320x200x256 ? 0x41 : 0x01;
    m_sequencerRegisters[0x01] = 0x00;
    if (mode == VideoMode::VgaGraphics640x350x2 || mode == VideoMode::VgaGraphics640x480x2)
      m_attributeRegisters[1] = 0x0f;
  }
  if (mode == VideoMode::VgaGraphics320x200x256)
    m_sequencerRegisters[0x04] = 0x0e; // extended memory + odd/even disable + chain-4
  else if (mode == VideoMode::VgaGraphics320x200x16 || mode == VideoMode::VgaGraphics640x200x16 ||
           mode == VideoMode::VgaGraphics640x350x2 || mode == VideoMode::VgaGraphics640x350x16 ||
           mode == VideoMode::VgaGraphics640x480x2 || mode == VideoMode::VgaGraphics640x480x16)
    m_sequencerRegisters[0x04] = 0x06; // planar graphics memory
  if (mode == VideoMode::CgaGraphics320x200x4)
    m_cgaModeControl = 0x0a; // graphics + video enable
  else if (mode == VideoMode::CgaGraphics640x200x2) {
    m_cgaModeControl = 0x1a; // 640px graphics + video enable
    m_cgaColorSelect = 0x0f; // BIOS/default foreground is bright white; explicit port writes can still choose black.
  }
  if (clearMemory && (mode == VideoMode::VgaGraphics320x200x16 || mode == VideoMode::VgaGraphics640x200x16 ||
       mode == VideoMode::VgaGraphics640x350x2 || mode == VideoMode::VgaGraphics640x350x16 ||
       mode == VideoMode::VgaGraphics640x480x2 || mode == VideoMode::VgaGraphics640x480x16) &&
      m_vgaPlaneMemory)
    memset(m_vgaPlaneMemory, 0, 4 * 64 * 1024);
  if (mode == VideoMode::VgaGraphics320x200x256 && clearMemory) {
    if (m_videoMemory)
      memset(m_videoMemory + (VgaGraphicsMemoryBase - VideoMemoryBase), 0, 0x10000);
    if (m_vgaPlaneMemory)
      memset(m_vgaPlaneMemory, 0, 4 * 64 * 1024);
  }
  if (clearMemory && (mode == VideoMode::CgaGraphics320x200x4 || mode == VideoMode::CgaGraphics640x200x2) && m_videoMemory)
    memset(m_videoMemory + (TextColorMemoryBase - VideoMemoryBase), 0, 0x4000);
  writeMemory8(0x0465, m_cgaModeControl);
  writeMemory8(0x0466, m_cgaColorSelect);
  updateTextRendererPalette();
  updateTextRendererFontMap();
  updateTextRendererUnderline();
  updateTextRendererCursorShape();
  updateTextRendererCursorPosition();
  updateTextRendererModeControl();
  updateTextRendererHorizontalPanning();
  updateTextRendererDisplayEnabled();
}

bool PcMachine::isGraphicsMode() const
{
  return m_videoMode != VideoMode::Text80;
}

int PcMachine::graphicsWidth() const
{
  switch (m_videoMode) {
    case VideoMode::CgaGraphics320x200x4: return CgaGraphics320Width;
    case VideoMode::CgaGraphics640x200x2: return CgaGraphicsWidth;
    case VideoMode::HerculesGraphics: return HerculesGraphicsWidth;
    case VideoMode::VgaGraphics320x200x16: return VgaGraphics16LowWidth;
    case VideoMode::VgaGraphics640x200x16:
    case VideoMode::VgaGraphics640x350x2:
    case VideoMode::VgaGraphics640x350x16:
    case VideoMode::VgaGraphics640x480x2:
    case VideoMode::VgaGraphics640x480x16: return VgaGraphics16Width;
    case VideoMode::VgaGraphics320x200x256: return VgaGraphics256Width;
    case VideoMode::Text80:
    default: return PcTextRenderer::Width;
  }
}

int PcMachine::graphicsHeight() const
{
  switch (m_videoMode) {
    case VideoMode::CgaGraphics320x200x4:
    case VideoMode::CgaGraphics640x200x2: return CgaGraphicsHeight;
    case VideoMode::HerculesGraphics: return HerculesGraphicsHeight;
    case VideoMode::VgaGraphics320x200x16:
    case VideoMode::VgaGraphics640x200x16: return VgaGraphics16LowHeight;
    case VideoMode::VgaGraphics640x350x2:
    case VideoMode::VgaGraphics640x350x16: return VgaGraphics16MediumHeight;
    case VideoMode::VgaGraphics640x480x2:
    case VideoMode::VgaGraphics640x480x16: return VgaGraphics16Height;
    case VideoMode::VgaGraphics320x200x256: return vgaMode13Height();
    case VideoMode::Text80:
    default: return PcTextRenderer::Height;
  }
}

bool PcMachine::openDisk(int index, char const * path, PcDiskImage::Geometry geometry)
{
  if (index < 0 || index >= DiskCount)
    return false;
  return m_disks[index].open(path, geometry);
}

PcDiskImage * PcMachine::disk(int index)
{
  if (index < 0 || index >= DiskCount)
    return nullptr;
  return m_disks[index].isOpen() ? &m_disks[index] : nullptr;
}

PcDiskImage const * PcMachine::disk(int index) const
{
  if (index < 0 || index >= DiskCount)
    return nullptr;
  return m_disks[index].isOpen() ? &m_disks[index] : nullptr;
}

bool PcMachine::flushDisks()
{
  bool ok = true;
  for (int i = 0; i < DiskCount; ++i) {
    if (m_disks[i].isOpen())
      ok = m_disks[i].flush() && ok;
  }
  return ok;
}

bool PcMachine::loadBootSector(int diskIndex, uint8_t biosDrive, uint16_t segment, uint16_t offset)
{
  PcDiskImage * image = disk(diskIndex);
  uint32_t linearAddress = (static_cast<uint32_t>(segment) << 4) + offset;
  if (!image || !m_ram || !isRangeValid(linearAddress, PcDiskImage::SectorSize, RamSize))
    return false;

  if (!image->readSectors(0, 1, m_ram + linearAddress))
    return false;

  m_bootState.loaded = true;
  m_bootState.diskIndex = diskIndex;
  m_bootState.biosDrive = biosDrive;
  m_bootState.segment = segment;
  m_bootState.offset = offset;
  m_bootState.linearAddress = linearAddress;
  return true;
}

bool PcMachine::prepareBootCpu()
{
  if (!m_ram || !m_bootState.loaded)
    return false;

  PcI8086::setCallbacks(this,
                        readPortCallback,
                        writePortCallback,
                        writeVideoMemory8Callback,
                        writeVideoMemory16Callback,
                        readVideoMemory8Callback,
                        readVideoMemory16Callback,
                        interruptCallback);
  PcI8086::setUnsupportedOpcodeHandler(unsupportedOpcodeCallback);
  // Route the 64 KiB EMS page frame at E000:0 to the pool so mapped pages never
  // need copying and aliased physical frames share one backing (see emsMapPage).
  PcI8086::setEmsWindow(static_cast<uint32_t>(EmsPageFrameSegment) << 4,
                        (static_cast<uint32_t>(EmsPageFrameSegment) << 4) +
                            static_cast<uint32_t>(EmsPhysicalPages) * EmsLogicalPageSize,
                        emsReadCallback,
                        emsWriteCallback);
  PcI8086::setMemory(m_ram);
  PcI8086::reset();
  initializeBiosDataArea();
  updateEmsWindowActive();
  PcI8086::setCS(m_bootState.segment);
  PcI8086::setIP(m_bootState.offset);
  PcI8086::setDS(0x0000);
  PcI8086::setES(0x0000);
  PcI8086::setSS(0x0000);
  PcI8086::setSP(m_bootState.offset);
  PcI8086::setDL(m_bootState.biosDrive);
  return true;
}

void PcMachine::pitReset()
{
  uint64_t const now = monotonicMicroseconds();
  for (int i = 0; i < 3; ++i) {
    m_pit[i] = PitChannel{};
    m_pit[i].accessMode = 3;
    m_pit[i].operatingMode = 3;
    m_pit[i].gate = (i != 2); // channels 0/1 are always enabled; channel 2 gated by port 61h
    m_pit[i].phaseBaseMicros = now;
  }
  m_pitChannel0NextIrqMicros = 0;
}

uint32_t PcMachine::pitDivisor(int channel) const
{
  if (channel < 0 || channel > 2)
    return 65536;
  return m_pit[channel].reloadValue ? m_pit[channel].reloadValue : 65536;
}

uint16_t PcMachine::pitCurrentCount(int channel) const
{
  if (channel < 0 || channel > 2)
    return 0;
  uint32_t const divisor = pitDivisor(channel);
  uint64_t const elapsedMicros = monotonicMicroseconds() - m_pit[channel].phaseBaseMicros;
  uint64_t elapsedTicks = (elapsedMicros * PitInputHz) / 1000000ull;
  // Mode 3 (square wave, the BIOS default for channel 0) decrements the counter
  // by two per input clock so it sweeps the full range twice per output period.
  if (m_pit[channel].operatingMode == 3)
    elapsedTicks *= 2;
  uint32_t const counted = static_cast<uint32_t>(elapsedTicks % divisor);
  uint32_t const current = divisor - counted; // ranges 1..divisor
  return static_cast<uint16_t>(current & 0xffff);
}

void PcMachine::pitLatch(int channel)
{
  if (channel < 0 || channel > 2)
    return;
  m_pit[channel].latchValue = pitCurrentCount(channel);
  m_pit[channel].latched = true;
  m_pit[channel].readHigh = false;
}

void PcMachine::onPitReload(int channel)
{
  m_pit[channel].phaseBaseMicros = monotonicMicroseconds();
  if (channel == 0) {
    uint64_t const period = (static_cast<uint64_t>(pitDivisor(0)) * 1000000ull) / PitInputHz;
    m_pitChannel0NextIrqMicros = m_pit[0].phaseBaseMicros + (period ? period : 1);
  }
}

void PcMachine::pitWriteCommand(uint8_t value)
{
  uint8_t const channel = static_cast<uint8_t>(value >> 6);
  uint8_t const access = static_cast<uint8_t>((value >> 4) & 0x03);
  if (channel == 3)
    return; // 8254 read-back command: not modelled
  if (access == 0) {
    pitLatch(channel); // counter-latch command
    return;
  }
  PitChannel & c = m_pit[channel];
  c.accessMode = access;
  uint8_t mode = static_cast<uint8_t>((value >> 1) & 0x07);
  if (mode == 6)
    mode = 2;
  else if (mode == 7)
    mode = 3;
  c.operatingMode = mode;
  c.bcd = (value & 0x01) != 0;
  c.writeHigh = false;
  c.readHigh = false;
  c.latched = false;
}

void PcMachine::pitWriteData(int channel, uint8_t value)
{
  if (channel < 0 || channel > 2)
    return;
  PitChannel & c = m_pit[channel];
  bool reloadComplete = false;
  switch (c.accessMode) {
    case 1: // lobyte only
      c.reloadValue = static_cast<uint16_t>((c.reloadValue & 0xff00) | value);
      reloadComplete = true;
      break;
    case 2: // hibyte only
      c.reloadValue = static_cast<uint16_t>((c.reloadValue & 0x00ff) | (value << 8));
      reloadComplete = true;
      break;
    default: // lobyte then hibyte
      if (!c.writeHigh) {
        c.reloadValue = static_cast<uint16_t>((c.reloadValue & 0xff00) | value);
        c.writeHigh = true;
      } else {
        c.reloadValue = static_cast<uint16_t>((c.reloadValue & 0x00ff) | (value << 8));
        c.writeHigh = false;
        reloadComplete = true;
      }
      break;
  }
  if (reloadComplete)
    onPitReload(channel);
}

uint8_t PcMachine::pitReadData(int channel)
{
  if (channel < 0 || channel > 2)
    return 0;
  PitChannel & c = m_pit[channel];
  uint16_t const count = c.latched ? c.latchValue : pitCurrentCount(channel);
  uint8_t result = 0;
  switch (c.accessMode) {
    case 1:
      result = static_cast<uint8_t>(count & 0xff);
      c.latched = false;
      break;
    case 2:
      result = static_cast<uint8_t>(count >> 8);
      c.latched = false;
      break;
    default:
      if (!c.readHigh) {
        result = static_cast<uint8_t>(count & 0xff);
        c.readHigh = true;
      } else {
        result = static_cast<uint8_t>(count >> 8);
        c.readHigh = false;
        c.latched = false;
      }
      break;
  }
  return result;
}

void PcMachine::serviceTimerInterrupt()
{
  uint64_t const now = monotonicMicroseconds();
  uint64_t period = (static_cast<uint64_t>(pitDivisor(0)) * 1000000ull) / PitInputHz;
  if (period == 0)
    period = 1;
  if (m_pitChannel0NextIrqMicros == 0)
    m_pitChannel0NextIrqMicros = now + period;
  if (now < m_pitChannel0NextIrqMicros)
    return;

  // Advance one period; if we fell far behind (guest paused, slow slice) resync
  // to now so a backlog of ticks cannot storm the guest with interrupts.
  if (now - m_pitChannel0NextIrqMicros > period * 4)
    m_pitChannel0NextIrqMicros = now + period;
  else
    m_pitChannel0NextIrqMicros += period;

  // The BIOS 0040:006C tick is derived from wall-clock time, so it stays correct
  // even when a game reprograms channel 0 to a higher IRQ0 frequency.
  m_bios.updateTimerBda(*this);
  if ((m_picMask & 0x01) == 0)
    PcI8086::IRQ(0x08);
}

bool PcMachine::speakerEnabled() const
{
  // Speaker sounds when both the timer-2 gate (bit 0) and the speaker data
  // enable (bit 1) of port 61h are set.
  return (m_port61 & 0x03) == 0x03;
}

uint32_t PcMachine::speakerFrequency() const
{
  uint32_t const divisor = pitDivisor(2);
  if (!speakerEnabled() || divisor == 0)
    return 0;
  return PitInputHz / divisor;
}

void PcMachine::stepCpu()
{
  if (--m_timerUpdateCountdown <= 0) {
    serviceTimerInterrupt();
    m_timerUpdateCountdown = 1024;
  }
  m_bios.dispatchMouseCallback(*this);
  if (m_keyboard.irqPending() && !m_keyboardIrqInService) {
    uint16_t const keyboardVectorOffset = readMemory16(0x0009 * 4 + 0);
    uint16_t const keyboardVectorSegment = readMemory16(0x0009 * 4 + 2);
    bool const biosKeyboardVector = isBiosKeyboardVector(keyboardVectorOffset, keyboardVectorSegment);
    if (PcI8086::halted()) {
      m_keyboard.acknowledgeIrq();
      PcI8086::triggerInterrupt(0x09);
      if (!biosKeyboardVector)
        m_keyboardIrqInService = true;
    } else if (PcI8086::IRQ(0x09)) {
      m_keyboard.acknowledgeIrq();
      if (!biosKeyboardVector)
        m_keyboardIrqInService = true;
    }
  }
  PcI8086::step();
}

int PcMachine::runCpuSteps(int maxSteps)
{
  int steps = 0;
  while (steps < maxSteps && !cpuHalted()) {
    stepCpu();
    ++steps;
  }
  return steps;
}

bool PcMachine::cpuHalted() const
{
  return PcI8086::halted();
}

void PcMachine::completeKeyboardIrqService()
{
  m_keyboardIrqInService = false;
}

void PcMachine::resetDiagnostics()
{
  m_diagnostics = {};
}

PcMachine::VgaDacState PcMachine::vgaDacState() const
{
  return {m_dacReadIndex,
          m_dacWriteIndex,
          m_dacReadComponent,
          m_dacWriteComponent,
          m_dacPelMask,
          m_dacState,
          m_dacPelMaskReadCount,
          m_dacCommandRegister};
}

void PcMachine::setVgaDacState(VgaDacState const & state)
{
  m_dacReadIndex = state.readIndex;
  m_dacWriteIndex = state.writeIndex;
  m_dacReadComponent = static_cast<uint8_t>(state.readComponent % 3);
  m_dacWriteComponent = static_cast<uint8_t>(state.writeComponent % 3);
  m_dacPelMask = state.pelMask;
  m_dacState = state.state == 0x03 ? 0x03 : 0x00;
  m_dacPelMaskReadCount = state.pelMaskReadCount > 4 ? 4 : state.pelMaskReadCount;
  m_dacCommandRegister = state.command;
  updateTextRendererPalette();
}

PcMachine::VgaAttributeState PcMachine::vgaAttributeState() const
{
  return {m_attributeIndex, m_attributeFlipFlop, m_attributeVideoEnabled};
}

void PcMachine::setVgaAttributeState(VgaAttributeState const & state)
{
  m_attributeIndex = state.index & 0x1f;
  m_attributeFlipFlop = state.flipFlop;
  m_attributeVideoEnabled = state.videoEnabled;
  m_textRenderer.setBlinkEnabled((m_attributeRegisters[0x10] & 0x08) != 0);
  updateTextRendererModeControl();
  updateTextRendererHorizontalPanning();
  updateTextRendererPalette();
  updateTextRendererDisplayEnabled();
}

PcMachine::VgaLatchState PcMachine::vgaLatchState() const
{
  VgaLatchState state = {};
  for (int i = 0; i < 4; ++i)
    state.plane[i] = m_vgaLatches[i];
  return state;
}

void PcMachine::setVgaLatchState(VgaLatchState const & state)
{
  for (int i = 0; i < 4; ++i)
    m_vgaLatches[i] = state.plane[i];
}

void PcMachine::setCgaCompatibilityState(uint8_t modeControl, uint8_t colorSelect)
{
  m_cgaModeControl = modeControl;
  m_cgaColorSelect = colorSelect;
  writeMemory8(0x0465, modeControl);
  writeMemory8(0x0466, colorSelect);
}

void PcMachine::setMouseInstalled(bool installed)
{
  m_bios.setMouseInstalled(installed);
}

void PcMachine::setMouseState(uint16_t x, uint16_t y, uint16_t buttons)
{
  m_bios.setMouseState(x, y, buttons);
}

void PcMachine::setMouseSourceState(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t buttons)
{
  m_bios.setMouseSourceState(x, y, width, height, buttons);
}

PcMachine::CpuState PcMachine::cpuState() const
{
  CpuState state;
  state.ax = PcI8086::AX();
  state.bx = PcI8086::BX();
  state.cx = PcI8086::CX();
  state.dx = PcI8086::DX();
  state.cs = PcI8086::CS();
  state.ds = PcI8086::DS();
  state.es = PcI8086::ES();
  state.ss = PcI8086::SS();
  state.ip = PcI8086::IP();
  state.sp = PcI8086::SP();
  state.halted = PcI8086::halted();
  return state;
}

uint8_t PcMachine::readPort(uint16_t port)
{
  if (isAbsentLegacyProbePort(port))
    return 0xff;
  if (isLegacyPrinterPort(port))
    return legacyPrinterPortRead(port);

  switch (port) {
    case 0x0020: // master PIC command/status
      return 0x00;
    case 0x0021: // master PIC interrupt mask
      return m_picMask;
    case 0x00a0: // slave PIC command/status
      return 0x00;
    case 0x00a1: // slave PIC interrupt mask
      return m_picSlaveMask;
    case 0x0022: // chipset/configuration index used by DOS probes
    case 0x0023: // chipset/configuration data used by DOS probes
      return 0x00;
    case 0x0040: // PIT channel 0 counter
      return pitReadData(0);
    case 0x0041: // PIT channel 1 counter
      return pitReadData(1);
    case 0x0042: // PIT channel 2 counter
      return pitReadData(2);
    case 0x0388: // AdLib/OPL2 status register
      return m_opl2.readStatus(monotonicMicroseconds());
    case 0x0389: // AdLib/OPL2 data port is write-only
      return 0xff;
    case 0x0060:
    {
      uint8_t const value = m_keyboard.readDataPort();
      m_bios.observeKeyboardScancode(*this, value);
      return value;
    }
    case 0x0061: // PPI/speaker control
      return m_port61;
    case 0x0064:
      return m_keyboard.readStatusPort();
    case 0x0070: // CMOS/RTC index
      return m_cmosIndex;
    case 0x0071: // CMOS/RTC data
      return cmosRegisterValue(m_cmosIndex);
    case 0x0080: // POST/ISA diagnostic delay port; no device state
      return 0x00;
    case 0x0092: // PS/2 system-control port A / fast A20 gate
      return 0x02; // A20 appears enabled; reset bit clear
    case 0x00f9: // emulator/debug port used by some DOS probes; keep quiet
      return 0x00;
    case 0x2861: // chipset/video probe port observed in DOS applications
      return 0x00;
    case 0x03b4: // Hercules/MDA or VGA mono CRTC index
      return isVgaMonoCrtcSelected() ? m_colorCrtcIndex : m_herculesCrtcIndex;
    case 0x03b5: // Hercules/MDA or VGA mono CRTC data
      return isVgaMonoCrtcSelected() ? m_colorCrtcRegisters[m_colorCrtcIndex & 0x1f]
                                     : m_herculesCrtcRegisters[m_herculesCrtcIndex & 0x1f];
    case 0x03b8: // Hercules/MDA mode control readback
      return m_herculesModeControl;
    case 0x03ba: // VGA mono status or Hercules status
      if (isVgaMonoCrtcSelected()) {
        m_attributeFlipFlop = false;
        return static_cast<uint8_t>(nextColorDisplayStatusBits(m_herculesStatus) | vgaStatusMuxBits());
      }
      m_herculesStatus ^= 0x89;
      return static_cast<uint8_t>(0x10 | m_herculesStatus);
    case 0x03bf: // Hercules configuration switch
      return m_herculesConfigRegister;
    case 0x03f2: // floppy controller digital output register
      return m_floppyDigitalOutputRegister;
    case 0x03c0: // VGA attribute controller address
      return static_cast<uint8_t>(m_attributeIndex | (m_attributeVideoEnabled ? 0x20 : 0x00));
    case 0x03c1: // VGA attribute controller data read
      return m_attributeRegisters[m_attributeIndex & 0x1f];
    case 0x03c2: // VGA input status register 0
      return vgaInputStatus0();
    case 0x03c3: // VGA video subsystem enable
      return m_vgaEnableRegister;
    case 0x03c4: // VGA sequencer index
      return m_sequencerIndex;
    case 0x03c5: // VGA sequencer data
      return m_sequencerRegisters[m_sequencerIndex & 0x07];
    case 0x03c6: // VGA DAC pel mask
      if (m_dacPelMaskReadCount >= 4) {
        m_dacPelMaskReadCount = 0;
        return m_dacCommandRegister;
      }
      ++m_dacPelMaskReadCount;
      return m_dacPelMask;
    case 0x03c7: // VGA DAC state
      m_dacPelMaskReadCount = 0;
      return m_dacState;
    case 0x03c8: // VGA DAC write index
      m_dacPelMaskReadCount = 0;
      return m_dacWriteIndex;
    case 0x03c9: // VGA DAC data
    {
      m_dacPelMaskReadCount = 0;
      uint8_t const value = m_dacPalette[m_dacReadIndex][m_dacReadComponent] & 0x3f;
      if (++m_dacReadComponent >= 3) {
        m_dacReadComponent = 0;
        ++m_dacReadIndex;
      }
      return value;
    }
    case 0x03ca: // VGA feature control read
      return m_featureControlRegister;
    case 0x03cc: // VGA misc output read
      return m_miscOutputRegister;
    case 0x03ce: // VGA graphics controller index
      return m_graphicsIndex;
    case 0x03cf: // VGA graphics controller data
      return m_graphicsRegisters[m_graphicsIndex & 0x0f];
    case 0x03d4: // CGA/VGA color CRTC index
      return m_colorCrtcIndex;
    case 0x03d5: // CGA/VGA color CRTC data
      return m_colorCrtcRegisters[m_colorCrtcIndex & 0x1f];
    case 0x03d8: // CGA mode control
      return m_cgaModeControl;
    case 0x03d9: // CGA color select
      return m_cgaColorSelect;
    case 0x03da: // CGA/VGA status: return changing retrace bits for probes
      m_attributeFlipFlop = false;
      return static_cast<uint8_t>(nextColorDisplayStatusBits(m_herculesStatus) | vgaStatusMuxBits());
    default:
      recordUnsupportedPortRead(port);
      return 0xff;
  }
}

void PcMachine::writePort(uint16_t port, uint8_t value)
{
  if (isAbsentLegacyProbePort(port))
    return;
  if (isLegacyPrinterPort(port))
    return;

  switch (port) {
    case 0x0020: // master PIC command
      if (value & 0x20) // non-specific/specific EOI; release IRQ1 in-service state too
        m_keyboardIrqInService = false;
      break;
    case 0x0021: // master PIC interrupt mask
      m_picMask = value;
      break;
    case 0x00a0: // slave PIC command
      break;
    case 0x00a1: // slave PIC interrupt mask
      m_picSlaveMask = value;
      break;
    case 0x0022: // chipset/configuration index used by DOS probes
    case 0x0023: // chipset/configuration data used by DOS probes
      break;
    case 0x0040: // PIT channel 0 reload data
      pitWriteData(0, value);
      break;
    case 0x0041: // PIT channel 1 reload data
      pitWriteData(1, value);
      break;
    case 0x0042: // PIT channel 2 reload data
      pitWriteData(2, value);
      break;
    case 0x0043: // PIT mode/command
      pitWriteCommand(value);
      break;
    case 0x0388: // AdLib/OPL2 register-select port
      m_opl2.writeAddress(value);
      break;
    case 0x0389: // AdLib/OPL2 register-data port
      m_opl2.writeData(value, monotonicMicroseconds());
      break;
    case 0x0060:
      m_keyboard.writeDataPort(value);
      break;
    case 0x0061: // PPI/speaker control
      m_port61 = value;
      m_pit[2].gate = (value & 0x01) != 0; // bit 0 gates PIT channel 2
      break;
    case 0x0064:
      m_keyboard.writeCommandPort(value);
      break;
    case 0x0070: // CMOS/RTC index; keep NMI mask bit as hardware does
      m_cmosIndex = value;
      break;
    case 0x0071: // CMOS/RTC data writes are ignored by the emulator
      break;
    case 0x0080: // POST/ISA diagnostic delay port
      break;
    case 0x0092: // PS/2 system-control port A / fast A20 gate
      // Memory above 1 MiB is not exposed, so fast-A20 toggles are accepted as
      // harmless no-ops. Ignore bit 0 to avoid guest-triggered reset loops.
      break;
    case 0x00f9: // emulator/debug port used by some DOS probes; keep quiet
      break;
    case 0x2861: // chipset/video probe port observed in DOS applications
      break;
    case 0x03b4: // Hercules/MDA or VGA mono CRTC index
      m_herculesCrtcIndex = value & 0x1f;
      if (isVgaMonoCrtcSelected())
        m_colorCrtcIndex = value & 0x1f;
      break;
    case 0x03b5: // Hercules/MDA or VGA mono CRTC data
      m_herculesCrtcRegisters[m_herculesCrtcIndex & 0x1f] = value;
      if (isVgaMonoCrtcSelected()) {
        if ((m_colorCrtcRegisters[0x11] & 0x80) && m_colorCrtcIndex <= 0x07) {
          if (m_colorCrtcIndex == 0x07) {
            m_colorCrtcRegisters[0x07] = static_cast<uint8_t>((m_colorCrtcRegisters[0x07] & ~0x10) |
                                                              (value & 0x10));
          }
          break;
        }
        m_colorCrtcRegisters[m_colorCrtcIndex & 0x1f] = normalizeVgaCrtcRegister(m_colorCrtcIndex, value);
        if ((m_colorCrtcIndex & 0x1f) == 0x0a || (m_colorCrtcIndex & 0x1f) == 0x0b)
          updateTextRendererCursorShape();
        if ((m_colorCrtcIndex & 0x1f) == 0x09)
          updateTextRendererCellHeightFromCrtc();
        if ((m_colorCrtcIndex & 0x1f) == 0x0c || (m_colorCrtcIndex & 0x1f) == 0x0d ||
            (m_colorCrtcIndex & 0x1f) == 0x0e || (m_colorCrtcIndex & 0x1f) == 0x0f)
          updateTextRendererCursorPosition();
        if ((m_colorCrtcIndex & 0x1f) == 0x14)
          updateTextRendererUnderline();
        if ((m_colorCrtcIndex & 0x1f) == 0x17)
          updateTextRendererDisplayEnabled();
      }
      break;
    case 0x03ba: // VGA mono feature control write / Hercules-compatible feature control
      m_featureControlRegister = normalizeVgaFeatureControl(value);
      break;
    case 0x03b8: // Hercules mode control
      m_herculesModeControl = value;
      m_herculesVideoEnabled = (value & 0x08) != 0;
      m_videoMode = (value & 0x02) ? VideoMode::HerculesGraphics : VideoMode::Text80;
      setEquipmentVideoBits(0x0030); // mono/Hercules adapter selected
      break;
    case 0x03bf: // Hercules configuration switch
      m_herculesConfigRegister = value & 0x03;
      if (m_herculesConfigRegister & 0x01)
        setEquipmentVideoBits(0x0030); // graphics-capable Hercules/MDA adapter present
      break;
    case 0x03f2: // floppy controller digital output register
      m_floppyDigitalOutputRegister = value;
      break;
    case 0x03c0: // VGA attribute controller index/data
      if (!m_attributeFlipFlop) {
        m_attributeIndex = value & 0x1f;
        m_attributeVideoEnabled = (value & 0x20) != 0;
      } else {
        value = normalizeVgaAttributeRegister(m_attributeIndex, value);
        m_attributeRegisters[m_attributeIndex & 0x1f] = value;
        if ((m_attributeIndex & 0x1f) == 0x10) {
          m_textRenderer.setBlinkEnabled((value & 0x08) != 0);
          uint8_t modeControl = readMemory8(0x0465);
          if (value & 0x08)
            modeControl |= 0x20;
          else
            modeControl &= static_cast<uint8_t>(~0x20);
          writeMemory8(0x0465, modeControl);
          updateTextRendererModeControl();
        }
        if ((m_attributeIndex & 0x1f) == 0x13)
          updateTextRendererHorizontalPanning();
        updateTextRendererPalette();
      }
      m_attributeFlipFlop = !m_attributeFlipFlop;
      updateTextRendererDisplayEnabled();
      break;
    case 0x03c2: // VGA misc output
      m_miscOutputRegister = value;
      if (m_videoMode != VideoMode::HerculesGraphics)
        writeMemory16(0x0463, (value & 0x01) ? 0x03d4 : 0x03b4);
      break;
    case 0x03c3: // VGA video subsystem enable
      m_vgaEnableRegister = value & 0x01;
      updateTextRendererDisplayEnabled();
      break;
    case 0x03c4: // VGA sequencer index
      m_sequencerIndex = value & 0x07;
      break;
    case 0x03c5: // VGA sequencer data
      m_sequencerRegisters[m_sequencerIndex & 0x07] = normalizeVgaSequencerRegister(m_sequencerIndex, value);
      if ((m_sequencerIndex & 0x07) == 0x03)
        updateTextRendererFontMap();
      if ((m_sequencerIndex & 0x07) == 0x01)
        updateTextRendererModeControl();
      if ((m_sequencerIndex & 0x07) == 0x00 || (m_sequencerIndex & 0x07) == 0x01)
        updateTextRendererDisplayEnabled();
      break;
    case 0x03c6: // VGA DAC pel mask
      if (m_dacPelMaskReadCount >= 4) {
        m_dacCommandRegister = value;
      } else {
        m_dacPelMask = value;
        updateTextRendererPalette();
      }
      m_dacPelMaskReadCount = 0;
      break;
    case 0x03c7: // VGA DAC read index
      m_dacPelMaskReadCount = 0;
      m_dacReadIndex = value;
      m_dacReadComponent = 0;
      m_dacState = 0x03;
      break;
    case 0x03c8: // VGA DAC write index
      m_dacPelMaskReadCount = 0;
      m_dacWriteIndex = value;
      m_dacWriteComponent = 0;
      m_dacState = 0x00;
      break;
    case 0x03c9: // VGA DAC data
      m_dacPelMaskReadCount = 0;
      m_dacPalette[m_dacWriteIndex][m_dacWriteComponent] = value & 0x3f;
      m_dacState = 0x00;
      if (++m_dacWriteComponent >= 3) {
        m_dacWriteComponent = 0;
        ++m_dacWriteIndex;
      }
      updateTextRendererPalette();
      break;
    case 0x03ca: // VGA feature control
      m_featureControlRegister = normalizeVgaFeatureControl(value);
      break;
    case 0x03ce: // VGA graphics controller index
      m_graphicsIndex = value & 0x0f;
      break;
    case 0x03cf: // VGA graphics controller data
      m_graphicsRegisters[m_graphicsIndex & 0x0f] = normalizeVgaGraphicsRegister(m_graphicsIndex, value);
      break;
    case 0x03d4: // CGA/VGA color CRTC index
      m_colorCrtcIndex = value & 0x1f;
      break;
    case 0x03d5: // CGA/VGA color CRTC data
      if ((m_colorCrtcRegisters[0x11] & 0x80) && m_colorCrtcIndex <= 0x07) {
        if (m_colorCrtcIndex == 0x07) {
          // VGA protects CRTC indexes 00h-07h with Vertical Retrace End bit 7,
          // but the Overflow register's line-compare bit remains writable.
          m_colorCrtcRegisters[0x07] = static_cast<uint8_t>((m_colorCrtcRegisters[0x07] & ~0x10) |
                                                            (value & 0x10));
        }
        break;
      }
      m_colorCrtcRegisters[m_colorCrtcIndex & 0x1f] = normalizeVgaCrtcRegister(m_colorCrtcIndex, value);
      if ((m_colorCrtcIndex & 0x1f) == 0x0a || (m_colorCrtcIndex & 0x1f) == 0x0b)
        updateTextRendererCursorShape();
      if ((m_colorCrtcIndex & 0x1f) == 0x09)
        updateTextRendererCellHeightFromCrtc();
      if ((m_colorCrtcIndex & 0x1f) == 0x0c || (m_colorCrtcIndex & 0x1f) == 0x0d ||
          (m_colorCrtcIndex & 0x1f) == 0x0e || (m_colorCrtcIndex & 0x1f) == 0x0f)
        updateTextRendererCursorPosition();
      if ((m_colorCrtcIndex & 0x1f) == 0x14)
        updateTextRendererUnderline();
      if ((m_colorCrtcIndex & 0x1f) == 0x17)
        updateTextRendererDisplayEnabled();
      break;
    case 0x03d8: // CGA mode control
      m_cgaModeControl = value;
      writeMemory8(0x0465, value);
      if (value & 0x02) {
        m_videoMode = (value & 0x10) ? VideoMode::CgaGraphics640x200x2 : VideoMode::CgaGraphics320x200x4;
        setEquipmentVideoBits(0x0020); // color adapter selected
        syncCgaModeBiosDataArea(m_videoMode);
      } else if (m_videoMode == VideoMode::CgaGraphics320x200x4 ||
                 m_videoMode == VideoMode::CgaGraphics640x200x2) {
        m_videoMode = VideoMode::Text80;
        syncCgaModeBiosDataArea(m_videoMode);
      }
      break;
    case 0x03d9: // CGA color select
      m_cgaColorSelect = value;
      writeMemory8(0x0466, value);
      break;
    case 0x03da: // VGA color feature control write
      m_featureControlRegister = normalizeVgaFeatureControl(value);
      break;
    default:
      recordUnsupportedPortWrite(port);
      break;
  }
}

uint8_t PcMachine::readMemory8(uint32_t address) const
{
  if (!m_ram || !isRangeValid(address, 1, RamSize))
    return 0xff;
  return m_ram[address];
}

uint16_t PcMachine::readMemory16(uint32_t address) const
{
  uint16_t lo = readMemory8(address);
  uint16_t hi = readMemory8(address + 1);
  return lo | (hi << 8);
}

void PcMachine::writeMemory8(uint32_t address, uint8_t value)
{
  if (!m_ram || !isRangeValid(address, 1, RamSize))
    return;
  m_ram[address] = value;
}

void PcMachine::writeMemory16(uint32_t address, uint16_t value)
{
  writeMemory8(address, static_cast<uint8_t>(value & 0xff));
  writeMemory8(address + 1, static_cast<uint8_t>(value >> 8));
}

bool PcMachine::isRamRangeValid(uint32_t address, size_t size) const
{
  return m_ram && isRangeValid(address, size, RamSize);
}

bool PcMachine::readMemoryBlock(uint32_t address, void * dest, size_t size) const
{
  if (!dest || !isRamRangeValid(address, size))
    return false;
  memcpy(dest, m_ram + address, size);
  return true;
}

bool PcMachine::writeMemoryBlock(uint32_t address, void const * src, size_t size)
{
  if (!src || !isRamRangeValid(address, size))
    return false;
  memcpy(m_ram + address, src, size);
  return true;
}

uint8_t PcMachine::readVideoMemory8(uint32_t address) const
{
  if (!m_videoMemory || address < VideoMemoryBase)
    return 0xff;
  uint32_t offset = address - VideoMemoryBase;
  if (!isRangeValid(offset, 1, VideoMemorySize))
    return 0xff;
  if (!isVgaMemoryAccessEnabled())
    return 0xff;
  uint32_t planeOffset = 0;
  bool const textModeVgaAperture = m_videoMode == VideoMode::Text80 &&
                                   address >= VgaGraphicsMemoryBase &&
                                   address < 0x000b0000;
  if (usesPlanarVgaMemory() || textModeVgaAperture) {
    if (!m_vgaPlaneMemory || !vgaPlanarOffsetForAddress(address, &planeOffset))
      return 0xff;
    bool const oddEvenRead = isVgaOddEvenReadEnabled();
    uint32_t const latchOffset = oddEvenRead ? (planeOffset >> 1) : planeOffset;
    for (int plane = 0; plane < 4; ++plane)
      const_cast<PcMachine *>(this)->m_vgaLatches[plane] = m_vgaPlaneMemory[plane * 0x10000 + latchOffset];
    if (m_graphicsRegisters[0x05] & 0x08) {
      uint8_t result = 0;
      uint8_t const colorCompare = m_graphicsRegisters[0x02] & 0x0f;
      uint8_t const colorDontCare = m_graphicsRegisters[0x07] & 0x0f;
      for (int bit = 0; bit < 8; ++bit) {
        uint8_t const mask = static_cast<uint8_t>(0x80 >> bit);
        uint8_t color = 0;
        for (int plane = 0; plane < 4; ++plane) {
          if (m_vgaLatches[plane] & mask)
            color |= static_cast<uint8_t>(1 << plane);
        }
        if (((color ^ colorCompare) & colorDontCare) == 0)
          result |= mask;
      }
      return result;
    }
    if (oddEvenRead)
      return m_vgaLatches[(planeOffset & 0x01) | (m_graphicsRegisters[0x04] & 0x02)];
    return m_vgaLatches[m_graphicsRegisters[0x04] & 0x03];
  }
  uint32_t chain4Offset = 0;
  if (m_videoMode == VideoMode::VgaGraphics320x200x256 && isVgaChain4Enabled()) {
    if (!vgaChain4OffsetForAddress(address, &chain4Offset))
      return 0xff;
    if (m_vgaPlaneMemory) {
      uint32_t const planeOffset = (chain4Offset >> 2) & 0xffff;
      for (int plane = 0; plane < 4; ++plane)
        const_cast<PcMachine *>(this)->m_vgaLatches[plane] = m_vgaPlaneMemory[plane * 0x10000 + planeOffset];
      if (m_graphicsRegisters[0x05] & 0x08) {
        uint8_t result = 0;
        uint8_t const colorCompare = m_graphicsRegisters[0x02] & 0x0f;
        uint8_t const colorDontCare = m_graphicsRegisters[0x07] & 0x0f;
        for (int bit = 0; bit < 8; ++bit) {
          uint8_t const mask = static_cast<uint8_t>(0x80 >> bit);
          uint8_t color = 0;
          for (int plane = 0; plane < 4; ++plane) {
            if (m_vgaLatches[plane] & mask)
              color |= static_cast<uint8_t>(1 << plane);
          }
          if (((color ^ colorCompare) & colorDontCare) == 0)
            result |= mask;
        }
        return result;
      }
    }
    return vgaReadChain4Byte(chain4Offset);
  }
  return m_videoMemory[offset];
}

uint16_t PcMachine::readVideoMemory16(uint32_t address) const
{
  uint16_t lo = readVideoMemory8(address);
  uint16_t hi = readVideoMemory8(address + 1);
  return lo | (hi << 8);
}

void PcMachine::writeVideoMemory8(uint32_t address, uint8_t value)
{
  if (!m_videoMemory || address < VideoMemoryBase)
    return;
  uint32_t offset = address - VideoMemoryBase;
  if (!isRangeValid(offset, 1, VideoMemorySize))
    return;
  if (!isVgaMemoryAccessEnabled())
    return;
  uint32_t planeOffset = 0;
  bool const textModeVgaAperture = m_videoMode == VideoMode::Text80 &&
                                   address >= VgaGraphicsMemoryBase &&
                                   address < 0x000b0000;
  if (usesPlanarVgaMemory() || textModeVgaAperture) {
    if (!m_vgaPlaneMemory || !vgaPlanarOffsetForAddress(address, &planeOffset))
      return;
    if (isVgaOddEvenWriteEnabled()) {
      uint8_t const oddEvenPlaneMask = (planeOffset & 0x01) ? 0x0a : 0x05;
      vgaWritePlaneByte(planeOffset >> 1, value, oddEvenPlaneMask);
    } else {
      vgaWritePlaneByte(planeOffset, value);
    }
    return;
  }
  uint32_t chain4Offset = 0;
  if (m_videoMode == VideoMode::VgaGraphics320x200x256 && isVgaChain4Enabled()) {
    if (!vgaChain4OffsetForAddress(address, &chain4Offset))
      return;
    uint32_t const plane = chain4Offset & 0x03;
    uint32_t const planeOffset = (chain4Offset >> 2) & 0xffff;
    vgaWritePlaneByte(planeOffset, value, static_cast<uint8_t>(1 << plane));
    if (m_videoMemory)
      m_videoMemory[VgaGraphicsMemoryBase - VideoMemoryBase + (chain4Offset & 0xffff)] = vgaReadChain4Byte(chain4Offset);
    return;
  }
  m_videoMemory[offset] = value;
}

void PcMachine::writeVideoMemory16(uint32_t address, uint16_t value)
{
  writeVideoMemory8(address, static_cast<uint8_t>(value & 0xff));
  writeVideoMemory8(address + 1, static_cast<uint8_t>(value >> 8));
}

uint8_t PcMachine::vgaReadChain4Byte(uint32_t offset) const
{
  offset &= 0xffff;
  if (m_vgaPlaneMemory) {
    uint32_t const plane = offset & 0x03;
    uint32_t const planeOffset = (offset >> 2) & 0xffff;
    return m_vgaPlaneMemory[plane * 0x10000 + planeOffset];
  }
  if (!m_videoMemory)
    return 0xff;
  return m_videoMemory[VgaGraphicsMemoryBase - VideoMemoryBase + offset];
}

void PcMachine::vgaWriteChain4Byte(uint32_t offset, uint8_t value)
{
  offset &= 0xffff;
  uint32_t const plane = offset & 0x03;
  if (!(m_sequencerRegisters[0x02] & (1 << plane)))
    return;
  if (m_videoMemory)
    m_videoMemory[VgaGraphicsMemoryBase - VideoMemoryBase + offset] = value;
  if (m_vgaPlaneMemory) {
    uint32_t const planeOffset = (offset >> 2) & 0xffff;
    m_vgaPlaneMemory[plane * 0x10000 + planeOffset] = value;
  }
}

void PcMachine::vgaWritePlaneByte(uint32_t offset, uint8_t value, uint8_t planeMask)
{
  if (!m_vgaPlaneMemory || offset >= 0x10000)
    return;

  uint8_t const rotate = m_graphicsRegisters[0x03] & 0x07;
  uint8_t const rotated = static_cast<uint8_t>((value >> rotate) | (value << ((8 - rotate) & 0x07)));
  uint8_t const logicalOp = (m_graphicsRegisters[0x03] >> 3) & 0x03;
  uint8_t const writeMode = m_graphicsRegisters[0x05] & 0x03;
  uint8_t const bitMaskReg = m_graphicsRegisters[0x08];
  uint8_t const setReset = m_graphicsRegisters[0x00] & 0x0f;
  uint8_t const enableSetReset = m_graphicsRegisters[0x01] & 0x0f;
  uint8_t const mapMask = m_sequencerRegisters[0x02] & planeMask & 0x0f;

  for (int plane = 0; plane < 4; ++plane) {
    if (!(mapMask & (1 << plane)))
      continue;

    uint8_t const latch = m_vgaLatches[plane];
    uint8_t input = rotated;
    uint8_t mask = bitMaskReg;

    switch (writeMode) {
      case 0:
        if (enableSetReset & (1 << plane))
          input = (setReset & (1 << plane)) ? 0xff : 0x00;
        break;
      case 1:
        m_vgaPlaneMemory[plane * 0x10000 + offset] = latch;
        continue;
      case 2:
        input = (value & (1 << plane)) ? 0xff : 0x00;
        break;
      case 3:
        mask = rotated & bitMaskReg;
        input = (setReset & (1 << plane)) ? 0xff : 0x00;
        break;
    }

    switch (logicalOp) {
      case 1: input &= latch; break;
      case 2: input |= latch; break;
      case 3: input ^= latch; break;
      default: break;
    }

    m_vgaPlaneMemory[plane * 0x10000 + offset] = static_cast<uint8_t>((input & mask) | (latch & ~mask));
  }
}

PcMachine::MouseCursorOverlay PcMachine::computeMouseOverlay(int sourceWidth,
                                                             int sourceHeight,
                                                             bool textMode,
                                                             int cellWidth,
                                                             int cellHeight) const
{
  MouseCursorOverlay overlay;
  PcBios::MouseRenderInfo const info = m_bios.mouseRenderInfo();
  if (!info.visible || sourceWidth <= 0 || sourceHeight <= 0)
    return overlay;

  // Honor a conditional-off (exclusion) region set via INT 33h AX=0010.
  if (info.excludeActive && info.x >= info.excludeLeft && info.x <= info.excludeRight &&
      info.y >= info.excludeTop && info.y <= info.excludeBottom)
    return overlay;

  // Map the DOS virtual mouse coordinate onto the source frame. This inverts the
  // forward mapping performed by setMouseSourceState so the pointer tracks touch.
  int const xSpan = info.maxX > info.minX ? (info.maxX - info.minX) : 0;
  int const ySpan = info.maxY > info.minY ? (info.maxY - info.minY) : 0;
  int sourceX = xSpan > 0
                  ? static_cast<int>(static_cast<long>(info.x - info.minX) * (sourceWidth - 1) / xSpan)
                  : 0;
  int sourceY = ySpan > 0
                  ? static_cast<int>(static_cast<long>(info.y - info.minY) * (sourceHeight - 1) / ySpan)
                  : 0;
  if (sourceX < 0)
    sourceX = 0;
  if (sourceX > sourceWidth - 1)
    sourceX = sourceWidth - 1;
  if (sourceY < 0)
    sourceY = 0;
  if (sourceY > sourceHeight - 1)
    sourceY = sourceHeight - 1;

  overlay.visible = true;
  if (textMode) {
    int const cw = cellWidth > 0 ? cellWidth : 1;
    int const ch = cellHeight > 0 ? cellHeight : 1;
    overlay.textCell = true;
    overlay.x = (sourceX / cw) * cw;
    overlay.y = (sourceY / ch) * ch;
    overlay.cellWidth = cw;
    overlay.cellHeight = ch;
  } else {
    overlay.textCell = false;
    overlay.x = sourceX - info.hotspotX;
    overlay.y = sourceY - info.hotspotY;
    memcpy(overlay.screenMask, info.screenMask, sizeof(overlay.screenMask));
    memcpy(overlay.cursorMask, info.cursorMask, sizeof(overlay.cursorMask));
  }
  return overlay;
}

uint8_t const * PcMachine::text80Buffer() const
{
  if (!m_videoMemory)
    return nullptr;
  uint32_t const textBase = (readMemory8(0x0449) == 0x07 || readMemory16(0x0463) == 0x03b4)
                              ? HerculesGraphicsMemoryBase
                              : TextColorMemoryBase;
  uint32_t const bdaPageOffset = readMemory16(0x044e);
  uint32_t pageOffset = vgaCrtcStartOffsetBytes();
  if (pageOffset == 0 && bdaPageOffset != 0)
    pageOffset = bdaPageOffset;
  uint32_t const offset = textBase - VideoMemoryBase + pageOffset;
  uint32_t columns = readMemory16(0x044a);
  if (columns != 40 && columns != 80)
    columns = PcTextRenderer::Columns;
  uint32_t rows = static_cast<uint32_t>(readMemory8(0x0484)) + 1;
  if (rows < 1 || rows > PcTextRenderer::MaxRows)
    rows = PcTextRenderer::Rows;
  uint32_t const visibleTextBytes = columns * rows * 2;
  if (!isRangeValid(offset, visibleTextBytes, VideoMemorySize))
    pageOffset = 0;
  return m_videoMemory + (textBase - VideoMemoryBase) + pageOffset;
}

void PcMachine::renderText80Frame(uint16_t * dest, int destPitchPixels) const
{
  m_textRenderer.renderFrame(text80Buffer(), dest, destPitchPixels);
}

void PcMachine::renderText80Line(int y, uint16_t * dest) const
{
  m_textRenderer.renderLine(text80Buffer(), y, dest);
}

bool PcMachine::isVgaChain4Enabled() const
{
  return (m_sequencerRegisters[0x04] & 0x08) != 0;
}

bool PcMachine::isVgaOddEvenReadEnabled() const
{
  return !isVgaChain4Enabled() &&
         (m_graphicsRegisters[0x05] & 0x10) != 0;
}

bool PcMachine::isVgaOddEvenWriteEnabled() const
{
  return !isVgaChain4Enabled() &&
         (m_graphicsRegisters[0x05] & 0x10) != 0 &&
         (m_sequencerRegisters[0x04] & 0x04) == 0;
}

bool PcMachine::isVgaMemoryAccessEnabled() const
{
  switch (m_videoMode) {
    case VideoMode::Text80:
    case VideoMode::VgaGraphics320x200x16:
    case VideoMode::VgaGraphics640x200x16:
    case VideoMode::VgaGraphics640x350x2:
    case VideoMode::VgaGraphics640x350x16:
    case VideoMode::VgaGraphics640x480x2:
    case VideoMode::VgaGraphics640x480x16:
    case VideoMode::VgaGraphics320x200x256:
      return (m_miscOutputRegister & 0x02) != 0;
    case VideoMode::CgaGraphics320x200x4:
    case VideoMode::CgaGraphics640x200x2:
    case VideoMode::HerculesGraphics:
    default:
      return true;
  }
}

bool PcMachine::isVgaDisplayEnabled() const
{
  return (m_vgaEnableRegister & 0x01) != 0 &&
         m_attributeVideoEnabled &&
         (m_colorCrtcRegisters[0x17] & 0x80) != 0 &&
         (m_sequencerRegisters[0x00] & 0x03) == 0x03 &&
         (m_sequencerRegisters[0x01] & 0x20) == 0;
}

bool PcMachine::isVgaMonoCrtcSelected() const
{
  return m_videoMode != VideoMode::HerculesGraphics &&
         (readMemory16(0x0463) == 0x03b4 || (m_miscOutputRegister & 0x01) == 0);
}

uint8_t PcMachine::vgaInputStatus0() const
{
  // Input Status #0 bit 4 exposes the monitor sense switch selected by
  // Misc Output clock-select bits 2-3.  Use the BIOS EGA/VGA switch byte so
  // direct hardware probes and INT 10h AX=1210h observe the same adapter.
  uint8_t const selectedSwitch = static_cast<uint8_t>((m_miscOutputRegister >> 2) & 0x03);
  uint8_t const switches = static_cast<uint8_t>(readMemory8(0x0488) & 0x0f);
  return (switches & (1u << selectedSwitch)) ? 0x10 : 0x00;
}

bool PcMachine::usesPlanarVgaMemory() const
{
  // A mode-13h program that clears the sequencer chain-4 bit (SEQ 04 bit 3) is
  // running unchained 256-color "Mode X": the CPU aperture becomes planar and
  // the sequencer Map Mask selects the target plane per write. This routes such
  // access through the planar read/write engine (map mask, latches, write modes)
  // and the planar 256-color renderer, which together give page flipping and
  // latch-copy fills. No explicit INT 10h mode number exists for Mode X, so it
  // is detected purely from the register state below.
  return m_videoMode == VideoMode::VgaGraphics320x200x16 ||
         m_videoMode == VideoMode::VgaGraphics640x200x16 ||
         m_videoMode == VideoMode::VgaGraphics640x350x2 ||
         m_videoMode == VideoMode::VgaGraphics640x350x16 ||
         m_videoMode == VideoMode::VgaGraphics640x480x2 ||
         m_videoMode == VideoMode::VgaGraphics640x480x16 ||
         (m_videoMode == VideoMode::VgaGraphics320x200x256 && !isVgaChain4Enabled());
}

bool PcMachine::vgaChain4OffsetForAddress(uint32_t address, uint32_t * offset) const
{
  uint32_t mappedOffset = 0;
  switch ((m_graphicsRegisters[0x06] >> 2) & 0x03) {
    case 0x00: // A0000-BFFFF, 128 KiB CPU aperture aliased onto 64 KiB VGA memory
      if (address < 0x000a0000 || address >= 0x000c0000)
        return false;
      mappedOffset = (address - 0x000a0000) & 0xffff;
      break;
    case 0x01: // A0000-AFFFF, 64 KiB
      if (address < 0x000a0000 || address >= 0x000b0000)
        return false;
      mappedOffset = address - 0x000a0000;
      break;
    case 0x02: // B0000-B7FFF, 32 KiB
      if (address < 0x000b0000 || address >= 0x000b8000)
        return false;
      mappedOffset = address - 0x000b0000;
      break;
    case 0x03: // B8000-BFFFF, 32 KiB
      if (address < 0x000b8000 || address >= 0x000c0000)
        return false;
      mappedOffset = address - 0x000b8000;
      break;
  }

  if (offset)
    *offset = mappedOffset & 0xffff;
  return true;
}

bool PcMachine::vgaPlanarOffsetForAddress(uint32_t address, uint32_t * offset) const
{
  uint32_t mappedOffset = 0;
  switch ((m_graphicsRegisters[0x06] >> 2) & 0x03) {
    case 0x00: // A0000-BFFFF, 128 KiB CPU aperture aliased onto 64 KiB VGA planes
      if (address < 0x000a0000 || address >= 0x000c0000)
        return false;
      mappedOffset = (address - 0x000a0000) & 0xffff;
      break;
    case 0x01: // A0000-AFFFF, 64 KiB
      if (address < 0x000a0000 || address >= 0x000b0000)
        return false;
      mappedOffset = address - 0x000a0000;
      break;
    case 0x02: // B0000-B7FFF, 32 KiB
      if (address < 0x000b0000 || address >= 0x000b8000)
        return false;
      mappedOffset = address - 0x000b0000;
      break;
    case 0x03: // B8000-BFFFF, 32 KiB
      if (address < 0x000b8000 || address >= 0x000c0000)
        return false;
      mappedOffset = address - 0x000b8000;
      break;
  }

  if (offset)
    *offset = mappedOffset & 0xffff;
  return true;
}

uint16_t PcMachine::vgaCrtcStartAddress() const
{
  return static_cast<uint16_t>((static_cast<uint16_t>(m_colorCrtcRegisters[0x0c]) << 8) |
                               m_colorCrtcRegisters[0x0d]);
}

uint32_t PcMachine::vgaCrtcAddressUnitBytes() const
{
  if (m_colorCrtcRegisters[0x14] & 0x40)
    return 4;
  return (m_colorCrtcRegisters[0x17] & 0x40) ? 1 : 2;
}

uint32_t PcMachine::vgaCrtcStartOffsetBytes() const
{
  return static_cast<uint32_t>(vgaCrtcStartAddress()) * vgaCrtcAddressUnitBytes();
}

int PcMachine::vgaMode13Height() const
{
  int const maximumHeight = isVgaChain4Enabled() ? VgaGraphics256Height : VgaGraphics16Height;
  int height = static_cast<int>(vgaVerticalDisplayEnd()) + 1;
  if (height < VgaGraphics256Height)
    height = VgaGraphics256Height;
  if (height > maximumHeight)
    height = maximumHeight;
  return height;
}

int PcMachine::vgaPlanarWidth() const
{
  switch (m_videoMode) {
    case VideoMode::VgaGraphics320x200x16: return VgaGraphics16LowWidth;
    case VideoMode::VgaGraphics640x200x16:
    case VideoMode::VgaGraphics640x350x2:
    case VideoMode::VgaGraphics640x350x16:
    case VideoMode::VgaGraphics640x480x2:
    case VideoMode::VgaGraphics640x480x16: return VgaGraphics16Width;
    default: return 0;
  }
}

int PcMachine::vgaPlanarHeight() const
{
  switch (m_videoMode) {
    case VideoMode::VgaGraphics320x200x16:
    case VideoMode::VgaGraphics640x200x16: return VgaGraphics16LowHeight;
    case VideoMode::VgaGraphics640x350x2:
    case VideoMode::VgaGraphics640x350x16: return VgaGraphics16MediumHeight;
    case VideoMode::VgaGraphics640x480x2:
    case VideoMode::VgaGraphics640x480x16: return VgaGraphics16Height;
    default: return 0;
  }
}

uint32_t PcMachine::vgaCrtcOffsetBytes(uint32_t fallback) const
{
  uint32_t const registerBytes = static_cast<uint32_t>(m_colorCrtcRegisters[0x13]) *
                                 ((m_colorCrtcRegisters[0x14] & 0x40) ? 4u : 2u);
  return registerBytes ? registerBytes : fallback;
}

uint8_t PcMachine::vgaBytePanningBytes() const
{
  return static_cast<uint8_t>((m_colorCrtcRegisters[0x08] >> 5) & 0x03);
}

uint32_t PcMachine::vgaMode13Chain4LineOffsetBytes() const
{
  // In BIOS mode 13h the CRTC Offset register defaults to 40.  With
  // chain-4/doubleword addressing that represents 320 CPU-visible bytes per
  // displayed scan line.  Programs can increase it for virtual-width/page
  // effects while still showing a 320-pixel window.  If software changes the
  // CRTC address unit, keep the same relationship to the memory-address
  // counter instead of forcing the BIOS default dword stride.
  uint32_t const bytes = static_cast<uint32_t>(m_colorCrtcRegisters[0x13]) * vgaCrtcAddressUnitBytes() * 2u;
  return bytes ? bytes : VgaGraphics256Width;
}

uint8_t PcMachine::vgaPresetRowScan() const
{
  return m_colorCrtcRegisters[0x08] & 0x1f;
}

uint8_t PcMachine::vgaScanLinesPerRow() const
{
  uint8_t scanLines = static_cast<uint8_t>((m_colorCrtcRegisters[0x09] & 0x1f) + 1);
  if (m_colorCrtcRegisters[0x09] & 0x80)
    scanLines = static_cast<uint8_t>(scanLines * 2);
  return scanLines ? scanLines : 1;
}

int PcMachine::vgaSourceRowForDisplayLine(int y, bool splitLine) const
{
  int const scanLines = vgaScanLinesPerRow();
  int const rowScan = splitLine ? 0 : vgaPresetRowScan();
  return (y + rowScan) / scanLines;
}

uint16_t PcMachine::vgaVerticalDisplayEnd() const
{
  return static_cast<uint16_t>(m_colorCrtcRegisters[0x12] |
                               ((m_colorCrtcRegisters[0x07] & 0x02) << 7) |
                               ((m_colorCrtcRegisters[0x07] & 0x40) << 3));
}

uint16_t PcMachine::vgaLineCompare() const
{
  return static_cast<uint16_t>(m_colorCrtcRegisters[0x18] |
                               ((m_colorCrtcRegisters[0x07] & 0x10) << 4) |
                               ((m_colorCrtcRegisters[0x09] & 0x40) << 3));
}

int PcMachine::vgaHorizontalPanningPixels(bool splitLine) const
{
  if (splitLine && (m_attributeRegisters[0x10] & 0x20))
    return 0;

  uint8_t const pan = m_attributeRegisters[0x13] & 0x0f;
  if (m_videoMode == VideoMode::VgaGraphics320x200x256)
    return pan >> 1;
  return pan;
}

uint8_t PcMachine::vgaAttributeColorOutput(uint8_t attributeIndex) const
{
  uint8_t const enabledAttributeIndex = attributeIndex & (m_attributeRegisters[0x12] & 0x0f);
  uint8_t const paletteRegister = m_attributeRegisters[enabledAttributeIndex & 0x0f];
  uint8_t dacIndex = paletteRegister & 0x0f;
  if (m_attributeRegisters[0x10] & 0x80)
    dacIndex |= static_cast<uint8_t>((m_attributeRegisters[0x14] & 0x03) << 4);
  else
    dacIndex |= paletteRegister & 0x30;
  dacIndex |= static_cast<uint8_t>((m_attributeRegisters[0x14] & 0x0c) << 4);
  return dacIndex;
}

uint8_t PcMachine::vgaAttributePaletteLowNibble(uint8_t attributeIndex) const
{
  uint8_t const enabledAttributeIndex = attributeIndex & (m_attributeRegisters[0x12] & 0x0f);
  return static_cast<uint8_t>(m_attributeRegisters[enabledAttributeIndex & 0x0f] & 0x0f);
}

uint8_t PcMachine::vgaAttributeDacIndex(uint8_t attributeIndex) const
{
  return vgaAttributeColorOutput(attributeIndex) & m_dacPelMask;
}

uint8_t PcMachine::vgaMode13ColorOutput(uint8_t pixel) const
{
  if (m_attributeRegisters[0x10] & 0x40) {
    // In 256-color mode the Attribute Controller's EGA-compatible 16-entry
    // palette translation is bypassed; the byte fetched by the VGA shifter is
    // already the DAC index.  PEL mask is applied later by vgaMode13DacIndex().
    return pixel;
  }
  return vgaAttributeColorOutput(pixel & 0x0f);
}

uint8_t PcMachine::vgaMode13DacIndex(uint8_t pixel) const
{
  return vgaMode13ColorOutput(pixel) & m_dacPelMask;
}

uint8_t PcMachine::vgaPlanarDisplayIndex(uint32_t rowOffset, int sourceX) const
{
  if (!m_vgaPlaneMemory || sourceX < 0)
    return 0;

  uint8_t index = 0;
  if ((m_graphicsRegisters[0x05] & 0x60) == 0x20) {
    // Shift Register Interleave mode: the display shifter consumes two bits
    // at a time from planes 0/2 for pixels 0-3 and planes 1/3 for pixels
    // 4-7.  This is the VGA/CGA-compatible 4-color shifter path selected by
    // GC mode register bit 5 when 256-color shift (bit 6) is clear.
    uint32_t const offset = (rowOffset + static_cast<uint32_t>(sourceX >> 3)) & 0xffff;
    int const pixelInByte = sourceX & 0x07;
    int const pairShift = 6 - ((pixelInByte & 0x03) * 2);
    int const lowPlane = pixelInByte < 4 ? 0 : 1;
    int const highPlane = pixelInByte < 4 ? 2 : 3;
    uint8_t const lowPair = static_cast<uint8_t>((m_vgaPlaneMemory[lowPlane * 0x10000 + offset] >> pairShift) & 0x03);
    uint8_t const highPair = static_cast<uint8_t>((m_vgaPlaneMemory[highPlane * 0x10000 + offset] >> pairShift) & 0x03);
    index = static_cast<uint8_t>(lowPair | (highPair << 2));
  } else {
    uint32_t const offset = (rowOffset + static_cast<uint32_t>(sourceX >> 3)) & 0xffff;
    uint8_t const mask = static_cast<uint8_t>(0x80 >> (sourceX & 7));
    for (int plane = 0; plane < 4; ++plane) {
      if (m_vgaPlaneMemory[plane * 0x10000 + offset] & mask)
        index |= static_cast<uint8_t>(1 << plane);
    }
  }

  return index;
}

void PcMachine::vgaWritePlanarDisplayIndex(uint32_t rowOffset, int sourceX, uint8_t color)
{
  if (!m_vgaPlaneMemory || sourceX < 0)
    return;

  color &= 0x0f;
  if ((m_graphicsRegisters[0x05] & 0x60) == 0x20) {
    uint32_t const offset = (rowOffset + static_cast<uint32_t>(sourceX >> 3)) & 0xffff;
    int const pixelInByte = sourceX & 0x07;
    int const pairShift = 6 - ((pixelInByte & 0x03) * 2);
    int const lowPlane = pixelInByte < 4 ? 0 : 1;
    int const highPlane = pixelInByte < 4 ? 2 : 3;
    uint8_t const pairMask = static_cast<uint8_t>(0x03 << pairShift);
    uint8_t & lowByte = m_vgaPlaneMemory[lowPlane * 0x10000 + offset];
    uint8_t & highByte = m_vgaPlaneMemory[highPlane * 0x10000 + offset];
    lowByte = static_cast<uint8_t>((lowByte & ~pairMask) | ((color & 0x03) << pairShift));
    highByte = static_cast<uint8_t>((highByte & ~pairMask) | (((color >> 2) & 0x03) << pairShift));
    return;
  }

  uint32_t const offset = (rowOffset + static_cast<uint32_t>(sourceX >> 3)) & 0xffff;
  uint8_t const mask = static_cast<uint8_t>(0x80 >> (sourceX & 7));
  for (int plane = 0; plane < 4; ++plane) {
    uint8_t & byte = m_vgaPlaneMemory[plane * 0x10000 + offset];
    if (color & (1 << plane))
      byte |= mask;
    else
      byte &= static_cast<uint8_t>(~mask);
  }
}

uint8_t PcMachine::vgaStatusMuxBits() const
{
  uint8_t const color = vgaStatusMuxColor();
  uint8_t lowBit = 0;
  uint8_t highBit = 0;
  switch ((m_attributeRegisters[0x12] >> 4) & 0x03) {
    case 0x00:
      lowBit = color & 0x01;
      highBit = (color >> 2) & 0x01;
      break;
    case 0x01:
      lowBit = (color >> 4) & 0x01;
      highBit = (color >> 5) & 0x01;
      break;
    case 0x02:
      lowBit = (color >> 1) & 0x01;
      highBit = (color >> 3) & 0x01;
      break;
    case 0x03:
      lowBit = (color >> 6) & 0x01;
      highBit = (color >> 7) & 0x01;
      break;
  }
  return static_cast<uint8_t>((lowBit << 4) | (highBit << 5));
}

uint8_t PcMachine::vgaStatusMuxColor() const
{
  if (!isVgaDisplayEnabled())
    return 0;

  switch (m_videoMode) {
    case VideoMode::VgaGraphics320x200x256:
    {
      int const height = vgaMode13Height();
      uint16_t const lineCompare = vgaLineCompare();
      int constexpr sampleY = 0;
      bool const splitLine = lineCompare < height && sampleY >= lineCompare;
      int const displayY = splitLine ? sampleY - lineCompare : sampleY;
      int const sourceY = vgaSourceRowForDisplayLine(displayY, splitLine);
      int const sourceX = vgaHorizontalPanningPixels(splitLine);
      if (sourceX >= VgaGraphics256Width)
        return vgaOverscanColorOutput();
      uint8_t const bytePan = (splitLine && (m_attributeRegisters[0x10] & 0x20)) ? 0 : vgaBytePanningBytes();
      if (!isVgaChain4Enabled() && m_vgaPlaneMemory) {
        uint32_t const start = splitLine ? 0 : vgaCrtcStartOffsetBytes();
        uint32_t const lineBytes = vgaCrtcOffsetBytes(VgaGraphics256Width / 4);
        uint32_t const rowOffset = (start + static_cast<uint32_t>(sourceY) * lineBytes + bytePan) & 0xffff;
        uint32_t const offset = (rowOffset + static_cast<uint32_t>(sourceX >> 2)) & 0xffff;
        uint8_t const plane = static_cast<uint8_t>(sourceX & 0x03);
        uint8_t const pixel = m_vgaPlaneMemory[plane * 0x10000 + offset];
        return vgaMode13ColorOutput(pixel);
      }
      uint32_t const start = (splitLine ? 0 : vgaCrtcStartOffsetBytes()) + bytePan;
      uint32_t const lineBytes = vgaMode13Chain4LineOffsetBytes();
      uint32_t const offset = start + static_cast<uint32_t>(sourceY) * lineBytes + static_cast<uint32_t>(sourceX);
      uint8_t const pixel = vgaReadChain4Byte(offset);
      return vgaMode13ColorOutput(pixel);
    }
    case VideoMode::VgaGraphics320x200x16:
    case VideoMode::VgaGraphics640x200x16:
    case VideoMode::VgaGraphics640x350x2:
    case VideoMode::VgaGraphics640x350x16:
    case VideoMode::VgaGraphics640x480x2:
    case VideoMode::VgaGraphics640x480x16:
    {
      int const width = vgaPlanarWidth();
      int const height = vgaPlanarHeight();
      if (!m_vgaPlaneMemory || width <= 0 || height <= 0)
        return 0;
      uint16_t const lineCompare = vgaLineCompare();
      int constexpr sampleY = 0;
      bool const splitLine = lineCompare < height && sampleY >= lineCompare;
      int const displayY = splitLine ? sampleY - lineCompare : sampleY;
      int const sourceY = vgaSourceRowForDisplayLine(displayY, splitLine);
      int const sourceX = vgaHorizontalPanningPixels(splitLine);
      if (sourceX >= width)
        return vgaOverscanColorOutput();
      uint8_t const bytePan = (splitLine && (m_attributeRegisters[0x10] & 0x20)) ? 0 : vgaBytePanningBytes();
      uint32_t const start = splitLine ? 0 : vgaCrtcStartOffsetBytes();
      uint32_t const lineBytes = vgaCrtcOffsetBytes(static_cast<uint32_t>(width / 8));
      uint32_t const rowOffset = (start + static_cast<uint32_t>(sourceY) * lineBytes + bytePan) & 0xffff;
      return vgaAttributeColorOutput(vgaPlanarDisplayIndex(rowOffset, sourceX));
    }
    case VideoMode::CgaGraphics320x200x4:
    case VideoMode::CgaGraphics640x200x2:
    {
      uint8_t color = 0;
      if (readGraphicsPixel(0, 0, &color))
        return color;
      return 0;
    }
    case VideoMode::Text80:
    {
      uint8_t const * text = text80Buffer();
      return text ? vgaAttributeColorOutput(m_textRenderer.sampleColorIndex(text, 0, 0)) : 0;
    }
    case VideoMode::HerculesGraphics:
    default:
      return 0;
  }
}

uint8_t PcMachine::vgaOverscanColorOutput() const
{
  uint8_t index = m_attributeRegisters[0x11] & 0x3f;
  if (m_videoMode != VideoMode::VgaGraphics320x200x256) {
    if (m_attributeRegisters[0x10] & 0x80)
      index = static_cast<uint8_t>((index & 0x0f) | ((m_attributeRegisters[0x14] & 0x03) << 4));
    index |= static_cast<uint8_t>((m_attributeRegisters[0x14] & 0x0c) << 4);
  }
  return index;
}

uint8_t PcMachine::vgaOverscanDacIndex() const
{
  return vgaOverscanColorOutput() & m_dacPelMask;
}

uint16_t PcMachine::vgaOverscanRgb565() const
{
  return dacRgb565(vgaOverscanDacIndex());
}

uint16_t PcMachine::dacRgb565(uint8_t index) const
{
  uint8_t const maskedIndex = index & m_dacPelMask;
  return rgb565From6Bit(m_dacPalette[maskedIndex][0], m_dacPalette[maskedIndex][1], m_dacPalette[maskedIndex][2]);
}

void PcMachine::updateTextRendererPalette()
{
  for (uint8_t i = 0; i < 16; ++i)
    m_textRenderer.setColorRgb565(i, dacRgb565(vgaAttributeDacIndex(i)));
  m_textRenderer.setOverscanColorRgb565(vgaOverscanRgb565());
}

uint16_t PcMachine::cgaRgb565(uint8_t index) const
{
  // VGA-compatible CGA modes still drive the external DAC, so INT 10h DAC
  // services and the PEL mask must affect 320x200/640x200 CGA rendering too.
  return dacRgb565(index & 0x0f);
}

void PcMachine::renderGraphicsFrame(uint16_t * dest, int destPitchPixels) const
{
  if (!dest)
    return;
  int const height = graphicsHeight();
  for (int y = 0; y < height; ++y)
    renderGraphicsLine(y, dest + y * destPitchPixels);
}

void PcMachine::renderGraphicsLine(int y, uint16_t * dest) const
{
  renderGraphicsLineInternal(y, dest, false);
}

void PcMachine::renderGraphicsFrameForDisplay(uint16_t * dest, int destPitchPixels) const
{
  if (!dest)
    return;
  int const height = graphicsHeight();
  for (int y = 0; y < height; ++y)
    renderGraphicsLineForDisplay(y, dest + y * destPitchPixels);
}

void PcMachine::renderGraphicsLineForDisplay(int y, uint16_t * dest) const
{
  renderGraphicsLineInternal(y, dest, true);
}

void PcMachine::renderGraphicsLineInternal(int y, uint16_t * dest, bool forceCgaDisplayEnable) const
{
  switch (m_videoMode) {
    case VideoMode::CgaGraphics320x200x4: renderCga320Line(y, dest, forceCgaDisplayEnable); return;
    case VideoMode::CgaGraphics640x200x2: renderCga640Line(y, dest, forceCgaDisplayEnable); return;
    case VideoMode::HerculesGraphics: renderHerculesGraphicsLine(y, dest); return;
    case VideoMode::VgaGraphics320x200x16:
    case VideoMode::VgaGraphics640x200x16:
    case VideoMode::VgaGraphics640x350x2:
    case VideoMode::VgaGraphics640x350x16:
    case VideoMode::VgaGraphics640x480x2:
    case VideoMode::VgaGraphics640x480x16: renderVgaPlanar16Line(y, dest); return;
    case VideoMode::VgaGraphics320x200x256: renderVgaMode13Line(y, dest); return;
    case VideoMode::Text80:
    default: renderText80Line(y, dest); return;
  }
}

bool PcMachine::readGraphicsPixel(int x, int y, uint8_t * color, uint16_t pageOffset) const
{
  if (!color || x < 0 || y < 0)
    return false;

  switch (m_videoMode) {
    case VideoMode::CgaGraphics320x200x4:
    {
      if (!m_videoMemory || x >= CgaGraphics320Width || y >= CgaGraphicsHeight)
        return false;
      uint32_t const offset = (TextColorMemoryBase - VideoMemoryBase) + pageOffset + ((y & 1) << 13) +
                              (CgaGraphics320Width / 4) * (y >> 1) + static_cast<uint32_t>(x >> 2);
      if (!isRangeValid(offset, 1, VideoMemorySize))
        return false;
      uint8_t const shift = static_cast<uint8_t>((3 - (x & 3)) * 2);
      *color = static_cast<uint8_t>((m_videoMemory[offset] >> shift) & 0x03);
      return true;
    }
    case VideoMode::CgaGraphics640x200x2:
    {
      if (!m_videoMemory || x >= CgaGraphicsWidth || y >= CgaGraphicsHeight)
        return false;
      uint32_t const offset = (TextColorMemoryBase - VideoMemoryBase) + pageOffset + ((y & 1) << 13) +
                              (CgaGraphicsWidth / 8) * (y >> 1) + static_cast<uint32_t>(x >> 3);
      if (!isRangeValid(offset, 1, VideoMemorySize))
        return false;
      *color = (m_videoMemory[offset] & (0x80 >> (x & 7))) ? 1 : 0;
      return true;
    }
    case VideoMode::VgaGraphics320x200x256:
    {
      int const height = vgaMode13Height();
      if (x >= VgaGraphics256Width || y >= height)
        return false;
      if (!isVgaChain4Enabled() && m_vgaPlaneMemory) {
        uint32_t const lineBytes = vgaCrtcOffsetBytes(VgaGraphics256Width / 4);
        uint32_t const offset = (pageOffset + static_cast<uint32_t>(y) * lineBytes + static_cast<uint32_t>(x >> 2)) & 0xffff;
        *color = m_vgaPlaneMemory[(x & 0x03) * 0x10000 + offset];
        return true;
      }
      if (!m_videoMemory)
        return false;
      uint32_t const lineBytes = vgaMode13Chain4LineOffsetBytes();
      uint32_t const offset = pageOffset + static_cast<uint32_t>(y) * lineBytes + static_cast<uint32_t>(x);
      *color = vgaReadChain4Byte(offset);
      return true;
    }
    case VideoMode::VgaGraphics320x200x16:
    case VideoMode::VgaGraphics640x200x16:
    case VideoMode::VgaGraphics640x350x2:
    case VideoMode::VgaGraphics640x350x16:
    case VideoMode::VgaGraphics640x480x2:
    case VideoMode::VgaGraphics640x480x16:
    {
      int const width = vgaPlanarWidth();
      int const height = vgaPlanarHeight();
      if (!m_vgaPlaneMemory || width <= 0 || height <= 0 || x >= width || y >= height)
        return false;
      uint32_t const lineBytes = vgaCrtcOffsetBytes(static_cast<uint32_t>(width / 8));
      uint32_t const rowOffset = (pageOffset + static_cast<uint32_t>(y) * lineBytes) & 0xffff;
      *color = vgaPlanarDisplayIndex(rowOffset, x);
      return true;
    }
    case VideoMode::HerculesGraphics:
    {
      if (!m_videoMemory || x >= HerculesGraphicsWidth || y >= HerculesGraphicsHeight)
        return false;
      uint32_t const herculesPageOffset = pageOffset & 0x8000;
      uint32_t const offset = herculesPageOffset + ((y & 0x03) << 13) +
                              HerculesGraphicsBytesPerLine * (y >> 2) + static_cast<uint32_t>(x >> 3);
      uint32_t const memoryOffset = HerculesGraphicsMemoryBase - VideoMemoryBase + offset;
      if (!isRangeValid(memoryOffset, 1, VideoMemorySize))
        return false;
      *color = (m_videoMemory[memoryOffset] & (0x80 >> (x & 7))) ? 1 : 0;
      return true;
    }
    case VideoMode::Text80:
    default:
      return false;
  }
}

bool PcMachine::writeGraphicsPixel(int x, int y, uint8_t color, bool xorMode, uint16_t pageOffset)
{
  if (x < 0 || y < 0)
    return false;

  uint8_t current = 0;
  if (xorMode && readGraphicsPixel(x, y, &current, pageOffset))
    color ^= current;

  switch (m_videoMode) {
    case VideoMode::CgaGraphics320x200x4:
    {
      if (!m_videoMemory || x >= CgaGraphics320Width || y >= CgaGraphicsHeight)
        return false;
      uint32_t const offset = (TextColorMemoryBase - VideoMemoryBase) + pageOffset + ((y & 1) << 13) +
                              (CgaGraphics320Width / 4) * (y >> 1) + static_cast<uint32_t>(x >> 2);
      if (!isRangeValid(offset, 1, VideoMemorySize))
        return false;
      uint8_t const shift = static_cast<uint8_t>((3 - (x & 3)) * 2);
      uint8_t const mask = static_cast<uint8_t>(0x03 << shift);
      m_videoMemory[offset] = static_cast<uint8_t>((m_videoMemory[offset] & ~mask) | ((color & 0x03) << shift));
      return true;
    }
    case VideoMode::CgaGraphics640x200x2:
    {
      if (!m_videoMemory || x >= CgaGraphicsWidth || y >= CgaGraphicsHeight)
        return false;
      uint32_t const offset = (TextColorMemoryBase - VideoMemoryBase) + pageOffset + ((y & 1) << 13) +
                              (CgaGraphicsWidth / 8) * (y >> 1) + static_cast<uint32_t>(x >> 3);
      if (!isRangeValid(offset, 1, VideoMemorySize))
        return false;
      uint8_t const mask = static_cast<uint8_t>(0x80 >> (x & 7));
      if (color & 1)
        m_videoMemory[offset] |= mask;
      else
        m_videoMemory[offset] &= static_cast<uint8_t>(~mask);
      return true;
    }
    case VideoMode::VgaGraphics320x200x256:
    {
      int const height = vgaMode13Height();
      if (x >= VgaGraphics256Width || y >= height)
        return false;
      if (!isVgaChain4Enabled() && m_vgaPlaneMemory) {
        uint32_t const lineBytes = vgaCrtcOffsetBytes(VgaGraphics256Width / 4);
        uint32_t const offset = (pageOffset + static_cast<uint32_t>(y) * lineBytes + static_cast<uint32_t>(x >> 2)) & 0xffff;
        m_vgaPlaneMemory[(x & 0x03) * 0x10000 + offset] = color;
        return true;
      }
      if (!m_videoMemory)
        return false;
      uint32_t const lineBytes = vgaMode13Chain4LineOffsetBytes();
      uint32_t const offset = pageOffset + static_cast<uint32_t>(y) * lineBytes + static_cast<uint32_t>(x);
      vgaWriteChain4Byte(offset, color);
      return true;
    }
    case VideoMode::VgaGraphics320x200x16:
    case VideoMode::VgaGraphics640x200x16:
    case VideoMode::VgaGraphics640x350x2:
    case VideoMode::VgaGraphics640x350x16:
    case VideoMode::VgaGraphics640x480x2:
    case VideoMode::VgaGraphics640x480x16:
    {
      int const width = vgaPlanarWidth();
      int const height = vgaPlanarHeight();
      if (!m_vgaPlaneMemory || width <= 0 || height <= 0 || x >= width || y >= height)
        return false;
      uint32_t const lineBytes = vgaCrtcOffsetBytes(static_cast<uint32_t>(width / 8));
      uint32_t const rowOffset = (pageOffset + static_cast<uint32_t>(y) * lineBytes) & 0xffff;
      vgaWritePlanarDisplayIndex(rowOffset, x, color);
      return true;
    }
    case VideoMode::HerculesGraphics:
    {
      if (!m_videoMemory || x >= HerculesGraphicsWidth || y >= HerculesGraphicsHeight)
        return false;
      uint32_t const herculesPageOffset = pageOffset & 0x8000;
      uint32_t const offset = herculesPageOffset + ((y & 0x03) << 13) +
                              HerculesGraphicsBytesPerLine * (y >> 2) + static_cast<uint32_t>(x >> 3);
      uint32_t const memoryOffset = HerculesGraphicsMemoryBase - VideoMemoryBase + offset;
      if (!isRangeValid(memoryOffset, 1, VideoMemorySize))
        return false;
      uint8_t const mask = static_cast<uint8_t>(0x80 >> (x & 7));
      if (color & 1)
        m_videoMemory[memoryOffset] |= mask;
      else
        m_videoMemory[memoryOffset] &= static_cast<uint8_t>(~mask);
      return true;
    }
    case VideoMode::Text80:
    default:
      return false;
  }
}

void PcMachine::renderCga320Line(int y, uint16_t * dest, bool forceDisplayEnable) const
{
  if (!dest)
    return;
  if ((!forceDisplayEnable && (m_cgaModeControl & 0x08) == 0) || y < 0 || y >= CgaGraphicsHeight) {
    for (int x = 0; x < CgaGraphics320Width; ++x)
      dest[x] = 0;
    return;
  }
  uint32_t const bdaPageOffset = readMemory16(0x044e);
  uint32_t pageOffset = vgaCrtcStartAddress();
  if (pageOffset == 0 && bdaPageOffset != 0)
    pageOffset = bdaPageOffset;
  uint32_t const offset = (TextColorMemoryBase - VideoMemoryBase) + pageOffset +
                          ((y & 1) << 13) + (CgaGraphics320Width / 4) * (y >> 1);
  if (!m_videoMemory || !isRangeValid(offset, CgaGraphics320Width / 4, VideoMemorySize)) {
    for (int x = 0; x < CgaGraphics320Width; ++x)
      dest[x] = 0;
    return;
  }
  uint16_t palette[4];
  static uint8_t const cgaPalettes[4][4] = {
    {0, 2, 4, 6}, {0, 3, 5, 7}, {0, 10, 12, 14}, {0, 11, 13, 15},
  };
  uint8_t const background = m_cgaColorSelect & 0x0f;
  uint8_t const paletteIndex = static_cast<uint8_t>(((m_cgaColorSelect & 0x10) ? 2 : 0) |
                                                    ((m_cgaColorSelect & 0x20) ? 1 : 0));
  palette[0] = cgaRgb565(background);
  if (m_cgaModeControl & 0x04) {
    uint8_t const intensity = (m_cgaColorSelect & 0x10) ? 0x08 : 0x00;
    palette[1] = cgaRgb565(static_cast<uint8_t>(0x03 | intensity));
    palette[2] = cgaRgb565(static_cast<uint8_t>(0x04 | intensity));
    palette[3] = cgaRgb565(static_cast<uint8_t>(0x07 | intensity));
  } else {
    for (int i = 1; i < 4; ++i)
      palette[i] = cgaRgb565(cgaPalettes[paletteIndex][i]);
  }

  uint8_t const * src = m_videoMemory + offset;
  for (int byte = 0; byte < CgaGraphics320Width / 4; ++byte) {
    uint8_t value = src[byte];
    dest[byte * 4 + 0] = palette[(value >> 6) & 0x03];
    dest[byte * 4 + 1] = palette[(value >> 4) & 0x03];
    dest[byte * 4 + 2] = palette[(value >> 2) & 0x03];
    dest[byte * 4 + 3] = palette[value & 0x03];
  }
}

void PcMachine::renderCga640Line(int y, uint16_t * dest, bool forceDisplayEnable) const
{
  if (!dest)
    return;
  if ((!forceDisplayEnable && (m_cgaModeControl & 0x08) == 0) || y < 0 || y >= CgaGraphicsHeight) {
    for (int x = 0; x < CgaGraphicsWidth; ++x)
      dest[x] = 0;
    return;
  }
  uint32_t const bdaPageOffset = readMemory16(0x044e);
  uint32_t pageOffset = vgaCrtcStartAddress();
  if (pageOffset == 0 && bdaPageOffset != 0)
    pageOffset = bdaPageOffset;
  uint32_t const offset = (TextColorMemoryBase - VideoMemoryBase) + pageOffset +
                          ((y & 1) << 13) + (CgaGraphicsWidth / 8) * (y >> 1);
  if (!m_videoMemory || !isRangeValid(offset, CgaGraphicsWidth / 8, VideoMemorySize)) {
    for (int x = 0; x < CgaGraphicsWidth; ++x)
      dest[x] = 0;
    return;
  }
  uint16_t const bg = cgaRgb565(0);
  uint16_t const fg = cgaRgb565(m_cgaColorSelect & 0x0f);
  uint8_t const * src = m_videoMemory + offset;
  for (int byte = 0; byte < CgaGraphicsWidth / 8; ++byte) {
    uint8_t bits = src[byte];
    for (int bit = 0; bit < 8; ++bit)
      dest[byte * 8 + bit] = (bits & (0x80 >> bit)) ? fg : bg;
  }
}

void PcMachine::renderVgaMode13Line(int y, uint16_t * dest) const
{
  if (!dest)
    return;
  int const height = vgaMode13Height();
  if (!isVgaDisplayEnabled() || !m_videoMemory || y < 0 || y >= height) {
    for (int x = 0; x < VgaGraphics256Width; ++x)
      dest[x] = 0;
    return;
  }
  if (y > vgaVerticalDisplayEnd()) {
    uint16_t const overscan = vgaOverscanRgb565();
    for (int x = 0; x < VgaGraphics256Width; ++x)
      dest[x] = overscan;
    return;
  }
  uint16_t const lineCompare = vgaLineCompare();
  bool const splitLine = lineCompare < height && y >= lineCompare;
  int const displayY = splitLine ? y - lineCompare : y;
  int const sourceY = vgaSourceRowForDisplayLine(displayY, splitLine);
  int const pan = vgaHorizontalPanningPixels(splitLine);
  uint8_t const bytePan = (splitLine && (m_attributeRegisters[0x10] & 0x20)) ? 0 : vgaBytePanningBytes();
  if (!isVgaChain4Enabled() && m_vgaPlaneMemory) {
    uint32_t const start = splitLine ? 0 : vgaCrtcStartOffsetBytes();
    uint32_t const lineBytes = vgaCrtcOffsetBytes(VgaGraphics256Width / 4);
    uint32_t const rowOffset = (start + static_cast<uint32_t>(sourceY) * lineBytes + bytePan) & 0xffff;
    for (int x = 0; x < VgaGraphics256Width; ++x) {
      int const sourceX = x + pan;
      if (sourceX >= VgaGraphics256Width) {
        dest[x] = vgaOverscanRgb565();
        continue;
      }
      uint32_t const offset = (rowOffset + static_cast<uint32_t>(sourceX >> 2)) & 0xffff;
      uint8_t const plane = static_cast<uint8_t>(sourceX & 0x03);
      uint8_t const pixel = m_vgaPlaneMemory[plane * 0x10000 + offset];
      dest[x] = dacRgb565(vgaMode13DacIndex(pixel));
    }
    return;
  }

  uint32_t const start = (splitLine ? 0 : vgaCrtcStartOffsetBytes()) + bytePan;
  uint32_t const lineBytes = vgaMode13Chain4LineOffsetBytes();
  for (int x = 0; x < VgaGraphics256Width; ++x) {
    int const sourceX = x + pan;
    if (sourceX >= VgaGraphics256Width) {
      dest[x] = vgaOverscanRgb565();
      continue;
    }
    uint32_t const offset = start + static_cast<uint32_t>(sourceY) * lineBytes + static_cast<uint32_t>(sourceX);
    uint8_t const pixel = vgaReadChain4Byte(offset);
    dest[x] = dacRgb565(vgaMode13DacIndex(pixel));
  }
}

void PcMachine::renderVgaPlanar16Line(int y, uint16_t * dest) const
{
  if (!dest)
    return;
  int const width = vgaPlanarWidth();
  int const height = vgaPlanarHeight();
  if (width <= 0)
    return;
  if (!isVgaDisplayEnabled() || !m_vgaPlaneMemory || y < 0 || y >= height) {
    for (int x = 0; x < width; ++x)
      dest[x] = 0;
    return;
  }
  if (y > vgaVerticalDisplayEnd()) {
    uint16_t const overscan = vgaOverscanRgb565();
    for (int x = 0; x < width; ++x)
      dest[x] = overscan;
    return;
  }
  uint16_t const lineCompare = vgaLineCompare();
  bool const splitLine = lineCompare < height && y >= lineCompare;
  int const displayY = splitLine ? y - lineCompare : y;
  int const sourceY = vgaSourceRowForDisplayLine(displayY, splitLine);
  int const pan = vgaHorizontalPanningPixels(splitLine);
  uint8_t const bytePan = (splitLine && (m_attributeRegisters[0x10] & 0x20)) ? 0 : vgaBytePanningBytes();
  uint32_t const start = splitLine ? 0 : vgaCrtcStartOffsetBytes();
  uint32_t const lineBytes = vgaCrtcOffsetBytes(static_cast<uint32_t>(width / 8));
  uint32_t const rowOffset = (start + static_cast<uint32_t>(sourceY) * lineBytes + bytePan) & 0xffff;
  for (int byte = 0; byte < width / 8; ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      int const destX = byte * 8 + bit;
      int const sourceX = destX + pan;
      if (sourceX >= width) {
        dest[destX] = vgaOverscanRgb565();
        continue;
      }
      uint8_t const dacIndex = vgaAttributeDacIndex(vgaPlanarDisplayIndex(rowOffset, sourceX));
      dest[destX] = dacRgb565(dacIndex);
    }
  }
}

void PcMachine::renderHerculesGraphicsFrame(uint16_t * dest, int destPitchPixels) const
{
  if (!dest)
    return;
  for (int y = 0; y < HerculesGraphicsHeight; ++y)
    renderHerculesGraphicsLine(y, dest + y * destPitchPixels);
}

void PcMachine::renderHerculesGraphicsLine(int y, uint16_t * dest) const
{
  if (!dest)
    return;

  uint16_t const black = 0x0000;
  uint16_t const white = 0xffff;
  if (!m_herculesVideoEnabled || !m_videoMemory || y < 0 || y >= HerculesGraphicsHeight) {
    for (int x = 0; x < HerculesGraphicsWidth; ++x)
      dest[x] = black;
    return;
  }

  // Hercules Graphics Card memory layout matches FabGL GraphicsAdapter:
  // offset 0x0000 for scanlines 0,4,8...
  // offset 0x2000 for scanlines 1,5,9...
  // offset 0x4000 for scanlines 2,6,10...
  // offset 0x6000 for scanlines 3,7,11...
  uint32_t const pageOffset = ((m_herculesConfigRegister & 0x02) && (m_herculesModeControl & 0x80)) ? 0x8000 : 0x0000;
  uint32_t const offset = pageOffset + ((y & 0x03) << 13) + HerculesGraphicsBytesPerLine * (y >> 2);
  if (!isRangeValid(HerculesGraphicsMemoryBase - VideoMemoryBase + offset, HerculesGraphicsBytesPerLine, VideoMemorySize)) {
    for (int x = 0; x < HerculesGraphicsWidth; ++x)
      dest[x] = black;
    return;
  }
  uint8_t const * src = m_videoMemory + (HerculesGraphicsMemoryBase - VideoMemoryBase) + offset;
  for (int byte = 0; byte < HerculesGraphicsBytesPerLine; ++byte) {
    uint8_t bits = src[byte];
    for (int bit = 0; bit < 8; ++bit)
      dest[byte * 8 + bit] = (bits & (0x80 >> bit)) ? white : black;
  }
}

bool PcMachine::isRangeValid(uint32_t address, size_t size, size_t limit)
{
  return size <= limit && address <= limit - size;
}

uint8_t PcMachine::readPortCallback(void * context, int address)
{
  return static_cast<PcMachine *>(context)->readPort(static_cast<uint16_t>(address));
}

void PcMachine::writePortCallback(void * context, int address, uint8_t value)
{
  static_cast<PcMachine *>(context)->writePort(static_cast<uint16_t>(address), value);
}

uint8_t PcMachine::readVideoMemory8Callback(void * context, int address)
{
  return static_cast<PcMachine *>(context)->readVideoMemory8(static_cast<uint32_t>(address));
}

uint16_t PcMachine::readVideoMemory16Callback(void * context, int address)
{
  return static_cast<PcMachine *>(context)->readVideoMemory16(static_cast<uint32_t>(address));
}

void PcMachine::writeVideoMemory8Callback(void * context, int address, uint8_t value)
{
  static_cast<PcMachine *>(context)->writeVideoMemory8(static_cast<uint32_t>(address), value);
}

void PcMachine::writeVideoMemory16Callback(void * context, int address, uint16_t value)
{
  static_cast<PcMachine *>(context)->writeVideoMemory16(static_cast<uint32_t>(address), value);
}

bool PcMachine::interruptCallback(void * context, int interruptNumber)
{
  auto * machine = static_cast<PcMachine *>(context);
  bool const handled = machine->m_bios.handleInterrupt(*machine, interruptNumber);
  if (!handled) {
    uint16_t const vectorOffset = machine->readMemory16(static_cast<uint32_t>(interruptNumber) * 4 + 0);
    uint16_t const vectorSegment = machine->readMemory16(static_cast<uint32_t>(interruptNumber) * 4 + 2);
    bool const hasVector = vectorOffset != 0 || vectorSegment != 0;
    if (!hasVector && interruptNumber != 0x09 && ((interruptNumber > 0x07 && interruptNumber < 0x20) || interruptNumber > 0x2f))
      machine->recordUnsupportedInterrupt(interruptNumber);
  }
  return handled;
}

void PcMachine::recordUnsupportedInterrupt(int interruptNumber)
{
  ++m_diagnostics.unsupportedInterruptCount;
  m_diagnostics.lastUnsupportedInterrupt = static_cast<uint8_t>(interruptNumber);
  m_diagnostics.lastUnsupportedInterruptAh = PcI8086::AH();
}

void PcMachine::unsupportedOpcodeCallback(void * context, uint16_t cs, uint16_t ip, uint8_t op0, uint8_t op1)
{
  auto * machine = static_cast<PcMachine *>(context);
  ++machine->m_diagnostics.unsupportedOpcodeCount;
  machine->m_diagnostics.lastUnsupportedOpcodeCs = cs;
  machine->m_diagnostics.lastUnsupportedOpcodeIp = ip;
  machine->m_diagnostics.lastUnsupportedOpcode0 = op0;
  machine->m_diagnostics.lastUnsupportedOpcode1 = op1;
}

void PcMachine::recordUnsupportedPortRead(uint16_t port)
{
  ++m_diagnostics.unsupportedPortReadCount;
  m_diagnostics.lastUnsupportedPortRead = port;
}

void PcMachine::recordUnsupportedPortWrite(uint16_t port)
{
  ++m_diagnostics.unsupportedPortWriteCount;
  m_diagnostics.lastUnsupportedPortWrite = port;
}

void PcMachine::recordDiskWrite(uint8_t biosDrive, uint64_t lba, uint8_t count)
{
  ++m_diagnostics.diskWriteCount;
  m_diagnostics.lastDiskWriteDrive = biosDrive;
  m_diagnostics.lastDiskWriteLba = lba;
  m_diagnostics.lastDiskWriteCount = count;
}

void PcMachine::setEquipmentVideoBits(uint16_t videoBits)
{
  uint16_t const equipment = readMemory16(0x0410);
  writeMemory16(0x0410, static_cast<uint16_t>((equipment & ~0x0030) | (videoBits & 0x0030)));
}

void PcMachine::syncCgaModeBiosDataArea(VideoMode mode)
{
  uint8_t biosMode = 0x03;
  uint16_t columns = PcTextRenderer::Columns;
  uint16_t pageSize = 0x1000;
  if (mode == VideoMode::CgaGraphics320x200x4) {
    biosMode = 0x04;
    columns = 40;
    pageSize = 0x4000;
  } else if (mode == VideoMode::CgaGraphics640x200x2) {
    biosMode = 0x06;
    columns = 80;
    pageSize = 0x4000;
  }

  writeMemory8(0x0449, biosMode);
  writeMemory16(0x044a, columns);
  writeMemory16(0x044c, pageSize);
  writeMemory16(0x044e, 0x0000);
  writeMemory8(0x0462, 0x00);
  writeMemory16(0x0463, 0x03d4);
  writeMemory8(0x0465, m_cgaModeControl);
  writeMemory8(0x0466, m_cgaColorSelect);
}

void PcMachine::initializeBiosDataArea()
{
  writeMemory16(0x0008 * 4 + 0, BiosTimerIrqOffset);
  writeMemory16(0x0008 * 4 + 2, BiosTimerIrqSegment);
  writeMemory16(0x0009 * 4 + 0, BiosKeyboardIrqOffset);
  writeMemory16(0x0009 * 4 + 2, BiosKeyboardIrqSegment);
  writeMemory16(0x0010 * 4 + 0, BiosVideoServiceOffset);
  writeMemory16(0x0010 * 4 + 2, BiosVideoServiceSegment);
  writeMemory16(0x0016 * 4 + 0, BiosKeyboardServiceOffset);
  writeMemory16(0x0016 * 4 + 2, BiosKeyboardServiceSegment);
  writeMemory16(0x001c * 4 + 0, BiosUserTimerOffset);
  writeMemory16(0x001c * 4 + 2, BiosUserTimerSegment);
  writeMemory16(0x001f * 4 + 0, BiosFont8x8Offset);
  writeMemory16(0x001f * 4 + 2, BiosFont8x8Segment);
  writeMemory16(0x0043 * 4 + 0, BiosFont8x16Offset);
  writeMemory16(0x0043 * 4 + 2, BiosFont8x16Segment);

  writeBiosFontImage(*this, BiosFont8x8Linear, 8);
  writeBiosFontImage(*this, BiosFont8x14Linear, 14);
  writeBiosFontImage(*this, BiosFont8x16Linear, 16);

  auto writeBiosCompatibleStub = [this](uint32_t linearAddress, uint8_t privateInterrupt) {
    static constexpr uint8_t StubPrefix[] = {
      0xcd, 0x00,                         // int privateInterrupt; patched below
      0x55,                               // push bp
      0x89, 0xe5,                         // mov bp, sp
      0xf7, 0x46, 0x06, 0x00, 0xf0,       // test word [bp + 6], 0xf000
      0x5d,                               // pop bp
      0x75, 0x01,                         // jnz iret
      0xcb,                               // retf
      0xcf,                               // iret
    };
    for (size_t i = 0; i < sizeof(StubPrefix); ++i)
      writeMemory8(linearAddress + static_cast<uint32_t>(i), i == 1 ? privateInterrupt : StubPrefix[i]);
  };

  uint32_t const keyboardIrqLinear = (static_cast<uint32_t>(BiosKeyboardIrqSegment) << 4) + BiosKeyboardIrqOffset;
  // Some DOS programs hook INT 09h and chain to the previous BIOS keyboard
  // handler with a plain FAR CALL instead of the safer PUSHF/CALL or JMP.
  // Use a small compatibility stub: service the scancode via a private BIOS
  // interrupt, then return with IRET when the caller supplied an interrupt
  // frame/FLAGS word at [SP+4], otherwise return as a normal far subroutine.
  writeBiosCompatibleStub(keyboardIrqLinear, BiosKeyboardInternalInterrupt);
  uint32_t const keyboardServiceLinear = (static_cast<uint32_t>(BiosKeyboardServiceSegment) << 4) + BiosKeyboardServiceOffset;
  writeBiosCompatibleStub(keyboardServiceLinear, BiosKeyboardServiceInterrupt);
  uint32_t const timerIrqLinear = (static_cast<uint32_t>(BiosTimerIrqSegment) << 4) + BiosTimerIrqOffset;
  writeBiosCompatibleStub(timerIrqLinear, BiosTimerInternalInterrupt);
  uint32_t const userTimerLinear = (static_cast<uint32_t>(BiosUserTimerSegment) << 4) + BiosUserTimerOffset;
  writeMemory8(userTimerLinear, 0xcf); // default INT 1Ch user hook is an IRET
  uint32_t const videoServiceLinear = (static_cast<uint32_t>(BiosVideoServiceSegment) << 4) + BiosVideoServiceOffset;
  writeBiosCompatibleStub(videoServiceLinear, BiosVideoServiceInterrupt);

  writeMemory16(0x0410, 0x0021); // equipment word: floppy + 80x25 color
  writeMemory16(0x0413, 640);    // conventional memory size in KB
  writeMemory16(0x041a, 0x001e); // keyboard buffer head
  writeMemory16(0x041c, 0x001e); // keyboard buffer tail
  writeMemory8(0x0449, 0x03);    // current video mode
  writeMemory16(0x044a, PcTextRenderer::Columns);
  writeMemory16(0x044c, 0x1000); // text page size
  writeMemory16(0x044e, 0x0000); // active page offset
  for (int page = 0; page < 8; ++page)
    writeMemory16(0x0450 + page * 2, 0x0000);
  writeMemory16(0x0460, 0x0607); // cursor shape
  writeMemory8(0x0462, 0x00);    // active display page
  writeMemory16(0x0463, 0x03d4); // color CRTC base port
  writeMemory8(0x0465, 0x29);    // CGA/VGA mode control mirror: 80-col text + blink
  writeMemory8(0x0466, 0x00);    // CGA color-select mirror
  writeMemory16(0x046c, 0x0000); // timer tick low
  writeMemory16(0x046e, 0x0000); // timer tick high
  writeMemory8(0x0470, 0x00);    // midnight rollover
  uint8_t fixedDiskCount = 0;
  for (int index = 2; index < DiskCount; ++index) {
    if (m_disks[index].isOpen())
      ++fixedDiskCount;
  }
  writeMemory8(0x0475, fixedDiskCount); // fixed disk count for BIOS drive 80h+
  writeMemory8(0x0484, PcTextRenderer::Rows - 1);
  writeMemory16(0x0485, PcTextRenderer::CellHeight);
  writeMemory8(0x0487, 0x60); // EGA/VGA control: 256 KiB RAM, active color display
  writeMemory8(0x0488, 0x09); // EGA/VGA switches: enhanced color display, primary EGA/VGA
  writeMemory8(0x0489, 0x11); // VGA active, 400-line mode-set option

  static constexpr uint32_t VideoBiosRomBase = 0x000c0000;
  static constexpr size_t VideoBiosRomSize = 2048;
  static constexpr char VideoBiosTitle[] = "TabDOS VGA BIOS";
  static constexpr char VideoBiosVersion[] = "Version 1.0";
  static constexpr char VideoBiosDate[] = "06/19/26";

  for (size_t i = 0; i < VideoBiosRomSize; ++i)
    writeMemory8(VideoBiosRomBase + static_cast<uint32_t>(i), 0);
  writeMemory8(VideoBiosRomBase + 0x00, 0x55);
  writeMemory8(VideoBiosRomBase + 0x01, 0xaa);
  writeMemory8(VideoBiosRomBase + 0x02, static_cast<uint8_t>(VideoBiosRomSize / 512));
  writeMemory8(VideoBiosRomBase + 0x03, 0xcb); // retf: safe no-op option-ROM init entry
  auto writeVideoBiosString = [this](uint32_t offset, char const * text) {
    for (uint32_t i = 0; text[i]; ++i)
      writeMemory8(VideoBiosRomBase + offset + i, static_cast<uint8_t>(text[i]));
  };
  writeVideoBiosString(0x0030, VideoBiosTitle);
  writeVideoBiosString(0x0050, VideoBiosVersion);
  writeVideoBiosString(VideoBiosRomSize - sizeof(VideoBiosDate), VideoBiosDate);
  uint8_t checksum = 0;
  for (size_t i = 0; i < VideoBiosRomSize - 1; ++i)
    checksum = static_cast<uint8_t>(checksum + readMemory8(VideoBiosRomBase + static_cast<uint32_t>(i)));
  writeMemory8(VideoBiosRomBase + VideoBiosRomSize - 1, static_cast<uint8_t>(0 - checksum));

  static constexpr uint16_t VideoTableSegment = 0xf000;
  static constexpr uint16_t VideoSavePointerOffset = 0xf1a0;
  static constexpr uint16_t VideoSecondarySavePointerOffset = 0xf1c0;
  static constexpr uint16_t VideoDccTableOffset = 0xf1e0;
  static constexpr uint16_t VideoParameterTableOffset = 0xf220;
  static constexpr uint8_t ColorVgaDccIndex = 0x0b; // DCC entry 11: no inactive display + color VGA

  auto linear = [](uint16_t segment, uint16_t offset) {
    return (static_cast<uint32_t>(segment) << 4) + offset;
  };
  auto writeFarPointer = [this](uint32_t address, uint16_t segment, uint16_t offset) {
    writeMemory16(address + 0, offset);
    writeMemory16(address + 2, segment);
  };

  writeMemory8(0x048a, ColorVgaDccIndex); // index into the Display Combination Code table
  writeFarPointer(0x04a8, VideoTableSegment, VideoSavePointerOffset);

  uint32_t const savePointer = linear(VideoTableSegment, VideoSavePointerOffset);
  writeFarPointer(savePointer + 0x00, VideoTableSegment, VideoParameterTableOffset);
  writeFarPointer(savePointer + 0x04, 0x0000, 0x0000);
  writeFarPointer(savePointer + 0x08, 0x0000, 0x0000);
  writeFarPointer(savePointer + 0x0c, 0x0000, 0x0000);
  writeFarPointer(savePointer + 0x10, VideoTableSegment, VideoSecondarySavePointerOffset);
  writeFarPointer(savePointer + 0x14, 0x0000, 0x0000);
  writeFarPointer(savePointer + 0x18, 0x0000, 0x0000);

  uint32_t const secondarySavePointer = linear(VideoTableSegment, VideoSecondarySavePointerOffset);
  writeMemory16(secondarySavePointer + 0x00, 0x001a); // table length including this word
  writeFarPointer(secondarySavePointer + 0x02, VideoTableSegment, VideoDccTableOffset);
  writeFarPointer(secondarySavePointer + 0x06, 0x0000, 0x0000);
  writeFarPointer(secondarySavePointer + 0x0a, 0x0000, 0x0000);
  writeFarPointer(secondarySavePointer + 0x0e, 0x0000, 0x0000);
  writeFarPointer(secondarySavePointer + 0x12, 0x0000, 0x0000);
  writeFarPointer(secondarySavePointer + 0x16, 0x0000, 0x0000);

  static constexpr uint8_t DccTable[] = {
    16, 1, 8, 0, // entries, version, maximum display type, reserved
    0x00, 0x00, //  0: no displays
    0x00, 0x01, //  1: MDPA
    0x00, 0x02, //  2: CGA
    0x02, 0x01, //  3: MDPA + CGA
    0x00, 0x04, //  4: EGA color
    0x04, 0x01, //  5: EGA color + MDPA
    0x00, 0x05, //  6: EGA mono
    0x02, 0x05, //  7: EGA mono + CGA
    0x00, 0x06, //  8: PGC
    0x01, 0x06, //  9: PGC + MDPA
    0x05, 0x06, // 10: PGC + EGA mono
    0x00, 0x08, // 11: VGA with analog color display
    0x01, 0x08, // 12: VGA color + MDPA
    0x00, 0x07, // 13: VGA with analog monochrome display
    0x02, 0x07, // 14: VGA mono + CGA
    0x02, 0x06, // 15: VGA mono + PGC
  };
  uint32_t const dccTable = linear(VideoTableSegment, VideoDccTableOffset);
  for (size_t i = 0; i < sizeof(DccTable); ++i)
    writeMemory8(dccTable + static_cast<uint32_t>(i), DccTable[i]);

  uint32_t const parameterTable = linear(VideoTableSegment, VideoParameterTableOffset);
  for (int entry = 0; entry < 29; ++entry) {
    uint32_t const tableEntry = parameterTable + static_cast<uint32_t>(entry) * 64;
    for (int i = 0; i < 64; ++i)
      writeMemory8(tableEntry + static_cast<uint32_t>(i), 0x00);
  }
  auto writeVideoParameterEntry = [this](uint8_t entry,
                                                         uint8_t columns,
                                                         uint8_t rowsMinusOne,
                                                         uint8_t cellHeight,
                                                         uint16_t pageSize) {
    uint32_t const tableEntry = parameterTable + static_cast<uint32_t>(entry) * 64;
    writeMemory8(tableEntry + 0x00, columns);
    writeMemory8(tableEntry + 0x01, rowsMinusOne);
    writeMemory8(tableEntry + 0x02, cellHeight);
    writeMemory16(tableEntry + 0x03, pageSize);
  };
  writeVideoParameterEntry(0x00, 40, 24, 8, 0x0800);
  writeVideoParameterEntry(0x01, 40, 24, 8, 0x0800);
  writeVideoParameterEntry(0x02, 80, 24, 8, 0x1000);
  writeVideoParameterEntry(0x03, 80, 24, 8, 0x1000);
  for (uint8_t entry = 0x04; entry <= 0x0e; ++entry)
    writeVideoParameterEntry(entry, entry == 0x06 || entry == 0x0e ? 80 : 40, 24, 8, entry == 0x0d ? 0x2000 : 0x4000);
  writeVideoParameterEntry(0x0f, 80, 24, 14, 0x8000);
  writeVideoParameterEntry(0x10, 80, 24, 14, 0x8000);
  writeVideoParameterEntry(0x11, 80, 24, 14, 0x8000);
  writeVideoParameterEntry(0x12, 80, 24, 14, 0x8000);
  writeVideoParameterEntry(0x13, 40, 24, 14, 0x0800);
  writeVideoParameterEntry(0x14, 40, 24, 14, 0x0800);
  writeVideoParameterEntry(0x15, 80, 24, 14, 0x1000);
  writeVideoParameterEntry(0x16, 80, 24, 14, 0x1000);
  writeVideoParameterEntry(0x17, 40, 24, 16, 0x0800);
  writeVideoParameterEntry(0x18, 80, 24, 16, 0x1000);
  writeVideoParameterEntry(0x19, 80, 24, 16, 0x1000);
  writeVideoParameterEntry(0x1a, 80, 29, 16, 0x0000);
  writeVideoParameterEntry(0x1b, 80, 29, 16, 0x0000);
  writeVideoParameterEntry(0x1c, 40, 24, 8, 0xfa00);

  setupEmsDriver();
}

void PcMachine::emsReset()
{
  for (int i = 0; i < EmsTotalPages; ++i)
    m_emsPageOwner[i] = 0;
  for (int h = 0; h <= EmsMaxHandles; ++h) {
    m_emsHandleActive[h] = false;
    m_emsHandlePageCount[h] = 0;
    m_emsSaved[h] = false;
    for (int p = 0; p < EmsPhysicalPages; ++p) {
      m_emsSavedHandle[h][p] = 0;
      m_emsSavedLogical[h][p] = -1;
    }
  }
  for (int p = 0; p < EmsPhysicalPages; ++p) {
    m_emsPhysMapHandle[p] = 0;
    m_emsPhysMapLogical[p] = -1;
    m_emsPhysMapPoolPage[p] = -1;
  }
  updateEmsWindowActive();
}

void PcMachine::updateEmsWindowActive()
{
  // The CPU only pays the page-frame redirect cost while at least one physical
  // page is mapped; otherwise E000:0 falls through to flat RAM.
  bool active = false;
  for (int p = 0; p < EmsPhysicalPages; ++p) {
    if (m_emsPhysMapPoolPage[p] >= 0) {
      active = true;
      break;
    }
  }
  PcI8086::setEmsWindowActive(active);
}

uint8_t PcMachine::emsReadCallback(void * context, int address)
{
  auto * machine = static_cast<PcMachine *>(context);
  uint32_t const offset = static_cast<uint32_t>(address) - (static_cast<uint32_t>(EmsPageFrameSegment) << 4);
  int const slot = static_cast<int>(offset / EmsLogicalPageSize);
  if (slot < 0 || slot >= EmsPhysicalPages)
    return 0xff;
  int const poolPage = machine->m_emsPhysMapPoolPage[slot];
  if (poolPage < 0 || !machine->m_emsPool)
    return 0xff; // unmapped physical frame reads as floating high
  uint32_t const inPage = offset % EmsLogicalPageSize;
  return machine->m_emsPool[static_cast<size_t>(poolPage) * EmsLogicalPageSize + inPage];
}

void PcMachine::emsWriteCallback(void * context, int address, uint8_t value)
{
  auto * machine = static_cast<PcMachine *>(context);
  uint32_t const offset = static_cast<uint32_t>(address) - (static_cast<uint32_t>(EmsPageFrameSegment) << 4);
  int const slot = static_cast<int>(offset / EmsLogicalPageSize);
  if (slot < 0 || slot >= EmsPhysicalPages)
    return;
  int const poolPage = machine->m_emsPhysMapPoolPage[slot];
  if (poolPage < 0 || !machine->m_emsPool)
    return; // writes to an unmapped physical frame are discarded
  uint32_t const inPage = offset % EmsLogicalPageSize;
  machine->m_emsPool[static_cast<size_t>(poolPage) * EmsLogicalPageSize + inPage] = value;
}

void PcMachine::setupEmsDriver()
{
  if (!m_emsPool)
    return; // no expanded memory: leave INT 67h unhooked

  // Place a minimal EMM device driver header at F800:0000 so that guest EMS
  // detection (which reads the 8-byte name at [INT 67h vector segment]:000A)
  // recognizes "EMMXXXX0". INT 67h itself is serviced by the emulator.
  //
  // LIMITATION: this only supports the interrupt-vector-name detection method.
  // The LIM-recommended method for transient programs -- DOS-opening the
  // "EMMXXXX0" character device (INT 21h AH=3D) and issuing IOCTL -- goes
  // through the DOS kernel booted from the disk image, which this BIOS layer
  // cannot intercept, so that open returns file-not-found. Fully supporting it
  // would require a resident EMM stub loaded via the guest's CONFIG.SYS that
  // links into the DOS device chain. Programs using only the vector-name check
  // (and any that call INT 67h directly) work as-is.
  static constexpr uint16_t EmmSegment = 0xf800;
  uint32_t const base = static_cast<uint32_t>(EmmSegment) << 4;
  writeMemory16(base + 0x00, 0xffff); // next device offset
  writeMemory16(base + 0x02, 0xffff); // next device segment (none)
  writeMemory16(base + 0x04, 0x8000); // attributes: character device
  writeMemory16(base + 0x06, 0x0016); // strategy entry offset (IRET stub)
  writeMemory16(base + 0x08, 0x0016); // interrupt entry offset (IRET stub)
  static constexpr char EmmName[8] = {'E', 'M', 'M', 'X', 'X', 'X', 'X', '0'};
  for (int i = 0; i < 8; ++i)
    writeMemory8(base + 0x0a + i, static_cast<uint8_t>(EmmName[i]));
  writeMemory8(base + 0x16, 0xcf); // IRET stub

  writeMemory16(0x67 * 4 + 0, 0x0016);   // INT 67h vector offset
  writeMemory16(0x67 * 4 + 2, EmmSegment); // INT 67h vector segment
}

int PcMachine::emsFreePageCount() const
{
  int free = 0;
  for (int i = 0; i < EmsTotalPages; ++i)
    if (m_emsPageOwner[i] == 0)
      ++free;
  return free;
}

int PcMachine::emsActiveHandleCount() const
{
  int count = 0;
  for (int h = 1; h <= EmsMaxHandles; ++h)
    if (m_emsHandleActive[h])
      ++count;
  return count;
}

int PcMachine::emsAllocateHandle(int pages)
{
  if (pages > emsFreePageCount())
    return -2; // not enough free pages
  int handle = -1;
  for (int h = 1; h <= EmsMaxHandles; ++h) {
    if (!m_emsHandleActive[h]) {
      handle = h;
      break;
    }
  }
  if (handle < 0)
    return -1; // no free handles
  int allocated = 0;
  for (int i = 0; i < EmsTotalPages && allocated < pages; ++i) {
    if (m_emsPageOwner[i] == 0) {
      m_emsPageOwner[i] = handle;
      ++allocated;
    }
  }
  m_emsHandleActive[handle] = true;
  m_emsHandlePageCount[handle] = pages;
  return handle;
}

bool PcMachine::emsFreeHandle(int handle)
{
  if (handle <= 0 || handle > EmsMaxHandles || !m_emsHandleActive[handle])
    return false;
  for (int i = 0; i < EmsTotalPages; ++i)
    if (m_emsPageOwner[i] == handle)
      m_emsPageOwner[i] = 0;
  for (int p = 0; p < EmsPhysicalPages; ++p) {
    if (m_emsPhysMapHandle[p] == handle) {
      m_emsPhysMapHandle[p] = 0;
      m_emsPhysMapLogical[p] = -1;
      m_emsPhysMapPoolPage[p] = -1;
    }
  }
  m_emsHandleActive[handle] = false;
  m_emsHandlePageCount[handle] = 0;
  m_emsSaved[handle] = false;
  updateEmsWindowActive();
  return true;
}

int PcMachine::emsLogicalToPool(int handle, int logicalPage) const
{
  if (handle <= 0 || handle > EmsMaxHandles || !m_emsHandleActive[handle])
    return -1;
  if (logicalPage < 0 || logicalPage >= m_emsHandlePageCount[handle])
    return -1;
  int seen = 0;
  for (int i = 0; i < EmsTotalPages; ++i) {
    if (m_emsPageOwner[i] == handle) {
      if (seen == logicalPage)
        return i;
      ++seen;
    }
  }
  return -1;
}

void PcMachine::emsMapPage(int physPage, int handle, int logicalPage)
{
  if (!m_emsPool || physPage < 0 || physPage >= EmsPhysicalPages)
    return;

  // Pure pointer remap: record which pool page this physical frame resolves to
  // and let the CPU page-frame redirect (emsRead/WriteCallback) hit the pool
  // directly. No data is copied, so mapping the same logical page into two
  // physical frames aliases one backing store, matching real EMS semantics.
  if (logicalPage < 0) {
    m_emsPhysMapHandle[physPage] = 0;
    m_emsPhysMapLogical[physPage] = -1;
    m_emsPhysMapPoolPage[physPage] = -1;
  } else {
    m_emsPhysMapHandle[physPage] = handle;
    m_emsPhysMapLogical[physPage] = logicalPage;
    m_emsPhysMapPoolPage[physPage] = emsLogicalToPool(handle, logicalPage);
  }
  updateEmsWindowActive();
}

bool PcMachine::handleEmsInterrupt()
{
  if (!m_emsPool)
    return false;

  uint8_t const function = PcI8086::AH();
  switch (function) {
    case 0x40: // get manager status
      PcI8086::setAH(0x00);
      return true;
    case 0x41: // get page frame segment
      PcI8086::setBX(EmsPageFrameSegment);
      PcI8086::setAH(0x00);
      return true;
    case 0x42: // get number of pages
      PcI8086::setBX(static_cast<uint16_t>(emsFreePageCount()));
      PcI8086::setDX(static_cast<uint16_t>(EmsTotalPages));
      PcI8086::setAH(0x00);
      return true;
    case 0x43: // allocate handle and pages
    {
      int const pages = PcI8086::BX();
      int const handle = emsAllocateHandle(pages);
      if (handle == -1) {
        PcI8086::setAH(0x85); // all handles in use
      } else if (handle == -2) {
        PcI8086::setAH(0x88); // not enough free pages
      } else {
        PcI8086::setDX(static_cast<uint16_t>(handle));
        PcI8086::setAH(0x00);
      }
      return true;
    }
    case 0x44: // map handle page
    {
      int const physPage = PcI8086::AL();
      int const logicalPage = PcI8086::BX();
      int const handle = PcI8086::DX();
      if (physPage < 0 || physPage >= EmsPhysicalPages) {
        PcI8086::setAH(0x8b); // illegal physical page
      } else if (handle <= 0 || handle > EmsMaxHandles || !m_emsHandleActive[handle]) {
        PcI8086::setAH(0x83); // invalid handle
      } else if (logicalPage == 0xffff) {
        emsMapPage(physPage, handle, -1); // unmap
        PcI8086::setAH(0x00);
      } else if (logicalPage >= m_emsHandlePageCount[handle]) {
        PcI8086::setAH(0x8a); // logical page out of range
      } else {
        emsMapPage(physPage, handle, logicalPage);
        PcI8086::setAH(0x00);
      }
      return true;
    }
    case 0x45: // release handle and pages
      PcI8086::setAH(emsFreeHandle(PcI8086::DX()) ? 0x00 : 0x83);
      return true;
    case 0x46: // get EMM version
      PcI8086::setAL(0x32); // version 3.2
      PcI8086::setAH(0x00);
      return true;
    case 0x47: // save page map
    {
      int const handle = PcI8086::DX();
      if (handle <= 0 || handle > EmsMaxHandles || !m_emsHandleActive[handle]) {
        PcI8086::setAH(0x83);
      } else {
        for (int p = 0; p < EmsPhysicalPages; ++p) {
          m_emsSavedHandle[handle][p] = m_emsPhysMapHandle[p];
          m_emsSavedLogical[handle][p] = m_emsPhysMapLogical[p];
        }
        m_emsSaved[handle] = true;
        PcI8086::setAH(0x00);
      }
      return true;
    }
    case 0x48: // restore page map
    {
      int const handle = PcI8086::DX();
      if (handle <= 0 || handle > EmsMaxHandles || !m_emsHandleActive[handle]) {
        PcI8086::setAH(0x83);
      } else if (!m_emsSaved[handle]) {
        PcI8086::setAH(0x8c); // no context saved
      } else {
        for (int p = 0; p < EmsPhysicalPages; ++p)
          emsMapPage(p, m_emsSavedHandle[handle][p], m_emsSavedLogical[handle][p]);
        PcI8086::setAH(0x00);
      }
      return true;
    }
    case 0x4b: // get number of open handles
      PcI8086::setBX(static_cast<uint16_t>(emsActiveHandleCount()));
      PcI8086::setAH(0x00);
      return true;
    case 0x4c: // get pages owned by handle
    {
      int const handle = PcI8086::DX();
      if (handle <= 0 || handle > EmsMaxHandles || !m_emsHandleActive[handle]) {
        PcI8086::setAH(0x83);
      } else {
        PcI8086::setBX(static_cast<uint16_t>(m_emsHandlePageCount[handle]));
        PcI8086::setAH(0x00);
      }
      return true;
    }
    case 0x4d: // get pages for all handles
    {
      uint32_t dest = (static_cast<uint32_t>(PcI8086::ES()) << 4) + PcI8086::DI();
      int count = 0;
      for (int h = 1; h <= EmsMaxHandles; ++h) {
        if (m_emsHandleActive[h]) {
          writeMemory16(dest, static_cast<uint16_t>(h));
          writeMemory16(dest + 2, static_cast<uint16_t>(m_emsHandlePageCount[h]));
          dest += 4;
          ++count;
        }
      }
      PcI8086::setBX(static_cast<uint16_t>(count));
      PcI8086::setAH(0x00);
      return true;
    }
    default:
      PcI8086::setAH(0x84); // unsupported function
      return true;
  }
}

} // namespace tabdos
