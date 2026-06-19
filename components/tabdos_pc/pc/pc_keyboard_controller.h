#pragma once

#include <stddef.h>
#include <stdint.h>

namespace tabdos {

class PcKeyboardController {
public:
  static constexpr uint8_t StatusOutputBufferFull = 0x01;
  static constexpr uint8_t StatusSystemFlag       = 0x04;
  static constexpr uint8_t StatusInhibitSwitch    = 0x10;

  PcKeyboardController();

  void reset();

  void onHidBootKeyboardReport(uint8_t modifiers, uint8_t const keycodes[6]);
  void onUsbDisconnected();

  uint8_t readDataPort();
  uint8_t readStatusPort() const;
  void writeDataPort(uint8_t value);
  void writeCommandPort(uint8_t value);

  bool irqPending() const { return m_irqPending; }
  void acknowledgeIrq();

  void getLeds(bool * numLock, bool * capsLock, bool * scrollLock) const;
  void setLeds(bool numLock, bool capsLock, bool scrollLock);

  int queuedByteCount() const { return m_queueCount; }
  bool keyboardEnabled() const { return !m_keyboardDisabled; }

private:
  enum class PendingWrite : uint8_t {
    None,
    CommandByte,
    KeyboardLeds,
    KeyboardTypematic,
    KeyboardScanCodeSet,
    OutputBuffer,
  };

  struct ScancodeSequence {
    uint8_t bytes[2];
    uint8_t length;
  };

  static constexpr int QueueSize = 128;

  static bool isValidBootKeycode(uint8_t usage);
  static bool containsKey(uint8_t const keycodes[6], uint8_t usage);
  static ScancodeSequence usageToSet1(uint8_t usage);
  static ScancodeSequence usageToSet2(uint8_t usage);
  static ScancodeSequence modifierToSet1(uint8_t modifierMask);
  static ScancodeSequence modifierToSet2(uint8_t modifierMask);

  void handleKeyDown(uint8_t usage);
  void handleKeyUp(uint8_t usage);
  void handleModifierDown(uint8_t modifierMask);
  void handleModifierUp(uint8_t modifierMask);
  void processHidBootKeyboardReport(uint8_t modifiers, uint8_t const keycodes[6]);
  void rememberDeferredHidReport(uint8_t modifiers, uint8_t const keycodes[6]);
  void applyDeferredHidReportIfEnabled();
  void enqueueScancode(ScancodeSequence sequence, bool release, bool set2Output);
  bool usesTranslatedSet1Output() const;
  void enqueueByte(uint8_t value);
  void handleKeyboardCommand(uint8_t value);
  void clearPressedState();
  void refreshIrq();

  uint8_t m_commandByte;
  bool m_keyboardDisabled;
  bool m_numLockLED;
  bool m_capsLockLED;
  bool m_scrollLockLED;
  uint8_t m_scanCodeSet;
  bool m_irqPending;
  PendingWrite m_pendingWrite;

  uint8_t m_lastModifiers;
  uint8_t m_lastKeys[6];
  bool m_deferredReportValid;
  uint8_t m_deferredModifiers;
  uint8_t m_deferredKeys[6];

  uint8_t m_queue[QueueSize];
  int m_queueRead;
  int m_queueWrite;
  int m_queueCount;
};

} // namespace tabdos
