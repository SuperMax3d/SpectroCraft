// SpectroCraft — SCTT RIFF chunk: embedded Whisper transcription segments.
//
// A custom top-level WAV chunk "SCTT" holds UTF-8 JSON:
//   {"version":1,"segments":[{"start":0.5,"end":2.75,"text":"hello"}, ...]}
// (times in seconds). It is written on save alongside the classic DAW
// cue/adtl marker chunks and parsed on load by scanning the RIFF directly —
// it never appears in the Markers panel. The module has no AI dependency, so
// builds without OpenVINO can still read/write existing SCTT data.

#pragma once

#include <string>
#include <vector>

#include "MarkerManager.h"   // Marker (for the cue/adtl payload builders)

namespace juce { class File; }

// (Global scope, like the rest of the non-AI project types.)

struct ScttSegment {
    double startSec = 0.0;
    double endSec = 0.0;
    std::string text;   // UTF-8
};

struct ScttData {
    int version = 1;
    std::vector<ScttSegment> segments;

    bool empty() const { return segments.empty(); }
};

// Serialize to the SCTT JSON schema (compact, no whitespace).
std::string scttToJson(const ScttData& d);

// Parse SCTT JSON. Returns false on malformed input (out is left untouched).
bool parseScttJson(const std::string& json, ScttData& out, std::string* errOut = nullptr);

// Read the raw payload of one top-level RIFF chunk (e.g. "SCMD"). For LIST
// chunks set adtlList to only match sub-lists of type "adtl".
bool readRiffChunkPayload(const juce::File& wavFile, const char fourcc[4],
                          std::string& out, bool adtlList = false);

// Read the "SCTT" chunk from a WAV file. Returns false when absent or unreadable.
bool readScttChunk(const juce::File& wavFile, ScttData& out, std::string* errOut = nullptr);

// Replace (or append) the "SCTT" chunk of an existing WAV file with `json`.
// Surgical RIFF rewrite: every other chunk is stream-copied byte-for-byte to a
// temp file (constant memory), the old SCTT chunk is dropped, and the new one
// is appended at the end. The original file is replaced atomically via rename.
// Returns false on I/O errors or when the file is not RIFF/WAVE.
bool replaceScttChunkInWav(const juce::File& wavFile, const std::string& json,
                           std::string* errOut = nullptr);

/** One managed chunk in a surgical multi-chunk RIFF rewrite. */
struct RiffPatchEntry {
    char fourcc [4] = {'\0', '\0', '\0', '\0'};   // e.g. "cue ", "SCTT", "SCMD" or "LIST"
    bool adtlList = false;      // for LIST chunks: only manage sub-lists of type "adtl"
    std::string payload;        // new payload (without the 8-byte chunk header)
    bool present = true;        // false => remove this chunk if it exists
};

/** Builds the payload of a "cue " chunk: u32 count + one 24-byte legacy DAW
    entry per marker. Returns an empty string when there are no markers (the
    caller should then omit the chunk entirely). */
std::string buildCueChunkPayload(const std::vector<Marker>& markers,
                                 double sampleRate, int64_t totalSamples);

/** Builds the payload of a LIST/"adtl" chunk: "adtl" + one ltxt entry per
    marker, then labl titles and note comments (legacy DAW layout). Returns an
    empty string when there are no markers. */
std::string buildAdtlListPayload(const std::vector<Marker>& markers,
                                 double sampleRate, int64_t totalSamples);

/** Surgical multi-chunk RIFF rewrite: for every entry in `entries` the matching
    top-level chunk is dropped and (when present && payload non-empty) a fresh
    copy is appended at the end. Every other chunk - including LIST chunks of
    other types such as INFO - is stream-copied byte-for-byte to a temp file
    (constant memory). The original file is replaced atomically via rename.
    Returns false on I/O errors or when the file is not RIFF/WAVE. */
bool patchRiffChunks(const juce::File& wavFile,
                     const std::vector<RiffPatchEntry>& entries,
                     std::string* errOut = nullptr);


