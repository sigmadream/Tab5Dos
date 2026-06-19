#pragma once

#include <stdint.h>

namespace tabdos {

class PcMachine;

class PcBios {
public:
  void reset();
  bool handleInterrupt(PcMachine & machine, int interruptNumber);
  void setMouseInstalled(bool installed);
  void setMouseState(uint16_t x, uint16_t y, uint16_t buttons);
  void setMouseSourceState(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t buttons);
  bool dispatchMouseCallback(PcMachine & machine);
  uint32_t timerDayTicks() const;
  void updateTimerBda(PcMachine & machine) const;
  void observeKeyboardScancode(PcMachine & machine, uint8_t rawScancode);

private:
  bool handleVideoInterrupt(PcMachine & machine);
  bool handleDiskInterrupt(PcMachine & machine);
  bool handleSerialInterrupt();
  bool handleKeyboardInterrupt(PcMachine & machine);
  bool handleTimerInterrupt(PcMachine & machine);
  bool handleSystemInterrupt(PcMachine & machine);
  bool handleClockInterrupt();
  bool handleMouseInterrupt();
  bool saveVideoState(PcMachine & machine, uint32_t buffer, uint16_t stateMask);
  bool restoreVideoState(PcMachine & machine, uint32_t buffer, uint16_t stateMask);
  void clampMousePosition();
  void clearMouseEventState();
  void updateMouseState(uint16_t x, uint16_t y, uint16_t buttons, bool recordMotion);
  uint64_t absoluteTimerTicks() const;

  static uint8_t toBcd(int value);
  void updateKeyboardFlags(PcMachine & machine);
  bool translateKeyboardScancode(PcMachine & machine, uint8_t rawScancode, uint16_t * key);
  bool fetchKey(PcMachine & machine, uint16_t * key);
  bool peekBdaKey(PcMachine & machine, uint16_t * key);
  bool popBdaKey(PcMachine & machine, uint16_t * key);
  void storeBdaKey(PcMachine & machine, uint16_t key);
  void clearTextScreen(PcMachine & machine, uint8_t attribute);
  void scrollTextWindow(PcMachine & machine,
                        uint8_t lines,
                        uint8_t attribute,
                        uint8_t top,
                        uint8_t left,
                        uint8_t bottom,
                        uint8_t right,
                        bool down);
  void scrollGraphicsWindow(PcMachine & machine,
                            uint8_t lines,
                            uint8_t color,
                            uint8_t top,
                            uint8_t left,
                            uint8_t bottom,
                            uint8_t right,
                            bool down);
  void scrollTextUp(PcMachine & machine, uint8_t attribute);
  bool loadUserFont(PcMachine & machine,
                    uint8_t bytesPerCharacter,
                    uint16_t firstCharacter,
                    uint16_t characterCount,
                    uint32_t source,
                    bool updateTextRenderer = true);
  void loadRomGraphicsFont(PcMachine & machine, uint8_t cellHeight);
  void writeTeletype(PcMachine & machine, uint8_t ch, uint8_t attribute);
  void writeGraphicsCharacter(PcMachine & machine, uint8_t ch, uint8_t color, uint8_t row, uint8_t column, uint8_t page, bool xorMode);
  uint16_t graphicsPageOffset(PcMachine const & machine, uint8_t page) const;
  uint8_t graphicsTextCellHeight(PcMachine const & machine) const;
  uint32_t textPageMemoryBase(PcMachine const & machine, uint8_t page) const;
  uint32_t activeTextMemoryBase(PcMachine const & machine) const;
  uint16_t activeTextColumns(PcMachine const & machine) const;
  uint8_t activeTextRows(PcMachine const & machine) const;
  uint8_t activeDisplayPage(PcMachine const & machine) const;
  void setCursor(PcMachine & machine, uint8_t page, uint8_t row, uint8_t column);
  int diskIndexForBiosDrive(uint8_t drive) const;
  uint16_t cursorOffset(PcMachine const & machine) const;
  uint16_t cursorOffsetForPage(PcMachine const & machine, uint8_t page) const;

  uint8_t m_cursorRow = 0;
  uint8_t m_cursorColumn = 0;
  uint8_t m_activeDisplayPage = 0;
  bool m_leftShift = false;
  bool m_rightShift = false;
  bool m_ctrl = false;
  bool m_alt = false;
  bool m_scrollLock = false;
  bool m_numLock = false;
  bool m_capsLock = false;
  bool m_insert = false;
  uint8_t m_dacColorPageMode = 0;
  bool m_defaultPaletteLoadingEnabled = true;
  bool m_grayScaleSummingEnabled = false;
  bool m_cursorEmulationEnabled = true;
  uint8_t m_userFont[256 * 16] = {};
  uint16_t m_mouseX = 0;
  uint16_t m_mouseY = 0;
  uint16_t m_mouseMinX = 0;
  uint16_t m_mouseMaxX = 639;
  uint16_t m_mouseMinY = 0;
  uint16_t m_mouseMaxY = 199;
  uint16_t m_mouseVisibleCount = 0;
  uint16_t m_mouseButtons = 0;
  uint16_t m_mousePressCount[3] = {};
  uint16_t m_mouseReleaseCount[3] = {};
  uint16_t m_mousePressX[3] = {};
  uint16_t m_mousePressY[3] = {};
  uint16_t m_mouseReleaseX[3] = {};
  uint16_t m_mouseReleaseY[3] = {};
  int16_t m_mouseMotionX = 0;
  int16_t m_mouseMotionY = 0;
  uint16_t m_mouseCallbackMask = 0;
  uint16_t m_mouseCallbackSegment = 0;
  uint16_t m_mouseCallbackOffset = 0;
  uint16_t m_mousePendingCallbackMask = 0;
  int16_t m_mousePendingCallbackMotionX = 0;
  int16_t m_mousePendingCallbackMotionY = 0;
  bool m_mouseInstalled = false;
  uint32_t m_timerBaseTicks = 0;
  uint64_t m_timerBaseMillis = 0;
};

} // namespace tabdos
