#include "pc_keyboard_controller.h"

#include <string.h>

namespace tabdos {

namespace {

constexpr uint8_t CommandByteKeyboardIrq     = 0x01;
constexpr uint8_t CommandByteSystemFlag      = 0x04;
constexpr uint8_t CommandByteKeyboardDisable = 0x10;
constexpr uint8_t CommandByteTranslation     = 0x40;

constexpr uint8_t Ack = 0xfa;
constexpr uint8_t Resend = 0xfe;

} // namespace

PcKeyboardController::PcKeyboardController()
{
  reset();
}

void PcKeyboardController::reset()
{
  m_commandByte = CommandByteKeyboardIrq | CommandByteSystemFlag | CommandByteTranslation;
  m_keyboardDisabled = false;
  m_numLockLED = false;
  m_capsLockLED = false;
  m_scrollLockLED = false;
  m_scanCodeSet = 2;
  m_irqPending = false;
  m_pendingWrite = PendingWrite::None;
  m_lastModifiers = 0;
  memset(m_lastKeys, 0, sizeof(m_lastKeys));
  m_deferredReportValid = false;
  m_deferredModifiers = 0;
  memset(m_deferredKeys, 0, sizeof(m_deferredKeys));
  m_queueRead = 0;
  m_queueWrite = 0;
  m_queueCount = 0;
}

void PcKeyboardController::onHidBootKeyboardReport(uint8_t modifiers, uint8_t const keycodes[6])
{
  if (m_keyboardDisabled) {
    rememberDeferredHidReport(modifiers, keycodes);
    return;
  }

  m_deferredReportValid = false;
  processHidBootKeyboardReport(modifiers, keycodes);
}

void PcKeyboardController::processHidBootKeyboardReport(uint8_t modifiers, uint8_t const keycodes[6])
{
  uint8_t modifierDown = modifiers & ~m_lastModifiers;
  for (uint8_t bit = 0x01; bit; bit <<= 1) {
    if (modifierDown & bit)
      handleModifierDown(bit);
  }

  for (int i = 0; i < 6; ++i) {
    uint8_t usage = keycodes[i];
    if (isValidBootKeycode(usage) && !containsKey(m_lastKeys, usage))
      handleKeyDown(usage);
  }

  for (int i = 0; i < 6; ++i) {
    uint8_t usage = m_lastKeys[i];
    if (isValidBootKeycode(usage) && !containsKey(keycodes, usage))
      handleKeyUp(usage);
  }

  uint8_t modifierUp = m_lastModifiers & ~modifiers;
  for (uint8_t bit = 0x01; bit; bit <<= 1) {
    if (modifierUp & bit)
      handleModifierUp(bit);
  }

  m_lastModifiers = modifiers;
  memcpy(m_lastKeys, keycodes, sizeof(m_lastKeys));
}

void PcKeyboardController::rememberDeferredHidReport(uint8_t modifiers, uint8_t const keycodes[6])
{
  m_deferredReportValid = true;
  m_deferredModifiers = modifiers;
  memcpy(m_deferredKeys, keycodes, sizeof(m_deferredKeys));
}

void PcKeyboardController::applyDeferredHidReportIfEnabled()
{
  if (m_keyboardDisabled || !m_deferredReportValid)
    return;
  m_deferredReportValid = false;
  processHidBootKeyboardReport(m_deferredModifiers, m_deferredKeys);
}

void PcKeyboardController::onUsbDisconnected()
{
  uint8_t empty[6] = {};
  onHidBootKeyboardReport(0, empty);
  clearPressedState();
}

uint8_t PcKeyboardController::readDataPort()
{
  if (m_queueCount == 0)
    return 0;

  uint8_t value = m_queue[m_queueRead];
  m_queueRead = (m_queueRead + 1) % QueueSize;
  --m_queueCount;
  refreshIrq();
  return value;
}

uint8_t PcKeyboardController::readStatusPort() const
{
  uint8_t status = StatusSystemFlag | StatusInhibitSwitch;
  if (m_queueCount > 0)
    status |= StatusOutputBufferFull;
  return status;
}

void PcKeyboardController::writeDataPort(uint8_t value)
{
  switch (m_pendingWrite) {
    case PendingWrite::CommandByte:
    {
      bool const wasDisabled = m_keyboardDisabled;
      m_commandByte = value;
      m_keyboardDisabled = (value & CommandByteKeyboardDisable) != 0;
      m_pendingWrite = PendingWrite::None;
      if (wasDisabled && !m_keyboardDisabled)
        applyDeferredHidReportIfEnabled();
      break;
    }

    case PendingWrite::KeyboardLeds:
      setLeds((value & 0x02) != 0, (value & 0x04) != 0, (value & 0x01) != 0);
      enqueueByte(Ack);
      m_pendingWrite = PendingWrite::None;
      break;

    case PendingWrite::KeyboardTypematic:
      enqueueByte(Ack);
      m_pendingWrite = PendingWrite::None;
      break;

    case PendingWrite::KeyboardScanCodeSet:
      if (value == 0) {
        enqueueByte(Ack);
        enqueueByte(m_scanCodeSet);
      } else if (value >= 1 && value <= 3) {
        enqueueByte(Ack);
        m_scanCodeSet = value;
      } else {
        enqueueByte(Resend);
      }
      m_pendingWrite = PendingWrite::None;
      break;

    case PendingWrite::OutputBuffer:
      enqueueByte(value);
      m_pendingWrite = PendingWrite::None;
      break;

    case PendingWrite::None:
      handleKeyboardCommand(value);
      break;
  }
}

void PcKeyboardController::writeCommandPort(uint8_t value)
{
  switch (value) {
    case 0x20: // read command byte
      enqueueByte(m_commandByte);
      break;

    case 0x60: // write command byte
      m_pendingWrite = PendingWrite::CommandByte;
      break;

    case 0xaa: // controller self-test
      enqueueByte(0x55);
      break;

    case 0xab: // keyboard port test
      enqueueByte(0x00);
      break;

    case 0xad: // disable keyboard
      m_keyboardDisabled = true;
      m_commandByte |= CommandByteKeyboardDisable;
      break;

    case 0xae: // enable keyboard
      m_keyboardDisabled = false;
      m_commandByte &= ~CommandByteKeyboardDisable;
      applyDeferredHidReportIfEnabled();
      break;

    case 0xd2: // write next byte to keyboard output buffer
      m_pendingWrite = PendingWrite::OutputBuffer;
      break;

    default:
      break;
  }
}

void PcKeyboardController::acknowledgeIrq()
{
  m_irqPending = false;
  refreshIrq();
}

void PcKeyboardController::getLeds(bool * numLock, bool * capsLock, bool * scrollLock) const
{
  if (numLock)
    *numLock = m_numLockLED;
  if (capsLock)
    *capsLock = m_capsLockLED;
  if (scrollLock)
    *scrollLock = m_scrollLockLED;
}

void PcKeyboardController::setLeds(bool numLock, bool capsLock, bool scrollLock)
{
  m_numLockLED = numLock;
  m_capsLockLED = capsLock;
  m_scrollLockLED = scrollLock;
}

bool PcKeyboardController::isValidBootKeycode(uint8_t usage)
{
  return usage >= 0x04 && usage <= 0x73;
}

bool PcKeyboardController::containsKey(uint8_t const keycodes[6], uint8_t usage)
{
  for (int i = 0; i < 6; ++i) {
    if (keycodes[i] == usage)
      return true;
  }
  return false;
}

PcKeyboardController::ScancodeSequence PcKeyboardController::usageToSet1(uint8_t usage)
{
  static constexpr uint8_t LETTERS[26] = {
    0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c,
  };
  static constexpr uint8_t NUMBERS[10] = { 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b };
  static constexpr uint8_t FUNCTION_KEYS[12] = { 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58 };

  if (usage >= 0x04 && usage <= 0x1d)
    return {{ LETTERS[usage - 0x04], 0x00 }, 1};
  if (usage >= 0x1e && usage <= 0x27)
    return {{ NUMBERS[usage - 0x1e], 0x00 }, 1};
  if (usage >= 0x3a && usage <= 0x45)
    return {{ FUNCTION_KEYS[usage - 0x3a], 0x00 }, 1};

  switch (usage) {
    case 0x28: return {{ 0x1c, 0x00 }, 1}; // Enter
    case 0x29: return {{ 0x01, 0x00 }, 1}; // Escape
    case 0x2a: return {{ 0x0e, 0x00 }, 1}; // Backspace
    case 0x2b: return {{ 0x0f, 0x00 }, 1}; // Tab
    case 0x2c: return {{ 0x39, 0x00 }, 1}; // Space
    case 0x2d: return {{ 0x0c, 0x00 }, 1}; // -
    case 0x2e: return {{ 0x0d, 0x00 }, 1}; // =
    case 0x2f: return {{ 0x1a, 0x00 }, 1}; // [
    case 0x30: return {{ 0x1b, 0x00 }, 1}; // ]
    case 0x31: return {{ 0x2b, 0x00 }, 1}; // Backslash
    case 0x32: return {{ 0x2b, 0x00 }, 1}; // non-US #/~
    case 0x33: return {{ 0x27, 0x00 }, 1}; // ;
    case 0x34: return {{ 0x28, 0x00 }, 1}; // '
    case 0x35: return {{ 0x29, 0x00 }, 1}; // `
    case 0x36: return {{ 0x33, 0x00 }, 1}; // ,
    case 0x37: return {{ 0x34, 0x00 }, 1}; // .
    case 0x38: return {{ 0x35, 0x00 }, 1}; // /
    case 0x39: return {{ 0x3a, 0x00 }, 1}; // Caps Lock

    // Prefer legacy keypad-style Set 1 navigation scancodes for DOS
    // compatibility. Many text-mode programs hook INT 09h directly and only
    // understand the original non-E0 cursor-key scancodes even though USB HID
    // exposes dedicated navigation usages.
    case 0x49: return {{ 0x52, 0x00 }, 1}; // Insert
    case 0x4a: return {{ 0x47, 0x00 }, 1}; // Home
    case 0x4b: return {{ 0x49, 0x00 }, 1}; // Page Up
    case 0x4c: return {{ 0x53, 0x00 }, 1}; // Delete
    case 0x4d: return {{ 0x4f, 0x00 }, 1}; // End
    case 0x4e: return {{ 0x51, 0x00 }, 1}; // Page Down
    case 0x4f: return {{ 0x4d, 0x00 }, 1}; // Right
    case 0x50: return {{ 0x4b, 0x00 }, 1}; // Left
    case 0x51: return {{ 0x50, 0x00 }, 1}; // Down
    case 0x52: return {{ 0x48, 0x00 }, 1}; // Up

    case 0x53: return {{ 0x45, 0x00 }, 1}; // Num Lock
    case 0x54: return {{ 0x37, 0x00 }, 1}; // Keypad /
    case 0x55: return {{ 0x37, 0x00 }, 1}; // Keypad *
    case 0x56: return {{ 0x4a, 0x00 }, 1}; // Keypad -
    case 0x57: return {{ 0x4e, 0x00 }, 1}; // Keypad +
    case 0x58: return {{ 0xe0, 0x1c }, 2}; // Keypad Enter
    case 0x59: return {{ 0x4f, 0x00 }, 1}; // Keypad 1
    case 0x5a: return {{ 0x50, 0x00 }, 1}; // Keypad 2
    case 0x5b: return {{ 0x51, 0x00 }, 1}; // Keypad 3
    case 0x5c: return {{ 0x4b, 0x00 }, 1}; // Keypad 4
    case 0x5d: return {{ 0x4c, 0x00 }, 1}; // Keypad 5
    case 0x5e: return {{ 0x4d, 0x00 }, 1}; // Keypad 6
    case 0x5f: return {{ 0x47, 0x00 }, 1}; // Keypad 7
    case 0x60: return {{ 0x48, 0x00 }, 1}; // Keypad 8
    case 0x61: return {{ 0x49, 0x00 }, 1}; // Keypad 9
    case 0x62: return {{ 0x52, 0x00 }, 1}; // Keypad 0
    case 0x63: return {{ 0x53, 0x00 }, 1}; // Keypad .

    default: return {{ 0x00, 0x00 }, 0};
  }
}


PcKeyboardController::ScancodeSequence PcKeyboardController::usageToSet2(uint8_t usage)
{
  static constexpr uint8_t LETTERS[26] = {
    0x1c, 0x32, 0x21, 0x23, 0x24, 0x2b, 0x34, 0x33, 0x43, 0x3b, 0x42, 0x4b, 0x3a,
    0x31, 0x44, 0x4d, 0x15, 0x2d, 0x1b, 0x2c, 0x3c, 0x2a, 0x1d, 0x22, 0x35, 0x1a,
  };
  static constexpr uint8_t NUMBERS[10] = { 0x16, 0x1e, 0x26, 0x25, 0x2e, 0x36, 0x3d, 0x3e, 0x46, 0x45 };
  static constexpr uint8_t FUNCTION_KEYS[12] = { 0x05, 0x06, 0x04, 0x0c, 0x03, 0x0b, 0x83, 0x0a, 0x01, 0x09, 0x78, 0x07 };

  if (usage >= 0x04 && usage <= 0x1d)
    return {{ LETTERS[usage - 0x04], 0x00 }, 1};
  if (usage >= 0x1e && usage <= 0x27)
    return {{ NUMBERS[usage - 0x1e], 0x00 }, 1};
  if (usage >= 0x3a && usage <= 0x45)
    return {{ FUNCTION_KEYS[usage - 0x3a], 0x00 }, 1};

  switch (usage) {
    case 0x28: return {{ 0x5a, 0x00 }, 1}; // Enter
    case 0x29: return {{ 0x76, 0x00 }, 1}; // Escape
    case 0x2a: return {{ 0x66, 0x00 }, 1}; // Backspace
    case 0x2b: return {{ 0x0d, 0x00 }, 1}; // Tab
    case 0x2c: return {{ 0x29, 0x00 }, 1}; // Space
    case 0x2d: return {{ 0x4e, 0x00 }, 1}; // -
    case 0x2e: return {{ 0x55, 0x00 }, 1}; // =
    case 0x2f: return {{ 0x54, 0x00 }, 1}; // [
    case 0x30: return {{ 0x5b, 0x00 }, 1}; // ]
    case 0x31: return {{ 0x5d, 0x00 }, 1}; // Backslash
    case 0x32: return {{ 0x5d, 0x00 }, 1}; // non-US #/~
    case 0x33: return {{ 0x4c, 0x00 }, 1}; // ;
    case 0x34: return {{ 0x52, 0x00 }, 1}; // '
    case 0x35: return {{ 0x0e, 0x00 }, 1}; // `
    case 0x36: return {{ 0x41, 0x00 }, 1}; // ,
    case 0x37: return {{ 0x49, 0x00 }, 1}; // .
    case 0x38: return {{ 0x4a, 0x00 }, 1}; // /
    case 0x39: return {{ 0x58, 0x00 }, 1}; // Caps Lock

    case 0x49: return {{ 0xe0, 0x70 }, 2}; // Insert
    case 0x4a: return {{ 0xe0, 0x6c }, 2}; // Home
    case 0x4b: return {{ 0xe0, 0x7d }, 2}; // Page Up
    case 0x4c: return {{ 0xe0, 0x71 }, 2}; // Delete
    case 0x4d: return {{ 0xe0, 0x69 }, 2}; // End
    case 0x4e: return {{ 0xe0, 0x7a }, 2}; // Page Down
    case 0x4f: return {{ 0xe0, 0x74 }, 2}; // Right
    case 0x50: return {{ 0xe0, 0x6b }, 2}; // Left
    case 0x51: return {{ 0xe0, 0x72 }, 2}; // Down
    case 0x52: return {{ 0xe0, 0x75 }, 2}; // Up

    case 0x53: return {{ 0x77, 0x00 }, 1}; // Num Lock
    case 0x54: return {{ 0xe0, 0x4a }, 2}; // Keypad /
    case 0x55: return {{ 0x7c, 0x00 }, 1}; // Keypad *
    case 0x56: return {{ 0x7b, 0x00 }, 1}; // Keypad -
    case 0x57: return {{ 0x79, 0x00 }, 1}; // Keypad +
    case 0x58: return {{ 0xe0, 0x5a }, 2}; // Keypad Enter
    case 0x59: return {{ 0x69, 0x00 }, 1}; // Keypad 1
    case 0x5a: return {{ 0x72, 0x00 }, 1}; // Keypad 2
    case 0x5b: return {{ 0x7a, 0x00 }, 1}; // Keypad 3
    case 0x5c: return {{ 0x6b, 0x00 }, 1}; // Keypad 4
    case 0x5d: return {{ 0x73, 0x00 }, 1}; // Keypad 5
    case 0x5e: return {{ 0x74, 0x00 }, 1}; // Keypad 6
    case 0x5f: return {{ 0x6c, 0x00 }, 1}; // Keypad 7
    case 0x60: return {{ 0x75, 0x00 }, 1}; // Keypad 8
    case 0x61: return {{ 0x7d, 0x00 }, 1}; // Keypad 9
    case 0x62: return {{ 0x70, 0x00 }, 1}; // Keypad 0
    case 0x63: return {{ 0x71, 0x00 }, 1}; // Keypad .

    default: return {{ 0x00, 0x00 }, 0};
  }
}

PcKeyboardController::ScancodeSequence PcKeyboardController::modifierToSet1(uint8_t modifierMask)
{
  switch (modifierMask) {
    case 0x01: return {{ 0x1d, 0x00 }, 1}; // Left Ctrl
    case 0x02: return {{ 0x2a, 0x00 }, 1}; // Left Shift
    case 0x04: return {{ 0x38, 0x00 }, 1}; // Left Alt
    case 0x08: return {{ 0xe0, 0x5b }, 2}; // Left GUI
    case 0x10: return {{ 0xe0, 0x1d }, 2}; // Right Ctrl
    case 0x20: return {{ 0x36, 0x00 }, 1}; // Right Shift
    case 0x40: return {{ 0xe0, 0x38 }, 2}; // Right Alt
    case 0x80: return {{ 0xe0, 0x5c }, 2}; // Right GUI
    default: return {{ 0x00, 0x00 }, 0};
  }
}


PcKeyboardController::ScancodeSequence PcKeyboardController::modifierToSet2(uint8_t modifierMask)
{
  switch (modifierMask) {
    case 0x01: return {{ 0x14, 0x00 }, 1}; // Left Ctrl
    case 0x02: return {{ 0x12, 0x00 }, 1}; // Left Shift
    case 0x04: return {{ 0x11, 0x00 }, 1}; // Left Alt
    case 0x08: return {{ 0xe0, 0x1f }, 2}; // Left GUI
    case 0x10: return {{ 0xe0, 0x14 }, 2}; // Right Ctrl
    case 0x20: return {{ 0x59, 0x00 }, 1}; // Right Shift
    case 0x40: return {{ 0xe0, 0x11 }, 2}; // Right Alt
    case 0x80: return {{ 0xe0, 0x27 }, 2}; // Right GUI
    default: return {{ 0x00, 0x00 }, 0};
  }
}

bool PcKeyboardController::usesTranslatedSet1Output() const
{
  return (m_commandByte & CommandByteTranslation) != 0 || m_scanCodeSet == 1;
}

void PcKeyboardController::handleKeyDown(uint8_t usage)
{
  bool const set1Output = usesTranslatedSet1Output();
  enqueueScancode(set1Output ? usageToSet1(usage) : usageToSet2(usage), false, !set1Output);
}

void PcKeyboardController::handleKeyUp(uint8_t usage)
{
  bool const set1Output = usesTranslatedSet1Output();
  enqueueScancode(set1Output ? usageToSet1(usage) : usageToSet2(usage), true, !set1Output);
}

void PcKeyboardController::handleModifierDown(uint8_t modifierMask)
{
  bool const set1Output = usesTranslatedSet1Output();
  enqueueScancode(set1Output ? modifierToSet1(modifierMask) : modifierToSet2(modifierMask), false, !set1Output);
}

void PcKeyboardController::handleModifierUp(uint8_t modifierMask)
{
  bool const set1Output = usesTranslatedSet1Output();
  enqueueScancode(set1Output ? modifierToSet1(modifierMask) : modifierToSet2(modifierMask), true, !set1Output);
}

void PcKeyboardController::enqueueScancode(ScancodeSequence sequence, bool release, bool set2Output)
{
  if (sequence.length == 0)
    return;

  if (set2Output) {
    if (sequence.length == 2 && sequence.bytes[0] == 0xe0) {
      enqueueByte(0xe0);
      if (release)
        enqueueByte(0xf0);
      enqueueByte(sequence.bytes[1]);
    } else {
      if (release)
        enqueueByte(0xf0);
      enqueueByte(sequence.bytes[0]);
    }
    return;
  }

  if (sequence.length == 2 && sequence.bytes[0] == 0xe0) {
    enqueueByte(0xe0);
    enqueueByte(release ? uint8_t(sequence.bytes[1] | 0x80) : sequence.bytes[1]);
  } else {
    enqueueByte(release ? uint8_t(sequence.bytes[0] | 0x80) : sequence.bytes[0]);
  }
}

void PcKeyboardController::enqueueByte(uint8_t value)
{
  if (m_queueCount == QueueSize)
    return;

  m_queue[m_queueWrite] = value;
  m_queueWrite = (m_queueWrite + 1) % QueueSize;
  ++m_queueCount;
  refreshIrq();
}

void PcKeyboardController::handleKeyboardCommand(uint8_t value)
{
  switch (value) {
    case 0xed: // set LEDs
      enqueueByte(Ack);
      m_pendingWrite = PendingWrite::KeyboardLeds;
      break;

    case 0xf2: // identify keyboard
      enqueueByte(Ack);
      enqueueByte(0xab);
      enqueueByte(0x83);
      break;

    case 0xf3: // set typematic rate/delay
      enqueueByte(Ack);
      m_pendingWrite = PendingWrite::KeyboardTypematic;
      break;

    case 0xf0: // set/read keyboard scan code set
      enqueueByte(Ack);
      m_pendingWrite = PendingWrite::KeyboardScanCodeSet;
      break;

    case 0xf4: // enable scanning
      m_keyboardDisabled = false;
      m_commandByte &= ~CommandByteKeyboardDisable;
      enqueueByte(Ack);
      applyDeferredHidReportIfEnabled();
      break;

    case 0xf5: // disable scanning and restore keyboard defaults
      m_keyboardDisabled = true;
      m_commandByte |= CommandByteKeyboardDisable;
      m_scanCodeSet = 2;
      clearPressedState();
      enqueueByte(Ack);
      break;

    case 0xf6: // restore keyboard defaults
      m_keyboardDisabled = false;
      m_commandByte &= ~CommandByteKeyboardDisable;
      m_scanCodeSet = 2;
      clearPressedState();
      enqueueByte(Ack);
      break;

    case 0xff: // reset keyboard
      enqueueByte(Ack);
      enqueueByte(0xaa);
      clearPressedState();
      break;

    default:
      enqueueByte(Resend);
      break;
  }
}

void PcKeyboardController::clearPressedState()
{
  m_lastModifiers = 0;
  memset(m_lastKeys, 0, sizeof(m_lastKeys));
  m_deferredReportValid = false;
  m_deferredModifiers = 0;
  memset(m_deferredKeys, 0, sizeof(m_deferredKeys));
}

void PcKeyboardController::refreshIrq()
{
  m_irqPending = (m_commandByte & CommandByteKeyboardIrq) && m_queueCount > 0;
}

} // namespace tabdos
