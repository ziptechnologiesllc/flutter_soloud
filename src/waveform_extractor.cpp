#include "player.h"
#include "soloud_wav.h"
#include <cstring>
#include <algorithm>
#include <cmath>

// External reference to the global player instance from bindings.cpp
extern std::unique_ptr<Player> player;

extern "C" {

/// Extract waveform samples from an already-loaded audio source.
///
/// This function finds a loaded sound by its hash and extracts sample data
/// directly from memory if it was loaded as a Wav (not WavStream).
///
/// @param hash The unique hash of the loaded audio source
/// @param numSamplesNeeded Number of samples to extract for visualization
/// @param startTime Start time in seconds (0 for beginning)
/// @param endTime End time in seconds (-1 for end of file)
/// @param average Whether to average samples when downsampling
/// @param pSamples Output buffer for the samples (must be pre-allocated)
/// @return 0 on success, error code otherwise
int extractSamplesFromLoadedSource(
    unsigned int hash,
    float startTime,
    float endTime,
    unsigned long numSamplesNeeded,
    bool average,
    float* pSamples)
{
    // Check if player is initialized
    if (!player || !player->isInited()) {
        return 5; // Error: player not initialized
    }

    // Find the sound by hash
    ActiveSound* sound = player->findByHash(hash);

    if (sound == nullptr) {
        // Sound not found
        return 1; // Error: sound not loaded
    }

    // Check if it's a Wav (loaded into memory) vs WavStream
    if (sound->soundType != TYPE_WAV) {
        // This is a stream, we can't access the data directly
        return 2; // Error: sound is streamed, not in memory
    }

    // Cast to Wav to access the sample data
    SoLoud::Wav* wav = static_cast<SoLoud::Wav*>(sound->sound.get());

    if (wav->mData == nullptr || wav->mSampleCount == 0) {
        // No data available
        return 3; // Error: no sample data
    }

    // Get audio properties
    unsigned int channels = wav->mChannels;
    float sampleRate = wav->mBaseSamplerate;
    unsigned int totalSamples = wav->mSampleCount;

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
        return 4; // Error: invalid time range
    }

    unsigned int rangeSamples = endSample - startSample;
    unsigned int samplesPerChannel = rangeSamples / channels;

    // Calculate step size for downsampling
    float stepSize = static_cast<float>(samplesPerChannel) / numSamplesNeeded;

    for (unsigned int i = 0; i < numSamplesNeeded; i++) {
        if (average) {
            // Use RMS (Root Mean Square) for proper waveform visualization
            // This gives the "energy" of the signal, not just raw average
            float startPos = i * stepSize;
            float endPos = (i + 1) * stepSize;
            unsigned int startIdx = static_cast<unsigned int>(startPos);
            unsigned int endIdx = static_cast<unsigned int>(std::ceil(endPos));
            endIdx = std::min(endIdx, samplesPerChannel);

            double sumSquares = 0.0;
            unsigned int count = 0;

            for (unsigned int j = startIdx; j < endIdx; j++) {
                for (unsigned int ch = 0; ch < channels; ch++) {
                    float sample = wav->mData[startSample + (j * channels) + ch];
                    sumSquares += sample * sample;
                    count++;
                }
            }

            // RMS = sqrt(sum of squares / count)
            pSamples[i] = count > 0 ? std::sqrt(sumSquares / count) : 0.0f;
        } else {
            // Simple sampling (pick one sample) - take absolute value for visualization
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

} // extern "C"