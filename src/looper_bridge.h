#ifndef LOOPER_BRIDGE_H
#define LOOPER_BRIDGE_H

#include "common.h"
#include <cstddef>
#include <cstdint>

/**
 * Looper Bridge - Allows flutter_recorder to directly load and play audio
 * through SoLoud without going through Dart.
 *
 * This enables zero-latency loop playback: when recording stops, native code
 * immediately loads the bounded audio into SoLoud and starts looping playback.
 * Dart is notified via callback to update UI state.
 */

#ifdef __cplusplus
extern "C" {
#endif

// Callback to notify Dart when a loop starts playing
// Parameters: soundHash, handle, durationSeconds
typedef void (*LooperPlaybackStartedCallback)(unsigned int soundHash,
                                               unsigned int handle,
                                               double durationSeconds);

// Set the callback (called from Dart during init)
FFI_PLUGIN_EXPORT void looper_setPlaybackStartedCallback(LooperPlaybackStartedCallback callback);

// Clear the callback
FFI_PLUGIN_EXPORT void looper_clearPlaybackStartedCallback();

// Load raw PCM float samples and immediately start looping playback
// This is the most efficient path - no WAV container overhead
// Returns: soundHash (0 on failure)
// Parameters:
//   samples - pointer to raw float samples (interleaved if stereo)
//   numSamples - total number of samples (frames * channels)
//   sampleRate - sample rate in Hz
//   channels - number of channels (1=mono, 2=stereo)
//   copy - if true, SoLoud copies the data; if false, uses pointer directly
//   takeOwnership - if true (and copy=false), SoLoud frees memory when done
//   outHandle - receives the playback handle
FFI_PLUGIN_EXPORT unsigned int looper_loadAndPlayRaw(float* samples,
                                                      unsigned int numSamples,
                                                      float sampleRate,
                                                      unsigned int channels,
                                                      bool copy,
                                                      bool takeOwnership,
                                                      unsigned int* outHandle);

#ifdef __cplusplus
}
#endif

#endif // LOOPER_BRIDGE_H
