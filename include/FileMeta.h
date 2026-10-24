// SpectroCraft - unified per-file metadata bundle.
//
// Every file carries one metadata bundle covering markers, saved spectral
// selections, channel scale calibration, axis reference calibrations and the
// Frequency Analysis settings (plus transcription in external mode). The
// bundle lives either embedded inside the WAV as RIFF chunks (CUE/ADTL for
// marker interop, SCTT for transcription, SCMD - a custom chunk holding the
// whole bundle as JSON) or in an external "<file>.meta.json" sidecar.
#pragma once

#include "MarkerManager.h"     // Marker
#include "SavedSelections.h"   // SavedSpectralSelection
#include "ScaleCalibration.h"  // ScaleCalibration
#include "ScttChunk.h"         // ScttData

namespace sc
{

/** Where a file's metadata bundle lives. */
enum class MetaMode
{
    Embedded,   ///< RIFF chunks inside the .wav (WAV files only)
    External    ///< "<file>.meta.json" sidecar next to the audio file
};

/** Frequency Analysis panel settings - per-file copy of the session state. */
struct FreqAnalysisState
{
    // NOTE: the defaults mirror Application::settings (log axis, 2048) so a
    // document without stored metadata does not flip the session's analysis
    // mode when it is activated.
    int axisMode = 1;     // 0..5 (classic FFT / log / 1/3-oct + curve modes)
    int fftSize = 2048;   // local FFT size (classic + Amp vs Freq mode)
    int refTime = 0;      // 0 relative, 1 absolute
    int smooth = 0;       // 0 / 3 / 5 cent window
    int track = 0;        // 0 max component, 1 total level

    bool isDefault() const
    {
        return axisMode == 1 && fftSize == 2048 && refTime == 0
            && smooth == 0 && track == 0;
    }
};

/** One alternative-axis reference point (per-file copy of the session state).
    alt(x) = x * altValue / refMain, anchored at zero. */
struct AltScaleRefData
{
    juce::String name = "Alt";
    bool enabled = true;
    double refMain = 0.0;   // main-axis value at the reference point (s or Hz)
    double altValue = 0.0;  // alternative-axis value at that point
};

/** Axis reference calibrations - per-file copy of the session state. */
struct AltRefState
{
    int freqScaleMode = 0;      // 0 Hz, 1 custom units (ref-click calibrated)
    double freqFactor = 1.0;    // displayed(f) = f * factor

    std::vector<AltScaleRefData> altTimeRefs;   // alternative time scale overlays
    std::vector<AltScaleRefData> altFreqRefs;   // alternative frequency scale overlays

    bool isDefault() const
    {
        return freqScaleMode == 0 && freqFactor == 1.0
            && altTimeRefs.empty() && altFreqRefs.empty();
    }
};

/** The complete per-file metadata bundle. */
struct MetaBundle
{
    std::vector<Marker> markers;
    std::vector<SavedSpectralSelection> selections;
    ScaleCalibration scale;
    AltRefState altRef;
    FreqAnalysisState freqAnalysis;
    ScttData transcription;   // only serialised in external mode

    /** Set by parseBundle() when the corresponding key was present in the
        document (used to merge bundles from several sources). */
    bool haveMarkers = false, haveSelections = false, haveScale = false,
         haveAltRef = false, haveFreqAnalysis = false, haveTranscription = false;

    /** True when the bundle holds anything worth persisting. */
    bool hasContent() const
    {
        return !markers.empty() || !selections.empty() || scale.isCalibrated()
            || !altRef.isDefault() || !freqAnalysis.isDefault();
    }
};

/** The external sidecar path: "<audio file>.meta.json". */
juce::File fileMetaSidecarPath (const juce::File& audioFile);

/** True when the WAV contains any of the managed metadata chunks
    (cue / LIST(adtl) / SCTT / SCMD). Cheap header-only scan. */
bool wavHasMetadataChunks (const juce::File& wav, std::string* errOut = nullptr);

/** Serialises the bundle to JSON. With includeTranscription=false this is
    exactly the payload of the embedded SCMD chunk; with true it is the
    external .meta.json document. */
std::string serialiseBundle (const MetaBundle& b, bool includeTranscription);

/** Parses a bundle document (either form). Missing keys keep the caller's
    current values so partial documents merge instead of clobbering. */
bool parseBundle (const juce::String& json, MetaBundle& out,
                  std::string* errOut = nullptr);

/** Where file-level metadata operations should store the bundle.
    Auto keeps whatever the file currently uses (the default). */
enum class MetaSaveTarget { Auto, Embedded, External };

/** Migrates a WAV file's per-file metadata to `target` WITHOUT opening it in
    the editor: every category is read from wherever it currently lives
    (unified .meta.json > legacy sidecars > embedded RIFF chunks, exactly like
    FileDoc::load), then written in the requested mode and removed from the
    other one. Audio bytes are never touched. target == Auto leaves the file
    alone. Returns false with errOut set on I/O or parse failure. */
bool migrateFileMetadata (const juce::File& wav, MetaSaveTarget target,
                          std::string* errOut = nullptr);

} // namespace sc
