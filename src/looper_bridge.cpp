#include "looper_bridge.h"
#include "player.h"
#include "common.h"
#include "soloud.h"
#include <memory>
#include <mutex>
#include <cstdio>

// Access the global player from bindings.cpp
extern std::unique_ptr<Player> player;
extern std::mutex loadMutex;

// Callback to notify Dart when loop playback starts
static LooperPlaybackStartedCallback g_looperCallback = nullptr;

extern "C" {

FFI_PLUGIN_EXPORT void looper_setPlaybackStartedCallback(LooperPlaybackStartedCallback callback) {
    g_looperCallback = callback;
    fprintf(stderr, "[Looper Bridge] Playback started callback set: %p\n", (void*)callback);
    fflush(stderr);
}

FFI_PLUGIN_EXPORT void looper_clearPlaybackStartedCallback() {
    g_looperCallback = nullptr;
    fprintf(stderr, "[Looper Bridge] Playback started callback cleared\n");
    fflush(stderr);
}

FFI_PLUGIN_EXPORT unsigned int looper_loadAndPlayRaw(float* samples,
                                                      unsigned int numSamples,
                                                      float sampleRate,
                                                      unsigned int channels,
                                                      bool copy,
                                                      bool takeOwnership,
                                                      unsigned int* outHandle) {
    if (!player || !player->isInited()) {
        fprintf(stderr, "[Looper Bridge] ERROR: Player not initialized\n");
        fflush(stderr);
        if (outHandle) *outHandle = 0;
        return 0;
    }

    if (!samples || numSamples == 0) {
        fprintf(stderr, "[Looper Bridge] ERROR: Invalid sample data\n");
        fflush(stderr);
        if (outHandle) *outHandle = 0;
        return 0;
    }

    unsigned int frames = numSamples / channels;
    double durationSec = (double)frames / sampleRate;

    // Minimal logging — fprintf can cause audio glitches if called near audio path

    unsigned int soundHash = 0;
    PlayerErrors loadErr;

    {
        std::lock_guard<std::mutex> lock(loadMutex);

        // Generate a unique name for this loop
        static int loopCounter = 0;
        char uniqueName[64];
        snprintf(uniqueName, sizeof(uniqueName), "native_loop_%d", ++loopCounter);

        // SoLoud stores audio in planar (non-interleaved) format:
        //   [L0, L1, ..., Ln, R0, R1, ..., Rn]
        // But the recorder outputs interleaved:
        //   [L0, R0, L1, R1, ..., Ln, Rn]
        // Deinterleave before loading.
        float *planarSamples = samples;
        bool freePlanar = false;
        if (channels > 1) {
            planarSamples = new float[numSamples];
            freePlanar = true;
            unsigned int framesCount = numSamples / channels;
            for (unsigned int ch = 0; ch < channels; ch++) {
                for (unsigned int f = 0; f < framesCount; f++) {
                    planarSamples[ch * framesCount + f] = samples[f * channels + ch];
                }
            }
        }

        // Load raw PCM directly - no WAV parsing needed!
        loadErr = player->loadRawWave(
            uniqueName,
            planarSamples,
            numSamples,
            sampleRate,
            channels,
            !freePlanar && copy,     // don't copy if we allocated new buffer
            freePlanar || takeOwnership, // take ownership of our new buffer
            soundHash
        );

        // If loadRawWave failed and we allocated, clean up
        if (loadErr != PlayerErrors::noError && freePlanar) {
            delete[] planarSamples;
        }
    }

    if (loadErr != PlayerErrors::noError) {
        fprintf(stderr, "[Looper Bridge] ERROR: loadRawWave failed with error %d\n", static_cast<int>(loadErr));
        fflush(stderr);
        if (outHandle) *outHandle = 0;
        return 0;
    }

    // Start looping playback immediately
    unsigned int handle = 0;
    PlayerErrors playErr = player->play(
        soundHash,
        handle,
        0,      // busId (0 = main bus)
        1.0f,   // volume
        0.0f,   // pan (centered)
        false,  // not paused
        true,   // looping enabled!
        0.0     // loop start at beginning
    );

    if (playErr != PlayerErrors::noError) {
        fprintf(stderr, "[Looper Bridge] ERROR: play failed with error %d\n", static_cast<int>(playErr));
        fflush(stderr);
        if (outHandle) *outHandle = 0;
        return 0;
    }

    // Align with existing loops by querying the first loop's actual SoLoud
    // playback position and seeking the new loop to match.
    // This compensates for the loading latency (deinterleave + loadRawWave).
    {
        std::lock_guard<std::recursive_mutex> lock(player->sounds_mutex);
        for (const auto& s : player->sounds) {
            if (s->soundHash != soundHash && s->sound) {
                // Found another playing sound — get its current position
                // and use modulo to find where we should be in our loop
                for (const auto& ah : s->handle) {
                    if (ah.handle != 0 && player->soloud.isValidVoiceHandle(ah.handle)) {
                        double refPos = player->soloud.getStreamPosition(ah.handle);
                        double refLen = player->soloud.getStreamTime(ah.handle);
                        if (refLen > 0) {
                            double refPhase = fmod(refPos, refLen);
                            double ourLen = (double)frames / sampleRate;
                            double seekPos = fmod(refPhase, ourLen);
                            player->soloud.seek(handle, seekPos);
                        }
                        goto aligned;
                    }
                }
            }
        }
        aligned:;
    }

    if (outHandle) *outHandle = handle;

    // Notify Dart via callback
    if (g_looperCallback) {
        g_looperCallback(soundHash, handle, durationSec);
    }

    return soundHash;
}

} // extern "C"
