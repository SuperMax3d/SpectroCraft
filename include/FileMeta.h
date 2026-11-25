// SpectroCraft - unified per-file metadata bundle.
//
// Every file carries one metadata bundle covering markers, saved spectral
// selections, channel scale calibration, axis reference calibrations and the
// Frequency Analysis settings (plus transcription in external mode). The
// bundle lives either embedded inside the WAV as RIFF chunks (CUE/ADTL for
// marker interop, SCTT for transcription, SCMD - a custom chunk holding the
// whole bundle as JSON) or in an external "<file>.meta.json" sidecar.
#pragma once

#include "DrawingItem.h"      // DrawingItem
#include "ItemGroups.h"       // ItemGroupList (group names, folded state)
#include "AxisScale.h"        // sc::axes::Entry (the Scale form)
#include "MarkerManager.h"     // Marker
#include "SavedSelections.h"   // SavedSpectralSelection
#include "ScaleCalibration.h"  // ScaleCalibration
#include "ScttChunk.h"         // ScttData
#include "Tables.h"            // sc::tables::TableData

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
    int thirdOctOrder = 40;   // dB-SPL bandpass order: 1,2,3,4,5,10,20,30 or 40
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

/** One video track attached to an audio document: a movie file shown in the
    Video window's track list, with its time offset against the document and
    the per-video picture adjustments (zoom/pan/rotation/contrast/brightness).
    The track's first frame is displayed at document time `offsetSecs`. */
struct VideoTrackData
{
    juce::File file;            // the video file (absolute path)
    /** The video's path relative to the audio file's folder, "/"-separated
        (written as "relFile"). Lets a file recorded on another machine - the
        mobile app, or a folder moved or copied - find its video next to the
        audio: see resolveVideoTracks(). Empty when the video is elsewhere. */
    juce::String relPath;
    double offsetSecs = 0.0;    // document time of the track's first frame

    // Per-video picture adjustments (the Video window restores them when the
    // track becomes the displayed one).
    float zoom = 1.0f;
    float panX = 0.0f, panY = 0.0f;
    int rotation = 0;           // extra clockwise turns in degrees (x90)
    double contrast = 1.0;      // multiplier around mid-grey
    double brightness = 0.0;    // additive -1..+1

    /** Play this track's own audio instead of the document's while it is
        the displayed picture. */
    bool solo = false;

    bool hasDefaultAdjustments() const noexcept
    {
        return zoom == 1.0f && panX == 0.0f && panY == 0.0f && rotation == 0
            && contrast == 1.0 && brightness == 0.0;
    }
};

/** The complete per-file metadata bundle. */
struct MetaBundle
{
    std::vector<Marker> markers;
    std::vector<SavedSpectralSelection> selections;
    std::vector<DrawingItem> drawings;   // spectrogram annotations (Drawing panel)
    /** Names and folded state of the drawing groups and of the stored
        selection groups ("drawingGroups" / "selectionGroups"; written only
        when one has a name or is folded). Read with their items. */
    ItemGroupList drawingGroups, selectionGroups;
    ScaleCalibration scale;
    AltRefState altRef;
    FreqAnalysisState freqAnalysis;
    ScttData transcription;   // only serialised in external mode

    /** Axis entries of the Scale form (time / amplitude / frequency axes).
        Written as the "axes" array; read back from it when present, otherwise
        rebuilt from the legacy "scale" / "altRef" fields so existing files keep
        their calibration and alternative scales (see sc::axes::migrateLegacy). */
    std::vector<sc::axes::Entry> axes;

    // Tables panel: imported CSV tables with per-parameter alignment, plus
    // the display mode (sc::tables::kModeWaveform...kModePanelOnly).
    std::vector<sc::tables::TableData> tables;
    int tablesMode = 0;

    // Video window: attached video tracks with offsets and per-video picture
    // adjustments (see VideoTrackData). Empty = no explicit tracks; the
    // document's auto-detected same-named movie, if any, is then shown as a
    // single implicit track at offset 0.
    std::vector<VideoTrackData> videoTracks;

    /** Set by parseBundle() when the corresponding key was present in the
        document (used to merge bundles from several sources). */
    bool haveMarkers = false, haveSelections = false, haveDrawings = false,
         haveScale = false, haveAltRef = false, haveFreqAnalysis = false,
         haveTranscription = false, haveTables = false, haveVideoTracks = false,
         haveAxes = false, haveSession = false, haveSoundModel = false,
         haveStoredPlots = false;

    /** Session (issue #21): the referenced files with their lanes and
        offsets, as written by sc::session::toVar (kept as JSON here; the
        document resolves the paths against its own folder). */
    juce::var session;

    /** Model panel (issue #40): the file's sound model, as written by
        sc::model::Model::toVar (kept as JSON here, like the session). */
    juce::var soundModel;
    /** Frequency Analysis stored plots (issue #73): the curves the mobile app
        keeps with a recording, as written by its StoredPlots::toVar (an array,
        kept as JSON here). The desktop carries them through its saves. */
    juce::var storedPlots;

    /** True when the bundle holds anything worth persisting. */
    bool hasContent() const
    {
        return !markers.empty() || !selections.empty() || !drawings.empty()
            || scale.isCalibrated() || !altRef.isDefault()
            || !freqAnalysis.isDefault() || !tables.empty()
            || !videoTracks.empty() || !axes.empty() || session.isObject()
            || soundModel.isObject()
            || (storedPlots.isArray() && storedPlots.size() > 0);
    }
};

/** The video's path relative to `audioFolder` ("/" separators), or "" when
    the video is not inside that folder. */
juce::String videoRelativePath (const juce::File& video, const juce::File& audioFolder);

/** Fills every track's relPath from its file (tracks inside `audioFolder`),
    before the bundle is written. */
void fillVideoRelativePaths (std::vector<VideoTrackData>& tracks, const juce::File& audioFolder);

/** After a bundle is read: a track whose absolute file is missing is looked
    up in `audioFolder` at its relPath, then by its bare file name, and points
    there when found. A track with only a relPath gets the resolved path even
    when the movie is missing (the Video window then shows it as missing). */
void resolveVideoTracks (std::vector<VideoTrackData>& tracks, const juce::File& audioFolder);

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
