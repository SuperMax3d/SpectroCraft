#pragma once
// ============================================================================
// SpectroCraft - thread-safe float ring buffer for recorder plugins
//
// A FIFO of interleaved 32-bit float samples shared between a plugin's
// capture thread (producer) and the host's pull loop (consumer). Reads
// CONSUME: every sample handed out is removed from the queue, so the host
// sees each captured frame exactly once in real time. When the ring fills
// up (host stalled), the oldest samples are dropped to make room.
// ============================================================================

#include <cstddef>
#include <mutex>
#include <vector>

namespace sc {
namespace rec {

class FloatRing
{
public:
    /** @param capacitySamples  total float slots (frames * channels); 0 = disabled
        @param numChannels      interleaved channel count stored per frame */
    void init (size_t capacitySamples, size_t numChannels)
    {
        std::lock_guard<std::mutex> l (m_);
        data_.assign (capacitySamples, 0.0f);
        ch_ = numChannels > 0 ? numChannels : 1;
        head_ = count_ = 0;
    }

    void clear()
    {
        std::lock_guard<std::mutex> l (m_);
        head_ = count_ = 0;
    }

    size_t capacitySamples() const { return data_.size(); }
    size_t channels() const { return ch_; }

    /** Number of complete frames currently queued. */
    long long availableFrames() const
    {
        std::lock_guard<std::mutex> l (m_);
        if (data_.empty()) return 0LL;
        return (long long) (count_ / ch_);
    }

    /** Appends `n` contiguous float samples, dropping the oldest when full. */
    void append (const float* src, size_t n)
    {
        if (src == nullptr || n == 0) return;
        std::lock_guard<std::mutex> l (m_);
        if (data_.empty()) return;
        for (size_t i = 0; i < n; ++i)
        {
            const size_t pos = (head_ + count_) % data_.size();
            if (pos == head_ && count_ == data_.size())   // full: drop oldest
            {
                head_ = (head_ + 1) % data_.size();
                --count_;
            }
            data_[pos] = src[i];
            ++count_;
        }
    }

    /** Copies up to maxFrames frames into `dest` (which must hold
        channels * maxFrames floats; channels >= the ring's channel count),
        consuming them from the FIFO. @return number of frames copied. */
    size_t readFrames (float* dest, int channels, long long maxFrames)
    {
        if (dest == nullptr || channels <= 0 || maxFrames <= 0) return 0;
        std::lock_guard<std::mutex> l (m_);
        if (data_.empty()) return 0;

        size_t frames = count_ / ch_;
        if (frames > (size_t) maxFrames) frames = (size_t) maxFrames;

        for (size_t f = 0; f < frames; ++f)
            for (size_t c = 0; c < ch_ && c < (size_t) channels; ++c)
                dest[(size_t) f * (size_t) channels + c] =
                    data_[(head_ + f * ch_ + c) % data_.size()];

        head_ = (head_ + frames * ch_) % data_.size();
        count_ -= frames * ch_;
        return frames;
    }

private:
    std::vector<float> data_;
    size_t head_ = 0, count_ = 0;   // [head_, head_+count_) mod capacity
    size_t ch_ = 1;
    mutable std::mutex m_;
};

}   // namespace rec
}   // namespace sc
