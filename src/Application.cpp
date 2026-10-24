/**
 * SpectroCraft - Application Implementation
 */

#include "Application.h"
#include "SpectralMaskProcessor.h"
#include "PythonScripts.h"
#include "Resampler.h"
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    #include "NoiseSuppression.h"
    #include "AI/ModelManager.h"
    #include "AI/OvRuntime.h"
    #include "AI/WhisperPipeline.h"
#endif
#include <algorithm>
#include <thread>

Application::Application (const juce::File& recentStorePath)
{
    emptyEngine.setUndoManager (&emptyUndo);
    emptyEngine.setMarkersForSave (&emptyMarkers.markers()); // embedded as RIFF cue/adtl in WAV saves

    spectralEngine.setColourMap (colourMap);

    // Real-time output: the player streams from the active engine's transport
    // into the default output device, so play/pause/loop actually produce sound.
    sourcePlayer.setSource (&emptyEngine);
    audioDevice = std::make_unique<juce::AudioDeviceManager>();
    audioDevice->addAudioCallback (&sourcePlayer);
    audioDevice->initialiseWithDefaultDevices (0, 2); // no input, stereo output

    // Recent-files store: persisted in the user's app-data folder so the File >
    // Open Recent list survives restarts. Saving is explicit (no auto-save timer)
    // so every change hits disk deterministically.
    juce::PropertiesFile::Options opts;
    opts.applicationName = "SpectroCraft";
    opts.filenameSuffix = ".recent";
    opts.millisecondsBeforeSaving = -1;   // only save() writes to disk
    juce::File storePath;
    if (recentStorePath != juce::File())
        storePath = recentStorePath;
#if SC_TESTS_EXE
    else
    {
        // Tests must never read or write the user's real settings file: a GUI
        // session that left non-default values behind would otherwise change
        // test outcomes. Use a fresh per-run temp store instead.
        static juce::File isolatedStore = [] ()
        {
            auto f = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("SpectroCraftTests.recent");
            if (f.existsAsFile()) f.deleteFile();
            return f;
        } ();
        storePath = isolatedStore;
    }
#else
    else
        storePath = opts.getDefaultFile();
#endif
    recentStore = std::make_unique<juce::PropertiesFile> (storePath, opts);

    for (int i = 0; i < kMaxRecentFiles; ++i)
    {
        const juce::String p = recentStore->getValue ("recent_" + juce::String (i + 1));
        if (p.isEmpty()) break;
        const juce::File f (p);
        if (f.existsAsFile()) recentFiles.push_back (f);   // skip stale entries
    }

    // Session settings: restore the persisted values (defaults when absent).
    loadSettings ();

    // AI feature preferences (last model/language, auto-download toggle).
    aiAutoDownload        = recentStore->getIntValue ("ai_auto_download", 1) != 0;
    lastNsModelId         = recentStore->getValue ("ai_ns_model", lastNsModelId).toStdString ();
    lastTranscribeModelId = recentStore->getValue ("ai_transcribe_model", lastTranscribeModelId).toStdString ();
    lastTranscribeLanguage= recentStore->getValue ("ai_transcribe_lang", lastTranscribeLanguage).toStdString ();
    lastTranscribeTranslate = recentStore->getIntValue ("ai_transcribe_translate", 0) != 0;
    lastTranscribeFast      = recentStore->getIntValue ("ai_transcribe_fast", 0) != 0;
    lastTranscribeChannel   = recentStore->getIntValue ("ai_transcribe_channel", 1);
    aiDevicePref            = recentStore->getValue ("ai_device", aiDevicePref).toStdString ();
}

// Round v up to the next power of two, then clamp into [lo, hi].
static int roundUpPow2Clamped (int v, int lo, int hi)
{
    if (v < 1) v = 1;
    int p = 1;
    while (p < v && p <= hi / 2) p <<= 1;
    return std::clamp (p, lo, hi);
}

void Application::loadSettings ()
{
    settings.fftSize       = roundUpPow2Clamped ((int) recentStore->getIntValue ("fft_size", settings.fftSize), 512, 65536);
    settings.logFrequency  = recentStore->getIntValue ("log_freq", settings.logFrequency ? 1 : 0) != 0;
    settings.snapToZeroCrossings = recentStore->getIntValue ("snap_zero", settings.snapToZeroCrossings ? 1 : 0) != 0;
    settings.fftAxisMode   = std::clamp ((int) recentStore->getIntValue ("fft_axis_mode", settings.fftAxisMode), 0, 5);
    settings.fftLocalN     = roundUpPow2Clamped ((int) recentStore->getIntValue ("fft_local_n", settings.fftLocalN), 1024, 65536);
    settings.fftRefTime    = std::clamp ((int) recentStore->getIntValue ("fft_ref_time", settings.fftRefTime), 0, 1);
    {   // smoothing window is one of None / 3 / 5 / 7 / 9 / 11 frames
        const int sm = recentStore->getIntValue ("fft_smooth", settings.fftSmooth);
        settings.fftSmooth = (sm == 3 || sm == 5 || sm == 7 || sm == 9 || sm == 11)
                             ? sm : ((sm >= 1 && sm <= 4) ? 3 : 0);
    }
    settings.fftTrack      = std::clamp ((int) recentStore->getIntValue ("fft_track", settings.fftTrack), 0, 2);
}

void Application::saveSettings ()
{
    recentStore->setValue ("fft_size", settings.fftSize);
    recentStore->setValue ("log_freq", settings.logFrequency ? 1 : 0);
    recentStore->setValue ("snap_zero", settings.snapToZeroCrossings ? 1 : 0);
    recentStore->setValue ("fft_axis_mode", settings.fftAxisMode);
    recentStore->setValue ("fft_local_n", settings.fftLocalN);
    recentStore->setValue ("fft_ref_time", settings.fftRefTime);
    recentStore->setValue ("fft_smooth", settings.fftSmooth);
    recentStore->setValue ("fft_track", settings.fftTrack);
    recentStore->save ();
}

void Application::saveAiSettings ()
{
    recentStore->setValue ("ai_auto_download", aiAutoDownload ? 1 : 0);
    recentStore->setValue ("ai_ns_model", juce::String (lastNsModelId));
    recentStore->setValue ("ai_transcribe_model", juce::String (lastTranscribeModelId));
    recentStore->setValue ("ai_transcribe_lang", juce::String (lastTranscribeLanguage));
    recentStore->setValue ("ai_transcribe_translate", lastTranscribeTranslate ? 1 : 0);
    recentStore->setValue ("ai_transcribe_fast", lastTranscribeFast ? 1 : 0);
    recentStore->setValue ("ai_transcribe_channel", lastTranscribeChannel);
    recentStore->setValue ("ai_device", juce::String (aiDevicePref));
    recentStore->save ();
}

Application::~Application() {}

FileDoc* Application::doc() const
{
    return (activeDocIndex >= 0 && activeDocIndex < (int) docs.size())
           ? docs[(size_t) activeDocIndex].get() : nullptr;
}

AudioEngine& Application::audioEngine() { return doc() ? doc()->engine : emptyEngine; }
MarkerManager& Application::markers()   { return doc() ? doc()->markers : emptyMarkers; }
::UndoManager& Application::undo()      { return doc() ? doc()->undo : emptyUndo; }
const ScaleCalibration& Application::scale() const
{ return doc() ? doc()->scale : emptyScale; }

bool Application::isDocLoaded (const FileDoc* d) const
{
    for (auto& p : docs)
        if (p.get() == d) return true;
    return false;
}

bool Application::openFile (const juce::File& file)
{
    // Already loaded? Just make it active again.
    for (int i = 0; i < (int) docs.size(); ++i)
        if (docs[(size_t) i]->file == file)
        { activate (i); addRecentFile (file); return true; }

    auto d = std::make_unique<FileDoc>();
    if (!d->load (file))
        return false;

    inheritSessionMeta (*d);   // fresh files keep the user's current display state
    FileDoc* raw = d.get();
    docs.push_back (std::move (d));
    activate ((int) docs.size() - 1);
    addRecentFile (file);
    return true;
}

void Application::loadFileAsync (const juce::File& file,
                                 std::function<void(float)> onProgress,
                                 std::function<void(bool)> onDone,
                                 const std::atomic<bool>* cancel)
{
    // The document is built entirely on the worker thread (pure I/O + DSP);
    // it only crosses to the message thread once fully loaded.
    auto* doc = new FileDoc();
    std::thread ([doc, file, onProgress, onDone, cancel, this] {
        const bool ok = doc->load (file, onProgress, cancel);

        juce::MessageManager::callAsync ([this, doc, file, ok, onDone] {
            if (ok)
            {
                inheritSessionMeta (*doc);   // message thread: session state is valid here
                docs.push_back (std::unique_ptr<FileDoc> (doc));
                activate ((int) docs.size() - 1);
                addRecentFile (file);
            }
            else
                delete doc;

            if (onDone) onDone (ok);
        });
    }).detach();
}

// Write the current list to the store: one property per slot, clearing any
// stale tail slots. Called after every mutation.
static void persistRecentFiles (Application& app)
{
    for (size_t i = 0; i < app.recentFiles.size(); ++i)
        app.recentStore->setValue ("recent_" + juce::String ((int) i + 1),
                                      app.recentFiles[i].getFullPathName());
    for (int i = (int) app.recentFiles.size() + 1; i <= Application::kMaxRecentFiles; ++i)
        app.recentStore->removeValue ("recent_" + juce::String (i));
    app.recentStore->save ();
}

void Application::addRecentFile (const juce::File& file)
{
    // Move to the front (dedupe by file identity), then cap at kMaxRecentFiles.
    recentFiles.erase (std::remove_if (recentFiles.begin(), recentFiles.end(),
                                      [&file] (const juce::File& f) { return f == file; }),
                       recentFiles.end());
    recentFiles.insert (recentFiles.begin(), file);
    while ((int) recentFiles.size() > kMaxRecentFiles)
        recentFiles.pop_back ();

    persistRecentFiles (*this);
}

void Application::removeRecentFile (const juce::File& file)
{
    auto it = std::find_if (recentFiles.begin(), recentFiles.end(),
                            [&file] (const juce::File& f) { return f == file; });
    if (it != recentFiles.end())
        recentFiles.erase (it);
    persistRecentFiles (*this);
}

void Application::clearRecentFiles()
{
    recentFiles.clear ();
    persistRecentFiles (*this);
}

void Application::activate (int index)
{
    if (index < 0 || index >= (int) docs.size()) return;
    endClonePick ();   // a picking session belongs to the doc being left
    cancelSelGesture ();
    FileDoc* d = docs[(size_t) index].get();

    // Save the current UI state back into the doc being left and stop its
    // playback. (The bounds check matters when activate() is called right
    // after a close: the old index may already be out of range.)
    if (activeDocIndex >= 0 && activeDocIndex != index
        && activeDocIndex < (int) docs.size())
    {
        syncActiveDoc ();
        FileDoc& prev = *docs[(size_t) activeDocIndex];
        if (prev.engine.isPlaying())
            prev.engine.stop();
    }

    activeDocIndex = index;
    selection = d->selection;
    playhead = d->playhead;
    visibleChannels = d->visibleChannels;

    // Pull this file's display state into the session (per-file metadata):
    // Frequency Analysis settings + axis reference calibrations.
    settings.fftAxisMode  = std::clamp (d->freqAnalysis.axisMode, 0, 5);
    settings.fftLocalN    = std::clamp (d->freqAnalysis.fftSize, 1024, 65536);
    settings.fftRefTime   = std::clamp (d->freqAnalysis.refTime, 0, 1);
    settings.fftSmooth    = d->freqAnalysis.smooth;
    settings.fftTrack     = std::clamp (d->freqAnalysis.track, 0, 2);
    specFreqScaleMode     = d->altRef.freqScaleMode == 1 ? 1 : 0;
    specFreqFactor        = d->altRef.freqFactor > 1e-30 ? d->altRef.freqFactor : 1.0;
    altTimeRefs.clear();
    for (const auto& r : d->altRef.altTimeRefs)
    {
        AltScaleRef a;   a.name = r.name; a.enabled = r.enabled;
        a.refMain = r.refMain; a.altValue = r.altValue;
        altTimeRefs.push_back (a);
    }
    altFreqRefs.clear();
    for (const auto& r : d->altRef.altFreqRefs)
    {
        AltScaleRef a;   a.name = r.name; a.enabled = r.enabled;
        a.refMain = r.refMain; a.altValue = r.altValue;
        altFreqRefs.push_back (a);
    }

    viewStart = 0.0;
    viewEnd = std::max (d->engine.duration(), 1e-6);

    sourcePlayer.setSource (&d->engine);
    d->engine.setPosition (d->playhead);
    spectralEngine.clearCache();   // tiles are keyed by sample position only

    if (onActiveDocChanged) onActiveDocChanged ();
}

void Application::cycleActiveDoc (bool forward)
{
    const int n = (int) docs.size();
    if (n < 2) return;
    const int i = (activeDocIndex >= 0 && activeDocIndex < n) ? activeDocIndex : 0;
    activate ((i + (forward ? 1 : -1) + n) % n);
}

void Application::syncActiveDoc()
{
    FileDoc* d = doc();
    if (!d) return;
    d->selection = selection;
    d->playhead = playhead;
    d->visibleChannels = visibleChannels;
}

void Application::inheritSessionMeta (FileDoc& d)
{
    // Only docs that carried no stored state inherit the session's; a file
    // with its own saved settings keeps them (activate() restores them).
    if (!d.haveStoredFreqAnalysis)
    {
        sc::FreqAnalysisState fa;
        fa.axisMode = std::clamp (settings.fftAxisMode, 0, 5);
        fa.fftSize  = std::clamp (settings.fftLocalN, 1024, 65536);
        fa.refTime  = std::clamp (settings.fftRefTime, 0, 1);
        const int sm = settings.fftSmooth;
        fa.smooth   = (sm == 3 || sm == 5 || sm == 7 || sm == 9 || sm == 11)
                      ? sm : ((sm >= 1 && sm <= 4) ? 3 : 0);
        fa.track    = std::clamp (settings.fftTrack, 0, 2);
        d.freqAnalysis = fa;
    }
    if (!d.haveStoredAltRef)
    {
        sc::AltRefState ar;
        ar.freqScaleMode  = specFreqScaleMode == 1 ? 1 : 0;
        ar.freqFactor     = specFreqFactor > 1e-30 ? specFreqFactor : 1.0;
        for (const auto& r : altTimeRefs)
        {
            sc::AltScaleRefData a;   a.name = r.name; a.enabled = r.enabled;
            a.refMain = r.refMain; a.altValue = r.altValue;
            ar.altTimeRefs.push_back (a);
        }
        for (const auto& r : altFreqRefs)
        {
            sc::AltScaleRefData a;   a.name = r.name; a.enabled = r.enabled;
            a.refMain = r.refMain; a.altValue = r.altValue;
            ar.altFreqRefs.push_back (a);
        }
        d.altRef = ar;
    }
}

void Application::pushActiveDocMetaTo (FileDoc& d)
{
    sc::FreqAnalysisState fa;
    fa.axisMode = std::clamp (settings.fftAxisMode, 0, 5);
    fa.fftSize  = std::clamp (settings.fftLocalN, 1024, 65536);
    fa.refTime  = std::clamp (settings.fftRefTime, 0, 1);
    const int sm = settings.fftSmooth;
    fa.smooth   = (sm == 3 || sm == 5 || sm == 7 || sm == 9 || sm == 11)
                  ? sm : ((sm >= 1 && sm <= 4) ? 3 : 0);
    fa.track    = std::clamp (settings.fftTrack, 0, 2);
    d.freqAnalysis = fa;

    sc::AltRefState ar;
    ar.freqScaleMode  = specFreqScaleMode == 1 ? 1 : 0;
    ar.freqFactor     = specFreqFactor > 1e-30 ? specFreqFactor : 1.0;
    for (const auto& r : altTimeRefs)
    {
        sc::AltScaleRefData a;   a.name = r.name; a.enabled = r.enabled;
        a.refMain = r.refMain; a.altValue = r.altValue;
        ar.altTimeRefs.push_back (a);
    }
    for (const auto& r : altFreqRefs)
    {
        sc::AltScaleRefData a;   a.name = r.name; a.enabled = r.enabled;
        a.refMain = r.refMain; a.altValue = r.altValue;
        ar.altFreqRefs.push_back (a);
    }
    d.altRef = ar;

    // A loaded file's stored state just changed: it no longer matches disk.
    // Brand-new documents have nothing on disk yet, so they stay clean until
    // something real is added to them.
    if (!d.file.getFullPathName().isEmpty())
        d.metaDirty = true;
}

void Application::pushActiveDocMeta()
{
    FileDoc* d = doc();
    if (d != nullptr)
        pushActiveDocMetaTo (*d);
}

bool Application::setActiveDocMetaMode (sc::MetaMode mode)
{
    FileDoc* d = doc();
    if (d == nullptr || !d->engine.isFileLoaded () || mode == d->metaMode)
        return true;   // nothing to do

    auto makeEntry = [] (const char* id, bool adtlList,
                         std::string payload, bool present) -> RiffPatchEntry
    {
        RiffPatchEntry e;
        std::memcpy (e.fourcc, id, 4);
        e.adtlList = adtlList;
        e.payload  = std::move (payload);
        e.present  = present;
        return e;
    };

    const bool isWav = d->file.getFileExtension().equalsIgnoreCase (".wav");
    if (!isWav && mode == sc::MetaMode::Embedded)
        return false;   // non-WAV files cannot carry RIFF chunks

    auto deleteLegacySidecars = [&] ()
    {
        juce::File m (d->file.getFullPathName() + ".markers.json");
        if (m.existsAsFile()) m.deleteFile();
        const juce::File s = savedSelectionsSidecarPath (d->file);
        if (s.existsAsFile()) s.deleteFile();
    };

    if (mode == sc::MetaMode::External)
    {
        // 1. Write the unified sidecar - it becomes the single source of truth.
        if (!d->writeMetaSidecar (d->file))
            return false;
        // 2. Remove every managed chunk from the WAV (audio bytes preserved).
        if (isWav)
        {
            std::vector<RiffPatchEntry> entries;
            entries.push_back (makeEntry ("cue ", false, {}, false));
            entries.push_back (makeEntry ("LIST", true,  {}, false));
            entries.push_back (makeEntry ("SCTT", false, {}, false));
            entries.push_back (makeEntry ("SCMD", false, {}, false));
            patchRiffChunks (d->file, entries);   // best effort: sidecar is safe
        }
        deleteLegacySidecars ();
    }
    else
    {
        // 1. Insert/replace every managed chunk inside the WAV.
        const std::vector<Marker>& ms = d->markers.markers();
        std::vector<RiffPatchEntry> entries;
        entries.push_back (makeEntry ("cue ", false,
            buildCueChunkPayload (ms, d->engine.sampleRate(), d->engine.numSamples()), true));
        entries.push_back (makeEntry ("LIST", true,
            buildAdtlListPayload (ms, d->engine.sampleRate(), d->engine.numSamples()), true));
        if (!d->sctt.empty ())
            entries.push_back (makeEntry ("SCTT", false, scttToJson (d->sctt), true));
        const std::string scmd = d->buildScmdJson ();
        if (!scmd.empty ())
            entries.push_back (makeEntry ("SCMD", false, scmd, true));

        if (!patchRiffChunks (d->file, entries))
            return false;   // WAV untouched; the sidecar (if any) is still valid

        // 2. The metadata now lives inside the WAV: drop every JSON copy.
        const juce::File unified = sc::fileMetaSidecarPath (d->file);
        if (unified.existsAsFile()) unified.deleteFile();
        deleteLegacySidecars ();
    }

    d->metaMode = mode;
    d->metaDirty = false;   // the disk now holds exactly this state
    return true;
}

bool Application::saveActiveDocMeta (std::string* errOut)
{
    FileDoc* d = doc();
    if (d == nullptr || !d->engine.isFileLoaded ())
        return false;
    if (d->file.getFullPathName().isEmpty())
    {
        if (errOut != nullptr)
            *errOut = "This document has no file path yet - use Save As first.";
        return false;
    }

    const bool isWav = d->file.getFileExtension().equalsIgnoreCase (".wav");
    if (!isWav && d->metaMode == sc::MetaMode::Embedded)
    {
        if (errOut != nullptr)
            *errOut = "Only WAV files can carry embedded metadata chunks.";
        return false;
    }

    auto makeEntry = [] (const char* id, bool adtlList,
                         std::string payload, bool present) -> RiffPatchEntry
    {
        RiffPatchEntry e;
        std::memcpy (e.fourcc, id, 4);
        e.adtlList = adtlList;
        e.payload  = std::move (payload);
        e.present  = present;
        return e;
    };

    auto deleteLegacySidecars = [&] ()
    {
        juce::File m (d->file.getFullPathName() + ".markers.json");
        if (m.existsAsFile()) m.deleteFile();
        const juce::File s = savedSelectionsSidecarPath (d->file);
        if (s.existsAsFile()) s.deleteFile();
    };

    if (d->metaMode == sc::MetaMode::External)
    {
        // The sidecar becomes the single source of truth.
        if (!d->writeMetaSidecar (d->file))
        {
            if (errOut != nullptr) *errOut = "Could not write the .meta.json sidecar.";
            return false;
        }
        // Drop stale embedded chunks so no second copy lingers in the WAV.
        if (isWav && sc::wavHasMetadataChunks (d->file))
        {
            std::vector<RiffPatchEntry> entries;
            entries.push_back (makeEntry ("cue ", false, {}, false));
            entries.push_back (makeEntry ("LIST", true,  {}, false));
            entries.push_back (makeEntry ("SCTT", false, {}, false));
            entries.push_back (makeEntry ("SCMD", false, {}, false));
            patchRiffChunks (d->file, entries);   // best effort: sidecar is safe
        }
        deleteLegacySidecars ();
    }
    else
    {
        // Surgical RIFF patch: only the managed chunks are rewritten, every
        // audio byte stays exactly where it was.
        const std::vector<Marker>& ms = d->markers.markers();
        std::vector<RiffPatchEntry> entries;
        entries.push_back (makeEntry ("cue ", false,
            buildCueChunkPayload (ms, d->engine.sampleRate(), d->engine.numSamples()), true));
        entries.push_back (makeEntry ("LIST", true,
            buildAdtlListPayload (ms, d->engine.sampleRate(), d->engine.numSamples()), true));
        if (!d->sctt.empty ())
            entries.push_back (makeEntry ("SCTT", false, scttToJson (d->sctt), true));
        const std::string scmd = d->buildScmdJson ();   // empty => chunk removed
        entries.push_back (makeEntry ("SCMD", false, scmd, !scmd.empty ()));

        if (!patchRiffChunks (d->file, entries, errOut))
            return false;   // WAV untouched

        // The metadata now lives inside the WAV: drop every JSON copy.
        const juce::File unified = sc::fileMetaSidecarPath (d->file);
        if (unified.existsAsFile()) unified.deleteFile();
        deleteLegacySidecars ();
    }

    d->metaDirty = false;   // disk now matches the in-memory state
    return true;
}

bool Application::docIsUnsaved (const FileDoc& d) const
{
    return d.engine.hasUnsavedChanges () || d.metaDirty;
}

std::vector<juce::String> Application::unsavedDocNames() const
{
    std::vector<juce::String> names;
    for (const auto& p : docs)
        if (docIsUnsaved (*p))
            names.push_back (p->name.isNotEmpty () ? p->name : p->file.getFileName());
    return names;
}

void Application::closeFile()
{
    if (docs.empty()) return;
    const int idx = (activeDocIndex >= 0 && activeDocIndex < (int) docs.size()) ? activeDocIndex : 0;
    closeDoc (idx);
}

void Application::closeDoc (int index)
{
    if (index < 0 || index >= (int) docs.size()) return;
    endClonePick ();   // a picking session belongs to the doc being closed
    cancelSelGesture ();
    FileDoc& d = *docs[(size_t) index];

    const bool wasActive = (index == activeDocIndex);
    if (wasActive) syncActiveDoc ();
    d.engine.stop();
    if (onFileClosed) onFileClosed (d);    // e.g. clear snapshots stored from this file

    // Detach the player BEFORE destroying the engine: AudioSourcePlayer::setSource()
    // calls releaseResources() on the old source, which must still be alive.
    if (wasActive)
        sourcePlayer.setSource (&emptyEngine);

    docs.erase (docs.begin() + index);
    if (!wasActive && activeDocIndex > index)
        --activeDocIndex;   // indices shifted left of the removed doc

    if (!docs.empty())
    {
        if (wasActive)
            activate (std::min (std::max (0, index - 1), (int) docs.size() - 1));
            // fires onActiveDocChanged: views reset to the new file
        else if (onStateChange)
            onStateChange ();   // file list changed; current view state untouched
    }
    else
    {
        activeDocIndex = -1;
        selection = { 0, 0 };
        playhead = 0.0;
        visibleChannels.clear();
        viewStart = 0; viewEnd = 10;
        sourcePlayer.setSource (&emptyEngine);
        if (onActiveDocChanged) onActiveDocChanged ();
    }
}

void Application::selectAll()
{
    // Go through setSelection() so the UI (both views + status bar) is
    // notified of the new range.
    if (audioEngine().isFileLoaded())
        setSelection ({ 0, audioEngine().duration() });
}

void Application::setSelection (juce::Range<double> s, bool snapEdges)
{
    if (snapEdges && settings.snapToZeroCrossings && audioEngine().isFileLoaded())
    {
        // Snap each edge to the nearest zero crossing of the first visible
        // channel within +/- 50 ms. If snapping would invert or collapse the
        // range, keep the raw edges instead.
        const int ch = visibleChannels.empty() ? 0 : visibleChannels.front();
        const double a = audioEngine().snapToZeroCrossing (s.getStart(), 0.05, ch);
        const double b = audioEngine().snapToZeroCrossing (s.getEnd(), 0.05, ch);
        if (a < b)
            s = { a, b };
    }

    const juce::Range<double> clamped = s.getIntersectionWith ({ 0, audioEngine().duration() });
    if (clamped == selection) return;
    selection = clamped;
    syncActiveDoc ();   // keep the active document's copy in step

    // Notify the UI: both views need the new range and the status bar shows
    // the selection start/stop times. Firing on every change keeps the readout
    // live while dragging a selection in either view.
    if (onStateChange)
        onStateChange ();
}

// ------------------------------------------------- spectral selection tools

void Application::setSelectionTool (SpectralSelMode m)
{
    selMode = m;
    // Leaving the spectral tools drops any active spectral selection: the time
    // range is what Time Range mode operates on, and keeping stale shapes
    // around would surprise the next effect.
    if (m == SpectralSelMode::TimeRange && doc() && !doc()->spectralSel.empty ())
        clearSpectralSelection ();
}

void Application::clearSpectralSelection ()
{
    bool changed = false;
    auto* d = doc();
    if (d && !d->spectralSel.empty ())
    {
        d->spectralSel.clear ();
        selection = { 0, 0 };   // the mirrored time range goes with it
        changed = true;
    }
    if (playSpectralSel)
    {
        playSpectralSel = false;
        changed = true;
    }
    pushSpectralMaskToEngine ();
    if (changed && onStateChange)
        onStateChange ();
}

void Application::pushSpectralMaskToEngine ()
{
    auto* d = doc();
    const bool active = playSpectralSel && d && !d->spectralSel.empty ();
    audioEngine ().setSpectralPlaybackMask (
        active ? std::make_shared<SpectralSelection> (d->spectralSel) : nullptr);
}

bool Application::hasSpectralSelection () const
{
    auto* d = doc();
    return d && !d->spectralSel.empty ();
}

void Application::refreshSpectralSelectionUI ()
{
    auto* d = doc ();
    if (!d) return;
    // Mirror the selection's time extent into the shared time selection (the
    // same way the spectral tools' commit path does), re-install the playback
    // mask and let the UI refresh.
    const double dur = audioEngine ().duration ();
    auto ext = d->spectralSel.timeExtent ().getIntersectionWith ({ 0, std::max (1e-9, dur) });
    setSelection (ext.getLength () > 1e-9 ? ext : juce::Range<double> { 0, 0 });
    pushSpectralMaskToEngine ();
    if (onStateChange)
        onStateChange ();
}

std::vector<double> Application::parseMultiplierList (const juce::String& text,
                                                      std::string* errOut)
{
    auto fail = [&] (const juce::String& msg) { if (errOut) *errOut = msg.toStdString (); return std::vector<double> {}; };

    // This JUCE build's fromTokens takes (string, breakChars, quoteChars).
    const auto tokens = juce::StringArray::fromTokens (text.trim (),
                                                       juce::String (" \t\r\n,;"),
                                                       juce::String());
    if (tokens.isEmpty ())
        return fail ("Enter at least one multiplier, e.g. 0.5, 2, 5.");

    std::vector<double> out;
    // No range-for: this JUCE build's StringArray has no begin()/end().
    for (int i = 0; i < tokens.size(); ++i)
    {
        const juce::String t = tokens[i].trim ();
        if (t.isEmpty ()) continue;
        // Strict digit check: getDoubleValue() would silently accept "2x".
        bool okNum = !t.isEmpty ();
        for (int i = 0; i < t.length(); ++i)
        {
            const char16_t c = t[i];
            if (!((c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-'
                  || c == 'e' || c == 'E')) { okNum = false; break; }
        }
        const double v = t.getDoubleValue ();
        if (!okNum || !std::isfinite (v) || !(v > 0.0))
            return fail ("Each multiplier must be a positive number: \"" + t + "\".");
        out.push_back (v);
    }
    if (out.empty ())
        return fail ("Enter at least one multiplier, e.g. 0.5, 2, 5.");
    return out;
}

bool Application::cloneAndMultiplySelection (const std::vector<double>& multipliers,
                                             std::string* errOut)
{
    auto fail = [&] (const juce::String& msg) { if (errOut) *errOut = msg.toStdString (); return false; };

    auto* d = doc ();
    if (!d || d->spectralSel.empty ())
        return fail ("No spectral selection to clone.");
    if (multipliers.empty ())
        return fail ("Enter at least one multiplier, e.g. 0.5, 2, 5.");
    for (double m : multipliers)
        if (!(m > 0.0) || !std::isfinite (m))
            return fail ("Multipliers must be positive numbers.");

    d->spectralSel.addScaledClones (multipliers);
    refreshSpectralSelectionUI ();
    return true;
}

void Application::setSpectralSelectionFrom (const SpectralSelection& sel)
{
    auto* d = doc ();
    if (!d) return;
    d->spectralSel = sel;   // deep copy of the stored shapes
    refreshSpectralSelectionUI ();
}

void Application::ensureTimeRangeVisible (double t0, double t1)
{
    if (!onSetTimeWindow) return;
    const double dur = audioEngine().duration();
    if (!(dur > 0.0)) return;

    auto cl = [&] (double v) { return std::clamp (v, 0.0, dur); };
    t0 = cl (t0); t1 = cl (t1);
    if (t1 < t0) std::swap (t0, t1);

    // Give the caller some breathing room: expand by 10% of the range length
    // on each side, as far as the file allows. Zero-length ranges have no
    // span to take a percentage of and are left alone.
    const double span = t1 - t0;
    if (span > 0.0)
    {
        const double m = 0.1 * span;
        t0 = std::max (0.0, t0 - m);
        t1 = std::min (dur, t1 + m);
    }

    const double vs = viewStart, ve = viewEnd;
    // Already fully inside the current window? Nothing to do.
    if (t0 >= vs - 1e-9 && t1 <= ve + 1e-9) return;

    const double W = std::max (ve - vs, 1e-9);
    const double S = t1 - t0;
    // Target is at least as wide as the view: zoom to it exactly.
    if (S >= W)
    {
        onSetTimeWindow (t0, t1);
        return;
    }

    // Otherwise keep the current span and pan just enough to include the range.
    const double n0 = std::clamp ((t0 < vs) ? t0 : t1 - W,
                                  0.0, std::max (0.0, dur - W));
    onSetTimeWindow (n0, std::min (n0 + W, dur));
}

void Application::startClonePick ()
{
    auto* d = doc ();
    if (!d || d->spectralSel.empty ()) return;
    clonePicking = true;
    haveClonePickRef = false;
    clonePickPts.clear ();
    clonePickShapeStarts.clear ();
    // Every click clones this snapshot - never the clones added by earlier
    // clicks in the same session (see SpectralSelection::appendScaledShapesFrom).
    clonePickBase = d->spectralSel;
    if (onStateChange)
        onStateChange ();   // status bar hint + panel button state
}

void Application::endClonePick ()
{
    if (!clonePicking) return;
    clonePicking = false;
    haveClonePickRef = false;
    clonePickPts.clear ();
    clonePickShapeStarts.clear ();
    clonePickBase.clear ();
    if (onStateChange)
        onStateChange ();
}

bool Application::clonePickClick (double tSecs, double fHz,
                                 double dtTolSecs, double dfTolHz)
{
    auto* d = doc ();
    if (!d || !(fHz > 0.0))
        return false;

    // Clicking near an already-picked point removes it (and the shapes its
    // pick added), so a stray click can be undone without Esc.
    if (dtTolSecs > 0.0 && dfTolHz > 0.0)
    {
        for (size_t i = 0; i < clonePickPts.size (); ++i)
            if (std::abs (clonePickPts[i].first - tSecs) <= dtTolSecs
                && std::abs (clonePickPts[i].second - fHz) <= dfTolHz)
            {
                removeClonePickPoint ((int) i);
                return true;
            }
    }

    if (d->spectralSel.empty ())
        return false;

    clonePickPts.emplace_back (tSecs, fHz);
    int shapeStart = -1;   // the reference point adds no shapes of its own
    if (!haveClonePickRef)
    {
        haveClonePickRef = true;
        clonePickRefF = fHz;
        if (onStateChange) onStateChange ();   // status bar shows the reference
    }
    else
    {
        // Clone the session-start snapshot, not the growing selection.
        shapeStart = d->spectralSel.count ();
        d->spectralSel.appendScaledShapesFrom (clonePickBase, fHz / clonePickRefF);
        refreshSpectralSelectionUI ();
    }
    clonePickShapeStarts.push_back (shapeStart);
    return true;
}

void Application::removeClonePickPoint (int index)
{
    auto* d = doc ();
    if (!d || index < 0 || index >= (int) clonePickPts.size ())
        return;

    // Drop the shapes this pick appended, keeping later points' start indices
    // valid (their blocks sit after this one and shift down by its size).
    const int start = clonePickShapeStarts[(size_t) index];
    const int nShapes = clonePickBase.count ();
    if (start >= 0 && nShapes > 0)
    {
        d->spectralSel.removeShapesAt (start, nShapes);
        for (int& s : clonePickShapeStarts)
            if (s > start) s -= nShapes;
    }

    clonePickPts.erase (clonePickPts.begin () + index);
    clonePickShapeStarts.erase (clonePickShapeStarts.begin () + index);

    // Removing the first point also clears the reference: the next click sets
    // a new one. The clones already committed to the selection stay put.
    if (index == 0 && haveClonePickRef)
        haveClonePickRef = false;

    refreshSpectralSelectionUI ();
}

void Application::stepBrushRadius (bool larger)
{
    brushRadiusHz = larger ? std::min (5000.0, brushRadiusHz * 1.5)
                           : std::max (5.0, brushRadiusHz / 1.5);
}

// ---------------------------------------------------------------------------
// Move/Scale selection gesture (spectral view right-click menu).
// The first click sets a reference point; mouse movement previews the
// transform live (always computed from the session-start snapshot, so moves
// never compound); the second click applies it. Esc or a right-click cancels.
// ---------------------------------------------------------------------------

void Application::startSelGesture (bool scale)
{
    auto* d = doc ();
    if (!d || d->spectralSel.empty ()) return;

    cancelSelGesture ();   // restore + clear any previous gesture state
    selGesturing = true;
    selGestScale = scale;
    haveSelGestRef = false;
    selGestBase = d->spectralSel;
    const auto bb = d->spectralSel.boundingBox();
    // Normalisation spans for the Scale gesture: one full span of mouse travel
    // from the reference doubles that axis. Guards keep degenerate selections
    // (zero-width/height) from dividing by ~0.
    selGestW = std::max (1e-3, bb.getWidth());
    selGestH = std::max (8.0,  bb.getHeight());
    if (onStateChange)
        onStateChange ();   // status bar hint + view repaint
}

bool Application::selGestureClick (double tSecs, double fHz, AxisLock lock)
{
    auto* d = doc ();
    if (!d || !selGesturing) return false;

    if (!haveSelGestRef)
    {
        // First click: establish the reference point.
        haveSelGestRef = true;
        selGestRefT = tSecs;
        selGestRefF = fHz;
        if (onStateChange) onStateChange ();   // draw the round indicator
    }
    else
    {
        // Second click: apply what is under the cursor and end the gesture.
        updateSelGesture (tSecs, fHz, lock);
        selGesturing = false;
        haveSelGestRef = false;
        selGestBase.clear ();
        refreshSpectralSelectionUI ();   // final mask push + UI sync
    }
    return true;
}

void Application::updateSelGesture (double tSecs, double fHz, AxisLock lock)
{
    auto* d = doc ();
    if (!d || !selGesturing || !haveSelGestRef) return;

    SpectralSelection preview;
    if (selGestScale)
    {
        // Factor 1 at the reference point itself; moving one full bbox span
        // away doubles that axis. Clamped so the gesture can never invert or
        // explode the selection.
        double st = 1.0 + (tSecs - selGestRefT) / selGestW;
        double sf = 1.0 + (fHz   - selGestRefF) / selGestH;
        if (lock == AxisLock::X)      sf = 1.0;
        else if (lock == AxisLock::Y) st = 1.0;
        st = std::clamp (st, 0.02, 50.0);
        sf = std::clamp (sf, 0.02, 50.0);
        preview = selGestBase.scaledAboutCopy (selGestRefT, selGestRefF, st, sf);
    }
    else
    {
        double dt = tSecs - selGestRefT;
        double df = fHz   - selGestRefF;
        if (lock == AxisLock::X)      df = 0.0;
        else if (lock == AxisLock::Y) dt = 0.0;
        preview = selGestBase.translatedCopy (dt, df);
    }

    d->spectralSel = std::move (preview);
    refreshSpectralSelectionUI ();
}

bool Application::cancelSelGesture ()
{
    if (!selGesturing) return false;

    auto* d = doc ();
    selGesturing = false;
    haveSelGestRef = false;

    // Restore the session-start selection (the live preview may have moved it).
    if (d)
        d->spectralSel = selGestBase;
    selGestBase.clear ();

    refreshSpectralSelectionUI ();   // re-sync extent/mask/UI even when unchanged
    return true;
}

// Sample range covered by the active spectral selection (its time extent).
static juce::Range<int64_t> spectralSelSamples (Application& app)
{
    AudioEngine& eng = app.audioEngine();
    auto ext = app.doc()->spectralSel.timeExtent ().getIntersectionWith ({ 0, eng.duration () });
    return { (int64_t) (ext.getStart () * eng.sampleRate ()),
             (int64_t) (ext.getEnd () * eng.sampleRate ()) };
}

// Gain applied only to the selected spectral content; everything else passes
// through bit-exact. linearGain == 1.0f is a guaranteed no-op.
static bool maskedGainEdit (Application& app, const juce::String& desc,
                            float linearGain, std::string* errOut)
{
    auto* d = app.doc();
    if (!d || d->spectralSel.empty ()) return false;
    // applyMaskedGain goes through replaceSamples, which does NOT rebuild the
    // waveform mipmaps (unlike deleteSamples/silenceSamples/applyPerSample).
    // Rebuild here so the waveform view shows the edit immediately.
    if (!SpectralMask::applyMaskedGain (app.audioEngine (), spectralSelSamples (app),
                                        d->spectralSel, linearGain, desc,
                                        &app.visibleChannels, errOut))
        return false;
    app.audioEngine ().rebuildMipmaps ();
    return true;
}

// Apply fn (in-place on one channel's masked part) to the selected spectral
// content only: A = masked signal, B = original - A, write back fn(A) + B.
static bool applyMaskedEffect (Application& app, const juce::String& desc,
                               std::function<void (float*, int)> fn,
                               std::string* errOut)
{
    auto* d = app.doc();
    if (!d || d->spectralSel.empty ()) return false;
    AudioEngine& eng = app.audioEngine ();
    const juce::Range<int64_t> range = spectralSelSamples (app);
    if (range.getLength () <= 0) return true;   // nothing to do

    const int len = (int) range.getLength ();
    std::vector<std::vector<float>> A;
    if (!SpectralMask::extractMasked (eng, range, d->spectralSel, A))
    {
        if (errOut) *errOut = "Spectral masking failed";
        return false;
    }

    const int nch = eng.numChannels ();
    std::vector<std::vector<float>> orig ((size_t) nch);
    for (int ch = 0; ch < nch; ++ch)
    {
        orig[(size_t) ch].assign ((size_t) len, 0.0f);
        if (!eng.readSamples (ch, range.getStart (), len, orig[(size_t) ch].data ()))
        {
            if (errOut) *errOut = "Failed to read audio";
            return false;
        }
    }

    // B = original - A on the affected channels; then out = B + fn(A).
    std::vector<std::vector<float>> out (orig);
    for (int ch : app.visibleChannels)
    {
        if (ch < 0 || ch >= nch) continue;
        for (int i = 0; i < len; ++i)
            out[(size_t) ch][(size_t) i] -= A[(size_t) ch][(size_t) i];
        fn (A[(size_t) ch].data (), len);
        for (int i = 0; i < len; ++i)
            out[(size_t) ch][(size_t) i] += A[(size_t) ch][(size_t) i];
    }

    // replaceSamples does not rebuild the waveform mipmaps (see maskedGainEdit);
    // do it here so the views update immediately after the edit.
    if (!eng.replaceSamples (range, out, desc, errOut))
        return false;
    eng.rebuildMipmaps ();
    return true;
}

bool Application::isChannelVisible (int ch) const
{
    return std::find (visibleChannels.begin(), visibleChannels.end(), ch) != visibleChannels.end();
}

void Application::toggleChannelVisible (int ch)
{
    if (ch < 0 || ch >= audioEngine().numChannels())
        return;
    auto it = std::find (visibleChannels.begin(), visibleChannels.end(), ch);
    if (it != visibleChannels.end())
        visibleChannels.erase (it);
    else
    {
        visibleChannels.push_back (ch);
        std::sort (visibleChannels.begin(), visibleChannels.end());
    }
    syncActiveDoc ();   // keep the active document's copy in step
}

int Application::addMarkerAtCursor()
{
    juce::String name = "Marker " + juce::String ((int) markers().count() + 1).paddedLeft ('0', 2);
    if (!selection.isEmpty())
        return markers().addRegionMarker (selection.getStart(), selection.getEnd(), name);
    return markers().addPointMarker (playhead, name);
}

static juce::Range<int64_t> selectionSamples (Application& app)
{
    const double rate = app.audioEngine().sampleRate();
    return { (int64_t) (app.selection.getStart() * rate),
             (int64_t) (app.selection.getEnd() * rate) };
}

bool Application::cut(std::string* errOut)
{
    if (selection.getLength() <= 0) return true;   // nothing to cut - not an error
    copy();
    return del (errOut);
}

void Application::copy()
{
    if (selection.getLength() <= 0) return;
    auto& eng = audioEngine();
    const int nch = eng.numChannels();
    const auto range = selectionSamples (*this)
                           .getIntersectionWith ({ 0, eng.numSamples() });
    if (range.getLength() <= 0 || nch <= 0) return;

    std::vector<std::vector<float>> data;
    if (!eng.readRange (range.getStart(), range.getLength(), data))
        return;

    // With a spectral selection active, only the selected (t,f) content is
    // copied - so both plain Paste and Copy-to-New-File receive it.
    auto* d = doc();
    if (d && !d->spectralSel.empty ())
    {
        std::vector<std::vector<float>> masked;
        if (SpectralMask::extractMasked (eng, range, d->spectralSel, masked)
            && masked.size() == (size_t) nch && !masked[0].empty ())
            data = std::move (masked);
    }

    clipboard = std::make_unique<juce::AudioBuffer<float>> (nch,
                                                            (int) range.getLength());
    for (int ch = 0; ch < nch; ++ch)
        std::copy (data[(size_t) ch].begin(), data[(size_t) ch].end(),
                   clipboard->getWritePointer (ch));
}

bool Application::paste(std::string* errOut)
{
    if (!canPaste()) return true;   // nothing to paste - not an error
    // Paste at the start of the selection, or at the cursor when nothing is
    // selected.
    const double t = !selection.isEmpty() ? selection.getStart() : playhead;
    if (!audioEngine().pasteAt ((int64_t) (t * audioEngine().sampleRate()),
                                *clipboard, errOut))
        return false;
    syncActiveDoc ();
    return true;
}

bool Application::pasteMix(std::string* errOut)
{
    if (!canPaste()) return true;   // nothing to mix - not an error
    const double t = !selection.isEmpty() ? selection.getStart() : playhead;
    if (!audioEngine().pasteMixAt ((int64_t) (t * audioEngine().sampleRate()),
                                   *clipboard, errOut))
        return false;
    syncActiveDoc ();
    return true;
}

bool Application::pasteToNewChannels(std::string* errOut)
{
    if (!canPaste()) return true;   // nothing to paste - not an error
    const double t = !selection.isEmpty() ? selection.getStart() : playhead;
    const int oldN = audioEngine().numChannels();
    if (!audioEngine().pasteToNewChannelsAt ((int64_t) (t * audioEngine().sampleRate()),
                                             *clipboard, errOut))
        return false;
    // Make the freshly added channels visible (leave other choices alone).
    const int n = audioEngine().numChannels();
    for (int ch = oldN; ch < n; ++ch)
        if (!isChannelVisible (ch))
            visibleChannels.push_back (ch);
    std::sort (visibleChannels.begin(), visibleChannels.end());
    syncActiveDoc ();
    return true;
}

// ---- Merging several files into one document (Shift + drag & drop) ----

namespace
{
    void mergeBandProgress (std::function<void(float)> onProgress, double lo, double hi, float p)
    {
        if (onProgress)
            onProgress ((float) (lo + (hi - lo) * std::min (1.0f, std::max (0.0f, p))));
    }

    // Read every file at its native rate (+ markers). Band: [lo, hi).
    bool readAllMergeSources (const std::vector<juce::File>& files, double lo, double hi,
                              std::vector<sc::MergeSource>& srcs,
                              std::function<void(float)> onProgress,
                              const std::atomic<bool>* cancel)
    {
        const int n = (int) files.size ();
        for (int i = 0; i < n; ++i)
        {
            if (cancel != nullptr && cancel->load ()) return false;
            sc::MergeSource s;
            const double fLo = lo + (hi - lo) * (double) i / n;
            const double fHi = lo + (hi - lo) * (double) (i + 1) / n;
            auto subProgress = [&onProgress, fLo, fHi] (float p)
                { mergeBandProgress (onProgress, fLo, fHi, p); };
            if (!sc::readMergeSource (files[i], 0.0, s, subProgress, cancel))
                return false;   // caller reports the error with the file name
            srcs.push_back (std::move (s));
        }
        return true;
    }

    bool resampleAllMergeSources (std::vector<sc::MergeSource>& srcs, double targetRate,
                                  double lo, double hi,
                                  std::function<void(float)> onProgress,
                                  const std::atomic<bool>* cancel)
    {
        const int n = (int) srcs.size ();
        for (int i = 0; i < n; ++i)
        {
            if (!sc::resampleMergeSource (srcs[i], targetRate, cancel)) return false;
            mergeBandProgress (onProgress,
                               lo + (hi - lo) * (double) i / n,
                               lo + (hi - lo) * (double) (i + 1) / n, 1.0f);
        }
        return true;
    }

    // Stop playback and wait for any in-flight audio block to finish reading
    // the engine's buffer before a long-running worker touches it.
    void quiesceEngine (AudioEngine& eng) { eng.quiesceForEdit (); }
} // namespace

bool Application::mergeFilesIntoActiveDoc (const std::vector<juce::File>& files, sc::MergeMode mode,
                                           std::function<void(float)> onProgress,
                                           const std::atomic<bool>* cancel,
                                           std::string* errOut)
{
    auto fail = [errOut] (const juce::String& m) -> bool
    {
        if (errOut != nullptr) *errOut = m.toStdString ();
        return false;
    };

    FileDoc* d = doc ();
    if (!d || !d->engine.isFileLoaded ()) return fail ("No file is open.");
    if (files.empty ()) return fail ("Nothing to merge.");

    auto& eng = d->engine;

    // 1. Read every dropped file at its native rate (+ markers). Band: 0..0.75.
    std::vector<sc::MergeSource> srcs;
    if (!readAllMergeSources (files, 0.0, 0.75, srcs, onProgress, cancel))
        return fail ("Could not read one of the dropped files.");

    // 2. Target rate = highest among the current doc and all sources.
    double targetRate = eng.sampleRate ();
    for (const auto& s : srcs) targetRate = std::max (targetRate, s.rate);
    const bool rateChanges = std::abs (eng.sampleRate () - targetRate)
                             > 1e-9 * std::max (eng.sampleRate (), targetRate);

    // 3. Resample the sources to the target rate. Band: 0.75..0.95.
    if (!resampleAllMergeSources (srcs, targetRate, 0.75, 0.95, onProgress, cancel))
        return fail ("Resampling one of the dropped files failed.");

    // 4. The merge worker may run for a long time - stop playback and wait for
    //    in-flight blocks before touching the engine's buffer.
    quiesceEngine (eng);
    if (!eng.ensureEditable (errOut, nullptr, cancel)) return false;

    const int oldN = eng.numChannels ();
    const int64_t oldLen = eng.numSamples ();

    // 5. Read the existing content and resample it when needed.
    std::vector<std::vector<float>> prefixData (oldN);
    for (int ch = 0; ch < oldN; ++ch)
    {
        prefixData[ch].resize ((size_t) oldLen);
        if (!eng.readSamples (ch, 0, oldLen, prefixData[ch].data ()))
            return fail ("Could not read the current file's audio.");
    }

    int64_t prefixLen = oldLen;
    if (rateChanges)
    {
        juce::AudioBuffer<float> in (oldN, (int) oldLen);
        for (int ch = 0; ch < oldN; ++ch)
            std::copy (prefixData[ch].begin (), prefixData[ch].end (), in.getWritePointer (ch));
        juce::AudioBuffer<float> rs;
        if (!sc::Resampler::resample (in, eng.sampleRate (), targetRate, rs, nullptr, cancel))
            return fail ("Resampling the current file failed.");
        prefixData.clear ();
        for (int ch = 0; ch < rs.getNumChannels (); ++ch)
            prefixData.push_back (std::vector<float> (rs.getReadPointer (ch),
                                                      rs.getReadPointer (ch) + std::max<int64_t> (0, rs.getNumSamples ())));
        prefixLen = rs.getNumSamples ();
    }

    // 6. Treat the existing content as source 0 and build the merged result.
    sc::MergeSource existing;
    existing.name = d->engine.getFile ().getFileName ().isEmpty () ? d->name
                                                                   : d->engine.getFile ().getFileName ();
    existing.rate = targetRate;
    existing.channels = (int) prefixData.size ();
    existing.lengthSamples = prefixLen;
    existing.data = std::move (prefixData);
    existing.markers = d->markers.markers ();

    std::vector<sc::MergeSource> all;
    all.push_back (std::move (existing));
    for (auto& s : srcs) all.push_back (std::move (s));

    const sc::MergeLayout lay = sc::computeMergeLayout (mode, all);
    std::vector<Marker> mergedMarkers =
        sc::buildMergedMarkers (mode, nullptr, lay, all, /*firstDropped=*/1);

    // 7. Apply the merge.
    if (mode == sc::MergeMode::Sequence && !rateChanges)
    {
        // Undoable: pure append at the old end - existing samples are untouched,
        // so the patch stores no old data and undo only truncates.
        juce::AudioBuffer<float> appended (lay.outChannels, (int) (lay.outLengthSamples - prefixLen));
        appended.clear ();   // setSize does not zero a fresh buffer in this JUCE build
        for (size_t i = 1; i < all.size (); ++i)
            for (int ch = 0; ch < all[i].channels && ch < lay.outChannels; ++ch)
                appended.copyFrom (ch, (int) (lay.segs[i].startSample - prefixLen),
                                   all[i].data[ch].data (),
                                   (int) std::min<int64_t> (all[i].lengthSamples,
                                                            lay.outLengthSamples - prefixLen));

        std::vector<Marker> oldMarkers = d->markers.markers ();
        auto oldSavedSel = d->savedSelections;

        if (!eng.appendSamples (appended, "Merged " + juce::String (files.size ())
                                + " file(s) in sequence"))
            return fail ("Could not append the merged audio.");

        // Side state: markers gained entries; saved selections are unchanged but
        // must be recorded so restorePatchSideState does not wipe them.
        d->markers.restore (mergedMarkers);
        Patch& p = d->undo.lastRecorded ();
        p.hasSideState = true;
        p.oldMarkers = std::move (oldMarkers);
        p.newMarkers = d->markers.markers ();
        p.oldSavedSel = std::move (oldSavedSel);
        p.newSavedSel = d->savedSelections;
    }
    else
    {
        // Whole-file replacement: parallel layout and/or a rate change. NOT
        // undoable, like Resample.
        juce::AudioBuffer<float> merged;
        sc::buildMergedAudio (mode, lay, all, merged);
        if (!eng.replaceWholeFile (merged, targetRate))
            return fail ("Could not replace the file's audio.");
        d->markers.restore (mergedMarkers);
    }

    // 8. Make every channel visible and sync the session state with the doc.
    visibleChannels.clear ();
    for (int ch = 0; ch < eng.numChannels (); ++ch)
        visibleChannels.push_back (ch);
    playhead = std::min (playhead, eng.duration ());
    const double dur = eng.duration ();
    selection = { std::clamp (selection.getStart (), 0.0, dur),
                  std::clamp (selection.getEnd (), 0.0, dur) };
    syncActiveDoc ();
    return true;
}

namespace
{
    // Unique display name for a document without a file path:
    // "Untitled", "Untitled 2", ...
    juce::String uniqueUntitledName (const std::vector<std::unique_ptr<FileDoc>>& docs)
    {
        const juce::String base = "Untitled";
        int n = 1;
        for (;; ++n)
        {
            const juce::String nm = (n == 1 ? base : base + " " + juce::String (n));
            bool used = false;
            for (const auto& o : docs)
                if (o->name == nm) { used = true; break; }
            if (!used) return nm;
        }
    }
}

// Message-thread only: creates a new document from an already-computed merge
// result and activates it. The heavy reading/resampling work is done by the
// caller on a worker thread (sc::mergeFilesToResult).
bool Application::createDocFromMerged (sc::MergeResult& r, std::string* errOut)
{
    if (r.channels <= 0 || !std::isfinite (r.rate) || r.rate <= 0.0
        || r.data.empty () || r.data.size () != (size_t) r.channels)
    {
        if (errOut != nullptr) *errOut = "Invalid merged data.";
        return false;
    }

    auto d = std::make_unique<FileDoc>();
    if (!d->engine.initNew (r.channels, r.rate, &r.data))
    {
        if (errOut != nullptr) *errOut = "Could not create the merged document.";
        return false;
    }

    d->name = uniqueUntitledName (docs);
    d->file = juce::File();   // no path until Save As
    d->playhead = 0.0;
    d->selection = { 0, 0 };
    d->visibleChannels.clear ();
    for (int i = 0; i < r.channels; ++i)
        d->visibleChannels.push_back (i);
    d->markers.restore (r.markers);

    // Inherit the current session's display state so activating the new doc
    // doesn't reset the Frequency Analysis panel / axis calibrations.
    pushActiveDocMetaTo (*d);

    docs.push_back (std::move (d));
    activate ((int) docs.size () - 1);   // fires onActiveDocChanged -> UI sync
    return true;
}

bool Application::newFile (int numChans, double sampleRate)
{
    auto d = std::make_unique<FileDoc>();
    if (!d->engine.initNew (numChans, sampleRate))
        return false;

    d->name = uniqueUntitledName (docs);
    d->file = juce::File();   // no path until Save As
    d->playhead = 0.0;
    d->selection = { 0, 0 };
    d->visibleChannels.clear();
    for (int i = 0; i < numChans; ++i)
        d->visibleChannels.push_back (i);

    // Inherit the current session's display state so activating the new doc
    // doesn't reset the Frequency Analysis panel / axis calibrations.
    pushActiveDocMetaTo (*d);

    docs.push_back (std::move (d));
    activate ((int) docs.size() - 1);   // fires onActiveDocChanged -> UI sync
    return true;
}

bool Application::copyToNewFile (std::string* errOut)
{
    auto fail = [errOut] (const char* msg) { if (errOut != nullptr) *errOut = msg; return false; };

    auto* d = doc();
    if (!d || !d->engine.isFileLoaded ())
        return fail ("No file is open.");

    const int64_t total = d->engine.numSamples();
    juce::Range<int64_t> range = selection.getLength() > 0
                                      ? selectionSamples (*this)
                                      : juce::Range<int64_t> { 0, total };
    range = range.getIntersectionWith ({ 0, total });
    if (range.getLength() <= 0)
        return fail ("Nothing to copy.");

    // The new file gets exactly the channels currently shown.
    std::vector<int> chans = visibleChannels;
    if (chans.empty())
        for (int i = 0; i < d->engine.numChannels(); ++i)
            chans.push_back (i);
    const int nch = (int) chans.size();

    std::vector<std::vector<float>> data;
    if (!d->engine.readRange (range.getStart(), range.getLength(), data))
        return fail ("Could not read the source audio.");

    // With a spectral selection active only the selected (t,f) content is
    // copied. extractMasked processes every channel.
    if (!d->spectralSel.empty ())
    {
        std::vector<std::vector<float>> masked;
        if (SpectralMask::extractMasked (d->engine, range, d->spectralSel, masked)
            && masked.size() == (size_t) d->engine.numChannels())
            data = std::move (masked);
    }

    // Keep exactly the visible channels, in display order.
    std::vector<std::vector<float>> picked;
    picked.reserve ((size_t) nch);
    for (int i = 0; i < nch; ++i)
        picked.push_back (std::move (data[(size_t) chans[(size_t) i]]));

    auto nd = std::make_unique<FileDoc>();
    if (!nd->engine.initNew (nch, d->engine.sampleRate(), &picked))
        return fail ("Could not create the new file.");

    nd->name = uniqueUntitledName (docs);
    nd->file = juce::File();   // no path until Save As
    nd->playhead = 0.0;
    nd->selection = { 0, 0 };
    nd->visibleChannels.clear();
    for (int i = 0; i < nch; ++i)
        nd->visibleChannels.push_back (i);

    // Inherit the current session's display state (see newFile).
    pushActiveDocMetaTo (*nd);

    docs.push_back (std::move (nd));
    activate ((int) docs.size() - 1);   // fires onActiveDocChanged -> UI sync
    return true;
}

bool Application::del(std::string* errOut)
{
    // With a spectral selection active, DEL zeros the selected (t,f) content
    // instead of trimming samples: the file length is unchanged.
    if (hasSpectralSelection ())
        return maskedGainEdit (*this, "Zeroed spectral selection", 0.0f, errOut);

    if (selection.getLength() <= 0) return true;   // nothing to delete - not an error
    FileDoc* d = doc();
    const double a = selection.getStart(), b = selection.getEnd();
    const size_t depthBefore = d->undo.depth();

    // Non-audio state that this deletion will shift / trim / drop. Captured
    // before the edit so undo/redo can restore it (Patch::hasSideState).
    std::vector<Marker> oldMarkers = d->markers.markers();
    auto oldSavedSel = d->savedSelections;

    if (!audioEngine().deleteSamples (selectionSamples (*this), errOut))
        return false;
    selection = { 0, 0 };

    if (d->undo.depth() > depthBefore)   // a patch was actually recorded
    {
        d->markers.adjustForDeletion (a, b);
        for (auto& ss : d->savedSelections)
            ss.data.adjustForDeletion (a, b);
        d->metaDirty = true;   // the adjusted list no longer matches disk

        Patch& p = d->undo.lastRecorded ();
        p.hasSideState = true;
        p.oldMarkers = std::move (oldMarkers);
        p.newMarkers = d->markers.markers();
        p.oldSavedSel = std::move (oldSavedSel);
        p.newSavedSel = d->savedSelections;
        if (onTimeRangeDeleted)
            onTimeRangeDeleted (d, a, b, p);   // records + adjusts FFT snapshots
    }

    syncActiveDoc ();
    return true;
}

// Effects apply only to the currently selected (visible) channels.
static const std::vector<int>& visibleChans (Application& app)
{
    return app.visibleChannels;
}

bool Application::silence(std::string* errOut)
{
    if (hasSpectralSelection ())
        return maskedGainEdit (*this, "Silenced spectral selection", 0.0f, errOut);

    if (selection.getLength() <= 0) return true;   // nothing to silence - not an error
    return audioEngine().silenceSamples (selectionSamples (*this), &visibleChans (*this), errOut);
}

bool Application::fadeIn(std::string* errOut)
{
    if (hasSpectralSelection ())
        return applyMaskedEffect (*this, "Faded in spectral selection",
                                  [] (float* d, int len) {
                                      const float div = (float) std::max (1, len - 1);
                                      for (int i = 0; i < len; ++i)
                                          d[i] *= (float) i / (float) div;
                                  }, errOut);

    if (selection.getLength() <= 0) return true;
    return audioEngine().fadeInSamples (selectionSamples (*this), &visibleChans (*this), errOut);
}

bool Application::fadeOut(std::string* errOut)
{
    if (hasSpectralSelection ())
        return applyMaskedEffect (*this, "Faded out spectral selection",
                                  [] (float* d, int len) {
                                      const float div = (float) std::max (1, len - 1);
                                      for (int i = 0; i < len; ++i)
                                          d[i] *= 1.0f - (float) i / (float) div;
                                  }, errOut);

    if (selection.getLength() <= 0) return true;
    return audioEngine().fadeOutSamples (selectionSamples (*this), &visibleChans (*this), errOut);
}

bool Application::normalize(std::string* errOut)
{
    if (hasSpectralSelection ())
        return applyMaskedEffect (*this, "Normalized spectral selection",
                                  [] (float* d, int len) {
                                      float peak = 0.0f;
                                      for (int i = 0; i < len; ++i)
                                          peak = std::max (peak, std::abs (d[i]));
                                      if (peak > 1e-9f)
                                      {
                                          const float s = 1.0f / peak;
                                          for (int i = 0; i < len; ++i) d[i] *= s;
                                      }
                                  }, errOut);

    if (selection.getLength() <= 0) return true;
    return audioEngine().normalizeSamples (selectionSamples (*this), 1.0f, &visibleChans (*this), errOut);
}

bool Application::reverse(std::string* errOut)
{
    if (hasSpectralSelection ())
        return applyMaskedEffect (*this, "Reversed spectral selection",
                                  [] (float* d, int len) {
                                      std::vector<float> tmp (d, d + len);
                                      for (int i = 0; i < len; ++i)
                                          d[i] = tmp[(size_t) (len - 1 - i)];
                                  }, errOut);

    if (selection.getLength() <= 0) return true;
    return audioEngine().reverseSamples (selectionSamples (*this), &visibleChans (*this), errOut);
}

bool Application::changeGain (float gainDb, std::string* errOut)
{
    // No-op gains must leave the file bit-exact: no STFT round-trip, no undo
    // entry, no "unsaved" flag.
    if (gainDb == 0.0f) return true;

    if (hasSpectralSelection ())
        return maskedGainEdit (*this, "Gained spectral selection",
                               (float) std::pow (10.0, gainDb / 20.0), errOut);

    if (selection.getLength() <= 0) return true;
    return audioEngine().gainSamples (selectionSamples (*this), gainDb, &visibleChans (*this), errOut);
}

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
bool Application::noiseSuppress (const std::string& modelId, const std::string& device,
                                 float attenLimDb, bool df3PostFilter,
                                 std::function<bool(float)> onProgress, std::string* errOut,
                                 bool autoDownload)
{
    auto fail = [&errOut] (const juce::String& msg) -> bool
    {
        if (errOut) *errOut = msg.toStdString ();
        return false;
    };

    if (!audioEngine().isFileLoaded () || selection.getLength() <= 0)
        return fail ("No audio selection.");

    // Auto-download disabled: never touch the network; require the model to be
    // present already. The failure dialog appends the manual-install URLs.
    if (!autoDownload)
    {
        auto& mm = sc::ai::ModelManager::instance ();
        const auto* spec = mm.find (modelId);
        if (spec == nullptr || !mm.isInstalled (*spec))
            return fail ("Model " + juce::String (modelId) +
                         " is not installed and auto-download is disabled.");
    }

    auto samples = selectionSamples (*this).getIntersectionWith ({ 0, audioEngine().numSamples () });
    if (samples.getLength () <= 0)
        return fail ("No audio selection.");

    // Streamed files must be materialised before the result can be committed;
    // do it now so an out-of-memory failure is reported up front instead of
    // after minutes of processing whose output would be silently dropped.
    std::string err;
    if (!audioEngine().ensureEditable (&err))
        return fail (err);

    const int nch = audioEngine().numChannels ();
    std::vector<std::vector<float>> data (nch);
    for (int ch = 0; ch < nch; ++ch)
    {
        data[ch].resize ((size_t) samples.getLength());
        if (!audioEngine().readSamples (ch, samples.getStart (), samples.getLength (), data[ch].data()))
            return fail ("Failed to read the selection.");
    }

    // The pipeline works on this copy; it is only committed on success.
    const bool ok = (modelId == "deepfilternet2" || modelId == "deepfilternet3")
        ? sc::runDeepFilterPipeline (modelId, device, audioEngine().sampleRate (), nch,
                                     data, attenLimDb, df3PostFilter,
                                     onProgress, &err, &visibleChans (*this))
        : sc::runDenseUnetPipeline (modelId, device, audioEngine().sampleRate (), nch,
                                    data, onProgress, &err, &visibleChans (*this));
    if (!ok)
        return fail (err);

    if (!audioEngine().replaceSamples (samples, data,
                                       "Noise suppressed (" + juce::String (modelId) + ")", &err))
        return fail (err);
    return true;
}

// ---------------------------------------------------------------------------
// Python scripts: run a user script against the selection (or whole file).
// Data exchange happens through temp float32 WAV files plus a metadata JSON;
// the result replaces that range in place and is undoable.

bool Application::runPythonScript (const juce::File& script, const juce::String& scriptText,
                                   const std::string& pythonCmd,
                                   std::function<bool(float)> onProgress, std::string* errOut)
{
    auto fail = [&errOut] (const juce::String& msg) -> bool
    {
        if (errOut != nullptr) *errOut = msg.toStdString ();
        return false;
    };

    // Progress wrapper: reports 0..1 and returns false when cancelled.
    auto prog = [onProgress] (float f) { return onProgress == nullptr || onProgress (f); };

    if (!audioEngine ().isFileLoaded ())
        return fail ("No file is loaded.");
    FileDoc* d = doc ();
    if (d == nullptr)
        return fail ("No file is loaded.");

    // Range: the current selection, or the whole file when nothing is selected.
    juce::Range<double> selSecs = selection;
    const bool wholeFile = selSecs.getLength () <= 0.0;
    if (wholeFile)
        selSecs = { 0.0, audioEngine ().duration () };

    const double rate = audioEngine ().sampleRate ();
    auto samples = juce::Range<int64_t> ((int64_t) (selSecs.getStart () * rate),
                                         (int64_t) (selSecs.getEnd () * rate))
                      .getIntersectionWith ({ 0, audioEngine ().numSamples () });
    if (samples.getLength () <= 0)
        return fail ("Nothing to process.");

    // Save the editor's text first so that what runs is exactly what was shown.
    if (!script.replaceWithText (scriptText))
        return fail ("Could not save " + script.getFileName ());

    if (!prog (0.0f))
        return false;   // cancelled before doing anything - silent

    // Streamed files must be materialised before the result can be committed;
    // do it now so an out-of-memory failure is reported up front instead of
    // after minutes of processing whose output would be silently dropped.
    std::string err;
    if (!audioEngine ().ensureEditable (&err, [prog] (float f) { return prog (0.25f * f); }, nullptr))
        return fail (err);

    const int nch = audioEngine ().numChannels ();
    std::vector<std::vector<float>> data (nch);
    for (int ch = 0; ch < nch; ++ch)
    {
        data[(size_t) ch].resize ((size_t) samples.getLength ());
        if (!audioEngine ().readSamples (ch, samples.getStart (), samples.getLength (),
                                         data[(size_t) ch].data ()))
            return fail ("Failed to read the selection.");
    }

    // Temp files for the exchange with the script.
    const juce::File tmpDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("SpectroCraftPython");
    if (!tmpDir.exists ())
        tmpDir.createDirectory ();
    const juce::String stamp = juce::String ((int64_t) juce::Time::getCurrentTime ().toMilliseconds ())
                              + "_" + juce::String ((size_t) this);
    const juce::File inWav  = tmpDir.getChildFile ("in_"   + stamp + ".wav");
    const juce::File outWav = tmpDir.getChildFile ("out_"  + stamp + ".wav");
    const juce::File metaJ  = tmpDir.getChildFile ("meta_" + stamp + ".json");

    auto cleanup = [&] { inWav.deleteFile (); outWav.deleteFile (); metaJ.deleteFile (); };

    if (!sc::py::writeFloatWav (inWav, data, rate))
    {
        cleanup ();
        return fail ("Could not write the temp input WAV.");
    }
    metaJ.replaceWithText (sc::py::buildMetaJson (rate, nch, audioEngine ().duration (),
                                                  selSecs, wholeFile,
                                                  audioEngine ().getFile ().getFileName (),
                                                  d->markers.markers ()));

    if (!prog (0.3f)) { cleanup (); return false; }   // cancelled - silent

    // Run the interpreter: <pythonCmd> <script> <in.wav> <out.wav> <meta.json>
    const juce::StringArray cmdTokens (juce::String (pythonCmd).trim ());
    if (cmdTokens.size () == 0)
    {
        cleanup ();
        return fail ("No Python interpreter command was provided.");
    }

    juce::StringArray scriptArgs;
    for (int i = 1; i < cmdTokens.size (); ++i)   // extra flags, e.g. "-3"
        scriptArgs.add (cmdTokens[i]);
    scriptArgs.add (script.getFullPathName ());
    scriptArgs.add (inWav.getFullPathName ());
    scriptArgs.add (outWav.getFullPathName ());
    scriptArgs.add (metaJ.getFullPathName ());

    bool cancelled = false;
    int rc = -1;
    juce::String out, errText;
    const bool started = sc::py::runProcess (cmdTokens[0].toStdString (), scriptArgs,
        [&cancelled, prog] { if (!prog (0.5f)) { cancelled = true; return false; } return true; },
        &rc, &out, &errText);

    if (!started || cancelled)
    {
        cleanup ();
        if (cancelled) return false;   // user cancel is not a failure - silent
        return fail ("Could not start the Python interpreter.");
    }

    if (rc != 0 || !outWav.existsAsFile ())
    {
        cleanup ();
        juce::String msg = "Python exited with code " + juce::String (rc);
        const juce::String tail = errText.trim ().isNotEmpty () ? errText.trim () : out.trim ();
        if (tail.isNotEmpty ())
            msg += "\n" + tail.substring (std::max (0, tail.length () - 2000));
        return fail (msg);
    }

    // Read the script's output back and validate it.
    double outRate = 0.0;
    int outCh = 0;
    std::vector<std::vector<float>> newData;
    if (!sc::py::readFloatWav (outWav, &outRate, &outCh, newData))
    {
        cleanup ();
        return fail ("Could not read the script's output WAV.");
    }

    const int64_t want = samples.getLength ();
    bool bad = std::abs (outRate - rate) > 1e-9 || outCh != nch;
    for (int ch = 0; !bad && ch < nch; ++ch)
        if ((int64_t) newData[(size_t) ch].size () != want) bad = true;

    if (bad)
    {
        cleanup ();
        return fail ("The script's output does not match the input: expected "
                     + juce::String (nch) + " channel(s), " + juce::String ((int64_t) rate)
                     + " Hz and " + juce::String (want) + " samples.");
    }

    if (!prog (0.9f)) { cleanup (); return false; }   // cancelled before commit - silent

    if (!audioEngine ().replaceSamples (samples, newData,
                                        "Python script '" + script.getFileName () + "'", &err))
    {
        cleanup ();
        return fail (err);
    }

    cleanup ();
    return true;
}

bool Application::transcribe (const std::string& modelId, const std::string& language,
                              bool translate, bool fast,
                              std::function<bool(float)> onProgress, std::string* errOut,
                              bool autoDownload)
{
    auto fail = [&errOut] (const juce::String& msg) -> bool
    {
        if (errOut != nullptr) *errOut = msg.toStdString ();
        return false;
    };

    FileDoc* d = doc ();
    if (d == nullptr || !audioEngine ().isFileLoaded ())
        return fail ("No file is loaded.");

    auto& mm = sc::ai::ModelManager::instance ();
    const sc::ai::ModelSpec* spec = mm.find (modelId);
    if (spec == nullptr)
        return fail ("Unknown transcription model: " + juce::String (modelId));

    // Install phase → progress [0, 0.15) (skipped when auto-download is off
    // and the model is already present).
    std::string err;
    if (!autoDownload && !mm.isInstalled (*spec))
        return fail ("Model " + juce::String (modelId) +
                     " is not installed and auto-download is disabled.");
    if (autoDownload && !mm.ensureInstalled (*spec,
            [&onProgress] (float f, const std::string&)
            { return onProgress == nullptr || onProgress (0.15f * f); }, &err))
        return fail (err);

    // Model file: ggml-<name>.bin.
    const juce::File dir = mm.dirFor (modelId);
    const std::string baseName = modelId.substr (std::string ("whisper-").size ());
    const juce::File ggmlModel = dir.getChildFile ("ggml-" + baseName + ".bin");

    sc::ai::WhisperTranscriber::Options opts;
    opts.language  = language;
    opts.translate = translate;
    opts.beamSize  = fast ? 1 : 5;   // greedy vs beam search

    // Pipeline progress [0,1] → overall [0.15, 1].
    std::vector<ScttSegment> segs;
    const bool ok = sc::ai::runWhisperPipeline (audioEngine (), visibleChans (*this),
                                                ggmlModel, opts, segs,
            [&onProgress] (float f) { return onProgress == nullptr || onProgress (0.15f + 0.85f * f); },
            &err);
    if (!ok)
        return fail (err);

    d->setTranscription (std::move (segs));
    return true;
}

bool Application::parseChannelText (const juce::String& text, int& out)
{
    const auto t = text.trim();
    if (t.isEmpty()) return false;

    bool negative = false;
    int i = 0;
    if (t[i] == '+' || t[i] == '-') { negative = (t[i] == '-'); ++i; }

    long long v = 0;
    for (; i < t.length(); ++i)
    {
        const char c = t[i];
        if (c < '0' || c > '9') return false;
        // At most 9 digits so the value always fits in an int.
        v = v * 10 + (long long) (c - '0');
        if (v > 2000000000LL) return false;
    }

    out = negative ? -(int) v : (int) v;
    return i > (t[0] == '+' || t[0] == '-' ? 1 : 0);   // at least one digit
}

bool Application::pickBatchChannels (int numFileChans, int channel,
                                     std::vector<int>& outChans, std::string& errOut)
{
    outChans.clear ();
    if (numFileChans <= 0)
    {
        errOut = "file has no channels";
        return false;
    }

    if (channel <= 0)   // mix every channel together
    {
        for (int c = 0; c < numFileChans; ++c)
            outChans.push_back (c);
        return true;
    }

    if (channel > numFileChans)
    {
        errOut = "channel " + std::to_string (channel) +
                 " is out of range (file has " + std::to_string (numFileChans) +")";
        return false;
    }

    outChans.push_back (channel - 1);   // user-facing numbers are 1-based
    return true;
}

bool Application::batchTranscribeFolder (const juce::File& folder,
                                         const std::string& modelId,
                                         const std::string& language, bool translate,
                                         bool fast, bool skipExisting, int channel,
                                         std::function<bool(float)> onProgress,
                                         std::function<void(const juce::String&)> onStatus,
                                         std::atomic<bool>* cancelOut,
                                         BatchTranscribeSummary* summaryOut,
                                         std::string* errOut,
                                         bool autoDownload,
                                         sc::MetaSaveTarget metaTarget)
{
    auto fail = [&errOut] (const juce::String& msg) -> bool
    {
        if (errOut != nullptr) *errOut = msg.toStdString ();
        return false;
    };

    BatchTranscribeSummary summary;

    if (!folder.isDirectory ())
        return fail ("Not a folder.");

    // Enumerate WAV files recursively (the pattern match is case-insensitive
    // on Windows). Sorted for a deterministic run order.
    juce::Array<juce::File> wavs;
    for (const auto& f : folder.findChildFiles (juce::File::findFiles, true, "*.wav"))
        if (f.existsAsFile ()) wavs.add (f);
    std::sort (wavs.begin (), wavs.end (),
               [] (const juce::File& a, const juce::File& b) { return a.getFullPathName () < b.getFullPathName (); });
    summary.total = (int) wavs.size ();
    if (summary.total == 0)
        return fail ("No WAV files found in " + folder.getFullPathName ());

    auto& mm = sc::ai::ModelManager::instance ();
    const sc::ai::ModelSpec* spec = mm.find (modelId);
    if (spec == nullptr)
        return fail ("Unknown transcription model: " + juce::String (modelId));

    // Install the model once, up front (progress [0, 0.05]).
    std::string err;
    if (!autoDownload && !mm.isInstalled (*spec))
        return fail ("Model " + juce::String (modelId) +
                     " is not installed and auto-download is disabled.");
    if (autoDownload && !mm.ensureInstalled (*spec,
            [&onProgress] (float f, const std::string&)
            { return onProgress == nullptr || onProgress (0.05f * f); }, &err))
        return fail (err);

    // Model file: ggml-<name>.bin.
    const juce::File dir = mm.dirFor (modelId);
    const std::string baseName = modelId.substr (std::string ("whisper-").size ());
    const juce::File ggmlModel = dir.getChildFile ("ggml-" + baseName + ".bin");

    sc::ai::WhisperTranscriber::Options opts;
    opts.language  = language;
    opts.translate = translate;
    opts.beamSize  = fast ? 1 : 5;   // greedy vs beam search

    const int total = summary.total;
    for (int i = 0; i < total; ++i)
    {
        // Cancellation is checked between files: the current file always
        // finishes before the run stops.
        if (cancelOut != nullptr && cancelOut->load ())
            break;

        const juce::File& f = wavs[(size_t) i];
        if (onStatus != nullptr) onStatus (f.getFileName ());
        if (onProgress != nullptr)
            onProgress ((float) (0.05 + 0.95 * ((double) i / total)));

        // Skip files that already carry an SCTT chunk, when asked to.
        if (skipExisting)
        {
            ScttData existing;
            std::string rerr;
            if (readScttChunk (f, existing, &rerr))
            {
                summary.skipped++;
                continue;
            }
        }

        // Headless load: the file never opens in the editor. The engine is
        // scoped so its read handle closes BEFORE we rewrite the file —
        // Windows won't let us replace a file that's still open elsewhere.
        ScttData data;
        {
            AudioEngine eng;
            if (!eng.loadFile (f))
            {
                summary.failed++;
                summary.failures.push_back (f.getFileName ().toStdString () + ": could not load file");
                continue;
            }

            // Reduce the requested channel(s) to mono for the transcription:
            // a single 1-based channel, or every channel mixed when <= 0.
            std::vector<int> chans;
            if (!pickBatchChannels (eng.numChannels (), channel, chans, err))
            {
                summary.failed++;
                summary.failures.push_back (f.getFileName ().toStdString () + ": " + err);
                continue;
            }

            const bool ok = sc::ai::runWhisperPipeline (eng, chans, ggmlModel, opts,
                                                        data.segments,
                [&onProgress, i, total] (float f) {
                    return onProgress == nullptr ||
                           onProgress ((float) (0.05 + 0.95 * ((double) i / total)
                                             + 0.95 * (double) f / total));
                }, &err);
            if (!ok)
            {
                summary.failed++;
                summary.failures.push_back (f.getFileName ().toStdString () + ": " + err);
                continue;
            }
        }   // <- eng destroyed here: the file handle is released

        // Write the SCTT chunk back into the file on disk, preserving every
        // other RIFF chunk.
        std::string werr;
        if (!replaceScttChunkInWav (f, scttToJson (data), &werr))
        {
            summary.failed++;
            summary.failures.push_back (f.getFileName ().toStdString () + ": " + werr);
            continue;
        }

        // Optional: force the file's metadata into a specific storage mode
        // (--meta-mode on the command line). Auto keeps whatever it found.
        if (metaTarget != sc::MetaSaveTarget::Auto)
        {
            std::string merr;
            if (!sc::migrateFileMetadata (f, metaTarget, &merr))
            {
                summary.failed++;
                summary.failures.push_back (f.getFileName ().toStdString ()
                                            + ": transcription written but metadata "
                                            + (metaTarget == sc::MetaSaveTarget::Embedded
                                               ? "embedding failed: " : "sidecar write failed: ")
                                            + merr);
                continue;
            }
        }

        summary.done++;
    }

    if (onProgress != nullptr) onProgress (1.0f);
    if (summaryOut != nullptr) *summaryOut = summary;
    return true;
}
#endif

void Application::undoSteps (int n)
{
    FileDoc* d = doc();
    if (!d || n <= 0) return;

    int done = 0;
    while (done < n)
    {
        Patch p;
        if (!d->undo.popUndo (p)) break;
        d->engine.applyPatch (p, false, /*rebuild=*/false);
        if (p.hasSideState) restorePatchSideState (d, p, false);
        ++done;
    }
    if (done > 0)
        d->engine.rebuildMipmaps();   // one rebuild for the whole jump
}

void Application::redoSteps (int n)
{
    FileDoc* d = doc();
    if (!d || n <= 0) return;

    int done = 0;
    while (done < n)
    {
        Patch p;
        if (!d->undo.popRedo (p)) break;
        d->engine.applyPatch (p, true, /*rebuild=*/false);
        if (p.hasSideState) restorePatchSideState (d, p, true);
        ++done;
    }
    if (done > 0)
        d->engine.rebuildMipmaps();
}

// Restores the non-audio state recorded in a patch: markers and named saved
// selections are restored directly; shared UI state (FFT snapshot slots) goes
// through onPatchSideStateRestored because it is owned by MainWindow.
void Application::restorePatchSideState (FileDoc* d, const Patch& p, bool isRedo)
{
    d->markers.restore (isRedo ? p.newMarkers : p.oldMarkers);
    d->savedSelections = isRedo ? p.newSavedSel : p.oldSavedSel;
    d->metaDirty = true;   // restored state differs from what was last saved
    // Markers/selections persist with the unified metadata on Save - no live sidecar.
    if (onPatchSideStateRestored)
        onPatchSideStateRestored (d, p, isRedo);
}

void Application::undoStep() { undoSteps (1); }
void Application::redoStep() { redoSteps (1); }

void Application::play()
{
    if (!audioEngine().isFileLoaded()) return;

    // Self-heal: if no output device is running (initialisation can fail at
    // startup when the default device isn't available yet), try again now.
    if (audioDevice->getCurrentAudioDevice() == nullptr)
        audioDevice->initialiseWithDefaultDevices (0, 2);

    // Restart from the beginning when playback is at (or past) the end.
    if (audioEngine().position() >= audioEngine().duration() - 1e-6)
        setPlayhead (0.0);
    audioEngine().play();
}
void Application::stop() { audioEngine().stop(); }
void Application::pause() { audioEngine().pause(); }

void Application::toggleLoop()
{
    if (!audioEngine().isFileLoaded()) return;
    if (audioEngine().isLooping())
    {
        audioEngine().setLooping (false);
        return;
    }
    // Loop the current selection, or the whole file when nothing is selected.
    juce::Range<double> r = selection;
    if (r.getLength() <= 1e-6)
        r = { 0.0, audioEngine().duration() };
    audioEngine().setLoopRange (r);
    audioEngine().setLooping (true);
}

void Application::setPlayhead (double t)
{
    playhead = std::clamp (t, 0.0, audioEngine().duration());
    audioEngine().setPosition (playhead);
    syncActiveDoc ();   // keep the active document's copy in step

    // Notify the UI so both views show the same cursor position.
    if (onPlayheadChanged)
        onPlayheadChanged (playhead);
}
