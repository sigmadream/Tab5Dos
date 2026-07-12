#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "pc_i8086.h"
#undef private
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "pc_machine.h"
#include "pc_opl2.h"
#include "presented_frame_cache.h"

namespace {

constexpr int EmsBase = 0xe0000;
constexpr int EmsEnd = 0xf0000;
constexpr int EmsPageSize = 16 * 1024;

struct EmsFixture {
  std::array<uint8_t, EmsEnd - EmsBase> bytes{};
};

uint8_t readEms(void * context, int address)
{
  return static_cast<EmsFixture *>(context)->bytes[static_cast<std::size_t>(address - EmsBase)];
}

void writeEms(void * context, int address, uint8_t value)
{
  static_cast<EmsFixture *>(context)->bytes[static_cast<std::size_t>(address - EmsBase)] = value;
}

bool expect(bool condition, char const * message)
{
  if (!condition)
    std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

bool testEmsWordBoundaries()
{
  std::array<uint8_t, 1024 * 1024> memory{};
  EmsFixture ems;
  tabdos::PcI8086::setCallbacks(&ems, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
  tabdos::PcI8086::setMemory(memory.data());
  tabdos::PcI8086::setEmsWindow(EmsBase, EmsEnd, readEms, writeEms);
  tabdos::PcI8086::setEmsWindowActive(true);

  memory[EmsBase - 1] = 0x11;
  ems.bytes.front() = 0x22;
  bool ok = expect(tabdos::PcI8086::RMEM16(EmsBase - 1) == 0x2211,
                   "word read crossing into EMS must dispatch its high byte to EMS");

  ems.bytes.back() = 0x33;
  memory[EmsEnd] = 0x44;
  ok &= expect(tabdos::PcI8086::RMEM16(EmsEnd - 1) == 0x4433,
               "word read crossing out of EMS must dispatch its high byte to RAM");

  tabdos::PcI8086::WMEM16(EmsBase - 1, 0x6655);
  ok &= expect(memory[EmsBase - 1] == 0x55 && ems.bytes.front() == 0x66,
               "word write crossing into EMS must update both address spaces");

  tabdos::PcI8086::WMEM16(EmsEnd - 1, 0x8877);
  ok &= expect(ems.bytes.back() == 0x77 && memory[EmsEnd] == 0x88,
               "word write crossing out of EMS must update both address spaces");

  int const pageBoundary = EmsBase + EmsPageSize;
  ems.bytes[EmsPageSize - 1] = 0x99;
  ems.bytes[EmsPageSize] = 0xaa;
  ok &= expect(tabdos::PcI8086::RMEM16(pageBoundary - 1) == 0xaa99,
               "word read crossing a 16 KiB EMS page boundary must dispatch both byte addresses");
  tabdos::PcI8086::WMEM16(pageBoundary - 1, 0xccbb);
  ok &= expect(ems.bytes[EmsPageSize - 1] == 0xbb && ems.bytes[EmsPageSize] == 0xcc,
               "word write crossing a 16 KiB EMS page boundary must update both mapped pages");
  tabdos::PcI8086::setEmsWindowActive(false);
  return ok;
}

bool testUnalignedRamWordAccess()
{
  std::array<uint8_t, 1024 * 1024> memory{};
  tabdos::PcI8086::setMemory(memory.data());
  tabdos::PcI8086::setEmsWindowActive(false);

  tabdos::PcI8086::WMEM16(0x0101, 0x3412);
  bool ok = expect(memory[0x0101] == 0x12 && memory[0x0102] == 0x34,
                   "unaligned 8086 word writes must preserve little-endian byte order");
  ok &= expect(tabdos::PcI8086::RMEM16(0x0101) == 0x3412,
               "unaligned 8086 word reads must not require host pointer alignment");
  tabdos::PcI8086::WMEM16(0x0fffff, 0x7856);
  ok &= expect(memory[0x0fffff] == 0x56 && memory[0x000000] == 0x78,
               "8086 word writes at the 1 MiB boundary must wrap the high byte to address zero");
  ok &= expect(tabdos::PcI8086::RMEM16(0x0fffff) == 0x7856,
               "8086 word reads at the 1 MiB boundary must wrap to the 20-bit address bus");
  memory[0x09daa] = 0xf4; // HLT at wrapped physical address for FFFE:9DCA
  tabdos::PcI8086::reset();
  tabdos::PcI8086::setCS(0xfffe);
  tabdos::PcI8086::setIP(0x9dca);
  tabdos::PcI8086::step();
  ok &= expect(tabdos::PcI8086::halted(),
               "instruction fetch above 1 MiB must wrap to the 8086 20-bit physical address");
  return ok;
}

bool testInstructionFetchAndStackUseMappedMemory()
{
  std::array<uint8_t, 1024 * 1024> memory{};
  EmsFixture ems;
  tabdos::PcI8086::setCallbacks(&ems, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
  tabdos::PcI8086::setMemory(memory.data());
  tabdos::PcI8086::setEmsWindow(EmsBase, EmsEnd, readEms, writeEms);

  memory[0x0fffff] = 0xb8; // MOV AX, 1234h, wrapping across the 20-bit bus boundary.
  memory[0x000000] = 0x34;
  memory[0x000001] = 0x12;
  tabdos::PcI8086::setEmsWindowActive(false);
  tabdos::PcI8086::reset();
  tabdos::PcI8086::setCS(0xffff);
  tabdos::PcI8086::setIP(0x000f);
  tabdos::PcI8086::step();
  bool ok = expect(tabdos::PcI8086::AX() == 0x1234,
                   "multi-byte instruction fetch must wrap at the 20-bit address boundary");

  memory[EmsBase] = 0xf4; // Flat RAM must be hidden while the EMS frame is mapped.
  ems.bytes[0] = 0xb8;    // MOV AX, 5678h from mapped expanded memory.
  ems.bytes[1] = 0x78;
  ems.bytes[2] = 0x56;
  tabdos::PcI8086::setEmsWindowActive(true);
  tabdos::PcI8086::reset();
  tabdos::PcI8086::setCS(0xe000);
  tabdos::PcI8086::setIP(0x0000);
  tabdos::PcI8086::step();
  ok &= expect(!tabdos::PcI8086::halted() && tabdos::PcI8086::AX() == 0x5678,
               "instruction fetch must execute bytes from the mapped EMS page frame");

  memory[0x0100] = 0x50; // PUSH AX
  tabdos::PcI8086::setCS(0x0000);
  tabdos::PcI8086::setIP(0x0100);
  tabdos::PcI8086::setSS(0xe000);
  tabdos::PcI8086::setSP(0x0002);
  tabdos::PcI8086::setAX(0xabcd);
  tabdos::PcI8086::step();
  ok &= expect(ems.bytes[0] == 0xcd && ems.bytes[1] == 0xab,
               "stack writes in the EMS page frame must use mapped expanded memory");

  tabdos::PcI8086::setEmsWindowActive(false);
  return ok;
}

void writeOpl(tabdos::Opl2 & opl, uint8_t reg, uint8_t value)
{
  opl.writeAddress(reg);
  opl.writeData(value, 0);
}

bool testOplZeroAttackAndResetHandoff()
{
  tabdos::Opl2 opl;
  writeOpl(opl, 0x20, 0x01);
  writeOpl(opl, 0x23, 0x01);
  writeOpl(opl, 0x40, 0x00);
  writeOpl(opl, 0x43, 0x00);
  writeOpl(opl, 0x60, 0x00);
  writeOpl(opl, 0x63, 0x00);
  writeOpl(opl, 0x80, 0x0f);
  writeOpl(opl, 0x83, 0x0f);
  writeOpl(opl, 0xa0, 0x98);
  writeOpl(opl, 0xb0, 0x31);

  tabdos::Opl2::RegisterSnapshot snapshot{};
  opl.snapshotRegisters(&snapshot);
  std::array<int16_t, 128> samples{};
  bool ok = expect(opl.render(snapshot, samples.data(), static_cast<int>(samples.size()), 48000),
                   "key-on channel should remain active while AR=0 holds its envelope");
  ok &= expect(std::all_of(samples.begin(), samples.end(), [](int16_t sample) { return sample == 0; }),
               "AR=0 must not jump to an audible envelope level");

  opl.reset();
  tabdos::Opl2::RegisterSnapshot resetSnapshot{};
  opl.snapshotRegisters(&resetSnapshot);
  ok &= expect(!opl.render(resetSnapshot, samples.data(), static_cast<int>(samples.size()), 48000),
               "a front-end reset must stop audio on the next snapshot");
  ok &= expect(!opl.anyKeyOn(), "reset generation handoff must clear audio-thread DSP state");

  writeOpl(opl, 0x20, 0x01);
  writeOpl(opl, 0x23, 0x01);
  writeOpl(opl, 0x40, 0x00);
  writeOpl(opl, 0x43, 0x00);
  writeOpl(opl, 0x60, 0xf0);
  writeOpl(opl, 0x63, 0xf0);
  writeOpl(opl, 0x80, 0x0f);
  writeOpl(opl, 0x83, 0x0f);
  writeOpl(opl, 0xa0, 0x98);
  writeOpl(opl, 0xb0, 0x31);
  tabdos::Opl2::RegisterSnapshot fastAttackSnapshot{};
  opl.snapshotRegisters(&fastAttackSnapshot);
  samples.fill(0);
  ok &= expect(opl.render(fastAttackSnapshot, samples.data(), static_cast<int>(samples.size()), 48000),
               "maximum attack rate should activate the channel");
  ok &= expect(std::any_of(samples.begin(), samples.end(), [](int16_t sample) { return sample != 0; }),
               "maximum attack rate should produce audible samples instead of holding at zero");
  return ok;
}

bool testPresentationPathTransitions()
{
  PresentedFrameCache cache;
  constexpr uint64_t GraphicsHash = 0x1234;
  constexpr uint64_t TextHash = 0x5678;

  bool ok = expect(!cache.matchesGraphics(GraphicsHash), "an empty cache must not skip graphics");
  cache.commitGraphics(GraphicsHash);
  ok &= expect(cache.matchesGraphics(GraphicsHash), "an identical current graphics frame may be skipped");
  cache.commitText(TextHash);
  ok &= expect(!cache.matchesGraphics(GraphicsHash),
               "G -> T -> same G must redraw because the panel currently contains text");
  ok &= expect(cache.matchesText(TextHash), "an identical current text frame may be skipped");
  cache.commitGraphics(GraphicsHash);
  ok &= expect(!cache.matchesText(TextHash),
               "T -> G -> same T must redraw because the panel currently contains graphics");
  cache.invalidate();
  ok &= expect(!cache.matchesGraphics(GraphicsHash) && !cache.matchesText(TextHash),
               "explicit invalidation must force the next presentation");
  return ok;
}

bool testEmsMappablePhysicalAddressArray()
{
  tabdos::PcMachine machine;
  bool ok = expect(machine.init(), "machine initialization must provide the EMS pool for host tests");
  if (!ok)
    return false;
  tabdos::PcI8086::reset();

  tabdos::PcI8086::setAX(0x4600);
  ok &= expect(machine.handleEmsInterrupt(), "EMS 46h must be handled");
  ok &= expect(tabdos::PcI8086::AH() == 0x00 &&
                   tabdos::PcI8086::AL() == tabdos::PcMachine::EmsVersion,
               "an EMS manager exposing function 58h must report LIM EMS 4.0");

  tabdos::PcI8086::setAX(0x5801);
  ok &= expect(machine.handleEmsInterrupt(), "EMS 58h/01h must be handled");
  ok &= expect(tabdos::PcI8086::AH() == 0x00 && tabdos::PcI8086::CX() == 4,
               "EMS 58h/01h must report four mappable physical pages");

  constexpr uint16_t BufferSegment = 0x2000;
  constexpr uint16_t BufferOffset = 0x0100;
  constexpr uint32_t BufferLinear = (static_cast<uint32_t>(BufferSegment) << 4) + BufferOffset;
  tabdos::PcI8086::setES(BufferSegment);
  tabdos::PcI8086::setDI(BufferOffset);
  tabdos::PcI8086::setAX(0x5800);
  ok &= expect(machine.handleEmsInterrupt(), "EMS 58h/00h must be handled");
  ok &= expect(tabdos::PcI8086::AH() == 0x00 && tabdos::PcI8086::CX() == 4,
               "EMS 58h/00h must return four physical address entries");
  for (int page = 0; page < 4; ++page) {
    uint16_t const expectedSegment = static_cast<uint16_t>(0xe000 + page * 0x0400);
    ok &= expect(machine.readMemory16(BufferLinear + static_cast<uint32_t>(page) * 4) == expectedSegment,
                 "EMS 58h/00h must return physical page segments in ascending address order");
    ok &= expect(machine.readMemory16(BufferLinear + static_cast<uint32_t>(page) * 4 + 2) == page,
                 "EMS 58h/00h must return physical page numbers matching the mapping slots");
  }

  tabdos::PcI8086::setAX(0x5802);
  ok &= expect(machine.handleEmsInterrupt() && tabdos::PcI8086::AH() == 0x8f,
               "EMS 58h must reject undefined subfunctions with status 8Fh");
  return ok;
}

bool testMachineMemoryApisUseMappedEms()
{
  tabdos::PcMachine machine;
  bool ok = expect(machine.init(), "machine initialization must provide the EMS pool for host tests");
  if (!ok)
    return false;

  tabdos::PcI8086::setBX(2);
  tabdos::PcI8086::setAX(0x4300);
  ok &= expect(machine.handleEmsInterrupt() && tabdos::PcI8086::AH() == 0x00,
               "EMS allocation must succeed before testing mapped machine memory");
  uint16_t const handle = tabdos::PcI8086::DX();
  for (uint8_t physicalPage = 0; physicalPage < 2; ++physicalPage) {
    tabdos::PcI8086::setAL(physicalPage);
    tabdos::PcI8086::setBX(physicalPage);
    tabdos::PcI8086::setDX(handle);
    tabdos::PcI8086::setAH(0x44);
    ok &= expect(machine.handleEmsInterrupt() && tabdos::PcI8086::AH() == 0x00,
                 "EMS logical pages must map into adjacent physical slots");
  }

  constexpr uint32_t Boundary = EmsBase + EmsPageSize;
  machine.ram()[Boundary - 1] = 0x11;
  machine.ram()[Boundary] = 0x22;
  std::array<uint8_t, 4> const source = {0xa1, 0xb2, 0xc3, 0xd4};
  std::array<uint8_t, 4> dest{};
  ok &= expect(machine.writeMemoryBlock(Boundary - 2, source.data(), source.size()),
               "machine block writes must accept ranges crossing mapped EMS pages");
  ok &= expect(machine.readMemoryBlock(Boundary - 2, dest.data(), dest.size()) && dest == source,
               "machine block reads must resolve both mapped EMS pages");
  ok &= expect(machine.ram()[Boundary - 1] == 0x11 && machine.ram()[Boundary] == 0x22,
               "mapped machine memory APIs must not write the hidden flat RAM backing");
  return ok;
}

bool testCpuAttachmentLifecycle()
{
  EmsFixture owner;
  tabdos::PcI8086::setCallbacks(&owner, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
  tabdos::PcI8086::setEmsWindowActive(true);

  bool ok = true;
  {
    tabdos::PcMachine unrelated;
    ok &= expect(unrelated.init(), "unrelated machine must initialize for attachment isolation test");
    ok &= expect(tabdos::PcI8086::isAttachedTo(&owner) && tabdos::PcI8086::s_emsActive,
                 "resetting another machine must not replace or mutate the active CPU attachment");
    ok &= expect(unrelated.runCpuSteps(1) == 0 && unrelated.cpuHalted(),
                 "an unattached machine must not execute another machine's CPU state");
  }

  auto * attached = new tabdos::PcMachine();
  tabdos::PcI8086::setCallbacks(attached, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
  ok &= expect(tabdos::PcI8086::isAttachedTo(attached), "test machine must own the CPU attachment");
  delete attached;
  ok &= expect(!tabdos::PcI8086::isAttachedTo(attached),
               "destroying the active machine must detach global CPU callbacks");
  return ok;
}

} // namespace

int main()
{
  bool const ok = testEmsWordBoundaries() && testUnalignedRamWordAccess() &&
                  testInstructionFetchAndStackUseMappedMemory() &&
                  testOplZeroAttackAndResetHandoff() &&
                  testPresentationPathTransitions() && testEmsMappablePhysicalAddressArray() &&
                  testMachineMemoryApisUseMappedEms() && testCpuAttachmentLifecycle();
  if (ok)
    std::puts("All regression tests passed");
  return ok ? 0 : 1;
}
