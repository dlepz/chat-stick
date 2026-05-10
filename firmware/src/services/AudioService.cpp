#include "AudioService.h"

#include "../Config.h"
#include "../diag/Log.h"
#include "../drivers/es8311/es8311.h"
#include <ESP_I2S.h>
#include <math.h>

namespace {
constexpr int kChunkSamples = MIC_SAMPLE_RATE * MIC_CHUNK_MS / 1000;
constexpr int kPlaybackChunkMs = 20;
constexpr int kPlaybackChunkSamples = PLAY_SAMPLE_RATE * kPlaybackChunkMs / 1000;
constexpr int kCodecI2cPort = 0;
constexpr es8311_mic_gain_t kMicGain = ES8311_MIC_GAIN_42DB;

I2SClass i2s;
es8311_handle_t codec = nullptr;
int currentSampleRate = 0;
int codecVolume = -1;
bool i2sReady = false;

int noteIndex(char note) {
  switch (toupper(note)) {
  case 'C':
    return 0;
  case 'D':
    return 2;
  case 'E':
    return 4;
  case 'F':
    return 5;
  case 'G':
    return 7;
  case 'A':
    return 9;
  case 'B':
    return 11;
  default:
    return -1;
  }
}

float noteFrequency(const String &noteToken) {
  if (noteToken.isEmpty()) {
    return 0.0f;
  }

  const char noteName = noteToken[0];
  if (toupper(noteName) == 'R') {
    return 0.0f;
  }

  int semitone = noteIndex(noteName);
  if (semitone < 0) {
    return -1.0f;
  }

  int index = 1;
  if (index < static_cast<int>(noteToken.length())) {
    if (noteToken[index] == '#') {
      semitone += 1;
      index++;
    } else if (noteToken[index] == 'b' || noteToken[index] == 'B') {
      semitone -= 1;
      index++;
    }
  }

  const int octave =
      index < static_cast<int>(noteToken.length()) ? noteToken.substring(index).toInt() : 4;
  const int midi = (octave + 1) * 12 + semitone;
  return 440.0f * powf(2.0f, (midi - 69) / 12.0f);
}

int codecVolumeFromLevel(int level) {
  return map(constrain(level, 0, 255), 0, 255, 0, 100);
}
} // namespace

AudioService::~AudioService() {
  stopPlaybackTask();
  if (_captureChunk) {
    free(_captureChunk);
    _captureChunk = nullptr;
  }
  if (_captureStereoChunk) {
    free(_captureStereoChunk);
    _captureStereoChunk = nullptr;
  }
  if (_playBuffer) {
    free(_playBuffer);
    _playBuffer = nullptr;
  }
  if (_stereoPlaybackChunk) {
    free(_stereoPlaybackChunk);
    _stereoPlaybackChunk = nullptr;
  }
  if (codec) {
    es8311_delete(codec);
    codec = nullptr;
  }
  if (_playbackWake) {
    vSemaphoreDelete(_playbackWake);
    _playbackWake = nullptr;
  }
  if (_playbackMutex) {
    vSemaphoreDelete(_playbackMutex);
    _playbackMutex = nullptr;
  }
  if (_audioMutex) {
    vSemaphoreDelete(_audioMutex);
    _audioMutex = nullptr;
  }
}

bool AudioService::init() {
  _audioMutex = xSemaphoreCreateRecursiveMutex();
  _playbackMutex = xSemaphoreCreateMutex();
  _playbackWake = xSemaphoreCreateBinary();
  if (!_audioMutex || !_playbackMutex || !_playbackWake) {
    Log::client("Audio", "playback sync allocation failed");
    return false;
  }

  _captureChunk = static_cast<int16_t *>(ps_malloc(_chunkBytes));
  _captureStereoChunk = static_cast<int16_t *>(ps_malloc(_stereoChunkBytes));
  _playBuffer = static_cast<uint8_t *>(ps_malloc(kMaxPlayBytes));
  _stereoPlaybackChunk =
      static_cast<int16_t *>(ps_malloc(kPlaybackChunkSamples * 2 * sizeof(int16_t)));
  _playCapacity = kMaxPlayBytes;

  if (!_captureChunk) {
    _captureChunk = static_cast<int16_t *>(malloc(_chunkBytes));
    if (_captureChunk) {
      Log::client("Audio", "using heap capture buffer fallback");
    }
  }

  if (!_captureStereoChunk) {
    _captureStereoChunk = static_cast<int16_t *>(malloc(_stereoChunkBytes));
    if (_captureStereoChunk) {
      Log::client("Audio", "using heap stereo capture buffer fallback");
    }
  }

  if (!_playBuffer) {
    _playBuffer = static_cast<uint8_t *>(malloc(kFallbackPlayBytes));
    if (_playBuffer) {
      _playCapacity = kFallbackPlayBytes;
      Log::client("Audio", "using heap playback buffer fallback bytes=%d",
                  _playCapacity);
    }
  }

  if (!_stereoPlaybackChunk) {
    _stereoPlaybackChunk = static_cast<int16_t *>(
        malloc(kPlaybackChunkSamples * 2 * sizeof(int16_t)));
  }

  if (!_captureChunk || !_captureStereoChunk || !_playBuffer ||
      !_stereoPlaybackChunk) {
    Log::client("Audio", "audio buffer allocation failed");
    return false;
  }

  pinMode(AUDIO_PA_ENABLE_PIN, OUTPUT);
  digitalWrite(AUDIO_PA_ENABLE_PIN, HIGH);

  if (!configureAudio(PLAY_SAMPLE_RATE)) {
    Log::client("Audio", "ES8311/I2S init failed");
    return false;
  }
  setVolume(_volume);
  Log::client("Audio", "capture chunk samples=%d bytes=%u", kChunkSamples,
              static_cast<unsigned>(_chunkBytes));
  Log::client("Audio", "playback buffer bytes=%d", _playCapacity);
  if (!startPlaybackTask()) {
    Log::client("Audio", "playback task start failed");
    return false;
  }
  return true;
}

void AudioService::setVolume(int level) {
  _volume = constrain(level, 0, 255);
  const int nextVolume = codecVolumeFromLevel(_volume);
  if (!takeAudioLock()) {
    return;
  }
  if (codec && codecVolume != nextVolume) {
    es8311_voice_volume_set(codec, nextVolume, nullptr);
    codecVolume = nextVolume;
  }
  releaseAudioLock();
}

bool AudioService::startRecording() {
  resetPlayback();
  if (!takeAudioLock()) {
    return false;
  }
  digitalWrite(AUDIO_PA_ENABLE_PIN, LOW);
  if (!configureAudio(MIC_SAMPLE_RATE)) {
    digitalWrite(AUDIO_PA_ENABLE_PIN, HIGH);
    setVolume(_volume);
    releaseAudioLock();
    return false;
  }
  if (!i2s.configureRX(MIC_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                       I2S_SLOT_MODE_STEREO, I2S_RX_TRANSFORM_NONE)) {
    configureAudio(PLAY_SAMPLE_RATE);
    digitalWrite(AUDIO_PA_ENABLE_PIN, HIGH);
    setVolume(_volume);
    releaseAudioLock();
    return false;
  }
  releaseAudioLock();
  return true;
}

void AudioService::stopRecording() {
  if (!takeAudioLock()) {
    return;
  }
  configureAudio(PLAY_SAMPLE_RATE);
  digitalWrite(AUDIO_PA_ENABLE_PIN, HIGH);
  setVolume(_volume);
  releaseAudioLock();
}

bool AudioService::captureChunk() {
  if (!takeAudioLock()) {
    return false;
  }
  if (!i2sReady) {
    releaseAudioLock();
    return false;
  }
  const size_t read =
      i2s.readBytes(reinterpret_cast<char *>(_captureStereoChunk),
                    _stereoChunkBytes);
  releaseAudioLock();
  if (read != _stereoChunkBytes) {
    return false;
  }

  const int samples = _chunkBytes / sizeof(int16_t);
  int64_t leftEnergy = 0;
  int64_t rightEnergy = 0;
  for (int i = 0; i < samples; i++) {
    leftEnergy += abs(static_cast<int>(_captureStereoChunk[i * 2]));
    rightEnergy += abs(static_cast<int>(_captureStereoChunk[i * 2 + 1]));
  }

  const int channel = rightEnergy > leftEnergy ? 1 : 0;
  int peak = 0;
  for (int i = 0; i < samples; i++) {
    const int16_t sample = _captureStereoChunk[i * 2 + channel];
    _captureChunk[i] = sample;
    peak = max(peak, abs(static_cast<int>(sample)));
  }
  _lastCaptureChannel = channel;
  _lastCaptureLeftAverageAbs =
      static_cast<int>(leftEnergy / max(1, samples));
  _lastCaptureRightAverageAbs =
      static_cast<int>(rightEnergy / max(1, samples));
  _lastCaptureAverageAbs = channel == 1 ? _lastCaptureRightAverageAbs
                                        : _lastCaptureLeftAverageAbs;
  _lastCapturePeak = peak;
  return true;
}

bool AudioService::playNamedSound(const String &name) {
  const String normalized = name;
  if (normalized.equalsIgnoreCase("beep")) {
    return playToneSequence("C6:120");
  }
  if (normalized.equalsIgnoreCase("success")) {
    return playToneSequence("C6:90 E6:90 G6:140");
  }
  if (normalized.equalsIgnoreCase("error")) {
    return playToneSequence("G4:120 R:40 C4:240");
  }
  if (normalized.equalsIgnoreCase("alert")) {
    return playToneSequence("A5:120 R:40 A5:120 R:40 A5:200");
  }
  if (normalized.equalsIgnoreCase("fanfare")) {
    return playToneSequence("C5:100 E5:100 G5:100 C6:260");
  }

  return false;
}

bool AudioService::playMelody(const String &melody) {
  return playToneSequence(melody);
}

void AudioService::resetPlayback() {
  if (!takePlaybackLock()) {
    return;
  }
  _playWritePos = 0;
  _playReadPos = 0;
  _playBufferedBytes = 0;
  _playbackStarted = false;
  _playChunkInFlight = false;
  releasePlaybackLock();
  wakePlaybackTask();
}

bool AudioService::queuePlayback(const uint8_t *data, size_t len) {
  if (len == 0) {
    return true;
  }

  if (!takePlaybackLock()) {
    return false;
  }
  const int freeBytes = _playCapacity - _playBufferedBytes;
  if (len > static_cast<size_t>(freeBytes)) {
    releasePlaybackLock();
    Log::client("Audio", "playback overflow dropping bytes=%u",
                static_cast<unsigned>(len));
    return false;
  }

  int remaining = static_cast<int>(len);
  int sourceOffset = 0;
  while (remaining > 0) {
    const int contiguous = min(remaining, _playCapacity - _playWritePos);
    memcpy(_playBuffer + _playWritePos, data + sourceOffset, contiguous);
    _playWritePos = (_playWritePos + contiguous) % _playCapacity;
    _playBufferedBytes += contiguous;
    sourceOffset += contiguous;
    remaining -= contiguous;
  }
  releasePlaybackLock();
  wakePlaybackTask();
  return true;
}

int AudioService::bufferedPlaybackBytes() const {
  if (!takePlaybackLock()) {
    return 0;
  }
  const int bytes = _playBufferedBytes;
  releasePlaybackLock();
  return bytes;
}

bool AudioService::playbackStarted() const {
  if (!takePlaybackLock()) {
    return false;
  }
  const bool started = _playbackStarted;
  releasePlaybackLock();
  return started;
}

void AudioService::markPlaybackStarted() {
  if (!takePlaybackLock()) {
    return;
  }
  _playbackStarted = true;
  releasePlaybackLock();
  wakePlaybackTask();
}

bool AudioService::advancePlayback() {
  wakePlaybackTask();
  return !playbackIdle();
}

bool AudioService::speakerBusy() const {
  if (!takePlaybackLock()) {
    return false;
  }
  const bool busy = _playChunkInFlight;
  releasePlaybackLock();
  return busy;
}

bool AudioService::playbackIdle() const {
  if (!takePlaybackLock()) {
    return true;
  }
  const bool idle = _playBufferedBytes == 0 && !_playChunkInFlight;
  releasePlaybackLock();
  return idle;
}

void AudioService::stopPlayback() {
  if (!takePlaybackLock()) {
    return;
  }
  _playbackStarted = false;
  _playReadPos = 0;
  _playWritePos = 0;
  _playBufferedBytes = 0;
  _playChunkInFlight = false;
  releasePlaybackLock();
  wakePlaybackTask();
}

bool AudioService::configureAudio(int sampleRate) {
  if (!takeAudioLock()) {
    return false;
  }
  if (i2sReady && currentSampleRate == sampleRate) {
    releaseAudioLock();
    return true;
  }

  i2sReady = false;
  currentSampleRate = 0;
  i2s.end();
  i2s.setPins(AUDIO_I2S_BCLK_PIN, AUDIO_I2S_WS_PIN, AUDIO_I2S_DOUT_PIN,
              AUDIO_I2S_DIN_PIN, AUDIO_I2S_MCLK_PIN);
  if (!i2s.begin(I2S_MODE_STD, sampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Log::client("Audio", "I2S begin failed");
    releaseAudioLock();
    return false;
  }

  if (!codec) {
    codec = es8311_create(kCodecI2cPort, ES8311_ADDRESS_0);
  }
  if (!codec) {
    Log::client("Audio", "ES8311 create failed");
    releaseAudioLock();
    return false;
  }

  const es8311_clock_config_t clockConfig = {
      .mclk_inverted = false,
      .sclk_inverted = false,
      .mclk_from_mclk_pin = true,
      .mclk_frequency = sampleRate * 256,
      .sample_frequency = sampleRate,
  };

  if (es8311_init(codec, &clockConfig, ES8311_RESOLUTION_16,
                  ES8311_RESOLUTION_16) != ESP_OK ||
      es8311_microphone_config(codec, false) != ESP_OK ||
      es8311_microphone_gain_set(codec, kMicGain) != ESP_OK) {
    Log::client("Audio", "ES8311 config failed");
    releaseAudioLock();
    return false;
  }

  currentSampleRate = sampleRate;
  codecVolume = -1;
  i2sReady = true;
  setVolume(_volume);
  releaseAudioLock();
  return true;
}

bool AudioService::takeAudioLock(TickType_t timeout) {
  return !_audioMutex ||
         xSemaphoreTakeRecursive(_audioMutex, timeout) == pdTRUE;
}

void AudioService::releaseAudioLock() {
  if (_audioMutex) {
    xSemaphoreGiveRecursive(_audioMutex);
  }
}

bool AudioService::takePlaybackLock(TickType_t timeout) const {
  return !_playbackMutex || xSemaphoreTake(_playbackMutex, timeout) == pdTRUE;
}

void AudioService::releasePlaybackLock() const {
  if (_playbackMutex) {
    xSemaphoreGive(_playbackMutex);
  }
}

bool AudioService::startPlaybackTask() {
  if (_playbackTask) {
    return true;
  }
  _playbackTaskRunning = true;
  const BaseType_t created =
      xTaskCreatePinnedToCore(&AudioService::playbackTaskTrampoline,
                              "audio_play", 4096, this, 3, &_playbackTask, 0);
  if (created != pdPASS) {
    _playbackTask = nullptr;
    _playbackTaskRunning = false;
    return false;
  }
  return true;
}

void AudioService::stopPlaybackTask() {
  if (!_playbackTask) {
    return;
  }
  _playbackTaskRunning = false;
  wakePlaybackTask();
  for (int i = 0; i < 100 && _playbackTask; i++) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void AudioService::wakePlaybackTask() {
  if (_playbackWake) {
    xSemaphoreGive(_playbackWake);
  }
}

void AudioService::playbackTaskTrampoline(void *ctx) {
  static_cast<AudioService *>(ctx)->playbackTaskLoop();
}

void AudioService::playbackTaskLoop() {
  while (_playbackTaskRunning) {
    xSemaphoreTake(_playbackWake, pdMS_TO_TICKS(20));
    while (_playbackTaskRunning && playAvailableChunk()) {
      taskYIELD();
    }
  }
  _playbackTask = nullptr;
  vTaskDelete(nullptr);
}

bool AudioService::playAvailableChunk() {
  if (!takePlaybackLock()) {
    return false;
  }
  int available = _playbackStarted ? _playBufferedBytes : 0;
  if (available <= 1) {
    releasePlaybackLock();
    return false;
  }

  const int maxMonoBytes =
      kPlaybackChunkSamples * static_cast<int>(sizeof(int16_t));
  int monoBytes = min(available, maxMonoBytes);
  monoBytes -= monoBytes % static_cast<int>(sizeof(int16_t));
  const int monoSamples = monoBytes / static_cast<int>(sizeof(int16_t));
  for (int i = 0; i < monoSamples; i++) {
    const int byteIndex = (_playReadPos + i * 2) % _playCapacity;
    const uint16_t lo = _playBuffer[byteIndex];
    const uint16_t hi = _playBuffer[(byteIndex + 1) % _playCapacity];
    const int16_t sample = static_cast<int16_t>(lo | (hi << 8));
    _stereoPlaybackChunk[i * 2] = sample;
    _stereoPlaybackChunk[i * 2 + 1] = sample;
  }
  _playReadPos = (_playReadPos + monoBytes) % _playCapacity;
  _playBufferedBytes -= monoBytes;
  if (_playBufferedBytes == 0) {
    _playReadPos = 0;
    _playWritePos = 0;
  }
  _playChunkInFlight = true;
  releasePlaybackLock();

  if (!takeAudioLock()) {
    if (takePlaybackLock()) {
      _playChunkInFlight = false;
      releasePlaybackLock();
    }
    return false;
  }
  if (!configureAudio(PLAY_SAMPLE_RATE)) {
    if (takePlaybackLock()) {
      _playChunkInFlight = false;
      releasePlaybackLock();
    }
    releaseAudioLock();
    return false;
  }
  digitalWrite(AUDIO_PA_ENABLE_PIN, HIGH);

  const size_t stereoBytes = monoSamples * 2 * sizeof(int16_t);
  const bool ok =
      i2s.write(reinterpret_cast<const uint8_t *>(_stereoPlaybackChunk),
                stereoBytes) == stereoBytes;
  if (takePlaybackLock()) {
    _playChunkInFlight = false;
    releasePlaybackLock();
  }
  releaseAudioLock();
  if (!ok) {
    Log::client("Audio", "playback write failed bytes=%u",
                static_cast<unsigned>(stereoBytes));
  }
  return ok;
}

bool AudioService::playToneSequence(const String &sequence) {
  if (sequence.isEmpty()) {
    return false;
  }

  stopPlayback();
  if (!takeAudioLock()) {
    return false;
  }
  if (!configureAudio(PLAY_SAMPLE_RATE)) {
    releaseAudioLock();
    return false;
  }
  digitalWrite(AUDIO_PA_ENABLE_PIN, HIGH);

  int start = 0;
  bool played = false;
  while (start < static_cast<int>(sequence.length())) {
    while (start < static_cast<int>(sequence.length()) &&
           (sequence[start] == ' ' || sequence[start] == ',')) {
      start++;
    }
    if (start >= static_cast<int>(sequence.length())) {
      break;
    }

    int end = start;
    while (end < static_cast<int>(sequence.length()) && sequence[end] != ' ' &&
           sequence[end] != ',') {
      end++;
    }

    const String token = sequence.substring(start, end);
    const int divider = token.indexOf(':');
    const String noteToken = divider >= 0 ? token.substring(0, divider) : token;
    const int durationMs =
        divider >= 0 ? max(10, static_cast<int>(token.substring(divider + 1).toInt())) : 200;
    const float frequency = noteFrequency(noteToken);

    if (frequency < 0.0f) {
      start = end + 1;
      continue;
    }

    int remainingSamples = PLAY_SAMPLE_RATE * durationMs / 1000;
    float phase = 0.0f;
    const float phaseStep = 2.0f * PI * frequency / PLAY_SAMPLE_RATE;
    const float amplitude = 16000.0f * (_volume / 255.0f);
    while (remainingSamples > 0) {
      const int samples = min(remainingSamples, kPlaybackChunkSamples);
      for (int i = 0; i < samples; i++) {
        const int16_t sample =
            frequency == 0.0f ? 0 : static_cast<int16_t>(sinf(phase) * amplitude);
        _stereoPlaybackChunk[i * 2] = sample;
        _stereoPlaybackChunk[i * 2 + 1] = sample;
        phase += phaseStep;
        if (phase > 2.0f * PI) {
          phase -= 2.0f * PI;
        }
      }
      i2s.write(reinterpret_cast<const uint8_t *>(_stereoPlaybackChunk),
                samples * 2 * sizeof(int16_t));
      remainingSamples -= samples;
    }

    played = true;
    start = end + 1;
  }

  releaseAudioLock();
  return played;
}
