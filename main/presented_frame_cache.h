#pragma once

#include <stdint.h>

// Tracks the single frame that is currently visible on the shared panel. Text
// and graphics hashes describe different inputs, so a hash is reusable only
// while the corresponding presentation path remains active.
class PresentedFrameCache {
public:
  bool matchesText(uint64_t hash) const { return m_kind == Kind::Text && hash == m_hash; }
  bool matchesGraphics(uint64_t hash) const { return m_kind == Kind::Graphics && hash == m_hash; }

  void commitText(uint64_t hash)
  {
    m_kind = Kind::Text;
    m_hash = hash;
  }

  void commitGraphics(uint64_t hash)
  {
    m_kind = Kind::Graphics;
    m_hash = hash;
  }

  void invalidate() { m_kind = Kind::None; }

private:
  enum class Kind { None, Text, Graphics };

  Kind m_kind = Kind::None;
  uint64_t m_hash = 0;
};
