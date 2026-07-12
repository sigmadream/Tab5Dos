#include "pc/pc_machine.h"
#include "pc/pc_i8086.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <vector>

using tabdos::PcDiskImage;
using tabdos::PcI8086;
using tabdos::PcKeyboardController;
using tabdos::PcMachine;
using tabdos::PcTextRenderer;

namespace tabdos {
extern const uint8_t PcFont8x16Data[4096];
}

static void createImage(char const * path, size_t bytes)
{
  FILE * file = fopen(path, "wb");
  assert(file);
  std::vector<uint8_t> zero(PcDiskImage::SectorSize, 0);
  for (size_t written = 0; written < bytes; written += zero.size())
    assert(fwrite(zero.data(), 1, zero.size(), file) == zero.size());
  assert(fclose(file) == 0);
}

static uint16_t pcMachineRgb565From6Bit(uint8_t r, uint8_t g, uint8_t b)
{
  uint8_t const r8 = static_cast<uint8_t>((static_cast<unsigned>(r & 0x3f) * 255 + 31) / 63);
  uint8_t const g8 = static_cast<uint8_t>((static_cast<unsigned>(g & 0x3f) * 255 + 31) / 63);
  uint8_t const b8 = static_cast<uint8_t>((static_cast<unsigned>(b & 0x3f) * 255 + 31) / 63);
  return static_cast<uint16_t>(((r8 & 0xf8) << 8) | ((g8 & 0xfc) << 3) | (b8 >> 3));
}

static uint16_t pcMachineDefaultVgaDacRgb565(uint8_t index)
{
  uint8_t const color = index & 0x0f;
  uint8_t const high = (color & 0x08) ? 63 : 42;
  uint8_t const low = (color & 0x08) ? 21 : 0;
  uint8_t r = (color & 0x04) ? high : low;
  uint8_t g = (color & 0x02) ? high : low;
  uint8_t b = (color & 0x01) ? high : low;
  if (color == 0x06)
    g = 21; // IBM VGA brown instead of dark yellow.
  return pcMachineRgb565From6Bit(r, g, b);
}

static void writeDacEntry(PcMachine & machine, uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
  machine.writePort(0x03c8, index);
  machine.writePort(0x03c9, r);
  machine.writePort(0x03c9, g);
  machine.writePort(0x03c9, b);
}

int main()
{
  PcMachine machine;
  assert(machine.init());
  assert(!machine.bootState().loaded);

  machine.writeMemory16(0x7c00, 0xaa55);
  assert(machine.readMemory16(0x7c00) == 0xaa55);
  machine.writeMemory8(PcMachine::RamSize, 0x12);
  assert(machine.readMemory8(PcMachine::RamSize) == 0xff);

  machine.writePort(0x70, 0x0d);
  assert(machine.readPort(0x70) == 0x0d);
  assert(machine.readPort(0x71) == 0x80);
  machine.writePort(0x70, 0x15);
  assert(machine.readPort(0x71) == 0x80);
  machine.writePort(0x70, 0x16);
  assert(machine.readPort(0x71) == 0x02);
  machine.writePort(0x80, 0x55);
  assert(machine.readPort(0x80) == 0x00);
  machine.writePort(0x008a, 0x12);
  assert(machine.readPort(0x008a) == 0xff);
  assert(machine.readPort(0x0092) == 0x00);
  machine.writePort(0x0092, 0x00);
  assert(machine.readPort(0x0092) == 0x00);
  machine.writePort(0x03cd, 0x56);
  assert(machine.readPort(0x03cd) == 0xff);
  machine.writePort(0x0a20, 0x34);
  assert(machine.readPort(0x0a20) == 0xff);
  assert(machine.readPort(0x0a24) == 0xff);
  assert(machine.diagnostics().unsupportedPortWriteCount == 0);

  machine.reset();
  machine.writePort(0x03ce, 0x06);
  assert((machine.readPort(0x03cf) & 0x0c) == 0x04); // default VGA A000-AFFFF aperture
  machine.writePort(0x03c4, 0x04);
  machine.writePort(0x03c5, 0x06); // disable odd/even addressing for linear plane RAM probing
  static uint8_t const textModePlaneValues[4] = {0x11, 0x22, 0x33, 0x44};
  for (int plane = 0; plane < 4; ++plane) {
    machine.writePort(0x03c4, 0x02);
    machine.writePort(0x03c5, static_cast<uint8_t>(1 << plane));
    machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 0xffff, textModePlaneValues[plane]);
  }
  for (int plane = 0; plane < 4; ++plane) {
    machine.writePort(0x03ce, 0x04);
    machine.writePort(0x03cf, static_cast<uint8_t>(plane));
    assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 0xffff) == textModePlaneValues[plane]);
  }
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x0741);
  assert(machine.readVideoMemory16(PcMachine::TextColorMemoryBase) == 0x0741);

  machine.reset();
  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c4, 0x04);
  machine.writePort(0x03c5, 0x02); // Sequencer odd/even-disable clear, but GC odd/even still clear.
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x00);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x0f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0x5a);
  for (int plane = 0; plane < 4; ++plane) {
    machine.writePort(0x03ce, 0x04);
    machine.writePort(0x03cf, static_cast<uint8_t>(plane));
    assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1) == 0x5a);
  }

  machine.reset();
  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c4, 0x04);
  machine.writePort(0x03c5, 0x02); // Sequencer permits odd/even host addressing.
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x10); // GC odd/even enables address bit 0 -> odd/even planes.
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x0f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0xa5);
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x00); // Disable odd/even readback so plane offsets can be inspected directly.
  machine.writePort(0x03ce, 0x04);
  machine.writePort(0x03cf, 0x00);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0x00);
  machine.writePort(0x03cf, 0x01);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0xa5);
  machine.writePort(0x03cf, 0x02);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0x00);
  machine.writePort(0x03cf, 0x03);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0xa5);

  machine.reset();
  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c4, 0x04);
  machine.writePort(0x03c5, 0x06); // Sequencer odd/even-disable set: direct planar writes.
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x00);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0x66);
  machine.writePort(0x03c5, 0x02);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x77);
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x00); // direct readback while GC odd/even is clear.
  machine.writePort(0x03ce, 0x04);
  machine.writePort(0x03cf, 0x00);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1) == 0x66);
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x10); // CPU reads use GC odd/even even when Sequencer bit 2 is set.
  machine.writePort(0x03ce, 0x04);
  machine.writePort(0x03cf, 0x00);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1) == 0x77);

  machine.reset();
  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x16);
  assert(machine.writeGraphicsPixel(0, 0, 0x0f, false));
  uint8_t stalePlanarPixel = 0;
  assert(machine.readGraphicsPixel(0, 0, &stalePlanarPixel));
  assert(stalePlanarPixel == 0x0f);
  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x200x16);
  stalePlanarPixel = 0xff;
  assert(machine.readGraphicsPixel(0, 0, &stalePlanarPixel));
  assert(stalePlanarPixel == 0x00);

  machine.reset();
  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  assert(machine.writeGraphicsPixel(0, 0, 0x0f, false));
  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16, true, false);
  stalePlanarPixel = 0;
  assert(machine.readGraphicsPixel(0, 0, &stalePlanarPixel));
  assert(stalePlanarPixel == 0x0f);

  machine.reset();
  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  std::vector<uint16_t> planarPaletteLine(PcMachine::VgaGraphics16Width, 0);
  for (uint8_t color = 0; color < 16; ++color) {
    uint8_t const red = static_cast<uint8_t>(color * 4);
    uint8_t const green = static_cast<uint8_t>(63 - color * 4);
    uint8_t const blue = static_cast<uint8_t>((color * 7) & 0x3f);
    machine.writePort(0x03c8, color);
    machine.writePort(0x03c9, red);
    machine.writePort(0x03c9, green);
    machine.writePort(0x03c9, blue);
    assert(machine.writeGraphicsPixel(color, 0, color, false));
  }
  machine.renderGraphicsLine(0, planarPaletteLine.data());
  for (uint8_t color = 0; color < 16; ++color) {
    uint8_t const red = static_cast<uint8_t>(color * 4);
    uint8_t const green = static_cast<uint8_t>(63 - color * 4);
    uint8_t const blue = static_cast<uint8_t>((color * 7) & 0x3f);
    assert(planarPaletteLine[color] == pcMachineRgb565From6Bit(red, green, blue));
  }

  machine.reset();
  // prepareBootCpu() normally initializes these BIOS Data Area pointers before
  // keyboard input starts. This isolated port test does not boot a disk, so set
  // up the empty BIOS keyboard ring explicitly.
  machine.writeMemory16(0x041a, 0x001e);
  machine.writeMemory16(0x041c, 0x001e);
  uint8_t keys[6] = {0x07, 0, 0, 0, 0, 0}; // HID d
  machine.keyboard().onHidBootKeyboardReport(0, keys);
  assert(machine.readPort(0x64) & PcKeyboardController::StatusOutputBufferFull);
  assert(machine.readPort(0x60) == 0x20);
  assert(machine.readMemory16(0x041a) == 0x001e);
  assert(machine.readMemory16(0x041c) == 0x001e); // shadow key is deferred for custom INT 09h handlers
  machine.keyboard().acknowledgeIrq();
  keys[0] = 0;
  machine.keyboard().onHidBootKeyboardReport(0, keys);
  assert(machine.readPort(0x60) == 0xa0);
  assert(machine.readMemory16(0x041c) == 0x0020); // next scancode flushes the unclaimed shadow key
  assert(machine.readMemory16(0x041e) == 0x2064);

  machine.reset();
  uint8_t const customKeyboardIrq[] = {
    0xfb,                         // sti: raw handlers often re-enable interrupts early
    0x50,                         // push ax
    0x53,                         // push bx
    0xe4, 0x60,                   // in al, 60h
    0x8b, 0x1e, 0x02, 0x05,       // mov bx, [0502h]
    0x88, 0x87, 0x00, 0x05,       // mov [bx+0500h], al
    0xff, 0x06, 0x02, 0x05,       // inc word [0502h]
    0x5b,                         // pop bx
    0xb0, 0x20,                   // mov al, 20h
    0xe6, 0x20,                   // out 20h, al (PIC EOI)
    0x58,                         // pop ax
    0xcf                          // iret
  };
  uint8_t const idleProgram[] = {
    0xfb,                         // sti
    0xf4,                         // hlt
    0xeb, 0xfd                    // jmp back to hlt after an interrupt
  };
  char irqPath[] = "/tmp/tabdos-machine-irq-XXXXXX";
  int irqFd = mkstemp(irqPath);
  assert(irqFd >= 0);
  close(irqFd);
  createImage(irqPath, 1474560);
  assert(machine.openDisk(0, irqPath));
  uint8_t irqSector[PcDiskImage::SectorSize] = {};
  memcpy(irqSector, idleProgram, sizeof(idleProgram));
  irqSector[510] = 0x55;
  irqSector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, irqSector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  assert(machine.readMemory8(0x000c0000) == 0x55);
  assert(machine.readMemory8(0x000c0001) == 0xaa);
  assert(machine.readMemory8(0x000c0003) == 0xcb);
  uint32_t const videoBiosRomSize = static_cast<uint32_t>(machine.readMemory8(0x000c0002)) * 512u;
  assert(videoBiosRomSize == 2048);
  uint8_t videoBiosChecksum = 0;
  for (uint32_t i = 0; i < videoBiosRomSize; ++i)
    videoBiosChecksum = static_cast<uint8_t>(videoBiosChecksum + machine.readMemory8(0x000c0000 + i));
  assert(videoBiosChecksum == 0);
  assert(strstr(reinterpret_cast<char const *>(machine.ram() + 0x000c0030), "TabDOS VGA BIOS"));
  assert(strstr(reinterpret_cast<char const *>(machine.ram() + 0x000c0050), "Version 1.0"));
  assert(memcmp(machine.ram() + 0x000c0000 + videoBiosRomSize - 9, "06/19/26", 8) == 0);
  assert(machine.writeMemoryBlock(0x10000, customKeyboardIrq, sizeof(customKeyboardIrq)));
  machine.writeMemory16(0x0009 * 4 + 0, 0x0000);
  machine.writeMemory16(0x0009 * 4 + 2, 0x1000);
  uint8_t rawKeys[6] = {0x04, 0, 0, 0, 0, 0}; // HID a press
  machine.keyboard().onHidBootKeyboardReport(0, rawKeys);
  rawKeys[0] = 0;
  machine.keyboard().onHidBootKeyboardReport(0, rawKeys); // release queued behind make
  for (int i = 0; i < 128; ++i)
    machine.stepCpu();
  assert(machine.readMemory16(0x0502) >= 2);
  assert(machine.readMemory8(0x0500) == 0x1e);
  assert(machine.readMemory8(0x0501) == 0x9e);
  unlink(irqPath);

  machine.reset();
  uint8_t const chainingKeyboardIrqNoEoi[] = {
    0xfb,                         // sti: raw handlers often re-enable interrupts early
    0xcd, 0x79,                   // old BIOS INT 09h compatibility stub body
    0xcf                          // iret: no explicit PIC EOI in the custom handler
  };
  char chainedIrqPath[] = "/tmp/tabdos-machine-chainirq-XXXXXX";
  int chainedIrqFd = mkstemp(chainedIrqPath);
  assert(chainedIrqFd >= 0);
  close(chainedIrqFd);
  createImage(chainedIrqPath, 1474560);
  assert(machine.openDisk(0, chainedIrqPath));
  memset(irqSector, 0, sizeof(irqSector));
  memcpy(irqSector, idleProgram, sizeof(idleProgram));
  irqSector[510] = 0x55;
  irqSector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, irqSector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  assert(machine.writeMemoryBlock(0x11000, chainingKeyboardIrqNoEoi, sizeof(chainingKeyboardIrqNoEoi)));
  machine.writeMemory16(0x0009 * 4 + 0, 0x1000);
  machine.writeMemory16(0x0009 * 4 + 2, 0x1000);
  rawKeys[0] = 0x04; // HID a press
  machine.keyboard().onHidBootKeyboardReport(0, rawKeys);
  rawKeys[0] = 0;
  machine.keyboard().onHidBootKeyboardReport(0, rawKeys);
  rawKeys[0] = 0x05; // HID b press queued behind the first make/break pair
  machine.keyboard().onHidBootKeyboardReport(0, rawKeys);
  rawKeys[0] = 0;
  machine.keyboard().onHidBootKeyboardReport(0, rawKeys);
  for (int i = 0; i < 256; ++i)
    machine.stepCpu();
  assert(machine.readMemory16(0x041a) == 0x001e);
  assert(machine.readMemory16(0x041c) == 0x0022);
  assert(machine.readMemory16(0x041e) == 0x1e61);
  assert(machine.readMemory16(0x0420) == 0x3062);
  unlink(chainedIrqPath);

  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x1e41); // 'A', yellow on blue
  assert(machine.readVideoMemory16(PcMachine::TextColorMemoryBase) == 0x1e41);
  uint16_t line[PcTextRenderer::Width] = {};
  bool sawForeground = false;
  uint16_t yellow = PcTextRenderer::cgaColorRgb565(0x0e);
  uint16_t blue = PcTextRenderer::cgaColorRgb565(0x01);
  for (int y = 0; y < PcTextRenderer::CellHeight; ++y) {
    machine.renderText80Line(y, line);
    for (int i = 0; i < PcTextRenderer::CellWidth; ++i) {
      assert(line[i] == yellow || line[i] == blue);
      sawForeground = sawForeground || line[i] == yellow;
    }
  }
  assert(sawForeground);

  static uint8_t int10UserFont[PcTextRenderer::CellHeight] = {};
  int10UserFont[0] = 0x80; // only the left-most pixel of 'A' is foreground
  assert(machine.writeMemoryBlock(0x0600, int10UserFont, sizeof(int10UserFont)));
  PcI8086::setAX(0x1100); // INT 10h: load user font
  PcI8086::setBH(PcTextRenderer::CellHeight);
  PcI8086::setBL(0x00); // block 0
  PcI8086::setCX(0x0001);
  PcI8086::setDX('A');
  PcI8086::setES(0x0000);
  PcI8086::setBP(0x0600);
  PcI8086::triggerInterrupt(0x10);
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x1e41);
  machine.renderText80Line(0, line);
  assert(line[0] == yellow);
  for (int x = 1; x < PcTextRenderer::CellWidth; ++x)
    assert(line[x] == blue);

  static uint8_t blinkFont[256 * PcTextRenderer::CellHeight] = {};
  blinkFont['A' * PcTextRenderer::CellHeight] = 0x80;
  machine.textRenderer().setFont({blinkFont, 256});
  machine.textRenderer().setFrameCounter(0x20);
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x9e41); // blink/intense-bg attr
  machine.renderText80Line(0, line);
  for (int x = 0; x < PcTextRenderer::CellWidth; ++x)
    assert(line[x] == blue);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x30); // attribute mode-control index
  machine.writePort(0x03c0, 0x01); // blink disabled: attr bit 7 is bright background
  machine.renderText80Line(0, line);
  uint16_t brightBlue = PcTextRenderer::cgaColorRgb565(0x09);
  assert(line[0] == yellow);
  for (int x = 1; x < PcTextRenderer::CellWidth; ++x)
    assert(line[x] == brightBlue);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x30);
  machine.writePort(0x03c0, 0x09); // blink enabled again
  machine.renderText80Line(0, line);
  for (int x = 0; x < PcTextRenderer::CellWidth; ++x)
    assert(line[x] == blue);

  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x0141); // fg attribute 1
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.renderText80Line(0, line);
  assert(line[0] == 0xf800);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x21); // leave AC in data phase for palette register 1
  PcI8086::setAX(0x1008); // BIOS read-overscan must not disturb AC phase/index
  PcI8086::triggerInterrupt(0x10);
  machine.writePort(0x03c0, 0x02); // still data for palette register 1, not a fresh index
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x21);
  assert(machine.readPort(0x03c1) == 0x02);

  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x21); // palette register 1
  machine.writePort(0x03c0, 0x02); // attribute color 1 now maps to DAC 2
  machine.renderText80Line(0, line);
  assert(line[0] == 0x001f);
  PcI8086::setAX(0x1018); // BIOS set PEL mask must refresh text-renderer DAC colors too
  PcI8086::setBX(0x0001);
  PcI8086::triggerInterrupt(0x10);
  assert(machine.vgaDacState().pelMask == 0x01);
  machine.renderText80Line(0, line);
  assert(line[0] == 0x0000);
  PcI8086::setAX(0x1019);
  PcI8086::setBX(0x0000);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::BL() == 0x01);
  PcI8086::setAX(0x1018);
  PcI8086::setBX(0x00ff);
  PcI8086::triggerInterrupt(0x10);
  machine.renderText80Line(0, line);
  assert(line[0] == 0x001f);
  machine.writePort(0x03c6, 0x01); // PEL mask maps DAC 2 to DAC 0
  machine.renderText80Line(0, line);
  assert(line[0] == 0x0000);
  machine.writePort(0x03c6, 0xff);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x32); // color plane enable register
  machine.writePort(0x03c0, 0x00); // mask attribute color bits before palette lookup
  machine.renderText80Line(0, line);
  assert(line[0] == 0x0000);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x32);
  machine.writePort(0x03c0, 0x0f);
  machine.renderText80Line(0, line);
  assert(line[0] == 0x001f);

  machine.writeMemory8(0x0700, 0x01);
  machine.writeMemory8(0x0701, 0x02);
  machine.writeMemory8(0x0702, 0x03);
  machine.writeMemory8(0x0703, 0x10);
  machine.writeMemory8(0x0704, 0x20);
  machine.writeMemory8(0x0705, 0x30);
  machine.writeMemory8(0x0706, 0x3f);
  machine.writeMemory8(0x0707, 0x00);
  machine.writeMemory8(0x0708, 0x00);
  PcI8086::setAX(0x1012); // set block of DAC registers
  PcI8086::setBX(0x0020);
  PcI8086::setCX(0x0003);
  PcI8086::setES(0x0000);
  PcI8086::setDX(0x0700);
  PcI8086::triggerInterrupt(0x10);
  memset(machine.ram() + 0x0730, 0, 9);
  PcI8086::setAX(0x1017); // read block of DAC registers
  PcI8086::setBX(0x0020);
  PcI8086::setCX(0x0003);
  PcI8086::setES(0x0000);
  PcI8086::setDX(0x0730);
  PcI8086::triggerInterrupt(0x10);
  for (int i = 0; i < 9; ++i)
    assert(machine.readMemory8(0x0730 + static_cast<uint32_t>(i)) == machine.readMemory8(0x0700 + static_cast<uint32_t>(i)));
  PcI8086::setAX(0x1015); // read individual DAC register from the block
  PcI8086::setBX(0x0021);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::DH() == 0x10);
  assert(PcI8086::CH() == 0x20);
  assert(PcI8086::CL() == 0x30);
  PcI8086::setAX(0x101b); // gray-scale summing on DAC index 22h
  PcI8086::setBX(0x0022);
  PcI8086::setCX(0x0001);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x1015);
  PcI8086::setBX(0x0022);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::DH() == 0x13); // round((30*63 + 50) / 100)
  assert(PcI8086::CH() == 0x13);
  assert(PcI8086::CL() == 0x13);

  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x21); // attribute color 1 maps to DAC low index 1
  machine.writePort(0x03c0, 0x01);
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x0141);
  PcI8086::setAX(0x1010);
  PcI8086::setBX(0x0001);
  PcI8086::setDX(0x3f00);
  PcI8086::setCX(0x0000);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x1010);
  PcI8086::setBX(0x0021);
  PcI8086::setDX(0x0000);
  PcI8086::setCX(0x3f00);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x1010);
  PcI8086::setBX(0x00c1);
  PcI8086::setDX(0x0000);
  PcI8086::setCX(0x003f);
  PcI8086::triggerInterrupt(0x10);
  machine.renderText80Line(0, line);
  assert(line[0] == pcMachineRgb565From6Bit(0x3f, 0x00, 0x00));
  PcI8086::setAX(0x1013); // 16 pages of 16 colors
  PcI8086::setBX(0x0100);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x1013);
  PcI8086::setBX(0x0201); // select page 2: DAC index 21h for attr 1
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x101a);
  PcI8086::setBX(0x0000);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::BL() == 0x01);
  assert(PcI8086::BH() == 0x02);
  machine.renderText80Line(0, line);
  assert(line[0] == pcMachineRgb565From6Bit(0x00, 0x3f, 0x00));
  PcI8086::setAX(0x1013); // 4 pages of 64 colors
  PcI8086::setBX(0x0000);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x1013);
  PcI8086::setBX(0x0301); // select page 3: DAC index C1h for attr 1
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x101a);
  PcI8086::setBX(0x0000);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::BL() == 0x00);
  assert(PcI8086::BH() == 0x03);
  machine.renderText80Line(0, line);
  assert(line[0] == pcMachineRgb565From6Bit(0x00, 0x00, 0x3f));
  machine.textRenderer().setFont(PcTextRenderer::defaultFont());
  machine.textRenderer().setFrameCounter(0);

  machine.reset();
  machine.writePort(0x03ce, 0x06);
  machine.writePort(0x03cf, 0x04); // expose VGA A000 font plane aperture in text mode
  machine.writePort(0x03c4, 0x04);
  machine.writePort(0x03c5, 0x06); // disable odd/even so map mask selects plane 2 directly
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x04); // write character generator plane
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 0x4000 + static_cast<uint32_t>('A') * 32, 0xff);
  machine.writePort(0x03c4, 0x03);
  machine.writePort(0x03c5, 0x04); // attr bit 3 clear: map 0, set: map 1
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x0f41); // bit 3 selects map 1, not high intensity
  machine.renderText80Line(0, line);
  uint16_t lightGray = PcTextRenderer::cgaColorRgb565(0x07);
  for (int x = 0; x < PcTextRenderer::CellWidth; ++x)
    assert(line[x] == lightGray);

  machine.reset();
  machine.writePort(0x03ce, 0x06);
  machine.writePort(0x03cf, 0x04); // expose VGA A000 font plane aperture in text mode
  machine.writePort(0x03c4, 0x04);
  machine.writePort(0x03c5, 0x06);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x04);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 0x4000 + static_cast<uint32_t>('A') * 32, 0xff);
  PcI8086::setAX(0x1103); // INT 10h: set VGA font block specifier
  PcI8086::setBX(0x0004); // attr bit 3 clear -> map 0, set -> map 1
  PcI8086::triggerInterrupt(0x10);
  machine.writePort(0x03c4, 0x03);
  assert(machine.readPort(0x03c5) == 0x04);
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x0f41);
  machine.renderText80Line(0, line);
  for (int x = 0; x < PcTextRenderer::CellWidth; ++x)
    assert(line[x] == lightGray);

  machine.reset();
  machine.writePort(0x03d4, 0x14);
  machine.writePort(0x03d5, 0x00); // make underline scan line visible at the top of the cell
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x0120); // underline-eligible blue-on-black space
  machine.renderText80Line(0, line);
  uint16_t blueUnderline = PcTextRenderer::cgaColorRgb565(0x01);
  for (int x = 0; x < PcTextRenderer::CellWidth; ++x)
    assert(line[x] == blueUnderline);
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x0320); // bit 1 suppresses VGA/MDA underline
  machine.renderText80Line(0, line);
  for (int x = 0; x < PcTextRenderer::CellWidth; ++x)
    assert(line[x] == PcTextRenderer::cgaColorRgb565(0x00));

  machine.reset();
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x1e41);
  machine.renderText80Line(0, line);
  assert(line[0] != 0x0000);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x00); // Attribute Controller PAS clear: text display disabled
  machine.renderText80Line(0, line);
  for (int x = 0; x < PcTextRenderer::Width; ++x)
    assert(line[x] == 0x0000);
  uint16_t disabledWideTextLine[PcTextRenderer::Width9] = {};
  machine.textRenderer().renderLine9Dot(machine.text80Buffer(), 0, disabledWideTextLine);
  for (int x = 0; x < PcTextRenderer::Width9; ++x)
    assert(disabledWideTextLine[x] == 0x0000);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x20); // PAS set: display enabled again
  machine.renderText80Line(0, line);
  assert(line[0] != 0x0000);
  machine.writePort(0x03c4, 0x01);
  machine.writePort(0x03c5, 0x20); // sequencer clocking mode bit 5: screen off blanks text too
  machine.renderText80Line(0, line);
  for (int x = 0; x < PcTextRenderer::Width; ++x)
    assert(line[x] == 0x0000);
  machine.writePort(0x03c5, 0x00);
  machine.writePort(0x03c4, 0x00);
  machine.writePort(0x03c5, 0x00); // sequencer reset asserted: display output blanked
  machine.renderText80Line(0, line);
  for (int x = 0; x < PcTextRenderer::Width; ++x)
    assert(line[x] == 0x0000);
  machine.writePort(0x03c5, 0x03);
  machine.writePort(0x03c3, 0x00); // video subsystem disabled
  machine.renderText80Line(0, line);
  for (int x = 0; x < PcTextRenderer::Width; ++x)
    assert(line[x] == 0x0000);
  machine.writePort(0x03c3, 0x01);
  machine.writePort(0x03d4, 0x17);
  machine.writePort(0x03d5, 0x00); // CRTC mode-control reset bit clear
  machine.renderText80Line(0, line);
  for (int x = 0; x < PcTextRenderer::Width; ++x)
    assert(line[x] == 0x0000);
  machine.writePort(0x03d5, 0x80);
  machine.renderText80Line(0, line);
  assert(line[0] != 0x0000);

  machine.writePort(0x03d4, 0x17);
  machine.writePort(0x03d5, 0x00);
  machine.renderText80Line(0, line);
  assert(line[0] == 0x0000);
  PcI8086::setAX(0x0003); // BIOS text mode set must restore CRTC display-enable state
  PcI8086::triggerInterrupt(0x10);
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x1e41);
  machine.renderText80Line(0, line);
  assert(line[0] != 0x0000);
  machine.writePort(0x03d4, 0x17);
  assert((machine.readPort(0x03d5) & 0x80) != 0);

  machine.reset();
  static uint8_t lineGraphicsFont[256 * PcTextRenderer::CellHeight] = {};
  lineGraphicsFont[0xc4 * PcTextRenderer::CellHeight] = 0x01;
  machine.textRenderer().setFont({lineGraphicsFont, 256});
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x1ec4);
  uint16_t wideTextLine[PcTextRenderer::Width9] = {};
  machine.textRenderer().renderLine9Dot(machine.text80Buffer(), 0, wideTextLine);
  assert(wideTextLine[7] == yellow);
  assert(wideTextLine[8] == yellow); // VGA text mode defaults to 9-dot line-graphics expansion
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x30);
  assert(machine.readPort(0x03c1) == 0x0c); // text mode, line graphics, blink; graphics bit clear
  machine.writePort(0x03c0, 0x08); // disable line graphics while keeping text blink enabled
  machine.textRenderer().renderLine9Dot(machine.text80Buffer(), 0, wideTextLine);
  assert(wideTextLine[8] == PcTextRenderer::cgaColorRgb565(0x01));
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x30);
  machine.writePort(0x03c0, 0x0c); // restore default text mode-control
  machine.textRenderer().renderLine9Dot(machine.text80Buffer(), 0, wideTextLine);
  assert(wideTextLine[8] == yellow);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x33); // horizontal PEL panning
  machine.writePort(0x03c0, 0x07);
  machine.textRenderer().renderLine9Dot(machine.text80Buffer(), 0, wideTextLine);
  assert(wideTextLine[0] == yellow); // panned source starts on the repeated ninth dot
  assert(wideTextLine[PcTextRenderer::Width9 - 1] == PcTextRenderer::cgaColorRgb565(0x00));
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x33);
  machine.writePort(0x03c0, 0x08);
  machine.textRenderer().renderLine9Dot(machine.text80Buffer(), 0, wideTextLine);
  assert(wideTextLine[0] == PcTextRenderer::cgaColorRgb565(0x01)); // raw 8 means zero effective pan in 9-dot text
  assert(wideTextLine[7] == yellow);
  assert(wideTextLine[8] == yellow);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x33);
  machine.writePort(0x03c0, 0x00);
  machine.writePort(0x03c4, 0x01);
  machine.writePort(0x03c5, 0x01); // 8-dot clocking disables ninth-dot repetition
  machine.textRenderer().renderLine9Dot(machine.text80Buffer(), 0, wideTextLine);
  assert(wideTextLine[8] != yellow);
  machine.textRenderer().setFont(PcTextRenderer::defaultFont());

  machine.reset();
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x1e20);
  PcI8086::setAX(0x0100); // INT 10h: set cursor shape
  PcI8086::setCX(0x0204);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x0200); // INT 10h: set cursor position
  PcI8086::setBX(0x0000);
  PcI8086::setDX(0x0000);
  PcI8086::triggerInterrupt(0x10);
  machine.renderText80Line(1, line);
  assert(line[0] == PcTextRenderer::cgaColorRgb565(0x01));
  machine.renderText80Line(2, line);
  for (int x = 0; x < PcTextRenderer::CellWidth; ++x)
    assert(line[x] == yellow);
  machine.renderText80Line(5, line);
  assert(line[0] == PcTextRenderer::cgaColorRgb565(0x01));
  PcI8086::setAX(0x0100); // cursor-disable bit in CH hides rendered cursor
  PcI8086::setCX(0x2004);
  PcI8086::triggerInterrupt(0x10);
  machine.renderText80Line(2, line);
  assert(line[0] == PcTextRenderer::cgaColorRgb565(0x01));

  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase + 2 * (PcTextRenderer::Columns + 2), 0x1e20);
  machine.writePort(0x03d4, 0x0a);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0b);
  machine.writePort(0x03d5, 0x00);
  uint16_t cursorAddress = PcTextRenderer::Columns + 2;
  machine.writePort(0x03d4, 0x0e);
  machine.writePort(0x03d5, static_cast<uint8_t>(cursorAddress >> 8));
  machine.writePort(0x03d4, 0x0f);
  machine.writePort(0x03d5, static_cast<uint8_t>(cursorAddress & 0xff));
  machine.renderText80Line(PcTextRenderer::CellHeight, line);
  assert(line[2 * PcTextRenderer::CellWidth] == yellow);
  machine.renderText80Line(0, line);
  assert(line[0] == PcTextRenderer::cgaColorRgb565(0x01));

  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase + 0x1000, 0x0743); // page 1, 'C'
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase + 0x1000 + 2, 0x1e20);
  cursorAddress = 0x0801;
  machine.writePort(0x03d4, 0x0e);
  machine.writePort(0x03d5, static_cast<uint8_t>(cursorAddress >> 8));
  machine.writePort(0x03d4, 0x0f);
  machine.writePort(0x03d5, static_cast<uint8_t>(cursorAddress & 0xff));
  machine.renderText80Line(0, line);
  assert(line[PcTextRenderer::CellWidth] != yellow);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x08);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x00); // text CRTC start address is word-addressed: 0x0800 -> byte 0x1000
  assert(machine.text80Buffer()[0] == 'C');
  machine.renderText80Line(0, line);
  assert(line[PcTextRenderer::CellWidth] == yellow);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x00);

  machine.writePort(0x03b4, 0x0f);
  machine.writePort(0x03b5, 0x2a);
  assert(machine.readPort(0x03b4) == 0x0f);
  assert(machine.readPort(0x03b5) == 0x2a);
  assert(machine.readPort(0x03bf) == 0x01);
  uint8_t hgcStatusA = machine.readPort(0x03ba);
  uint8_t hgcStatusB = machine.readPort(0x03ba);
  assert(hgcStatusA != hgcStatusB);
  assert((hgcStatusA & 0x70) == 0x10);
  assert((hgcStatusB & 0x70) == 0x10);
  machine.writePort(0x03b4, 0x0f);
  uint8_t const savedHgcCursorLow = machine.readPort(0x03b5);
  machine.writePort(0x03b5, 0x66);
  assert(machine.readPort(0x03b5) == 0x66);
  machine.writePort(0x03b5, savedHgcCursorLow);
  uint8_t const initialHgcStatusBit7 = machine.readPort(0x03ba) & 0x80;
  uint8_t changedHgcStatus = initialHgcStatusBit7;
  for (int i = 0; i < 8 && changedHgcStatus == initialHgcStatusBit7; ++i)
    changedHgcStatus = machine.readPort(0x03ba) & 0x80;
  assert(changedHgcStatus != initialHgcStatusBit7);
  uint8_t const hgcCardIdBits = machine.readPort(0x03ba) & 0x70;
  uint16_t const simcityStyleHgcCode = hgcCardIdBits == 0x50 ? 0x0382
                                     : hgcCardIdBits == 0x10 ? 0x0181
                                                             : 0x0380;
  assert(simcityStyleHgcCode == 0x0181);
  machine.writePort(0x00f9, 0x12);
  assert(machine.readPort(0x00f9) == 0x00);
  machine.writePort(0x2861, 0x12);
  assert(machine.readPort(0x2861) == 0x00);
  machine.writePort(0x0022, 0x01);
  machine.writePort(0x0023, 0x02);
  assert(machine.readPort(0x0022) == 0x00);
  assert(machine.readPort(0x0023) == 0x00);
  machine.writePort(0x0201, 0x00);
  assert(machine.readPort(0x0201) == 0xff);
  assert(machine.readPort(0x0238) == 0xff);
  assert(machine.readPort(0x023a) == 0xff);
  assert(machine.readPort(0x023e) == 0xff);
  assert(machine.readPort(0x023f) == 0xff);
  assert(machine.readPort(0x02e8) == 0xff);
  assert(machine.readPort(0x02ff) == 0xff);
  assert(machine.readPort(0x03e8) == 0xff);
  assert(machine.readPort(0x03ea) == 0xff);
  assert(machine.readPort(0x03ff) == 0xff);
  machine.writePort(0x56e0, 0x12);
  assert(machine.readPort(0x56e0) == 0xff);
  assert(machine.readPort(0x56e1) == 0xff);
  machine.writePort(0x92e8, 0x34);
  assert(machine.readPort(0x92e8) == 0xff);
  assert(machine.readPort(0x92e9) == 0xff);
  machine.writePort(0xaae0, 0x56);
  assert(machine.readPort(0xaae0) == 0xff);
  assert(machine.readPort(0xaae1) == 0xff);
  assert(machine.readPort(0xe2e0) == 0xff);
  assert(machine.readPort(0xe2e1) == 0xff);
  assert(machine.readPort(0x1ee0) == 0xff);
  assert(machine.readPort(0x1ee1) == 0xff);
  assert(machine.readPort(0x2110) == 0xff);
  assert(machine.readPort(0x2111) == 0xff);
  assert(machine.readPort(0x2120) == 0xff);
  assert(machine.readPort(0x2121) == 0xff);
  assert(machine.readPort(0x2130) == 0xff);
  assert(machine.readPort(0x2131) == 0xff);
  assert(machine.readPort(0x2140) == 0xff);
  assert(machine.readPort(0x2141) == 0xff);
  assert(machine.readPort(0x21f0) == 0xff);
  assert(machine.readPort(0x21f1) == 0xff);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x34);
  assert(machine.readPort(0x03d4) == 0x0c);
  assert(machine.readPort(0x03d5) == 0x34);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d8, 0x0a);
  machine.writePort(0x03d9, 0x25);
  assert(machine.readPort(0x03d8) == 0x0a);
  assert(machine.readPort(0x03d9) == 0x25);
  assert(machine.videoMode() == PcMachine::VideoMode::CgaGraphics320x200x4);
  assert(machine.readMemory8(0x0449) == 0x04);
  assert(machine.readMemory16(0x044a) == 40);
  assert(machine.readMemory16(0x044c) == 0x4000);
  assert(machine.readMemory16(0x044e) == 0x0000);
  assert(machine.readMemory8(0x0462) == 0x00);
  assert(machine.readMemory16(0x0463) == 0x03d4);
  machine.writeVideoMemory8(PcMachine::TextColorMemoryBase, 0xc0);
  std::vector<uint16_t> directCgaLine(PcMachine::CgaGraphicsWidth, 0);
  machine.renderGraphicsLine(0, directCgaLine.data());
  assert(directCgaLine[0] != 0x0000);
  machine.writePort(0x03d8, 0x02); // graphics selected but CGA video-enable bit is clear
  machine.renderGraphicsLine(0, directCgaLine.data());
  assert(directCgaLine[0] == 0x0000);
  machine.renderGraphicsLineForDisplay(0, directCgaLine.data());
  assert(directCgaLine[0] != 0x0000);
  machine.writePort(0x03d8, 0x0a);
  machine.renderGraphicsLine(0, directCgaLine.data());
  assert(directCgaLine[0] != 0x0000);
  machine.writePort(0x03d8, 0x1a);
  assert(machine.videoMode() == PcMachine::VideoMode::CgaGraphics640x200x2);
  assert(machine.readPort(0x03d9) == 0x25); // direct mode-control writes do not reset color-select hardware
  assert(machine.readMemory8(0x0449) == 0x06);
  assert(machine.readMemory16(0x044a) == 80);
  assert(machine.readMemory16(0x044c) == 0x4000);
  machine.writeVideoMemory8(PcMachine::TextColorMemoryBase, 0x80);
  machine.renderGraphicsLine(0, directCgaLine.data());
  assert(directCgaLine[0] != 0x0000);
  machine.writePort(0x03d8, 0x12); // 640px graphics with display disabled
  machine.renderGraphicsLine(0, directCgaLine.data());
  assert(directCgaLine[0] == 0x0000);
  machine.renderGraphicsLineForDisplay(0, directCgaLine.data());
  assert(directCgaLine[0] != 0x0000);
  machine.writePort(0x03d8, 0x1a);
  machine.renderGraphicsLine(0, directCgaLine.data());
  assert(directCgaLine[0] != 0x0000);
  machine.writePort(0x03d8, 0x0a);
  assert(machine.videoMode() == PcMachine::VideoMode::CgaGraphics320x200x4);
  assert(machine.readPort(0x03d9) == 0x25);
  machine.writeVideoMemory8(PcMachine::TextColorMemoryBase, 0x40);
  machine.renderGraphicsLine(0, directCgaLine.data());
  assert(directCgaLine[0] == pcMachineDefaultVgaDacRgb565(0x03));
  machine.writePort(0x03d8, 0x08);
  assert(machine.videoMode() == PcMachine::VideoMode::Text80);
  assert(machine.readMemory8(0x0449) == 0x03);
  assert(machine.readMemory16(0x044a) == PcTextRenderer::Columns);
  assert(machine.readMemory16(0x044c) == 0x1000);
  machine.writePort(0x00a0, 0x20); // slave PIC EOI/command used by PC/AT-era DOS games
  machine.writePort(0x00a1, 0xfb);
  assert(machine.readPort(0x00a0) == 0x00);
  assert(machine.readPort(0x00a1) == 0xfb);
  uint64_t const legacyPortReadsBefore = machine.diagnostics().unsupportedPortReadCount;
  uint64_t const legacyPortWritesBefore = machine.diagnostics().unsupportedPortWriteCount;
  machine.writePort(0x03f2, 0x1c); // floppy controller DOR: old games toggle motors during probes
  assert(machine.readPort(0x03f2) == 0x1c);
  machine.writePort(0x03bc, 0x55); // LPT data ports may be probed directly even with no printer
  machine.writePort(0x0378, 0xaa);
  machine.writePort(0x0278, 0x5a);
  assert(machine.readPort(0x03bc) == 0xff);
  assert(machine.readPort(0x03bd) == 0xff);
  assert(machine.readPort(0x03be) == 0x00);
  assert(machine.readPort(0x0378) == 0xff);
  assert(machine.readPort(0x0379) == 0xff);
  assert(machine.readPort(0x037a) == 0x00);
  assert(machine.readPort(0x0278) == 0xff);
  assert(machine.readPort(0x0279) == 0xff);
  assert(machine.readPort(0x027a) == 0x00);
  assert(machine.readPort(0x001c) == 0xff); // low DMA/decode startup probe on some MS-DOS images
  assert(machine.readPort(0x001d) == 0xff);
  assert(machine.readPort(0x0232) == 0xff); // alternate legacy adapter probe range
  assert(machine.readPort(0x0236) == 0xff);
  machine.writePort(0x001c, 0x00);
  machine.writePort(0x001d, 0x00);
  machine.writePort(0x0232, 0x00);
  machine.writePort(0x0236, 0x00);
  machine.writePort(0x02f2, 0xff); // MS-DOS 6.x probes this adjacent legacy adapter window
  assert(machine.readPort(0x02f2) == 0xff);
  machine.writePort(0x02f7, 0x00);
  assert(machine.diagnostics().unsupportedPortReadCount == legacyPortReadsBefore);
  assert(machine.diagnostics().unsupportedPortWriteCount == legacyPortWritesBefore);
  bool sawActiveDisplay = false;
  bool sawHorizontalBlank = false;
  bool sawVerticalRetrace = false;
  for (int sample = 0; sample < 400 &&
                       !(sawActiveDisplay && sawHorizontalBlank && sawVerticalRetrace);
       ++sample) {
    uint8_t const status = machine.readPort(0x03da);
    sawActiveDisplay = sawActiveDisplay || ((status & 0x09) == 0x00);
    sawHorizontalBlank = sawHorizontalBlank || ((status & 0x09) == 0x01);
    sawVerticalRetrace = sawVerticalRetrace || ((status & 0x09) == 0x09);
    usleep(100);
  }
  assert(sawActiveDisplay);
  assert(sawHorizontalBlank);
  assert(sawVerticalRetrace);

  machine.setVideoMode(PcMachine::VideoMode::Text80);
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x1e20); // space: visible top-left pixel is background blue
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x32);
  machine.writePort(0x03c0, 0x0f); // status mux selects color bits 0 and 2
  assert((machine.readPort(0x03da) & 0x30) == 0x10);
  static uint8_t statusMuxFont[256 * PcTextRenderer::CellHeight] = {};
  statusMuxFont['A' * PcTextRenderer::CellHeight] = 0x80; // top-left pixel is foreground
  machine.textRenderer().setFont({statusMuxFont, 256});
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x1e41);
  assert((machine.readPort(0x03da) & 0x30) == 0x20);
  machine.textRenderer().setFont(PcTextRenderer::defaultFont());

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0xa5);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x32);
  machine.writePort(0x03c0, 0x0f); // status mux: color bits 0 and 2, with all color planes enabled
  assert((machine.readPort(0x03da) & 0x30) == 0x30);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x32);
  machine.writePort(0x03c0, 0x2f); // status mux: color bits 1 and 3, with all color planes enabled
  assert((machine.readPort(0x03da) & 0x30) == 0x00);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x32);
  machine.writePort(0x03c0, 0x3f); // status mux: color bits 6 and 7, with all color planes enabled
  assert((machine.readPort(0x03da) & 0x30) == 0x20);
  machine.writePort(0x03c6, 0x0f); // RAMDAC PEL mask must not affect Attribute Controller status MUX
  assert((machine.readPort(0x03da) & 0x30) == 0x20);
  machine.writePort(0x03c6, 0xff);
  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 0, 0x00);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 4, 0xa5);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x01); // mode-13h dword start: first visible pixel is byte offset 4
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x32);
  machine.writePort(0x03c0, 0x0f);
  assert((machine.readPort(0x03da) & 0x30) == 0x30);

  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 0, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 4, 0x02);
  machine.writePort(0x03d4, 0x07);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x09);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x18);
  machine.writePort(0x03d5, 0x01); // split screen starts after the sampled top line
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x32);
  machine.writePort(0x03c0, 0x0f);
  assert((machine.readPort(0x03da) & 0x30) == 0x00); // status mux samples CRTC-started top line, not split line 1
  machine.writePort(0x03d4, 0x18);
  machine.writePort(0x03d5, 0xff);
  machine.writePort(0x03d4, 0x07);
  machine.writePort(0x03d5, 0x10);
  machine.writePort(0x03d4, 0x09);
  machine.writePort(0x03d5, 0x40);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x00);

  machine.setVideoMode(PcMachine::VideoMode::Text80);
  machine.writePort(0x03c2, 0x67);
  assert(machine.readPort(0x03cc) == 0x67);
  assert(machine.readMemory16(0x0463) == 0x03d4);
  machine.writePort(0x03c2, 0x66);
  assert(machine.readMemory16(0x0463) == 0x03b4);
  machine.writePort(0x03c2, 0x67);
  assert(machine.readMemory16(0x0463) == 0x03d4);
  machine.writeMemory8(0x0488, 0x09); // switches 0 and 3 set for color VGA
  assert((machine.readPort(0x03c2) & 0x10) == 0x00); // selected by Misc bits 2-3 = 1
  machine.writePort(0x03c2, 0x63); // select switch 0
  assert((machine.readPort(0x03c2) & 0x10) == 0x10);
  machine.writePort(0x03c2, 0x6b); // select switch 2
  assert((machine.readPort(0x03c2) & 0x10) == 0x00);
  machine.writePort(0x03c2, 0x6f); // select switch 3
  assert((machine.readPort(0x03c2) & 0x10) == 0x10);
  machine.writePort(0x03c2, 0x63);
  machine.writePort(0x03c3, 0x01);
  assert(machine.readPort(0x03c3) == 0x01);
  machine.writePort(0x03c3, 0xfe); // only bit 0 enables the VGA video subsystem
  assert(machine.readPort(0x03c3) == 0x00);
  assert((machine.readPort(0x03da) & 0x30) == 0x00);
  machine.writePort(0x03c3, 0x01);
  assert(machine.readPort(0x03c3) == 0x01);
  machine.writePort(0x03ca, 0x08);
  assert(machine.readPort(0x03ca) == 0x08);
  machine.writePort(0x03da, 0x03);
  assert(machine.readPort(0x03ca) == 0x00); // reserved feature bits read back clear
  machine.writePort(0x03ba, 0x0f);
  assert(machine.readPort(0x03ca) == 0x08); // only Vertical Sync Select is preserved

  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x30); // attribute mode-control index
  machine.writePort(0x03c0, 0xff);
  assert(machine.readPort(0x03c1) == 0xef);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x33); // horizontal PEL panning
  machine.writePort(0x03c0, 0xff);
  assert(machine.readPort(0x03c1) == 0x0f);

  machine.writePort(0x03c4, 0x00);
  machine.writePort(0x03c5, 0xff);
  assert(machine.readPort(0x03c5) == 0x03);
  machine.writePort(0x03c4, 0x01);
  machine.writePort(0x03c5, 0xff);
  assert(machine.readPort(0x03c5) == 0x3d);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0xff);
  assert(machine.readPort(0x03c5) == 0x0f);
  machine.writePort(0x03c4, 0x03);
  machine.writePort(0x03c5, 0xff);
  assert(machine.readPort(0x03c5) == 0x3f);
  machine.writePort(0x03c4, 0x04);
  machine.writePort(0x03c5, 0xff);
  assert(machine.readPort(0x03c5) == 0x0f);

  machine.writePort(0x03ce, 0x00);
  machine.writePort(0x03cf, 0xff);
  assert(machine.readPort(0x03cf) == 0x0f);
  machine.writePort(0x03ce, 0x03);
  machine.writePort(0x03cf, 0xff);
  assert(machine.readPort(0x03cf) == 0x1f);
  machine.writePort(0x03ce, 0x04);
  machine.writePort(0x03cf, 0xff);
  assert(machine.readPort(0x03cf) == 0x03);
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0xff);
  assert(machine.readPort(0x03cf) == 0x7b);
  machine.writePort(0x03ce, 0x06);
  machine.writePort(0x03cf, 0xff);
  assert(machine.readPort(0x03cf) == 0x0f);
  machine.writePort(0x03ce, 0x07);
  machine.writePort(0x03cf, 0xff);
  assert(machine.readPort(0x03cf) == 0x0f);

  machine.writePort(0x03d4, 0x08);
  machine.writePort(0x03d5, 0xff);
  assert(machine.readPort(0x03d5) == 0x7f);
  machine.writePort(0x03d4, 0x0a);
  machine.writePort(0x03d5, 0xff);
  assert(machine.readPort(0x03d5) == 0x3f);
  machine.writePort(0x03d4, 0x0b);
  machine.writePort(0x03d5, 0xff);
  assert(machine.readPort(0x03d5) == 0x7f);
  machine.writePort(0x03d4, 0x14);
  machine.writePort(0x03d5, 0xff);
  assert(machine.readPort(0x03d5) == 0x7f);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03ce, 0x05);
  assert((machine.readPort(0x03cf) & 0x40) == 0x40); // BIOS mode 13h exposes 256-color shift mode
  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03ce, 0x05);
  assert((machine.readPort(0x03cf) & 0x40) == 0x00);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c2, 0x62); // VGA mono I/O select: status port is 03BAh
  assert(machine.readMemory16(0x0463) == 0x03b4);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x21); // leave AC flip-flop in data phase for palette register 1
  uint8_t monoVgaStatusA = machine.readPort(0x03ba);
  uint8_t monoVgaStatusB = machine.readPort(0x03ba);
  assert((monoVgaStatusA & ~0x39) == 0);
  assert((monoVgaStatusB & ~0x39) == 0);
  machine.writePort(0x03c0, 0x22); // must be interpreted as an index after 03BAh reset
  machine.writePort(0x03c0, 0x05); // data for palette register 2
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x22);
  assert(machine.readPort(0x03c1) == 0x05);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x23);
  machine.writePort(0x03c0, 0xff); // AC palette registers expose only six DAC-select bits
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x23);
  assert(machine.readPort(0x03c1) == 0x3f);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x31);
  machine.writePort(0x03c0, 0xc1); // overscan is also a six-bit DAC selector
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x31);
  assert(machine.readPort(0x03c1) == 0x01);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x21);
  assert(machine.readPort(0x03c1) == 0x01);

  machine.writePort(0x03b4, 0x06);
  machine.writePort(0x03b5, 0x12);
  machine.writePort(0x03b4, 0x11);
  machine.writePort(0x03b5, 0x80); // protect VGA CRTC registers 00h-07h through mono port
  machine.writePort(0x03b4, 0x06);
  machine.writePort(0x03b5, 0x34);
  assert(machine.readPort(0x03b5) == 0x12);
  machine.writePort(0x03b4, 0x11);
  machine.writePort(0x03b5, 0x00);
  machine.writePort(0x03c2, 0x63);
  assert(machine.readMemory16(0x0463) == 0x03d4);

  machine.writePort(0x03c2, 0x00); // mono I/O selected and CPU VRAM aperture disabled
  assert(machine.readMemory16(0x0463) == 0x03b4);
  machine.writePort(0x03ca, 0x07);
  PcI8086::setAX(0x0013);
  PcI8086::triggerInterrupt(0x10);
  assert(machine.videoMode() == PcMachine::VideoMode::VgaGraphics320x200x256);
  assert((machine.readPort(0x03cc) & 0x03) == 0x03); // color I/O + ERAM restored by BIOS mode set
  assert(machine.readMemory16(0x0463) == 0x03d4);
  assert(machine.readPort(0x03ca) == 0x00);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x01);
  std::vector<uint16_t> modeSetGraphicsLine(PcMachine::VgaGraphics256Width, 0);
  machine.renderGraphicsLine(0, modeSetGraphicsLine.data());
  assert(modeSetGraphicsLine[0] == 0xf800);

  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x25);
  assert(machine.readPort(0x03c0) == 0x25);
  machine.writePort(0x03b8, 0x0a); // graphics + video enable
  assert(machine.readPort(0x03b8) == 0x0a);
  assert(machine.videoMode() == PcMachine::VideoMode::HerculesGraphics);
  assert(machine.isHerculesVideoEnabled());
  assert((machine.readMemory16(0x0410) & 0x0030) == 0x0030);
  machine.writeVideoMemory8(PcMachine::HerculesGraphicsMemoryBase, 0x80);
  std::vector<uint16_t> hgcLine(PcMachine::HerculesGraphicsWidth, 0);
  machine.renderHerculesGraphicsLine(0, hgcLine.data());
  assert(hgcLine[0] == 0xffff);
  assert(hgcLine[1] == 0x0000);
  machine.writePort(0x03b8, 0x02); // graphics selected, video output disabled
  assert(machine.videoMode() == PcMachine::VideoMode::HerculesGraphics);
  assert(!machine.isHerculesVideoEnabled());
  machine.renderHerculesGraphicsLine(0, hgcLine.data());
  assert(hgcLine[0] == 0x0000);
  machine.writePort(0x03b8, 0x0a);
  assert(machine.isHerculesVideoEnabled());
  machine.renderHerculesGraphicsLine(0, hgcLine.data());
  assert(hgcLine[0] == 0xffff);
  machine.writeVideoMemory8(PcMachine::HerculesGraphicsMemoryBase + 0x2000, 0x40);
  machine.renderHerculesGraphicsLine(1, hgcLine.data());
  assert(hgcLine[0] == 0x0000);
  assert(hgcLine[1] == 0xffff);
  uint8_t hgcPixelColor = 0;
  assert(machine.writeGraphicsPixel(4, 2, 0x01, false));
  assert(machine.readGraphicsPixel(4, 2, &hgcPixelColor));
  assert(hgcPixelColor == 0x01);
  machine.renderHerculesGraphicsLine(2, hgcLine.data());
  assert(hgcLine[4] == 0xffff);
  assert(machine.writeGraphicsPixel(4, 2, 0x01, true));
  assert(machine.readGraphicsPixel(4, 2, &hgcPixelColor));
  assert(hgcPixelColor == 0x00);
  machine.renderHerculesGraphicsLine(2, hgcLine.data());
  assert(hgcLine[4] == 0x0000);
  machine.writePort(0x03bf, 0x03); // enable Hercules graphics and page-1 addressing
  assert(machine.readPort(0x03bf) == 0x03);
  assert((machine.readMemory16(0x0410) & 0x0030) == 0x0030);
  machine.writeVideoMemory8(PcMachine::HerculesGraphicsMemoryBase + 0x8000, 0x20);
  machine.writePort(0x03b8, 0x8a); // graphics + video enable + page 1 select
  machine.renderHerculesGraphicsLine(0, hgcLine.data());
  assert(hgcLine[0] == 0x0000);
  assert(hgcLine[2] == 0xffff);
  machine.writePort(0x03b8, 0x00);
  assert(machine.videoMode() == PcMachine::VideoMode::Text80);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x01);
  std::vector<uint16_t> graphicsLine(PcMachine::VgaGraphics16Width, 0);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  uint8_t const miscBeforeVramAccessTest = static_cast<uint8_t>(machine.readPort(0x03cc) | 0x02);
  machine.writePort(0x03c2, miscBeforeVramAccessTest);
  machine.writePort(0x03c2, static_cast<uint8_t>(miscBeforeVramAccessTest & ~0x02)); // ERAM clear: CPU VRAM aperture disabled
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0xff);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x02);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800); // display still scans existing VRAM while CPU aperture is disabled
  machine.writePort(0x03c2, miscBeforeVramAccessTest); // restore ERAM
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x02);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x001f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x01);
  machine.writePort(0x03c8, 0x31);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0x31);
  assert((machine.readPort(0x03c7) & 0x03) == 0x00);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[1] == 0xf81f); // mode 13h defaults to 8-bit DAC indexing
  machine.writePort(0x03c8, 0x41);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 2, 0x41);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x21); // AC palette register 1
  machine.writePort(0x03c0, 0x02); // must not remap 41h while AC 256-color bit is set
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[2] == 0x07e0);

  machine.reset();
  PcI8086::setAX(0x0013);
  PcI8086::triggerInterrupt(0x10);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  PcI8086::setAX(0x0200);
  PcI8086::setBX(0x0000);
  PcI8086::setDX(0x0000);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x0e41); // teletype 'A' in graphics mode
  PcI8086::setBX(0x0001); // page 0, color 1
  PcI8086::triggerInterrupt(0x10);
  int glyphX = -1;
  int glyphY = -1;
  for (int y = 0; y < 8 && glyphX < 0; ++y) {
    uint8_t const sourceRow = static_cast<uint8_t>(y * PcTextRenderer::CellHeight / 8);
    uint8_t const bits = tabdos::PcFont8x16Data['A' * PcTextRenderer::CellHeight + sourceRow];
    for (int x = 0; x < 8; ++x) {
      if (bits & (0x80 >> x)) {
        glyphX = x;
        glyphY = y;
        break;
      }
    }
  }
  assert(glyphX >= 0 && glyphY >= 0);
  uint8_t pixelColor = 0;
  assert(machine.readGraphicsPixel(glyphX, glyphY, &pixelColor));
  assert(pixelColor == 0x01);
  std::fill(graphicsLine.begin(), graphicsLine.end(), 0);
  machine.renderGraphicsLine(glyphY, graphicsLine.data());
  assert(graphicsLine[glyphX] == 0xf800);
  assert(machine.videoMemory()[PcMachine::TextColorMemoryBase - PcMachine::VideoMemoryBase] == 0x00);
  assert(machine.videoMemory()[PcMachine::TextColorMemoryBase - PcMachine::VideoMemoryBase + 1] == 0x00);
  assert(machine.readMemory16(0x0450) == 0x0001);

  machine.reset();
  PcI8086::setAX(0x0013);
  PcI8086::triggerInterrupt(0x10);
  uint8_t customGraphicsFont[8] = {0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  assert(machine.writeMemoryBlock(0x0700, customGraphicsFont, sizeof(customGraphicsFont)));
  PcI8086::setAX(0x1120); // graphics-mode user 8x8 font
  PcI8086::setCX(0x0001);
  PcI8086::setDX('A');
  PcI8086::setES(0x0000);
  PcI8086::setBP(0x0700);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x0200);
  PcI8086::setBX(0x0000);
  PcI8086::setDX(0x0000);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x0941);
  PcI8086::setBX(0x0002);
  PcI8086::setCX(0x0001);
  PcI8086::triggerInterrupt(0x10);
  assert(machine.readGraphicsPixel(0, 0, &pixelColor));
  assert(pixelColor == 0x02);
  assert(machine.readGraphicsPixel(7, 0, &pixelColor));
  assert(pixelColor == 0x02);
  assert(machine.readGraphicsPixel(0, 1, &pixelColor));
  assert(pixelColor == 0x00);
  assert(machine.readMemory16(0x0485) == 8);
  PcI8086::setAX(0x1123); // restore ROM 8x8 graphics font for following BIOS graphics-character checks
  PcI8086::triggerInterrupt(0x10);

  machine.writePort(0x03c8, 0x04);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  PcI8086::setAX(0x0200);
  PcI8086::setBX(0x0000);
  PcI8086::setDX(0x0002);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x0a41); // graphics write-character-only also uses BL as foreground color
  PcI8086::setBX(0x0004); // page 0, color 4
  PcI8086::setCX(0x0001);
  PcI8086::triggerInterrupt(0x10);
  pixelColor = 0;
  assert(machine.readGraphicsPixel(2 * 8 + glyphX, glyphY, &pixelColor));
  assert(pixelColor == 0x04);
  std::fill(graphicsLine.begin(), graphicsLine.end(), 0);
  machine.renderGraphicsLine(glyphY, graphicsLine.data());
  assert(graphicsLine[2 * 8 + glyphX] == 0x07e0);

  machine.reset();
  PcI8086::setAX(0x0013);
  PcI8086::triggerInterrupt(0x10);
  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  uint8_t const graphicsString[] = {'H', 0x02, 'I', 0x02};
  assert(machine.writeMemoryBlock(0x0600, graphicsString, sizeof(graphicsString)));
  PcI8086::setAX(0x1303); // write string with attributes, update cursor
  PcI8086::setBX(0x0000); // page 0
  PcI8086::setCX(0x0002);
  PcI8086::setDH(0x01); // row 1
  PcI8086::setDL(0x02); // column 2
  PcI8086::setES(0x0000);
  PcI8086::setBP(0x0600);
  PcI8086::triggerInterrupt(0x10);
  int stringGlyphX = -1;
  int stringGlyphY = -1;
  for (int y = 0; y < 8 && stringGlyphX < 0; ++y) {
    uint8_t const sourceRow = static_cast<uint8_t>(y * PcTextRenderer::CellHeight / 8);
    uint8_t const bits = tabdos::PcFont8x16Data['H' * PcTextRenderer::CellHeight + sourceRow];
    for (int x = 0; x < 8; ++x) {
      if (bits & (0x80 >> x)) {
        stringGlyphX = x;
        stringGlyphY = y;
        break;
      }
    }
  }
  assert(stringGlyphX >= 0 && stringGlyphY >= 0);
  pixelColor = 0;
  assert(machine.readGraphicsPixel(2 * 8 + stringGlyphX, 1 * 8 + stringGlyphY, &pixelColor));
  assert(pixelColor == 0x02);
  std::fill(graphicsLine.begin(), graphicsLine.end(), 0);
  machine.renderGraphicsLine(1 * 8 + stringGlyphY, graphicsLine.data());
  assert(graphicsLine[2 * 8 + stringGlyphX] == 0x001f);
  assert(machine.videoMemory()[PcMachine::TextColorMemoryBase - PcMachine::VideoMemoryBase + 2 * (1 * 40 + 2)] == 0x00);
  assert(machine.readMemory16(0x0450) == 0x0104);

  assert(machine.writeGraphicsPixel(16, 8, 0x02, false));
  assert(machine.writeGraphicsPixel(24, 8, 0x02, false));
  PcI8086::setAH(0x06); // scroll up / clear graphics text window
  PcI8086::setAL(0x00); // clear window
  PcI8086::setBH(0x00); // fill color
  PcI8086::setCH(0x01);
  PcI8086::setCL(0x02);
  PcI8086::setDH(0x01);
  PcI8086::setDL(0x02);
  PcI8086::triggerInterrupt(0x10);
  pixelColor = 0xff;
  assert(machine.readGraphicsPixel(16, 8, &pixelColor));
  assert(pixelColor == 0x00);
  assert(machine.readGraphicsPixel(24, 8, &pixelColor));
  assert(pixelColor == 0x02);

  assert(machine.writeGraphicsPixel(0, 0, 0x01, false));
  assert(machine.writeGraphicsPixel(0, 8, 0x02, false));
  PcI8086::setAH(0x06); // scroll up
  PcI8086::setAL(0x01); // one text row
  PcI8086::setBH(0x00);
  PcI8086::setCH(0x00);
  PcI8086::setCL(0x00);
  PcI8086::setDH(0x01);
  PcI8086::setDL(0x00);
  PcI8086::triggerInterrupt(0x10);
  assert(machine.readGraphicsPixel(0, 0, &pixelColor));
  assert(pixelColor == 0x02);
  assert(machine.readGraphicsPixel(0, 8, &pixelColor));
  assert(pixelColor == 0x00);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x31);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0x31);

  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x30); // attribute mode-control index
  machine.writePort(0x03c0, 0x01); // clear 8-bit color enable: low nibble goes through attr palette
  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x21); // palette register 1
  machine.writePort(0x03c0, 0x02);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[1] == 0x001f);
  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c8, 0x03);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 0, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0x02);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 2, 0x03);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x33); // horizontal PEL panning
  machine.writePort(0x03c0, 0x04); // 256-color panning units: 4 -> 2 pixels
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x07e0);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x33);
  machine.writePort(0x03c0, 0x00);
  assert(machine.graphicsWidth() == PcMachine::VgaGraphics256Width);
  assert(machine.graphicsHeight() == PcMachine::VgaGraphics256Height);
  pixelColor = 0;
  assert(machine.writeGraphicsPixel(6, 7, 0x01, false));
  assert(machine.readGraphicsPixel(6, 7, &pixelColor));
  assert(pixelColor == 0x01);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x00); // Attribute Controller PAS clear: display disabled
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x0000);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x20); // PAS set: display enabled again
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  machine.writePort(0x03c4, 0x01);
  machine.writePort(0x03c5, 0x20); // sequencer clocking mode bit 5: screen off
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x0000);
  machine.writePort(0x03c5, 0x00);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  machine.writePort(0x03c4, 0x00);
  machine.writePort(0x03c5, 0x00); // sequencer reset asserted: display output blanked
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x0000);
  machine.writePort(0x03c5, 0x03); // async/sync reset released
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  machine.writePort(0x03c5, 0x20);
  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x01);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  machine.writePort(0x03c3, 0x00); // video subsystem disabled: blank output
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x0000);
  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x01);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  machine.writePort(0x03d4, 0x17);
  machine.writePort(0x03d5, 0x00); // CRTC mode-control reset bit clear: display logic disabled
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x0000);
  machine.writePort(0x03d5, 0x80);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);

  machine.writePort(0x03c8, 0x11);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  assert(machine.readPort(0x03c7) == 0x00);
  machine.writePort(0x03c7, 0x11);
  assert(machine.readPort(0x03c7) == 0x03);
  assert(machine.readPort(0x03c9) == 0x00);
  assert(machine.readPort(0x03c9) == 0x3f);
  assert(machine.readPort(0x03c9) == 0x00);
  assert(machine.readPort(0x03c7) == 0x03);
  machine.writePort(0x03c8, 0xff);
  machine.writePort(0x03c9, 0x01);
  machine.writePort(0x03c9, 0x02);
  machine.writePort(0x03c9, 0x03);
  assert(machine.readPort(0x03c8) == 0x00); // DAC index wraps after entry 255
  machine.writePort(0x03c7, 0xff);
  assert(machine.readPort(0x03c9) == 0x01);
  assert(machine.readPort(0x03c9) == 0x02);
  assert(machine.readPort(0x03c9) == 0x03);
  machine.writePort(0x03c8, 0xff);
  machine.writePort(0x03c9, 0x04);
  machine.writePort(0x03c9, 0x05);
  machine.writePort(0x03c9, 0x06);
  machine.writePort(0x03c9, 0x07); // fourth component wraps to DAC entry 0 red
  machine.writePort(0x03c7, 0xff);
  assert(machine.readPort(0x03c9) == 0x04);
  assert(machine.readPort(0x03c9) == 0x05);
  assert(machine.readPort(0x03c9) == 0x06);
  assert(machine.readPort(0x03c9) == 0x07);
  machine.writePort(0x03c8, 0x12);
  machine.writePort(0x03c9, 0xff);
  machine.writePort(0x03c9, 0x7f);
  machine.writePort(0x03c9, 0x40);
  machine.writePort(0x03c7, 0x12);
  assert(machine.readPort(0x03c9) == 0x3f);
  assert(machine.readPort(0x03c9) == 0x3f);
  assert(machine.readPort(0x03c9) == 0x00);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x12);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xffe0);
  machine.writePort(0x03c6, 0x0f); // mask 0x11 pixels down to DAC entry 0x01
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x11);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  machine.writePort(0x03c6, 0xff);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c6, 0x0f);
  assert(machine.readPort(0x03c6) == 0x0f);
  assert(machine.readPort(0x03c6) == 0x0f);
  assert(machine.readPort(0x03c6) == 0x0f);
  assert(machine.readPort(0x03c6) == 0x0f);
  machine.writePort(0x03c6, 0x8e); // unlocked DAC command register; must not overwrite PEL mask
  assert(machine.vgaDacState().command == 0x8e);
  assert(machine.vgaDacState().pelMask == 0x0f);
  assert(machine.readPort(0x03c6) == 0x0f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x11);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  assert(machine.readPort(0x03c6) == 0x0f);
  assert(machine.readPort(0x03c6) == 0x0f);
  assert(machine.readPort(0x03c6) == 0x0f);
  assert(machine.readPort(0x03c6) == 0x8e); // fifth read after unlock returns hidden DAC command
  assert(machine.readPort(0x03c6) == 0x0f); // command read closes the hidden-register window again

  assert(machine.readPort(0x03c6) == 0x0f);
  assert(machine.readPort(0x03c6) == 0x0f);
  assert(machine.readPort(0x03c6) == 0x0f);
  assert(machine.readPort(0x03c7) == 0x00); // other DAC ports reset the 03C6h unlock count
  machine.writePort(0x03c6, 0x3c);
  assert(machine.vgaDacState().pelMask == 0x3c);
  assert(machine.vgaDacState().command == 0x8e);

  assert(machine.readPort(0x03c6) == 0x3c);
  assert(machine.readPort(0x03c6) == 0x3c);
  assert(machine.readPort(0x03c6) == 0x3c);
  assert(machine.readPort(0x03c8) == machine.vgaDacState().writeIndex);
  machine.writePort(0x03c6, 0x2d);
  assert(machine.vgaDacState().pelMask == 0x2d);
  assert(machine.vgaDacState().command == 0x8e);

  assert(machine.readPort(0x03c6) == 0x2d);
  assert(machine.readPort(0x03c6) == 0x2d);
  assert(machine.readPort(0x03c6) == 0x2d);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c6, 0x1e);
  assert(machine.vgaDacState().pelMask == 0x1e);
  assert(machine.vgaDacState().command == 0x8e);
  machine.writePort(0x03c6, 0xff);

  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c6, 0x0f);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x21);
  machine.writePort(0x03c0, 0x31);
  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c7, 0x01);
  assert(machine.readPort(0x03c9) == 0x00);
  assert(machine.readPort(0x03c9) == 0x00);
  assert(machine.readPort(0x03c9) == 0x2a); // default EGA blue restored by mode set
  machine.writePort(0x03c7, 0x10);
  assert(machine.readPort(0x03c9) == 0x00);
  assert(machine.readPort(0x03c9) == 0x00);
  assert(machine.readPort(0x03c9) == 0x00); // standard VGA mode-13h grayscale ramp starts black
  machine.writePort(0x03c7, 0x1f);
  assert(machine.readPort(0x03c9) == 0x3f);
  assert(machine.readPort(0x03c9) == 0x3f);
  assert(machine.readPort(0x03c9) == 0x3f); // grayscale ramp ends white
  machine.writePort(0x03c7, 0x20);
  assert(machine.readPort(0x03c9) == 0x00);
  assert(machine.readPort(0x03c9) == 0x00);
  assert(machine.readPort(0x03c9) == 0x3f); // standard VGA hue cycle starts at blue
  machine.writePort(0x03c7, 0xff);
  assert(machine.readPort(0x03c9) == 0x00);
  assert(machine.readPort(0x03c9) == 0x00);
  assert(machine.readPort(0x03c9) == 0x00); // trailing compatibility entry is black
  assert(machine.readPort(0x03c6) == 0xff);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x21);
  assert(machine.readPort(0x03c1) == 0x01);

  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 4, 0x02);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x01);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x001f);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0x02);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x33);
  machine.writePort(0x03c0, 0x02); // 256-color horizontal PEL panning: raw 2 -> source x+1
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x001f);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0x02);
  machine.writePort(0x03d4, 0x08);
  machine.writePort(0x03d5, 0x20); // byte panning: source byte +1 appears at dest x
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x001f);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 320, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 640, 0x02);
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800); // default mode-13h CRTC Offset 40 -> 320-byte stride
  machine.writePort(0x03d4, 0x13);
  machine.writePort(0x03d5, 0x50); // virtual width: 80 * 8 = 640 bytes per displayed scan line
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(graphicsLine[0] == 0x001f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 160, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 320, 0x02);
  machine.writePort(0x03d4, 0x13);
  machine.writePort(0x03d5, 0x28);
  machine.writePort(0x03d4, 0x14);
  machine.writePort(0x03d5, 0x00); // word-addressed CRTC offset: 40 * 4 = 160 linear bytes in chain-4
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 80, 0x02);
  machine.writePort(0x03d4, 0x17);
  machine.writePort(0x03d5, 0xc0); // byte-addressed CRTC offset: 40 * 2 = 80 linear bytes in chain-4
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(graphicsLine[0] == 0x001f);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + PcMachine::VgaGraphics256Width, 0x02);
  machine.writePort(0x03d4, 0x09);
  machine.writePort(0x03d5, 0x41); // max scan line 1: each source row is rendered twice
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  machine.renderGraphicsLine(2, graphicsLine.data());
  assert(graphicsLine[0] == 0x001f);
  machine.writePort(0x03d4, 0x08);
  machine.writePort(0x03d5, 0x01); // preset row scan fine-scrolls by one repeated scan line
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(graphicsLine[0] == 0x001f);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + PcMachine::VgaGraphics256Width, 0x02);
  machine.writePort(0x03d4, 0x09);
  machine.writePort(0x03d5, 0xc0); // double-scan bit repeats source rows without changing memory
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  machine.renderGraphicsLine(2, graphicsLine.data());
  assert(graphicsLine[0] == 0x001f);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 10 * PcMachine::VgaGraphics256Width, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 11 * PcMachine::VgaGraphics256Width, 0x01);
  machine.writePort(0x03d4, 0x07);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x12);
  machine.writePort(0x03d5, 0x0a); // vertical display end: line 10 is last visible line
  machine.renderGraphicsLine(10, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  machine.renderGraphicsLine(11, graphicsLine.data());
  assert(graphicsLine[0] == 0x0000);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x31);
  machine.writePort(0x03c0, 0x02); // overscan/border color uses DAC entry 2
  machine.renderGraphicsLine(11, graphicsLine.data());
  assert(graphicsLine[0] == 0x001f);
  machine.writePort(0x03c6, 0x01); // PEL mask affects overscan DAC lookup too
  machine.renderGraphicsLine(11, graphicsLine.data());
  assert(graphicsLine[0] == 0x0000);
  machine.writePort(0x03c6, 0xff);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c8, 0x03);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 0, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0x02);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 2, 0x03);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 2) == 0x03);
  machine.writePort(0x03c4, 0x04);
  machine.writePort(0x03c5, 0x06); // unchain: chain-4 writes remain visible as planar pixels
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  assert(graphicsLine[1] == 0x001f);
  assert(graphicsLine[2] == 0xffe0);
  machine.writePort(0x03c6, 0x01); // PEL mask also applies to unchained/Mode-X 256-color output
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  assert(graphicsLine[1] == 0x0000);
  assert(graphicsLine[2] == 0xf800);
  machine.writePort(0x03c6, 0xff);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 4, 0x02);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x01);
  machine.writePort(0x03d4, 0x07);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x09);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x18);
  machine.writePort(0x03d5, 0x01); // split: line 1 restarts display address at zero
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x001f);
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + PcMachine::VgaGraphics256Width + 4, 0x02);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x01);
  machine.writePort(0x03d4, 0x18);
  machine.writePort(0x03d5, 0xff);
  machine.writePort(0x03d4, 0x07);
  machine.writePort(0x03d5, 0x10);
  machine.writePort(0x03d4, 0x09);
  machine.writePort(0x03d5, 0x40); // line compare 0x3ff: split disabled for 200-line mode
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(graphicsLine[0] == 0x001f);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 4, 0x02);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x01);
  machine.writePort(0x03d4, 0x09);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x18);
  machine.writePort(0x03d5, 0x01);
  machine.writePort(0x03d4, 0x11);
  machine.writePort(0x03d5, 0x80); // protect CRTC registers 00h-07h
  machine.writePort(0x03d4, 0x07);
  machine.writePort(0x03d5, 0x00); // protected, but overflow line-compare bit 8 remains writable
  assert(machine.readPort(0x03d5) == 0x00);
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  machine.writePort(0x03d4, 0x11);
  machine.writePort(0x03d5, 0x00); // unlock
  machine.writePort(0x03d4, 0x07);
  machine.writePort(0x03d5, 0x10);
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(graphicsLine[0] != 0xf800);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x22);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c4, 0x04);
  machine.writePort(0x03c5, 0x06); // Mode X-style unchained planar memory
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x04); // plane 2 only
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x22);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x0000);
  assert(graphicsLine[2] == 0x07e0);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x02); // plane 1 only
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 4, 0x22);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x01); // mode-13h/Mode-X dword start: raw 1 -> byte offset 4
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x0000);
  assert(graphicsLine[1] == 0x07e0);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x00);
  assert(machine.writeGraphicsPixel(3, 0, 0x22, false));
  assert(machine.readGraphicsPixel(3, 0, &pixelColor));
  assert(pixelColor == 0x22);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[3] == 0x07e0);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x22);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c4, 0x04);
  machine.writePort(0x03c5, 0x06); // unchained 256-color planar memory
  machine.writePort(0x03d4, 0x13);
  machine.writePort(0x03d5, 0x14); // 80 bytes per scan line when doubleword addressing is active
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x01); // plane 0, first pixel of scanline 1
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 80, 0x22);
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(graphicsLine[0] == 0x07e0);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x22);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c4, 0x04);
  machine.writePort(0x03c5, 0x06); // Mode X-style unchained planar memory
  machine.writePort(0x03d4, 0x07);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x12);
  machine.writePort(0x03d5, 0xef); // vertical display end 239: 320x240 Mode X-style frame
  assert(machine.graphicsHeight() == 240);
  assert(machine.writeGraphicsPixel(0, 239, 0x22, false));
  assert(machine.readGraphicsPixel(0, 239, &pixelColor));
  assert(pixelColor == 0x22);
  machine.renderGraphicsLine(239, graphicsLine.data());
  assert(graphicsLine[0] == 0x07e0);
  machine.renderGraphicsLine(240, graphicsLine.data());
  assert(graphicsLine[0] == 0x0000);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x01); // write plane 0 only
  machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase); // prime VGA latch
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x80);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] != 0x0000);
  assert(graphicsLine[1] == 0x0000);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x32); // color plane enable
  machine.writePort(0x03c0, 0x00); // mask out all attribute color bits
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x0000);
  assert(machine.readGraphicsPixel(0, 0, &pixelColor));
  assert(pixelColor == 0x01); // BIOS pixel reads return memory color, not AC-masked display color
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x32);
  machine.writePort(0x03c0, 0x0f);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] != 0x0000);
  assert(machine.graphicsWidth() == PcMachine::VgaGraphics16Width);
  assert(machine.graphicsHeight() == PcMachine::VgaGraphics16Height);
  assert(machine.writeGraphicsPixel(9, 2, 0x0e, false));
  assert(machine.readGraphicsPixel(9, 2, &pixelColor));
  assert(pixelColor == 0x0e);
  assert(machine.writeGraphicsPixel(9, 2, 0x02, true));
  assert(machine.readGraphicsPixel(9, 2, &pixelColor));
  assert(pixelColor == 0x0c);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x03);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c8, 0x04);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0xc9); // pairs 3,0,2,1
  machine.writePort(0x03c5, 0x02);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x4e); // pairs 1,0,3,2
  machine.writePort(0x03c5, 0x04);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x10); // adds high pair 1 to pixel 1
  machine.writePort(0x03c5, 0x08);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x00);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x0f);
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x20); // shift register interleave display mode
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x001f); // plane 0 pair -> color 3
  assert(graphicsLine[1] == 0xffe0); // planes 0/2 pair -> color 4
  assert(graphicsLine[2] == 0x07e0);
  assert(graphicsLine[3] == 0xf800);
  assert(graphicsLine[4] == 0xf800); // plane 1 pair path
  assert(graphicsLine[5] == 0x0000);
  assert(graphicsLine[6] == 0x001f);
  assert(graphicsLine[7] == 0x07e0);
  assert(machine.readGraphicsPixel(0, 0, &pixelColor));
  assert(pixelColor == 0x03);
  assert(machine.readGraphicsPixel(1, 0, &pixelColor));
  assert(pixelColor == 0x04);
  assert(machine.readGraphicsPixel(4, 0, &pixelColor));
  assert(pixelColor == 0x01);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x0f);
  assert(machine.writeGraphicsPixel(1, 0, 0x04, false));
  assert(machine.readGraphicsPixel(1, 0, &pixelColor));
  assert(pixelColor == 0x04);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[1] == 0xffe0);
  assert(machine.writeGraphicsPixel(1, 0, 0x01, true));
  assert(machine.readGraphicsPixel(1, 0, &pixelColor));
  assert(pixelColor == 0x05);
  assert(machine.writeGraphicsPixel(1, 0, 0x04, false));
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x32);
  machine.writePort(0x03c0, 0x03); // color-plane enable masks interleaved color 4 to 0
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x001f);
  assert(graphicsLine[1] == 0x0000);
  assert(machine.readGraphicsPixel(1, 0, &pixelColor));
  assert(pixelColor == 0x04); // interleaved readback remains the raw planar color
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x32);
  machine.writePort(0x03c0, 0x0f);
  assert((machine.readPort(0x03da) & 0x30) == 0x10); // status mux samples interleaved color 3
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x00);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  assert(machine.writeGraphicsPixel(0, 0, 0x01, false));
  assert(machine.writeGraphicsPixel(0, 1, 0x02, false));
  machine.writePort(0x03d4, 0x09);
  machine.writePort(0x03d5, 0x41); // planar max scan line 1 repeats the same memory row twice
  machine.renderGraphicsLine(0, graphicsLine.data());
  uint16_t const planarRepeatedColor = graphicsLine[0];
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(graphicsLine[0] == planarRepeatedColor);
  machine.renderGraphicsLine(2, graphicsLine.data());
  assert(graphicsLine[0] != planarRepeatedColor);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x16);
  assert(machine.graphicsWidth() == PcMachine::VgaGraphics16LowWidth);
  assert(machine.graphicsHeight() == PcMachine::VgaGraphics16LowHeight);
  assert(machine.writeGraphicsPixel(319, 199, 0x0e, false));
  assert(machine.readGraphicsPixel(319, 199, &pixelColor));
  assert(pixelColor == 0x0e);
  machine.renderGraphicsLine(199, graphicsLine.data());
  assert(graphicsLine[319] != 0x0000);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x200x16);
  assert(machine.graphicsWidth() == PcMachine::VgaGraphics16Width);
  assert(machine.graphicsHeight() == PcMachine::VgaGraphics16LowHeight);
  assert(machine.writeGraphicsPixel(639, 199, 0x0b, false));
  assert(machine.readGraphicsPixel(639, 199, &pixelColor));
  assert(pixelColor == 0x0b);
  machine.renderGraphicsLine(199, graphicsLine.data());
  assert(graphicsLine[639] != 0x0000);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x350x2);
  assert(machine.graphicsWidth() == PcMachine::VgaGraphics16Width);
  assert(machine.graphicsHeight() == PcMachine::VgaGraphics16MediumHeight);
  assert(machine.writeGraphicsPixel(639, 349, 0x01, false));
  assert(machine.readGraphicsPixel(639, 349, &pixelColor));
  assert(pixelColor == 0x01);
  machine.renderGraphicsLine(349, graphicsLine.data());
  assert(graphicsLine[639] == 0xffff);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x350x16);
  assert(machine.graphicsWidth() == PcMachine::VgaGraphics16Width);
  assert(machine.graphicsHeight() == PcMachine::VgaGraphics16MediumHeight);
  assert(machine.writeGraphicsPixel(639, 349, 0x0d, false));
  assert(machine.readGraphicsPixel(639, 349, &pixelColor));
  assert(pixelColor == 0x0d);
  machine.renderGraphicsLine(349, graphicsLine.data());
  assert(graphicsLine[639] != 0x0000);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x2);
  assert(machine.graphicsWidth() == PcMachine::VgaGraphics16Width);
  assert(machine.graphicsHeight() == PcMachine::VgaGraphics16Height);
  assert(machine.writeGraphicsPixel(639, 479, 0x01, false));
  assert(machine.readGraphicsPixel(639, 479, &pixelColor));
  assert(pixelColor == 0x01);
  machine.renderGraphicsLine(479, graphicsLine.data());
  assert(graphicsLine[639] == 0xffff);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writeGraphicsPixel(1, 0, 0x0e, false);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x33);
  machine.writePort(0x03c0, 0x01);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] != 0x0000);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x33);
  machine.writePort(0x03c0, 0x00);

  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0x80);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x01);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] != 0x0000);
  assert(graphicsLine[1] == 0x0000);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x00);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0x80);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x01);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] != 0x0000); // default planar modes use byte-addressed CRTC start
  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 2, 0x80);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x01);
  machine.writePort(0x03d4, 0x17);
  machine.writePort(0x03d5, 0x80); // word-addressed CRTC start: raw 1 -> byte offset 2
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] != 0x0000);
  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 4, 0x80);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x01);
  machine.writePort(0x03d4, 0x14);
  machine.writePort(0x03d5, 0x40); // dword-addressed CRTC start: raw 1 -> byte offset 4
  machine.writePort(0x03d4, 0x17);
  machine.writePort(0x03d5, 0x80);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] != 0x0000);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 4, 0x80);
  machine.writePort(0x03d4, 0x13);
  machine.writePort(0x03d5, 0x01); // offset register: one dword per logical scan line
  machine.writePort(0x03d4, 0x14);
  machine.writePort(0x03d5, 0x40);
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(graphicsLine[0] != 0x0000);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x01); // plane 0 at start address 1
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0x80);
  machine.writePort(0x03c5, 0x02); // plane 1 at address zero for split area
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x80);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x01);
  machine.writePort(0x03d4, 0x07);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x09);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x18);
  machine.writePort(0x03d5, 0x01);
  machine.renderGraphicsLine(0, graphicsLine.data());
  uint16_t const beforeSplitColor = graphicsLine[0];
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(beforeSplitColor != 0x0000);
  assert(graphicsLine[0] != 0x0000);
  assert(graphicsLine[0] != beforeSplitColor);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x01); // plane 0
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0x80); // normal area with byte panning
  machine.writePort(0x03c5, 0x02); // plane 1
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 0, 0x80); // split area should ignore byte panning
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x08);
  machine.writePort(0x03d5, 0x20); // byte panning by one byte before split
  machine.writePort(0x03d4, 0x07);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x09);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x18);
  machine.writePort(0x03d5, 0x01); // split line starts at display line 1
  machine.renderGraphicsLine(0, graphicsLine.data());
  uint16_t const bytePannedBeforeSplit = graphicsLine[0];
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(bytePannedBeforeSplit != 0x0000);
  assert(graphicsLine[0] != 0x0000);
  assert(graphicsLine[0] == bytePannedBeforeSplit); // AC bit 5 clear: split line still honors byte panning.
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x30);
  machine.writePort(0x03c0, 0x21); // AC bit 5 set: split line forces pixel/byte panning to zero.
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(graphicsLine[0] != 0x0000);
  assert(graphicsLine[0] != bytePannedBeforeSplit);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writeGraphicsPixel(0, 0, 0x01, false);
  machine.writePort(0x03c8, 0x21);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x30);
  machine.writePort(0x03c0, 0x89); // 16 pages of 16 colors: Color Select supplies DAC bits 4-5
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x34);
  machine.writePort(0x03c0, 0x02); // page 2: attr 1 maps to DAC 0x21
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x07e0);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writeGraphicsPixel(0, 0, 0x01, false);
  machine.writePort(0x03c8, 0x81);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x30);
  machine.writePort(0x03c0, 0x09); // 4 pages of 64 colors: palette bits 4-5 remain active
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x34);
  machine.writePort(0x03c0, 0x08); // page 2: color-select bits 2-3 become DAC bits 6-7
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c8, 0x81);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03d4, 0x07);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x12);
  machine.writePort(0x03d5, 0x00); // vertical display end: only line 0 visible
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x31);
  machine.writePort(0x03c0, 0x01); // overscan low six bits select DAC 1
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x34);
  machine.writePort(0x03c0, 0x08); // Color Select bits 2-3 supply DAC bits 6-7 outside 256-color mode
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c8, 0x21);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03d4, 0x07);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x12);
  machine.writePort(0x03d5, 0x00); // vertical display end: only line 0 visible
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x30);
  machine.writePort(0x03c0, 0x81); // Color Select bits 0-1 replace overscan DAC bits 4-5 too
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x31);
  machine.writePort(0x03c0, 0x01);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x34);
  machine.writePort(0x03c0, 0x02);
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(graphicsLine[0] == 0x07e0);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x01);
  machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x80);

  machine.writePort(0x03ce, 0x00);
  machine.writePort(0x03cf, 0x0e); // set/reset is independent from read-mode-1 color compare
  machine.writePort(0x03ce, 0x02);
  machine.writePort(0x03cf, 0x01); // color compare = plane-0 pixel
  machine.writePort(0x03ce, 0x07);
  machine.writePort(0x03cf, 0x0f); // compare all planes
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x08); // read mode 1
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0x80);
  machine.writePort(0x03ce, 0x02);
  machine.writePort(0x03cf, 0x00); // compare with background
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0x7f);
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x00);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x00);
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0xff);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x12);
  machine.writePort(0x03c5, 0x02);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x34);
  machine.writePort(0x03c5, 0x04);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x56);
  machine.writePort(0x03c5, 0x08);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x78);
  machine.writePort(0x03c5, 0x0f);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0x12); // latch = 12/34/56/78
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x01); // write mode 1: copy latches to destination
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0x00); // ignored by write mode 1
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0xff); // CPU data ignored
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x00);
  machine.writePort(0x03ce, 0x04);
  machine.writePort(0x03cf, 0x00);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1) == 0x12);
  machine.writePort(0x03cf, 0x01);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1) == 0x34);
  machine.writePort(0x03cf, 0x02);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1) == 0x56);
  machine.writePort(0x03cf, 0x03);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1) == 0x78);
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0xff);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c8, 0x0b);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase); // latch starts from cleared destination
  machine.writePort(0x03ce, 0x00);
  machine.writePort(0x03cf, 0x0b); // set/reset color: planes 0,1,3
  machine.writePort(0x03ce, 0x01);
  machine.writePort(0x03cf, 0x0f); // enable set/reset for all planes
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0x80); // affect only the first pixel in the byte
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x00); // write mode 0
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x00); // CPU byte ignored by enabled set/reset
  assert(machine.readGraphicsPixel(0, 0, &pixelColor));
  assert(pixelColor == 0x0b);
  assert(machine.readGraphicsPixel(1, 0, &pixelColor));
  assert(pixelColor == 0x00);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  assert(graphicsLine[1] == 0x0000);
  machine.writePort(0x03ce, 0x01);
  machine.writePort(0x03cf, 0x00);
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0xff);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x01); // plane 0 only
  machine.writePort(0x03ce, 0x04);
  machine.writePort(0x03cf, 0x00); // read plane 0
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0xff);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0xaa);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0xaa); // latch = 0xaa
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x00); // memory changes; latch remains 0xaa
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0x0f); // unmasked high nibble must come from latch, not current memory
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x55);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0xa5);
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0xff);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x01); // plane 0 only
  machine.writePort(0x03ce, 0x04);
  machine.writePort(0x03cf, 0x00); // read plane 0
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0xff);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x0f);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0x0f); // latch = 0x0f
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x02); // write mode 2: CPU bit 0 expands to plane 0 byte
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0xf0);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x01);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0xff);

  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x00);
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0xff);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0xaa);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0xaa); // latch = 0xaa
  machine.writePort(0x03ce, 0x03);
  machine.writePort(0x03cf, 0x18); // logical op XOR
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x02); // write mode 2
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x01);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0x55);

  machine.writePort(0x03ce, 0x03);
  machine.writePort(0x03cf, 0x00);
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x00);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0xaa);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0xaa); // latch = 0xaa
  machine.writePort(0x03ce, 0x00);
  machine.writePort(0x03cf, 0x01); // set/reset plane 0 = 1
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0x0f);
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x03); // write mode 3: rotated CPU byte AND bit mask selects pixels
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0xff);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0xaf);

  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x00);
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0xff);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0xaa);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0xaa); // latch = 0xaa
  machine.writePort(0x03ce, 0x00);
  machine.writePort(0x03cf, 0x00); // set/reset plane 0 = 0
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0x0f);
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x03); // write mode 3
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0xff);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0xa0);

  machine.writePort(0x03ce, 0x00);
  machine.writePort(0x03cf, 0x00);
  machine.writePort(0x03ce, 0x03);
  machine.writePort(0x03cf, 0x00);
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x00);
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0xff);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c4, 0x04);
  machine.writePort(0x03c5, 0x02); // enable VGA odd/even host writes
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x0f);
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x10); // enable odd/even host reads
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 0, 0x12);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0x34);
  machine.writePort(0x03ce, 0x04);
  machine.writePort(0x03cf, 0x00);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 0) == 0x12);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1) == 0x34);
  machine.writePort(0x03ce, 0x04);
  machine.writePort(0x03cf, 0x02);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 0) == 0x12);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1) == 0x34);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x01);
  machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x80);
  machine.writePort(0x03c8, 0x31);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x30);
  machine.writePort(0x03c0, 0x81); // attribute color-select supplies DAC bits 4-5
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x34);
  machine.writePort(0x03c0, 0x03);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x21);
  machine.writePort(0x03c0, 0x01);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  machine.writePort(0x03ce, 0x06);
  machine.writePort(0x03cf, 0x0d); // graphics mode, CPU aperture at B8000-BFFFF
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x01);
  machine.readVideoMemory8(PcMachine::TextColorMemoryBase); // prime latch through mapped B800 window
  machine.writeVideoMemory8(PcMachine::TextColorMemoryBase, 0x80);
  assert(machine.readVideoMemory8(PcMachine::TextColorMemoryBase) == 0x80);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0xff);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0xff); // A000 is outside the selected B800 aperture
  assert(machine.readVideoMemory8(PcMachine::TextColorMemoryBase) == 0x80);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] != 0x0000);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x05);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03ce, 0x06);
  machine.writePort(0x03cf, 0x0d); // mode 13h chain-4 CPU aperture at B8000-BFFFF
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x05);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0xff);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x0000);
  machine.writeVideoMemory8(PcMachine::TextColorMemoryBase, 0x05);
  assert(machine.readVideoMemory8(PcMachine::TextColorMemoryBase) == 0x05);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xffe0);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x04);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x04); // chain-4 writes only plane 2
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 0, 0x04);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 2, 0x04);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 0) == 0x00);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 2) == 0x04);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x0000);
  assert(graphicsLine[2] == 0xf81f);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x11);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x51);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c8, 0x05);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 0, 0x05);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0x05);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 2, 0x51);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x32);
  machine.writePort(0x03c0, 0x01); // 256-color output remains a direct byte-sized DAC index
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  assert(graphicsLine[1] == 0xf800);
  assert(graphicsLine[2] == 0x001f);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x32);
  machine.writePort(0x03c0, 0x02);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  assert(graphicsLine[1] == 0xf800);
  assert(graphicsLine[2] == 0x001f);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x05);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x25); // AC palette register 5 is bypassed while 8-bit PEL is enabled
  machine.writePort(0x03c0, 0x02);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x05);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);

  machine.writePort(0x03c8, 0x51);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x25); // high source nibble 5 is not AC-remapped in 8-bit PEL mode
  machine.writePort(0x03c0, 0x0a);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x21); // low source nibble 1 is not AC-remapped in 8-bit PEL mode
  machine.writePort(0x03c0, 0x03);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0x51);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[1] == 0x07e0);
  machine.writePort(0x03c6, 0x0f); // PEL mask applies after direct DAC indexing: 51h -> 01h
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[1] == 0xf800);
  machine.writePort(0x03c6, 0xff);
  machine.writePort(0x03c8, 0x03);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x30);
  machine.writePort(0x03c0, 0x01); // disable 8-bit PEL: pixel 51h uses only low nibble through attr 1
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[1] == 0xf800);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x05);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0xc5);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x05);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x34);
  machine.writePort(0x03c0, 0x0c); // Color Select bits 2-3 must not affect 256-color DAC index
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x04);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x05);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c4, 0x04);
  machine.writePort(0x03c5, 0x06); // Mode X-style unchained planar memory
  machine.writePort(0x03c4, 0x02);
  machine.writePort(0x03c5, 0x04); // plane/x%4==2
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x05);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x32);
  machine.writePort(0x03c0, 0x01);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[2] == 0xf800);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x32);
  machine.writePort(0x03c0, 0x04);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[2] == 0xf800);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0xaa);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0xaa); // prime chain-4 latch
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0x0f); // chain-4 write mode 0 must preserve unmasked latch bits
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x55);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0xa5);
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0xff);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x02); // write mode 2 expands CPU bit for the addressed chain-4 plane
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0xf0);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0x02);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1) == 0xf0);
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x00);
  machine.writePort(0x03ce, 0x08);
  machine.writePort(0x03cf, 0xff);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 0, 0xff); // plane 0, byte 0
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 1, 0x00); // plane 1, byte 0
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 2, 0xff); // plane 2, byte 0
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 3, 0x00); // plane 3, byte 0
  machine.writePort(0x03ce, 0x00);
  machine.writePort(0x03cf, 0x0a); // set/reset is independent from read-mode-1 color compare
  machine.writePort(0x03ce, 0x02);
  machine.writePort(0x03cf, 0x05); // color compare = planes 0 and 2
  machine.writePort(0x03ce, 0x07);
  machine.writePort(0x03cf, 0x0f); // compare all planes
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x08); // read mode 1
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0xff);
  machine.writePort(0x03ce, 0x02);
  machine.writePort(0x03cf, 0x01); // mismatch plane 2
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0x00);
  machine.writePort(0x03ce, 0x05);
  machine.writePort(0x03cf, 0x00);

  machine.setVideoMode(PcMachine::VideoMode::CgaGraphics320x200x4);
  machine.writeVideoMemory8(PcMachine::TextColorMemoryBase, 0xc0); // first pixel color index 3
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] != 0x0000);
  machine.writeVideoMemory8(PcMachine::TextColorMemoryBase, 0x40); // first pixel color index 1
  machine.writePort(0x03d9, 0x00);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == pcMachineDefaultVgaDacRgb565(0x02)); // palette 0, low intensity: green
  machine.writePort(0x03d9, 0x10);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == pcMachineDefaultVgaDacRgb565(0x0a)); // palette 0, high intensity: bright green
  machine.writePort(0x03d9, 0x20);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == pcMachineDefaultVgaDacRgb565(0x03)); // palette 1, low intensity: cyan
  machine.writePort(0x03d9, 0x30);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == pcMachineDefaultVgaDacRgb565(0x0b)); // palette 1, high intensity: bright cyan
  machine.writePort(0x03d9, 0x00);
  writeDacEntry(machine, 0x02, 0x3f, 0x00, 0x00);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == pcMachineRgb565From6Bit(0x3f, 0x00, 0x00)); // CGA color 1 follows DAC[2].
  writeDacEntry(machine, 0x00, 0x00, 0x00, 0x3f);
  machine.writePort(0x03c6, 0x00);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == pcMachineRgb565From6Bit(0x00, 0x00, 0x3f)); // PEL mask applies in CGA too.
  machine.writePort(0x03c6, 0xff);
  writeDacEntry(machine, 0x00, 0x00, 0x00, 0x00);
  writeDacEntry(machine, 0x02, 0x00, 0x2a, 0x00);
  machine.writePort(0x03d8, 0x0e); // 320x200 BW/composite-suppressed CGA palette path.
  machine.writePort(0x03d9, 0x20); // palette-select bit is ignored by the mode-5 RGB palette.
  machine.writeVideoMemory8(PcMachine::TextColorMemoryBase, 0x6c); // color indexes 1,2,3,0
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == pcMachineDefaultVgaDacRgb565(0x03)); // low-intensity cyan
  assert(graphicsLine[1] == pcMachineDefaultVgaDacRgb565(0x04)); // low-intensity red
  assert(graphicsLine[2] == pcMachineDefaultVgaDacRgb565(0x07)); // low-intensity white/light gray
  machine.writePort(0x03d9, 0x30);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == pcMachineDefaultVgaDacRgb565(0x0b)); // high-intensity cyan
  assert(graphicsLine[1] == pcMachineDefaultVgaDacRgb565(0x0c)); // high-intensity red
  assert(graphicsLine[2] == pcMachineDefaultVgaDacRgb565(0x0f)); // high-intensity white
  machine.writePort(0x03d8, 0x0a);
  machine.writePort(0x03d9, 0x00);
  machine.writeVideoMemory8(PcMachine::TextColorMemoryBase, 0x00);
  PcI8086::setAX(0x0b00);
  PcI8086::setBX(0x0004);
  PcI8086::triggerInterrupt(0x10);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == pcMachineDefaultVgaDacRgb565(0x04)); // BIOS AH=0Bh/BH=0 changes CGA 320 background.
  PcI8086::setAX(0x0b00);
  PcI8086::setBX(0x0101);
  PcI8086::triggerInterrupt(0x10);
  machine.writeVideoMemory8(PcMachine::TextColorMemoryBase, 0x40);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == pcMachineDefaultVgaDacRgb565(0x03)); // BIOS AH=0Bh/BH=1 selects CGA 320 palette 1.
  machine.writePort(0x03d9, 0x00);
  assert((machine.readPort(0x03d8) & 0x08) != 0); // BIOS-style CGA mode defaults to visible output
  assert(machine.graphicsWidth() == PcMachine::CgaGraphics320Width);
  assert(machine.graphicsHeight() == PcMachine::CgaGraphicsHeight);
  assert(machine.writeGraphicsPixel(4, 1, 0x02, false));
  assert(machine.readGraphicsPixel(4, 1, &pixelColor));
  assert(pixelColor == 0x02);
  assert(machine.writeGraphicsPixel(4, 1, 0x03, true));
  assert(machine.readGraphicsPixel(4, 1, &pixelColor));
  assert(pixelColor == 0x01);
  machine.writeVideoMemory8(PcMachine::TextColorMemoryBase, 0x00);
  machine.writeVideoMemory8(PcMachine::TextColorMemoryBase + 2, 0xc0);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x02); // CGA CRTC start offset selects byte 2 as first displayed byte
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] != 0x0000);

  machine.setVideoMode(PcMachine::VideoMode::CgaGraphics640x200x2);
  assert((machine.readPort(0x03d9) & 0x0f) == 0x0f);
  machine.setVideoMode(PcMachine::VideoMode::CgaGraphics320x200x4);
  assert(machine.readPort(0x03d9) == 0x00); // 320x200 mode resets stale 640x200 foreground/color-select state.
  machine.writeVideoMemory8(PcMachine::TextColorMemoryBase, 0x40);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == pcMachineDefaultVgaDacRgb565(0x02)); // default CGA palette 0 color 1 is green.

  machine.setVideoMode(PcMachine::VideoMode::CgaGraphics640x200x2);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x00);
  machine.writeVideoMemory8(PcMachine::TextColorMemoryBase, 0x80);
  assert((machine.readPort(0x03d9) & 0x0f) == 0x0f); // BIOS/default 640x200 foreground is white.
  writeDacEntry(machine, 0x0f, 0x00, 0x3f, 0x00);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == pcMachineRgb565From6Bit(0x00, 0x3f, 0x00)); // 640x200 foreground follows DAC[15].
  writeDacEntry(machine, 0x0f, 0x3f, 0x3f, 0x3f);
  writeDacEntry(machine, 0x00, 0x00, 0x00, 0x00);
  machine.writePort(0x03d9, 0x00);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == pcMachineRgb565From6Bit(0x00, 0x00, 0x00)); // explicit foreground 0 is black.
  machine.writePort(0x03d9, 0x04);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == pcMachineDefaultVgaDacRgb565(0x04)); // foreground lower nibble is programmable.
  PcI8086::setAX(0x0b00);
  PcI8086::setBX(0x0002);
  PcI8086::triggerInterrupt(0x10);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == pcMachineDefaultVgaDacRgb565(0x02)); // BIOS AH=0Bh/BH=0 changes CGA 640 foreground.
  machine.writeVideoMemory8(PcMachine::TextColorMemoryBase, 0x00);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == pcMachineRgb565From6Bit(0x00, 0x00, 0x00)); // 640 background remains black.
  machine.writeVideoMemory8(PcMachine::TextColorMemoryBase, 0x80);
  PcI8086::setAX(0x0b00);
  PcI8086::setBX(0x0101);
  PcI8086::triggerInterrupt(0x10);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == pcMachineDefaultVgaDacRgb565(0x02)); // BH=1 palette select does not affect CGA 640 foreground.
  machine.writePort(0x03d9, 0x0f);
  assert(machine.writeGraphicsPixel(9, 1, 0x01, false));
  assert(machine.readGraphicsPixel(9, 1, &pixelColor));
  assert(pixelColor == 0x01);
  assert(machine.writeGraphicsPixel(9, 1, 0x01, true));
  assert(machine.readGraphicsPixel(9, 1, &pixelColor));
  assert(pixelColor == 0x00);
  machine.writeVideoMemory8(PcMachine::TextColorMemoryBase, 0x00);
  machine.writeVideoMemory8(PcMachine::TextColorMemoryBase + 3, 0x80);
  machine.writePort(0x03d4, 0x0c);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x0d);
  machine.writePort(0x03d5, 0x03);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] != 0x0000);

  char path[] = "/tmp/tabdos-machine-disk-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);
  createImage(path, 1440 * 1024);
  assert(machine.openDisk(0, path));
  assert(machine.disk(0));
  assert(machine.disk(0)->geometry().cylinders == 80);

  uint8_t sector[PcDiskImage::SectorSize];
  uint8_t const boundInRangeProgram[] = {
    0xc7, 0x06, 0x00, 0x05, 0x03, 0x00, // mov word [0500h], 3
    0xc7, 0x06, 0x02, 0x05, 0x07, 0x00, // mov word [0502h], 7
    0xb8, 0x05, 0x00,                   // mov ax, 5
    0x62, 0x06, 0x00, 0x05,             // bound ax, [0500h]
    0xa3, 0x04, 0x05,                   // mov [0504h], ax
    0xf4                                // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, boundInRangeProgram, sizeof(boundInRangeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(20);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0504) == 0x0005);

  uint8_t const enterNestedFrameProgram[] = {
    0xbc, 0x00, 0x06,                   // mov sp, 0600h
    0xbd, 0x04, 0x05,                   // mov bp, 0504h
    0xc7, 0x06, 0x02, 0x05, 0x34, 0x12, // mov word [0502h], 1234h
    0xc8, 0x00, 0x00, 0x02,             // enter 0, 2: copy parent frame word
    0xa1, 0xfc, 0x05,                   // mov ax, [05fch]
    0xa3, 0x00, 0x05,                   // mov [0500h], ax
    0xf4                                // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, enterNestedFrameProgram, sizeof(enterNestedFrameProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(20);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x1234);

  uint8_t const trapFlagWrapProgram[] = {
    0xcf // iret to FFFE:9DCA with TF set
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, trapFlagWrapProgram, sizeof(trapFlagWrapProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.writeMemory16(0x0600, 0x9dca);
  machine.writeMemory16(0x0602, 0xfffe);
  machine.writeMemory16(0x0604, 0xf102); // TF set, 8086-reserved high flags set
  uint8_t const wrappedTarget[] = {
    0xc6, 0x06, 0x00, 0x05, 0x42, // mov byte [0500h], 42h: must execute before INT 01h
    0xf4                          // hlt after the guest trap handler returns
  };
  for (size_t i = 0; i < sizeof(wrappedTarget); ++i)
    machine.writeMemory8(0x09daa + i, wrappedTarget[i]);
  uint8_t const trapHandler[] = {
    0xa0, 0x00, 0x05,                   // mov al, [0500h]
    0xa2, 0x01, 0x05,                   // mov [0501h], al
    0x55,                               // push bp
    0x89, 0xe5,                         // mov bp, sp
    0x81, 0x66, 0x06, 0xff, 0xfe,       // and word [bp+6], 0feffh: clear saved TF
    0x5d,                               // pop bp
    0xcf                                // iret
  };
  for (size_t i = 0; i < sizeof(trapHandler); ++i)
    machine.writeMemory8(0x0700 + i, trapHandler[i]);
  machine.writeMemory16(0x0004, 0x0700); // guest INT 01h vector
  machine.writeMemory16(0x0006, 0x0000);
  PcI8086::setSS(0x0000);
  PcI8086::setSP(0x0600);
  machine.runCpuSteps(32);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0x42);
  assert(machine.readMemory8(0x0501) == 0x42); // handler observed target instruction first
  assert(PcI8086::CS() == 0xfffe && PcI8086::IP() == 0x9dcf);
  assert(!PcI8086::flagTF());
  assert(machine.diagnostics().unsupportedOpcodeCount == 0);

  constexpr uint8_t indirectNearJumpProgram[] = {
    0xb8, 0x10, 0x7c, // mov ax, 7c10h
    0xff, 0xe0,       // jmp ax: must preserve AX's high byte
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0xc7, 0x06, 0x00, 0x05, 0x34, 0x12, // mov word [0500h], 1234h
    0xf4                                // hlt
  };
  static_assert(indirectNearJumpProgram[16] == 0xc7);
  memset(sector, 0, sizeof(sector));
  memcpy(sector, indirectNearJumpProgram, sizeof(indirectNearJumpProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x1234);

  uint8_t const boundTrapProgram[] = {
    0xc7, 0x06, 0x14, 0x00, 0x30, 0x7c, // mov word [0014h], 7c30h
    0xc7, 0x06, 0x16, 0x00, 0x00, 0x00, // mov word [0016h], 0000h
    0xc7, 0x06, 0x00, 0x05, 0x03, 0x00, // mov word [0500h], 3
    0xc7, 0x06, 0x02, 0x05, 0x07, 0x00, // mov word [0502h], 7
    0xb8, 0x09, 0x00,                   // mov ax, 9
    0x62, 0x06, 0x00, 0x05,             // bound ax, [0500h]
    0xc7, 0x06, 0x06, 0x05, 0x11, 0x11, // mov word [0506h], 1111h
    0xf4,                               // hlt
    0x90, 0x90, 0x90, 0x90,             // pad to 7c30h
    0x90, 0x90, 0x90, 0x90,
    0x90, 0x90,
    0xc7, 0x06, 0x06, 0x05, 0x0d, 0xb0, // mov word [0506h], b00dh
    0xf4                                // hlt
  };
  static_assert(sizeof(boundTrapProgram) == 0x37, "BOUND trap handler must stay at 0000:7c30");
  memset(sector, 0, sizeof(sector));
  memcpy(sector, boundTrapProgram, sizeof(boundTrapProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(30);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0506) == 0xb00d);

  machine.reset();
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  PcI8086::setAX(0x0013); // INT 10h: mode 13h, required before DAC-heavy VGA programs
  PcI8086::triggerInterrupt(0x10);
  assert(machine.videoMode() == PcMachine::VideoMode::VgaGraphics320x200x256);
  PcI8086::setAX(0x1010); // set individual DAC register
  PcI8086::setBX(0x002a);
  PcI8086::setDH(10);
  PcI8086::setCH(20);
  PcI8086::setCL(30);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x1015); // read individual DAC register
  PcI8086::setBX(0x002a);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::DH() == 10);
  assert(PcI8086::CH() == 20);
  assert(PcI8086::CL() == 30);

  PcI8086::setAX(0x1010); // BIOS DAC writes still clamp 8-bit inputs to VGA's 6-bit components
  PcI8086::setBX(0x002b);
  PcI8086::setDH(0xff);
  PcI8086::setCH(0x7f);
  PcI8086::setCL(0x40);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x1015);
  PcI8086::setBX(0x002b);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::DH() == 0x3f);
  assert(PcI8086::CH() == 0x3f);
  assert(PcI8086::CL() == 0x00);

  machine.writeMemory8(0x0600, 0x00);
  machine.writeMemory8(0x0601, 0x3f);
  machine.writeMemory8(0x0602, 0x00);
  machine.writeMemory8(0x0603, 0x3f);
  machine.writeMemory8(0x0604, 0x00);
  machine.writeMemory8(0x0605, 0x00);
  PcI8086::setAX(0x1012); // set block of DAC registers
  PcI8086::setBX(0x0030);
  PcI8086::setCX(2);
  PcI8086::setES(0x0000);
  PcI8086::setDX(0x0600);
  PcI8086::triggerInterrupt(0x10);
  for (uint16_t i = 0; i < 6; ++i)
    machine.writeMemory8(0x0610 + i, 0xff);
  PcI8086::setAX(0x1017); // read block of DAC registers
  PcI8086::setBX(0x0030);
  PcI8086::setCX(2);
  PcI8086::setES(0x0000);
  PcI8086::setDX(0x0610);
  PcI8086::triggerInterrupt(0x10);
  assert(machine.readMemory8(0x0610) == 0x00);
  assert(machine.readMemory8(0x0611) == 0x3f);
  assert(machine.readMemory8(0x0612) == 0x00);
  assert(machine.readMemory8(0x0613) == 0x3f);
  assert(machine.readMemory8(0x0614) == 0x00);
  assert(machine.readMemory8(0x0615) == 0x00);

  machine.writeMemory8(0x0620, 0xff);
  machine.writeMemory8(0x0621, 0x7f);
  machine.writeMemory8(0x0622, 0x40);
  PcI8086::setAX(0x1012); // block DAC service also passes through 6-bit DAC hardware semantics
  PcI8086::setBX(0x0032);
  PcI8086::setCX(1);
  PcI8086::setES(0x0000);
  PcI8086::setDX(0x0620);
  PcI8086::triggerInterrupt(0x10);
  machine.writeMemory8(0x0623, 0xff);
  machine.writeMemory8(0x0624, 0xff);
  machine.writeMemory8(0x0625, 0xff);
  PcI8086::setAX(0x1017);
  PcI8086::setBX(0x0032);
  PcI8086::setCX(1);
  PcI8086::setES(0x0000);
  PcI8086::setDX(0x0623);
  PcI8086::triggerInterrupt(0x10);
  assert(machine.readMemory8(0x0623) == 0x3f);
  assert(machine.readMemory8(0x0624) == 0x3f);
  assert(machine.readMemory8(0x0625) == 0x00);

  uint32_t const largeDacWriteTable = 0x7000;
  uint32_t const largeDacReadTable = 0x18000;
  uint16_t const largeDacCount = 0x5556; // 65538 bytes, just beyond a uint16_t loop range.
  size_t const largeDacBytes = static_cast<size_t>(largeDacCount) * 3;
  for (size_t i = 0; i < largeDacBytes; ++i) {
    static uint8_t const triplet[3] = {0x11, 0x22, 0x33};
    machine.writeMemory8(largeDacWriteTable + static_cast<uint32_t>(i), triplet[i % 3]);
    machine.writeMemory8(largeDacReadTable + static_cast<uint32_t>(i), 0xff);
  }
  PcI8086::setAX(0x1012); // set a DAC block larger than 64 KiB of component data
  PcI8086::setBX(0x00fe);
  PcI8086::setCX(largeDacCount);
  PcI8086::setES(static_cast<uint16_t>(largeDacWriteTable >> 4));
  PcI8086::setDX(0x0000);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x1017); // read it back; DAC index wraps repeatedly but the transfer must finish
  PcI8086::setBX(0x00fe);
  PcI8086::setCX(largeDacCount);
  PcI8086::setES(static_cast<uint16_t>(largeDacReadTable >> 4));
  PcI8086::setDX(0x0000);
  PcI8086::triggerInterrupt(0x10);
  for (int i = 0; i < 12; ++i) {
    static uint8_t const triplet[3] = {0x11, 0x22, 0x33};
    assert(machine.readMemory8(largeDacReadTable + static_cast<uint32_t>(i)) == triplet[i % 3]);
  }
  assert(machine.readMemory8(largeDacReadTable + static_cast<uint32_t>(largeDacBytes - 3)) == 0x11);
  assert(machine.readMemory8(largeDacReadTable + static_cast<uint32_t>(largeDacBytes - 2)) == 0x22);
  assert(machine.readMemory8(largeDacReadTable + static_cast<uint32_t>(largeDacBytes - 1)) == 0x33);

  PcI8086::setAX(0x1018); // set PEL mask through BIOS
  PcI8086::setBL(0x0f);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x1019); // read PEL mask through BIOS
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::BL() == 0x0f);
  machine.readPort(0x03c6);
  machine.readPort(0x03c6);
  machine.readPort(0x03c6);
  machine.readPort(0x03c6); // next raw 03C6h read/write would expose DAC command, not PEL mask
  PcI8086::setAX(0x1019);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::BL() == 0x0f);
  PcI8086::setAX(0x1018);
  PcI8086::setBL(0x07);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x1019);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::BL() == 0x07);
  PcI8086::setAX(0x1018);
  PcI8086::setBL(0xff);
  PcI8086::triggerInterrupt(0x10);

  PcI8086::setAX(0x1010); // gray-scale summing should rewrite R/G/B equally
  PcI8086::setBX(0x0040);
  PcI8086::setDH(10);
  PcI8086::setCH(20);
  PcI8086::setCL(30);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x101b);
  PcI8086::setBX(0x0040);
  PcI8086::setCX(1);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x1015);
  PcI8086::setBX(0x0040);
  PcI8086::triggerInterrupt(0x10);
  uint8_t const expectedGray = static_cast<uint8_t>((30 * 10 + 59 * 20 + 11 * 30 + 50) / 100);
  assert(PcI8086::DH() == expectedGray);
  assert(PcI8086::CH() == expectedGray);
  assert(PcI8086::CL() == expectedGray);

  machine.writePort(0x03c8, 0x60);
  machine.writePort(0x03c9, 10);
  machine.writePort(0x03c9, 20);
  machine.writePort(0x03c9, 30);
  machine.writePort(0x03c8, 0x52);
  machine.writePort(0x03c9, 0x11); // partial DAC write cursor: DAC[52h].green is next
  PcI8086::setAX(0x101b);
  PcI8086::setBX(0x0060);
  PcI8086::setCX(1);
  PcI8086::triggerInterrupt(0x10);
  machine.writePort(0x03c9, 0x22);
  machine.writePort(0x03c9, 0x33);
  PcI8086::setAX(0x1015);
  PcI8086::setBX(0x0052);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::DH() == 0x11);
  assert(PcI8086::CH() == 0x22);
  assert(PcI8086::CL() == 0x33);
  PcI8086::setAX(0x1015);
  PcI8086::setBX(0x0060);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::DH() == expectedGray);
  assert(PcI8086::CH() == expectedGray);
  assert(PcI8086::CL() == expectedGray);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  PcI8086::setAX(0x1013); // 16 pages of 16 colors
  PcI8086::setBX(0x0100);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x1013); // select DAC color page 3
  PcI8086::setBX(0x0301);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x101a);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::BL() == 0x01);
  assert(PcI8086::BH() == 0x03);
  machine.writePort(0x03c8, 0x31);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  assert(machine.writeGraphicsPixel(0, 0, 0x01, false));
  std::vector<uint16_t> biosPaletteLine(PcMachine::VgaGraphics16Width, 0);
  machine.renderGraphicsLine(0, biosPaletteLine.data());
  assert(biosPaletteLine[0] == 0x07e0);

  PcI8086::setAX(0x1201); // disable default palette loading on mode set
  PcI8086::setBX(0x0031);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x0012); // mode set must preserve DAC color-page mode coherently
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x101a);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::BL() == 0x01);
  assert(PcI8086::BH() == 0x03);
  assert(machine.writeGraphicsPixel(0, 0, 0x01, false));
  machine.renderGraphicsLine(0, biosPaletteLine.data());
  assert(biosPaletteLine[0] == 0x07e0);
  PcI8086::setAX(0x1200); // restore default palette loading for later tests
  PcI8086::setBX(0x0031);
  PcI8086::triggerInterrupt(0x10);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  PcI8086::setAX(0x1013); // 16 pages of 16 colors
  PcI8086::setBX(0x0100);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x1013); // select DAC color page 5
  PcI8086::setBX(0x0501);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x1c01); // save hardware state only
  PcI8086::setBX(0x0700);
  PcI8086::setCX(0x0001);
  PcI8086::setES(0x0000);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::AL() == 0x1c);
  PcI8086::setAX(0x1013); // switch to 4 pages of 64 colors
  PcI8086::setBX(0x0000);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x1013); // select page 2 in the alternate mode
  PcI8086::setBX(0x0201);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x101a);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::BL() == 0x00);
  assert(PcI8086::BH() == 0x02);
  PcI8086::setAX(0x1c02); // restore hardware state only; BIOS query must follow restored AC mode bit
  PcI8086::setBX(0x0700);
  PcI8086::setCX(0x0001);
  PcI8086::setES(0x0000);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::AL() == 0x1c);
  PcI8086::setAX(0x101a);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::BL() == 0x01);
  assert(PcI8086::BH() == 0x05);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  PcI8086::setAX(0x1013); // make the BIOS shadow disagree with the direct AC programming below
  PcI8086::setBX(0x0000); // 4 pages of 64 colors
  PcI8086::triggerInterrupt(0x10);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x30); // Attribute Controller mode control
  machine.writePort(0x03c0, 0x81); // direct port path: graphics + 16 pages of 16 DAC colors
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x34); // Color Select
  machine.writePort(0x03c0, 0x05); // page 5 when AC mode-control bit 7 is set
  PcI8086::setAX(0x101a);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::BL() == 0x01);
  assert(PcI8086::BH() == 0x05);

  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x34);
  machine.writePort(0x03c0, 0x00);
  PcI8086::setAX(0x1013); // BIOS select-page must honor the directly-programmed AC page mode
  PcI8086::setBX(0x0701);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x101a);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::BL() == 0x01);
  assert(PcI8086::BH() == 0x07);

  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics640x480x16);
  PcI8086::setAX(0x1013); // stale BIOS shadow: 4 pages of 64 colors
  PcI8086::setBX(0x0000);
  PcI8086::triggerInterrupt(0x10);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x30);
  machine.writePort(0x03c0, 0x81); // actual AC state: 16 pages of 16 colors
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x34);
  machine.writePort(0x03c0, 0x05); // page 5 -> DAC[51h] for attribute color 1
  machine.writePort(0x03c8, 0x51);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  PcI8086::setAX(0x1201); // disable default palette loading
  PcI8086::setBX(0x0031);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x0012); // mode set must preserve direct AC color-page state, not stale BIOS shadow
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x101a);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::BL() == 0x01);
  assert(PcI8086::BH() == 0x05);
  assert(machine.writeGraphicsPixel(0, 0, 0x01, false));
  machine.renderGraphicsLine(0, biosPaletteLine.data());
  assert(biosPaletteLine[0] == 0x07e0);
  PcI8086::setAX(0x1200); // restore default palette loading for later tests
  PcI8086::setBX(0x0031);
  PcI8086::triggerInterrupt(0x10);

  machine.reset();
  uint8_t const equipmentWordProgram[] = {
    0xcd, 0x11,             // int 11h: get equipment word
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0xb8, 0x07, 0x00,       // mov ax, 0x0007
    0xcd, 0x10,             // int 10h: set monochrome text mode
    0xcd, 0x11,             // int 11h
    0xa3, 0x02, 0x05,       // mov [0x0502], ax
    0xb8, 0x03, 0x00,       // mov ax, 0x0003
    0xcd, 0x10,             // int 10h: set color 80x25 text mode
    0xcd, 0x11,             // int 11h
    0xa3, 0x04, 0x05,       // mov [0x0504], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, equipmentWordProgram, sizeof(equipmentWordProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(48);
  assert(machine.cpuHalted());
  assert((machine.readMemory16(0x0500) & 0x0030) == 0x0020);
  assert((machine.readMemory16(0x0502) & 0x0030) == 0x0030);
  assert((machine.readMemory16(0x0504) & 0x0030) == 0x0020);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const monoTextMemoryProgram[] = {
    0xb8, 0x07, 0x00,       // mov ax, 0x0007
    0xcd, 0x10,             // int 10h: set monochrome text mode
    0xb8, 0x4d, 0x0e,       // mov ax, 0x0e4d: teletype 'M'
    0xcd, 0x10,             // int 10h
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, monoTextMemoryProgram, sizeof(monoTextMemoryProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(24);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0449) == 0x07);
  assert(machine.readMemory16(0x0463) == 0x03b4);
  assert((machine.readMemory16(0x0410) & 0x0030) == 0x0030);
  assert((machine.readVideoMemory16(PcMachine::HerculesGraphicsMemoryBase) & 0x00ff) == 'M');
  assert(machine.text80Buffer()[0] == 'M');
  assert((machine.readVideoMemory16(PcMachine::TextColorMemoryBase) & 0x00ff) != 'M');
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.writeVideoMemory16(PcMachine::HerculesGraphicsMemoryBase + 2, 0x074e);
  machine.writePort(0x03b4, 0x0c);
  machine.writePort(0x03b5, 0x00);
  machine.writePort(0x03b4, 0x0d);
  machine.writePort(0x03b5, 0x01); // mono CRTC start address is in character cells
  assert(machine.readPort(0x03b4) == 0x0d);
  assert(machine.readPort(0x03b5) == 0x01);
  assert(machine.text80Buffer()[0] == 'N');

  machine.reset();
  uint8_t const printerStatusProgram[] = {
    0xb4, 0x02,             // mov ah, 0x02
    0x30, 0xd2,             // xor dl, dl: LPT1
    0xcd, 0x17,             // int 17h: get printer status
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0xb8, 0x41, 0x00,       // mov ax, 0x0041: print 'A'
    0xcd, 0x17,             // int 17h: print character
    0xa3, 0x02, 0x05,       // mov [0x0502], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, printerStatusProgram, sizeof(printerStatusProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(32);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0501) == 0x01);
  assert(machine.readMemory8(0x0503) == 0x01);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const serialStatusProgram[] = {
    0xb4, 0x00,             // mov ah, 0x00
    0x30, 0xd2,             // xor dl, dl: COM1
    0xb0, 0xe3,             // mov al, 0xe3: 9600 8N1
    0xcd, 0x14,             // int 14h: initialize serial port
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0xb4, 0x03,             // mov ah, 0x03
    0xcd, 0x14,             // int 14h: get serial status
    0xa3, 0x02, 0x05,       // mov [0x0502], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, serialStatusProgram, sizeof(serialStatusProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(32);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0501) == 0x80);
  assert(machine.readMemory8(0x0503) == 0x80);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x1e42);
  uint8_t const textNoClearModeProgram[] = {
    0xb8, 0x83, 0x00,       // mov ax, 0x0083: set 80x25 text, preserve display memory
    0xcd, 0x10,             // int 10h
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, textNoClearModeProgram, sizeof(textNoClearModeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(80);
  assert(machine.cpuHalted());
  assert(machine.readVideoMemory16(PcMachine::TextColorMemoryBase) == 0x1e42);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x1e41);
  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x31); // Attribute Controller overscan/border color
  machine.writePort(0x03c0, 0x02);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x33); // Attribute Controller horizontal PEL panning
  machine.writePort(0x03c0, 0x01);
  machine.renderText80Line(0, line);
  assert(line[PcTextRenderer::Width - 1] == 0x07e0);

  machine.reset();
  machine.setVideoMode(PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x5a);
  uint8_t const graphicsNoClearModeProgram[] = {
    0xb8, 0x93, 0x00,       // mov ax, 0x0093: set mode 13h, preserve display memory
    0xcd, 0x10,             // int 10h
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, graphicsNoClearModeProgram, sizeof(graphicsNoClearModeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(80);
  assert(machine.cpuHalted());
  assert(machine.videoMode() == PcMachine::VideoMode::VgaGraphics320x200x256);
  assert(machine.readVideoMemory8(PcMachine::VgaGraphicsMemoryBase) == 0x5a);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  machine.textRenderer().setFont({blinkFont, 256});
  machine.textRenderer().setFrameCounter(0x20);
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase, 0x9e41);
  uint8_t const biosBlinkDisableProgram[] = {
    0xb8, 0x03, 0x10,       // mov ax, 0x1003
    0xb3, 0x00,             // mov bl, 0: disable blink / enable intense background
    0xcd, 0x10,             // int 10h
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, biosBlinkDisableProgram, sizeof(biosBlinkDisableProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  machine.renderText80Line(0, line);
  assert((machine.readMemory8(0x0465) & 0x20) == 0);
  assert(line[0] == yellow);
  for (int x = 1; x < PcTextRenderer::CellWidth; ++x)
    assert(line[x] == brightBlue);
  machine.textRenderer().setFont(PcTextRenderer::defaultFont());
  machine.textRenderer().setFrameCounter(0);

  machine.reset();
  uint8_t const videoModeControlBdaProgram[] = {
    0xb8, 0x03, 0x10,       // mov ax, 0x1003
    0x31, 0xdb,             // xor bx, bx: disable blink / enable intense background
    0xcd, 0x10,             // int 10h
    0xa0, 0x65, 0x04,       // mov al, [0x0465]
    0xa2, 0x00, 0x05,       // mov [0x0500], al
    0xb8, 0x03, 0x10,       // mov ax, 0x1003
    0xbb, 0x01, 0x00,       // mov bx, 1: enable blink
    0xcd, 0x10,             // int 10h
    0xa0, 0x65, 0x04,       // mov al, [0x0465]
    0xa2, 0x01, 0x05,       // mov [0x0501], al
    0xb8, 0x03, 0x00,       // mov ax, 0x0003
    0xcd, 0x10,             // int 10h: mode set restores default blink state
    0xa0, 0x65, 0x04,       // mov al, [0x0465]
    0xa2, 0x02, 0x05,       // mov [0x0502], al
    0xba, 0xd8, 0x03,       // mov dx, 0x03d8
    0xb0, 0x0a,             // mov al, 0x0a
    0xee,                   // out dx, al: CGA mode control mirror
    0x42,                   // inc dx -> 0x03d9
    0xb0, 0x35,             // mov al, 0x35
    0xee,                   // out dx, al: CGA color-select mirror
    0xa0, 0x65, 0x04,       // mov al, [0x0465]
    0xa2, 0x03, 0x05,       // mov [0x0503], al
    0xa0, 0x66, 0x04,       // mov al, [0x0466]
    0xa2, 0x04, 0x05,       // mov [0x0504], al
    0xb8, 0x01, 0x00,       // mov ax, 0x0001
    0xcd, 0x10,             // int 10h: 40x25 color text
    0xba, 0xd8, 0x03,       // mov dx, 0x03d8
    0xec,                   // in al, dx
    0xa2, 0x05, 0x05,       // mov [0x0505], al
    0xa0, 0x65, 0x04,       // mov al, [0x0465]
    0xa2, 0x06, 0x05,       // mov [0x0506], al
    0xb8, 0x05, 0x00,       // mov ax, 0x0005
    0xcd, 0x10,             // int 10h: 320x200 gray/composite-suppressed CGA
    0xba, 0xd8, 0x03,       // mov dx, 0x03d8
    0xec,                   // in al, dx
    0xa2, 0x07, 0x05,       // mov [0x0507], al
    0xa0, 0x65, 0x04,       // mov al, [0x0465]
    0xa2, 0x08, 0x05,       // mov [0x0508], al
    0xb8, 0x06, 0x00,       // mov ax, 0x0006
    0xcd, 0x10,             // int 10h: 640x200 CGA
    0xba, 0xd8, 0x03,       // mov dx, 0x03d8
    0xec,                   // in al, dx
    0xa2, 0x09, 0x05,       // mov [0x0509], al
    0xa0, 0x65, 0x04,       // mov al, [0x0465]
    0xa2, 0x0a, 0x05,       // mov [0x050a], al
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, videoModeControlBdaProgram, sizeof(videoModeControlBdaProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(160);
  assert(machine.cpuHalted());
  assert((machine.readMemory8(0x0500) & 0x20) == 0);
  assert((machine.readMemory8(0x0501) & 0x20) != 0);
  assert((machine.readMemory8(0x0502) & 0x20) != 0);
  assert(machine.readMemory8(0x0503) == 0x0a);
  assert(machine.readMemory8(0x0504) == 0x35);
  assert(machine.readMemory8(0x0505) == 0x28);
  assert(machine.readMemory8(0x0506) == 0x28);
  assert(machine.readMemory8(0x0507) == 0x0e);
  assert(machine.readMemory8(0x0508) == 0x0e);
  assert(machine.readMemory8(0x0509) == 0x1a);
  assert(machine.readMemory8(0x050a) == 0x1a);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);
  assert(machine.diagnostics().unsupportedPortWriteCount == 0);

  machine.reset();
  uint8_t const vgaGraphicsPageProgram[] = {
    0xb8, 0x0d, 0x00,       // mov ax, 0x000d
    0xcd, 0x10,             // int 10h: set 320x200x16
    0xb4, 0x0c,             // mov ah, 0x0c
    0xb0, 0x0e,             // mov al, color 14
    0xb7, 0x01,             // mov bh, page 1
    0x31, 0xc9,             // xor cx, cx
    0x31, 0xd2,             // xor dx, dx
    0xcd, 0x10,             // int 10h: write pixel to page 1
    0xb4, 0x0d,             // mov ah, 0x0d
    0xb7, 0x01,             // mov bh, page 1
    0x31, 0xc9,             // xor cx, cx
    0x31, 0xd2,             // xor dx, dx
    0xcd, 0x10,             // int 10h: read pixel from page 1
    0xa2, 0x00, 0x05,       // mov [0x0500], al
    0xb4, 0x0d,             // mov ah, 0x0d
    0xb7, 0x00,             // mov bh, page 0
    0x31, 0xc9,             // xor cx, cx
    0x31, 0xd2,             // xor dx, dx
    0xcd, 0x10,             // int 10h: read pixel from page 0
    0xa2, 0x01, 0x05,       // mov [0x0501], al
    0xb8, 0x01, 0x05,       // mov ax, 0x0501
    0xcd, 0x10,             // int 10h: make page 1 visible
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, vgaGraphicsPageProgram, sizeof(vgaGraphicsPageProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(64);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0x0e);
  assert(machine.readMemory8(0x0501) == 0x00);
  assert(machine.readMemory8(0x0462) == 0x01);
  assert(machine.readMemory16(0x044e) == 0x2000);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] != 0x0000);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const cgaGraphicsPageProgram[] = {
    0xb8, 0x04, 0x00,       // mov ax, 0x0004
    0xcd, 0x10,             // int 10h: set CGA 320x200x4
    0xb4, 0x0c,             // mov ah, 0x0c
    0xb0, 0x03,             // mov al, color 3
    0xb7, 0x01,             // mov bh, page 1
    0x31, 0xc9,             // xor cx, cx
    0x31, 0xd2,             // xor dx, dx
    0xcd, 0x10,             // int 10h: write pixel to page 1
    0xb4, 0x0d,             // mov ah, 0x0d
    0xb7, 0x01,             // mov bh, page 1
    0x31, 0xc9,             // xor cx, cx
    0x31, 0xd2,             // xor dx, dx
    0xcd, 0x10,             // int 10h: read pixel from page 1
    0xa2, 0x00, 0x05,       // mov [0x0500], al
    0xb4, 0x0d,             // mov ah, 0x0d
    0xb7, 0x00,             // mov bh, page 0
    0x31, 0xc9,             // xor cx, cx
    0x31, 0xd2,             // xor dx, dx
    0xcd, 0x10,             // int 10h: read pixel from page 0
    0xa2, 0x01, 0x05,       // mov [0x0501], al
    0xb8, 0x01, 0x05,       // mov ax, 0x0501
    0xcd, 0x10,             // int 10h: make page 1 visible
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, cgaGraphicsPageProgram, sizeof(cgaGraphicsPageProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(64);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0x03);
  assert(machine.readMemory8(0x0501) == 0x00);
  assert(machine.readMemory8(0x0462) == 0x01);
  assert(machine.readMemory16(0x044e) == 0x4000);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] != 0x0000);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const textPageCursorProgram[] = {
    0xb8, 0x03, 0x00,       // mov ax, 0x0003
    0xcd, 0x10,             // int 10h: set 80x25 text mode
    0xb4, 0x02,             // mov ah, 0x02
    0xb7, 0x07,             // mov bh, page 7
    0xb6, 0x01,             // mov dh, row 1
    0xb2, 0x02,             // mov dl, column 2
    0xcd, 0x10,             // int 10h: set cursor on page 7
    0xb8, 0x45, 0x09,       // mov ax, 0x0945: write 'E' with attribute
    0xbb, 0x5a, 0x07,       // mov bx, 0x075a: page 7, attr 0x5a
    0xb9, 0x01, 0x00,       // mov cx, 1
    0xcd, 0x10,             // int 10h
    0xb4, 0x08,             // mov ah, 0x08
    0xb7, 0x07,             // mov bh, page 7
    0xcd, 0x10,             // int 10h: read page 7 cursor cell
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0xb8, 0x07, 0x05,       // mov ax, 0x0507
    0xcd, 0x10,             // int 10h: make page 7 visible
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, textPageCursorProgram, sizeof(textPageCursorProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(80);
  assert(machine.cpuHalted());
  uint32_t const page7Cell = PcMachine::TextColorMemoryBase + 0x7000 + 2 * (1 * 80 + 2);
  assert(machine.readMemory16(0x0500) == 0x5a45);
  assert(machine.readVideoMemory16(page7Cell) == 0x5a45);
  assert(machine.readVideoMemory16(PcMachine::TextColorMemoryBase + 2 * (1 * 80 + 2)) != 0x5a45);
  assert(machine.readMemory8(0x0462) == 0x07);
  assert(machine.readMemory16(0x044e) == 0x7000);
  assert(machine.text80Buffer()[2 * (1 * 80 + 2)] == 'E');
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  uint8_t const bootProgram[] = {
    0xb4, 0x00,             // mov ah, 0x00
    0xb0, 0x03,             // mov al, 0x03
    0xcd, 0x10,             // int 10h: set text mode
    0xb4, 0x0e,             // mov ah, 0x0e
    0xb0, 0x41,             // mov al, 'A'
    0xcd, 0x10,             // int 10h: teletype
    0xb8, 0x01, 0x02,       // mov ax, 0x0201 (read one sector)
    0xbb, 0x00, 0x06,       // mov bx, 0x0600
    0xb9, 0x02, 0x00,       // mov cx, 0x0002 (cyl 0, sector 2)
    0xba, 0x00, 0x00,       // mov dx, 0x0000 (head 0, fd0)
    0xcd, 0x13,             // int 13h: read CHS sector into ES:BX
    0xf4                    // hlt
  };
  memcpy(sector, bootProgram, sizeof(bootProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  uint8_t sector2[PcDiskImage::SectorSize] = {};
  memcpy(sector2, "INT13-SECTOR-2", 14);
  assert(machine.disk(0)->writeSectors(1, 1, sector2));
  machine.writeMemory8(PcMachine::DefaultBootLinearAddress, 0x00);
  assert(machine.loadBootSector(0, 0x00));
  PcMachine::BootState bootState = machine.bootState();
  assert(bootState.loaded);
  assert(bootState.diskIndex == 0);
  assert(bootState.biosDrive == 0x00);
  assert(bootState.segment == 0x0000);
  assert(bootState.offset == 0x7c00);
  assert(bootState.linearAddress == PcMachine::DefaultBootLinearAddress);
  assert(memcmp(machine.ram() + PcMachine::DefaultBootLinearAddress, bootProgram, sizeof(bootProgram)) == 0);
  assert(machine.readMemory16(PcMachine::DefaultBootLinearAddress + 510) == 0xaa55);
  assert(machine.prepareBootCpu());
  PcMachine::CpuState cpuState = machine.cpuState();
  assert(cpuState.cs == 0x0000);
  assert(cpuState.ip == 0x7c00);
  assert((cpuState.dx & 0x00ff) == 0x0000);
  assert(machine.runCpuSteps(32) == 12);
  assert(machine.cpuHalted());
  assert((machine.readVideoMemory16(PcMachine::TextColorMemoryBase) & 0x00ff) == 'A');
  assert(memcmp(machine.ram() + 0x0600, "INT13-SECTOR-2", 14) == 0);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const hltTimerWakeProgram[] = {
    0xfb,                   // sti
    0xf4,                   // hlt: should resume on the BIOS timer IRQ
    0xc6, 0x06, 0x00, 0x05, 0x42, // mov byte [0x0500], 0x42
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, hltTimerWakeProgram, sizeof(hltTimerWakeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  for (int i = 0; i < 20 && machine.readMemory8(0x0500) != 0x42; ++i) {
    usleep(60000);
    for (int j = 0; j < 4096 && machine.readMemory8(0x0500) != 0x42; ++j)
      machine.stepCpu();
  }
  assert(machine.readMemory8(0x0500) == 0x42);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const imulProgram[] = {
    0x69, 0x1e, 0x00, 0x05, 0x03, 0x00, // imul bx, [0x0500], 3
    0x6b, 0x0e, 0x00, 0x05, 0xfe,       // imul cx, [0x0500], -2
    0xf4                                // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, imulProgram, sizeof(imulProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.writeMemory16(0x0500, 0xfffb); // -5
  assert(machine.runCpuSteps(8) == 3);
  assert(machine.cpuHalted());
  cpuState = machine.cpuState();
  assert(cpuState.bx == 0xfff1); // -15
  assert(cpuState.cx == 0x000a); // 10
  assert(!PcI8086::flagCF());

  machine.reset();
  uint8_t const outsbProgram[] = {
    0xba, 0xf9, 0x00,       // mov dx, 0x00f9
    0xbe, 0x00, 0x05,       // mov si, 0x0500
    0xb9, 0x03, 0x00,       // mov cx, 3
    0xf3, 0x6e,             // rep outsb
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, outsbProgram, sizeof(outsbProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.writeMemory8(0x0500, 0x11);
  machine.writeMemory8(0x0501, 0x22);
  machine.writeMemory8(0x0502, 0x33);
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  cpuState = machine.cpuState();
  assert(cpuState.cx == 0);
  assert(PcI8086::SI() == 0x0503);
  assert(machine.diagnostics().unsupportedPortWriteCount == 0);

  machine.reset();
  uint8_t const operandSizePrefixProgram[] = {
    0xb0, 0x01,             // mov al, 1
    0xf8,                   // clc
    0x66, 0x10, 0xc0,       // operand-size prefix; adc al, al
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, operandSizePrefixProgram, sizeof(operandSizePrefixProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  cpuState = machine.cpuState();
  assert((cpuState.ax & 0x00ff) == 0x0002);

  machine.reset();
  uint8_t const popCsProgram[] = {
    0x0e,                   // push cs
    0x0f,                   // pop cs (8086 opcode; not a 286+ prefix here)
    0xb0, 0x4f,             // mov al, 'O'
    0xb4, 0x0e,             // mov ah, 0x0e
    0xcd, 0x10,             // int 10h: teletype
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, popCsProgram, sizeof(popCsProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert((machine.readVideoMemory16(PcMachine::TextColorMemoryBase) & 0x00ff) == 'O');
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const nullFarCallbackProgram[] = {
    0xff, 0x1e, 0x00, 0x05, // call far [0x0500] (0000:0000 optional callback)
    0xb8, 0xfe, 0xca,       // mov ax, 0xcafe
    0xa3, 0x04, 0x05,       // mov [0x0504], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, nullFarCallbackProgram, sizeof(nullFarCallbackProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.writeMemory16(0x0500, 0x0000);
  machine.writeMemory16(0x0502, 0x0000);
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0504) == 0xcafe);

  machine.reset();
  uint8_t const textMode40Program[] = {
    0xb8, 0x01, 0x00,       // mov ax, 0x0001
    0xcd, 0x10,             // int 10h: set 40x25 color text mode
    0xb4, 0x0f,             // mov ah, 0x0f
    0xcd, 0x10,             // int 10h: get current video mode
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0xb8, 0x02, 0x02,       // mov ax, 0x0202
    0x31, 0xdb,             // xor bx, bx
    0xba, 0x27, 0x00,       // mov dx, row 0, column 39
    0xcd, 0x10,             // int 10h: set cursor
    0xb4, 0x0e,             // mov ah, 0x0e
    0xb0, 0x5a,             // mov al, 'Z'
    0xcd, 0x10,             // int 10h: teletype at column 39
    0xb0, 0x59,             // mov al, 'Y'
    0xcd, 0x10,             // int 10h: wraps to row 1, column 0
    0xb4, 0x03,             // mov ah, 0x03
    0x30, 0xff,             // xor bh, bh
    0xcd, 0x10,             // int 10h: get cursor position
    0x89, 0x16, 0x02, 0x05, // mov [0x0502], dx
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, textMode40Program, sizeof(textMode40Program));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(64);
  assert(machine.cpuHalted());
  assert(machine.videoMode() == PcMachine::VideoMode::Text80);
  assert(machine.readMemory16(0x0500) == 0x2801); // AH=40 columns, AL=mode 1
  assert(machine.readMemory16(0x044a) == 40);
  assert(machine.readMemory16(0x044c) == 0x0800);
  assert((machine.readVideoMemory16(PcMachine::TextColorMemoryBase + 2 * 39) & 0x00ff) == 'Z');
  assert((machine.readVideoMemory16(PcMachine::TextColorMemoryBase + 2 * 40) & 0x00ff) == 'Y');
  assert(machine.readMemory16(0x0502) == 0x0101); // row 1, column 1 after writing Y
  bool renderedZ = false;
  for (int y = 0; y < PcTextRenderer::CellHeight; ++y) {
    machine.renderText80Line(y, line);
    for (int x = 39 * PcTextRenderer::CellWidth * 2; x < 40 * PcTextRenderer::CellWidth * 2; ++x)
      renderedZ = renderedZ || line[x] != 0x0000;
  }
  assert(renderedZ);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const textMode80BwProgram[] = {
    0xb8, 0x02, 0x00,       // mov ax, 0x0002
    0xcd, 0x10,             // int 10h: set 80x25 text mode
    0xb4, 0x0f,             // mov ah, 0x0f
    0xcd, 0x10,             // int 10h: get current video mode
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, textMode80BwProgram, sizeof(textMode80BwProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(24);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x5002);
  assert(machine.readMemory16(0x044a) == 80);
  assert(machine.readMemory16(0x044c) == 0x1000);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const egaModeProgram[] = {
    0xb8, 0x0d, 0x00,       // mov ax, 0x000d
    0xcd, 0x10,             // int 10h: set 320x200 16-color EGA/VGA mode
    0xb4, 0x0f,             // mov ah, 0x0f
    0xcd, 0x10,             // int 10h: get current video mode
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0x88, 0x3e, 0x02, 0x05, // mov [0x0502], bh
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, egaModeProgram, sizeof(egaModeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(24);
  assert(machine.cpuHalted());
  assert(machine.videoMode() == PcMachine::VideoMode::VgaGraphics320x200x16);
  assert(machine.readMemory16(0x0500) == 0x280d); // AH=40 columns, AL=mode
  assert(machine.readMemory8(0x0502) == 0x00);
  assert(machine.readMemory16(0x044a) == 40);
  assert(machine.readMemory16(0x044c) == 0x2000);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const egaMonoModeProgram[] = {
    0xb8, 0x0f, 0x00,       // mov ax, 0x000f
    0xcd, 0x10,             // int 10h: set 640x350 2-color EGA/VGA mode
    0xb4, 0x0f,             // mov ah, 0x0f
    0xcd, 0x10,             // int 10h: get current video mode
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, egaMonoModeProgram, sizeof(egaMonoModeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(24);
  assert(machine.cpuHalted());
  assert(machine.videoMode() == PcMachine::VideoMode::VgaGraphics640x350x2);
  assert(machine.readMemory16(0x0500) == 0x500f);
  assert(machine.readMemory16(0x044a) == 80);
  assert(machine.readMemory16(0x044c) == 0x8000);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const vgaMonoModeProgram[] = {
    0xb8, 0x11, 0x00,       // mov ax, 0x0011
    0xcd, 0x10,             // int 10h: set 640x480 2-color VGA mode
    0xb4, 0x0f,             // mov ah, 0x0f
    0xcd, 0x10,             // int 10h: get current video mode
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, vgaMonoModeProgram, sizeof(vgaMonoModeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(24);
  assert(machine.cpuHalted());
  assert(machine.videoMode() == PcMachine::VideoMode::VgaGraphics640x480x2);
  assert(machine.readMemory16(0x0500) == 0x5011);
  assert(machine.readMemory16(0x044a) == 80);
  assert(machine.readMemory16(0x044c) == 0x0000);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const displayCombinationProgram[] = {
    0xb8, 0x00, 0x1a,       // mov ax, 0x1a00
    0xcd, 0x10,             // int 10h: get display combination code
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0x89, 0x1e, 0x02, 0x05, // mov [0x0502], bx
    0xb8, 0x01, 0x1a,       // mov ax, 0x1a01
    0xbb, 0x08, 0x01,       // mov bx, 0x0108: active VGA color, inactive MDPA
    0xcd, 0x10,             // int 10h: set display combination code
    0xa3, 0x04, 0x05,       // mov [0x0504], ax
    0xa0, 0x8a, 0x04,       // mov al, [0x048a]
    0xa2, 0x08, 0x05,       // mov [0x0508], al
    0xb8, 0x00, 0x1a,       // mov ax, 0x1a00
    0xcd, 0x10,             // int 10h: read back selected combination
    0xa3, 0x0a, 0x05,       // mov [0x050a], ax
    0x89, 0x1e, 0x0c, 0x05, // mov [0x050c], bx
    0xb8, 0x01, 0x1a,       // mov ax, 0x1a01
    0xbb, 0x09, 0x09,       // mov bx, 0x0909: unsupported combination
    0xcd, 0x10,             // int 10h: must fail without changing the BDA DCC index
    0xa3, 0x0e, 0x05,       // mov [0x050e], ax
    0xa0, 0x8a, 0x04,       // mov al, [0x048a]
    0xa2, 0x10, 0x05,       // mov [0x0510], al
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, displayCombinationProgram, sizeof(displayCombinationProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(80);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0x1a);
  assert(machine.readMemory16(0x0502) == 0x0008);
  assert(machine.readMemory8(0x0504) == 0x1a);
  assert(machine.readMemory8(0x0508) == 0x0c);
  assert(machine.readMemory8(0x050a) == 0x1a);
  assert(machine.readMemory16(0x050c) == 0x0108);
  assert(machine.readMemory8(0x050e) == 0x00);
  assert(machine.readMemory8(0x0510) == 0x0c);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const vgaConfigInfoProgram[] = {
    0xb8, 0x00, 0x12,       // mov ax, 0x1200
    0xbb, 0x10, 0x00,       // mov bx, 0x0010
    0xcd, 0x10,             // int 10h: return EGA/VGA information
    0x89, 0x1e, 0x00, 0x05, // mov [0x0500], bx
    0x89, 0x0e, 0x02, 0x05, // mov [0x0502], cx
    0xa0, 0x87, 0x04,       // mov al, [0x0487]
    0xa2, 0x04, 0x05,       // mov [0x0504], al
    0xa0, 0x88, 0x04,       // mov al, [0x0488]
    0xa2, 0x05, 0x05,       // mov [0x0505], al
    0xa0, 0x89, 0x04,       // mov al, [0x0489]
    0xa2, 0x06, 0x05,       // mov [0x0506], al
    0xa0, 0x8a, 0x04,       // mov al, [0x048a]
    0xa2, 0x07, 0x05,       // mov [0x0507], al
    0x31, 0xc0,             // xor ax, ax
    0x8e, 0xc0,             // mov es, ax
    0xbf, 0x20, 0x05,       // mov di, 0x0520
    0x31, 0xdb,             // xor bx, bx
    0xb8, 0x00, 0x1b,       // mov ax, 0x1b00
    0xcd, 0x10,             // int 10h: get VGA dynamic state table
    0xa2, 0x08, 0x05,       // mov [0x0508], al
    0xa1, 0x20, 0x05,       // mov ax, [0x0520]
    0xa3, 0x09, 0x05,       // mov [0x0509], ax
    0xa1, 0x22, 0x05,       // mov ax, [0x0522]
    0xa3, 0x0b, 0x05,       // mov [0x050b], ax
    0xa0, 0x51, 0x05,       // mov al, [0x0551]
    0xa2, 0x0d, 0x05,       // mov [0x050d], al
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, vgaConfigInfoProgram, sizeof(vgaConfigInfoProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(48);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x0003);
  assert(machine.readMemory16(0x0502) == 0x0009);
  assert(machine.readMemory8(0x0504) == 0x60);
  assert(machine.readMemory8(0x0505) == 0x09);
  assert(machine.readMemory8(0x0506) == 0x11);
  assert(machine.readMemory8(0x0507) == 0x0b);
  assert(machine.readMemory8(0x0508) == 0x1b);
  assert(machine.readMemory16(0x0509) == 0xf180);
  assert(machine.readMemory16(0x050b) == 0xf000);
  assert(machine.readMemory8(0x050d) == 0x03);
  uint32_t const videoSavePointer = (static_cast<uint32_t>(machine.readMemory16(0x04aa)) << 4) +
                                    machine.readMemory16(0x04a8);
  assert(machine.readMemory16(videoSavePointer + 0x00) == 0xf220);
  assert(machine.readMemory16(videoSavePointer + 0x02) == 0xf000);
  assert(machine.readMemory16(videoSavePointer + 0x10) == 0xf1c0);
  assert(machine.readMemory16(videoSavePointer + 0x12) == 0xf000);
  uint32_t const secondarySavePointer = (static_cast<uint32_t>(machine.readMemory16(videoSavePointer + 0x12)) << 4) +
                                        machine.readMemory16(videoSavePointer + 0x10);
  assert(machine.readMemory16(secondarySavePointer + 0x00) == 0x001a);
  assert(machine.readMemory16(secondarySavePointer + 0x02) == 0xf1e0);
  assert(machine.readMemory16(secondarySavePointer + 0x04) == 0xf000);
  uint32_t const dccTable = (static_cast<uint32_t>(machine.readMemory16(secondarySavePointer + 0x04)) << 4) +
                            machine.readMemory16(secondarySavePointer + 0x02);
  assert(machine.readMemory8(dccTable + 0x00) == 16);
  assert(machine.readMemory8(dccTable + 0x01) == 1);
  assert(machine.readMemory8(dccTable + 0x02) == 8);
  assert(machine.readMemory8(dccTable + 0x04 + 2 * 0x0b + 0) == 0x00);
  assert(machine.readMemory8(dccTable + 0x04 + 2 * 0x0b + 1) == 0x08);
  uint32_t const videoParameterTable = (static_cast<uint32_t>(machine.readMemory16(videoSavePointer + 0x02)) << 4) +
                                       machine.readMemory16(videoSavePointer + 0x00);
  assert(machine.readMemory8(videoParameterTable + 0x1c * 64 + 0x00) == 40);
  assert(machine.readMemory8(videoParameterTable + 0x1c * 64 + 0x01) == 24);
  assert(machine.readMemory8(videoParameterTable + 0x1c * 64 + 0x02) == 8);
  assert(machine.readMemory16(videoParameterTable + 0x1c * 64 + 0x03) == 0xfa00);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const vgaScanLineSelectProgram[] = {
    0xb8, 0x00, 0x12,       // mov ax, 0x1200
    0xbb, 0x30, 0x00,       // mov bx, 0x0030
    0xcd, 0x10,             // int 10h: select 200 scan lines
    0xa1, 0x85, 0x04,       // mov ax, [0x0485]
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0xa0, 0x84, 0x04,       // mov al, [0x0484]
    0xa2, 0x02, 0x05,       // mov [0x0502], al
    0xb8, 0x01, 0x12,       // mov ax, 0x1201
    0xbb, 0x30, 0x00,       // mov bx, 0x0030
    0xcd, 0x10,             // int 10h: select 350 scan lines
    0xa1, 0x85, 0x04,       // mov ax, [0x0485]
    0xa3, 0x04, 0x05,       // mov [0x0504], ax
    0xa0, 0x84, 0x04,       // mov al, [0x0484]
    0xa2, 0x06, 0x05,       // mov [0x0506], al
    0xb8, 0x02, 0x12,       // mov ax, 0x1202
    0xbb, 0x30, 0x00,       // mov bx, 0x0030
    0xcd, 0x10,             // int 10h: select 400 scan lines
    0xa1, 0x85, 0x04,       // mov ax, [0x0485]
    0xa3, 0x08, 0x05,       // mov [0x0508], ax
    0xa0, 0x84, 0x04,       // mov al, [0x0484]
    0xa2, 0x0a, 0x05,       // mov [0x050a], al
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, vgaScanLineSelectProgram, sizeof(vgaScanLineSelectProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(96);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 8);
  assert(machine.readMemory8(0x0502) == 24);
  assert(machine.readMemory16(0x0504) == 14);
  assert(machine.readMemory8(0x0506) == 24);
  assert(machine.readMemory16(0x0508) == 16);
  assert(machine.readMemory8(0x050a) == 24);
  machine.writePort(0x03d4, 0x09);
  assert((machine.readPort(0x03d5) & 0x1f) == 15);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const vgaScanLine200Keeps25RowsProgram[] = {
    0xb8, 0x00, 0x12,       // mov ax, 0x1200
    0xbb, 0x30, 0x00,       // mov bx, 0x0030
    0xcd, 0x10,             // int 10h: select 200 scan lines
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, vgaScanLine200Keeps25RowsProgram, sizeof(vgaScanLine200Keeps25RowsProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(24);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0485) == 8);
  assert(machine.readMemory8(0x0484) == PcTextRenderer::Rows - 1);
  assert(machine.textRenderer().cellHeight() == 8);
  assert(machine.textRenderer().rows() == PcTextRenderer::Rows);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.setVideoMode(PcMachine::VideoMode::Text80);
  machine.writePort(0x03d4, 0x09);
  machine.writePort(0x03d5, 0x47); // direct CRTC max scan line: 8-scanline text cells
  assert((machine.readPort(0x03d5) & 0x1f) == 7);
  assert(machine.readMemory16(0x0485) == 8);
  assert(machine.readMemory8(0x0484) == 49);
  assert(machine.textRenderer().cellHeight() == 8);
  assert(machine.textRenderer().rows() == 50);
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase + 2 * (49 * PcTextRenderer::Columns), 0x0fdb);
  std::fill(graphicsLine.begin(), graphicsLine.end(), 0);
  machine.renderText80Line(49 * 8 + 3, graphicsLine.data());
  bool sawDirectCrtcLastRow = false;
  for (int x = 0; x < PcTextRenderer::CellWidth; ++x)
    sawDirectCrtcLastRow = sawDirectCrtcLastRow || graphicsLine[x] != 0x0000;
  assert(sawDirectCrtcLastRow);
  machine.writePort(0x03d5, 0x4d); // 14-scanline text cells
  assert(machine.readMemory16(0x0485) == 14);
  assert(machine.textRenderer().cellHeight() == 14);
  machine.writePort(0x03d5, 0x4f); // 16-scanline text cells
  assert(machine.readMemory16(0x0485) == PcTextRenderer::CellHeight);
  assert(machine.readMemory8(0x0484) == PcTextRenderer::Rows - 1);
  assert(machine.textRenderer().cellHeight() == PcTextRenderer::CellHeight);
  assert(machine.textRenderer().rows() == PcTextRenderer::Rows);

  machine.reset();
  uint8_t const vgaVideoAddressingProgram[] = {
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: set mode 13h
    0xb8, 0x00, 0xa0,       // mov ax, 0xa000
    0x8e, 0xc0,             // mov es, ax
    0x26, 0xc6, 0x06, 0x00, 0x00, 0x12, // mov byte [es:0], 0x12
    0xb8, 0x01, 0x12,       // mov ax, 0x1201
    0xbb, 0x32, 0x00,       // mov bx, 0x0032: disable video addressing
    0xcd, 0x10,             // int 10h
    0x26, 0xc6, 0x06, 0x00, 0x00, 0x34, // ignored while disabled
    0x26, 0xa0, 0x00, 0x00, // mov al, [es:0]
    0xa2, 0x00, 0x05,       // mov [0x0500], al
    0xb8, 0x00, 0x12,       // mov ax, 0x1200
    0xbb, 0x32, 0x00,       // mov bx, 0x0032: enable video addressing
    0xcd, 0x10,             // int 10h
    0x26, 0xa0, 0x00, 0x00, // mov al, [es:0]
    0xa2, 0x01, 0x05,       // mov [0x0501], al
    0x26, 0xc6, 0x06, 0x00, 0x00, 0x56, // mov byte [es:0], 0x56
    0x26, 0xa0, 0x00, 0x00, // mov al, [es:0]
    0xa2, 0x02, 0x05,       // mov [0x0502], al
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, vgaVideoAddressingProgram, sizeof(vgaVideoAddressingProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(96);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0xff);
  assert(machine.readMemory8(0x0501) == 0x12);
  assert(machine.readMemory8(0x0502) == 0x56);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const vgaCursorEmulationProgram[] = {
    0xb8, 0x01, 0x12,       // mov ax, 0x1201
    0xbb, 0x34, 0x00,       // mov bx, 0x0034: disable cursor emulation
    0xcd, 0x10,             // int 10h
    0xa2, 0x00, 0x05,       // mov [0x0500], al
    0xa0, 0x87, 0x04,       // mov al, [0x0487]
    0xa2, 0x03, 0x05,       // mov [0x0503], al
    0x31, 0xc0,             // xor ax, ax
    0x8e, 0xc0,             // mov es, ax
    0xbf, 0x20, 0x05,       // mov di, 0x0520
    0x31, 0xdb,             // xor bx, bx
    0xb8, 0x00, 0x1b,       // mov ax, 0x1b00
    0xcd, 0x10,             // int 10h: get VGA dynamic state table
    0xa0, 0x4d, 0x05,       // mov al, [0x054d] state flags
    0xa2, 0x04, 0x05,       // mov [0x0504], al
    0xb8, 0x00, 0x12,       // mov ax, 0x1200
    0xbb, 0x34, 0x00,       // mov bx, 0x0034: enable cursor emulation
    0xcd, 0x10,             // int 10h
    0xa2, 0x01, 0x05,       // mov [0x0501], al
    0xa0, 0x87, 0x04,       // mov al, [0x0487]
    0xa2, 0x05, 0x05,       // mov [0x0505], al
    0x31, 0xc0,             // xor ax, ax
    0x8e, 0xc0,             // mov es, ax
    0xbf, 0x20, 0x05,       // mov di, 0x0520
    0x31, 0xdb,             // xor bx, bx
    0xb8, 0x00, 0x1b,       // mov ax, 0x1b00
    0xcd, 0x10,             // int 10h: get VGA dynamic state table
    0xa0, 0x4d, 0x05,       // mov al, [0x054d] state flags
    0xa2, 0x06, 0x05,       // mov [0x0506], al
    0xb8, 0x02, 0x12,       // mov ax, 0x1202
    0xbb, 0x34, 0x00,       // mov bx, 0x0034: invalid selector
    0xcd, 0x10,             // int 10h
    0xa2, 0x02, 0x05,       // mov [0x0502], al
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, vgaCursorEmulationProgram, sizeof(vgaCursorEmulationProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(128);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0x12);
  assert(machine.readMemory8(0x0501) == 0x12);
  assert(machine.readMemory8(0x0502) == 0x00);
  assert((machine.readMemory8(0x0503) & 0x01) != 0);
  assert((machine.readMemory8(0x0504) & 0x10) == 0);
  assert((machine.readMemory8(0x0505) & 0x01) == 0);
  assert((machine.readMemory8(0x0506) & 0x10) != 0);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const vgaRefreshControlProgram[] = {
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: set mode 13h
    0xb8, 0x00, 0xa0,       // mov ax, 0xa000
    0x8e, 0xc0,             // mov es, ax
    0x26, 0xc6, 0x06, 0x00, 0x00, 0x04, // mov byte [es:0], 4 (default red)
    0xb8, 0x01, 0x12,       // mov ax, 0x1201
    0xbb, 0x36, 0x00,       // mov bx, 0x0036: disable refresh
    0xcd, 0x10,             // int 10h
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, vgaRefreshControlProgram, sizeof(vgaRefreshControlProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(64);
  assert(machine.cpuHalted());
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0x0000);
  machine.writePort(0x03c4, 0x01);
  assert((machine.readPort(0x03c5) & 0x20) != 0);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const vgaRefreshReenableProgram[] = {
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: set mode 13h
    0xb8, 0x00, 0xa0,       // mov ax, 0xa000
    0x8e, 0xc0,             // mov es, ax
    0x26, 0xc6, 0x06, 0x00, 0x00, 0x04, // mov byte [es:0], 4
    0xb8, 0x01, 0x12,       // mov ax, 0x1201
    0xbb, 0x36, 0x00,       // mov bx, 0x0036: disable refresh
    0xcd, 0x10,             // int 10h
    0xb8, 0x00, 0x12,       // mov ax, 0x1200
    0xbb, 0x36, 0x00,       // mov bx, 0x0036: enable refresh
    0xcd, 0x10,             // int 10h
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, vgaRefreshReenableProgram, sizeof(vgaRefreshReenableProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(96);
  assert(machine.cpuHalted());
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] != 0x0000);
  machine.writePort(0x03c4, 0x01);
  assert((machine.readPort(0x03c5) & 0x20) == 0);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const defaultPaletteLoadingProgram[] = {
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: set mode 13h
    0xb8, 0x10, 0x10,       // mov ax, 0x1010
    0xbb, 0x04, 0x00,       // mov bx, 4
    0xb6, 0x01,             // mov dh, red=1
    0xb9, 0x03, 0x02,       // mov cx, 0x0203 (CH=green=2, CL=blue=3)
    0xcd, 0x10,             // int 10h: set DAC entry 4
    0xb8, 0x01, 0x12,       // mov ax, 0x1201
    0xbb, 0x31, 0x00,       // mov bx, 0x0031: disable default palette loading
    0xcd, 0x10,             // int 10h
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: mode set preserves DAC palette
    0xb8, 0x15, 0x10,       // mov ax, 0x1015
    0xbb, 0x04, 0x00,       // mov bx, 4
    0xcd, 0x10,             // int 10h: read DAC entry 4
    0x89, 0x16, 0x00, 0x05, // mov [0x0500], dx
    0x89, 0x0e, 0x02, 0x05, // mov [0x0502], cx
    0xb8, 0x00, 0x12,       // mov ax, 0x1200
    0xbb, 0x31, 0x00,       // mov bx, 0x0031: enable default palette loading
    0xcd, 0x10,             // int 10h
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: mode set reloads default palette
    0xb8, 0x15, 0x10,       // mov ax, 0x1015
    0xbb, 0x04, 0x00,       // mov bx, 4
    0xcd, 0x10,             // int 10h: read DAC entry 4
    0x89, 0x16, 0x04, 0x05, // mov [0x0504], dx
    0x89, 0x0e, 0x06, 0x05, // mov [0x0506], cx
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, defaultPaletteLoadingProgram, sizeof(defaultPaletteLoadingProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(160);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0501) == 0x01);
  assert(machine.readMemory8(0x0503) == 0x02);
  assert(machine.readMemory8(0x0502) == 0x03);
  assert(machine.readMemory8(0x0505) == 42);
  assert(machine.readMemory8(0x0507) == 0x00);
  assert(machine.readMemory8(0x0506) == 0x00);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const paletteDisabledModeSetPreservesDacCursorProgram[] = {
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: set mode 13h
    0xb8, 0x01, 0x12,       // mov ax, 0x1201
    0xbb, 0x31, 0x00,       // mov bx, 0x0031: disable default palette loading
    0xcd, 0x10,             // int 10h
    0xba, 0xc8, 0x03,       // mov dx, 0x03c8
    0xb0, 0x05,             // mov al, 5
    0xee,                   // out dx, al: DAC write index = 5
    0xba, 0xc9, 0x03,       // mov dx, 0x03c9
    0xb0, 0x11,             // mov al, 0x11
    0xee,                   // out dx, al: write DAC[5].red; cursor now expects green
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: preserve palette and DAC cursor
    0xba, 0xc9, 0x03,       // mov dx, 0x03c9
    0xb0, 0x22,             // mov al, 0x22
    0xee,                   // out dx, al: should write DAC[5].green
    0xb0, 0x33,             // mov al, 0x33
    0xee,                   // out dx, al: should write DAC[5].blue
    0xb8, 0x15, 0x10,       // mov ax, 0x1015
    0xbb, 0x05, 0x00,       // mov bx, 5
    0xcd, 0x10,             // int 10h: read DAC entry 5
    0x89, 0x16, 0x00, 0x05, // mov [0x0500], dx
    0x89, 0x0e, 0x02, 0x05, // mov [0x0502], cx
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, paletteDisabledModeSetPreservesDacCursorProgram, sizeof(paletteDisabledModeSetPreservesDacCursorProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(128);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0501) == 0x11);
  assert(machine.readMemory8(0x0503) == 0x22);
  assert(machine.readMemory8(0x0502) == 0x33);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const paletteDisabledModeSetPreservesDacPageProgram[] = {
    0xb8, 0x12, 0x00,       // mov ax, 0x0012
    0xcd, 0x10,             // int 10h: set VGA 640x480x16
    0xb8, 0x13, 0x10,       // mov ax, 0x1013
    0xbb, 0x00, 0x01,       // mov bx, 0x0100: 16 pages of 16 colors
    0xcd, 0x10,             // int 10h: select DAC page mode
    0xb8, 0x13, 0x10,       // mov ax, 0x1013
    0xbb, 0x01, 0x03,       // mov bx, 0x0301: page 3
    0xcd, 0x10,             // int 10h: select DAC page
    0xb8, 0x01, 0x12,       // mov ax, 0x1201
    0xbb, 0x31, 0x00,       // mov bx, 0x0031: disable default palette loading
    0xcd, 0x10,             // int 10h
    0xb8, 0x12, 0x00,       // mov ax, 0x0012
    0xcd, 0x10,             // int 10h: mode set preserves DAC page state
    0xb8, 0x1a, 0x10,       // mov ax, 0x101a
    0xcd, 0x10,             // int 10h: read DAC page state
    0x89, 0x1e, 0x00, 0x05, // mov [0x0500], bx
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, paletteDisabledModeSetPreservesDacPageProgram, sizeof(paletteDisabledModeSetPreservesDacPageProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(128);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x0301);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const defaultGrayScaleSummingProgram[] = {
    0xb8, 0x00, 0x12,       // mov ax, 0x1200
    0xbb, 0x33, 0x00,       // mov bx, 0x0033: enable default gray-scale summing
    0xcd, 0x10,             // int 10h
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: mode set loads grayscale-summed default palette
    0xb8, 0x15, 0x10,       // mov ax, 0x1015
    0xbb, 0x04, 0x00,       // mov bx, 4
    0xcd, 0x10,             // int 10h: read DAC entry 4
    0x89, 0x16, 0x00, 0x05, // mov [0x0500], dx
    0x89, 0x0e, 0x02, 0x05, // mov [0x0502], cx
    0xb8, 0x01, 0x12,       // mov ax, 0x1201
    0xbb, 0x33, 0x00,       // mov bx, 0x0033: disable default gray-scale summing
    0xcd, 0x10,             // int 10h
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: mode set reloads color default palette
    0xb8, 0x15, 0x10,       // mov ax, 0x1015
    0xbb, 0x04, 0x00,       // mov bx, 4
    0xcd, 0x10,             // int 10h: read DAC entry 4
    0x89, 0x16, 0x04, 0x05, // mov [0x0504], dx
    0x89, 0x0e, 0x06, 0x05, // mov [0x0506], cx
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, defaultGrayScaleSummingProgram, sizeof(defaultGrayScaleSummingProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(160);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0501) == 13);
  assert(machine.readMemory8(0x0503) == 13);
  assert(machine.readMemory8(0x0502) == 13);
  assert(machine.readMemory8(0x0505) == 42);
  assert(machine.readMemory8(0x0507) == 0x00);
  assert(machine.readMemory8(0x0506) == 0x00);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const biosTimerProgram[] = {
    0xb4, 0x01,             // mov ah, 0x01
    0xb9, 0x00, 0x00,       // mov cx, 0x0000
    0xba, 0x78, 0x56,       // mov dx, 0x5678
    0xcd, 0x1a,             // int 1Ah: set system timer ticks
    0xb4, 0x00,             // mov ah, 0x00
    0xcd, 0x1a,             // int 1Ah: get system timer ticks
    0x89, 0x0e, 0x00, 0x05, // mov [0x0500], cx
    0x89, 0x16, 0x02, 0x05, // mov [0x0502], dx
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, biosTimerProgram, sizeof(biosTimerProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  {
    uint32_t const ticks = (static_cast<uint32_t>(machine.readMemory16(0x0500)) << 16) | machine.readMemory16(0x0502);
    assert(ticks >= 0x00005678);
    assert(ticks < 0x00005690);
  }
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const videoStateProgram[] = {
    0xb8, 0x00, 0x1b,       // mov ax, 0x1b00
    0xbb, 0x00, 0x00,       // mov bx, 0x0000
    0xbf, 0x00, 0x06,       // mov di, 0x0600
    0xcd, 0x10,             // int 10h: video state info
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, videoStateProgram, sizeof(videoStateProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  cpuState = machine.cpuState();
  assert((cpuState.ax & 0x00ff) == 0x001b);
  assert(machine.readMemory16(0x0600) == 0xf180); // F000:F180 static VGA functionality table
  assert(machine.readMemory16(0x0602) == 0xf000);
  assert(machine.readMemory8(0xff180) == 0xff); // modes 00h-07h-compatible supported
  assert(machine.readMemory8(0xff181) == 0xe0); // modes 0Dh-0Fh supported
  assert(machine.readMemory8(0xff182) == 0x0f); // modes 10h-13h supported
  assert(machine.readMemory8(0x0604) == 0x03); // current BIOS video mode
  assert(machine.readMemory16(0x0605) == 80);   // columns copied from BDA
  assert(machine.readMemory8(0x0622) == 24);    // rows minus one
  assert(machine.readMemory16(0x0623) == 16);   // character height
  assert(machine.readMemory8(0x0625) == 0x08);  // VGA analog color display
  assert(machine.readMemory16(0x0627) == 16);
  assert(machine.readMemory8(0x0629) == 8);
  assert(machine.readMemory8(0x062a) == 0);     // 200 scan-line text mode
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint32_t const truncatedVideoStateBuffer = PcMachine::RamSize - 900;
  machine.writeMemory8(truncatedVideoStateBuffer + 0, 'T');
  machine.writeMemory8(truncatedVideoStateBuffer + 1, 'D');
  machine.writeMemory8(truncatedVideoStateBuffer + 2, 'V');
  machine.writeMemory8(truncatedVideoStateBuffer + 3, 'S');
  uint8_t const videoStateRestoreRejectsTruncatedBufferProgram[] = {
    0xb8, 0x00, 0xf0,       // mov ax, 0xf000
    0x8e, 0xc0,             // mov es, ax
    0xbb, 0x7c, 0xfc,       // mov bx, 0xfc7c (linear 0xffc7c: 900 bytes left, not a full state buffer)
    0xb8, 0x02, 0x1c,       // mov ax, 0x1c02
    0xb9, 0xff, 0xff,       // mov cx, 0xffff
    0xcd, 0x10,             // int 10h: restore video state must fail cleanly
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0x9c,                   // pushf
    0x58,                   // pop ax
    0xa3, 0x02, 0x05,       // mov [0x0502], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, videoStateRestoreRejectsTruncatedBufferProgram, sizeof(videoStateRestoreRejectsTruncatedBufferProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(48);
  assert(machine.cpuHalted());
  assert((machine.readMemory16(0x0500) & 0x00ff) == 0x0000);
  assert((machine.readMemory16(0x0502) & 0x0001) != 0); // CF set
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const videoStateRestoresClampedAttributePaletteProgram[] = {
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: set VGA 320x200x256
    0xb8, 0x00, 0x10,       // mov ax, 0x1000
    0xbb, 0x03, 0xaa,       // mov bx, 0xaa03: palette register 3 clamps to 0x2a
    0xcd, 0x10,             // int 10h: set single palette register
    0xb8, 0x01, 0x10,       // mov ax, 0x1001
    0xb7, 0xf1,             // mov bh, 0xf1: overscan clamps to 0x31
    0xcd, 0x10,             // int 10h: set overscan
    0xb8, 0x01, 0x1c,       // mov ax, 0x1c01
    0xbb, 0x00, 0x06,       // mov bx, 0x0600
    0xb9, 0xff, 0xff,       // mov cx, 0xffff
    0xcd, 0x10,             // int 10h: save video state
    0xb8, 0x00, 0x10,       // mov ax, 0x1000
    0xbb, 0x03, 0x00,       // mov bx, 0x0003: corrupt palette register 3
    0xcd, 0x10,
    0xb8, 0x01, 0x10,       // mov ax, 0x1001
    0x30, 0xff,             // xor bh, bh: corrupt overscan
    0xcd, 0x10,
    0xb8, 0x02, 0x1c,       // mov ax, 0x1c02
    0xbb, 0x00, 0x06,       // mov bx, 0x0600
    0xb9, 0xff, 0xff,       // mov cx, 0xffff
    0xcd, 0x10,             // int 10h: restore video state
    0xb8, 0x09, 0x10,       // mov ax, 0x1009
    0xba, 0x00, 0x05,       // mov dx, 0x0500
    0xcd, 0x10,             // int 10h: read all palette registers
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, videoStateRestoresClampedAttributePaletteProgram, sizeof(videoStateRestoresClampedAttributePaletteProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(160);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0503) == 0x2a);
  assert(machine.readMemory8(0x0510) == 0x31);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const graphicsVideoStateProgram[] = {
    0xb8, 0x12, 0x00,       // mov ax, 0x0012
    0xcd, 0x10,             // int 10h: set VGA 640x480x16
    0xb8, 0x00, 0x1b,       // mov ax, 0x1b00
    0xbb, 0x00, 0x00,       // mov bx, 0x0000
    0xbf, 0x00, 0x06,       // mov di, 0x0600
    0xcd, 0x10,             // int 10h: video state info
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, graphicsVideoStateProgram, sizeof(graphicsVideoStateProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(32);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0604) == 0x12);
  assert(machine.readMemory8(0x0622) == 29);    // 640x480 graphics exposes 30 text rows
  assert(machine.readMemory16(0x0623) == 16);   // 16-scanline graphics text cells
  assert(machine.readMemory16(0x0627) == 16);
  assert(machine.readMemory8(0x0629) == 1);
  assert(machine.readMemory8(0x062a) == 3); // 480 scan-line graphics mode
  assert(machine.readMemory8(0x0484) == 29);
  assert(machine.readMemory16(0x0485) == 16);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const mode13VideoStateProgram[] = {
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: set VGA 320x200x256
    0xb8, 0x00, 0x1b,       // mov ax, 0x1b00
    0xbb, 0x00, 0x00,       // mov bx, 0x0000
    0xbf, 0x00, 0x06,       // mov di, 0x0600
    0xcd, 0x10,             // int 10h: video state info
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, mode13VideoStateProgram, sizeof(mode13VideoStateProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(32);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0604) == 0x13);
  assert(machine.readMemory8(0x0622) == 24);    // 320x200x256 keeps 25 8-pixel graphics text rows
  assert(machine.readMemory16(0x0623) == 8);
  assert(machine.readMemory16(0x0627) == 256);
  assert(machine.readMemory8(0x0629) == 1);
  assert(machine.readMemory8(0x062a) == 0); // 200 scan-line 256-color VGA mode
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const textModeResetsGraphicsMetricsProgram[] = {
    0xb8, 0x12, 0x00,       // mov ax, 0x0012
    0xcd, 0x10,             // int 10h: set VGA 640x480x16 (30 graphics text rows)
    0xb8, 0x03, 0x00,       // mov ax, 0x0003
    0xcd, 0x10,             // int 10h: return to 80x25 text mode
    0xa0, 0x84, 0x04,       // mov al, [0x0484]
    0xa2, 0x00, 0x05,       // mov [0x0500], al
    0xa1, 0x85, 0x04,       // mov ax, [0x0485]
    0xa3, 0x02, 0x05,       // mov [0x0502], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, textModeResetsGraphicsMetricsProgram, sizeof(textModeResetsGraphicsMetricsProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(32);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 24);
  assert(machine.readMemory16(0x0502) == 16);
  assert(machine.textRenderer().rows() == PcTextRenderer::Rows);
  assert(machine.textRenderer().cellHeight() == PcTextRenderer::CellHeight);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const videoExtensionProbeProgram[] = {
    0xb8, 0x00, 0xef,       // mov ax, 0xef00
    0xcd, 0x10,             // int 10h: extension probe
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, videoExtensionProbeProgram, sizeof(videoExtensionProbeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const videoBiosRomEntryProgram[] = {
    0x9a, 0x03, 0x00, 0x00, 0xc0, // call far C000:0003: option-ROM init entry
    0xb8, 0xef, 0xbe,             // mov ax, 0xbeef
    0xa3, 0x00, 0x05,             // mov [0x0500], ax
    0xf4                          // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, videoBiosRomEntryProgram, sizeof(videoBiosRomEntryProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(32);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0xbeef);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const int10HookedProgram[] = {
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: must dispatch to the hooked vector
    0xa3, 0x02, 0x05,       // mov [0x0502], ax
    0xf4                    // hlt
  };
  uint8_t const int10Hook[] = {
    0x1e,                   // push ds
    0x50,                   // push ax
    0x31, 0xc0,             // xor ax, ax
    0x8e, 0xd8,             // mov ds, ax
    0xc6, 0x06, 0x00, 0x05, 0x7c, // mov byte [0x0500], 0x7c
    0x58,                   // pop ax
    0x1f,                   // pop ds
    0x9c,                   // pushf: old INT 10h handler returns with iret
    0x9a, 0x70, 0xf1, 0x00, 0xf0, // call far F000:F170 old INT 10h handler
    0x1e,                   // push ds
    0x50,                   // push ax
    0x31, 0xc0,             // xor ax, ax
    0x8e, 0xd8,             // mov ds, ax
    0xc6, 0x06, 0x01, 0x05, 0x7d, // mov byte [0x0501], 0x7d
    0x58,                   // pop ax
    0x1f,                   // pop ds
    0xcf                    // iret
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, int10HookedProgram, sizeof(int10HookedProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  assert(machine.readMemory16(0x0010 * 4 + 0) == 0xf170);
  assert(machine.readMemory16(0x0010 * 4 + 2) == 0xf000);
  assert(machine.writeMemoryBlock(0x10000, int10Hook, sizeof(int10Hook)));
  machine.writeMemory16(0x0010 * 4 + 0, 0x0000);
  machine.writeMemory16(0x0010 * 4 + 2, 0x1000);
  machine.runCpuSteps(96);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0x7c);
  assert(machine.readMemory8(0x0501) == 0x7d);
  assert(machine.videoMode() == PcMachine::VideoMode::VgaGraphics320x200x256);
  assert(machine.readMemory8(0x0449) == 0x13);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const vbeProbeFailsCleanlyProgram[] = {
    0xb8, 0x00, 0x4f,       // mov ax, 0x4f00: VBE controller info
    0xbf, 0x00, 0x06,       // mov di, 0x0600
    0xcd, 0x10,             // int 10h
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0x9c,                   // pushf
    0x58,                   // pop ax
    0xa3, 0x02, 0x05,       // mov [0x0502], ax
    0xb8, 0x02, 0x4f,       // mov ax, 0x4f02: set VBE mode
    0xbb, 0x01, 0x01,       // mov bx, 0x0101
    0xcd, 0x10,             // int 10h
    0xa3, 0x04, 0x05,       // mov [0x0504], ax
    0x9c,                   // pushf
    0x58,                   // pop ax
    0xa3, 0x06, 0x05,       // mov [0x0506], ax
    0xb8, 0x08, 0x4f,       // mov ax, 0x4f08: VBE DAC palette control
    0xcd, 0x10,             // int 10h
    0xa3, 0x08, 0x05,       // mov [0x0508], ax
    0x9c,                   // pushf
    0x58,                   // pop ax
    0xa3, 0x0a, 0x05,       // mov [0x050a], ax
    0xb8, 0x09, 0x4f,       // mov ax, 0x4f09: VBE palette entries
    0xcd, 0x10,             // int 10h
    0xa3, 0x0c, 0x05,       // mov [0x050c], ax
    0x9c,                   // pushf
    0x58,                   // pop ax
    0xa3, 0x0e, 0x05,       // mov [0x050e], ax
    0xb8, 0x00, 0x4e,       // mov ax, 0x4e00: XGA environment info
    0xcd, 0x10,             // int 10h
    0xa3, 0x10, 0x05,       // mov [0x0510], ax
    0x9c,                   // pushf
    0x58,                   // pop ax
    0xa3, 0x12, 0x05,       // mov [0x0512], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, vbeProbeFailsCleanlyProgram, sizeof(vbeProbeFailsCleanlyProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(96);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x014f);
  assert((machine.readMemory16(0x0502) & 0x0001) != 0);
  assert(machine.readMemory16(0x0504) == 0x014f);
  assert((machine.readMemory16(0x0506) & 0x0001) != 0);
  assert(machine.readMemory16(0x0508) == 0x014f);
  assert((machine.readMemory16(0x050a) & 0x0001) != 0);
  assert(machine.readMemory16(0x050c) == 0x014f);
  assert((machine.readMemory16(0x050e) & 0x0001) != 0);
  assert(machine.readMemory16(0x0510) == 0x014e);
  assert((machine.readMemory16(0x0512) & 0x0001) != 0);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const fontInfoProgram[] = {
    0xb8, 0x30, 0x11,       // mov ax, 0x1130
    0xb7, 0x04,             // mov bh, 4 (8x16 VGA font)
    0xcd, 0x10,             // int 10h: get font information
    0x8c, 0x06, 0x00, 0x05, // mov [0x0500], es
    0x89, 0x2e, 0x02, 0x05, // mov [0x0502], bp
    0x89, 0x0e, 0x04, 0x05, // mov [0x0504], cx
    0x89, 0x16, 0x06, 0x05, // mov [0x0506], dx
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, fontInfoProgram, sizeof(fontInfoProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(32);
  assert(machine.cpuHalted());
  uint32_t const fontLinear = (static_cast<uint32_t>(machine.readMemory16(0x0500)) << 4) + machine.readMemory16(0x0502);
  uint32_t const int1fFontLinear = (static_cast<uint32_t>(machine.readMemory16(0x001f * 4 + 2)) << 4) +
                                   machine.readMemory16(0x001f * 4 + 0);
  uint32_t const int43FontLinear = (static_cast<uint32_t>(machine.readMemory16(0x0043 * 4 + 2)) << 4) +
                                   machine.readMemory16(0x0043 * 4 + 0);
  assert(int1fFontLinear == 0xfc800);
  assert(int43FontLinear == fontLinear);
  assert(int43FontLinear == 0xfe000);
  assert(machine.readMemory16(0x0504) == PcTextRenderer::CellHeight);
  assert((machine.readMemory16(0x0506) & 0x00ff) == PcTextRenderer::Rows - 1);
  assert(machine.isRamRangeValid(fontLinear, sizeof(tabdos::PcFont8x16Data)));
  assert(memcmp(machine.ram() + fontLinear, tabdos::PcFont8x16Data, sizeof(tabdos::PcFont8x16Data)) == 0);
  assert(machine.isRamRangeValid(int1fFontLinear, 8 * 256));
  for (uint16_t ch = 0; ch < 256; ++ch) {
    for (uint8_t row = 0; row < 8; ++row) {
      uint8_t const sourceRow = static_cast<uint8_t>(row * PcTextRenderer::CellHeight / 8);
      assert(machine.readMemory8(int1fFontLinear + static_cast<uint32_t>(ch) * 8 + row) ==
             tabdos::PcFont8x16Data[ch * PcTextRenderer::CellHeight + sourceRow]);
    }
  }
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const fontLoad8x8Program[] = {
    0xb8, 0x12, 0x11,       // mov ax, 0x1112: load ROM 8x8 font
    0xcd, 0x10,             // int 10h
    0xb8, 0x30, 0x11,       // mov ax, 0x1130
    0xb7, 0x03,             // mov bh, 3 (8x8 ROM font)
    0xcd, 0x10,             // int 10h: get font information
    0x8c, 0x06, 0x04, 0x05, // mov [0x0504], es
    0x89, 0x2e, 0x06, 0x05, // mov [0x0506], bp
    0x89, 0x0e, 0x00, 0x05, // mov [0x0500], cx
    0x89, 0x16, 0x02, 0x05, // mov [0x0502], dx
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, fontLoad8x8Program, sizeof(fontLoad8x8Program));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(48);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 8);
  assert((machine.readMemory16(0x0502) & 0x00ff) == 49);
  uint32_t const font8x8Linear = (static_cast<uint32_t>(machine.readMemory16(0x0504)) << 4) + machine.readMemory16(0x0506);
  assert(font8x8Linear == int1fFontLinear);
  assert(((static_cast<uint32_t>(machine.readMemory16(0x0043 * 4 + 2)) << 4) +
          machine.readMemory16(0x0043 * 4 + 0)) == int43FontLinear);
  assert(memcmp(machine.ram() + int43FontLinear, tabdos::PcFont8x16Data, sizeof(tabdos::PcFont8x16Data)) == 0);
  assert(machine.readMemory8(0x0484) == 49);
  assert(machine.readMemory16(0x0485) == 8);
  assert(machine.textRenderer().cellHeight() == 8);
  assert(machine.textRenderer().rows() == 50);
  machine.writePort(0x03d4, 0x09);
  assert((machine.readPort(0x03d5) & 0x1f) == 7);
  machine.writeVideoMemory16(PcMachine::TextColorMemoryBase + 2 * (49 * PcTextRenderer::Columns), 0x0fdb);
  std::fill(graphicsLine.begin(), graphicsLine.end(), 0);
  machine.renderText80Line(49 * 8 + 3, graphicsLine.data());
  bool sawLastTextRow = false;
  for (int x = 0; x < PcTextRenderer::CellWidth; ++x)
    sawLastTextRow = sawLastTextRow || graphicsLine[x] != 0x0000;
  assert(sawLastTextRow);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const fontLoad8x16Program[] = {
    0xb8, 0x14, 0x11,       // mov ax, 0x1114: load ROM 8x16 font
    0xcd, 0x10,             // int 10h
    0xb8, 0x30, 0x11,       // mov ax, 0x1130
    0xb7, 0x04,             // mov bh, 4 (8x16 ROM font)
    0xcd, 0x10,             // int 10h: get font information
    0x89, 0x0e, 0x00, 0x05, // mov [0x0500], cx
    0x89, 0x16, 0x02, 0x05, // mov [0x0502], dx
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, fontLoad8x16Program, sizeof(fontLoad8x16Program));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(48);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == PcTextRenderer::CellHeight);
  assert((machine.readMemory16(0x0502) & 0x00ff) == PcTextRenderer::Rows - 1);
  assert(machine.readMemory8(0x0484) == PcTextRenderer::Rows - 1);
  assert(machine.readMemory16(0x0485) == PcTextRenderer::CellHeight);
  assert(machine.textRenderer().cellHeight() == PcTextRenderer::CellHeight);
  assert(machine.textRenderer().rows() == PcTextRenderer::Rows);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const biosWriteStringProgram[] = {
    0xb4, 0x01,             // mov ah, 0x01
    0xb5, 0x06,             // mov ch, 0x06
    0xb1, 0x07,             // mov cl, 0x07
    0xcd, 0x10,             // int 10h: set cursor shape
    0xb8, 0x01, 0x13,       // mov ax, 0x1301 (write string, update cursor)
    0xbb, 0x0e, 0x00,       // mov bx, 0x000e (page 0, yellow)
    0xb9, 0x05, 0x00,       // mov cx, 5
    0xba, 0x03, 0x02,       // mov dx, 0x0203 (row 2, column 3)
    0xbd, 0x00, 0x05,       // mov bp, 0x0500
    0xcd, 0x10,             // int 10h: write ES:BP string
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, biosWriteStringProgram, sizeof(biosWriteStringProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  memcpy(machine.ram() + 0x0500, "HELLO", 5);
  machine.runCpuSteps(32);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0460) == 0x0607);
  assert(machine.readMemory16(0x0450) == 0x0208);
  for (int i = 0; i < 5; ++i) {
    uint16_t const cell = machine.readVideoMemory16(PcMachine::TextColorMemoryBase + 2 * (2 * PcTextRenderer::Columns + 3 + i));
    assert((cell & 0x00ff) == static_cast<uint8_t>("HELLO"[i]));
    assert((cell >> 8) == 0x0e);
  }
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const biosScrollUpWindowProgram[] = {
    0xb8, 0x01, 0x06,       // mov ax, 0x0601
    0xb7, 0x1e,             // mov bh, 0x1e
    0xb9, 0x02, 0x01,       // mov cx, 0x0102 (top=1,left=2)
    0xba, 0x04, 0x03,       // mov dx, 0x0304 (bottom=3,right=4)
    0xcd, 0x10,             // int 10h: scroll window up
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, biosScrollUpWindowProgram, sizeof(biosScrollUpWindowProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  for (int row = 0; row < 5; ++row) {
    for (int col = 0; col < 6; ++col) {
      uint16_t const cell = static_cast<uint16_t>(0x10 + row) << 8 | static_cast<uint8_t>('0' + row);
      machine.writeVideoMemory16(PcMachine::TextColorMemoryBase + 2 * (row * PcTextRenderer::Columns + col), cell);
    }
  }
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert((machine.readVideoMemory16(PcMachine::TextColorMemoryBase + 2 * (1 * PcTextRenderer::Columns + 2)) & 0x00ff) == '2');
  assert((machine.readVideoMemory16(PcMachine::TextColorMemoryBase + 2 * (2 * PcTextRenderer::Columns + 2)) & 0x00ff) == '3');
  assert(machine.readVideoMemory16(PcMachine::TextColorMemoryBase + 2 * (3 * PcTextRenderer::Columns + 2)) == 0x1e20);
  assert((machine.readVideoMemory16(PcMachine::TextColorMemoryBase + 2 * (1 * PcTextRenderer::Columns + 1)) & 0x00ff) == '1');
  assert((machine.readVideoMemory16(PcMachine::TextColorMemoryBase + 2 * (0 * PcTextRenderer::Columns + 2)) & 0x00ff) == '0');
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const biosScrollDownWindowProgram[] = {
    0xb8, 0x01, 0x07,       // mov ax, 0x0701
    0xb7, 0x2f,             // mov bh, 0x2f
    0xb9, 0x02, 0x01,       // mov cx, 0x0102 (top=1,left=2)
    0xba, 0x04, 0x03,       // mov dx, 0x0304 (bottom=3,right=4)
    0xcd, 0x10,             // int 10h: scroll window down
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, biosScrollDownWindowProgram, sizeof(biosScrollDownWindowProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  for (int row = 0; row < 5; ++row) {
    for (int col = 0; col < 6; ++col) {
      uint16_t const cell = static_cast<uint16_t>(0x20 + row) << 8 | static_cast<uint8_t>('A' + row);
      machine.writeVideoMemory16(PcMachine::TextColorMemoryBase + 2 * (row * PcTextRenderer::Columns + col), cell);
    }
  }
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert(machine.readVideoMemory16(PcMachine::TextColorMemoryBase + 2 * (1 * PcTextRenderer::Columns + 2)) == 0x2f20);
  assert((machine.readVideoMemory16(PcMachine::TextColorMemoryBase + 2 * (2 * PcTextRenderer::Columns + 2)) & 0x00ff) == 'B');
  assert((machine.readVideoMemory16(PcMachine::TextColorMemoryBase + 2 * (3 * PcTextRenderer::Columns + 2)) & 0x00ff) == 'C');
  assert((machine.readVideoMemory16(PcMachine::TextColorMemoryBase + 2 * (1 * PcTextRenderer::Columns + 1)) & 0x00ff) == 'B');
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const paletteProgram[] = {
    0xb8, 0x00, 0x10,       // mov ax, 0x1000
    0xcd, 0x10,             // int 10h: palette service
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, paletteProgram, sizeof(paletteProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const dacControlProgram[] = {
    0xb8, 0x18, 0x10,       // mov ax, 0x1018
    0xb3, 0x0f,             // mov bl, 0x0f
    0xcd, 0x10,             // int 10h: set PEL mask
    0xb8, 0x19, 0x10,       // mov ax, 0x1019
    0xcd, 0x10,             // int 10h: read PEL mask
    0x89, 0x1e, 0x00, 0x05, // mov [0x0500], bx
    0xb8, 0x13, 0x10,       // mov ax, 0x1013
    0xbb, 0x00, 0x01,       // mov bx, 0x0100 (BL=0: 16 pages of 16 colors)
    0xcd, 0x10,             // int 10h: select DAC page mode
    0xb8, 0x13, 0x10,       // mov ax, 0x1013
    0xbb, 0x01, 0x02,       // mov bx, 0x0201 (BL=1: page 2)
    0xcd, 0x10,             // int 10h: select DAC page
    0xb8, 0x1a, 0x10,       // mov ax, 0x101a
    0xcd, 0x10,             // int 10h: read DAC page state
    0x89, 0x1e, 0x02, 0x05, // mov [0x0502], bx
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, dacControlProgram, sizeof(dacControlProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(48);
  assert(machine.cpuHalted());
  assert((machine.readMemory16(0x0500) & 0x00ff) == 0x000f);
  assert(machine.readMemory16(0x0502) == 0x0201);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const dacPageModeResetProgram[] = {
    0xb8, 0x13, 0x10,       // mov ax, 0x1013
    0xbb, 0x00, 0x01,       // mov bx, 0x0100 (BL=0: 16 pages of 16 colors)
    0xcd, 0x10,             // int 10h: select DAC page mode
    0xb8, 0x13, 0x10,       // mov ax, 0x1013
    0xbb, 0x01, 0x03,       // mov bx, 0x0301 (BL=1: page 3)
    0xcd, 0x10,             // int 10h: select DAC page
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: set VGA 320x200x256; resets DAC page mode
    0xb8, 0x1a, 0x10,       // mov ax, 0x101a
    0xcd, 0x10,             // int 10h: read DAC page state
    0x89, 0x1e, 0x00, 0x05, // mov [0x0500], bx
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, dacPageModeResetProgram, sizeof(dacPageModeResetProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(48);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x0000);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const paletteReadAllProgram[] = {
    0xb8, 0x00, 0x10,       // mov ax, 0x1000
    0xbb, 0x03, 0xaa,       // mov bx, 0xaa03 (palette register 3 clamps to 0x2a)
    0xcd, 0x10,             // int 10h: set single palette register
    0xb8, 0x01, 0x10,       // mov ax, 0x1001
    0xb7, 0xf1,             // mov bh, 0xf1 (overscan clamps to 0x31)
    0xcd, 0x10,             // int 10h: set overscan
    0xb8, 0x09, 0x10,       // mov ax, 0x1009
    0xba, 0x00, 0x06,       // mov dx, 0x0600
    0xcd, 0x10,             // int 10h: read all palette registers
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, paletteReadAllProgram, sizeof(paletteReadAllProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(32);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0603) == 0x2a);
  assert(machine.readMemory8(0x0610) == 0x31);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const paletteReadPreservesAcPhaseProgram[] = {
    0xba, 0xda, 0x03,       // mov dx, 0x03da
    0xec,                   // in al, dx: reset AC flip-flop to index phase
    0xb8, 0x07, 0x10,       // mov ax, 0x1007
    0xbb, 0x01, 0x00,       // mov bx, 0x0001
    0xcd, 0x10,             // int 10h: read palette register 1
    0xba, 0xc0, 0x03,       // mov dx, 0x03c0
    0xb0, 0x21,             // mov al, 0x21: palette register 1, PAS set
    0xee,                   // out dx, al: must still be interpreted as index
    0xb0, 0x05,             // mov al, 0x05
    0xee,                   // out dx, al: data for palette register 1
    0xb8, 0x07, 0x10,       // mov ax, 0x1007
    0xbb, 0x01, 0x00,       // mov bx, 0x0001
    0xcd, 0x10,             // int 10h: read palette register 1
    0x88, 0x3e, 0x00, 0x05, // mov [0x0500], bh
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, paletteReadPreservesAcPhaseProgram, sizeof(paletteReadPreservesAcPhaseProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(64);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0x05);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const paletteWritePreservesAcPhaseProgram[] = {
    0xba, 0xda, 0x03,       // mov dx, 0x03da
    0xec,                   // in al, dx: reset AC flip-flop to index phase
    0xba, 0xc0, 0x03,       // mov dx, 0x03c0
    0xb0, 0x22,             // mov al, 0x22: palette register 2, PAS set
    0xee,                   // out dx, al: leave AC flip-flop in data phase
    0xb8, 0x00, 0x10,       // mov ax, 0x1000
    0xbb, 0x01, 0x04,       // mov bx, 0x0401: set palette register 1 = 4
    0xcd, 0x10,             // int 10h: must not disturb pending data phase for register 2
    0xba, 0xc0, 0x03,       // mov dx, 0x03c0
    0xb0, 0x07,             // mov al, 0x07
    0xee,                   // out dx, al: data for palette register 2
    0xb8, 0x07, 0x10,       // mov ax, 0x1007
    0xbb, 0x02, 0x00,       // mov bx, 0x0002
    0xcd, 0x10,             // int 10h: read palette register 2
    0x88, 0x3e, 0x00, 0x05, // mov [0x0500], bh
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, paletteWritePreservesAcPhaseProgram, sizeof(paletteWritePreservesAcPhaseProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(64);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0x07);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const vgaSetBorderProgram[] = {
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: set VGA 320x200x256
    0xb8, 0x10, 0x10,       // mov ax, 0x1010
    0xbb, 0x04, 0x00,       // mov bx, 4
    0xb6, 0x3f,             // mov dh, red
    0xb9, 0x3f, 0x00,       // mov cx, 0x003f (CH=green, CL=blue)
    0xcd, 0x10,             // int 10h: set DAC register 4 to magenta
    0xb4, 0x0b,             // mov ah, 0x0b
    0xbb, 0x04, 0x00,       // mov bx, 0x0004 (BH=0, BL=border color)
    0xcd, 0x10,             // int 10h: set VGA border/overscan
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, vgaSetBorderProgram, sizeof(vgaSetBorderProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(64);
  assert(machine.cpuHalted());
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x31);
  assert(machine.readPort(0x03c1) == 0x04);
  machine.writePort(0x03d4, 0x07);
  machine.writePort(0x03d5, 0x00);
  machine.writePort(0x03d4, 0x12);
  machine.writePort(0x03d5, 0x00);
  machine.renderGraphicsLine(1, graphicsLine.data());
  assert(graphicsLine[0] == 0xf81f);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const individualDacRegisterProgram[] = {
    0xb8, 0x10, 0x10,       // mov ax, 0x1010
    0xbb, 0x2a, 0x00,       // mov bx, 0x002a (BX=index)
    0xb6, 0x01,             // mov dh, red
    0xb9, 0x03, 0x02,       // mov cx, 0x0203 (CH=green, CL=blue)
    0xcd, 0x10,             // int 10h: set DAC register
    0xb8, 0x15, 0x10,       // mov ax, 0x1015
    0xbb, 0x2a, 0x00,       // mov bx, 0x002a
    0xcd, 0x10,             // int 10h: read DAC register
    0x89, 0x16, 0x00, 0x05, // mov [0x0500], dx (DH=red)
    0x89, 0x0e, 0x02, 0x05, // mov [0x0502], cx (CH=green, CL=blue)
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, individualDacRegisterProgram, sizeof(individualDacRegisterProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(48);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0501) == 0x01);
  assert(machine.readMemory8(0x0503) == 0x02);
  assert(machine.readMemory8(0x0502) == 0x03);
  machine.writePort(0x03c7, 0x2a);
  assert(machine.readPort(0x03c9) == 0x01);
  assert(machine.readPort(0x03c9) == 0x02);
  assert(machine.readPort(0x03c9) == 0x03);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const grayscaleDacProgram[] = {
    0xb8, 0x10, 0x10,       // mov ax, 0x1010
    0xbb, 0x20, 0x00,       // mov bx, 0x0020 (BX=index)
    0xb6, 0x3f,             // mov dh, red
    0xb9, 0x00, 0x00,       // mov cx, 0x0000
    0xcd, 0x10,             // int 10h: set DAC register 0x20 to red
    0xb8, 0x1b, 0x10,       // mov ax, 0x101b
    0xbb, 0x20, 0x00,       // mov bx, 0x0020
    0xb9, 0x01, 0x00,       // mov cx, 1
    0xcd, 0x10,             // int 10h: grayscale sum one DAC entry
    0xb8, 0x15, 0x10,       // mov ax, 0x1015
    0xbb, 0x20, 0x00,       // mov bx, 0x0020
    0xcd, 0x10,             // int 10h: read DAC register
    0x89, 0x16, 0x00, 0x05, // mov [0x0500], dx (DH=R)
    0x89, 0x0e, 0x02, 0x05, // mov [0x0502], cx (CH=G, CL=B)
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, grayscaleDacProgram, sizeof(grayscaleDacProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(48);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0501) == 19);
  assert(machine.readMemory8(0x0503) == 19);
  assert(machine.readMemory8(0x0502) == 19);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const videoStateSaveRestoreProgram[] = {
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: set VGA 320x200x256
    0xb8, 0x10, 0x10,       // mov ax, 0x1010
    0xbb, 0x21, 0x00,       // mov bx, 0x0021 (BX=index)
    0xb6, 0x3f,             // mov dh, red
    0xb9, 0x00, 0x00,       // mov cx, 0x0000
    0xcd, 0x10,             // int 10h: set DAC register 0x21 to red
    0xb8, 0x00, 0x1c,       // mov ax, 0x1c00
    0xb9, 0xff, 0xff,       // mov cx, 0xffff
    0xcd, 0x10,             // int 10h: get video state buffer size
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0x89, 0x1e, 0x02, 0x05, // mov [0x0502], bx
    0xb8, 0x01, 0x1c,       // mov ax, 0x1c01
    0xbb, 0x00, 0x06,       // mov bx, 0x0600
    0xb9, 0xff, 0xff,       // mov cx, 0xffff
    0xcd, 0x10,             // int 10h: save video state
    0xb8, 0x10, 0x10,       // mov ax, 0x1010
    0xbb, 0x21, 0x00,       // mov bx, 0x0021
    0xb6, 0x00,             // mov dh, red=0
    0xb9, 0x3f, 0x00,       // mov cx, 0x003f (CL=blue)
    0xcd, 0x10,             // int 10h: overwrite DAC register 0x21 to blue
    0xb8, 0x02, 0x1c,       // mov ax, 0x1c02
    0xbb, 0x00, 0x06,       // mov bx, 0x0600
    0xb9, 0xff, 0xff,       // mov cx, 0xffff
    0xcd, 0x10,             // int 10h: restore video state
    0xb8, 0x15, 0x10,       // mov ax, 0x1015
    0xbb, 0x21, 0x00,       // mov bx, 0x0021
    0xcd, 0x10,             // int 10h: read DAC register 0x21
    0x89, 0x16, 0x04, 0x05, // mov [0x0504], dx (DH=R)
    0x89, 0x0e, 0x06, 0x05, // mov [0x0506], cx (CH=G, CL=B)
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, videoStateSaveRestoreProgram, sizeof(videoStateSaveRestoreProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(96);
  assert(machine.cpuHalted());
  assert((machine.readMemory16(0x0500) & 0x00ff) == 0x001c);
  assert(machine.readMemory16(0x0502) >= 16);
  assert(machine.readMemory8(0x0505) == 0x3f);
  assert(machine.readMemory8(0x0507) == 0x00);
  assert(machine.readMemory8(0x0506) == 0x00);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const videoStateSavesPelMaskWhileDacCommandUnlockedProgram[] = {
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: set VGA 320x200x256
    0xb8, 0x10, 0x10,       // mov ax, 0x1010
    0xbb, 0x01, 0x00,       // mov bx, 1
    0xb6, 0x3f,             // mov dh, red
    0xb9, 0x00, 0x00,       // mov cx, 0
    0xcd, 0x10,             // int 10h: set DAC[1] to red
    0xba, 0xc6, 0x03,       // mov dx, 0x03c6
    0xb0, 0x0f,             // mov al, 0x0f
    0xee,                   // out dx, al: PEL mask = 0x0f
    0xec, 0xec, 0xec, 0xec, // four reads unlock hidden DAC command register
    0xb0, 0x8e,             // mov al, 0x8e
    0xee,                   // out dx, al: hidden DAC command, not PEL mask
    0xec, 0xec, 0xec, 0xec, // leave command register unlocked while saving video state
    0xb8, 0x01, 0x1c,       // mov ax, 0x1c01
    0xbb, 0x00, 0x06,       // mov bx, 0x0600
    0xb9, 0xff, 0xff,       // mov cx, 0xffff
    0xcd, 0x10,             // int 10h: save video state; must save actual PEL mask 0x0f
    0xba, 0xc8, 0x03,       // mov dx, 0x03c8
    0x30, 0xc0,             // xor al, al
    0xee,                   // out dx, al: reset DAC command unlock sequence
    0xba, 0xc6, 0x03,       // mov dx, 0x03c6
    0xb0, 0xff,             // mov al, 0xff
    0xee,                   // out dx, al: overwrite PEL mask
    0xb8, 0x02, 0x1c,       // mov ax, 0x1c02
    0xbb, 0x00, 0x06,       // mov bx, 0x0600
    0xb9, 0xff, 0xff,       // mov cx, 0xffff
    0xcd, 0x10,             // int 10h: restore PEL mask and DAC command state
    0xb8, 0x00, 0xa0,       // mov ax, 0xa000
    0x8e, 0xc0,             // mov es, ax
    0x31, 0xff,             // xor di, di
    0xb0, 0x11,             // mov al, 0x11
    0xaa,                   // stosb: with restored mask 0x0f, displays through DAC[1]
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, videoStateSavesPelMaskWhileDacCommandUnlockedProgram, sizeof(videoStateSavesPelMaskWhileDacCommandUnlockedProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(160);
  assert(machine.cpuHalted());
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  PcI8086::setAX(0x0013);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x1010);
  PcI8086::setBX(0x0044);
  PcI8086::setDH(0x3f);
  PcI8086::setCH(0x00);
  PcI8086::setCL(0x00);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x1c01);
  PcI8086::setBX(0x0600);
  PcI8086::setCX(0x0001); // save hardware registers only, not DAC palette
  PcI8086::setES(0x0000);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::AL() == 0x1c);
  PcI8086::setAX(0x1010);
  PcI8086::setBX(0x0044);
  PcI8086::setDH(0x00);
  PcI8086::setCH(0x00);
  PcI8086::setCL(0x3f);
  PcI8086::triggerInterrupt(0x10);
  PcI8086::setAX(0x1c02);
  PcI8086::setBX(0x0600);
  PcI8086::setCX(0x0001); // restoring hardware state must leave DAC[44h] blue
  PcI8086::setES(0x0000);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::AL() == 0x1c);
  PcI8086::setAX(0x1015);
  PcI8086::setBX(0x0044);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::DH() == 0x00);
  assert(PcI8086::CH() == 0x00);
  assert(PcI8086::CL() == 0x3f);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  PcI8086::setAX(0x0013);
  PcI8086::triggerInterrupt(0x10);
  machine.writePort(0x03c8, 0xa3);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x51);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x25);
  machine.writePort(0x03c0, 0x0a); // AC palette changes are preserved but bypassed in 8-bit PEL mode
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x21);
  machine.writePort(0x03c0, 0x03); // pixel 51h still selects DAC entry 51h directly
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase, 0x51);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  PcI8086::setAX(0x1c01);
  PcI8086::setBX(0x0600);
  PcI8086::setCX(0xffff);
  PcI8086::setES(0x0000);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::AL() == 0x1c);
  machine.writePort(0x03c8, 0xa3);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x25);
  machine.writePort(0x03c0, 0x05);
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x21);
  machine.writePort(0x03c0, 0x01);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  PcI8086::setAX(0x1c02);
  PcI8086::setBX(0x0600);
  PcI8086::setCX(0xffff);
  PcI8086::setES(0x0000);
  PcI8086::triggerInterrupt(0x10);
  assert(PcI8086::AL() == 0x1c);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const videoStateRestoresVgaLatchesProgram[] = {
    0xb8, 0x12, 0x00,       // mov ax, 0x0012
    0xcd, 0x10,             // int 10h: set VGA 640x480x16
    0xba, 0xc4, 0x03,       // mov dx, 0x03c4
    0xb0, 0x02,             // mov al, 2: sequencer map mask index
    0xee,                   // out dx, al
    0x42,                   // inc dx: 0x03c5
    0xb0, 0x01,             // mov al, 1: plane 0 only
    0xee,                   // out dx, al
    0xba, 0xce, 0x03,       // mov dx, 0x03ce
    0xb0, 0x04,             // mov al, 4: read map select
    0xee,                   // out dx, al
    0x42,                   // inc dx: 0x03cf
    0xb0, 0x00,             // mov al, 0: plane 0
    0xee,                   // out dx, al
    0xba, 0xce, 0x03,       // mov dx, 0x03ce
    0xb0, 0x08,             // mov al, 8: bit mask
    0xee,                   // out dx, al
    0x42,                   // inc dx
    0xb0, 0xff,             // mov al, 0xff
    0xee,                   // out dx, al
    0xb8, 0x00, 0xa0,       // mov ax, 0xa000
    0x8e, 0xc0,             // mov es, ax
    0x31, 0xff,             // xor di, di
    0xb0, 0xaa,             // mov al, 0xaa
    0xaa,                   // stosb: plane0[0]=0xaa
    0x26, 0xa0, 0x00, 0x00, // mov al, es:[0]: latch=0xaa
    0xb8, 0x01, 0x1c,       // mov ax, 0x1c01
    0xbb, 0x00, 0x06,       // mov bx, 0x0600
    0xb9, 0xff, 0xff,       // mov cx, 0xffff
    0xcd, 0x10,             // int 10h: save video state including latch
    0xbf, 0x01, 0x00,       // mov di, 1
    0xb0, 0x55,             // mov al, 0x55
    0xaa,                   // stosb: plane0[1]=0x55
    0x26, 0xa0, 0x01, 0x00, // mov al, es:[1]: corrupt latch=0x55
    0xb8, 0x02, 0x1c,       // mov ax, 0x1c02
    0xbb, 0x00, 0x06,       // mov bx, 0x0600
    0xb9, 0xff, 0xff,       // mov cx, 0xffff
    0xcd, 0x10,             // int 10h: restore video state and latch
    0xba, 0xce, 0x03,       // mov dx, 0x03ce
    0xb0, 0x08,             // mov al, 8: bit mask
    0xee,                   // out dx, al
    0x42,                   // inc dx
    0xb0, 0x0f,             // mov al, 0x0f: low nibble from CPU, high nibble from latch
    0xee,                   // out dx, al
    0x31, 0xff,             // xor di, di
    0xb0, 0x00,             // mov al, 0
    0xaa,                   // stosb: should leave high nibble from restored latch 0xaa -> 0xa0
    0x26, 0xa0, 0x00, 0x00, // mov al, es:[0]
    0xa2, 0x00, 0x05,       // mov [0x0500], al
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, videoStateRestoresVgaLatchesProgram, sizeof(videoStateRestoresVgaLatchesProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(180);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0xa0);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const videoStateRestoresDacCursorProgram[] = {
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: set VGA 320x200x256
    0xba, 0xc8, 0x03,       // mov dx, 0x03c8
    0xb0, 0x05,             // mov al, 5
    0xee,                   // out dx, al: DAC write index = 5
    0x42,                   // inc dx: 0x03c9
    0xb0, 0x11,             // mov al, 0x11
    0xee,                   // out dx, al: write red component only
    0xb8, 0x01, 0x1c,       // mov ax, 0x1c01
    0xbb, 0x00, 0x06,       // mov bx, 0x0600
    0xb9, 0xff, 0xff,       // mov cx, 0xffff
    0xcd, 0x10,             // int 10h: save video state with partial DAC write cursor
    0xba, 0xc8, 0x03,       // mov dx, 0x03c8
    0x30, 0xc0,             // xor al, al
    0xee,                   // out dx, al: corrupt current DAC write cursor
    0x42,                   // inc dx
    0xb0, 0x3e,             // mov al, 0x3e
    0xee,                   // out dx, al: corrupt DAC entry 0
    0xb8, 0x02, 0x1c,       // mov ax, 0x1c02
    0xbb, 0x00, 0x06,       // mov bx, 0x0600
    0xb9, 0xff, 0xff,       // mov cx, 0xffff
    0xcd, 0x10,             // int 10h: restore video state and DAC cursor
    0xba, 0xc9, 0x03,       // mov dx, 0x03c9
    0xb0, 0x22,             // mov al, 0x22
    0xee,                   // out dx, al: should continue with DAC[5].green
    0xb0, 0x33,             // mov al, 0x33
    0xee,                   // out dx, al: should continue with DAC[5].blue
    0xb8, 0x15, 0x10,       // mov ax, 0x1015
    0xbb, 0x05, 0x00,       // mov bx, 5
    0xcd, 0x10,             // int 10h: read DAC entry 5
    0x89, 0x16, 0x00, 0x05, // mov [0x0500], dx (DH=R)
    0x89, 0x0e, 0x02, 0x05, // mov [0x0502], cx (CH=G, CL=B)
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, videoStateRestoresDacCursorProgram, sizeof(videoStateRestoresDacCursorProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(128);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0501) == 0x11);
  assert(machine.readMemory8(0x0503) == 0x22);
  assert(machine.readMemory8(0x0502) == 0x33);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const videoStateRestoresAttributePhaseProgram[] = {
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: set VGA 320x200x256
    0xba, 0xda, 0x03,       // mov dx, 0x03da
    0xec,                   // in al, dx: reset attribute flip-flop to index phase
    0xb8, 0x01, 0x1c,       // mov ax, 0x1c01
    0xbb, 0x00, 0x06,       // mov bx, 0x0600
    0xb9, 0xff, 0xff,       // mov cx, 0xffff
    0xcd, 0x10,             // int 10h: save video state with AC in index phase
    0xb8, 0x02, 0x1c,       // mov ax, 0x1c02
    0xbb, 0x00, 0x06,       // mov bx, 0x0600
    0xb9, 0xff, 0xff,       // mov cx, 0xffff
    0xcd, 0x10,             // int 10h: restore AC flip-flop phase
    0xba, 0xc0, 0x03,       // mov dx, 0x03c0
    0xb0, 0x21,             // mov al, 0x21: palette register 1, PAS set
    0xee,                   // out dx, al: must be interpreted as index
    0xb0, 0x02,             // mov al, 0x02
    0xee,                   // out dx, al: data for palette register 1
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, videoStateRestoresAttributePhaseProgram, sizeof(videoStateRestoresAttributePhaseProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(96);
  assert(machine.cpuHalted());
  machine.readPort(0x03da);
  machine.writePort(0x03c0, 0x21);
  assert(machine.readPort(0x03c1) == 0x02);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const videoStateRestoresModeProgram[] = {
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: set VGA 320x200x256
    0xb4, 0x0c,             // mov ah, 0x0c
    0xb0, 0x07,             // mov al, 7
    0xb9, 0x02, 0x00,       // mov cx, 2
    0xba, 0x03, 0x00,       // mov dx, 3
    0xcd, 0x10,             // int 10h: write pixel
    0xb8, 0x01, 0x1c,       // mov ax, 0x1c01
    0xbb, 0x00, 0x06,       // mov bx, 0x0600
    0xb9, 0xff, 0xff,       // mov cx, 0xffff
    0xcd, 0x10,             // int 10h: save video state
    0xb8, 0x03, 0x00,       // mov ax, 0x0003
    0xcd, 0x10,             // int 10h: switch away to text mode
    0xb8, 0x02, 0x1c,       // mov ax, 0x1c02
    0xbb, 0x00, 0x06,       // mov bx, 0x0600
    0xb9, 0xff, 0xff,       // mov cx, 0xffff
    0xcd, 0x10,             // int 10h: restore video state
    0xb4, 0x0d,             // mov ah, 0x0d
    0xb7, 0x00,             // mov bh, page 0
    0xb9, 0x02, 0x00,       // mov cx, 2
    0xba, 0x03, 0x00,       // mov dx, 3
    0xcd, 0x10,             // int 10h: read restored graphics pixel
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, videoStateRestoresModeProgram, sizeof(videoStateRestoresModeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(128);
  assert(machine.cpuHalted());
  assert(machine.videoMode() == PcMachine::VideoMode::VgaGraphics320x200x256);
  assert(machine.readMemory8(0x0449) == 0x13);
  assert((machine.readMemory16(0x0500) & 0x00ff) == 0x07);
  machine.renderGraphicsLine(3, graphicsLine.data());
  assert(graphicsLine[2] != 0x0000);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const vbeProbeProgram[] = {
    0xb8, 0x00, 0x4f,       // mov ax, 0x4f00
    0xcd, 0x10,             // int 10h: VBE controller info probe
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0xb8, 0x02, 0x4f,       // mov ax, 0x4f02
    0xbb, 0x01, 0x01,       // mov bx, 0x0101
    0xcd, 0x10,             // int 10h: VBE set mode probe
    0xa3, 0x02, 0x05,       // mov [0x0502], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, vbeProbeProgram, sizeof(vbeProbeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(32);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x014f);
  assert(machine.readMemory16(0x0502) == 0x014f);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const videoOemProbeProgram[] = {
    0xb8, 0x00, 0xfa,       // mov ax, 0xfa00
    0xcd, 0x10,             // int 10h: OEM/video extension probe
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, videoOemProbeProgram, sizeof(videoOemProbeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const videoOemProbeFeProgram[] = {
    0xb8, 0x00, 0xfe,       // mov ax, 0xfe00
    0xcd, 0x10,             // int 10h: OEM/video extension probe
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, videoOemProbeFeProgram, sizeof(videoOemProbeFeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const activeDisplayPageProgram[] = {
    0xb8, 0x02, 0x05,       // mov ax, 0x0502
    0xcd, 0x10,             // int 10h: set active display page 2
    0xb4, 0x0e,             // mov ah, 0x0e
    0xb0, 0x51,             // mov al, 'Q'
    0xcd, 0x10,             // int 10h: teletype on active page
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, activeDisplayPageProgram, sizeof(activeDisplayPageProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0462) == 2);
  assert(machine.readMemory16(0x044e) == 0x2000);
  machine.writePort(0x03d4, 0x0c);
  assert(machine.readPort(0x03d5) == 0x10);
  machine.writePort(0x03d4, 0x0d);
  assert(machine.readPort(0x03d5) == 0x00);
  assert((machine.readVideoMemory16(PcMachine::TextColorMemoryBase + 0x2000) & 0x00ff) == 'Q');
  assert(machine.text80Buffer()[0] == 'Q');
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  PcI8086::setAX(0x0013);
  PcI8086::triggerInterrupt(0x10);
  assert(machine.videoMode() == PcMachine::VideoMode::VgaGraphics320x200x256);
  machine.writePort(0x03c8, 0x01);
  machine.writePort(0x03c9, 0x3f);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c8, 0x02);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x00);
  machine.writePort(0x03c9, 0x3f);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 0xfa00, 0x01);
  machine.writeVideoMemory8(PcMachine::VgaGraphicsMemoryBase + 0xe800, 0x02);
  PcI8086::setAX(0x0501);
  PcI8086::triggerInterrupt(0x10);
  assert(machine.readMemory8(0x0462) == 1);
  assert(machine.readMemory16(0x044e) == 0xfa00);
  machine.writePort(0x03d4, 0x0c);
  assert(machine.readPort(0x03d5) == 0x3e);
  machine.writePort(0x03d4, 0x0d);
  assert(machine.readPort(0x03d5) == 0x80);
  std::fill(graphicsLine.begin(), graphicsLine.end(), 0);
  machine.renderGraphicsLine(0, graphicsLine.data());
  assert(graphicsLine[0] == 0xf800); // page 1 begins at byte 0xfa00, not raw CRTC 0xfa00 * 4 wraparound
  machine.writePort(0x03d4, 0x0e);
  machine.writePort(0x03d5, 0x12);
  machine.writePort(0x03d4, 0x0f);
  machine.writePort(0x03d5, 0x34);
  PcI8086::setAX(0x0003);
  PcI8086::triggerInterrupt(0x10);
  assert(machine.videoMode() == PcMachine::VideoMode::Text80);
  assert(machine.readMemory8(0x0462) == 0);
  assert(machine.readMemory16(0x044e) == 0);
  machine.writePort(0x03d4, 0x0c);
  assert(machine.readPort(0x03d5) == 0x00);
  machine.writePort(0x03d4, 0x0d);
  assert(machine.readPort(0x03d5) == 0x00);
  machine.writePort(0x03d4, 0x0e);
  assert(machine.readPort(0x03d5) == 0x00);
  machine.writePort(0x03d4, 0x0f);
  assert(machine.readPort(0x03d5) == 0x00);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const getCurrentVideoModeProgram[] = {
    0xb8, 0x02, 0x05,       // mov ax, 0x0502
    0xcd, 0x10,             // int 10h: set active display page 2
    0xb4, 0x0f,             // mov ah, 0x0f
    0xcd, 0x10,             // int 10h: get current video mode
    0xa3, 0x00, 0x05,       // mov [0x0500], ax (AH=columns, AL=mode)
    0x89, 0x1e, 0x02, 0x05, // mov [0x0502], bx (BH=active page)
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, getCurrentVideoModeProgram, sizeof(getCurrentVideoModeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0x03);
  assert(machine.readMemory8(0x0501) == PcTextRenderer::Columns);
  assert(machine.readMemory8(0x0503) == 2);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const biosMode13PixelProgram[] = {
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: set 320x200x256
    0xb4, 0x0c,             // mov ah, 0x0c
    0xb0, 0x05,             // mov al, 5
    0xb9, 0x07, 0x00,       // mov cx, 7
    0xba, 0x08, 0x00,       // mov dx, 8
    0xcd, 0x10,             // int 10h: write pixel
    0xb4, 0x0d,             // mov ah, 0x0d
    0xb9, 0x07, 0x00,       // mov cx, 7
    0xba, 0x08, 0x00,       // mov dx, 8
    0xcd, 0x10,             // int 10h: read pixel
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, biosMode13PixelProgram, sizeof(biosMode13PixelProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(64);
  assert(machine.cpuHalted());
  assert((machine.readMemory16(0x0500) & 0x00ff) == 0x05);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const biosDacRegisterProgram[] = {
    0xb8, 0x13, 0x00,       // mov ax, 0x0013
    0xcd, 0x10,             // int 10h: set 320x200x256
    0xb8, 0x10, 0x10,       // mov ax, 0x1010: set individual DAC register
    0xbb, 0x02, 0x00,       // mov bx, 2
    0xb6, 0x15,             // mov dh, red
    0xb5, 0x2a,             // mov ch, green
    0xb1, 0x3f,             // mov cl, blue
    0xcd, 0x10,             // int 10h
    0xb8, 0x15, 0x10,       // mov ax, 0x1015: read individual DAC register
    0xbb, 0x02, 0x00,       // mov bx, 2
    0xcd, 0x10,             // int 10h
    0x89, 0x16, 0x00, 0x05, // mov [0x0500], dx (DH=red)
    0x89, 0x0e, 0x02, 0x05, // mov [0x0502], cx (CH=green, CL=blue)
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, biosDacRegisterProgram, sizeof(biosDacRegisterProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(128);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0501) == 0x15);
  assert(machine.readMemory8(0x0503) == 0x2a);
  assert(machine.readMemory8(0x0502) == 0x3f);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const biosMode12PixelProgram[] = {
    0xb8, 0x12, 0x00,       // mov ax, 0x0012
    0xcd, 0x10,             // int 10h: set 640x480x16
    0xb4, 0x0c,             // mov ah, 0x0c
    0xb0, 0x0e,             // mov al, 14
    0xb9, 0x09, 0x00,       // mov cx, 9
    0xba, 0x02, 0x00,       // mov dx, 2
    0xcd, 0x10,             // int 10h: write pixel
    0xb4, 0x0d,             // mov ah, 0x0d
    0xb9, 0x09, 0x00,       // mov cx, 9
    0xba, 0x02, 0x00,       // mov dx, 2
    0xcd, 0x10,             // int 10h: read pixel
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, biosMode12PixelProgram, sizeof(biosMode12PixelProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(64);
  assert(machine.cpuHalted());
  assert((machine.readMemory16(0x0500) & 0x00ff) == 0x0e);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const fpuProbeProgram[] = {
    0xdb, 0xe3,             // fninit: ignored when no 8087 is present
    0xd9, 0x3e, 0x58, 0x00, // fnstcw [0x0058]: ignored, preserving sentinel
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, fpuProbeProgram, sizeof(fpuProbeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.writeMemory16(0x0058, 0x5a5a);
  assert(machine.runCpuSteps(8) == 3);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0058) == 0x5a5a);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const eddCheckProgram[] = {
    0xb8, 0x00, 0x41,       // mov ax, 0x4100
    0xbb, 0xaa, 0x55,       // mov bx, 0x55aa
    0xba, 0x00, 0x00,       // mov dx, 0x0000 (fd0)
    0xcd, 0x13,             // int 13h: EDD installation check
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, eddCheckProgram, sizeof(eddCheckProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  cpuState = machine.cpuState();
  assert((cpuState.ax & 0xff00) == 0x2100);
  assert(cpuState.bx == 0xaa55);
  assert(cpuState.cx & 0x0001);
  assert(!PcI8086::flagCF());
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const eddReadProgram[] = {
    0xb8, 0x00, 0x42,       // mov ax, 0x4200
    0xbe, 0x00, 0x05,       // mov si, 0x0500 (DAP)
    0xba, 0x00, 0x00,       // mov dx, 0x0000 (fd0)
    0xcd, 0x13,             // int 13h: extended read
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, eddReadProgram, sizeof(eddReadProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.writeMemory8(0x0500, 0x10);      // DAP size
  machine.writeMemory8(0x0501, 0x00);
  machine.writeMemory16(0x0502, 0x0001);   // one sector
  machine.writeMemory16(0x0504, 0x0700);   // offset
  machine.writeMemory16(0x0506, 0x0000);   // segment
  machine.writeMemory16(0x0508, 0x0001);   // LBA 1
  machine.writeMemory16(0x050a, 0x0000);
  machine.writeMemory16(0x050c, 0x0000);
  machine.writeMemory16(0x050e, 0x0000);
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert(memcmp(machine.ram() + 0x0700, "INT13-SECTOR-2", 14) == 0);
  assert(!PcI8086::flagCF());
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const keyboardBootProgram[] = {
    0xb4, 0x00,             // mov ah, 0x00
    0xcd, 0x16,             // int 16h: read key
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, keyboardBootProgram, sizeof(keyboardBootProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  uint8_t keyReport[6] = {0x07, 0, 0, 0, 0, 0}; // HID d
  machine.keyboard().onHidBootKeyboardReport(0x02, keyReport); // left shift + d
  assert(machine.runCpuSteps(8) == 4);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x2044); // scan d, ASCII 'D'

  machine.reset();
  memset(sector, 0, sizeof(sector));
  memcpy(sector, keyboardBootProgram, sizeof(keyboardBootProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  keyReport[0] = 0x09; // HID f
  machine.keyboard().onHidBootKeyboardReport(0x04, keyReport); // left alt + f
  assert(machine.runCpuSteps(8) >= 4);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x2100); // Alt+F is an extended BIOS key

  machine.reset();
  memset(sector, 0, sizeof(sector));
  memcpy(sector, keyboardBootProgram, sizeof(keyboardBootProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  keyReport[0] = 0x29; // Escape
  machine.keyboard().onHidBootKeyboardReport(0, keyReport);
  assert(machine.runCpuSteps(8) >= 4);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x011b);

  machine.reset();
  memset(sector, 0, sizeof(sector));
  memcpy(sector, keyboardBootProgram, sizeof(keyboardBootProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  keyReport[0] = 0x51; // HID down arrow
  machine.keyboard().onHidBootKeyboardReport(0, keyReport);
  assert(machine.runCpuSteps(8) >= 4);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x5000);

  machine.reset();
  memset(sector, 0, sizeof(sector));
  memcpy(sector, keyboardBootProgram, sizeof(keyboardBootProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  assert(machine.runCpuSteps(4) == 4);
  assert(!machine.cpuHalted());
  assert(machine.cpuState().ip == 0x7c02); // INT 16h AH=00 blocks until a real key arrives
  keyReport[0] = 0x43; // HID F10
  machine.keyboard().onHidBootKeyboardReport(0, keyReport);
  machine.runCpuSteps(8);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x4400);

  machine.reset();
  uint8_t const keyboardOldHandlerCallProgram[] = {
    0x9a, 0x00, 0xf1, 0x00, 0xf0, // call far F000:F100 old INT 09h handler
    0xb8, 0x34, 0x12,             // mov ax, 0x1234
    0xa3, 0x00, 0x05,             // mov [0x0500], ax
    0xf4                          // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, keyboardOldHandlerCallProgram, sizeof(keyboardOldHandlerCallProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x1234);

  machine.reset();
  uint8_t const keyboardShiftFlagsProgram[] = {
    0xb4, 0x02,             // mov ah, 0x02
    0xcd, 0x16,             // int 16h: get shift flags
    0xa2, 0x00, 0x05,       // mov [0x0500], al
    0xb4, 0x12,             // mov ah, 0x12
    0xcd, 0x16,             // int 16h: get enhanced shift flags
    0xa3, 0x02, 0x05,       // mov [0x0502], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, keyboardShiftFlagsProgram, sizeof(keyboardShiftFlagsProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  uint8_t noKeys[6] = {};
  machine.keyboard().onHidBootKeyboardReport(0x04, noKeys); // left alt only
  assert(machine.runCpuSteps(16) >= 6);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) & 0x08);
  assert(machine.readMemory16(0x0502) & 0x0208);
  assert(machine.readMemory8(0x0417) & 0x08);

  machine.reset();
  uint8_t const keyboardExtensionProgram[] = {
    0xb4, 0x55,             // mov ah, 0x55
    0xcd, 0x16,             // int 16h: keyboard extension/private probe
    0xb4, 0x12,             // mov ah, 0x12
    0xcd, 0x16,             // int 16h: get enhanced shift flags
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, keyboardExtensionProgram, sizeof(keyboardExtensionProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x0000);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const mouseDriverProbeProgram[] = {
    0xb8, 0x00, 0x00,       // mov ax, 0x0000
    0xcd, 0x33,             // int 33h: reset/get installed flag
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0x89, 0x1e, 0x02, 0x05, // mov [0x0502], bx
    0xb8, 0x04, 0x00,       // mov ax, 0x0004
    0xb9, 0x7b, 0x00,       // mov cx, 123
    0xba, 0xc8, 0x00,       // mov dx, 200
    0xcd, 0x33,             // int 33h: set cursor position
    0xb8, 0x03, 0x00,       // mov ax, 0x0003
    0xcd, 0x33,             // int 33h: get buttons/position
    0x89, 0x1e, 0x04, 0x05, // mov [0x0504], bx
    0x89, 0x0e, 0x06, 0x05, // mov [0x0506], cx
    0x89, 0x16, 0x08, 0x05, // mov [0x0508], dx
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, mouseDriverProbeProgram, sizeof(mouseDriverProbeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(32);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x0000);
  assert(machine.readMemory16(0x0502) == 0x0000);
  assert(machine.readMemory16(0x0504) == 0x0000);
  assert(machine.readMemory16(0x0506) == 123);
  assert(machine.readMemory16(0x0508) == 199);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const installedMouseProgram[] = {
    0xb8, 0x00, 0x00,       // mov ax, 0x0000
    0xcd, 0x33,             // int 33h: reset/get installed flag
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0x89, 0x1e, 0x02, 0x05, // mov [0x0502], bx
    0xb8, 0x07, 0x00,       // mov ax, 0x0007
    0xb9, 0x00, 0x00,       // mov cx, 0
    0xba, 0x7f, 0x02,       // mov dx, 639
    0xcd, 0x33,             // int 33h: set horizontal range
    0xb8, 0x08, 0x00,       // mov ax, 0x0008
    0xb9, 0x00, 0x00,       // mov cx, 0
    0xba, 0xdf, 0x01,       // mov dx, 479
    0xcd, 0x33,             // int 33h: set vertical range
    0xb8, 0x03, 0x00,       // mov ax, 0x0003
    0xcd, 0x33,             // int 33h: get buttons/position
    0x89, 0x1e, 0x04, 0x05, // mov [0x0504], bx
    0x89, 0x0e, 0x06, 0x05, // mov [0x0506], cx
    0x89, 0x16, 0x08, 0x05, // mov [0x0508], dx
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, installedMouseProgram, sizeof(installedMouseProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.setMouseInstalled(true);
  machine.setMouseState(320, 240, 1);
  machine.runCpuSteps(48);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0xffff);
  assert(machine.readMemory16(0x0502) == 0x0002);
  assert(machine.readMemory16(0x0504) == 0x0000); // reset clears stale button state
  assert(machine.readMemory16(0x0506) == 0);
  assert(machine.readMemory16(0x0508) == 0);
  machine.setMouseState(400, 300, 1);

  machine.reset();
  uint8_t const mouseButtonEventProgram[] = {
    0xbb, 0x00, 0x00,       // mov bx, 0 (left button)
    0xb8, 0x05, 0x00,       // mov ax, 0x0005
    0xcd, 0x33,             // int 33h: get button press info
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0x89, 0x1e, 0x02, 0x05, // mov [0x0502], bx
    0x89, 0x0e, 0x04, 0x05, // mov [0x0504], cx
    0x89, 0x16, 0x06, 0x05, // mov [0x0506], dx
    0xbb, 0x00, 0x00,       // mov bx, 0 (left button)
    0xb8, 0x06, 0x00,       // mov ax, 0x0006
    0xcd, 0x33,             // int 33h: get button release info
    0xa3, 0x08, 0x05,       // mov [0x0508], ax
    0x89, 0x1e, 0x0a, 0x05, // mov [0x050a], bx
    0x89, 0x0e, 0x0c, 0x05, // mov [0x050c], cx
    0x89, 0x16, 0x0e, 0x05, // mov [0x050e], dx
    0xb8, 0x0b, 0x00,       // mov ax, 0x000b
    0xcd, 0x33,             // int 33h: read motion counters
    0x89, 0x0e, 0x10, 0x05, // mov [0x0510], cx
    0x89, 0x16, 0x12, 0x05, // mov [0x0512], dx
    0xb8, 0x0b, 0x00,       // mov ax, 0x000b
    0xcd, 0x33,             // int 33h: counters are reset after read
    0x89, 0x0e, 0x14, 0x05, // mov [0x0514], cx
    0x89, 0x16, 0x16, 0x05, // mov [0x0516], dx
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, mouseButtonEventProgram, sizeof(mouseButtonEventProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.setMouseState(10, 20, 0);
  machine.setMouseState(111, 22, 1);
  machine.setMouseState(123, 45, 0);
  machine.runCpuSteps(96);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x0000);
  assert(machine.readMemory16(0x0502) == 0x0001);
  assert(machine.readMemory16(0x0504) == 111);
  assert(machine.readMemory16(0x0506) == 22);
  assert(machine.readMemory16(0x0508) == 0x0000);
  assert(machine.readMemory16(0x050a) == 0x0001);
  assert(machine.readMemory16(0x050c) == 123);
  assert(machine.readMemory16(0x050e) == 45);
  assert(machine.readMemory16(0x0510) == 123);
  assert(machine.readMemory16(0x0512) == 45);
  assert(machine.readMemory16(0x0514) == 0);
  assert(machine.readMemory16(0x0516) == 0);

  machine.reset();
  uint8_t const mouseCallbackProgram[] = {
    0xb8, 0x0c, 0x00,       // mov ax, 0x000c
    0xb9, 0x07, 0x00,       // mov cx, move + left press + left release events
    0xba, 0x00, 0x06,       // mov dx, 0x0600
    0xcd, 0x33,             // int 33h: define user callback at ES:DX
    0xeb, 0xfe              // jmp $; callback returns here
  };
  uint8_t const mouseCallbackHandler[] = {
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0x89, 0x1e, 0x02, 0x05, // mov [0x0502], bx
    0x89, 0x0e, 0x04, 0x05, // mov [0x0504], cx
    0x89, 0x16, 0x06, 0x05, // mov [0x0506], dx
    0x89, 0x36, 0x08, 0x05, // mov [0x0508], si
    0x89, 0x3e, 0x0a, 0x05, // mov [0x050a], di
    0xcb                    // retf
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, mouseCallbackProgram, sizeof(mouseCallbackProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  memcpy(machine.ram() + 0x0600, mouseCallbackHandler, sizeof(mouseCallbackHandler));
  machine.setMouseInstalled(true);
  machine.runCpuSteps(12); // register callback and enter the polling loop
  machine.setMouseState(20, 30, 1);
  machine.runCpuSteps(32);
  assert(machine.readMemory16(0x0500) == 0x0003); // move + left press
  assert(machine.readMemory16(0x0502) == 0x0001);
  assert(machine.readMemory16(0x0504) == 20);
  assert(machine.readMemory16(0x0506) == 30);
  assert(machine.readMemory16(0x0508) == 20);
  assert(machine.readMemory16(0x050a) == 30);

  machine.reset();
  uint8_t const mouseIretCallbackProgram[] = {
    0xb8, 0x0c, 0x00,       // mov ax, 0x000c
    0xb9, 0x07, 0x00,       // mov cx, move + left press + left release events
    0xba, 0x00, 0x06,       // mov dx, 0x0600
    0xcd, 0x33,             // int 33h: define user callback at ES:DX
    0xeb, 0xfe              // jmp $; callback returns here
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, mouseIretCallbackProgram, sizeof(mouseIretCallbackProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.writeMemory8(0x0600, 0xcf); // IRET-only callback stub used by some DOS mouse clients
  machine.setMouseInstalled(true);
  machine.runCpuSteps(12); // register callback and enter the polling loop
  uint16_t const spBeforeIretCallback = machine.cpuState().sp;
  machine.setMouseState(40, 50, 1);
  machine.runCpuSteps(16);
  assert(machine.cpuState().sp == spBeforeIretCallback);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const systemConfigurationProgram[] = {
    0xb8, 0x00, 0xc0,       // mov ax, 0xc000
    0xcd, 0x15,             // int 15h: get system configuration
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, systemConfigurationProgram, sizeof(systemConfigurationProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  cpuState = machine.cpuState();
  assert((cpuState.ax & 0xff00) == 0x0000);
  assert(cpuState.bx == 0xe6f5);
  assert(cpuState.es == 0xf000);
  assert(machine.readMemory16(0x000fe6f5) == 0x0008);
  assert(machine.readMemory8(0x000fe6f7) == 0xfc);
  assert(!PcI8086::flagCF());
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const unsupportedPointingDeviceProgram[] = {
    0xb4, 0xc2,             // mov ah, 0xc2
    0xcd, 0x15,             // int 15h: PS/2 pointing-device BIOS probe
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, unsupportedPointingDeviceProgram, sizeof(unsupportedPointingDeviceProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0501) == 0x86);
  assert(PcI8086::flagCF());
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const unsupportedSystemExtensionProgram[] = {
    0xb4, 0xd8,             // mov ah, 0xd8
    0xcd, 0x15,             // int 15h: system/bus extension probe
    0xa3, 0x00, 0x05,       // mov [0x0500], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, unsupportedSystemExtensionProgram, sizeof(unsupportedSystemExtensionProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0501) == 0x86);
  assert(PcI8086::flagCF());
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const legacySystemProbeProgram[] = {
    0xb4, 0x06,             // mov ah, 06h: legacy/private INT 15h probe
    0xcd, 0x15,             // int 15h
    0x72, 0x07,             // jc unsupported
    0xc6, 0x06, 0x00, 0x05, 0x00, // mov byte [0500h], 00h
    0xeb, 0x09,             // jmp done
    0x88, 0x26, 0x00, 0x05, // unsupported: mov [0500h], ah
    0xc6, 0x06, 0x01, 0x05, 0x01, // mov byte [0501h], 01h
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, legacySystemProbeProgram, sizeof(legacySystemProbeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(24);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0x86);
  assert(machine.readMemory8(0x0501) == 0x01);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const legacySystem11ProbeProgram[] = {
    0xb4, 0x11,             // mov ah, 11h: legacy/system extension probe
    0xcd, 0x15,             // int 15h
    0x72, 0x07,             // jc unsupported
    0xc6, 0x06, 0x00, 0x05, 0x00, // mov byte [0500h], 00h
    0xeb, 0x09,             // jmp done
    0x88, 0x26, 0x00, 0x05, // unsupported: mov [0500h], ah
    0xc6, 0x06, 0x01, 0x05, 0x01, // mov byte [0501h], 01h
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, legacySystem11ProbeProgram, sizeof(legacySystem11ProbeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(24);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0x86);
  assert(machine.readMemory8(0x0501) == 0x01);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const system41ProbeProgram[] = {
    0xb8, 0x01, 0x41,       // mov ax, 4101h: MS-DOS IO.SYS system/event probe
    0xcd, 0x15,             // int 15h
    0x72, 0x07,             // jc unsupported
    0xc6, 0x06, 0x00, 0x05, 0x00, // mov byte [0500h], 00h
    0xeb, 0x09,             // jmp done
    0x88, 0x26, 0x00, 0x05, // unsupported: mov [0500h], ah
    0xc6, 0x06, 0x01, 0x05, 0x01, // mov byte [0501h], 01h
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, system41ProbeProgram, sizeof(system41ProbeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(24);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0x86);
  assert(machine.readMemory8(0x0501) == 0x01);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const oemSystem64ProbeProgram[] = {
    0xb4, 0x64,             // mov ah, 64h: OEM/system extension probe
    0xcd, 0x15,             // int 15h
    0x72, 0x07,             // jc unsupported
    0xc6, 0x06, 0x00, 0x05, 0x00, // mov byte [0500h], 00h
    0xeb, 0x09,             // jmp done
    0x88, 0x26, 0x00, 0x05, // unsupported: mov [0500h], ah
    0xc6, 0x06, 0x01, 0x05, 0x01, // mov byte [0501h], 01h
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, oemSystem64ProbeProgram, sizeof(oemSystem64ProbeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(24);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0x86);
  assert(machine.readMemory8(0x0501) == 0x01);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const extendedMemorySizeProgram[] = {
    0xb4, 0x88,             // mov ah, 88h: get extended memory size
    0xcd, 0x15,             // int 15h
    0xa3, 0x00, 0x05,       // mov [0500h], ax
    0x9c,                   // pushf
    0x58,                   // pop ax
    0xa3, 0x02, 0x05,       // mov [0502h], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, extendedMemorySizeProgram, sizeof(extendedMemorySizeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x0000);
  assert((machine.readMemory16(0x0502) & 0x0001) == 0x0000);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const extendedMemoryE801Program[] = {
    0xb8, 0x01, 0xe8,       // mov ax, E801h: get extended memory size
    0xcd, 0x15,             // int 15h
    0xa3, 0x00, 0x05,       // mov [0500h], ax
    0x89, 0x1e, 0x02, 0x05, // mov [0502h], bx
    0x89, 0x0e, 0x04, 0x05, // mov [0504h], cx
    0x89, 0x16, 0x06, 0x05, // mov [0506h], dx
    0x9c,                   // pushf
    0x58,                   // pop ax
    0xa3, 0x08, 0x05,       // mov [0508h], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, extendedMemoryE801Program, sizeof(extendedMemoryE801Program));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(32);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x0000);
  assert(machine.readMemory16(0x0502) == 0x0000);
  assert(machine.readMemory16(0x0504) == 0x0000);
  assert(machine.readMemory16(0x0506) == 0x0000);
  assert((machine.readMemory16(0x0508) & 0x0001) == 0x0000);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);


  machine.reset();
  PcI8086::setAX(0xe820);
  PcI8086::setBX(0x0000);
  PcI8086::setCX(20);
  PcI8086::setDX(0x4150); // low word of SMAP in this 16-bit CPU core
  PcI8086::setES(0x0000);
  PcI8086::setDI(0x0600);
  PcI8086::triggerInterrupt(0x15);
  assert(!PcI8086::flagCF());
  assert(PcI8086::AX() == 0x4150);
  assert(PcI8086::BX() == 0x0001);
  assert(PcI8086::CX() == 20);
  assert(machine.readMemory16(0x0600) == 0x0000);
  assert(machine.readMemory16(0x0602) == 0x0000);
  assert(machine.readMemory16(0x0608) == 0x0000);
  assert(machine.readMemory16(0x060a) == 0x000a); // 640 KiB usable conventional RAM
  assert(machine.readMemory16(0x0610) == 0x0001);
  assert(machine.readMemory16(0x0612) == 0x0000);

  PcI8086::setAX(0xe820);
  PcI8086::setBX(0x0001);
  PcI8086::setCX(20);
  PcI8086::setDX(0x4150);
  PcI8086::setES(0x0000);
  PcI8086::setDI(0x0620);
  PcI8086::triggerInterrupt(0x15);
  assert(!PcI8086::flagCF());
  assert(PcI8086::AX() == 0x4150);
  assert(PcI8086::BX() == 0x0000);
  assert(PcI8086::CX() == 20);
  assert(machine.readMemory16(0x0620) == 0x0000);
  assert(machine.readMemory16(0x0622) == 0x000a); // reserved area starts at A0000h
  assert(machine.readMemory16(0x0628) == 0x0000);
  assert(machine.readMemory16(0x062a) == 0x0006); // 384 KiB VGA/BIOS reserved area
  assert(machine.readMemory16(0x0630) == 0x0002);
  assert(machine.readMemory16(0x0632) == 0x0000);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  PcI8086::setAX(0x2403);
  PcI8086::setBX(0xffff);
  PcI8086::triggerInterrupt(0x15);
  assert(!PcI8086::flagCF());
  assert((PcI8086::AX() & 0xff00) == 0x0000);
  assert(PcI8086::BX() == 0x0000);

  PcI8086::setAX(0x2402);
  PcI8086::triggerInterrupt(0x15);
  assert(!PcI8086::flagCF());
  assert(PcI8086::AX() == 0x0000);

  PcI8086::setAX(0x2400);
  PcI8086::triggerInterrupt(0x15);
  assert(!PcI8086::flagCF());
  assert((PcI8086::AX() & 0xff00) == 0x0000);

  PcI8086::setAX(0x2401);
  PcI8086::triggerInterrupt(0x15);
  assert(PcI8086::flagCF());
  assert((PcI8086::AX() & 0xff00) == 0x8600);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const ebdaSegmentProbeProgram[] = {
    0xb4, 0xc1,             // mov ah, C1h: get EBDA segment
    0xcd, 0x15,             // int 15h
    0x72, 0x07,             // jc unsupported
    0xc6, 0x06, 0x00, 0x05, 0x00, // mov byte [0500h], 00h
    0xeb, 0x09,             // jmp done
    0x88, 0x26, 0x00, 0x05, // unsupported: mov [0500h], ah
    0xc6, 0x06, 0x01, 0x05, 0x01, // mov byte [0501h], 01h
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, ebdaSegmentProbeProgram, sizeof(ebdaSegmentProbeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(24);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0x86);
  assert(machine.readMemory8(0x0501) == 0x01);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const videoBiosExtensionProbeProgram[] = {
    0xb4, 0x30,             // mov ah, 30h: vendor/extension video BIOS probe
    0xcd, 0x10,             // int 10h
    0xa3, 0x00, 0x05,       // mov [0500h], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, videoBiosExtensionProbeProgram, sizeof(videoBiosExtensionProbeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x3000);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const oemVideoProbeProgram[] = {
    0xb4, 0xff,             // mov ah, FFh: OEM/video BIOS extension probe
    0xcd, 0x10,             // int 10h
    0xa3, 0x00, 0x05,       // mov [0500h], ax
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, oemVideoProbeProgram, sizeof(oemVideoProbeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(16);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0xff00);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const oemClockProbeProgram[] = {
    0xb4, 0x35,             // mov ah, 35h: OEM/BIOS clock extension probe
    0xcd, 0x1a,             // int 1Ah
    0x72, 0x07,             // jc unsupported
    0xc6, 0x06, 0x00, 0x05, 0x00, // mov byte [0500h], 00h
    0xeb, 0x09,             // jmp done
    0x88, 0x26, 0x00, 0x05, // unsupported: mov [0500h], ah
    0xc6, 0x06, 0x01, 0x05, 0x01, // mov byte [0501h], 01h
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, oemClockProbeProgram, sizeof(oemClockProbeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(24);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0x86);
  assert(machine.readMemory8(0x0501) == 0x01);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const oemClock36ProbeProgram[] = {
    0xb4, 0x36,             // mov ah, 36h: OEM/BIOS clock extension probe
    0xcd, 0x1a,             // int 1Ah
    0x72, 0x07,             // jc unsupported
    0xc6, 0x06, 0x00, 0x05, 0x00, // mov byte [0500h], 00h
    0xeb, 0x09,             // jmp done
    0x88, 0x26, 0x00, 0x05, // unsupported: mov [0500h], ah
    0xc6, 0x06, 0x01, 0x05, 0x01, // mov byte [0501h], 01h
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, oemClock36ProbeProgram, sizeof(oemClock36ProbeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(24);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0x86);
  assert(machine.readMemory8(0x0501) == 0x01);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const oemClock60ProbeProgram[] = {
    0xb4, 0x60,             // mov ah, 60h: OEM/BIOS clock extension probe
    0xcd, 0x1a,             // int 1Ah
    0x72, 0x07,             // jc unsupported
    0xc6, 0x06, 0x00, 0x05, 0x00, // mov byte [0500h], 00h
    0xeb, 0x09,             // jmp done
    0x88, 0x26, 0x00, 0x05, // unsupported: mov [0500h], ah
    0xc6, 0x06, 0x01, 0x05, 0x01, // mov byte [0501h], 01h
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, oemClock60ProbeProgram, sizeof(oemClock60ProbeProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(24);
  assert(machine.cpuHalted());
  assert(machine.readMemory8(0x0500) == 0x86);
  assert(machine.readMemory8(0x0501) == 0x01);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  machine.reset();
  uint8_t const writeBootProgram[] = {
    0xb8, 0x44, 0x4f,       // mov ax, 'DO'
    0xa3, 0x00, 0x07,       // mov [0x0700], ax
    0xb8, 0x53, 0x57,       // mov ax, 'SW'
    0xa3, 0x02, 0x07,       // mov [0x0702], ax
    0xb8, 0x52, 0x49,       // mov ax, 'RI'
    0xa3, 0x04, 0x07,       // mov [0x0704], ax
    0xb8, 0x54, 0x45,       // mov ax, 'TE'
    0xa3, 0x06, 0x07,       // mov [0x0706], ax
    0xb8, 0x01, 0x03,       // mov ax, 0x0301 (write one sector)
    0xbb, 0x00, 0x07,       // mov bx, 0x0700
    0xb9, 0x03, 0x00,       // mov cx, 0x0003 (cyl 0, sector 3)
    0xba, 0x00, 0x00,       // mov dx, 0x0000 (head 0, fd0)
    0xcd, 0x13,             // int 13h: write ES:BX to CHS sector
    0xf4                    // hlt
  };
  memset(sector, 0, sizeof(sector));
  memcpy(sector, writeBootProgram, sizeof(writeBootProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(0)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(0, 0x00));
  assert(machine.prepareBootCpu());
  assert(machine.runCpuSteps(32) == 14);
  assert(machine.cpuHalted());
  assert(machine.diagnostics().diskWriteCount == 1);
  assert(machine.diagnostics().lastDiskWriteDrive == 0x00);
  assert(machine.diagnostics().lastDiskWriteLba == 2);
  assert(machine.diagnostics().lastDiskWriteCount == 1);
  PcDiskImage reopened;
  assert(reopened.open(path));
  uint8_t persisted[PcDiskImage::SectorSize] = {};
  assert(reopened.readSectors(2, 1, persisted));
  assert(memcmp(persisted, "DOSWRITE", 8) == 0);

  char hardPath[] = "/tmp/tabdos-machine-hard-XXXXXX";
  int hardFd = mkstemp(hardPath);
  assert(hardFd >= 0);
  close(hardFd);
  createImage(hardPath, 20 * 1024 * 1024);
  assert(machine.openDisk(2, hardPath));
  machine.reset();
  memset(sector, 0, sizeof(sector));
  sector[0] = 0xf4; // hlt
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(2)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(2, 0x80));
  assert(machine.prepareBootCpu());
  cpuState = machine.cpuState();
  assert((cpuState.dx & 0x00ff) == 0x0080);
  assert(machine.readMemory8(0x0475) == 1);

  uint8_t const hardSeekProgram[] = {
    0xb4, 0x0c,             // mov ah, 0Ch: seek to cylinder
    0xb9, 0x01, 0x00,       // mov cx, 0001h (cylinder 0, sector 1 encoding)
    0xba, 0x80, 0x00,       // mov dx, 0080h (head 0, hard disk 80h)
    0xcd, 0x13,             // int 13h
    0xa3, 0x00, 0x05,       // mov [0500h], ax
    0x9c,                   // pushf
    0x58,                   // pop ax
    0xa3, 0x02, 0x05,       // mov [0502h], ax
    0xf4                    // hlt
  };
  machine.reset();
  memset(sector, 0, sizeof(sector));
  memcpy(sector, hardSeekProgram, sizeof(hardSeekProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(2)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(2, 0x80));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(24);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x0000);
  assert((machine.readMemory16(0x0502) & 0x0001) == 0x0000);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);

  uint8_t const hardReadyProgram[] = {
    0xb4, 0x10,             // mov ah, 10h: check drive ready
    0xb2, 0x80,             // mov dl, 80h
    0xcd, 0x13,             // int 13h
    0xa3, 0x00, 0x05,       // mov [0500h], ax
    0x9c,                   // pushf
    0x58,                   // pop ax
    0xa3, 0x02, 0x05,       // mov [0502h], ax
    0xf4                    // hlt
  };
  machine.reset();
  memset(sector, 0, sizeof(sector));
  memcpy(sector, hardReadyProgram, sizeof(hardReadyProgram));
  sector[510] = 0x55;
  sector[511] = 0xaa;
  assert(machine.disk(2)->writeSectors(0, 1, sector));
  assert(machine.loadBootSector(2, 0x80));
  assert(machine.prepareBootCpu());
  machine.runCpuSteps(20);
  assert(machine.cpuHalted());
  assert(machine.readMemory16(0x0500) == 0x0000);
  assert((machine.readMemory16(0x0502) & 0x0001) == 0x0000);
  assert(machine.diagnostics().unsupportedInterruptCount == 0);
  unlink(hardPath);

  assert(!machine.loadBootSector(1, 0x01));
  assert(!machine.loadBootSector(0, 0x00, 0xffff, 0x0010));

  // Keep LOOP/LOOPZ/LOOPNZ behavior locked before moving these high-frequency
  // instructions onto the short decoder path used by KOEI's inner loops.
  machine.reset();
  machine.writeMemory8(0x0700, 0xe2); // loop 0700h
  machine.writeMemory8(0x0701, 0xfe);
  PcI8086::setCS(0x0000);
  PcI8086::setIP(0x0700);
  PcI8086::setCX(2);
  machine.stepCpu();
  assert(PcI8086::CX() == 1 && PcI8086::IP() == 0x0700);
  machine.stepCpu();
  assert(PcI8086::CX() == 0 && PcI8086::IP() == 0x0702);

  machine.writeMemory8(0x0710, 0xe1); // loopz 0710h
  machine.writeMemory8(0x0711, 0xfe);
  PcI8086::setIP(0x0710);
  PcI8086::setCX(2);
  PcI8086::setFlagZF(true);
  machine.stepCpu();
  assert(PcI8086::CX() == 1 && PcI8086::IP() == 0x0710);
  PcI8086::setFlagZF(false);
  machine.stepCpu();
  assert(PcI8086::CX() == 0 && PcI8086::IP() == 0x0712);

  machine.writeMemory8(0x0720, 0xe0); // loopnz 0720h
  machine.writeMemory8(0x0721, 0xfe);
  PcI8086::setIP(0x0720);
  PcI8086::setCX(2);
  PcI8086::setFlagZF(false);
  machine.stepCpu();
  assert(PcI8086::CX() == 1 && PcI8086::IP() == 0x0720);
  PcI8086::setFlagZF(true);
  machine.stepCpu();
  assert(PcI8086::CX() == 0 && PcI8086::IP() == 0x0722);

  // INC/DEC register forms preserve CF while updating the arithmetic flags.
  machine.writeMemory8(0x0730, 0x40); // inc ax
  machine.writeMemory8(0x0731, 0x48); // dec ax
  PcI8086::setIP(0x0730);
  PcI8086::setAX(0x7fff);
  PcI8086::setFlagCF(true);
  machine.stepCpu();
  assert(PcI8086::AX() == 0x8000);
  assert(PcI8086::flagCF() && PcI8086::flagOF() && PcI8086::flagAF());
  assert(PcI8086::flagSF() && !PcI8086::flagZF() && PcI8086::flagPF());
  PcI8086::setAX(0x8000);
  PcI8086::setFlagCF(false);
  machine.stepCpu();
  assert(PcI8086::AX() == 0x7fff);
  assert(!PcI8086::flagCF() && PcI8086::flagOF() && PcI8086::flagAF());
  assert(!PcI8086::flagSF() && !PcI8086::flagZF() && PcI8086::flagPF());

  // Register-only forms dominate the measured KOEI loop. Lock their values
  // and flags before bypassing the general ModR/M memory decoder.
  machine.writeMemory8(0x0740, 0x02); // add al, bl
  machine.writeMemory8(0x0741, 0xc3);
  machine.writeMemory8(0x0742, 0x13); // adc dx, bx
  machine.writeMemory8(0x0743, 0xd3);
  machine.writeMemory8(0x0744, 0xfe); // inc al
  machine.writeMemory8(0x0745, 0xc0);
  machine.writeMemory8(0x0746, 0xfe); // dec al
  machine.writeMemory8(0x0747, 0xc8);
  PcI8086::setIP(0x0740);
  PcI8086::setAL(0x7f);
  PcI8086::setBL(0x01);
  PcI8086::setFlagCF(false);
  machine.stepCpu();
  assert(PcI8086::AL() == 0x80);
  assert(!PcI8086::flagCF() && PcI8086::flagOF() && PcI8086::flagAF());
  assert(PcI8086::flagSF() && !PcI8086::flagZF() && !PcI8086::flagPF());
  PcI8086::setDX(0xffff);
  PcI8086::setBX(0x0000);
  PcI8086::setFlagCF(true);
  machine.stepCpu();
  assert(PcI8086::DX() == 0x0000);
  assert(PcI8086::flagCF() && !PcI8086::flagOF() && PcI8086::flagAF());
  assert(!PcI8086::flagSF() && PcI8086::flagZF() && PcI8086::flagPF());
  PcI8086::setAL(0x7f);
  PcI8086::setFlagCF(true);
  machine.stepCpu();
  assert(PcI8086::AL() == 0x80 && PcI8086::flagCF() && PcI8086::flagOF());
  PcI8086::setAL(0x80);
  PcI8086::setFlagCF(false);
  machine.stepCpu();
  assert(PcI8086::AL() == 0x7f && !PcI8086::flagCF() && PcI8086::flagOF());

  memset(sector, 0, sizeof(sector));
  memcpy(sector, "MACHINE", 7);
  assert(machine.disk(0)->writeChs(0, 0, 1, 1, sector));
  memset(sector, 0, sizeof(sector));
  assert(machine.disk(0)->readChs(0, 0, 1, 1, sector));
  assert(memcmp(sector, "MACHINE", 7) == 0);
  unlink(path);

  printf("pc_machine_test passed\n");
  return 0;
}
