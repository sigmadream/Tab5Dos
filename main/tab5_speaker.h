#pragma once

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_err.h"

#include <stdint.h>

// Drives the Tab5 speaker codec to reproduce the emulated PC speaker (PIT
// channel 2 square wave). The emulator core stays hardware independent; this
// class turns a target frequency into audio chunks written to esp_codec_dev.
class Tab5Speaker {
public:
  Tab5Speaker() = default;
  ~Tab5Speaker();

  Tab5Speaker(Tab5Speaker const &) = delete;
  Tab5Speaker & operator=(Tab5Speaker const &) = delete;

  esp_err_t init();
  bool ready() const { return m_dev != nullptr; }

  // Render and play one chunk mixing the PC speaker square wave at frequencyHz
  // (0 = no tone) with an optional OPL2 mono buffer of ChunkFrames samples
  // (oplMono may be null when the FM synth is idle). Blocks for roughly one
  // chunk duration, pacing the caller to real time.
  void playChunk(uint32_t frequencyHz, int16_t const * oplMono);

  static constexpr int SampleRate = 32000;
  static constexpr int ChunkFrames = 512; // ~16 ms per chunk

private:
  esp_codec_dev_handle_t m_dev = nullptr;
  double m_phase = 0.0;
  int16_t m_buffer[ChunkFrames * 2] = {}; // interleaved stereo
};
