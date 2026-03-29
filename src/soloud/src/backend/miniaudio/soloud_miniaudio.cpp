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

#include "soloud.h"
#include "soloud_internal.h"

#ifdef __ANDROID__
#include <android/log.h>
#define SOLOUD_TAG "FlutterSoLoud"
#endif

#include "../../../../aec_bridge.h"

#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
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

namespace SoLoud
{
    result miniaudio_init(SoLoud::Soloud *aSoloud, unsigned int aFlags, unsigned int aSamplerate, unsigned int aBuffer)
    {
        return NOT_IMPLEMENTED;
    }
}

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

// Seems that on miniaudio there is still an issue when uninitializing the device
// addressed by this issue: https://github.com/mackron/miniaudio/issues/466
// For me this happens using AAudio on android <= 10 (but not on Samsung Galaxy S9+).
// Disablig AAudio in favor of OpenSL is a workaround to prevent the crash.
// #if defined(__ANDROID__) && (__ANDROID_API__ <= 29)
// #define MA_NO_AAUDIO
// #endif
// #define MA_DEBUG_OUTPUT
#include "miniaudio.h"
#ifdef __ANDROID__
#include <android/api-level.h>
#endif
#include <math.h>
#include <chrono>
#include <thread>
#include <mutex>
#if defined(_WIN32) || defined(_WIN64)
#  include <windows.h>
#else
#  include <pthread.h>
#  include <sys/resource.h>
#endif

namespace SoLoud
{
    ma_device gDevice;
    SoLoud::Soloud *soloud;
    ma_context context;
    volatile bool gDeviceStopped = true;  // Track device stopped state for proper cleanup

    // Slave mode: When true, SoLoud doesn't own an audio device.
    // Instead, Capture's duplex device calls our mix function directly.
    static bool gSlaveMode = false;
    static unsigned int gSlaveChannels = 2;
    static unsigned int gSlaveSamplerate = 48000;

    // Forward declarations for functions used in on_notification
    result soloud_miniaudio_pause(SoLoud::Soloud *aSoloud);
    result soloud_miniaudio_resume(SoLoud::Soloud *aSoloud);
    result miniaudio_ensure_thread_device_started();
    static bool gDeviceStartDeferred = false; // Track deferred device start on Windows
    static bool gDeviceInitDeferred = false;  // Track deferred device init on Windows
    static bool gDeviceInitialized = false;   // Track if device is actually initialized
    static std::thread* gInitThread = nullptr; // Background thread for device init
    static std::mutex gInitMutex; // Protect device init state
    
    // Configuration to store for deferred initialization
    struct DeferredDeviceConfig {
        ma_device_config config;
        ma_context_config contextConfig;
        bool useContext;
        bool useContextConfig;
    };
    static DeferredDeviceConfig gDeferredConfig;

    // Added by Marco Bavagnoli
    void on_notification(const ma_device_notification* pNotification)
    {
        MA_ASSERT(pNotification != NULL);

        switch (pNotification->type)
        {
            case ma_device_notification_type_started:
            {
                gDeviceStopped = false;
                if (soloud->_stateChangedCallback != nullptr) soloud->_stateChangedCallback(0);
            } break;

            case ma_device_notification_type_stopped:
            {
                gDeviceStopped = true;
                if (soloud->_stateChangedCallback != nullptr) soloud->_stateChangedCallback(1);
            } break;

            case ma_device_notification_type_rerouted:
            {
                if (soloud->_stateChangedCallback != nullptr) soloud->_stateChangedCallback(2);
            } break;

            case ma_device_notification_type_interruption_began:
            {
                // Automatically pause the audio device when the OS signals an interruption.
                soloud_miniaudio_pause(soloud);
                if (soloud->_stateChangedCallback != nullptr) soloud->_stateChangedCallback(3);
            } break;

            case ma_device_notification_type_interruption_ended:
            {
                // On CoreAudio platforms (macOS/iOS) when the the interruption begins
                // the device is automatically stopped (not uninited with ma_device_uninit).
                // So we need to start it again when the interruption ends.
                soloud->resume();
                if (soloud->_stateChangedCallback != nullptr)
                    soloud->_stateChangedCallback(4);
            } break;

            case ma_device_notification_type_unlocked:
            {
                if (soloud->_stateChangedCallback != nullptr) soloud->_stateChangedCallback(5);
            } break;

            default: break;
        }
    }

    void soloud_miniaudio_audiomixer(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
    {
        static bool first_call = true;
        if (first_call) {
#ifdef __ANDROID__
            int policy;
            struct sched_param param;
            if (pthread_getschedparam(pthread_self(), &policy, &param) == 0) {
                // Attempt to elevate to Realtime FIFO
                param.sched_priority = 1;
                if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
                    if (pthread_setschedparam(pthread_self(), SCHED_RR, &param) != 0) {
                        // If denied Realtime, check if we are stuck in SCHED_BATCH (3)
                        // and try to escape to SCHED_OTHER (0)
                        if (policy == 3) {
                             param.sched_priority = 0;
                             pthread_setschedparam(pthread_self(), 0, &param);
                        }
                    }
                }
                // Plus set highest niceness
                setpriority(PRIO_PROCESS, 0, -20);
            }
#endif
        }
        first_call = false;
        SoLoud::Soloud *soloud = (SoLoud::Soloud *)pDevice->pUserData;
        soloud->mix((float *)pOutput, frameCount);

        // Send output audio to AEC reference buffer (if callback is set)
        if (g_aecOutputCallback != nullptr) {
            g_aecOutputCallback((const float *)pOutput, frameCount,
                                pDevice->playback.channels);
        }
    }

    static void soloud_miniaudio_deinit(SoLoud::Soloud *aSoloud)
    {
        // In slave mode, we don't own a device
        if (gSlaveMode) {
            if (g_unregisterSlaveCallback != nullptr) {
                g_unregisterSlaveCallback();
            }
            gSlaveMode = false;
            return;
        }

        // Clean up initialization thread if it's still running
        if (gInitThread != nullptr)
        {
            if (gInitThread->joinable())
            {
                gInitThread->join();
            }
            delete gInitThread;
            gInitThread = nullptr;
        }
        
        if (gDeviceInitialized)
        {
            // Check if device is already stopped before calling ma_device_stop()
            // (which can cause an ANR on Android using OpenSSL #333).
            // This should prevent ANR on Android where ma_device_stop() can block indefinitely
            // if the device is in an unknown state
            if (ma_device_get_state(&gDevice) != ma_device_state_stopped)
            {
                ma_device_stop(&gDevice);
                
                // Wait for device to actually stop before uninitializing
                // Timeout after 500ms to prevent infinite blocking
                int timeoutMs = 0;
                int maxTimeoutMs = 500;
                while (!gDeviceStopped && timeoutMs < maxTimeoutMs)
                {
                    // Small sleep to avoid busy-waiting
#if defined(_WIN32) || defined(_WIN64)
                    Sleep(1);
#else
                    usleep(1000);  // 1ms sleep
#endif
                    timeoutMs += 1;
                }
            }
            
            // Set flag to stopped in case notification wasn't received
            gDeviceStopped = true;
            
            // From miniaudio.h doc:
            // "This will explicitly stop the device. You do not need to call `ma_device_stop()` beforehand, but it's harmless if you do."
            ma_device_uninit(&gDevice);
            gDeviceInitialized = false;
        }
#if defined(MA_HAS_COREAUDIO) || defined(__ANDROID__)
        ma_context_uninit(&context);
#endif
    }

    // Pause the audio device: stops the CoreAudio AudioUnit (or platform equivalent)
    // without uninitialising it. This is the correct way to "pause" on iOS/macOS —
    // it tells the OS the app has nothing to render, which preserves AVAudioSession
    // state and keeps MPRemoteCommandCenter routing intact.
    result soloud_miniaudio_pause(SoLoud::Soloud *aSoloud)
    {
        if (ma_device_get_state(&gDevice) == ma_device_state_started)
        {
            ma_result res = ma_device_stop(&gDevice);
            if (res != MA_SUCCESS)
                return UNKNOWN_ERROR;
        }
        return 0;
    }

    // Resume the audio device after soloud_miniaudio_pause(). On iOS, the
    // AVAudioSession must already be active (the app is responsible for calling
    // [AVAudioSession setActive:YES]) before calling this.
    result soloud_miniaudio_resume(SoLoud::Soloud *aSoloud)
    {
        if (aSoloud == nullptr)
            return UNKNOWN_ERROR;

        // In slave mode, we don't own the device
        if (gSlaveMode)
            return 0;

        // Check if device is stopped and start it if needed
        if (ma_device_get_state(&gDevice) == ma_device_state_stopped)
        {
#if defined(MA_APPLE_MOBILE)
            // On iOS, after any audio interruption the AVAudioSession MUST be
            // explicitly re-activated before restarting the Audio Unit.
            //
            // Without this call, iOS does not restore remote command routing
            // (Lock Screen controls, AirPods) to this app after the device
            // restarts. This is because:
            //   1. miniaudio registers its own AVAudioSessionInterruptionNotification
            //      observer alongside audio_session (the Flutter package), so both
            //      handle interruptions concurrently.
            //   2. miniaudio can restart AudioOutputUnit before audio_session has
            //      had a chance to call setActive:YES, leaving the unit running
            //      against an inactive session — breaking remote command routing.
            //   3. Apple's audio interruption recovery guidelines explicitly require
            //      setActive:YES before restarting the Audio Unit.
            @autoreleasepool {
                [[AVAudioSession sharedInstance] setActive:YES error:nil];
            }
#endif
            ma_result result = ma_device_start(&gDevice);
            if (result != MA_SUCCESS)
                return UNKNOWN_ERROR;
        }
        return 0;
    }

    result miniaudio_init(SoLoud::Soloud *aSoloud, unsigned int aFlags, unsigned int aSamplerate, unsigned int aBuffer, unsigned int aChannels, void *pPlaybackInfos_id)
    {
        soloud = aSoloud;
        ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
        if (pPlaybackInfos_id != NULL)
        {
            deviceConfig.playback.pDeviceID = (ma_device_id*)pPlaybackInfos_id;
        }
        deviceConfig.periodSizeInFrames = aBuffer;
        deviceConfig.playback.format    = ma_format_f32;
        deviceConfig.playback.channels  = aChannels;
        deviceConfig.sampleRate         = aSamplerate;
        deviceConfig.dataCallback       = soloud_miniaudio_audiomixer;
        deviceConfig.pUserData          = (void *)aSoloud;

        // deviceConfig.aaudio.usage       = ma_aaudio_usage_default;
        // deviceConfig.aaudio.contentType = ma_aaudio_content_type_default;
        // deviceConfig.aaudio.inputPreset = ma_aaudio_input_preset_default;
        deviceConfig.notificationCallback = on_notification;

#ifdef _WIN32
        // On Windows, defer the entire device initialization to avoid interfering with
        // the main thread's message pump. This fixes compatibility with plugins like
        // desktop_drop that rely on COM windowed messages.
        gDeferredConfig.config = deviceConfig;
        gDeferredConfig.useContext = false;
        gDeferredConfig.useContextConfig = false;
        gDeviceInitDeferred = true;
        gDeviceStartDeferred = false;
        
        // On Windows, start the audio device initialization in background.
        // This ensures the device is ready by the time play() is called,
        // without blocking the main thread's message pump.
        aSoloud->postinit_internal(aSamplerate, aBuffer, aFlags, aChannels);

        // Use safe default values for postinit
        miniaudio_ensure_thread_device_started();

#elif defined(MA_HAS_COREAUDIO)
        // Disable CoreAudio context
        ma_context_config contextConfig = ma_context_config_init();
        contextConfig.coreaudio.sessionCategory = ma_ios_session_category_none;
        contextConfig.coreaudio.noAudioSessionActivate = true;
        contextConfig.coreaudio.noAudioSessionDeactivate = true;

        ma_result result = ma_context_init(NULL, 0, &contextConfig, &context);
        if (result != MA_SUCCESS) {
            return UNKNOWN_ERROR;
        }
        if (ma_device_init(&context, &deviceConfig, &gDevice) != MA_SUCCESS)
        {
            ma_context_uninit(&context);
            return UNKNOWN_ERROR;
        }
        gDeviceInitialized = true;
        aSoloud->postinit_internal(gDevice.sampleRate, gDevice.playback.internalPeriodSizeInFrames, aFlags, gDevice.playback.channels);
        ma_device_start(&gDevice);
        gDeviceInitDeferred = false;
        gDeviceStartDeferred = false;
        
#elif defined(__ANDROID__)
        ma_backend backends[] = { ma_backend_aaudio, ma_backend_opensl };
        ma_uint32 backendCount = 2;
        if (android_get_device_api_level() <= 29) {
            backends[0] = ma_backend_opensl;
            backendCount = 1;
        }

        ma_context_config contextConfig = ma_context_config_init();
        if (ma_context_init(backends, backendCount, &contextConfig, &context) != MA_SUCCESS) {
            return UNKNOWN_ERROR;
        }
        if (ma_device_init(&context, &deviceConfig, &gDevice) != MA_SUCCESS) {
            ma_context_uninit(&context);
            return UNKNOWN_ERROR;
        }
        gDeviceInitialized = true;
        aSoloud->postinit_internal(gDevice.sampleRate, gDevice.playback.internalPeriodSizeInFrames, aFlags, gDevice.playback.channels);
        ma_device_start(&gDevice);
        gDeviceInitDeferred = false;
        gDeviceStartDeferred = false;
        
#else
        // Linux and other platforms
        if (ma_device_init(NULL, &deviceConfig, &gDevice) != MA_SUCCESS)
        {
            return UNKNOWN_ERROR;
        }
        gDeviceInitialized = true;
        aSoloud->postinit_internal(gDevice.sampleRate, gDevice.playback.internalPeriodSizeInFrames, aFlags, gDevice.playback.channels);
        ma_device_start(&gDevice);
        gDeviceInitDeferred = false;
        gDeviceStartDeferred = false;
#endif

        aSoloud->mBackendCleanupFunc = soloud_miniaudio_deinit;
        aSoloud->mBackendPauseFunc   = soloud_miniaudio_pause;
        aSoloud->mBackendResumeFunc  = soloud_miniaudio_resume;
        aSoloud->mBackendString = "MiniAudio";
        return 0;
    }

    // Background thread function to initialize the audio device
    static void miniaudio_init_thread_func()
    {
        std::lock_guard<std::mutex> lock(gInitMutex);
        
        if (!gDeviceInitDeferred)
            return;

        if (ma_device_init(NULL, &gDeferredConfig.config, &gDevice) == MA_SUCCESS)
        {
            gDeviceInitDeferred = false;
            gDeviceInitialized = true;
            // Start the device after initialization
            if (ma_device_get_state(&gDevice) != ma_device_state_started)
            {
                ma_device_start(&gDevice);
            }
            gDeviceStartDeferred = false;
        }
    }

    // Ensure the device is started. Called on first audio operation on Windows.
    // On Windows, this runs device init on a background thread to avoid blocking the message pump.
    result miniaudio_ensure_thread_device_started()
    {
        if (!gDeviceInitDeferred)
            return 0; // Already initialized and started

        // Create a background thread to initialize and start the device
        // This prevents the main thread's message pump from being blocked
        if (gInitThread == nullptr)
        {
            gInitThread = new std::thread(miniaudio_init_thread_func);
            
            // Wait for the thread to complete (with reasonable timeout)
            // The thread uses a mutex to protect device access
            if (gInitThread && gInitThread->joinable())
            {
                gInitThread->join();
                delete gInitThread;
                gInitThread = nullptr;
            }
        }

        // Verify the device is ready
        if (gDeviceInitDeferred)
            return UNKNOWN_ERROR; // Init failed
            
        return 0;
    }

    result miniaudio_changeDevice_impl(void *pPlaybackInfos_id)
    {
        if (soloud == nullptr)
            return UNKNOWN_ERROR;

        ma_device_uninit(&gDevice);
        ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
        deviceConfig.playback.pDeviceID = (ma_device_id *)pPlaybackInfos_id;
        deviceConfig.periodSizeInFrames = soloud->mBufferSize;
        deviceConfig.playback.format    = ma_format_f32;
        deviceConfig.playback.channels  = soloud->mChannels;
        deviceConfig.sampleRate         = soloud->mSamplerate;
        deviceConfig.dataCallback       = soloud_miniaudio_audiomixer;
        deviceConfig.pUserData          = (void *)soloud;
        if (ma_device_init(NULL, &deviceConfig, &gDevice) != MA_SUCCESS)
        {
            gDeviceInitialized = false;
            return UNKNOWN_ERROR;
        }
        gDeviceInitialized = true;
        gDeviceStopped = false;  // Device is about to start
        ma_device_start(&gDevice);
        return 0;
    }
    // =========================================================================
    // SLAVE MODE - For unified audio device (AEC clock synchronization)
    // =========================================================================

    // Callback function that Capture's data_callback will invoke to get mixed audio.
    static void soloud_slave_mix_callback(float *output, unsigned int frameCount,
                                          unsigned int channels) {
        if (soloud == nullptr) {
            memset(output, 0, frameCount * channels * sizeof(float));
            return;
        }

        unsigned int soloudChannels = soloud->getBackendChannels();

        if (soloudChannels == channels) {
            soloud->mix(output, frameCount);
        } else if (soloudChannels == 1 && channels == 2) {
            static thread_local std::vector<float> monoBuffer;
            if (monoBuffer.size() < frameCount) monoBuffer.resize(frameCount);
            soloud->mix(monoBuffer.data(), frameCount);
            for (unsigned int i = 0; i < frameCount; i++) {
                output[i * 2] = monoBuffer[i];
                output[i * 2 + 1] = monoBuffer[i];
            }
        } else if (soloudChannels == 2 && channels == 1) {
            static thread_local std::vector<float> stereoBuffer;
            if (stereoBuffer.size() < frameCount * 2) stereoBuffer.resize(frameCount * 2);
            soloud->mix(stereoBuffer.data(), frameCount);
            for (unsigned int i = 0; i < frameCount; i++) {
                output[i] = (stereoBuffer[i * 2] + stereoBuffer[i * 2 + 1]) * 0.5f;
            }
        } else {
            memset(output, 0, frameCount * channels * sizeof(float));
        }
    }

    // Dynamically look up slave bridge symbols from flutter_recorder library
    static void *g_flutterRecorderHandle = nullptr;

    static bool lookupSlaveBridgeSymbols() {
#ifndef _WIN32
        if (g_registerSlaveCallback != nullptr) return true;

        // First try RTLD_DEFAULT — works when both plugins are statically linked
        // into the same binary (macOS/iOS with use_frameworks! :linkage => :static)
        g_registerSlaveCallback = (SoloudRegisterSlaveCallbackFn)dlsym(
            RTLD_DEFAULT, "soloud_registerSlaveMixCallback");
        g_unregisterSlaveCallback = (SoloudUnregisterSlaveCallbackFn)dlsym(
            RTLD_DEFAULT, "soloud_unregisterSlaveMixCallback");

        if (g_registerSlaveCallback != nullptr) return true;

        // Fall back to dlopen — works when plugins are separate dynamic libraries
        const char *libNames[] = {
#if defined(__APPLE__)
            "@rpath/flutter_recorder.framework/flutter_recorder",
            "flutter_recorder.framework/flutter_recorder",
#endif
            "libflutter_recorder.so",
            "./lib/libflutter_recorder.so",
            nullptr
        };

        for (int i = 0; libNames[i] != nullptr; i++) {
            g_flutterRecorderHandle = dlopen(libNames[i], RTLD_NOW | RTLD_NOLOAD);
            if (g_flutterRecorderHandle != nullptr) break;
        }
        if (g_flutterRecorderHandle == nullptr) {
            for (int i = 0; libNames[i] != nullptr; i++) {
                g_flutterRecorderHandle = dlopen(libNames[i], RTLD_NOW);
                if (g_flutterRecorderHandle != nullptr) break;
            }
        }
        if (g_flutterRecorderHandle == nullptr) return false;

        g_registerSlaveCallback = (SoloudRegisterSlaveCallbackFn)dlsym(
            g_flutterRecorderHandle, "soloud_registerSlaveMixCallback");
        g_unregisterSlaveCallback = (SoloudUnregisterSlaveCallbackFn)dlsym(
            g_flutterRecorderHandle, "soloud_unregisterSlaveMixCallback");

        return g_registerSlaveCallback != nullptr;
#else
        return false;
#endif
    }

    // Initialize SoLoud in slave mode (no audio device created)
    result miniaudio_init_slave(SoLoud::Soloud *aSoloud, unsigned int aFlags,
                                unsigned int aSamplerate, unsigned int aBuffer,
                                unsigned int aChannels) {
        if (!lookupSlaveBridgeSymbols()) return UNKNOWN_ERROR;

        soloud = aSoloud;
        gSlaveMode = true;
        gSlaveChannels = aChannels;
        gSlaveSamplerate = aSamplerate;

        aSoloud->enableLockFreeMode();
        aSoloud->postinit_internal(aSamplerate, aBuffer, aFlags, aChannels);
        aSoloud->mBackendCleanupFunc = soloud_miniaudio_deinit;
        g_registerSlaveCallback(soloud_slave_mix_callback);
        aSoloud->mBackendString = "MiniAudio (Slave Mode)";
        return 0;
    }

    bool miniaudio_isSlaveMode_impl() { return gSlaveMode; }

    result miniaudio_ensureDeviceStarted_impl() {
        if (gSlaveMode) return 0;
        return miniaudio_ensure_thread_device_started();
    }

};
#endif
