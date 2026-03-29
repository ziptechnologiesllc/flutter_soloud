/*
SoLoud audio engine - Lock-free command queue for slave mode
Copyright (c) 2024

This is an SPSC (Single-Producer Single-Consumer) lock-free queue
for passing commands from the main thread to the audio thread.

Usage:
- Main thread: push commands (play, stop, setVolume, etc.)
- Audio thread: pop and execute commands at start of mix()
*/

#ifndef SOLOUD_LOCKFREE_H
#define SOLOUD_LOCKFREE_H

#include <atomic>
#include <cstdint>

namespace SoLoud
{
    // Command types
    enum CommandType : uint8_t
    {
        CMD_NONE = 0,
        CMD_PLAY,           // Start playing a voice (voice slot already allocated)
        CMD_STOP,           // Stop a voice
        CMD_STOP_ALL,       // Stop all voices
        CMD_SET_PAUSE,      // Pause/unpause a voice
        CMD_SET_VOLUME,     // Set voice volume
        CMD_SET_PAN,        // Set voice pan
        CMD_SET_SPEED,      // Set relative play speed
        CMD_SEEK,           // Seek to position
        CMD_SET_LOOPING,    // Set looping state
        CMD_SET_DELAY,      // Set delay samples
        CMD_FADE_VOLUME,    // Fade volume over time
        CMD_SCHEDULE_PAUSE, // Schedule pause at time
        CMD_SCHEDULE_STOP,  // Schedule stop at time
    };

    // Command structure - 64 bytes for cache line alignment
    struct alignas(64) AudioCommand
    {
        CommandType type;
        uint8_t padding1[3];

        // Voice identification
        int voiceIndex;         // Direct voice index for pre-allocated slots
        unsigned int handle;    // Handle for existing voices

        // Command parameters (union to save space)
        union {
            struct { float volume; float pan; bool paused; bool looping; } play;
            struct { float value; } setFloat;
            struct { unsigned int value; } setUint;
            struct { float target; float time; } fade;
            struct { double position; } seek;
            struct { bool value; } setBool;
            struct { float time; } schedule;
        } params;

        // For CMD_PLAY: pointer to pre-created instance (ownership transferred)
        void* instancePtr;

        uint8_t padding2[16]; // Pad to 64 bytes
    };

    // Lock-free SPSC ring buffer for commands
    // Power-of-2 size for efficient modulo
    class CommandQueue
    {
    public:
        static constexpr size_t QUEUE_SIZE = 256; // Must be power of 2
        static constexpr size_t QUEUE_MASK = QUEUE_SIZE - 1;

        CommandQueue() : mWritePos(0), mReadPos(0)
        {
            for (size_t i = 0; i < QUEUE_SIZE; i++)
            {
                mCommands[i].type = CMD_NONE;
            }
        }

        // Producer (main thread): try to push a command
        // Returns true if successful, false if queue is full
        bool tryPush(const AudioCommand& cmd)
        {
            size_t writePos = mWritePos.load(std::memory_order_relaxed);
            size_t nextWritePos = (writePos + 1) & QUEUE_MASK;

            // Check if queue is full
            if (nextWritePos == mReadPos.load(std::memory_order_acquire))
            {
                return false; // Queue full
            }

            // Write the command
            mCommands[writePos] = cmd;

            // Publish the write
            mWritePos.store(nextWritePos, std::memory_order_release);
            return true;
        }

        // Consumer (audio thread): try to pop a command
        // Returns true if a command was available, false if queue empty
        bool tryPop(AudioCommand& cmd)
        {
            size_t readPos = mReadPos.load(std::memory_order_relaxed);

            // Check if queue is empty
            if (readPos == mWritePos.load(std::memory_order_acquire))
            {
                return false; // Queue empty
            }

            // Read the command
            cmd = mCommands[readPos];

            // Publish the read
            mReadPos.store((readPos + 1) & QUEUE_MASK, std::memory_order_release);
            return true;
        }

        // Check if queue is empty (for polling)
        bool isEmpty() const
        {
            return mReadPos.load(std::memory_order_acquire) ==
                   mWritePos.load(std::memory_order_acquire);
        }

        // Approximate number of pending commands
        size_t size() const
        {
            size_t w = mWritePos.load(std::memory_order_relaxed);
            size_t r = mReadPos.load(std::memory_order_relaxed);
            return (w - r) & QUEUE_MASK;
        }

    private:
        alignas(64) AudioCommand mCommands[QUEUE_SIZE];
        alignas(64) std::atomic<size_t> mWritePos;
        alignas(64) std::atomic<size_t> mReadPos;
    };

    // Atomic voice slot allocator
    // Main thread can reserve a slot, audio thread confirms or rejects
    class VoiceSlotAllocator
    {
    public:
        static constexpr int INVALID_SLOT = -1;
        static constexpr int MAX_VOICES = 1024; // Match VOICE_COUNT

        VoiceSlotAllocator()
        {
            for (int i = 0; i < MAX_VOICES; i++)
            {
                // 0 = free, 1 = reserved (main thread), 2 = active (audio thread)
                mSlotState[i].store(0, std::memory_order_relaxed);
            }
        }

        // Main thread: reserve a free slot
        // Returns slot index or INVALID_SLOT if none available
        int reserveSlot()
        {
            for (int i = 0; i < MAX_VOICES; i++)
            {
                int expected = 0; // Free
                if (mSlotState[i].compare_exchange_strong(
                    expected, 1, // Reserve
                    std::memory_order_acq_rel))
                {
                    return i;
                }
            }
            return INVALID_SLOT;
        }

        // Audio thread: activate a reserved slot (after processing CMD_PLAY)
        void activateSlot(int slot)
        {
            if (slot >= 0 && slot < MAX_VOICES)
            {
                mSlotState[slot].store(2, std::memory_order_release);
            }
        }

        // Audio thread: free a slot (after voice stops)
        void freeSlot(int slot)
        {
            if (slot >= 0 && slot < MAX_VOICES)
            {
                mSlotState[slot].store(0, std::memory_order_release);
            }
        }

        // Cancel a reservation (if play fails before audio thread processes it)
        void cancelReservation(int slot)
        {
            if (slot >= 0 && slot < MAX_VOICES)
            {
                int expected = 1; // Reserved
                mSlotState[slot].compare_exchange_strong(
                    expected, 0, // Free
                    std::memory_order_acq_rel);
            }
        }

        // Check if slot is active (for handle validation)
        bool isActive(int slot) const
        {
            if (slot < 0 || slot >= MAX_VOICES) return false;
            return mSlotState[slot].load(std::memory_order_acquire) == 2;
        }

    private:
        alignas(64) std::atomic<int> mSlotState[MAX_VOICES];
    };

} // namespace SoLoud

#endif // SOLOUD_LOCKFREE_H
