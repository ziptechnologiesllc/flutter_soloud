/*
SoLoud audio engine
Copyright (c) 2013-2020 Jari Komppa

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
claim that you wrote the original software. If you use this software
in a product, an acknowledgment in the product documentation would be
appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/
#include <stdlib.h>
#include <string.h>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#define SOLOUD_TAG "FlutterSoLoud"
#endif

#include "../../../../aec_bridge.h"
#include "soloud.h"
#include "soloud_thread.h"

// Dynamic symbol lookup for slave bridge (flutter_recorder is a separate library)
// We use dlsym to find the registration functions at runtime since the plugins
// don't link against each other.
#ifndef _WIN32
#include <dlfcn.h>
#endif

// Slave bridge callback type (must match flutter_recorder's soloud_slave_bridge.h)
typedef void (*SoloudSlaveMixCallback)(float *output, unsigned int frameCount,
                                       unsigned int channels);
typedef void (*SoloudRegisterSlaveCallbackFn)(SoloudSlaveMixCallback callback);
typedef void (*SoloudUnregisterSlaveCallbackFn)(void);

// Cached function pointers for slave bridge (looked up once at init)
static SoloudRegisterSlaveCallbackFn g_registerSlaveCallback = nullptr;
static SoloudUnregisterSlaveCallbackFn g_unregisterSlaveCallback = nullptr;

#if !defined(WITH_MINIAUDIO)

#error "WITH_MINIAUDIO NOT DEFINED"

namespace SoLoud {
result miniaudio_init(SoLoud::Soloud *aSoloud, unsigned int aFlags,
                      unsigned int aSamplerate, unsigned int aBuffer) {
  return NOT_IMPLEMENTED;
}
} // namespace SoLoud

#else

#define MINIAUDIO_IMPLEMENTATION
// // #define MA_NO_NULL
// #define MA_NO_DECODING
// #define MA_NO_WAV
// #define MA_NO_FLAC
// #define MA_NO_MP3
// #define MA_NO_AUTOINITIALIZATION
// #define MA_NO_VORBIS
// #define MA_NO_OPUS
#define MA_NO_MIDI

// Seems that on miniaudio there is still an issue when uninitializing the
// device addressed by this issue:
// https://github.com/mackron/miniaudio/issues/466 For me this happens using
// AAudio on android <= 10 (but not on Samsung Galaxy S9+). Disablig AAudio in
// favor of OpenSL is a workaround to prevent the crash.
#if defined(__ANDROID__) && (__ANDROID_API__ <= 29)
#define MA_NO_AAUDIO
#endif
// #define MA_DEBUG_OUTPUT
#include "miniaudio.h"
#include <math.h>

namespace SoLoud {
ma_device gDevice;
SoLoud::Soloud *soloud;
ma_context context;

// Slave mode: When true, SoLoud doesn't own an audio device.
// Instead, Capture's duplex device calls our mix function directly.
// This ensures perfect clock synchronization for AEC on Linux.
static bool gSlaveMode = false;
static unsigned int gSlaveChannels = 2;
static unsigned int gSlaveSamplerate = 48000;

// Added by Marco Bavagnoli
void on_notification(const ma_device_notification *pNotification) {
  MA_ASSERT(pNotification != NULL);
  if (soloud->_stateChangedCallback == nullptr)
    return;

  switch (pNotification->type) {
  case ma_device_notification_type_started: {
    soloud->_stateChangedCallback(0);
  } break;

  case ma_device_notification_type_stopped: {
    soloud->_stateChangedCallback(1);
  } break;

  case ma_device_notification_type_rerouted: {
    soloud->_stateChangedCallback(2);
  } break;

  case ma_device_notification_type_interruption_began: {
    soloud->_stateChangedCallback(3);
  } break;

  case ma_device_notification_type_interruption_ended: {
#if defined(MA_HAS_COREAUDIO)
    // On macOS and iOS when the the interruption begins
    // the device is automatically stopped (not uninited with ma_device_uninit).
    // So we need to start it again when the interruption ends.
    miniaudio_ensureDeviceStarted_impl();
#endif
    soloud->_stateChangedCallback(4);
  } break;

  case ma_device_notification_type_unlocked: {
    soloud->_stateChangedCallback(5);
  } break;

  default:
    break;
  }
}

void soloud_miniaudio_audiomixer(ma_device *pDevice, void *pOutput,
                                 const void *pInput, ma_uint32 frameCount) {
  SoLoud::Soloud *soloud = (SoLoud::Soloud *)pDevice->pUserData;
  soloud->mix((float *)pOutput, frameCount);

  // Send output audio to AEC reference buffer (if callback is set)
  static int mixCount = 0;
  if (mixCount++ % 500 == 0) {
    fprintf(stderr, "[Soloud Miniaudio] Mixer running (frame=%d, cb=%p)\n",
            mixCount, g_aecOutputCallback);
    fflush(stderr);
  }

  if (g_aecOutputCallback != nullptr) {
    static int logCount = 0;
    if (logCount++ < 50) {
      fprintf(stderr,
              "[Soloud Miniaudio] Calling AEC callback %p (frames=%d)\n",
              g_aecOutputCallback, frameCount);
      fflush(stderr);
    }
    g_aecOutputCallback((const float *)pOutput, frameCount,
                        pDevice->playback.channels);
  } else {
    static int logCountNull = 0;
    if (logCountNull++ < 5) {
      fprintf(stderr, "[Soloud Miniaudio] AEC callback is NULL\n");
      fflush(stderr);
    }
  }
}

static void soloud_miniaudio_deinit(SoLoud::Soloud *aSoloud) {
  // In slave mode, we don't own a device
  if (gSlaveMode) {
    if (g_unregisterSlaveCallback != nullptr) {
      g_unregisterSlaveCallback();
    }
    gSlaveMode = false;
    return;
  }

  ma_device_stop(&gDevice);
  ma_device_uninit(&gDevice);
#if defined(MA_HAS_COREAUDIO)
  ma_context_uninit(&context);
#endif
}

result miniaudio_init(SoLoud::Soloud *aSoloud, unsigned int aFlags,
                      unsigned int aSamplerate, unsigned int aBuffer,
                      unsigned int aChannels, void *pPlaybackInfos_id) {
  // Ensure we're not in slave mode when using normal init
  gSlaveMode = false;
  soloud = aSoloud;
  ma_device_config deviceConfig =
      ma_device_config_init(ma_device_type_playback);
  if (pPlaybackInfos_id != NULL) {
    deviceConfig.playback.pDeviceID = (ma_device_id *)pPlaybackInfos_id;
  }
  deviceConfig.periodSizeInFrames = aBuffer;
  deviceConfig.playback.format = ma_format_f32;
  deviceConfig.playback.channels = aChannels;
  deviceConfig.sampleRate = aSamplerate;
  deviceConfig.dataCallback = soloud_miniaudio_audiomixer;
  deviceConfig.pUserData = (void *)aSoloud;

  // deviceConfig.aaudio.usage       = ma_aaudio_usage_default;
  // deviceConfig.aaudio.contentType = ma_aaudio_content_type_default;
  // deviceConfig.aaudio.inputPreset = ma_aaudio_input_preset_default;
  if (aSoloud->_stateChangedCallback != nullptr)
    deviceConfig.notificationCallback = on_notification;

#if defined(MA_HAS_COREAUDIO)
  // Disable CoreAudio context
  ma_context_config contextConfig = ma_context_config_init();
  contextConfig.coreaudio.sessionCategory = ma_ios_session_category_none;
  contextConfig.coreaudio.noAudioSessionActivate = true;
  contextConfig.coreaudio.noAudioSessionDeactivate = true;

  ma_result result = ma_context_init(NULL, 0, &contextConfig, &context);
  if (result != MA_SUCCESS) {
    return UNKNOWN_ERROR;
  }
  if (ma_device_init(&context, &deviceConfig, &gDevice) != MA_SUCCESS) {
    ma_context_uninit(&context);
    return UNKNOWN_ERROR;
  }
#else
  if (ma_device_init(NULL, &deviceConfig, &gDevice) != MA_SUCCESS) {
    return UNKNOWN_ERROR;
  }
#endif

  aSoloud->postinit_internal(gDevice.sampleRate,
                             gDevice.playback.internalPeriodSizeInFrames,
                             aFlags, gDevice.playback.channels);

  aSoloud->mBackendCleanupFunc = soloud_miniaudio_deinit;

  ma_device_start(&gDevice);
  aSoloud->mBackendString = "MiniAudio";
  return 0;
}

result miniaudio_changeDevice_impl(void *pPlaybackInfos_id) {
  if (soloud == nullptr)
    return UNKNOWN_ERROR;

  ma_device_uninit(&gDevice);
  ma_device_config deviceConfig =
      ma_device_config_init(ma_device_type_playback);
  deviceConfig.playback.pDeviceID = (ma_device_id *)pPlaybackInfos_id;
  deviceConfig.periodSizeInFrames = soloud->mBufferSize;
  deviceConfig.playback.format = ma_format_f32;
  deviceConfig.playback.channels = soloud->mChannels;
  deviceConfig.sampleRate = soloud->mSamplerate;
  deviceConfig.dataCallback = soloud_miniaudio_audiomixer;
  deviceConfig.pUserData = (void *)soloud;
  if (ma_device_init(NULL, &deviceConfig, &gDevice) != MA_SUCCESS) {
    return UNKNOWN_ERROR;
  }
  ma_device_start(&gDevice);
  return 0;
}

// Added to ensure miniaudio device is started when needed, ie by an
// interruption. On macOS and iOS when an interruption begins (ie anothe app
// needs the audio context), the device is automatically stopped (not uninited
// with ma_device_uninit). So we need to check if the device is stopped and
// start it again.
result miniaudio_ensureDeviceStarted_impl() {
  if (soloud == nullptr)
    return UNKNOWN_ERROR;

  // In slave mode, we don't own the device
  if (gSlaveMode)
    return 0;

  // Check if device is stopped and start it if needed
  if (ma_device_get_state(&gDevice) == ma_device_state_stopped) {
    ma_result result = ma_device_start(&gDevice);
    if (result != MA_SUCCESS) {
      return UNKNOWN_ERROR;
    }
  }
  return 0;
}

// =============================================================================
// SLAVE MODE - For unified audio device (Linux AEC clock synchronization)
// =============================================================================

// Callback function that Capture's data_callback will invoke to get mixed audio.
// This is registered with the slave bridge when slave mode is initialized.
static void soloud_slave_mix_callback(float *output, unsigned int frameCount,
                                      unsigned int channels) {
  if (soloud == nullptr) {
    // No SoLoud instance - fill with silence
    memset(output, 0, frameCount * channels * sizeof(float));
    return;
  }

  unsigned int soloudChannels = soloud->getBackendChannels();

  // Debug log first few callbacks to diagnose channel issues
  static int mixDebugCount = 0;
  if (mixDebugCount < 5) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, SOLOUD_TAG,
                        "[SoLoud Slave Mix] SoLoud channels=%u, output channels=%u, frames=%u",
                        soloudChannels, channels, frameCount);
#else
    fprintf(stderr, "[SoLoud Slave Mix] SoLoud channels=%u, output channels=%u, frames=%u\n",
            soloudChannels, channels, frameCount);
    fflush(stderr);
#endif
    mixDebugCount++;
  }

  if (soloudChannels == channels) {
    // Channels match - mix directly into output buffer
    soloud->mix(output, frameCount);
  } else if (soloudChannels == 1 && channels == 2) {
    // SoLoud is mono, but device is stereo - mix to temp buffer then expand
    // Use a thread-local static buffer to avoid allocation in audio callback
    static thread_local std::vector<float> monoBuffer;
    if (monoBuffer.size() < frameCount) {
      monoBuffer.resize(frameCount);
    }

    // Mix mono into temp buffer
    soloud->mix(monoBuffer.data(), frameCount);

    // Expand mono to stereo: copy each sample to both L and R
    for (unsigned int i = 0; i < frameCount; i++) {
      output[i * 2] = monoBuffer[i];
      output[i * 2 + 1] = monoBuffer[i];
    }
  } else if (soloudChannels == 2 && channels == 1) {
    // SoLoud is stereo, but device is mono - mix then downmix
    static thread_local std::vector<float> stereoBuffer;
    if (stereoBuffer.size() < frameCount * 2) {
      stereoBuffer.resize(frameCount * 2);
    }

    // Mix stereo into temp buffer
    soloud->mix(stereoBuffer.data(), frameCount);

    // Downmix stereo to mono: average L and R
    for (unsigned int i = 0; i < frameCount; i++) {
      output[i] = (stereoBuffer[i * 2] + stereoBuffer[i * 2 + 1]) * 0.5f;
    }
  } else {
    // Unsupported channel combination - fill with silence and log error
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_ERROR, SOLOUD_TAG,
                        "[SoLoud Slave Mix] ERROR: Unsupported channel combo: soloud=%u, output=%u",
                        soloudChannels, channels);
#else
    fprintf(stderr, "[SoLoud Slave Mix] ERROR: Unsupported channel combo: soloud=%u, output=%u\n",
            soloudChannels, channels);
    fflush(stderr);
#endif
    memset(output, 0, frameCount * channels * sizeof(float));
  }

  // Note: In slave mode, we do NOT call the AEC output callback here.
  // The Capture plugin will write to the AEC reference buffer directly
  // in its callback, ensuring perfect frame-level synchronization.
  // This is the whole point of slave mode - one callback, one clock.
}

// Dynamically look up slave bridge symbols from flutter_recorder library
// This is needed because the plugins are separate shared libraries.
// Flutter loads plugins with RTLD_LOCAL, so we need to explicitly dlopen
// the library to get a handle we can use for symbol lookup.
static void *g_flutterRecorderHandle = nullptr;

static bool lookupSlaveBridgeSymbols() {
#ifndef _WIN32
  if (g_registerSlaveCallback != nullptr) {
    return true;  // Already looked up
  }

  // Flutter loads plugins with RTLD_LOCAL, so RTLD_DEFAULT won't find them.
  // We need to explicitly open the library to get a handle for dlsym.
  // RTLD_NOLOAD ensures we don't load a second copy - we just get a handle
  // to the already-loaded library.
  const char *libNames[] = {
#if defined(__APPLE__)
      "@rpath/flutter_recorder.framework/flutter_recorder",  // iOS/macOS framework
      "flutter_recorder.framework/flutter_recorder",         // Relative framework
#endif
      "libflutter_recorder.so",           // Linux/Android standard name
      "./lib/libflutter_recorder.so",     // Relative to executable
      nullptr
  };

  for (int i = 0; libNames[i] != nullptr; i++) {
    g_flutterRecorderHandle = dlopen(libNames[i], RTLD_NOW | RTLD_NOLOAD);
    if (g_flutterRecorderHandle != nullptr) {
      fprintf(stderr, "[SoLoud Slave] Found flutter_recorder library: %s\n",
              libNames[i]);
      fflush(stderr);
      break;
    }
  }

  if (g_flutterRecorderHandle == nullptr) {
    // Try without RTLD_NOLOAD - maybe it wasn't loaded yet?
    // This shouldn't happen if init order is correct, but let's be safe.
    for (int i = 0; libNames[i] != nullptr; i++) {
      g_flutterRecorderHandle = dlopen(libNames[i], RTLD_NOW);
      if (g_flutterRecorderHandle != nullptr) {
        fprintf(stderr, "[SoLoud Slave] Loaded flutter_recorder library: %s\n",
                libNames[i]);
        fflush(stderr);
        break;
      }
    }
  }

  if (g_flutterRecorderHandle == nullptr) {
    fprintf(stderr, "[SoLoud Slave] ERROR: Could not open flutter_recorder "
            "library: %s\n", dlerror());
    fflush(stderr);
    return false;
  }

  // Look up symbols using the library handle
  g_registerSlaveCallback = (SoloudRegisterSlaveCallbackFn)dlsym(
      g_flutterRecorderHandle, "soloud_registerSlaveMixCallback");
  g_unregisterSlaveCallback = (SoloudUnregisterSlaveCallbackFn)dlsym(
      g_flutterRecorderHandle, "soloud_unregisterSlaveMixCallback");

  if (g_registerSlaveCallback == nullptr) {
    fprintf(stderr, "[SoLoud Slave] ERROR: Could not find "
            "soloud_registerSlaveMixCallback symbol: %s\n", dlerror());
    fflush(stderr);
    dlclose(g_flutterRecorderHandle);
    g_flutterRecorderHandle = nullptr;
    return false;
  }

  if (g_unregisterSlaveCallback == nullptr) {
    fprintf(stderr, "[SoLoud Slave] WARNING: Could not find "
            "soloud_unregisterSlaveMixCallback symbol\n");
    fflush(stderr);
  }

  fprintf(stderr, "[SoLoud Slave] Successfully found slave bridge symbols\n");
  fflush(stderr);
  return true;
#else
  // Windows: slave mode not supported (WASAPI handles clock sync)
  fprintf(stderr, "[SoLoud Slave] Slave mode not supported on Windows\n");
  return false;
#endif
}

// Deinit function for slave mode
static void soloud_slave_deinit(SoLoud::Soloud *aSoloud) {
  fprintf(stderr, "[SoLoud Slave] Deinitializing slave mode\n");
  fflush(stderr);

  // Unregister the slave callback using dynamically looked up function
  if (g_unregisterSlaveCallback != nullptr) {
    g_unregisterSlaveCallback();
  }
  gSlaveMode = false;
}

// Initialize SoLoud in slave mode (no audio device created)
// The Capture plugin's duplex device will drive audio output.
result miniaudio_init_slave(SoLoud::Soloud *aSoloud, unsigned int aFlags,
                            unsigned int aSamplerate, unsigned int aBuffer,
                            unsigned int aChannels) {
  fprintf(stderr,
          "[SoLoud Slave] Initializing slave mode: samplerate=%u, buffer=%u, "
          "channels=%u\n",
          aSamplerate, aBuffer, aChannels);
  fflush(stderr);

  // First, look up the slave bridge symbols from flutter_recorder
  if (!lookupSlaveBridgeSymbols()) {
    fprintf(stderr, "[SoLoud Slave] Failed to find slave bridge symbols. "
            "Make sure flutter_recorder is initialized first.\n");
    fflush(stderr);
    return UNKNOWN_ERROR;
  }

  soloud = aSoloud;
  gSlaveMode = true;
  gSlaveChannels = aChannels;
  gSlaveSamplerate = aSamplerate;

  // Enable lock-free mode for slave operation
  // Instead of using a mutex (which can cause audio thread blocking),
  // we use a lock-free command queue for thread synchronization.
  aSoloud->enableLockFreeMode();
  fprintf(stderr, "[SoLoud Slave] Enabled lock-free mode\n");
  fflush(stderr);

  // Initialize SoLoud's internal state without creating a device
  aSoloud->postinit_internal(aSamplerate, aBuffer, aFlags, aChannels);

  // Set cleanup function
  aSoloud->mBackendCleanupFunc = soloud_slave_deinit;

  // Register our mix callback with the slave bridge
  // Capture's data_callback will call this to get mixed audio
  g_registerSlaveCallback(soloud_slave_mix_callback);

  aSoloud->mBackendString = "MiniAudio (Slave Mode)";

  fprintf(stderr,
          "[SoLoud Slave] Slave mode initialized successfully. Waiting for "
          "Capture to drive audio.\n");
  fflush(stderr);

  return 0;
}

// Check if SoLoud is in slave mode
bool miniaudio_isSlaveMode_impl() { return gSlaveMode; }

}; // namespace SoLoud
#endif
