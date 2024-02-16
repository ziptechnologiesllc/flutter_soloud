#include "capture.h"
#include "soloud.h"

#include <cstdarg>
#include <memory.h>

#define CAPTURE_BUFFER_SIZE 256
#define BIG_BUFFER_SIZE 44100 * 10

int currentFrame = 0;
float capturedBuffer[CAPTURE_BUFFER_SIZE];
float *bigBuffer;
//float bigBuffer[BIG_BUFFER_SIZE];
void data_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount)
{

    // Process the captured audio data as needed
    // For example, you can copy it to a buffer for later use.
    float *captured = (float *)(pInput); // Assuming float format
    // Do something with the captured audio data...
    // printf("framecound: %d   data_callback %f\n", frameCount, captured[0]);
    memcpy(capturedBuffer, captured, sizeof(float) * CAPTURE_BUFFER_SIZE);
    //memcpy(pOutput, captured, sizeof(float) * CAPTURE_BUFFER_SIZE);
    memcpy(&bigBuffer[CAPTURE_BUFFER_SIZE * currentFrame], captured, sizeof(float) * CAPTURE_BUFFER_SIZE);
    currentFrame++;
}

Capture::Capture() : mInited(false){};
Capture::~Capture()
{
    dispose();
}

std::vector<CaptureDevice> Capture::listCaptureDevices()
{
    printf("***************** LIST DEVICES START\n");
    std::vector<CaptureDevice> ret;
    if ((result = ma_context_init(NULL, 0, NULL, &context)) != MA_SUCCESS)
    {
        printf("Failed to initialize context %d\n", result);
        return ret;
    }

    if ((result = ma_context_get_devices(
             &context,
             &pPlaybackInfos,
             &playbackCount,
             &pCaptureInfos,
             &captureCount)) != MA_SUCCESS)
    {
        printf("Failed to get devices %d\n", result);
        return ret;
    }

    // Loop over each device info and do something with it. Here we just print
    // the name with their index. You may want
    // to give the user the opportunity to choose which device they'd prefer.
    for (ma_uint32 i = 0; i < playbackCount; i++)
    {
        printf("######%s %d - %s\n",
                     pCaptureInfos[i].isDefault ? " X" : "-",
                     i,
                     pCaptureInfos[i].name);
        CaptureDevice cd;
        cd.name = strdup(pCaptureInfos[i].name);
        cd.isDefault = pCaptureInfos[i].isDefault;
        ret.push_back(cd);
    }
    printf("***************** LIST DEVICES END\n");
    return ret;
}

CaptureErrors Capture::init(int deviceID, float* bufferFromDart)
{
    if (mInited) return capture_init_failed;
    deviceConfig = ma_device_config_init(ma_device_type_capture);
    deviceConfig.periodSizeInFrames = CAPTURE_BUFFER_SIZE;
    if (deviceID != -1)
        deviceConfig.capture.pDeviceID = &pPlaybackInfos[deviceID].id;
    deviceConfig.capture.format = ma_format_f32;
    deviceConfig.capture.channels = 1;
    deviceConfig.sampleRate = 44100;
    deviceConfig.dataCallback = data_callback;
    deviceConfig.pUserData = nullptr;

    result = ma_device_init(NULL, &deviceConfig, &device);
    if (result != MA_SUCCESS)
    {
        printf("Failed to initialize capture device.\n");
        return capture_init_failed;
    }
    initializeBuffer(bufferFromDart);
    mInited = true;

    return capture_noError;
}

void Capture::initializeBuffer(float* bufferFromDart)
{
    bigBuffer = (float *) bufferFromDart;
}

void Capture::dispose()
{
    mInited = false;
    ma_device_uninit(&device);
}

bool Capture::isInited()
{
    return mInited;
}

bool Capture::isStarted()
{
    ma_device_state result = ma_device_get_state(&device);
    return result == ma_device_state_started;
}

CaptureErrors Capture::startCapture()
{
    if (!mInited)
        return capture_not_inited;

    result = ma_device_start(&device);
    if (result != MA_SUCCESS)
    {
        ma_device_uninit(&device);
        printf("Failed to start device.\n");
        return failed_to_start_device;
    }
    return capture_noError;
}

CaptureErrors Capture::stopCapture()
{
    if (!mInited) {
        return capture_not_inited;

    }

    ma_device_uninit(&device);
    mInited = false;
    return capture_noError;
}

float *Capture::getWave()
{
    return capturedBuffer;
}

float *Capture::getFullWave()
{
    return bigBuffer;
}