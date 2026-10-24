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

// Lightweight copy of a curve-snapshot slot's metadata (no series data):
// enough to restore a stored Freq/Amp-vs-Time or Amp-vs-Freq curve after
// undo/redo, where the series is recomputed from the restored audio.
struct CurveSnapshotMeta
{
    double t0 = 0.0, t1 = 0.0;   // analysed time segment of the source file
    int mode = -1;               // axis mode at store time (3..5); -1 => slot empty
    int track = 0;               // 0=Max, 1=Total (at store time)
    int smooth = 0;              // smoothing window in points: 0/3/5 (at store time)
    bool shown = false;
    std::vector<int> chans;      // visible channels at store time
};

// Before/after state of one curve-snapshot slot for a patch.
struct CurveSlotChange
{
    bool changed = false;
    CurveSnapshotMeta before, after;
};

struct Patch
{
    int64_t oldStart = 0;   // region replaced in the old buffer [oldStart, oldEnd)
    int64_t oldEnd = 0;
    std::vector<std::vector<float>> oldData;  // per-channel data that was removed (for undo)
    std::vector<std::vector<float>> newData;  // per-channel data that was inserted (for redo)
    int64_t lengthDelta = 0;                  // newTotal - oldTotal
    // Channel-count change carried by this patch (both zero => unchanged).
    // Used by edits that add or remove channels, e.g. "Paste to New
    // Channel(s)" adds the clipboard's channels: newData then has one entry
    // per channel of the NEW count and oldData covers only the OLD ones.
    int oldChans = 0;
    int newChans = 0;

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
    static constexpr int kSnapshotSlots = 8;
    std::array<SnapshotSlotChange, kSnapshotSlots> snapshotSlots {};
    // Same for the curve snapshots stored in the Freq/Amp analysis modes.
    std::array<CurveSlotChange, kSnapshotSlots> curveSlots {};
};
