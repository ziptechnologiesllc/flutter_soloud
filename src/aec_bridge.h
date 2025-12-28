#ifndef AEC_BRIDGE_H
#define AEC_BRIDGE_H

#include <cstddef>

/**
 * AEC Bridge - Provides access to SoLoud's audio output for echo cancellation.
 *
 * This bridge allows the flutter_recorder plugin to receive a copy of the
 * audio being played through SoLoud, which is used as the reference signal
 * for adaptive echo cancellation.
 *
 * Usage:
 *   1. flutter_recorder creates an AECReferenceBuffer
 *   2. flutter_recorder calls aec_setOutputCallback with a callback function
 *   3. SoLoud's audio mixer calls the callback after each mix() operation
 *   4. The callback writes the audio to the reference buffer
 */

// Callback type for receiving audio output
// Parameters: audio data (float*), frame count, channel count
typedef void (*AECOutputCallback)(const float* data, size_t frameCount, unsigned int channels);

// Global callback pointer (set by flutter_recorder)
extern AECOutputCallback g_aecOutputCallback;

// Set the output callback (called from flutter_recorder)
inline void aec_setOutputCallback(AECOutputCallback callback) {
    g_aecOutputCallback = callback;
}

// Clear the output callback
inline void aec_clearOutputCallback() {
    g_aecOutputCallback = nullptr;
}

#endif // AEC_BRIDGE_H
