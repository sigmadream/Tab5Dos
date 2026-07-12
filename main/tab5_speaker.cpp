#include "tab5_speaker.h"

#include "esp_log.h"

#include <math.h>

namespace {
static char const * TAG = "tab5_speaker";
static constexpr int OutputVolume = 70;    // codec output volume (0-100)
static constexpr int16_t ToneAmplitude = 7000; // headroom below full scale
}

Tab5Speaker::~Tab5Speaker()
{
  if (m_dev)
    esp_codec_dev_close(m_dev);
}

esp_err_t Tab5Speaker::init()
{
  esp_err_t err = bsp_audio_init(nullptr);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "bsp_audio_init failed: %s", esp_err_to_name(err));
    return err;
  }

  m_dev = bsp_audio_codec_speaker_init();
  if (!m_dev) {
    ESP_LOGW(TAG, "speaker codec init failed; PC speaker audio disabled");
    return ESP_FAIL;
  }

  esp_codec_dev_set_out_vol(m_dev, OutputVolume);

  esp_codec_dev_sample_info_t fs = {};
  fs.bits_per_sample = 16;
  fs.channel = 2;
  fs.channel_mask = 0;
  fs.sample_rate = SampleRate;
  int const rc = esp_codec_dev_open(m_dev, &fs);
  if (rc != 0) {
    ESP_LOGW(TAG, "speaker codec open failed: %d", rc);
    esp_codec_dev_close(m_dev);
    m_dev = nullptr;
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "PC speaker audio ready: %d Hz stereo", SampleRate);
  return ESP_OK;
}

void Tab5Speaker::playChunk(uint32_t frequencyHz, int16_t const * oplMono)
{
  if (!m_dev)
    return;

  // Frequencies above Nyquist would only produce aliasing noise; treat the tone
  // as silent instead. Very low counts still render fine as a slow square wave.
  bool const squareSilent = frequencyHz == 0 || frequencyHz >= static_cast<uint32_t>(SampleRate / 2);
  double const step = squareSilent ? 0.0 : static_cast<double>(frequencyHz) / SampleRate;
  if (squareSilent)
    m_phase = 0.0;

  for (int i = 0; i < ChunkFrames; ++i) {
    int32_t sample = 0;
    if (!squareSilent) {
      sample += m_phase < 0.5 ? ToneAmplitude : -ToneAmplitude;
      m_phase += step;
      if (m_phase >= 1.0)
        m_phase -= 1.0;
    }
    if (oplMono)
      sample += oplMono[i];
    if (sample > 32767)
      sample = 32767;
    else if (sample < -32768)
      sample = -32768;
    m_buffer[i * 2] = static_cast<int16_t>(sample);
    m_buffer[i * 2 + 1] = static_cast<int16_t>(sample);
  }

  esp_codec_dev_write(m_dev, m_buffer, static_cast<int>(sizeof(m_buffer)));
}
