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

    // Get SoLoud's backend sample rate for comparison
    unsigned int backendSampleRate = player->soloud.getBackendSamplerate();
    fprintf(stderr, "[Looper Bridge] Loading raw PCM: %u samples (%u frames), %u ch @ %.0f Hz (%.3f sec)\n",
            numSamples, frames, channels, sampleRate, durationSec);
    fprintf(stderr, "[Looper Bridge] SoLoud backend sampleRate: %u Hz\n", backendSampleRate);
    if (backendSampleRate != (unsigned int)sampleRate) {
        fprintf(stderr, "[Looper Bridge] WARNING: Sample rate mismatch! Audio=%.0f, Backend=%u\n",
                sampleRate, backendSampleRate);
    }
    fflush(stderr);

    unsigned int soundHash = 0;
    PlayerErrors loadErr;

    {
        std::lock_guard<std::mutex> lock(loadMutex);

        // Generate a unique name for this loop
        static int loopCounter = 0;
        char uniqueName[64];
        snprintf(uniqueName, sizeof(uniqueName), "native_loop_%d", ++loopCounter);

        // Load raw PCM directly - no WAV parsing needed!
        loadErr = player->loadRawWave(
            uniqueName,
            samples,
            numSamples,
            sampleRate,
            channels,
            copy,
            takeOwnership,
            soundHash
        );
    }

    if (loadErr != PlayerErrors::noError) {
        fprintf(stderr, "[Looper Bridge] ERROR: loadRawWave failed with error %d\n", static_cast<int>(loadErr));
        fflush(stderr);
        if (outHandle) *outHandle = 0;
        return 0;
    }

    fprintf(stderr, "[Looper Bridge] Loaded sound with hash: %u\n", soundHash);
    fflush(stderr);

    // Start looping playback immediately
    unsigned int handle = 0;
    PlayerErrors playErr = player->play(
        soundHash,
        handle,
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

    fprintf(stderr, "[Looper Bridge] Started looping playback: hash=%u handle=%u\n", soundHash, handle);
    fflush(stderr);

    if (outHandle) *outHandle = handle;

    // Notify Dart via callback
    if (g_looperCallback) {
        fprintf(stderr, "[Looper Bridge] Notifying Dart: hash=%u handle=%u duration=%.3f\n",
                soundHash, handle, durationSec);
        fflush(stderr);
        g_looperCallback(soundHash, handle, durationSec);
    }

    return soundHash;
}

} // extern "C"
