#pragma once

#include <stdint.h>

namespace tabdos {

// Compact Yamaha YM3812 (OPL2) emulation: the two timers and status register are
// modelled accurately so AdLib detection succeeds, and a 9-channel two-operator
// FM synthesizer renders the music. The synthesizer is hardware independent and
// generates mono samples at an arbitrary output rate; the host mixes and plays
// them. It is not sample-exact against real silicon but reproduces melodic FM
// voices (feedback, FM/AM connection, ADSR envelopes, the four OPL2 waveforms).
class Opl2 {
public:
  struct RegisterSnapshot {
    uint8_t registers[256];
    uint32_t resetGeneration;
  };

  Opl2();

  // Reset front-end registers immediately. The audio-owned synthesis state is
  // cleared when the next snapshot generation reaches render().
  void reset();

  // ---- Front-end: touched by the emulated CPU under the machine mutex. ----
  // Port interface: 388h selects a register (writeAddress) and reads status;
  // 389h writes the selected register's data (writeData). nowMicros lets a
  // timer-start write stamp its start time immediately, so the guest's
  // start -> wait -> read-status detection sequence measures a real interval.
  void writeAddress(uint8_t reg) { m_address = reg; }
  void writeData(uint8_t value, uint64_t nowMicros);
  uint8_t readStatus(uint64_t nowMicros);
  // Copy the register file for the audio thread to synthesize from without
  // holding the machine mutex during the (expensive) FM render.
  void snapshotRegisters(RegisterSnapshot * out) const;

  // ---- Back-end: touched only by the audio thread (no mutex). ----
  // Synthesize `frames` mono int16 samples at `sampleRate` from a register
  // snapshot taken by snapshotRegisters(). Key-on/off edges are detected by
  // comparing the snapshot's B0-B8 registers against the previous chunk, so no
  // register write path mutates the DSP state. Returns false (buffer untouched)
  // when no operator is audible, so the host can idle.
  bool render(RegisterSnapshot const & snapshot, int16_t * out, int frames, uint32_t sampleRate);

  bool anyKeyOn() const;

private:
  static constexpr int OperatorSlots = 22; // register offsets 0x00..0x15 (with gaps)
  static constexpr int Channels = 9;

  enum class EnvPhase { Off, Attack, Decay, Sustain, Release };

  // ESP32-P4 has a single-precision FPU only, so the synthesis hot path uses
  // float throughout to avoid software-emulated doubles.
  struct Operator {
    float phase;
    float envLevel;    // linear 0..1
    EnvPhase envPhase;
    float feedback;    // previous output for the modulator feedback path
  };

  void writeRegister(uint8_t reg, uint8_t value, uint64_t nowMicros);
  void advanceTimers(uint64_t nowMicros);
  void keyOnChannel(int channel, bool on);
  void resetSynthesisState();

  static float waveform(uint8_t select, float phase);
  static float attackStep(uint8_t rate, uint32_t sampleRate);
  static float fallStep(uint8_t rate, uint32_t sampleRate);

  uint8_t m_registers[256];
  uint8_t m_address;
  uint8_t m_status;
  uint32_t m_resetGeneration;
  Operator m_operators[OperatorSlots];
  bool m_channelKeyOn[Channels];
  uint32_t m_renderGeneration;

  bool m_timer1Running;
  bool m_timer2Running;
  bool m_timer1Masked;
  bool m_timer2Masked;
  uint64_t m_timer1StartMicros;
  uint64_t m_timer2StartMicros;
};

} // namespace tabdos
