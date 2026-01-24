/*
SoLoud audio engine
Copyright (c) 2013-2015 Jari Komppa

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

#include <string.h>
#include "soloud_internal.h"
#include "soloud_lockfree.h"

// Core "basic" operations - play, stop, etc

namespace SoLoud
{
	handle Soloud::play(AudioSource &aSound, float aVolume, float aPan, bool aPaused, unsigned int aBus)
	{
		if (aSound.mFlags & AudioSource::SINGLE_INSTANCE)
		{
			// Only one instance allowed, stop others
			aSound.stop();
		}

		// Creation of an audio instance may take significant amount of time,
		// so let's not do it inside the audio thread mutex.
		aSound.mSoloud = this;
		SoLoud::AudioSourceInstance *instance = aSound.createInstance();

		// Lock-free mode: use command queue instead of mutex
		if (mLockFreeMode.load(std::memory_order_acquire) && mVoiceSlotAllocator && mCommandQueue)
		{
			// Reserve a voice slot atomically (no lock needed)
			int ch = mVoiceSlotAllocator->reserveSlot();
			if (ch < 0)
			{
				delete instance;
				return UNKNOWN_ERROR;
			}

			// Assign audio source ID (atomic increment would be safer but this is rarely contended)
			if (!aSound.mAudioSourceID)
			{
				aSound.mAudioSourceID = mAudioSourceID++;
			}

			// Initialize the instance (safe from main thread - instance isn't in mVoice yet)
			instance->mAudioSourceID = aSound.mAudioSourceID;
			instance->mBusHandle = aBus;
			instance->init(aSound, mPlayIndex);
			m3dData[ch].init(aSound);

			// Create filters on the instance
			for (int i = 0; i < FILTERS_PER_STREAM; i++)
			{
				if (aSound.mFilter[i])
				{
					instance->mFilter[i] = aSound.mFilter[i]->createInstance();
				}
			}

			// Capture play index BEFORE incrementing (this is the index for this voice)
			unsigned int thisPlayIndex = mPlayIndex;

			// Increment play index for next voice
			mPlayIndex++;
			if (mPlayIndex == 0xfffff)
			{
				mPlayIndex = 0;
			}

			// Queue the play command for the audio thread
			AudioCommand cmd;
			cmd.type = CMD_PLAY;
			cmd.voiceIndex = ch;
			cmd.handle = 0; // Not used for CMD_PLAY
			cmd.params.play.volume = (aVolume < 0) ? aSound.mVolume : aVolume;
			cmd.params.play.pan = aPan;
			cmd.params.play.paused = aPaused;
			cmd.instancePtr = instance;

			if (!mCommandQueue->tryPush(cmd))
			{
				// Queue full - this shouldn't happen with properly sized queue
				mVoiceSlotAllocator->cancelReservation(ch);
				delete instance;
				return UNKNOWN_ERROR;
			}

			// Compute handle directly (can't use getHandleFromVoice_internal because
			// mVoice[ch] isn't set yet - it will be set by the audio thread)
			// Handle format: lower 12 bits = voice+1, upper bits = play index
			handle h = (ch + 1) | (thisPlayIndex << 12);
			return h;
		}

		// Original mutex-based path for non-slave mode
		lockAudioMutex_internal();
		int ch = findFreeVoice_internal();
		if (ch < 0)
		{
			unlockAudioMutex_internal();
			delete instance;
			return UNKNOWN_ERROR;
		}
		if (!aSound.mAudioSourceID)
		{
			aSound.mAudioSourceID = mAudioSourceID;
			mAudioSourceID++;
		}
		mVoice[ch] = instance;
		mVoice[ch]->mAudioSourceID = aSound.mAudioSourceID;
		mVoice[ch]->mBusHandle = aBus;
		mVoice[ch]->init(aSound, mPlayIndex);
		m3dData[ch].init(aSound);

		mPlayIndex++;

		// 20 bits, skip the last one (top bits full = voice group)
		if (mPlayIndex == 0xfffff)
		{
			mPlayIndex = 0;
		}

		if (aPaused)
		{
			mVoice[ch]->mFlags |= AudioSourceInstance::PAUSED;
		}

		setVoicePan_internal(ch, aPan);
		if (aVolume < 0)
		{
			setVoiceVolume_internal(ch, aSound.mVolume);
		}
		else
		{
			setVoiceVolume_internal(ch, aVolume);
		}

		// Fix initial voice volume ramp up
		int i;
		for (i = 0; i < MAX_CHANNELS; i++)
		{
			mVoice[ch]->mCurrentChannelVolume[i] = mVoice[ch]->mChannelVolume[i] * mVoice[ch]->mOverallVolume;
		}

		setVoiceRelativePlaySpeed_internal(ch, 1);

		for (i = 0; i < FILTERS_PER_STREAM; i++)
		{
			if (aSound.mFilter[i])
			{
				mVoice[ch]->mFilter[i] = aSound.mFilter[i]->createInstance();
			}
		}

		mActiveVoiceDirty = true;

		unlockAudioMutex_internal();

		int handle = getHandleFromVoice_internal(ch);
		return handle;
	}

	handle Soloud::playClocked(time aSoundTime, AudioSource &aSound, float aVolume, float aPan, unsigned int aBus)
	{
		handle h = play(aSound, aVolume, aPan, 1, aBus);
		lockAudioMutex_internal();
		// mLastClockedTime is cleared to zero at start of every output buffer
		time lasttime = mLastClockedTime;
		if (lasttime == 0)
		{
			mLastClockedTime = aSoundTime;
			lasttime = aSoundTime;
		}
		unlockAudioMutex_internal();
		int samples = (int)floor((aSoundTime - lasttime) * mSamplerate);
		// Make sure we don't delay too much (or overflow)
		if (samples < 0 || samples > 2048)		
			samples = 0;
		setDelaySamples(h, samples);
		setPause(h, 0);
		return h;
	}

	handle Soloud::playBackground(AudioSource &aSound, float aVolume, bool aPaused, unsigned int aBus)
	{
		handle h = play(aSound, aVolume, 0.0f, aPaused, aBus);
		setPanAbsolute(h, 1.0f, 1.0f);
		return h;
	}

	result Soloud::seek(handle aVoiceHandle, time aSeconds)
	{
		// In lock-free mode, queue the command for the audio thread
		if (mLockFreeMode.load(std::memory_order_acquire) && mCommandQueue)
		{
			AudioCommand cmd;
			cmd.type = CMD_SEEK;
			cmd.voiceIndex = -1;
			cmd.handle = aVoiceHandle;
			cmd.params.seek.position = aSeconds;
			cmd.instancePtr = nullptr;
			mCommandQueue->tryPush(cmd);
			return SO_NO_ERROR;
		}

		result res = SO_NO_ERROR;
		result singleres = SO_NO_ERROR;
		FOR_ALL_VOICES_PRE
			singleres = mVoice[ch]->seek(aSeconds, mScratch.mData, mScratchSize);
		if (singleres != SO_NO_ERROR)
			res = singleres;
		FOR_ALL_VOICES_POST
		return res;
	}


	void Soloud::stop(handle aVoiceHandle)
	{
		// In lock-free mode, queue the command for the audio thread
		if (mLockFreeMode.load(std::memory_order_acquire) && mCommandQueue)
		{
			AudioCommand cmd;
			cmd.type = CMD_STOP;
			cmd.voiceIndex = -1;
			cmd.handle = aVoiceHandle;
			cmd.instancePtr = nullptr;
			mCommandQueue->tryPush(cmd);
			return;
		}

		FOR_ALL_VOICES_PRE
			stopVoice_internal(ch);
		FOR_ALL_VOICES_POST
	}

	void Soloud::stopAudioSource(AudioSource &aSound)
	{
		if (aSound.mAudioSourceID)
		{
			lockAudioMutex_internal();
			
			int i;
			for (i = 0; i < (signed)mHighestVoice; i++)
			{
				if (mVoice[i] && mVoice[i]->mAudioSourceID == aSound.mAudioSourceID)
				{
					stopVoice_internal(i);
				}
			}
			unlockAudioMutex_internal();
		}
	}

	void Soloud::stopAll()
	{
		int i;
		lockAudioMutex_internal();
		for (i = 0; i < (signed)mHighestVoice; i++)
		{
			stopVoice_internal(i);
		}
		unlockAudioMutex_internal();
	}

	int Soloud::countAudioSource(AudioSource &aSound)
	{
		int count = 0;
		if (aSound.mAudioSourceID)
		{
			lockAudioMutex_internal();

			int i;
			for (i = 0; i < (signed)mHighestVoice; i++)
			{
				if (mVoice[i] && mVoice[i]->mAudioSourceID == aSound.mAudioSourceID)
				{
					count++;
				}
			}
			unlockAudioMutex_internal();
		}
		return count;
	}

}
