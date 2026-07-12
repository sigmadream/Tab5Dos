#pragma once

#include <stdint.h>

namespace tabdos {

// Display-neutral description of the resolved INT 33h mouse cursor.
struct MouseCursorOverlay {
  bool visible = false;
  bool textCell = false;
  int x = 0;
  int y = 0;
  int cellWidth = 0;
  int cellHeight = 0;
  uint16_t screenMask[16] = {};
  uint16_t cursorMask[16] = {};
};

} // namespace tabdos
