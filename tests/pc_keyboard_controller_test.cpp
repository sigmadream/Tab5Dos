#include "pc/pc_keyboard_controller.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>

using tabdos::PcKeyboardController;

static std::vector<uint8_t> drain(PcKeyboardController & keyboard)
{
  std::vector<uint8_t> bytes;
  while (keyboard.readStatusPort() & PcKeyboardController::StatusOutputBufferFull) {
    assert(keyboard.irqPending());
    bytes.push_back(keyboard.readDataPort());
    keyboard.acknowledgeIrq();
  }
  return bytes;
}

static void expectBytes(std::vector<uint8_t> const & actual, std::vector<uint8_t> const & expected)
{
  if (actual != expected) {
    fprintf(stderr, "expected:");
    for (auto b : expected)
      fprintf(stderr, " %02x", b);
    fprintf(stderr, "\nactual:  ");
    for (auto b : actual)
      fprintf(stderr, " %02x", b);
    fprintf(stderr, "\n");
    assert(false);
  }
}

int main()
{
  PcKeyboardController keyboard;
  uint8_t keys[6] = {};

  keys[0] = 0x04; // HID a
  keyboard.onHidBootKeyboardReport(0, keys);
  expectBytes(drain(keyboard), {0x1e});

  keys[0] = 0;
  keyboard.onHidBootKeyboardReport(0, keys);
  expectBytes(drain(keyboard), {0x9e});

  keys[0] = 0x04; // Shift+a
  keyboard.onHidBootKeyboardReport(0x02, keys);
  expectBytes(drain(keyboard), {0x2a, 0x1e});

  keys[0] = 0;
  keyboard.onHidBootKeyboardReport(0, keys);
  expectBytes(drain(keyboard), {0x9e, 0xaa});

  keys[0] = 0x52; // Up arrow
  keyboard.onHidBootKeyboardReport(0, keys);
  expectBytes(drain(keyboard), {0x48});

  keys[0] = 0;
  keyboard.onHidBootKeyboardReport(0, keys);
  expectBytes(drain(keyboard), {0xc8});

  keys[0] = 0x06; // Ctrl+C
  keyboard.onHidBootKeyboardReport(0x01, keys);
  expectBytes(drain(keyboard), {0x1d, 0x2e});

  keyboard.onUsbDisconnected();
  expectBytes(drain(keyboard), {0xae, 0x9d});

  keyboard.writeCommandPort(0x20);
  auto commandByte = drain(keyboard);
  assert(commandByte.size() == 1);
  assert(commandByte[0] & 0x01);

  keyboard.writeCommandPort(0xad);
  assert(!keyboard.keyboardEnabled());
  keys[0] = 0x07; // d is deferred while disabled
  keyboard.onHidBootKeyboardReport(0, keys);
  assert(drain(keyboard).empty());

  keyboard.writeCommandPort(0xae);
  assert(keyboard.keyboardEnabled());
  expectBytes(drain(keyboard), {0x20});
  keyboard.onHidBootKeyboardReport(0, keys);
  assert(drain(keyboard).empty());
  keys[0] = 0;
  keyboard.onHidBootKeyboardReport(0, keys);
  expectBytes(drain(keyboard), {0xa0});

  keyboard.writeCommandPort(0xad);
  assert(!keyboard.keyboardEnabled());
  keys[0] = 0x08; // e pressed and released entirely while disabled is not replayed
  keyboard.onHidBootKeyboardReport(0, keys);
  keys[0] = 0;
  keyboard.onHidBootKeyboardReport(0, keys);
  assert(drain(keyboard).empty());
  keyboard.writeCommandPort(0xae);
  assert(drain(keyboard).empty());

  keyboard.writeDataPort(0xed);
  expectBytes(drain(keyboard), {0xfa});
  keyboard.writeDataPort(0x07);
  expectBytes(drain(keyboard), {0xfa});
  bool num = false, caps = false, scroll = false;
  keyboard.getLeds(&num, &caps, &scroll);
  assert(num && caps && scroll);

  keyboard.writeDataPort(0xf0); // query current scan-code set
  expectBytes(drain(keyboard), {0xfa});
  keyboard.writeDataPort(0x00);
  expectBytes(drain(keyboard), {0xfa, 0x02});

  keyboard.writeDataPort(0xf0); // set scan-code set 1 for programs that probe it
  expectBytes(drain(keyboard), {0xfa});
  keyboard.writeDataPort(0x01);
  expectBytes(drain(keyboard), {0xfa});
  keyboard.writeDataPort(0xf0);
  expectBytes(drain(keyboard), {0xfa});
  keyboard.writeDataPort(0x00);
  expectBytes(drain(keyboard), {0xfa, 0x01});

  keyboard.writeDataPort(0xf6); // restore defaults
  expectBytes(drain(keyboard), {0xfa});
  keyboard.writeDataPort(0xf0);
  expectBytes(drain(keyboard), {0xfa});
  keyboard.writeDataPort(0x00);
  expectBytes(drain(keyboard), {0xfa, 0x02});

  keys[0] = 0x04; // default controller translation exposes Set 1 even with keyboard Set 2
  keyboard.onHidBootKeyboardReport(0, keys);
  expectBytes(drain(keyboard), {0x1e});
  keys[0] = 0;
  keyboard.onHidBootKeyboardReport(0, keys);
  expectBytes(drain(keyboard), {0x9e});

  keyboard.writeCommandPort(0x60); // disable 8042 translation in the controller command byte
  keyboard.writeDataPort(0x05);
  keys[0] = 0x04; // HID a now emits raw Set 2 make/break
  keyboard.onHidBootKeyboardReport(0, keys);
  expectBytes(drain(keyboard), {0x1c});
  keys[0] = 0;
  keyboard.onHidBootKeyboardReport(0, keys);
  expectBytes(drain(keyboard), {0xf0, 0x1c});

  keys[0] = 0x52; // Extended Set 2 keys use E0 F0 release sequences
  keyboard.onHidBootKeyboardReport(0, keys);
  expectBytes(drain(keyboard), {0xe0, 0x75});
  keys[0] = 0;
  keyboard.onHidBootKeyboardReport(0, keys);
  expectBytes(drain(keyboard), {0xe0, 0xf0, 0x75});

  keyboard.writeDataPort(0xf0); // explicit keyboard Set 1 still emits Set 1 without translation
  expectBytes(drain(keyboard), {0xfa});
  keyboard.writeDataPort(0x01);
  expectBytes(drain(keyboard), {0xfa});
  keys[0] = 0x04;
  keyboard.onHidBootKeyboardReport(0, keys);
  expectBytes(drain(keyboard), {0x1e});
  keys[0] = 0;
  keyboard.onHidBootKeyboardReport(0, keys);
  expectBytes(drain(keyboard), {0x9e});

  printf("pc_keyboard_controller_test passed\n");
  return 0;
}
