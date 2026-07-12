#pragma once

#include "pc_disk_image.h"
#include "pc_bios.h"
#include "pc_i8086.h"
#include "pc_keyboard_controller.h"
#include "pc_mouse_cursor_overlay.h"
#include "pc_opl2.h"
#include "pc_text_renderer.h"

#include <stddef.h>
#include <stdint.h>

namespace tabdos {

class PcMachine {
public:
  struct BootState {
    bool loaded;
    int diskIndex;
    uint8_t biosDrive;
    uint16_t segment;
    uint16_t offset;
    uint32_t linearAddress;
  };

  struct CpuState {
    uint16_t ax;
    uint16_t bx;
    uint16_t cx;
    uint16_t dx;
    uint16_t cs;
    uint16_t ds;
    uint16_t es;
    uint16_t ss;
    uint16_t ip;
    uint16_t sp;
    bool halted;
  };

  struct Diagnostics {
    uint64_t unsupportedInterruptCount;
    uint64_t unsupportedPortReadCount;
    uint64_t unsupportedPortWriteCount;
    uint8_t lastUnsupportedInterrupt;
    uint8_t lastUnsupportedInterruptAh;
    uint16_t lastUnsupportedInterruptCs;
    uint16_t lastUnsupportedInterruptIp;
    uint16_t lastUnsupportedPortRead;
    uint16_t lastUnsupportedPortWrite;
    uint64_t diskWriteCount;
    uint64_t lastDiskWriteLba;
    uint8_t lastDiskWriteCount;
    uint8_t lastDiskWriteDrive;
    uint64_t unsupportedOpcodeCount;
    uint16_t lastUnsupportedOpcodeCs;
    uint16_t lastUnsupportedOpcodeIp;
    uint8_t lastUnsupportedOpcode0;
    uint8_t lastUnsupportedOpcode1;
  };

  struct VgaDacState {
    uint8_t readIndex;
    uint8_t writeIndex;
    uint8_t readComponent;
    uint8_t writeComponent;
    uint8_t pelMask;
    uint8_t state;
    uint8_t pelMaskReadCount;
    uint8_t command;
  };

  struct VgaAttributeState {
    uint8_t index;
    bool flipFlop;
    bool videoEnabled;
  };

  struct VgaLatchState {
    uint8_t plane[4];
  };

  enum class VideoMode {
    Text80,
    CgaGraphics320x200x4,
    CgaGraphics640x200x2,
    HerculesGraphics,
    VgaGraphics320x200x16,
    VgaGraphics640x200x16,
    VgaGraphics640x350x2,
    VgaGraphics640x350x16,
    VgaGraphics640x480x2,
    VgaGraphics640x480x16,
    VgaGraphics320x200x256,
  };

  static constexpr size_t RamSize = 1024 * 1024;
  static constexpr size_t VideoMemorySize = 128 * 1024;
  static constexpr uint32_t VideoMemoryBase = 0x000a0000;
  static constexpr uint32_t VgaGraphicsMemoryBase = 0x000a0000;
  static constexpr uint32_t TextColorMemoryBase = 0x000b8000;
  static constexpr uint32_t HerculesGraphicsMemoryBase = 0x000b0000;
  static constexpr int CgaGraphics320Width = 320;
  static constexpr int CgaGraphicsWidth = 640;
  static constexpr int CgaGraphicsHeight = 200;
  static constexpr int VgaGraphics256Width = 320;
  static constexpr int VgaGraphics256Height = 200;
  static constexpr int VgaGraphics16LowWidth = 320;
  static constexpr int VgaGraphics16MediumWidth = 640;
  static constexpr int VgaGraphics16LowHeight = 200;
  static constexpr int VgaGraphics16MediumHeight = 350;
  static constexpr int VgaGraphics16Width = 640;
  static constexpr int VgaGraphics16Height = 480;
  static constexpr int HerculesGraphicsWidth = 720;
  static constexpr int HerculesGraphicsHeight = 348;
  static constexpr int HerculesGraphicsBytesPerLine = HerculesGraphicsWidth / 8;
  static constexpr uint32_t DefaultBootLinearAddress = 0x00007c00;
  static constexpr int DiskCount = 4;

  // LIM EMS 4.0 expanded memory subset: a 64 KiB page frame of four 16 KiB
  // physical pages, backed by a PSRAM pool that DOS programs map via INT 67h.
  static constexpr uint16_t EmsPageFrameSegment = 0xe000;
  static constexpr uint8_t EmsVersion = 0x40;
  static constexpr int EmsPhysicalPages = 4;
  static constexpr int EmsLogicalPageSize = 16 * 1024;
  static constexpr int EmsTotalPages = 256; // 4 MiB pool
  static constexpr int EmsMaxHandles = 64;

  PcMachine();
  ~PcMachine();

  PcMachine(PcMachine const &) = delete;
  PcMachine & operator=(PcMachine const &) = delete;

  bool init();
  void reset();

  uint8_t * ram() { return m_ram; }
  uint8_t const * ram() const { return m_ram; }
  uint8_t * videoMemory() { return m_videoMemory; }
  uint8_t const * videoMemory() const { return m_videoMemory; }
  uint8_t const * vgaPlaneMemory() const { return m_vgaPlaneMemory; }

  PcKeyboardController & keyboard() { return m_keyboard; }
  PcTextRenderer & textRenderer() { return m_textRenderer; }
  PcTextRenderer const & textRenderer() const { return m_textRenderer; }

  bool openDisk(int index, char const * path, PcDiskImage::Geometry geometry = {});
  PcDiskImage * disk(int index);
  PcDiskImage const * disk(int index) const;
  bool flushDisks();
  bool loadBootSector(int diskIndex,
                      uint8_t biosDrive,
                      uint16_t segment = 0x0000,
                      uint16_t offset = 0x7c00);
  BootState bootState() const { return m_bootState; }
  bool prepareBootCpu();
  void stepCpu();
  int runCpuSteps(int maxSteps);
  bool cpuHalted() const;
  CpuState cpuState() const;
  Diagnostics diagnostics() const { return m_diagnostics; }
  void completeKeyboardIrqService();
  VgaDacState vgaDacState() const;
  void setVgaDacState(VgaDacState const & state);
  VgaAttributeState vgaAttributeState() const;
  void setVgaAttributeState(VgaAttributeState const & state);
  VgaLatchState vgaLatchState() const;
  void setVgaLatchState(VgaLatchState const & state);
  void setCgaCompatibilityState(uint8_t modeControl, uint8_t colorSelect);
  VideoMode videoMode() const { return m_videoMode; }
  bool isHerculesVideoEnabled() const { return m_herculesVideoEnabled; }
  bool isGraphicsMode() const;
  int graphicsWidth() const;
  int graphicsHeight() const;
  void setVideoMode(VideoMode mode, bool resetState = true, bool clearMemory = true);
  void resetDiagnostics();
  void recordDiskWrite(uint8_t biosDrive, uint64_t lba, uint8_t count);
  void setMouseInstalled(bool installed);
  void setMouseState(uint16_t x, uint16_t y, uint16_t buttons);
  void setMouseSourceState(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t buttons);

  // PC speaker state derived from the 8254 channel 2 and PPI port 61h, consumed
  // by the host audio task. speakerFrequency() returns 0 when the tone is off.
  bool speakerEnabled() const;
  uint32_t speakerFrequency() const;

  // AdLib/OPL2 (ports 388h/389h). The audio thread copies the register file
  // under the machine mutex (snapshotOpl2), then synthesizes without the mutex
  // held (synthesizeOpl2) so FM rendering never stalls the emulated CPU.
  // synthesizeOpl2 returns false when no voice is sounding.
  void snapshotOpl2(Opl2::RegisterSnapshot * out) const { m_opl2.snapshotRegisters(out); }
  bool synthesizeOpl2(Opl2::RegisterSnapshot const & snapshot, int16_t * out, int frames, uint32_t sampleRate)
  {
    return m_opl2.render(snapshot, out, frames, sampleRate);
  }

  bool emsAvailable() const { return m_emsPool != nullptr; }
  bool handleEmsInterrupt();

  uint8_t readPort(uint16_t port);
  void writePort(uint16_t port, uint8_t value);

  uint8_t readMemory8(uint32_t address) const;
  uint16_t readMemory16(uint32_t address) const;
  void writeMemory8(uint32_t address, uint8_t value);
  void writeMemory16(uint32_t address, uint16_t value);
  bool isRamRangeValid(uint32_t address, size_t size) const;
  bool readMemoryBlock(uint32_t address, void * dest, size_t size) const;
  bool writeMemoryBlock(uint32_t address, void const * src, size_t size);

  uint8_t readVideoMemory8(uint32_t address) const;
  uint16_t readVideoMemory16(uint32_t address) const;
  void writeVideoMemory8(uint32_t address, uint8_t value);
  void writeVideoMemory16(uint32_t address, uint16_t value);

  MouseCursorOverlay computeMouseOverlay(int sourceWidth, int sourceHeight, bool textMode, int cellWidth, int cellHeight) const;

  uint8_t const * text80Buffer() const;
  void renderText80Frame(uint16_t * dest, int destPitchPixels) const;
  void renderText80Line(int y, uint16_t * dest) const;
  void renderGraphicsFrame(uint16_t * dest, int destPitchPixels) const;
  void renderGraphicsLine(int y, uint16_t * dest) const;
  void renderGraphicsFrameForDisplay(uint16_t * dest, int destPitchPixels) const;
  void renderGraphicsLineForDisplay(int y, uint16_t * dest) const;
  bool writeGraphicsPixel(int x, int y, uint8_t color, bool xorMode, uint16_t pageOffset = 0);
  bool readGraphicsPixel(int x, int y, uint8_t * color, uint16_t pageOffset = 0) const;
  void renderHerculesGraphicsFrame(uint16_t * dest, int destPitchPixels) const;
  void renderHerculesGraphicsLine(int y, uint16_t * dest) const;

private:
  void emsReset();
  void setupEmsDriver();
  int emsAllocateHandle(int pages);
  bool emsFreeHandle(int handle);
  int emsLogicalToPool(int handle, int logicalPage) const;
  void emsMapPage(int physPage, int handle, int logicalPage);
  int emsFreePageCount() const;
  int emsActiveHandleCount() const;
  bool emsWindowActive() const;
  void updateEmsWindowActive();
  static uint8_t emsReadCallback(void * context, int address);
  static void emsWriteCallback(void * context, int address, uint8_t value);

  void pitReset();
  void pitWriteCommand(uint8_t value);
  void pitWriteData(int channel, uint8_t value);
  uint8_t pitReadData(int channel);
  void pitLatch(int channel);
  uint16_t pitCurrentCount(int channel) const;
  uint32_t pitDivisor(int channel) const;
  void onPitReload(int channel);
  void serviceTimerInterrupt();

  static bool isRangeValid(uint32_t address, size_t size, size_t limit);
  static uint8_t readPortCallback(void * context, int address);
  static void writePortCallback(void * context, int address, uint8_t value);
  static uint8_t readVideoMemory8Callback(void * context, int address);
  static uint16_t readVideoMemory16Callback(void * context, int address);
  static void writeVideoMemory8Callback(void * context, int address, uint8_t value);
  static void writeVideoMemory16Callback(void * context, int address, uint16_t value);
  static bool interruptCallback(void * context, int interruptNumber);
  static void unsupportedOpcodeCallback(void * context, uint16_t cs, uint16_t ip, uint8_t op0, uint8_t op1);
  void recordUnsupportedInterrupt(int interruptNumber);
  void recordUnsupportedPortRead(uint16_t port);
  void recordUnsupportedPortWrite(uint16_t port);
  void initializeBiosDataArea();
  void setEquipmentVideoBits(uint16_t videoBits);
  void resetVideoState();
  void resetDacPalette();
  void updateTextRendererPalette();
  void loadDefaultTextFontPlane();
  void updateTextRendererFontMap();
  void updateTextRendererUnderline();
  void updateTextRendererCursorShape();
  void updateTextRendererCellHeightFromCrtc();
  void updateTextRendererModeControl();
  void updateTextRendererHorizontalPanning();
  void updateTextRendererDisplayEnabled();
  void updateTextRendererCursorPosition();
  bool isVgaChain4Enabled() const;
  bool isVgaOddEvenReadEnabled() const;
  bool isVgaOddEvenWriteEnabled() const;
  bool isVgaMemoryAccessEnabled() const;
  bool isVgaDisplayEnabled() const;
  bool isVgaMonoCrtcSelected() const;
  uint8_t vgaInputStatus0() const;
  bool usesPlanarVgaMemory() const;
  bool vgaChain4OffsetForAddress(uint32_t address, uint32_t * offset) const;
  bool vgaPlanarOffsetForAddress(uint32_t address, uint32_t * offset) const;
  uint16_t vgaCrtcStartAddress() const;
  uint32_t vgaCrtcAddressUnitBytes() const;
  uint32_t vgaCrtcStartOffsetBytes() const;
  int vgaMode13Height() const;
  int vgaPlanarWidth() const;
  int vgaPlanarHeight() const;
  uint32_t vgaCrtcOffsetBytes(uint32_t fallback) const;
  uint8_t vgaBytePanningBytes() const;
  uint32_t vgaMode13Chain4LineOffsetBytes() const;
  uint8_t vgaPresetRowScan() const;
  uint8_t vgaScanLinesPerRow() const;
  int vgaSourceRowForDisplayLine(int y, bool splitLine) const;
  uint16_t vgaVerticalDisplayEnd() const;
  uint16_t vgaLineCompare() const;
  int vgaHorizontalPanningPixels(bool splitLine = false) const;
  uint8_t vgaAttributePaletteLowNibble(uint8_t attributeIndex) const;
  uint8_t vgaAttributeColorOutput(uint8_t attributeIndex) const;
  uint8_t vgaPlanarDisplayIndex(uint32_t rowOffset, int sourceX) const;
  void vgaWritePlanarDisplayIndex(uint32_t rowOffset, int sourceX, uint8_t color);
  uint8_t vgaAttributeDacIndex(uint8_t attributeIndex) const;
  uint8_t vgaMode13ColorOutput(uint8_t pixel) const;
  uint8_t vgaMode13DacIndex(uint8_t pixel) const;
  uint8_t vgaStatusMuxBits() const;
  uint8_t vgaStatusMuxColor() const;
  uint8_t vgaOverscanColorOutput() const;
  uint8_t vgaOverscanDacIndex() const;
  uint16_t vgaOverscanRgb565() const;
  uint16_t dacRgb565(uint8_t index) const;
  uint16_t cgaRgb565(uint8_t index) const;
  void syncCgaModeBiosDataArea(VideoMode mode);
  void renderCga320Line(int y, uint16_t * dest, bool forceDisplayEnable = false) const;
  void renderCga640Line(int y, uint16_t * dest, bool forceDisplayEnable = false) const;
  void renderGraphicsLineInternal(int y, uint16_t * dest, bool forceCgaDisplayEnable) const;
  void renderVgaMode13Line(int y, uint16_t * dest) const;
  void renderVgaPlanar16Line(int y, uint16_t * dest) const;
  uint8_t vgaReadChain4Byte(uint32_t offset) const;
  void vgaWriteChain4Byte(uint32_t offset, uint8_t value);
  void vgaWritePlaneByte(uint32_t offset, uint8_t value, uint8_t planeMask = 0x0f);

  uint8_t * m_ram;
  uint8_t * m_videoMemory;
  uint8_t * m_vgaPlaneMemory;
  uint8_t * m_emsPool;
  int m_emsPageOwner[EmsTotalPages];
  bool m_emsHandleActive[EmsMaxHandles + 1];
  int m_emsHandlePageCount[EmsMaxHandles + 1];
  int m_emsPhysMapHandle[EmsPhysicalPages];
  int m_emsPhysMapLogical[EmsPhysicalPages];
  int m_emsPhysMapPoolPage[EmsPhysicalPages]; // cached O(1) pool page per slot, -1 unmapped
  bool m_emsSaved[EmsMaxHandles + 1];
  int m_emsSavedHandle[EmsMaxHandles + 1][EmsPhysicalPages];
  int m_emsSavedLogical[EmsMaxHandles + 1][EmsPhysicalPages];
  PcKeyboardController m_keyboard;
  PcBios m_bios;
  PcTextRenderer m_textRenderer;
  Opl2 m_opl2;
  PcDiskImage m_disks[DiskCount];
  BootState m_bootState;
  Diagnostics m_diagnostics;
  VideoMode m_videoMode;
  bool m_keyboardIrqInService;
  uint8_t m_picMask;
  uint8_t m_picSlaveMask;
  uint8_t m_port61;
  uint8_t m_floppyDigitalOutputRegister;
  uint8_t m_cmosIndex;
  // 8253/8254 Programmable Interval Timer. Channel 0 drives IRQ0 at its
  // programmed frequency, channel 2 gates the PC speaker. Counts are derived
  // from wall-clock time so guest delay loops and reprogrammed timer music work.
  struct PitChannel {
    uint16_t reloadValue;   // 0 represents the full 65536 divisor
    uint16_t latchValue;    // value captured by a latch command
    bool latched;           // a latched value is pending readback
    uint8_t accessMode;     // 1=lobyte, 2=hibyte, 3=lobyte/hibyte
    uint8_t operatingMode;  // 0..5 (modes 6/7 alias 2/3)
    bool bcd;
    bool writeHigh;         // next write is the high byte (access mode 3)
    bool readHigh;          // next read is the high byte (access mode 3)
    bool gate;              // gate input (channel 2 tied to port 61h bit 0)
    uint64_t phaseBaseMicros;
  };
  PitChannel m_pit[3];
  uint64_t m_pitChannel0NextIrqMicros;
  int m_timerUpdateCountdown;
  uint8_t m_colorCrtcIndex;
  uint8_t m_colorCrtcRegisters[32];
  uint8_t m_cgaModeControl;
  uint8_t m_cgaColorSelect;
  uint8_t m_miscOutputRegister;
  uint8_t m_featureControlRegister;
  uint8_t m_vgaEnableRegister;
  uint8_t m_attributeIndex;
  uint8_t m_attributeRegisters[0x20];
  bool m_attributeFlipFlop;
  bool m_attributeVideoEnabled;
  uint8_t m_sequencerIndex;
  uint8_t m_sequencerRegisters[8];
  uint8_t m_graphicsIndex;
  uint8_t m_graphicsRegisters[16];
  uint8_t m_vgaLatches[4];
  uint8_t m_dacReadIndex;
  uint8_t m_dacWriteIndex;
  uint8_t m_dacReadComponent;
  uint8_t m_dacWriteComponent;
  uint8_t m_dacPelMask;
  uint8_t m_dacPelMaskReadCount;
  uint8_t m_dacCommandRegister;
  uint8_t m_dacState;
  uint8_t m_dacPalette[256][3];
  uint8_t m_herculesCrtcIndex;
  uint8_t m_herculesCrtcRegisters[32];
  uint8_t m_herculesConfigRegister;
  uint8_t m_herculesModeControl;
  uint8_t m_herculesStatus;
  bool m_herculesVideoEnabled;
};

} // namespace tabdos
