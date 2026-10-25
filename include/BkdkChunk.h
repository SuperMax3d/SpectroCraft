// SpectroCraft — "bkdk" RIFF chunk: per-channel calibration metadata.
//
// A custom top-level WAV chunk "bkdk" (written by some legacy measurement
// software) holds NUL-separated ASCII strings laid out as:
//   Version \0 Date \0 [ 11 fields per channel group ] Foot \0 (+ padding)
// The relevant field is #8 of each channel group ("Calibration"): a gain that
// converts raw samples to pascals (sample * Calibration = Pa). Writers differ
// in how many channel groups they emit; when only ONE group is defined it
// applies to every channel.

#pragma once

#include <JuceHeader.h>
#include <string>
#include <vector>
#include <array>
#include <cmath>

namespace sc
{

/** Parsed contents of a "bkdk" chunk (unknown fields kept verbatim). */
struct BkdkData
{
    juce::String version, date, foot;
    std::vector<std::array<juce::String, 11>> channels;   // one entry per group

    int numGroups() const { return static_cast<int> (channels.size()); }

    /** Samples->Pascal gain for channel `ch`: field #8 of its group.
        A single defined group applies to every channel; a missing group or
        unparseable value falls back to 1.0 (no scaling). */
    double calibrationFor (int ch) const;
};

/** Parses the raw payload of a "bkdk" chunk: NUL-separated ASCII strings,
    Version + Date first, then as many complete 11-field channel groups as
    fit, then Foot (trailing padding zeros are ignored). Empty fields stay in
    place — the layout is positional. Returns false when the payload holds
    fewer than two non-empty tokens (out is left untouched). */
bool parseBkdkPayload (const std::string& payload, BkdkData& out);

} // namespace sc
