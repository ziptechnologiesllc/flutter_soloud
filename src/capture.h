#ifndef CAPTURE_H
#define CAPTURE_H

#include "enums.h"
#ifndef COMMON_H
#include "common.h"
#endif

#include <vector>
#include <string>

#include "soloud/src/backend/miniaudio/miniaudio.h"

struct CaptureDevice {
    char* name;
    unsigned int isDefault;
};

class Capture {
public:
    Capture();
    ~Capture();

    /// returns a list of available capture devices
    /// detected by miniaudio
    std::vector<CaptureDevice> listCaptureDevices();

    /// @brief initialize the capture with a [deviceID]. A list of devices
    ///     can be acquired with [listCaptureDevices].
    ///     If [deviceID] is -1, the default will be used
    /// @param deviceID 
    /// @return 
    CaptureErrors init(int deviceID, float* bufferFromDart);

    /// @brief Must be called when there is no more need of the capture or when closing the app
    /// @return 
    void dispose();

    bool isInited();
    bool isStarted();
    CaptureErrors startCapture();
    CaptureErrors stopCapture();
    void initializeBuffer(float* bufferFromDart);

    float *getWave();
    float *getFullWave();

private:
    ma_context context;
    ma_device_info *pPlaybackInfos;
    ma_uint32 playbackCount;
    ma_device_info *pCaptureInfos;
    ma_uint32 captureCount;
    ma_result result;
    // ma_encoder_config encoderConfig;
    // ma_encoder encoder;
    ma_device_config deviceConfig;
    ma_device device;

    /// true when the capture is initialized
    bool mInited;
};




#endif // CAPTURE_H