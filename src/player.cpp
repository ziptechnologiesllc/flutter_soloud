#include "common.h"
#include "player.h"
#include "soloud.h"
#include "synth/basic_wave.h"

#include <algorithm>
#include <cstdarg>
#include <random> 
#ifdef _IS_WIN_
#include <stddef.h> // for size_t
#else
#include <unistd.h>
#endif

Player::Player() : mInited(false), mFilters(&soloud){};
Player::~Player()
{
    dispose();
}

PlayerErrors Player::init()
{
    if (mInited)
        dispose();

    // initialize SoLoud.
    SoLoud::result result = soloud.init(
        SoLoud::Soloud::CLIP_ROUNDOFF,
        SoLoud::Soloud::MINIAUDIO, 44100, 2048, 2U);
    // soloud.init(1U, 0U, 44100, 2048, 2U);
    // SoLoud::Thread::sleep(100);
    if (result == SoLoud::SO_NO_ERROR)
        mInited = true;
    else
        result = backendNotInited;

    return (PlayerErrors)result;
}

void Player::dispose()
{
    // Clean up SoLoud
    soloud.deinit();
    mInited = false;
    sounds.clear();
}

bool Player::isInited()
{
    return mInited;
}

int Player::getSoundsCount()
{
    return (int)sounds.size();
}

const std::string Player::getErrorString(PlayerErrors aErrorCode) const
{
    switch (aErrorCode)
    {
    case noError:
        return "No error";
    case invalidParameter:
        return "Some parameter is invalid";
    case fileNotFound:
        return "File not found";
    case fileLoadFailed:
        return "File found, but could not be loaded";
    case fileAlreadyLoaded:
        return "File already loaded";
    case dllNotFound:
        return "DLL not found, or wrong DLL";
    case outOfMemory:
        return "Out of memory";
    case notImplemented:
        return "Feature not implemented";
    case backendNotInited:
        return "Player not yet initialized";
    case filterNotFound:
        return "Filter not found";
    case unknownError:
        return "Unknown error";
    }
    return "Other error";
}

PlayerErrors Player::loadFile(const std::string &completeFileName, unsigned int &hash)
{
    if (!mInited)
        return backendNotInited;

    hash = 0;

    unsigned int newHash = (unsigned int)std::hash<std::string>{}(completeFileName);
    /// check if the sound has been already loaded
    auto const &s = std::find_if(
        sounds.begin(), sounds.end(),
        [&](std::unique_ptr<ActiveSound> const &f)
        { return f->soundHash == newHash; });

    if (s != sounds.end()) {
        hash = newHash;
        return fileAlreadyLoaded;
    }

    sounds.push_back(std::make_unique<ActiveSound>());
    sounds.back().get()->completeFileName = std::string(completeFileName);
    hash = sounds.back().get()->soundHash = newHash;
    sounds.back().get()->sound = std::make_unique<SoLoud::Wav>();
    sounds.back().get()->soundType = TYPE_WAV;
    SoLoud::result result = 
        static_cast<SoLoud::Wav*>(sounds.back().get()->sound.get())->load(completeFileName.c_str());
    if (result != SoLoud::SO_NO_ERROR)
    {
        sounds.emplace_back();
    }
    return (PlayerErrors)result;
}

PlayerErrors Player::loadFromMemory(const float *buffer, unsigned int &hash, unsigned int &length)
{
    if (!mInited)
        return backendNotInited;

    hash = 0;

    unsigned int newHash = (unsigned int)std::hash<std::string>{}("memory-mapped-sample");
    /// check if the sound has been already loaded
    auto const &s = std::find_if(
            sounds.begin(), sounds.end(),
            [&](std::unique_ptr<ActiveSound> const &f)
            { return f->soundHash == newHash; });
    if (s != sounds.end()) {
        hash = newHash;
        return fileAlreadyLoaded;
    }

    sounds.push_back(std::make_unique<ActiveSound>());
    sounds.back().get()->completeFileName = std::string("memory-mapped-sample");
    hash = sounds.back().get()->soundHash = newHash;
    sounds.back().get()->sound = std::make_unique<SoLoud::Wav>();
    sounds.back().get()->soundType = TYPE_WAV;
    SoLoud::result result =
            static_cast<SoLoud::Wav*>(sounds.back().get()->sound.get())->loadRawWav(buffer, length, 44100.0f, 2, false, false);
    if (result != SoLoud::SO_NO_ERROR)
    {
        sounds.emplace_back();
    }
    return (PlayerErrors)result;
}

PlayerErrors Player::loadWaveform(
        int waveform, 
        bool superWave,
        float scale,
        float detune,
        unsigned int &hash)
{
    if (!mInited)
        return backendNotInited;

    hash = 0;

    std::random_device rd;
    std::mt19937 g(rd());
    std::uniform_int_distribution<unsigned int> dist(0, INT64_MAX);

    hash = dist(g);
    
    sounds.push_back(std::make_unique<ActiveSound>());
    sounds.back().get()->completeFileName = "";
    hash = sounds.back().get()->soundHash = hash;
    sounds.back().get()->sound = std::make_unique<Basicwave>
        ((SoLoud::Soloud::WAVEFORM)waveform, superWave, detune, scale);
    sounds.back().get()->soundType = TYPE_SYNTH;

    return noError;
}

void Player::setWaveformScale(unsigned int soundHash, float newScale)
{
    auto const &s = std::find_if(sounds.begin(), sounds.end(),
                                 [&](std::unique_ptr<ActiveSound> const &f)
                                 { return f->soundHash == soundHash; });

    if (s == sounds.end() || s->get()->soundType != TYPE_SYNTH)
        return;

    static_cast<Basicwave*>(s->get()->sound.get())->setScale(newScale);
}

void Player::setWaveformDetune(unsigned int soundHash, float newDetune)
{
    auto const &s = std::find_if(sounds.begin(), sounds.end(),
                                 [&](std::unique_ptr<ActiveSound> const &f)
                                 { return f->soundHash == soundHash; });

    if (s == sounds.end() || s->get()->soundType != TYPE_SYNTH)
        return;

    static_cast<Basicwave*>(s->get()->sound.get())->setDetune(newDetune);
}

void Player::setWaveform(unsigned int soundHash, int newWaveform)
{
    auto const &s = std::find_if(sounds.begin(), sounds.end(),
                                 [&](std::unique_ptr<ActiveSound> const &f)
                                 { return f->soundHash == soundHash; });

    if (s == sounds.end() || s->get()->soundType != TYPE_SYNTH)
        return;

    static_cast<Basicwave*>(s->get()->sound.get())->
        setWaveform((SoLoud::Soloud::WAVEFORM)newWaveform);
}

void Player::setWaveformFreq(unsigned int soundHash, float newFreq)
{
    auto const &s = std::find_if(sounds.begin(), sounds.end(),
                                 [&](std::unique_ptr<ActiveSound> const &f)
                                 { return f->soundHash == soundHash; });

    if (s == sounds.end() || s->get()->soundType != TYPE_SYNTH)
        return;

    static_cast<Basicwave*>(s->get()->sound.get())->setFreq(newFreq);
}

void Player::setWaveformSuperwave(unsigned int soundHash, bool superwave)
{
    auto const &s = std::find_if(sounds.begin(), sounds.end(),
                                 [&](std::unique_ptr<ActiveSound> const &f)
                                 { return f->soundHash == soundHash; });

    if (s == sounds.end() || s->get()->soundType != TYPE_SYNTH)
        return;

    static_cast<Basicwave*>(s->get()->sound.get())->setSuperWave(superwave);
}


void Player::pauseSwitch(unsigned int handle)
{
    setPause(handle, !soloud.getPause(handle));
}

void Player::setPause(unsigned int handle, bool pause)
{
    soloud.setPause(handle, pause);
}

bool Player::getPause(unsigned int handle)
{
    return soloud.getPause(handle);
}


void Player::setRelativePlaySpeed(unsigned int handle, float speed)
{
    if (speed < 0.05) speed = 0.05;
    soloud.setRelativePlaySpeed(handle, speed);
}

float Player::getRelativePlaySpeed(unsigned int handle)
{
    return soloud.getRelativePlaySpeed(handle);
}

unsigned int Player::play(
    unsigned int soundHash,
    float volume,
    float pan,
    bool paused)
{
    auto const &s = std::find_if(sounds.begin(), sounds.end(),
                                 [&](std::unique_ptr<ActiveSound> const &f)
                                 { return f->soundHash == soundHash; });

    if (s == sounds.end())
        return 0;

    ActiveSound *sound = s->get();
    SoLoud::handle newHandle = soloud.play(*sound->sound.get(), volume, pan, paused, 0);
    sound->handle.emplace_back(newHandle);
    return newHandle;
}

void Player::stop(unsigned int handle)
{
    int handleId;
    ActiveSound *sound = findByHandle(handle, &handleId);
    if (sound == nullptr)
        return;
    soloud.stop(handle);
    // remove the handle from the list
    sound->handle.erase(std::remove_if(sound->handle.begin(), sound->handle.end(),
                                       [handle](SoLoud::handle &f)
                                       { return f == handle; }));
}

void Player::disposeSound(unsigned int soundHash)
{
    auto const &s = std::find_if(sounds.begin(), sounds.end(),
                                 [&](std::unique_ptr<ActiveSound> const &f)
                                 { return f->soundHash == soundHash; });

    if (s == sounds.end())
        return;

    s->get()->sound.get()->stop();
    // remove the sound from the list
    sounds.erase(std::remove_if(sounds.begin(), sounds.end(),
                                [soundHash](std::unique_ptr<ActiveSound> &f)
                                { return f.get()->soundHash == soundHash; }));
}

void Player::disposeAllSound()
{
    soloud.stopAll();
    sounds.clear();
}

void Player::setLooping(unsigned int handle, bool enable)
{
    soloud.setLooping(handle, enable);
}

PlayerErrors Player::textToSpeech(const std::string &textToSpeech, unsigned int &handle)
{
    if (!mInited)
        return backendNotInited;

    sounds.push_back(std::make_unique<ActiveSound>());
    sounds.back().get()->completeFileName = std::string("");
    SoLoud::result result = speech.setText(textToSpeech.c_str());
    if (result == SoLoud::SO_NO_ERROR)
    {
        handle = soloud.play(speech);
        sounds.back().get()->handle.push_back(handle);
    }
    else
    {
        sounds.emplace_back();
    }
    return (PlayerErrors)result;
}

void Player::setVisualizationEnabled(bool enabled)
{
    soloud.setVisualizationEnable(enabled);
}

bool Player::isVisualizationEnabled()
{
    return soloud.mFlags & SoLoud::Soloud::ENABLE_VISUALIZATION;
}

float *Player::calcFFT()
{
    return soloud.calcFFT();
}

float *Player::getWave()
{
    return soloud.getWave();
}

// The length in seconds
double Player::getLength(unsigned int soundHash)
{
    auto const &s = std::find_if(sounds.begin(), sounds.end(),
                                 [&](std::unique_ptr<ActiveSound> const &f)
                                 { return f->soundHash == soundHash; });
    if (s == sounds.end() || s->get()->soundType != TYPE_WAV)
        return 0.0;
    return static_cast<SoLoud::Wav*>(s->get()->sound.get())->getLength();
}

// time in seconds
PlayerErrors Player::seek(SoLoud::handle handle, float time)
{
    if (!mInited)
        return backendNotInited;

    SoLoud::result result = soloud.seek(handle, time);
    return (PlayerErrors)result;
}

// returns time in seconds
double Player::getPosition(SoLoud::handle handle)
{
    return soloud.getStreamPosition(handle);
}

float Player::getGlobalVolume()
{
    return soloud.getGlobalVolume();
}

void Player::setGlobalVolume(float volume)
{
    return soloud.setGlobalVolume(volume);
}

float Player::getVolume(SoLoud::handle handle)
{
    return soloud.getVolume(handle);
}

void Player::setVolume(SoLoud::handle handle, float volume)
{
    return soloud.setVolume(handle, volume);
}

bool Player::getIsValidVoiceHandle(SoLoud::handle handle)
{
    return soloud.isValidVoiceHandle(handle);
}

ActiveSound *Player::findByHandle(SoLoud::handle handle, int *handleId)
{
    *handleId = -1;
    int i = 0;
    // TODO: do a better std pls! \o/ :)
    while (i < (int)sounds.size())
    {
        int index = 0;
        while (index < (int)sounds[i].get()->handle.size())
        {
            if (sounds[i].get()->handle[index] == handle)
            {
                *handleId = index;
                return sounds[i].get();
            }
            ++index;
        }
        ++i;
    }

    return nullptr;
}

void Player::debug()
{
    int n = 0;
    for (auto &sound : sounds)
    {
        printf("%d: \thandle: ", n);
        for (auto &handle : sound.get()->handle)
            printf("%d ", handle);
        printf("  %s\n", sound.get()->completeFileName.c_str());

        n++;
    }
}

/////////////////////////////////////////
/// faders
/////////////////////////////////////////

void Player::fadeGlobalVolume(float to, float time)
{
    soloud.fadeGlobalVolume(to, time);
}

void Player::fadeVolume(SoLoud::handle handle, float to, float time)
{
    soloud.fadeVolume(handle, to, time);
}

void Player::fadePan(SoLoud::handle handle, float to, float time)
{
    soloud.fadePan(handle, to, time);
}

void Player::fadeRelativePlaySpeed(SoLoud::handle handle, float to, float time)
{
    soloud.fadeRelativePlaySpeed(handle, to, time);
}

void Player::schedulePause(SoLoud::handle handle, float time)
{
    soloud.schedulePause(handle, time);
}

void Player::scheduleStop(SoLoud::handle handle, float time)
{
    soloud.scheduleStop(handle, time);
}

void Player::oscillateVolume(SoLoud::handle handle, float from, float to, float time)
{
    soloud.oscillateVolume(handle, from, to, time);
}

void Player::oscillatePan(SoLoud::handle handle, float from, float to, float time)
{
    soloud.oscillatePan(handle, from, to, time);
}

void Player::oscillateRelativePlaySpeed(SoLoud::handle handle, float from, float to, float time)
{
    soloud.oscillateRelativePlaySpeed(handle, from, to, time);
}

void Player::oscillateGlobalVolume(float from, float to, float time)
{
    soloud.oscillateGlobalVolume(from, to, time);
}


/////////////////////////////////////////
/// 3D audio methods
/////////////////////////////////////////

void Player::update3dAudio()
{
    soloud.update3dAudio();
}

unsigned int Player::play3d(
    unsigned int soundHash,
    float aPosX,
    float aPosY,
    float aPosZ,
    float aVelX,
    float aVelY,
    float aVelZ,
    float aVolume,
    bool aPaused,
    unsigned int aBus)
{
    auto const &s = std::find_if(sounds.begin(), sounds.end(),
                                 [&](std::unique_ptr<ActiveSound> const &f)
                                 { return f->soundHash == soundHash; });
    if (s == sounds.end())
        return 0;

    ActiveSound *sound = s->get();
    SoLoud::handle newHandle = soloud.play3d(
        *sound->sound.get(),
        aPosX, aPosY, aPosZ,
        aVelX, aVelY, aVelZ,
        aVolume,
        aPaused,
        aBus);
    sound->handle.emplace_back(newHandle);
    return newHandle;
}

void Player::set3dSoundSpeed(float speed)
{
    soloud.set3dSoundSpeed(speed);
}

float Player::get3dSoundSpeed()
{
    return soloud.get3dSoundSpeed();
}

void Player::set3dListenerParameters(
    float aPosX, float aPosY, float aPosZ,
    float aAtX, float aAtY, float aAtZ,
    float aUpX, float aUpY, float aUpZ,
    float aVelocityX, float aVelocityY, float aVelocityZ)
{
    soloud.set3dListenerParameters(
        aPosX, aPosY, aPosZ,
        aAtX, aAtY, aAtZ,
        aUpX, aUpY, aUpZ,
        aVelocityX, aVelocityY, aVelocityZ);
}

void Player::set3dListenerPosition(float aPosX,
                                   float aPosY,
                                   float aPosZ)
{
    soloud.set3dListenerPosition(aPosX, aPosY, aPosZ);
}

void Player::set3dListenerAt(float aAtX,
                             float aAtY,
                             float aAtZ)
{
    soloud.set3dListenerAt(aAtX, aAtY, aAtZ);
}

void Player::set3dListenerUp(float aUpX,
                             float aUpY,
                             float aUpZ)
{
    soloud.set3dListenerAt(aUpX, aUpY, aUpZ);
}

void Player::set3dListenerVelocity(float aVelocityX,
                                   float aVelocityY,
                                   float aVelocityZ)
{
    soloud.set3dListenerVelocity(aVelocityX, aVelocityY, aVelocityZ);
}

void Player::set3dSourceParameters(
    unsigned int aVoiceHandle,
    float aPosX, float aPosY, float aPosZ,
    float aVelocityX, float aVelocityY, float aVelocityZ)
{
    soloud.set3dSourceParameters(aVoiceHandle,
                                 aPosX, aPosY, aPosZ,
                                 aVelocityX, aVelocityY, aVelocityZ);
}

void Player::set3dSourcePosition(
    unsigned int aVoiceHandle,
    float aPosX,
    float aPosY,
    float aPosZ)
{
    soloud.set3dSourcePosition(aVoiceHandle, aPosX, aPosY, aPosZ);
}

void Player::set3dSourceVelocity(
    unsigned int aVoiceHandle,
    float aVelocityX,
    float aVelocityY,
    float aVelocityZ)
{
    soloud.set3dSourceVelocity(aVoiceHandle, aVelocityX, aVelocityY, aVelocityZ);
}

void Player::set3dSourceMinMaxDistance(
    unsigned int aVoiceHandle,
    float aMinDistance,
    float aMaxDistance)
{
    soloud.set3dSourceMinMaxDistance(aVoiceHandle, aMinDistance, aMaxDistance);
}

void Player::set3dSourceAttenuation(
    unsigned int aVoiceHandle,
    unsigned int aAttenuationModel,
    float aAttenuationRolloffFactor)
{
    soloud.set3dSourceAttenuation(aVoiceHandle, aAttenuationModel, aAttenuationRolloffFactor);
}

void Player::set3dSourceDopplerFactor(
    unsigned int aVoiceHandle,
    float aDopplerFactor)
{
    soloud.set3dSourceDopplerFactor(aVoiceHandle, aDopplerFactor);
}