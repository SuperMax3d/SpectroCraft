// SpectroCraft Mobile - loop recording arithmetic.
//
// Loop mode keeps the last `capacity` frames: while filling, frames are
// appended; once full, each new frame overwrites the oldest one in place (the
// document stays the same length and the spectrogram cache stays valid). At
// stop the content is rotated so the file starts with the oldest frame.
// (The desktop's Loop mode overwrites the same way but does not rotate at stop,
// so its file starts wherever the write head was.)
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace scm
{
struct LoopPlan
{
    int64_t capacity = 0;   // frames kept (0 = unlimited)
    int64_t written = 0;    // frames received since the start

    bool unlimited() const noexcept { return capacity <= 0; }
    bool full() const noexcept { return ! unlimited() && written >= capacity; }

    /** Where the next frame goes in the document (frames). */
    int64_t writePos() const noexcept { return unlimited() ? written : written % capacity; }

    /** How many of `n` incoming frames fit before the write position wraps
        (so one block never straddles the end of the window). */
    int64_t chunkBeforeWrap (int64_t n) const noexcept
    {
        if (unlimited()) return n;
        return std::min (n, capacity - writePos());
    }

    /** True while the next write appends (the window is still filling). */
    bool appending() const noexcept { return unlimited() || written < capacity; }

    /** Frames to rotate left at stop so the file starts with the oldest frame
        (0 when the window never wrapped). */
    int64_t rotationAtStop() const noexcept { return full() ? written % capacity : 0; }
};

/** Rotates every channel left by `k` frames (std::rotate). */
inline void rotateChannels (std::vector<std::vector<float>>& chans, int64_t k)
{
    for (auto& c : chans)
    {
        if (c.empty()) continue;
        const auto s = (int64_t) c.size();
        const auto r = ((k % s) + s) % s;
        std::rotate (c.begin(), c.begin() + r, c.end());
    }
}
} // namespace scm
