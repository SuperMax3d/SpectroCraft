/**
 * SpectroCraft - Application Implementation
 */

#include "Application.h"
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    #include "NoiseSuppression.h"
    #include "AI/ModelManager.h"
    #include "AI/WhisperPipeline.h"
#endif
#include <algorithm>
#include <cstdio>
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
    settings.fftAxisMode   = std::clamp ((int) recentStore->getIntValue ("fft_axis_mode", settings.fftAxisMode), 0, 2);
    settings.fftLocalN     = roundUpPow2Clamped ((int) recentStore->getIntValue ("fft_local_n", settings.fftLocalN), 1024, 65536);
}

void Application::saveSettings ()
{
    recentStore->setValue ("fft_size", settings.fftSize);
    recentStore->setValue ("log_freq", settings.logFrequency ? 1 : 0);
    recentStore->setValue ("snap_zero", settings.snapToZeroCrossings ? 1 : 0);
    recentStore->setValue ("fft_axis_mode", settings.fftAxisMode);
    recentStore->setValue ("fft_local_n", settings.fftLocalN);
    recentStore->save ();
}

void Application::saveAiSettings ()
{
    recentStore->setValue ("ai_auto_download", aiAutoDownload ? 1 : 0);
    recentStore->setValue ("ai_ns_model", juce::String (lastNsModelId));
    recentStore->setValue ("ai_transcribe_model", juce::String (lastTranscribeModelId));
    recentStore->setValue ("ai_transcribe_lang", juce::String (lastTranscribeLanguage));
    recentStore->setValue ("ai_transcribe_translate", lastTranscribeTranslate ? 1 : 0);
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

void Application::closeFile()
{
    if (docs.empty()) return;
    const int idx = (activeDocIndex >= 0 && activeDocIndex < (int) docs.size()) ? activeDocIndex : 0;
    closeDoc (idx);
}

void Application::closeDoc (int index)
{
    if (index < 0 || index >= (int) docs.size()) return;
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

void Application::cut()
{
    if (selection.getLength() <= 0) return;
    copy();
    del();
}

void Application::copy()
{
    if (selection.getLength() <= 0) return;
    audioEngine().copySelection (selectionSamples (*this));
}

void Application::paste()
{
    if (!audioEngine().canPaste()) return;
    // Paste at the start of the selection, or at the cursor when nothing is
    // selected.
    const double t = !selection.isEmpty() ? selection.getStart() : playhead;
    audioEngine().pasteAt ((int64_t) (t * audioEngine().sampleRate()));
}

void Application::del()
{
    if (selection.getLength() <= 0) return;
    audioEngine().deleteSamples (selectionSamples (*this));
    selection = { 0, 0 };
    syncActiveDoc ();
}

// Effects apply only to the currently selected (visible) channels.
static const std::vector<int>& visibleChans (Application& app)
{
    return app.visibleChannels;
}

void Application::silence()
{
    if (selection.getLength() <= 0) return;
    audioEngine().silenceSamples (selectionSamples (*this), &visibleChans (*this));
}

void Application::fadeIn()
{
    if (selection.getLength() <= 0) return;
    audioEngine().fadeInSamples (selectionSamples (*this), &visibleChans (*this));
}

void Application::fadeOut()
{
    if (selection.getLength() <= 0) return;
    audioEngine().fadeOutSamples (selectionSamples (*this), &visibleChans (*this));
}

void Application::normalize()
{
    if (selection.getLength() <= 0) return;
    audioEngine().normalizeSamples (selectionSamples (*this), 1.0f, &visibleChans (*this));
}

void Application::reverse()
{
    if (selection.getLength() <= 0) return;
    audioEngine().reverseSamples (selectionSamples (*this), &visibleChans (*this));
}

void Application::changeGain (float gainDb)
{
    if (selection.getLength() <= 0 || gainDb == 0.0f) return;
    audioEngine().gainSamples (selectionSamples (*this), gainDb, &visibleChans (*this));
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

    const int nch = audioEngine().numChannels ();
    std::vector<std::vector<float>> data (nch);
    for (int ch = 0; ch < nch; ++ch)
    {
        data[ch].resize ((size_t) samples.getLength());
        if (!audioEngine().readSamples (ch, samples.getStart (), samples.getLength (), data[ch].data()))
            return fail ("Failed to read the selection.");
    }

    // The pipeline works on this copy; it is only committed on success.
    std::string err;
    const bool ok = (modelId == "deepfilternet2" || modelId == "deepfilternet3")
        ? sc::runDeepFilterPipeline (modelId, device, audioEngine().sampleRate (), nch,
                                     data, attenLimDb, df3PostFilter,
                                     onProgress, &err, &visibleChans (*this))
        : sc::runDenseUnetPipeline (modelId, device, audioEngine().sampleRate (), nch,
                                    data, onProgress, &err, &visibleChans (*this));
    if (!ok)
        return fail (err);

    audioEngine().replaceSamples (samples, data,
                                  "Noise suppressed (" + juce::String (modelId) + ")");
    return true;
}

bool Application::transcribe (const std::string& modelId, const std::string& language,
                              bool translate,
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

    // Model files: ggml-<name>.bin + the OpenVINO encoder IR pair.
    const juce::File dir = mm.dirFor (modelId);
    const std::string baseName = modelId.substr (std::string ("whisper-").size ());
    const juce::File ggmlModel = dir.getChildFile ("ggml-" + baseName + ".bin");
    const juce::File irXml     = dir.getChildFile ("ggml-" + baseName + "-encoder-openvino.xml");

    sc::ai::WhisperTranscriber::Options opts;
    opts.language  = language;
    opts.translate = translate;

    // Pipeline progress [0,1] → overall [0.15, 1].
    std::vector<ScttSegment> segs;
    const bool ok = sc::ai::runWhisperPipeline (audioEngine (), visibleChans (*this),
                                                ggmlModel, irXml, opts, segs,
            [&onProgress] (float f) { return onProgress == nullptr || onProgress (0.15f + 0.85f * f); },
            &err);
    if (!ok)
        return fail (err);

    d->setTranscription (std::move (segs));
    return true;
}

bool Application::batchTranscribeFolder (const juce::File& folder,
                                         const std::string& modelId,
                                         const std::string& language, bool translate,
                                         bool skipExisting,
                                         std::function<bool(float)> onProgress,
                                         std::function<void(const juce::String&)> onStatus,
                                         std::atomic<bool>* cancelOut,
                                         BatchTranscribeSummary* summaryOut,
                                         std::string* errOut,
                                         bool autoDownload)
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

    // Model files: ggml-<name>.bin + the optional OpenVINO encoder IR pair.
    const juce::File dir = mm.dirFor (modelId);
    const std::string baseName = modelId.substr (std::string ("whisper-").size ());
    const juce::File ggmlModel = dir.getChildFile ("ggml-" + baseName + ".bin");
    const juce::File irXml     = dir.getChildFile ("ggml-" + baseName + "-encoder-openvino.xml");

    sc::ai::WhisperTranscriber::Options opts;
    opts.language  = language;
    opts.translate = translate;

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

            // Mix ALL channels to mono for the transcription.
            std::vector<int> chans ((size_t) eng.numChannels());
            for (int c = 0; c < eng.numChannels (); ++c)
                chans[(size_t) c] = c;

            const bool ok = sc::ai::runWhisperPipeline (eng, chans, ggmlModel, irXml, opts,
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
        ++done;
    }
    if (done > 0)
        d->engine.rebuildMipmaps();
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
