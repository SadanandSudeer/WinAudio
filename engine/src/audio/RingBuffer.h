#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace winaudio::audio {

// Cache-line-aligned Lock-Free Single-Producer Single-Consumer (SPSC) stereo float32 ring buffer.
// Capacity is always a power of two in stereo frames.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
class StereoRingBuffer {
public:
    explicit StereoRingBuffer(uint32_t minCapacityFrames = 32768) {
        resize(minCapacityFrames);
    }

    void resize(uint32_t minCapacityFrames) {
        uint32_t cap = 1024;
        while (cap < minCapacityFrames) cap <<= 1;
        capacity_ = cap;
        mask_ = cap - 1;
        buffer_.assign(static_cast<size_t>(cap) * 2, 0.0f);
        reset();
    }

    void reset() noexcept {
        writePos_.store(0, std::memory_order_relaxed);
        readPos_.store(0, std::memory_order_relaxed);
    }

    uint32_t capacity() const noexcept { return capacity_; }

    uint32_t availableRead() const noexcept {
        const uint32_t w = writePos_.load(std::memory_order_acquire);
        const uint32_t r = readPos_.load(std::memory_order_relaxed);
        return w - r;
    }

    uint32_t availableWrite() const noexcept {
        const uint32_t w = writePos_.load(std::memory_order_relaxed);
        const uint32_t r = readPos_.load(std::memory_order_acquire);
        return capacity_ - (w - r);
    }

    // Producer: write up to numFrames interleaved stereo samples. Returns frames written.
    uint32_t write(const float* stereoIn, uint32_t numFrames) noexcept {
        const uint32_t w = writePos_.load(std::memory_order_relaxed);
        const uint32_t r = readPos_.load(std::memory_order_acquire);
        const uint32_t freeFrames = capacity_ - (w - r);
        const uint32_t toWrite = (numFrames < freeFrames) ? numFrames : freeFrames;
        if (toWrite == 0) return 0;

        const uint32_t startIdx = w & mask_;
        const uint32_t firstPart = std::min(toWrite, capacity_ - startIdx);
        std::memcpy(&buffer_[static_cast<size_t>(startIdx) * 2], stereoIn, static_cast<size_t>(firstPart) * 2 * sizeof(float));

        const uint32_t secondPart = toWrite - firstPart;
        if (secondPart > 0) {
            std::memcpy(&buffer_[0], stereoIn + static_cast<size_t>(firstPart) * 2, static_cast<size_t>(secondPart) * 2 * sizeof(float));
        }

        writePos_.store(w + toWrite, std::memory_order_release);
        return toWrite;
    }

    // Consumer: pop a single stereo frame (fast inline path for ASRC pull).
    inline bool popFrame(float& outL, float& outR) noexcept {
        const uint32_t r = readPos_.load(std::memory_order_relaxed);
        const uint32_t w = writePos_.load(std::memory_order_acquire);
        if (r == w) return false;

        const uint32_t idx = r & mask_;
        outL = buffer_[static_cast<size_t>(idx) * 2];
        outR = buffer_[static_cast<size_t>(idx) * 2 + 1];
        readPos_.store(r + 1, std::memory_order_release);
        return true;
    }

    // Consumer: read up to numFrames interleaved stereo samples. Returns frames read.
    uint32_t read(float* stereoOut, uint32_t numFrames) noexcept {
        const uint32_t r = readPos_.load(std::memory_order_relaxed);
        const uint32_t w = writePos_.load(std::memory_order_acquire);
        const uint32_t avail = w - r;
        const uint32_t toRead = (numFrames < avail) ? numFrames : avail;
        if (toRead == 0) return 0;

        const uint32_t startIdx = r & mask_;
        const uint32_t firstPart = std::min(toRead, capacity_ - startIdx);
        std::memcpy(stereoOut, &buffer_[static_cast<size_t>(startIdx) * 2], static_cast<size_t>(firstPart) * 2 * sizeof(float));

        const uint32_t secondPart = toRead - firstPart;
        if (secondPart > 0) {
            std::memcpy(stereoOut + static_cast<size_t>(firstPart) * 2, &buffer_[0], static_cast<size_t>(secondPart) * 2 * sizeof(float));
        }

        readPos_.store(r + toRead, std::memory_order_release);
        return toRead;
    }

    // Consumer: discard oldest frames (used only for extreme overrun recovery)
    uint32_t skip(uint32_t numFrames) noexcept {
        const uint32_t r = readPos_.load(std::memory_order_relaxed);
        const uint32_t w = writePos_.load(std::memory_order_acquire);
        const uint32_t avail = w - r;
        const uint32_t toSkip = (numFrames < avail) ? numFrames : avail;
        readPos_.store(r + toSkip, std::memory_order_release);
        return toSkip;
    }

private:
    uint32_t capacity_ = 0;
    uint32_t mask_ = 0;
    std::vector<float> buffer_;

    alignas(64) std::atomic<uint32_t> writePos_{ 0 };
    alignas(64) std::atomic<uint32_t> readPos_{ 0 };
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace winaudio::audio
