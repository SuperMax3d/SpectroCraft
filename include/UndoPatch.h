#pragma once
// SpectroCraft - Undo/redo patch record
#include <vector>
#include <cstdint>

struct Patch
{
    int64_t oldStart = 0;   // region replaced in the old buffer [oldStart, oldEnd)
    int64_t oldEnd = 0;
    std::vector<std::vector<float>> oldData;  // per-channel data that was removed (for undo)
    std::vector<std::vector<float>> newData;  // per-channel data that was inserted (for redo)
    int64_t lengthDelta = 0;                  // newTotal - oldTotal
};
