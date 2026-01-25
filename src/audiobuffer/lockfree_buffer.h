#ifndef LOCKFREE_BUFFER_H
#define LOCKFREE_BUFFER_H

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

/// Lock-free SPSC (Single Producer Single Consumer) ring buffer for audio streaming.
/// Producer (addData) and consumer (getAudio) can run on different threads without locks.
/// This is critical for real-time audio where the audio callback cannot block.
class LockFreeAudioBuffer {
private:
    std::vector<float> mBuffer;          // Pre-allocated ring buffer
    size_t mCapacity;                     // Capacity in floats
    std::atomic<size_t> mWritePos{0};    // Producer writes here (only producer modifies)
    std::atomic<size_t> mReadPos{0};     // Consumer reads here (only consumer modifies)

    // Cache line padding to prevent false sharing between read and write positions
    char mPadding[64];

public:
    LockFreeAudioBuffer() : mCapacity(0) {}

    ~LockFreeAudioBuffer() = default;

    /// Initialize with capacity in bytes (will be converted to floats)
    void setSizeInBytes(size_t bytes) {
        mCapacity = bytes / sizeof(float);
        if (mCapacity < 1024) mCapacity = 1024;  // Minimum size
        mBuffer.resize(mCapacity, 0.0f);
        mWritePos.store(0, std::memory_order_relaxed);
        mReadPos.store(0, std::memory_order_relaxed);
    }

    /// Get available space for writing (floats)
    size_t availableForWrite() const {
        size_t write = mWritePos.load(std::memory_order_acquire);
        size_t read = mReadPos.load(std::memory_order_acquire);

        if (write >= read) {
            // Write ahead of read: available = capacity - (write - read) - 1
            // We keep 1 slot empty to distinguish full from empty
            return mCapacity - (write - read) - 1;
        } else {
            // Read ahead of write (wrapped): available = read - write - 1
            return read - write - 1;
        }
    }

    /// Get available data for reading (floats)
    size_t availableForRead() const {
        size_t write = mWritePos.load(std::memory_order_acquire);
        size_t read = mReadPos.load(std::memory_order_acquire);

        if (write >= read) {
            return write - read;
        } else {
            // Wrapped: data from read to end + data from start to write
            return (mCapacity - read) + write;
        }
    }

    /// Add float data to buffer (called from producer thread)
    /// Returns number of floats actually written
    size_t addData(const float* data, size_t numFloats) {
        if (mCapacity == 0 || numFloats == 0) return 0;

        size_t available = availableForWrite();
        size_t toWrite = (numFloats > available) ? available : numFloats;
        if (toWrite == 0) return 0;

        size_t write = mWritePos.load(std::memory_order_relaxed);

        // Handle wrap-around
        size_t firstPart = mCapacity - write;
        if (firstPart > toWrite) firstPart = toWrite;

        // Copy first part (from write to end of buffer)
        std::memcpy(&mBuffer[write], data, firstPart * sizeof(float));

        // Copy second part if wrapped (from start of buffer)
        if (toWrite > firstPart) {
            std::memcpy(&mBuffer[0], data + firstPart, (toWrite - firstPart) * sizeof(float));
        }

        // Update write position atomically (release ensures data is visible to consumer)
        size_t newWrite = (write + toWrite) % mCapacity;
        mWritePos.store(newWrite, std::memory_order_release);

        return toWrite;
    }

    /// Read float data from buffer (called from consumer/audio thread)
    /// Returns number of floats actually read
    /// This is NON-BLOCKING - returns 0 if no data available
    size_t readData(float* dest, size_t maxFloats) {
        if (mCapacity == 0 || maxFloats == 0) return 0;

        size_t available = availableForRead();
        size_t toRead = (maxFloats > available) ? available : maxFloats;
        if (toRead == 0) return 0;

        size_t read = mReadPos.load(std::memory_order_relaxed);

        // Handle wrap-around
        size_t firstPart = mCapacity - read;
        if (firstPart > toRead) firstPart = toRead;

        // Copy first part (from read to end of buffer)
        std::memcpy(dest, &mBuffer[read], firstPart * sizeof(float));

        // Copy second part if wrapped (from start of buffer)
        if (toRead > firstPart) {
            std::memcpy(dest + firstPart, &mBuffer[0], (toRead - firstPart) * sizeof(float));
        }

        // Update read position atomically (release ensures consumer has finished reading)
        size_t newRead = (read + toRead) % mCapacity;
        mReadPos.store(newRead, std::memory_order_release);

        return toRead;
    }

    /// Peek at data without consuming (for PRESERVED mode)
    /// Returns number of floats available to peek
    size_t peekData(float* dest, size_t maxFloats, size_t offset = 0) const {
        if (mCapacity == 0 || maxFloats == 0) return 0;

        size_t available = availableForRead();
        if (offset >= available) return 0;

        size_t canPeek = available - offset;
        size_t toPeek = (maxFloats > canPeek) ? canPeek : maxFloats;
        if (toPeek == 0) return 0;

        size_t read = (mReadPos.load(std::memory_order_acquire) + offset) % mCapacity;

        // Handle wrap-around
        size_t firstPart = mCapacity - read;
        if (firstPart > toPeek) firstPart = toPeek;

        std::memcpy(dest, &mBuffer[read], firstPart * sizeof(float));

        if (toPeek > firstPart) {
            std::memcpy(dest + firstPart, &mBuffer[0], (toPeek - firstPart) * sizeof(float));
        }

        return toPeek;
    }

    /// Clear the buffer
    void clear() {
        mWritePos.store(0, std::memory_order_release);
        mReadPos.store(0, std::memory_order_release);
    }

    /// Get capacity in floats
    size_t capacity() const { return mCapacity; }

    /// Check if buffer is empty
    bool empty() const { return availableForRead() == 0; }
};

#endif // LOCKFREE_BUFFER_H
