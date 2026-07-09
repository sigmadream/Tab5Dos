#include "pc_opl2.h"

#include <math.h>
#include <string.h>

namespace tabdos {

namespace {

// Two-operator melodic channel to operator-register-offset mapping.
constexpr int kModOffset[9] = {0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12};
constexpr int kCarOffset[9] = {0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x15};

// MULT register (0..15) to frequency multiplier.
constexpr float kMultiplier[16] = {0.5f, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 12, 12, 15, 15};

constexpr float kTwoPi = 6.28318530717958647692f;
constexpr uint32_t kOplNativeRate = 49716; // YM3812 sample rate for fnum math

float attenuationFromTotalLevel(uint8_t tl)
{
  // Total level is 0.75 dB per step, 0 = loudest.
  return powf(10.0f, -0.75f * static_cast<float>(tl & 0x3f) / 20.0f);
}

float sustainLevel(uint8_t sl)
{
  if ((sl & 0x0f) == 0x0f)
    return 0.0f; // SL 15 is defined as maximum attenuation (silence)
  return powf(10.0f, -3.0f * static_cast<float>(sl & 0x0f) / 20.0f);
}

} // namespace

Opl2::Opl2()
{
  reset();
}

void Opl2::reset()
{
  memset(m_registers, 0, sizeof(m_registers));
  m_address = 0;
  m_status = 0;
  for (int i = 0; i < OperatorSlots; ++i)
    m_operators[i] = Operator{0.0f, 0.0f, EnvPhase::Off, 0.0f};
  for (int c = 0; c < Channels; ++c)
    m_channelKeyOn[c] = false;
  m_timer1Running = false;
  m_timer2Running = false;
  m_timer1Masked = false;
  m_timer2Masked = false;
  m_timer1StartMicros = 0;
  m_timer2StartMicros = 0;
}

void Opl2::writeData(uint8_t value, uint64_t nowMicros)
{
  writeRegister(m_address, value, nowMicros);
}

void Opl2::snapshotRegisters(uint8_t out[256]) const
{
  memcpy(out, m_registers, sizeof(m_registers));
}

void Opl2::writeRegister(uint8_t reg, uint8_t value, uint64_t nowMicros)
{
  m_registers[reg] = value;

  if (reg == 0x04) { // timer control
    if (value & 0x80) {
      // IRQ/status reset: clear overflow flags, timers keep their settings.
      m_status = 0;
      return;
    }
    m_timer1Masked = (value & 0x40) != 0;
    m_timer2Masked = (value & 0x20) != 0;
    bool const start1 = (value & 0x01) != 0;
    bool const start2 = (value & 0x02) != 0;
    // Stamp the start time at the moment the timer is enabled so the mandated
    // >=80us wait before the detection status read counts against a real
    // interval. A rising edge (re)starts the count; disabling stops it.
    if (start1 && !m_timer1Running)
      m_timer1StartMicros = nowMicros;
    if (start2 && !m_timer2Running)
      m_timer2StartMicros = nowMicros;
    m_timer1Running = start1;
    m_timer2Running = start2;
    return;
  }

  // Key-on/off is handled by the audio thread via register-snapshot edge
  // detection in render(); no DSP state is mutated on the register-write path.
}

void Opl2::keyOnChannel(int channel, bool on)
{
  if (channel < 0 || channel >= Channels)
    return;
  if (on == m_channelKeyOn[channel])
    return;
  m_channelKeyOn[channel] = on;
  int const offsets[2] = {kModOffset[channel], kCarOffset[channel]};
  for (int i = 0; i < 2; ++i) {
    Operator & op = m_operators[offsets[i]];
    if (on) {
      op.envPhase = EnvPhase::Attack;
      op.phase = 0.0f;
      op.feedback = 0.0f;
    } else if (op.envPhase != EnvPhase::Off) {
      op.envPhase = EnvPhase::Release;
    }
  }
}

void Opl2::advanceTimers(uint64_t nowMicros)
{
  // Start times are stamped when the timer is enabled (see writeRegister), so
  // the overflow flag latches only after a real interval has elapsed.
  if (m_timer1Running && nowMicros >= m_timer1StartMicros) {
    uint64_t const period = static_cast<uint64_t>(256 - m_registers[0x02]) * 80; // 80 us units
    if (period > 0 && nowMicros - m_timer1StartMicros >= period) {
      m_status |= 0x40; // timer 1 overflow flag
      if (!m_timer1Masked)
        m_status |= 0x80;
    }
  }
  if (m_timer2Running && nowMicros >= m_timer2StartMicros) {
    uint64_t const period = static_cast<uint64_t>(256 - m_registers[0x03]) * 320; // 320 us units
    if (period > 0 && nowMicros - m_timer2StartMicros >= period) {
      m_status |= 0x20; // timer 2 overflow flag
      if (!m_timer2Masked)
        m_status |= 0x80;
    }
  }
}

uint8_t Opl2::readStatus(uint64_t nowMicros)
{
  advanceTimers(nowMicros);
  return m_status;
}

float Opl2::waveform(uint8_t select, float phase)
{
  phase -= floorf(phase); // wrap to 0..1
  float const s = sinf(kTwoPi * phase);
  switch (select & 0x03) {
    case 1: // half sine: negative half muted
      return phase < 0.5f ? s : 0.0f;
    case 2: // absolute sine
      return fabsf(s);
    case 3: { // quarter (pulse) sine
      float p = phase - 0.5f * floorf(phase / 0.5f);
      return p < 0.25f ? sinf(kTwoPi * p) : 0.0f;
    }
    default:
      return s;
  }
}

float Opl2::attackStep(uint8_t rate, uint32_t sampleRate)
{
  if (rate == 0)
    return 0.0f;
  float const seconds = 4.0f * powf(0.5f, static_cast<float>(rate));
  return 1.0f / (static_cast<float>(sampleRate) * seconds);
}

float Opl2::fallStep(uint8_t rate, uint32_t sampleRate)
{
  if (rate == 0)
    return 0.0f;
  float const seconds = 8.0f * powf(0.5f, static_cast<float>(rate));
  return 1.0f / (static_cast<float>(sampleRate) * seconds);
}

bool Opl2::anyKeyOn() const
{
  for (int i = 0; i < OperatorSlots; ++i)
    if (m_operators[i].envPhase != EnvPhase::Off)
      return true;
  return false;
}

bool Opl2::render(uint8_t const * regs, int16_t * out, int frames, uint32_t sampleRate)
{
  if (!regs || !out || frames <= 0 || sampleRate == 0)
    return false;

  // Detect key-on/off edges from the register snapshot (B0-B8 bit 5). This is
  // the only place DSP state is keyed, keeping the audio thread free of the
  // machine mutex; it quantizes key events to one render chunk.
  for (int c = 0; c < Channels; ++c)
    keyOnChannel(c, (regs[0xb0 + c] & 0x20) != 0);

  if (!anyKeyOn())
    return false;

  bool const waveformEnable = (regs[0x01] & 0x20) != 0;

  // Per-channel precomputed parameters (register state is constant for a chunk).
  struct ChannelParams {
    bool active;
    bool additive;
    float feedbackScale;
    int modOffset;
    int carOffset;
    float modPhaseInc;
    float carPhaseInc;
    float modAtten;
    float carAtten;
    uint8_t modWave;
    uint8_t carWave;
    float modAttack, modDecay, modRelease, modSustain;
    bool modEgSustain;
    float carAttack, carDecay, carRelease, carSustain;
    bool carEgSustain;
  };

  ChannelParams params[Channels];
  bool anyActive = false;
  for (int c = 0; c < Channels; ++c) {
    ChannelParams & p = params[c];
    p.modOffset = kModOffset[c];
    p.carOffset = kCarOffset[c];
    p.active = m_operators[p.modOffset].envPhase != EnvPhase::Off ||
               m_operators[p.carOffset].envPhase != EnvPhase::Off;
    if (!p.active)
      continue;
    anyActive = true;

    uint16_t const fnum = static_cast<uint16_t>(regs[0xa0 + c] | ((regs[0xb0 + c] & 0x03) << 8));
    uint8_t const block = static_cast<uint8_t>((regs[0xb0 + c] >> 2) & 0x07);
    float const baseFreq = static_cast<float>(fnum) * static_cast<float>(kOplNativeRate) /
                           static_cast<float>(1u << (20 - block));

    uint8_t const fb = static_cast<uint8_t>((regs[0xc0 + c] >> 1) & 0x07);
    p.additive = (regs[0xc0 + c] & 0x01) != 0;
    p.feedbackScale = fb == 0 ? 0.0f : powf(2.0f, static_cast<float>(fb)) / 16.0f;

    auto operatorParams = [&](int offset, float & inc, float & atten, uint8_t & wave,
                              float & attack, float & decay, float & release, float & sustain,
                              bool & egSustain) {
      uint8_t const mult = regs[0x20 + offset] & 0x0f;
      inc = baseFreq * kMultiplier[mult] / static_cast<float>(sampleRate);
      atten = attenuationFromTotalLevel(regs[0x40 + offset]);
      wave = waveformEnable ? static_cast<uint8_t>(regs[0xe0 + offset] & 0x03) : 0;
      attack = attackStep(static_cast<uint8_t>(regs[0x60 + offset] >> 4), sampleRate);
      decay = fallStep(static_cast<uint8_t>(regs[0x60 + offset] & 0x0f), sampleRate);
      release = fallStep(static_cast<uint8_t>(regs[0x80 + offset] & 0x0f), sampleRate);
      sustain = sustainLevel(static_cast<uint8_t>(regs[0x80 + offset] >> 4));
      egSustain = (regs[0x20 + offset] & 0x20) != 0;
    };
    operatorParams(p.modOffset, p.modPhaseInc, p.modAtten, p.modWave,
                   p.modAttack, p.modDecay, p.modRelease, p.modSustain, p.modEgSustain);
    operatorParams(p.carOffset, p.carPhaseInc, p.carAtten, p.carWave,
                   p.carAttack, p.carDecay, p.carRelease, p.carSustain, p.carEgSustain);
  }

  if (!anyActive)
    return false;

  // Decay uses the decay rate (DR); the release phase and the percussive
  // (EGT=0) tail past the sustain level use the release rate (RR).
  auto stepEnvelope = [](Operator & op, float attack, float decay, float release, float sustain,
                         bool egSustain) {
    switch (op.envPhase) {
      case EnvPhase::Attack:
        op.envLevel += attack;
        if (attack <= 0.0f)
          op.envLevel = 1.0f; // instantaneous attack when rate is maximal-ish
        if (op.envLevel >= 1.0f) {
          op.envLevel = 1.0f;
          op.envPhase = EnvPhase::Decay;
        }
        break;
      case EnvPhase::Decay:
        op.envLevel -= decay;
        if (op.envLevel <= sustain) {
          op.envLevel = sustain;
          op.envPhase = EnvPhase::Sustain;
        }
        break;
      case EnvPhase::Sustain:
        if (!egSustain) {
          op.envLevel -= release;
          if (op.envLevel <= 0.0f) {
            op.envLevel = 0.0f;
            op.envPhase = EnvPhase::Off;
          }
        }
        break;
      case EnvPhase::Release:
        op.envLevel -= release;
        if (op.envLevel <= 0.0f) {
          op.envLevel = 0.0f;
          op.envPhase = EnvPhase::Off;
        }
        break;
      case EnvPhase::Off:
        break;
    }
  };

  for (int i = 0; i < frames; ++i) {
    float sample = 0.0f;
    for (int c = 0; c < Channels; ++c) {
      ChannelParams & p = params[c];
      if (!p.active)
        continue;
      Operator & mod = m_operators[p.modOffset];
      Operator & car = m_operators[p.carOffset];

      stepEnvelope(mod, p.modAttack, p.modDecay, p.modRelease, p.modSustain, p.modEgSustain);
      float const modInput = p.feedbackScale * mod.feedback;
      float const modOut = waveform(p.modWave, mod.phase + modInput) * mod.envLevel * p.modAtten;
      mod.feedback = modOut;
      mod.phase += p.modPhaseInc;
      if (mod.phase >= 1.0f)
        mod.phase -= floorf(mod.phase);

      stepEnvelope(car, p.carAttack, p.carDecay, p.carRelease, p.carSustain, p.carEgSustain);
      float carOut;
      if (p.additive) {
        carOut = waveform(p.carWave, car.phase) * car.envLevel * p.carAtten;
        sample += modOut + carOut;
      } else {
        carOut = waveform(p.carWave, car.phase + modOut * 0.5f) * car.envLevel * p.carAtten;
        sample += carOut;
      }
      car.phase += p.carPhaseInc;
      if (car.phase >= 1.0f)
        car.phase -= floorf(car.phase);

      // Deactivate the channel for the rest of the chunk once both operators go
      // silent, so a released voice stops adding cost.
      if (mod.envPhase == EnvPhase::Off && car.envPhase == EnvPhase::Off)
        p.active = false;
    }

    float scaled = sample * 3000.0f;
    if (scaled > 32767.0f)
      scaled = 32767.0f;
    else if (scaled < -32768.0f)
      scaled = -32768.0f;
    out[i] = static_cast<int16_t>(scaled);
  }
  return true;
}

} // namespace tabdos
