// Prevent Windows min/max macros from conflicting with std::min/std::max
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "waveform_extractor.h"
#include "common.h"
#include "player.h"
#include "soloud.h"
#include "soloud_wav.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <atomic>

// External reference to the global player instance from bindings.cpp
extern std::unique_ptr<Player> player;

// Callback for async waveform extraction
static WaveformExtractedCallback g_waveformCallback = nullptr;

extern "C" {

FFI_PLUGIN_EXPORT void setWaveformExtractedCallback(WaveformExtractedCallback callback) {
    g_waveformCallback = callback;
}

FFI_PLUGIN_EXPORT void clearWaveformExtractedCallback() {
    g_waveformCallback = nullptr;
}

/// Extract waveform samples from an already-loaded audio source.
int extractSamplesFromLoadedSource(unsigned int hash, float startTime,
                                   float endTime,
                                   unsigned long numSamplesNeeded, bool average,
                                   float *pSamples) {
  // Use the global player pointer from bindings.cpp
  Player *p = player.get();
  if (p == nullptr || !p->isInited()) {
    return 5;
  }

  // Find the sound by hash
  ActiveSound *activeSound = p->findByHash(hash);
  if (activeSound == nullptr) {
    return 1;
  }

  SoLoud::AudioSource *source = activeSound->sound.get();
  if (source == nullptr) {
    return 1;
  }

  // Currently we only support extracting from Wav objects (loaded into memory)
  if (activeSound->soundType != TYPE_WAV) {
    return 2;
  }

  SoLoud::Wav *wav = static_cast<SoLoud::Wav *>(source);
  if (wav->mData == nullptr) {
    return 3;
  }

  unsigned int channels = wav->mChannels;
  float sampleRate = wav->mBaseSamplerate;
  unsigned int totalSamples = wav->mSampleCount * channels;

  // Calculate sample range
  unsigned int startSample = 0;
  unsigned int endSample = totalSamples;

  if (startTime > 0) {
    startSample = static_cast<unsigned int>(startTime * sampleRate * channels);
    startSample = std::min(startSample, totalSamples);
  }

  if (endTime >= 0) {
    endSample = static_cast<unsigned int>(endTime * sampleRate * channels);
    endSample = std::min(endSample, totalSamples);
  }

  // Ensure valid range
  if (startSample >= endSample) {
    return 4;
  }

  unsigned int rangeSamples = endSample - startSample;
  unsigned int samplesPerChannel = rangeSamples / channels;

  // Calculate step size for downsampling
  float stepSize = static_cast<float>(samplesPerChannel) / numSamplesNeeded;

  for (unsigned int i = 0; i < numSamplesNeeded; i++) {
    if (average) {
      // Use RMS (Root Mean Square) for proper waveform visualization
      float startPos = i * stepSize;
      float endPos = (i + 1) * stepSize;
      unsigned int startIdx = static_cast<unsigned int>(startPos);
      unsigned int endIdx = static_cast<unsigned int>(std::ceil(endPos));
      endIdx = std::min(endIdx, samplesPerChannel);

      double sumSquares = 0.0;
      unsigned int count = 0;

      for (unsigned int j = startIdx; j < endIdx; j++) {
        for (unsigned int ch = 0; ch < channels; ch++) {
          float val = wav->mData[startSample + (j * channels) + ch];
          sumSquares += val * val;
          count++;
        }
      }

      pSamples[i] = count > 0 ? (float)std::sqrt(sumSquares / count) : 0.0f;
    } else {
      unsigned int idx = static_cast<unsigned int>(i * stepSize);
      idx = std::min(idx, samplesPerChannel - 1);
      float sum = 0.0f;
      for (unsigned int ch = 0; ch < channels; ch++) {
        sum += std::fabs(wav->mData[startSample + (idx * channels) + ch]);
      }
      pSamples[i] = sum / channels;
    }
  }

  return 0; // Success
}

FFI_PLUGIN_EXPORT void extractWaveformAsync(
    unsigned int hash,
    float* outBuffer,
    unsigned int numSamples,
    float startTime,
    float endTime,
    bool average
) {
    // Capture parameters for the thread
    std::thread([=]() {
        // Run extraction on background thread
        int result = extractSamplesFromLoadedSource(
            hash, startTime, endTime, numSamples, average, outBuffer
        );

        // Notify Dart via callback (if set)
        if (g_waveformCallback) {
            g_waveformCallback(hash, result);
        }
    }).detach();
}

} // extern "C"