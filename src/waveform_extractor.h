#ifndef WAVEFORM_EXTRACTOR_H
#define WAVEFORM_EXTRACTOR_H

#include "soloud_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Synchronous waveform extraction (blocks calling thread)
FFI_PLUGIN_EXPORT int extractSamplesFromLoadedSource(
    unsigned int hash,
    float startTime,
    float endTime,
    unsigned long numSamplesNeeded,
    bool average,
    float *pSamples
);

/// Callback for async waveform extraction
/// Parameters: soundHash, error (0 = success)
typedef void (*WaveformExtractedCallback)(unsigned int soundHash, int error);

/// Set the callback for async waveform extraction
FFI_PLUGIN_EXPORT void setWaveformExtractedCallback(WaveformExtractedCallback callback);

/// Clear the callback
FFI_PLUGIN_EXPORT void clearWaveformExtractedCallback();

/// Async waveform extraction - runs on background thread, zero copy
/// outBuffer must be pre-allocated by caller with numSamples floats
/// Callback is invoked when extraction completes
FFI_PLUGIN_EXPORT void extractWaveformAsync(
    unsigned int hash,
    float* outBuffer,
    unsigned int numSamples,
    float startTime,
    float endTime,
    bool average
);

#ifdef __cplusplus
}
#endif

#endif // WAVEFORM_EXTRACTOR_H
