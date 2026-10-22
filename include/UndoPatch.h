#pragma once
// SpectroCraft - Undo/redo patch record
#include <JuceHeader.h>   // juce::String (action name shown in the History tab)
#include "MarkerManager.h"     // Marker (side state for time-range deletions)
#include "SavedSelections.h"   // SavedSpectralSelection (same)
#include <vector>
#include <array>
#include <cstdint>

// Lightweight copy of an FFT snapshot slot's metadata (no curve data): enough
// to restore a stored frequency analysis after undo/redo, where the curve is
// recomputed from the restored audio.
struct SnapshotMeta
{
    double t0 = 0.0, t1 = 0.0;   // analysed time segment of the source file
    int n = 0;                   // FFT size (n == 0 => slot empty)
    bool shown = false;          // overlay visibility at capture time
    std::vector<int> chans;      // visible channels at store time
};

// Before/after state of one snapshot slot for a patch. `changed` is false when
// the edit did not touch this slot (both metas are then meaningless).
struct SnapshotSlotChange
{
    bool changed = false;
    SnapshotMeta before, after;
};

struct Patch
{
    int64_t oldStart = 0;   // region replaced in the old buffer [oldStart, oldEnd)
    int64_t oldEnd = 0;
    std::vector<std::vector<float>> oldData;  // per-channel data that was removed (for undo)
    std::vector<std::vector<float>> newData;  // per-channel data that was inserted (for redo)
    int64_t lengthDelta = 0;                  // newTotal - oldTotal

    // Human-readable action name shown in the History tab, e.g. "Deleted samples".
    juce::String description;

    // Non-audio state affected by the edit. Deleting a time range also shifts
    // / trims / drops markers, stored frequency analyses (FFT snapshot slots)
    // and named spectral selections of that file; their before/after states
    // are recorded here so undo/redo restores them together with the audio.
    // hasSideState is false for edits that only touch audio data.
    bool hasSideState = false;
    std::vector<Marker> oldMarkers, newMarkers;
    std::vector<SavedSpectralSelection> oldSavedSel, newSavedSel;
    static constexpr int kSnapshotSlots = 10;
    std::array<SnapshotSlotChange, kSnapshotSlots> snapshotSlots {};
};
