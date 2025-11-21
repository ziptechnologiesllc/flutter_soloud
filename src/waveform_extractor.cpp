#include "player.h"
#include "soloud_wav.h"
#include <cstring>
#include <algorithm>
#include <cmath>

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
    // Get the Player instance
    Player& player = Player::instance();

    // Find the sound by hash
    std::shared_ptr<ActiveSound> sound = player.findByHash(hash);

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

    // Resample/downsample to the requested number of samples
    if (samplesPerChannel <= numSamplesNeeded) {
        // We have fewer samples than requested, just copy them
        // Mix down to mono if multi-channel
        for (unsigned int i = 0; i < samplesPerChannel && i < numSamplesNeeded; i++) {
            float sum = 0.0f;
            for (unsigned int ch = 0; ch < channels; ch++) {
                sum += wav->mData[startSample + (i * channels) + ch];
            }
            pSamples[i] = sum / channels;
        }
        // Fill remaining with zeros
        for (unsigned int i = samplesPerChannel; i < numSamplesNeeded; i++) {
            pSamples[i] = 0.0f;
        }
    } else {
        // Downsample to the requested number
        float stepSize = static_cast<float>(samplesPerChannel) / numSamplesNeeded;

        for (unsigned int i = 0; i < numSamplesNeeded; i++) {
            if (average) {
                // Average over a window
                float startPos = i * stepSize;
                float endPos = (i + 1) * stepSize;
                unsigned int startIdx = static_cast<unsigned int>(startPos);
                unsigned int endIdx = static_cast<unsigned int>(std::ceil(endPos));
                endIdx = std::min(endIdx, samplesPerChannel);

                float sum = 0.0f;
                unsigned int count = 0;

                for (unsigned int j = startIdx; j < endIdx; j++) {
                    for (unsigned int ch = 0; ch < channels; ch++) {
                        sum += wav->mData[startSample + (j * channels) + ch];
                        count++;
                    }
                }

                pSamples[i] = count > 0 ? sum / count : 0.0f;
            } else {
                // Simple sampling (pick one sample)
                unsigned int idx = static_cast<unsigned int>(i * stepSize);
                float sum = 0.0f;
                for (unsigned int ch = 0; ch < channels; ch++) {
                    sum += wav->mData[startSample + (idx * channels) + ch];
                }
                pSamples[i] = sum / channels;
            }
        }
    }

    return 0; // Success
}

} // extern "C"