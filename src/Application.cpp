/**
 * SpectroCraft - Application Implementation
 */

#include "Application.h"
#include "ContentAwareTimeScale.h"
#include "Recorder.h"
#include "StartupTrace.h"
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

// Monotonic time warps for edits that insert / delete / stretch timeline
// data. The live playhead and selection are warped with the same functions
// FileDoc's remapFor* methods use for markers, spectral selections, drawings
// and SCTT segments.
static double warpInsertion (double t, double a, double len)
{
    return t < a ? t : t + len;
}

static double warpDeletion (double t, double a, double b)
{
    if (t < a) return t;
    if (t >= b) return t - (b - a);
    return a;   // inside the deleted range: collapses to its start
}

static double warpStretch (double t, double a, double bOld, double bNew)
{
    if (t <= a) return t;
    if (t >= bOld) return t + (bNew - bOld);
    const double k = (bNew - a) / (bOld - a);
    return a + (t - a) * k;
}

Application::Application (const juce::File& recentStorePath)
{
    emptyEngine.setUndoManager (&emptyUndo);
    emptyEngine.setMarkersForSave (&emptyMarkers.markers()); // embedded as RIFF cue/adtl in WAV saves

    spectralEngine.setColourMap (colourMap);

    // Real-time output: the player streams from the active engine's transport
    // into the default output device, so play/pause/loop actually produce sound.
    sourcePlayer.setSource (&emptyEngine);
    scStart::mark ("before-audio-device-create");
    audioDevice = std::make_unique<juce::AudioDeviceManager>();
    audioDevice->addAudioCallback (&sourcePlayer);
    // The expensive part - enumerating and opening the default WASAPI device -
    // is deferred: it runs on a worker thread (see startAudioInitAsync), kicked
    // off right after the window appears so startup is never blocked by a slow
    // audio stack. First playback waits for completion via ensureAudioReady.
    scStart::mark ("after-audio-device-create");

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
    scStart::mark ("before-properties-file");
    recentStore = std::make_unique<juce::PropertiesFile> (storePath, opts);
    scStart::mark ("after-properties-file");

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

// Normalise a persisted octave order to one of the supported values.
static int syncThirdOctOrder (int v)
{
    static const int kOrders[9] = { 1, 2, 3, 4, 5, 10, 20, 30, 40 };
    for (int i = 0; i < 9; ++i)
        if (kOrders[i] == v) return v;
    return 40;
}

void Application::loadSettings ()
{
    settings.fftSize       = roundUpPow2Clamped ((int) recentStore->getIntValue ("fft_size", settings.fftSize), 512, 65536);
    settings.fftAuto       = recentStore->getIntValue ("fft_auto", settings.fftAuto ? 1 : 0) != 0;
    {
        const double ts = recentStore->getValue ("auto_time_slices",
                                                 juce::String (settings.autoTargetTimeSlices)).getDoubleValue();
        settings.autoTargetTimeSlices = std::isfinite (ts) ? std::clamp (ts, 1.0, 1000.0) : 80.0;
        const double bp = recentStore->getValue ("auto_bins_per_pixel",
                                                 juce::String (settings.autoBinsPerPixel)).getDoubleValue();
        settings.autoBinsPerPixel = std::isfinite (bp) ? std::clamp (bp, 0.1, 16.0) : 1.25;
    }
    settings.logFrequency  = recentStore->getIntValue ("log_freq", settings.logFrequency ? 1 : 0) != 0;
    settings.snapToZeroCrossings = recentStore->getIntValue ("snap_zero", settings.snapToZeroCrossings ? 1 : 0) != 0;
    settings.fftAxisMode   = std::clamp ((int) recentStore->getIntValue ("fft_axis_mode", settings.fftAxisMode), 0, 5);
    settings.fftLocalN     = roundUpPow2Clamped ((int) recentStore->getIntValue ("fft_local_n", settings.fftLocalN), 1024, 65536);
    settings.fftThirdOctOrder = syncThirdOctOrder (recentStore->getIntValue ("fft_third_oct_order", settings.fftThirdOctOrder));
    settings.fftRefTime    = std::clamp ((int) recentStore->getIntValue ("fft_ref_time", settings.fftRefTime), 0, 1);
    {   // smoothing window is one of None / 3 / 5 / 7 / 9 / 11 frames
        const int sm = recentStore->getIntValue ("fft_smooth", settings.fftSmooth);
        settings.fftSmooth = (sm == 3 || sm == 5 || sm == 7 || sm == 9 || sm == 11)
                             ? sm : ((sm >= 1 && sm <= 4) ? 3 : 0);
    }
    settings.fftTrack      = std::clamp ((int) recentStore->getIntValue ("fft_track", settings.fftTrack), 0, 2);
    // Drawing panel defaults (last used ink settings for new drawings).
    settings.drawWidth     = std::clamp (recentStore->getValue ("draw_width",
                                                               juce::String (settings.drawWidth)).getFloatValue(),
                                         0.5f, 20.0f);
    settings.drawColourARGB = recentStore->getIntValue ("draw_colour", settings.drawColourARGB);
    settings.drawTextSize   = std::clamp (recentStore->getValue ("draw_text_size",
                                                                juce::String (settings.drawTextSize)).getFloatValue(),
                                          6.0f, 72.0f);
    // Track axis placement + grid lines (View menu).
    settings.trackAxisOnRight   = recentStore->getIntValue ("track_axis_right", 0) != 0;
    settings.trackRulerAtBottom = recentStore->getIntValue ("track_ruler_bottom", 0) != 0;
    settings.showAmpGrid        = recentStore->getIntValue ("show_amp_grid", 1) != 0;
    settings.showFreqGrid       = recentStore->getIntValue ("show_freq_grid", 1) != 0;
    // Spectrogram harmonic markers (Spectral Controls panel).
    settings.specHarmVisible    = recentStore->getIntValue ("spec_harm_visible",
                                                            settings.specHarmVisible ? 1 : 0) != 0;
    settings.specHarmMaxK       = std::clamp (recentStore->getIntValue ("spec_harm_max",
                                                                        settings.specHarmMaxK), 1, 1000);
    settings.specHarmN          = std::clamp (recentStore->getIntValue ("spec_harm_n",
                                                                        settings.specHarmN), 1, 1000);
    settings.specHarmNumbers    = recentStore->getIntValue ("spec_harm_numbers",
                                                            settings.specHarmNumbers ? 1 : 0) != 0;
    // Spectrogram export overlays (File -> Spectrogram submenu).
    settings.exportAxes       = recentStore->getIntValue ("exp_axes", 0) != 0;
    settings.exportMarkers    = recentStore->getIntValue ("exp_markers", 0) != 0;
    settings.exportSelections = recentStore->getIntValue ("exp_selections", 0) != 0;
    settings.exportDrawings   = recentStore->getIntValue ("exp_drawings", 0) != 0;
    // Stem Separation (Effects menu).
    settings.acppModel          = recentStore->getValue ("acpp_model", juce::String ());
    settings.acppBackend        = std::clamp ((int) recentStore->getIntValue ("acpp_backend", 1), 1, 3);
    settings.acppExtractOriginal = recentStore->getIntValue ("acpp_extract_orig", 1) != 0;
    settings.stemSepMode        = std::clamp ((int) recentStore->getIntValue ("stem_sep_mode", 1), 1, 3);
    settings.acppModel6         = recentStore->getValue ("acpp_model_6", juce::String ());
    settings.stemSepOutputMode  = std::clamp ((int) recentStore->getIntValue ("stem_sep_output", 1), 1, 3);
    // Super Resolution (Effects menu).
    settings.superResMode       = std::clamp ((int) recentStore->getIntValue ("super_res_mode", 1), 1, 2);
    settings.superResModel      = recentStore->getValue ("super_res_model", juce::String ());
    settings.superResOutputMode = std::clamp ((int) recentStore->getIntValue ("super_res_output", 1), 1, 3);
    // Filter EQ: remembered curve + user presets.
    settings.eqLastCurve        = recentStore->getValue ("eq_last_curve", juce::String ());
    settings.eqUserPresets      = recentStore->getValue ("eq_user_presets", juce::String ());
    // Enveloppe: remembered curve + user presets.
    settings.envLastCurve       = recentStore->getValue ("env_last_curve", juce::String ());
    settings.envUserPresets     = recentStore->getValue ("env_user_presets", juce::String ());

    // Split spectral selection configuration: "in->dest;in->dest" with 1-based
    // channel numbers.
    {
        juce::StringArray toks;
        toks.addTokens (recentStore->getValue ("split_config", juce::String ()), ";", "");
        settings.splitConfig.clear ();
        for (const auto& t : toks)
        {
            juce::StringArray kv;
            kv.addTokens (t, "->");
            if (kv.size () == 2)
            {
                const int in  = kv[0].getIntValue () - 1;
                const int dst = kv[1].getIntValue () - 1;
                if (in >= 0 && dst >= 0) settings.splitConfig.push_back ({ in, dst });
            }
        }
    }
    // Recording device selection (Transport menu).
    recording.pluginId   = recentStore->getValue ("rec_plugin", juce::String ());
    recording.deviceId   = recentStore->getValue ("rec_device", juce::String ());
    recording.channels   = std::clamp (recentStore->getIntValue ("rec_channels", recording.channels), 1, 512);
    recording.channelMode = std::clamp (recentStore->getIntValue ("rec_channel_mode", 0), 0, 512);
    recording.srcChannels = std::clamp (recentStore->getIntValue ("rec_src_channels", 0), 0, 512);
    {
        const double r = recentStore->getValue ("rec_rate", juce::String (recording.rate)).getDoubleValue ();
        recording.rate = r > 100.0 ? r : 48000.0;
    }
    recording.bitDepth   = std::clamp (recentStore->getIntValue ("rec_depth", recording.bitDepth), 8, 64);
    {
        const int m = recentStore->getIntValue ("rec_mode", 0);
        recording.mode = (m == 1) ? RecordingState::Mode::Loop
                       : (m == 2) ? RecordingState::Mode::Timed
                                 : RecordingState::Mode::Unlimited;
    }
    {
        const double d = recentStore->getValue ("rec_duration", juce::String ())
                             .getDoubleValue ();
        recording.durationSecs = (d > 0.0 && std::isfinite (d)) ? d : 0.0;
    }
}

void Application::saveSettings ()
{
    recentStore->setValue ("fft_size", settings.fftSize);
    recentStore->setValue ("fft_auto", settings.fftAuto ? 1 : 0);
    recentStore->setValue ("auto_time_slices", settings.autoTargetTimeSlices);
    recentStore->setValue ("auto_bins_per_pixel", settings.autoBinsPerPixel);
    recentStore->setValue ("log_freq", settings.logFrequency ? 1 : 0);
    recentStore->setValue ("snap_zero", settings.snapToZeroCrossings ? 1 : 0);
    recentStore->setValue ("fft_axis_mode", settings.fftAxisMode);
    recentStore->setValue ("fft_local_n", settings.fftLocalN);
    recentStore->setValue ("fft_third_oct_order", settings.fftThirdOctOrder);
    recentStore->setValue ("fft_ref_time", settings.fftRefTime);
    recentStore->setValue ("fft_smooth", settings.fftSmooth);
    recentStore->setValue ("fft_track", settings.fftTrack);
    recentStore->setValue ("draw_width", settings.drawWidth);
    recentStore->setValue ("draw_colour", settings.drawColourARGB);
    recentStore->setValue ("draw_text_size", settings.drawTextSize);
    recentStore->setValue ("track_axis_right", settings.trackAxisOnRight ? 1 : 0);
    recentStore->setValue ("track_ruler_bottom", settings.trackRulerAtBottom ? 1 : 0);
    recentStore->setValue ("show_amp_grid", settings.showAmpGrid ? 1 : 0);
    recentStore->setValue ("show_freq_grid", settings.showFreqGrid ? 1 : 0);
    recentStore->setValue ("spec_harm_visible", settings.specHarmVisible ? 1 : 0);
    recentStore->setValue ("spec_harm_max", settings.specHarmMaxK);
    recentStore->setValue ("spec_harm_n", settings.specHarmN);
    recentStore->setValue ("spec_harm_numbers", settings.specHarmNumbers ? 1 : 0);
    recentStore->setValue ("exp_axes", settings.exportAxes ? 1 : 0);
    recentStore->setValue ("exp_markers", settings.exportMarkers ? 1 : 0);
    recentStore->setValue ("exp_selections", settings.exportSelections ? 1 : 0);
    recentStore->setValue ("exp_drawings", settings.exportDrawings ? 1 : 0);
    recentStore->setValue ("acpp_model", settings.acppModel);
    recentStore->setValue ("acpp_backend", settings.acppBackend);
    recentStore->setValue ("acpp_extract_orig", settings.acppExtractOriginal ? 1 : 0);
    recentStore->setValue ("stem_sep_mode", settings.stemSepMode);
    recentStore->setValue ("acpp_model_6", settings.acppModel6);
    recentStore->setValue ("stem_sep_output", settings.stemSepOutputMode);
    recentStore->setValue ("super_res_mode", settings.superResMode);
    recentStore->setValue ("super_res_model", settings.superResModel);
    recentStore->setValue ("super_res_output", settings.superResOutputMode);
    recentStore->setValue ("eq_last_curve", settings.eqLastCurve);
    recentStore->setValue ("eq_user_presets", settings.eqUserPresets);
    recentStore->setValue ("env_last_curve", settings.envLastCurve);
    recentStore->setValue ("env_user_presets", settings.envUserPresets);

    juce::String splitCfg;
    for (const auto& p : settings.splitConfig)
        splitCfg += juce::String (p.first + 1) + "->" + juce::String (p.second + 1) + ";";
    recentStore->setValue ("split_config", splitCfg);
    recentStore->setValue ("rec_plugin", recording.pluginId);
    recentStore->setValue ("rec_device", recording.deviceId);
    recentStore->setValue ("rec_channels", recording.channels);
    recentStore->setValue ("rec_channel_mode", recording.channelMode);
    recentStore->setValue ("rec_src_channels", recording.srcChannels);
    recentStore->setValue ("rec_rate", juce::String (recording.rate));
    recentStore->setValue ("rec_depth", recording.bitDepth);
    recentStore->setValue ("rec_mode", (int) recording.mode);
    recentStore->setValue ("rec_duration", juce::String (recording.durationSecs));
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

Application::~Application()
{
    if (audioInitThread.joinable())
        audioInitThread.join();   // never let the worker outlive this object
}

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
    settings.fftThirdOctOrder = syncThirdOctOrder (d->freqAnalysis.thirdOctOrder);
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
        fa.thirdOctOrder = syncThirdOctOrder (settings.fftThirdOctOrder);
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
    fa.thirdOctOrder = syncThirdOctOrder (settings.fftThirdOctOrder);
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
    // Closing the document being recorded into ends the session first, so
    // recDoc_ can never dangle.
    if (recording.active && recDoc_ == docs[(size_t) index].get ())
        stopRecording ();
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

// ============================================================================
// Recording (plugin-based capture)
// ============================================================================

void Application::stepRecordingWindow (double& t0, double& t1, bool& followEnd,
                                       double newDur)
{
    // "At the end" = right edge within epsBehind behind / epsAhead ahead of
    // the data. A single tick can append up to one second of audio (the
    // per-tick read cap), so epsBehind must be wider than that or following
    // would disengage on every big tick.
    const double epsBehind = 1.5, epsAhead = 0.1;
    const auto atEnd = [&] (double d) { return t1 >= d - epsBehind && t1 <= d + epsAhead; };

    if (followEnd)
    {
        if (!atEnd (newDur))
        {   // The user moved the window away from the edge: stop following and
            // leave it exactly where they put it.
            followEnd = false;
            return;
        }
    }
    else if (!atEnd (newDur))
        return;   // not at the end yet - don't touch the user's window

    followEnd = true;
    double w = (t1 - t0) > 0.05 ? (t1 - t0) : kRecFollowMinWidthSecs;
    // While the window is still anchored at time zero it is ramping up from
    // the start of the recording: grow it to the minimum follow width so the
    // user gets a comfortable view instead of a sliver. Once the window has
    // started sliding (t0 > 0) its width is whatever the user set.
    if (t0 <= 0.05 && w < kRecFollowMinWidthSecs)
        w = kRecFollowMinWidthSecs;
    t1 = newDur;
    t0 = std::max (0.0, t1 - w);
}

static juce::String recordingDocName()
{
    const auto now = juce::Time::getCurrentTime ();
    // getMonth() is zero-based in this JUCE build.
    juce::String s = "Recording_" + juce::String (now.getYear ()) + "-"
                    + juce::String (now.getMonth () + 1).paddedLeft ('0', 2) + "-"
                    + juce::String (now.getDayOfMonth ()).paddedLeft ('0', 2)
                    + "_" + juce::String (now.getHours ()).paddedLeft ('0', 2)
                    + juce::String (now.getMinutes ()).paddedLeft ('0', 2)
                    + juce::String (now.getSeconds ()).paddedLeft ('0', 2);
    return s;
}

void Application::setRecordingMode (RecordingState::Mode m, double secs)
{
    if (m == RecordingState::Mode::Unlimited || !(secs > 0.0))
        secs = 0.0;
    recording.mode = m;
    recording.durationSecs = secs;
    saveSettings ();
}

juce::String Application::formatRecDurationSecs (double secs)
{
    const int64_t ms = (int64_t) std::llround (secs * 1000.0);
    if (ms < 3600000)
    {
        if (ms % 1000 == 0)
            return juce::String (ms / 1000) + "s";
        const int whole = (int) (ms / 1000);
        const int tenth = (int) ((ms % 1000) / 100);
        return juce::String (whole) + "." + juce::String (tenth) + "s";
    }
    const int totalSecs = (int) std::min<int64_t> (ms / 1000, INT_MAX);
    const int h = totalSecs / 3600;
    return juce::String (h) + ":"
           + juce::String ((totalSecs % 3600) / 60).paddedLeft ('0', 2) + ":"
           + juce::String (totalSecs % 60).paddedLeft ('0', 2);
}

bool Application::pullRecFrames (FileDoc* d, long long cap)
{
    auto& eng = d->engine;
    const int ch = eng.numChannels ();
    if (ch <= 0 || cap <= 0) return false;

    // Loop mode: once the document reaches its configured length it stops
    // growing and progressively overwrites from the start instead. The write
    // position is where the running capture stream currently sits inside the
    // fixed-size window, so the buffer always holds one contiguous stretch of
    // the stream (a sliding window). While still filling, pulls are clamped
    // so the document lands exactly on its capacity.
    const bool loopMode = recording.mode == RecordingState::Mode::Loop
                          && recording.loopCapFrames > 0;
    // numSamples() is the per-channel length, i.e. the frame count.
    const bool loopFull = loopMode
                          && eng.numSamples () >= recording.loopCapFrames;

    auto& host = sc::rec::RecorderHost::instance ();
    const long long avail = host.availableFrames ();
    if (avail <= 0) return false;
    long long n = std::min (avail, cap);
    int64_t writePosFrame = 0;   // only used in loop mode
    if (loopMode)
    {
        const int64_t pos = recording.framesRecorded % recording.loopCapFrames;
        writePosFrame = pos;
        n = std::min (n, (long long) (recording.loopCapFrames - pos));   // stop at the wrap edge
    }

    // The plugin delivers in its own source layout. When a single channel is
    // selected we read the full source width and pick that channel out.
    const int srcCh = std::max (ch, recording.srcChannels > 0 ? recording.srcChannels : ch);
    recTmpBuf_.resize ((size_t) n * (size_t) srcCh);
    const int frames = host.read (recTmpBuf_.data (), srcCh, n);
    if (frames <= 0) return false;

    float* docData = recTmpBuf_.data ();
    std::vector<float> mapped;   // only used when a channel pick is needed
    if (!(srcCh == ch && recording.channelMode == 0))
    {
        const int pick = recording.channelMode >= 1 ? recording.channelMode - 1 : 0;
        mapped.resize ((size_t) frames * (size_t) ch);
        for (long long f = 0; f < frames; ++f)
            for (int c = 0; c < ch; ++c)
                mapped[(size_t) f * (size_t) ch + (size_t) c] =
                    recTmpBuf_[(size_t) f * (size_t) srcCh
                              + (recording.channelMode >= 1 ? (size_t) pick : (size_t) c)];
        docData = mapped.data ();
    }

    const bool ok = loopFull ? eng.overwriteSamples (docData, writePosFrame * ch, frames)
                             : eng.appendSamples (docData, frames);
    if (!ok)
        return false;   // audio thread busy - data stays in the plugin ring

    recording.framesRecorded += frames;
    return true;
}

bool Application::startRecording (std::string* errOut)
{
    auto fail = [errOut] (const juce::String& msg) {
        if (errOut != nullptr) *errOut = msg.toStdString ();
        return false;
    };

    if (recording.active)
        return fail ("Already recording.");

    auto& host = sc::rec::RecorderHost::instance ();
    host.scanPlugins ();   // idempotent; picks up DLLs dropped in since startup

    if (recording.pluginId.isEmpty () || recording.deviceId.isEmpty ())
        return fail ("Choose a recording device first: Transport > Recording Device.");

    if (host.findPlugin (recording.pluginId) == nullptr)
        return fail ("The selected recorder plugin is no longer available.");

    // Determine the device's source channel count so a single-channel
    // selection can be honoured host-side: the plugin always captures its
    // full width and pullRecFrames() picks out the wanted channel.
    int devCh = recording.channels;
    if (const auto* p = host.findPlugin (recording.pluginId))
        for (const auto& d : p->devices)
            if (d.id == recording.deviceId) { devCh = std::max (1, d.maxChannels); break; }

    int wantCh;
    if (recording.channelMode >= 1 && recording.channelMode <= devCh)
        wantCh = devCh;   // capture everything; pick one channel in pullRecFrames
    else
    {
        recording.channelMode = 0;   // stale/invalid selection - record all channels
        wantCh = std::clamp (recording.channels, 1, devCh);
    }

    sc_rec_negotiated neg {};
    if (!host.configure (recording.pluginId, recording.deviceId,
                         wantCh, recording.rate, recording.bitDepth,
                         &neg, errOut))
        return false;   // *errOut already set by the host
    recording.srcChannels = neg.channels;
    recording.channels = (recording.channelMode >= 1) ? 1 : neg.channels;
    recording.rate     = neg.rate;
    recording.bitDepth = neg.bitDepth;

    // Mode-specific limits (Transport > Recording Options).
    recording.startTime = juce::Time::getCurrentTime ();
    if (recording.mode == RecordingState::Mode::Timed && recording.durationSecs > 0.0)
        recording.timedTargetFrames = (int64_t) std::llround (recording.durationSecs * neg.rate);
    else
        recording.timedTargetFrames = 0;
    if (recording.mode == RecordingState::Mode::Loop && recording.durationSecs > 0.0)
        recording.loopCapFrames = (int64_t) std::llround (recording.durationSecs * neg.rate);
    else
        recording.loopCapFrames = 0;

    if (!newFile (recording.channels, recording.rate))
        return fail ("Could not create a new document for the recording.");
    FileDoc* d = doc ();

    // Timestamped name, made unique against the other open documents.
    juce::String nm = recordingDocName ();
    int k = 2;
    for (;;)
    {
        bool used = false;
        for (const auto& dd : docs)
            if (dd->name == nm) { used = true; break; }
        if (!used) break;
        nm = recordingDocName () + " " + juce::String (k++);
    }
    d->name = nm;

    recDoc_ = d;
    if (!host.start (errOut))
    {
        host.stop ();
        recDoc_ = nullptr;
        closeDoc ((int) docs.size () - 1);   // drop the empty document we just made
        return false;   // *errOut already set by the host
    }

    recording.active = true;
    recording.framesRecorded = 0;
    recording.followEnd = true;
    viewStart = 0.0;
    viewEnd = std::max (1e-3, d->engine.duration ());   // ~0 at start; the first
                                                       // tick snaps to the end
    recTmpBuf_.clear ();
    startTimerHz (25);
    if (onStateChange) onStateChange ();
    return true;
}

void Application::stopRecording ()
{
    if (!recording.active)
        return;

    auto& host = sc::rec::RecorderHost::instance ();
    host.stop ();

    FileDoc* d = recDoc_;

    // Drain whatever the plugin still has buffered.
    if (d != nullptr)
    {
        auto& eng = d->engine;
        // Up to ~1 s of audio per pass keeps the drain quick.
        for (int guard = 0; guard < 100 && host.availableFrames () > 0; ++guard)
            if (!pullRecFrames (d, eng.sampleRate ()))
                break;   // nothing left or audio thread busy - rest stays in ring
    }

    // A session that ran for at least a second without receiving a single
    // frame is a broken capture (the device's audio engine rejected our
    // session, the endpoint died, ...): report it instead of leaving an empty
    // document behind. Stopping within the first second keeps the document -
    // the ring may simply not have filled yet.
    const double elapsedSecs = (juce::Time::getCurrentTime ().getMilliseconds ()
                                - recording.startTime.getMilliseconds ()) / 1000.0;
    const bool zeroData = recording.framesRecorded == 0 && elapsedSecs >= 1.0;
    juce::String lastErr;
    if (zeroData)
        lastErr = host.lastError ();

    recording.active = false;
    recDoc_ = nullptr;
    stopTimer ();

    if (d != nullptr)
    {
        if (zeroData)
        {
            int idx = -1;
            for (int i = 0; i < (int) docs.size (); ++i)
                if (docs[(size_t) i].get () == d) { idx = i; break; }
            if (idx >= 0)
                closeDoc (idx);   // safe: recording.active is false now

            juce::String msg = "No audio data was received from the device.";
            const juce::String le = lastErr.trim ();
            if (!le.isEmpty () && le != "no error")
                msg += "\n\n" + le;
            juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                               "Recording", msg);
        }
        else
            d->engine.rebuildMipmaps ();   // final overview refresh for the finished file
    }

    if (onStateChange) onStateChange ();
}

void Application::timerCallback ()
{
    if (!recording.active || recDoc_ == nullptr)
        return;

    // Validate the pointer: closeDoc() stops recording first, but be safe.
    bool stillThere = false;
    for (const auto& dd : docs)
        if (dd.get () == recDoc_) { stillThere = true; break; }
    if (!stillThere) { stopRecording (); return; }

    FileDoc* d = recDoc_;
    auto& eng = d->engine;
    const int ch = eng.numChannels ();
    if (ch <= 0)
        return;

    // Pull whatever the plugin has produced. Skip this tick while the audio
    // thread is inside the buffer - the data stays in the plugin's ring and
    // is picked up next tick, so nothing is lost.
    bool appended = false;
    if (!eng.audioThreadInBuffer ())
    {
        auto& host = sc::rec::RecorderHost::instance ();
        // Up to ~1 s of audio per tick keeps the UI responsive.
        if (host.availableFrames () > 0)
            appended = pullRecFrames (d, eng.sampleRate ());
    }

    // Timed mode: stop automatically once the requested amount is recorded.
    if (recording.mode == RecordingState::Mode::Timed
        && recording.timedTargetFrames > 0
        && recording.framesRecorded >= recording.timedTargetFrames)
    {
        stopRecording ();   // drains the ring and keeps the finished document
        return;
    }

    // Zero-data watchdog: a capture that delivers nothing for ~3 s is almost
    // certainly broken. Stop and report instead of leaving an empty document
    // behind (stopRecording() shows the failure itself).
    if (recording.framesRecorded == 0 &&
        (juce::Time::getCurrentTime ().getMilliseconds ()
         - recording.startTime.getMilliseconds ()) / 1000.0 > 3.0)
    {
        stopRecording ();
        return;
    }

    // Follow-end logic + view updates. Only push the window while the recorded
    // document is the active one; otherwise data keeps accumulating silently.
    if (doc () == d)
    {
        const double oldT0 = viewStart, oldT1 = viewEnd;
        stepRecordingWindow (viewStart, viewEnd, recording.followEnd, eng.duration ());
        if (viewStart != oldT0 || viewEnd != oldT1)
        {
            if (onSetTimeWindow) onSetTimeWindow (viewStart, viewEnd);
        }
        else if (appended && onStateChange)
        {
            onStateChange ();   // repaint both views with the same window
        }
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

void Application::addDrawing (const DrawingItem& item)
{
    FileDoc* d = doc ();
    if (d == nullptr) return;
    DrawingItem it = item;
    it.normaliseBounds ();
    d->drawings.push_back (it);
    d->metaDirty = true;   // the list persists with the file's metadata on Save
    if (onDrawingsChanged)
        onDrawingsChanged ();
}

void Application::deleteSelectedDrawings (const std::vector<int>& rowsIn)
{
    FileDoc* d = doc ();
    if (d == nullptr) return;

    std::vector<int> rows;
    for (int r : rowsIn)
        if (r >= 0 && r < (int) d->drawings.size ())
            rows.push_back (r);
    if (rows.empty()) return;

    // Erase from the bottom up so earlier indices stay valid.
    std::sort (rows.begin (), rows.end (), std::greater<int>());
    for (int r : rows)
        d->drawings.erase (d->drawings.begin () + r);
    d->metaDirty = true;
    if (onDrawingsChanged)
        onDrawingsChanged ();
}

void Application::applyDrawingEdit (int row, const DrawingItem& updated)
{
    FileDoc* d = doc ();
    if (d == nullptr || row < 0 || row >= (int) d->drawings.size ()) return;

    // The edit dialog changes only the editable fields; kind and geometry
    // stay where the user placed them.
    DrawingItem& it = d->drawings [(size_t) row];
    it.text       = updated.text;
    it.comment    = updated.comment;
    it.colourARGB = updated.colourARGB;
    it.widthPx    = std::clamp (updated.widthPx, 0.5f, 20.0f);
    it.textSize   = std::clamp (updated.textSize, 6.0f, 72.0f);

    d->metaDirty = true;
    if (onDrawingsChanged)
        onDrawingsChanged ();
}

void Application::setShowAllDrawings (bool on)
{
    if (showAllDrawings == on) return;
    showAllDrawings = on;
    if (onStateChange) onStateChange ();   // repaints the views + refreshes panels
}

void Application::toggleDrawingHidden (int row)
{
    FileDoc* d = doc ();
    if (d == nullptr || row < 0 || row >= (int) d->drawings.size ()) return;
    d->drawings [(size_t) row].hidden = !d->drawings [(size_t) row].hidden;
    d->metaDirty = true;   // the visibility state persists with the metadata
    if (onDrawingsChanged)
        onDrawingsChanged ();
}

void Application::ensureDrawingVisible (double t0, double f0, double t1, double f1)
{
    const double dur = audioEngine().duration();
    if (!(dur > 0.0)) return;
    const double maxF = 0.5 * audioEngine().sampleRate();

    // The usual +10% breathing room on every side, clamped to the file.
    DrawingItem::expandBounds10 (t0, f0, t1, f1, dur, maxF);

    // Time window: same pan/zoom policy as ensureTimeRangeVisible(), but the
    // 10% margin was already applied above (no double expansion).
    if (onSetTimeWindow)
    {
        const double vs = viewStart, ve = viewEnd;
        if (!(t0 >= vs - 1e-9 && t1 <= ve + 1e-9))
        {
            const double W = std::max (ve - vs, 1e-9);
            const double S = t1 - t0;
            if (S >= W)
                onSetTimeWindow (t0, t1);
            else
            {
                const double n0 = std::clamp ((t0 < vs) ? t0 : t1 - W,
                                              0.0, std::max (0.0, dur - W));
                onSetTimeWindow (n0, std::min (n0 + W, dur));
            }
        }
    }

    // Frequency window: only adjust when the band isn't already fully visible
    // in the spectrogram's local axis - same policy as the time side above.
    if (onSetSpectralFreqRange && f1 > f0)
    {
        bool inside = false;
        if (getSpectralFreqRange)
        {
            const auto cur = getSpectralFreqRange ();
            inside = cur.first <= f0 + 1e-9 && cur.second >= f1 - 1e-9
                     && cur.second > cur.first;
        }
        if (!inside)
            onSetSpectralFreqRange (f0, f1);
    }
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
    FileDoc* d = doc ();
    const size_t depthBefore = d->undo.depth ();
    std::vector<Marker> oldMarkers = d->markers.markers ();
    auto oldSavedSel = d->savedSelections;
    TimeAnchoredState oldTA = captureTimeAnchored ();   // playhead/sel/spectral/drawings/SCTT

    if (!audioEngine().pasteAt ((int64_t) (t * audioEngine().sampleRate()),
                                *clipboard, errOut))
        return false;

    const double lenSecs = (double) clipboard->getNumSamples () / audioEngine ().sampleRate ();
    d->remapForInsertion (t, lenSecs);   // markers/savedSel/spectralSel/drawings/SCTT

    // Content from t on shifted right by lenSecs; the live cursor/selection follow.
    playhead = playhead < t ? playhead : playhead + lenSecs;
    selection = { warpInsertion (selection.getStart (), t, lenSecs),
                  warpInsertion (selection.getEnd (),   t, lenSecs) };

    if (d->undo.depth () > depthBefore)   // a patch was actually recorded
    {
        Patch& p = d->undo.lastRecorded ();
        p.hasSideState = true;
        p.oldMarkers  = std::move (oldMarkers);
        p.newMarkers  = d->markers.markers ();
        p.oldSavedSel = std::move (oldSavedSel);
        p.newSavedSel = d->savedSelections;
        commitTimeAnchoredToPatch (oldTA, p);
        if (onTimeRangeInserted)
            onTimeRangeInserted (d, t, lenSecs, p);   // records + adjusts FFT snapshots
    }

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
bool Application::createDocFromMerged (sc::MergeResult& r, std::string* errOut,
                                       const juce::String& suggestedName)
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

    if (!suggestedName.isEmpty ())
    {
        // Keep document names unique across the open set.
        d->name = suggestedName;
        int n = 2;
        while (std::find_if (docs.begin (), docs.end (),
                             [&] (const auto& x) { return x->name == d->name; }) != docs.end ())
            d->name = suggestedName + juce::String (" ") + juce::String (n++);
    }
    else
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

// Message-thread only. Applies a stem-separation result to the active document
// according to outputMode: 1 = create a new document from it (the historical
// behaviour), 2 = replace the current file's content with it (one undoable
// step; markers inside the extracted range keep their relative position,
// everything else is dropped with the old timeline), 3 = append its tracks
// after the existing channels (one undoable step).
bool Application::applyStemSeparationResult (sc::MergeResult& r, int outputMode,
                                             double extStartSecs,
                                             const juce::String& suggestedName,
                                             std::string* errOut)
{
    auto fail = [errOut] (const char* msg) { if (errOut != nullptr) *errOut = msg; return false; };

    if (r.channels <= 0 || !std::isfinite (r.rate) || r.rate <= 0.0
        || r.data.empty () || r.data.size () != (size_t) r.channels)
        return fail ("Invalid stem separation result.");

    // Mode 1: a brand-new document (the historical behaviour).
    if (outputMode == 1)
        return createDocFromMerged (r, errOut, suggestedName);

    auto* d = doc ();
    if (!d || !d->engine.isFileLoaded ())
        return fail ("No file is open.");
    auto& eng = d->engine;

    // The worker normalises every track to the source document's rate.
    if (std::abs (r.rate - eng.sampleRate ()) > 1e-6 * r.rate)
        return fail ("The result sample rate does not match the open file.");

    const int nTracks = (int) r.data.size ();
    std::vector<Marker> oldMarkers = d->markers.markers ();
    auto oldSavedSel = d->savedSelections;
    TimeAnchoredState oldTA = captureTimeAnchored ();   // overwrite mode remaps it

    if (outputMode == 2)
    {
        // Overwrite: one undoable patch replaces the whole content. State anchored
        // inside the extracted range keeps its relative position; everything else
        // is dropped with the old timeline - modelled as two deletions, [0,
        // extStart) and [extStart+newDur, totalOld).
        const double newDur   = (double) r.data[0].size () / eng.sampleRate ();
        const double totalOld = eng.duration ();

        d->remapForDeletion (0.0, extStartSecs);
        d->remapForDeletion (newDur, totalOld - extStartSecs);

        if (!eng.replaceWholeFileUndoable (r.data, "Stem separation"))
            return fail ("Could not replace the file content.");

        // Keep playhead/selection at their relative position inside the extracted range.
        const double dur = eng.duration ();
        auto W = [&](double t) {
            double u = (t < extStartSecs) ? 0.0 : t - extStartSecs;   // after first deletion
            if (totalOld > extStartSecs && u >= newDur && u <= totalOld - extStartSecs)
                u = newDur;                                          // inside the second deleted range
            else if (u > totalOld - extStartSecs)
                u -= (totalOld - extStartSecs) - newDur;             // after it: shift left
            return std::clamp (u, 0.0, dur);
        };
        playhead  = W (playhead);
        selection = { W (selection.getStart ()), W (selection.getEnd ()) };

        // The new file has exactly the stem tracks; show all of them.
        visibleChannels.clear ();
        for (int ch = 0; ch < nTracks; ++ch)
            visibleChannels.push_back (ch);
    }
    else if (outputMode == 3)
    {
        // Append: existing channels stay untouched and each stem becomes a new
        // track holding its data over the extracted range [extStart, extEnd] -
        // so the stems line up in time with what was separated from them. One
        // undoable step via replaceWithNoiseTracks with an unchanged copy of
        // every current channel as the "filtered" data.
        const int64_t total = eng.numSamples ();
        const int64_t startSample = jlimit ((int64_t) 0, total,
            (int64_t) std::llround (extStartSecs * eng.sampleRate ()));
        const int64_t len = jmin ((int64_t) r.data[0].size (), total - startSample);
        if (len <= 0)
            return fail ("The extracted range no longer fits in the file.");

        std::vector<std::vector<float>> filtered ((size_t) eng.numChannels ());
        for (int ch = 0; ch < eng.numChannels (); ++ch)
        {
            filtered[(size_t) ch].resize ((size_t) len);
            if (!eng.readSamples (ch, startSample, len, filtered[(size_t) ch].data ()))
                return fail ("Could not read the current file content.");
        }

        std::vector<std::vector<float>> stems;
        for (auto& t : r.data)
        {
            auto v = std::move (t);
            v.resize ((size_t) len, 0.0f);   // truncate if the tool ran long, pad if short
            stems.push_back (std::move (v));
        }

        if (!eng.replaceWithNoiseTracks ({ startSample, startSample + len }, filtered, stems,
                    "Stem separation (" + juce::String (nTracks) + " tracks appended)"))
            return fail ("Could not append the stem tracks.");

        // Make the freshly added channels visible.
        const int n = eng.numChannels ();
        for (int ch = n - nTracks; ch < n; ++ch)
            if (!isChannelVisible (ch))
                visibleChannels.push_back (ch);
        std::sort (visibleChannels.begin (), visibleChannels.end ());
    }
    else
        return fail ("Unknown stem separation output mode.");

    // Side state: record the marker/saved-selection change so undo/redo restores it.
    Patch& p = d->undo.lastRecorded ();
    p.hasSideState = true;
    p.oldMarkers = std::move (oldMarkers);
    p.newMarkers = d->markers.markers ();
    p.oldSavedSel = std::move (oldSavedSel);
    p.newSavedSel = d->savedSelections;
    if (outputMode == 2)
        commitTimeAnchoredToPatch (oldTA, p);   // overwrite remapped the time-anchored state

    syncActiveDoc ();
    return true;
}

// Message-thread only. Applies a super-resolution result to the active document
// according to outputMode: 1 = create a new document from it, 2 = replace the
// current file's content with it (one undoable step; unlike stem separation
// the result may carry a DIFFERENT sample rate - both SR families emit mono
// 48 kHz regardless of the input - so the patch also records the rate change),
// 3 = append its track after the existing channels over the extracted range
// (one undoable step; the caller must have resampled it to the document's
// rate first).
bool Application::applySuperResolutionResult (sc::MergeResult& r, int outputMode,
                                              double extStartSecs,
                                              const juce::String& suggestedName,
                                              std::string* errOut)
{
    auto fail = [errOut] (const char* msg) { if (errOut != nullptr) *errOut = msg; return false; };

    if (r.channels <= 0 || !std::isfinite (r.rate) || r.rate <= 0.0
        || r.data.empty () || r.data.size () != (size_t) r.channels)
        return fail ("Invalid super resolution result.");

    // Mode 1: a brand-new document at the result's native rate/channel count.
    if (outputMode == 1)
        return createDocFromMerged (r, errOut, suggestedName);

    auto* d = doc ();
    if (!d || !d->engine.isFileLoaded ())
        return fail ("No file is open.");
    auto& eng = d->engine;

    std::vector<Marker> oldMarkers = d->markers.markers ();
    auto oldSavedSel = d->savedSelections;
    TimeAnchoredState oldTA = captureTimeAnchored ();   // overwrite mode remaps it

    if (outputMode == 2)
    {
        // Overwrite: one undoable patch replaces the whole content, including
        // a possible sample-rate change. State anchored inside the extracted
        // range keeps its relative position; everything else is dropped with
        // the old timeline - modelled as two deletions, [0, extStart) and
        // [extStart+newDur, totalOld).
        const double newDur   = (double) r.data[0].size () / r.rate;
        const double totalOld = eng.duration ();

        d->remapForDeletion (0.0, extStartSecs);
        d->remapForDeletion (newDur, totalOld - extStartSecs);

        // r.rate may differ from the document's rate: recordReplace stores it
        // in the patch and undo restores the old one.
        if (!eng.replaceWholeFileUndoable (r.data, "Super resolution", r.rate))
            return fail ("Could not replace the file content.");

        // Keep playhead/selection at their relative position inside the extracted range.
        const double dur = eng.duration ();
        auto W = [&](double t) {
            double u = (t < extStartSecs) ? 0.0 : t - extStartSecs;   // after first deletion
            if (totalOld > extStartSecs && u >= newDur && u <= totalOld - extStartSecs)
                u = newDur;                                          // inside the second deleted range
            else if (u > totalOld - extStartSecs)
                u -= (totalOld - extStartSecs) - newDur;             // after it: shift left
            return std::clamp (u, 0.0, dur);
        };
        playhead  = W (playhead);
        selection = { W (selection.getStart ()), W (selection.getEnd ()) };

        // The new file has exactly the enhanced track; show it.
        visibleChannels.clear ();
        for (int ch = 0; ch < eng.numChannels (); ++ch)
            visibleChannels.push_back (ch);
    }
    else if (outputMode == 3)
    {
        // Append: existing channels stay untouched and the enhanced track
        // becomes a new channel holding its data over the extracted range -
        // so it lines up in time with what was enhanced. The worker resamples
        // to the document's rate for this mode, hence the guard.
        if (std::abs (r.rate - eng.sampleRate ()) > 1e-6 * r.rate)
            return fail ("The result sample rate does not match the open file.");

        const int nTracks = (int) r.data.size ();
        const int64_t total = eng.numSamples ();
        const int64_t startSample = jlimit ((int64_t) 0, total,
            (int64_t) std::llround (extStartSecs * eng.sampleRate ()));
        const int64_t len = jmin ((int64_t) r.data[0].size (), total - startSample);
        if (len <= 0)
            return fail ("The extracted range no longer fits in the file.");

        std::vector<std::vector<float>> filtered ((size_t) eng.numChannels ());
        for (int ch = 0; ch < eng.numChannels (); ++ch)
        {
            filtered[(size_t) ch].resize ((size_t) len);
            if (!eng.readSamples (ch, startSample, len, filtered[(size_t) ch].data ()))
                return fail ("Could not read the current file content.");
        }

        std::vector<std::vector<float>> stems;
        for (auto& t : r.data)
        {
            auto v = std::move (t);
            v.resize ((size_t) len, 0.0f);   // truncate if the tool ran long, pad if short
            stems.push_back (std::move (v));
        }

        if (!eng.replaceWithNoiseTracks ({ startSample, startSample + len }, filtered, stems,
                    "Super resolution (1 track appended)"))
            return fail ("Could not append the enhanced track.");

        // Make the freshly added channels visible.
        const int n = eng.numChannels ();
        for (int ch = n - nTracks; ch < n; ++ch)
            if (!isChannelVisible (ch))
                visibleChannels.push_back (ch);
        std::sort (visibleChannels.begin (), visibleChannels.end ());
    }
    else
        return fail ("Unknown super resolution output mode.");

    // Side state: record the marker/saved-selection change so undo/redo restores it.
    Patch& p = d->undo.lastRecorded ();
    p.hasSideState = true;
    p.oldMarkers = std::move (oldMarkers);
    p.newMarkers = d->markers.markers ();
    p.oldSavedSel = std::move (oldSavedSel);
    p.newSavedSel = d->savedSelections;
    if (outputMode == 2)
        commitTimeAnchoredToPatch (oldTA, p);   // overwrite remapped the time-anchored state

    syncActiveDoc ();
    if (onStateChange) onStateChange ();   // refresh the main window immediately
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
    TimeAnchoredState oldTA = captureTimeAnchored ();   // playhead/sel/spectral/drawings/SCTT

    if (!audioEngine().deleteSamples (selectionSamples (*this), errOut))
        return false;
    selection = { 0, 0 };
    playhead = warpDeletion (playhead, a, b);   // the cursor follows its anchored position

    if (d->undo.depth() > depthBefore)   // a patch was actually recorded
    {
        d->remapForDeletion (a, b);      // markers/savedSel/spectralSel/drawings/SCTT

        Patch& p = d->undo.lastRecorded ();
        p.hasSideState = true;
        p.oldMarkers = std::move (oldMarkers);
        p.newMarkers = d->markers.markers();
        p.oldSavedSel = std::move (oldSavedSel);
        p.newSavedSel = d->savedSelections;
        commitTimeAnchoredToPatch (oldTA, p);
        if (onTimeRangeDeleted)
            onTimeRangeDeleted (d, a, b, p, false);   // records + adjusts FFT snapshots
    }

    syncActiveDoc ();
    return true;
}

// Trim Selection: keep only the selected time range. Everything before and
// after it is deleted in one undoable step; markers, saved selections and
// FFT snapshot slots are adjusted for both removed ranges.
bool Application::trimSelection (std::string* errOut)
{
    if (selection.getLength() <= 0) return true;   // nothing to trim - not an error
    FileDoc* d = doc();
    auto& eng = audioEngine ();
    const double totalSecs = eng.duration();
    auto range = selectionSamples (*this).getIntersectionWith ({ 0, eng.numSamples () });
    if (range.getLength() <= 0 || range.getLength() >= eng.numSamples ()) return true;

    const double aSec = (double) range.getStart() / eng.sampleRate ();
    const double bSec = (double) range.getEnd() / eng.sampleRate ();

    // Non-audio state that this trim will shift / drop. Captured before the
    // edit so undo/redo can restore it (Patch::hasSideState).
    const size_t depthBefore = d->undo.depth();
    std::vector<Marker> oldMarkers = d->markers.markers ();
    auto oldSavedSel = d->savedSelections;
    TimeAnchoredState oldTA = captureTimeAnchored ();   // playhead/sel/spectral/drawings/SCTT

    if (!eng.trimToRange (range, errOut))
        return false;

    // Two deletions: [0, aSec) and [bSec, totalSecs). Adjust in that order so
    // the second range is expressed in post-first-deletion coordinates.
    const double bNew = bSec - aSec;
    const double totalNew = totalSecs - aSec;
    d->remapForDeletion (0.0, aSec);
    d->remapForDeletion (bNew, totalNew);

    selection = { 0.0, eng.duration() };   // the whole trimmed track is now selected
    playhead = warpDeletion (warpDeletion (playhead, 0.0, aSec), bNew, totalNew);

    if (d->undo.depth() > depthBefore)   // a patch was actually recorded
    {
        Patch& p = d->undo.lastRecorded ();
        p.hasSideState = true;
        p.oldMarkers = std::move (oldMarkers);
        p.newMarkers = d->markers.markers();
        p.oldSavedSel = std::move (oldSavedSel);
        p.newSavedSel = d->savedSelections;
        commitTimeAnchoredToPatch (oldTA, p);
        if (onTimeRangeDeleted)
        {
            onTimeRangeDeleted (d, 0.0, aSec, p, false);
            // Second range: keep the original before-state recorded above for
            // any slot touched by both deletions.
            onTimeRangeDeleted (d, bNew, totalNew, p, true);
        }
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

// ------------------------------------------------- Invert / Time Stretch / Filter EQ

namespace {
/** Target range for the whole-file-capable effects: the time selection when
    present, otherwise the entire file. */
juce::Range<int64_t> effectTargetRange (Application& app)
{
    auto& eng = app.audioEngine ();
    const int64_t total = eng.numSamples ();
    if (!app.selection.isEmpty ())
        return selectionSamples (app).getIntersectionWith ({ 0, total });
    return { 0, total };
}
} // namespace

bool Application::invertSelection (std::string* errOut)
{
    auto& eng = audioEngine ();
    if (!eng.isFileLoaded ())
        return true;   // nothing to invert - not an error

    if (hasSpectralSelection ())
        return applyMaskedEffect (*this, "Inverted spectral selection",
                                  [] (float* d, int len) {
                                      for (int i = 0; i < len; ++i)
                                          d[i] = -d[i];
                                  }, errOut);

    const auto range = effectTargetRange (*this);
    if (range.getLength () <= 0) return true;
    applyPerSample (eng, range, [] (float v, int, int) { return -v; },
                    &visibleChans (*this), "Inverted selection");
    return true;
}

bool Application::timeStretch (float percentChange, std::string* errOut)
{
    auto& eng = audioEngine ();
    if (!eng.isFileLoaded ())
        return false;   // the dialog is only reachable with a file loaded
    if (std::abs ((double) percentChange) < 1e-6)
        return true;   // 0 %: no-op, bit-exact

    FileDoc* d = doc ();
    const double rate = eng.sampleRate ();
    const int64_t totalBefore = eng.numSamples ();
    juce::Range<int64_t> range;
    if (!selection.isEmpty ())
        range = selectionSamples (*this).getIntersectionWith ({ 0, totalBefore });
    else
        range = { 0, totalBefore };
    if (range.getLength () <= 0) return true;

    const double aSec = (double) range.getStart () / rate;
    const double oldEndSecs = (double) range.getEnd () / rate;
    const size_t depthBefore = d->undo.depth ();
    std::vector<Marker> oldMarkers = d->markers.markers ();
    auto oldSavedSel = d->savedSelections;
    TimeAnchoredState oldTA = captureTimeAnchored ();   // playhead/sel/spectral/drawings/SCTT

    if (!eng.stretchRange (range, percentChange, errOut))
        return false;

    // The stretched region now occupies [aSec, bNew) on the new timeline.
    const int64_t delta = eng.numSamples () - totalBefore;
    const double bNew = ((double) range.getStart () + (double) range.getLength ()
                         + (double) delta) / rate;

    d->remapForStretch (aSec, oldEndSecs, bNew);   // markers/savedSel/spectralSel/drawings/SCTT
    selection = { aSec, bNew };   // the stretched region stays selected
    playhead = warpStretch (playhead, aSec, oldEndSecs, bNew);

    if (d->undo.depth () > depthBefore)   // a patch was actually recorded
    {
        Patch& p = d->undo.lastRecorded ();
        p.hasSideState = true;
        p.oldMarkers = std::move (oldMarkers);
        p.newMarkers = d->markers.markers ();
        p.oldSavedSel = std::move (oldSavedSel);
        p.newSavedSel = d->savedSelections;
        commitTimeAnchoredToPatch (oldTA, p);
        if (onTimeRangeStretched)
            onTimeRangeStretched (d, aSec, oldEndSecs, bNew, p);
    }

    syncActiveDoc ();
    return true;
}

// ---------------------------------------------------------------------------
// Content Aware Time Scale
// ---------------------------------------------------------------------------

namespace {
// In-memory analysis cap: nch * span floats must fit comfortably (~1.2 GB).
constexpr size_t kCatMaxBytes = (size_t) 1200 * 1024 * 1024;
} // namespace

bool Application::catAnalyzeActiveRange (sc::cat::Analysis& out)
{
    auto& eng = audioEngine ();
    if (!eng.isFileLoaded ()) return false;

    const double rate = eng.sampleRate ();
    const int nch = eng.numChannels ();
    const int64_t total = eng.numSamples ();
    juce::Range<int64_t> range = selection.isEmpty ()
        ? juce::Range<int64_t> { 0, total }
        : selectionSamples (*this).getIntersectionWith ({ 0, total });
    if (range.getLength () <= 0) return false;

    const size_t bytes = (size_t) nch * (size_t) range.getLength () * sizeof (float);
    if (bytes > kCatMaxBytes) return false;   // too large for in-memory analysis

    std::vector<std::vector<float>> data;
    if (!eng.readRange (range.getStart (), range.getLength (), data))
        return false;

    return sc::cat::analyze (data, nch, rate, out);   // false when too short
}

bool Application::contentAwareTimeScale (bool reduceGaps, double percentile,
                                        double speedFactor, std::string* errOut)
{
    auto& eng = audioEngine ();
    if (!eng.isFileLoaded ()) { *errOut = "No file loaded."; return false; }

    FileDoc* d = doc ();
    const double rate = eng.sampleRate ();
    const int nch = eng.numChannels ();
    const int64_t total = eng.numSamples ();

    juce::Range<int64_t> range = selection.isEmpty ()
        ? juce::Range<int64_t> { 0, total }
        : selectionSamples (*this).getIntersectionWith ({ 0, total });
    if (range.getLength () <= 0) return true;   // nothing to do - not an error

    const size_t bytes = (size_t) nch * (size_t) range.getLength () * sizeof (float);
    if (bytes > kCatMaxBytes)
    { *errOut = "The selection is too large for content-aware analysis."; return false; }

    std::vector<std::vector<float>> data;
    if (!eng.readRange (range.getStart (), range.getLength (), data))
    { *errOut = "Could not read the audio data."; return false; }

    sc::cat::Analysis a;
    if (!sc::cat::analyze (data, nch, rate, a))
    { *errOut = "The selection is too short for content-aware time scaling."; return false; }

    std::vector<sc::cat::Edit> edits;
    if (reduceGaps)
    {
        edits = sc::cat::reduceGapsEdits (a, percentile, 3);
        if (edits.empty ())
            return true;   // no gap qualifies - bit-exact no-op
    }
    else
    {
        if (!(speedFactor > 0.0)) { *errOut = "Invalid speed factor."; return false; }
        const double refDur = (double) range.getLength () / rate;
        const int64_t delta = (int64_t) std::llround ((refDur / speedFactor - refDur) * rate);
        if (delta == 0)
            return true;   // factor ~1: bit-exact no-op
        juce::Random rng;   // fresh seed per run: the same region is not always reused
        edits = sc::cat::planTimeScaleEdits (a, delta, rng);
        if (edits.empty ())
            return true;   // no stable material found - bit-exact no-op
    }

    auto newData = sc::cat::applyEdits (data, nch, rate, edits);

    const double aSec = (double) range.getStart () / rate;
    const double oldEndSecs = (double) range.getEnd () / rate;
    const size_t depthBefore = d->undo.depth ();
    std::vector<Marker> oldMarkers = d->markers.markers ();
    auto oldSavedSel = d->savedSelections;
    TimeAnchoredState oldTA = captureTimeAnchored ();   // playhead/sel/spectral/drawings/SCTT

    if (!eng.replaceRangeWithDifferentLength (range, newData, "Content-aware time scale"))
        return false;   // errOut set by the engine when non-null

    const double bNew = ((double) range.getStart () + (double) newData[0].size ()) / rate;

    d->remapForStretch (aSec, oldEndSecs, bNew);   // markers/savedSel/spectralSel/drawings/SCTT
    selection = { aSec, bNew };                    // the edited range stays selected
    playhead = warpStretch (playhead, aSec, oldEndSecs, bNew);

    if (d->undo.depth () > depthBefore)   // a patch was actually recorded
    {
        Patch& p = d->undo.lastRecorded ();
        p.hasSideState = true;
        p.oldMarkers = std::move (oldMarkers);
        p.newMarkers = d->markers.markers ();
        p.oldSavedSel = std::move (oldSavedSel);
        p.newSavedSel = d->savedSelections;
        commitTimeAnchoredToPatch (oldTA, p);
    }

    syncActiveDoc ();
    return true;
}

bool Application::applyFilterEq (const std::vector<sc::eq::EqPoint>& pts,
                                 std::string* errOut)
{
    auto& eng = audioEngine ();
    if (!eng.isFileLoaded ())
        return true;   // nothing to filter - not an error
    if (sc::eq::isFlatCurve (pts))
        return true;   // flat curve: bit-exact no-op, no undo entry

    if (hasSpectralSelection ())
        return applyMaskedEffect (*this, "Filter EQ on spectral selection",
            [rate = eng.sampleRate (), &pts] (float* d, int len) {
                sc::eq::applyEq (d, len, rate, pts);
            }, errOut);

    const auto range = effectTargetRange (*this);
    if (range.getLength () <= 0) return true;
    return eng.eqFilterRange (range, pts, &visibleChans (*this), errOut);
}

bool Application::applyEnvelope (const std::vector<sc::env::EnvPoint>& pts,
                                 std::string* errOut)
{
    auto& eng = audioEngine ();
    if (!eng.isFileLoaded ())
        return true;   // nothing to envelope - not an error
    if (sc::env::isFlatCurve (pts))
        return true;   // flat curve: bit-exact no-op, no undo entry

    if (hasSpectralSelection ())
        return applyMaskedEffect (*this, "Envelope on spectral selection",
            [rate = eng.sampleRate (), &pts] (float* d, int len) {
                sc::env::applyEnvelope (d, len, rate, pts);
            }, errOut);

    const auto range = effectTargetRange (*this);
    if (range.getLength () <= 0) return true;
    return eng.envelopeRange (range, pts, &visibleChans (*this), errOut);
}

// ------------------------------------------------- signal generation (Effects > Generate ...)

namespace {

struct GenPlan
{
    enum class Mode { Spectral, TimeRange, InsertAtPlayhead };
    Mode mode = Mode::InsertAtPlayhead;
    juce::Range<int64_t> range;      // Spectral / TimeRange modes (absolute samples)
    int64_t insertPos = 0;          // InsertAtPlayhead mode
};

/** Resolves where the generated signal goes: an active spectral selection is
    replaced inside its region, else a time selection is replaced, else the
    signal is inserted at the playhead with the given duration. */
static bool resolveGenPlan (Application& app, double durationSecs, GenPlan& plan,
                            std::string* errOut)
{
    auto* d = app.doc();
    if (!d || !app.audioEngine().isFileLoaded())
    {
        if (errOut != nullptr) *errOut = "No file is open.";
        return false;
    }

    AudioEngine& eng = app.audioEngine ();
    if (app.hasSpectralSelection ())
    {
        plan.mode = GenPlan::Mode::Spectral;
        plan.range = spectralSelSamples (app);
        return true;   // a zero-length range is a harmless no-op downstream
    }

    if (!app.selection.isEmpty ())
    {
        plan.mode = GenPlan::Mode::TimeRange;
        plan.range = selectionSamples (app).getIntersectionWith ({ 0, eng.numSamples () });
        return true;
    }

    const double dur = std::isfinite (durationSecs) && durationSecs > 0.0 ? durationSecs : 1.0;
    plan.mode = GenPlan::Mode::InsertAtPlayhead;
    plan.insertPos = std::clamp ((int64_t) (app.playhead * eng.sampleRate ()),
                                 (int64_t) 0, eng.numSamples ());
    return true;
}

static int64_t genInsertLength (AudioEngine& eng, double durationSecs)
{
    const double dur = std::isfinite (durationSecs) && durationSecs > 0.0 ? durationSecs : 1.0;
    return std::max<int64_t> (1, (int64_t) std::llround (dur * eng.sampleRate ()));
}

/** Replaces plan.range with generated data on the visible channels only; the
    other channels keep their current content so the undo patch stays complete. */
static bool replaceRangeWithGen (Application& app, const GenPlan& plan,
                                 std::vector<std::vector<float>>& data,
                                 const juce::String& desc, std::string* errOut)
{
    AudioEngine& eng = app.audioEngine ();
    const int nch = eng.numChannels ();
    const int64_t len = plan.range.getLength ();

    std::vector<std::vector<float>> newData ((size_t) nch);
    for (int ch = 0; ch < nch; ++ch)
    {
        if (app.isChannelVisible (ch))
            newData[ch] = data[(size_t) ch];
        else
        {
            newData[ch].assign ((size_t) len, 0.0f);
            eng.readSamples (ch, plan.range.getStart (), len, newData[ch].data ());
        }
    }

    if (!eng.replaceSamples (plan.range, newData, desc, errOut))
        return false;
    eng.rebuildMipmaps ();   // replaceSamples does not rebuild them
    return true;
}

/** Inserts generated data at the playhead (InsertAtPlayhead mode) and remaps
    every time-anchored state for the insertion; one undoable patch. */
static bool applyGenInsert (Application& app, const GenPlan& plan,
                            std::vector<std::vector<float>>& data,
                            const juce::String& desc, std::string* errOut)
{
    AudioEngine& eng = app.audioEngine ();
    FileDoc* d = app.doc ();
    const size_t depthBefore = d->undo.depth ();
    std::vector<Marker> oldMarkers = d->markers.markers ();
    auto oldSavedSel = d->savedSelections;
    Application::TimeAnchoredState oldTA = app.captureTimeAnchored ();

    if (!eng.insertSamples (plan.insertPos, data, desc, errOut))
        return false;

    const double aSec    = (double) plan.insertPos / eng.sampleRate ();
    const double lenSecs = (double) data[0].size () / eng.sampleRate ();
    d->remapForInsertion (aSec, lenSecs);   // markers/savedSel/spectralSel/drawings/SCTT

    // Content from aSec on shifted right by lenSecs; the live cursor/selection follow.
    app.playhead = warpInsertion (app.playhead, aSec, lenSecs);
    app.selection = { warpInsertion (app.selection.getStart (), aSec, lenSecs),
                      warpInsertion (app.selection.getEnd (),   aSec, lenSecs) };

    if (d->undo.depth () > depthBefore)   // a patch was actually recorded
    {
        Patch& p = d->undo.lastRecorded ();
        p.hasSideState = true;
        p.oldMarkers  = std::move (oldMarkers);
        p.newMarkers  = d->markers.markers ();
        p.oldSavedSel = std::move (oldSavedSel);
        p.newSavedSel = d->savedSelections;
        app.commitTimeAnchoredToPatch (oldTA, p);
        if (app.onTimeRangeInserted)
            app.onTimeRangeInserted (d, aSec, lenSecs, p);   // records + adjusts FFT snapshots
    }

    return true;
}

/** Band-limits the generated data to the spectral selection region and mixes
    it back: inside the band the original content is replaced by the generated
    signal, outside it everything passes through bit-exact. */
static bool applyGenToSpectralSel (Application& app, const GenPlan& plan,
                                   std::vector<std::vector<float>>& data,
                                   const juce::String& desc, std::string* errOut)
{
    auto* d = app.doc();
    AudioEngine& eng = app.audioEngine ();

    // The generators fill every channel identically and the mask is a pure
    // time/frequency region, so one masked copy suffices for all channels.
    std::vector<std::vector<float>> genA;
    if (!SpectralMask::maskBufferToSelection (data, plan.range.getStart (), d->spectralSel,
                                              eng.sampleRate (), genA))
    {
        if (errOut != nullptr) *errOut = "Spectral masking failed.";
        return false;
    }
    const std::vector<float> genMono = genA.empty () ? std::vector<float> {} : genA[0];

    // Same structure as applyMaskedEffect: out = (original - A) + fn(A), with
    // fn replacing the extracted band content by the generated signal.
    auto fn = [&genMono] (float* dst, int len) {
        std::copy (genMono.begin (), genMono.end (), dst);
    };
    return applyMaskedEffect (app, desc, fn, errOut);
}

} // namespace

bool Application::generateSilence (double durationSecs, std::string* errOut)
{
    GenPlan plan;
    if (!resolveGenPlan (*this, durationSecs, plan, errOut)) return false;

    AudioEngine& eng = audioEngine ();

    if (plan.mode == GenPlan::Mode::Spectral)
        return maskedGainEdit (*this, "Generated silence in spectral selection", 0.0f, errOut);

    const int64_t len = plan.mode == GenPlan::Mode::InsertAtPlayhead
                        ? genInsertLength (eng, durationSecs) : plan.range.getLength ();
    if (len <= 0) return true;   // nothing to do

    std::vector<std::vector<float>> data ((size_t) eng.numChannels (),
                                          std::vector<float> ((size_t) len, 0.0f));

    if (plan.mode == GenPlan::Mode::InsertAtPlayhead)
        return applyGenInsert (*this, plan, data, "Generated silence", errOut);
    return replaceRangeWithGen (*this, plan, data, "Generated silence", errOut);
}

bool Application::generateNoise (sc::NoiseType type, float intensity,
                                 double durationSecs, std::string* errOut)
{
    GenPlan plan;
    if (!resolveGenPlan (*this, durationSecs, plan, errOut)) return false;

    AudioEngine& eng = audioEngine ();
    const juce::String typeName = type == sc::NoiseType::White ? "white"
                              : (type == sc::NoiseType::Pink  ? "pink" : "brown");

    if (plan.mode == GenPlan::Mode::Spectral)
    {
        const int64_t len = plan.range.getLength ();
        if (len <= 0) return true;   // nothing to do
        std::vector<std::vector<float>> data;
        sc::generateNoise (type, intensity, eng.numChannels (), len, 0, data);
        return applyGenToSpectralSel (*this, plan, data,
                                      "Generated " + typeName + " noise in spectral selection",
                                      errOut);
    }

    const int64_t len = plan.mode == GenPlan::Mode::InsertAtPlayhead
                        ? genInsertLength (eng, durationSecs) : plan.range.getLength ();
    if (len <= 0) return true;   // nothing to do

    std::vector<std::vector<float>> data;
    sc::generateNoise (type, intensity, eng.numChannels (), len, 0, data);

    if (plan.mode == GenPlan::Mode::InsertAtPlayhead)
        return applyGenInsert (*this, plan, data,
                               "Generated " + typeName + " noise", errOut);
    return replaceRangeWithGen (*this, plan, data, "Generated " + typeName + " noise", errOut);
}

bool Application::generateTone (const std::vector<sc::ToneSpec>& tones, sc::WaveShape shape,
                                double durationSecs, std::string* errOut)
{
    if (tones.empty ())
    {
        if (errOut != nullptr) *errOut = "No tones specified.";
        return false;
    }

    GenPlan plan;
    if (!resolveGenPlan (*this, durationSecs, plan, errOut)) return false;

    AudioEngine& eng = audioEngine ();

    if (plan.mode == GenPlan::Mode::Spectral)
    {
        const int64_t len = plan.range.getLength ();
        if (len <= 0) return true;   // nothing to do
        std::vector<std::vector<float>> data;
        sc::generateTone (tones, shape, eng.sampleRate (),
                          (double) plan.range.getStart () / eng.sampleRate (),
                          eng.numChannels (), len, data);
        return applyGenToSpectralSel (*this, plan, data,
                                      "Generated tone in spectral selection", errOut);
    }

    const int64_t len = plan.mode == GenPlan::Mode::InsertAtPlayhead
                        ? genInsertLength (eng, durationSecs) : plan.range.getLength ();
    if (len <= 0) return true;   // nothing to do

    std::vector<std::vector<float>> data;
    const double t0 = plan.mode == GenPlan::Mode::InsertAtPlayhead
                      ? (double) plan.insertPos / eng.sampleRate ()
                      : (double) plan.range.getStart () / eng.sampleRate ();
    sc::generateTone (tones, shape, eng.sampleRate (), t0,
                      eng.numChannels (), len, data);

    if (plan.mode == GenPlan::Mode::InsertAtPlayhead)
        return applyGenInsert (*this, plan, data, "Generated tone", errOut);
    return replaceRangeWithGen (*this, plan, data, "Generated tone", errOut);
}

bool Application::splitSpectralSelectionConfigured (std::string* errOut)
{
    auto fail = [errOut] (const juce::String& msg) {
        if (errOut != nullptr) *errOut = msg.toStdString ();
        return false;
    };

    auto* d = doc();
    if (!d || d->spectralSel.empty ())
        return fail ("No spectral selection is active.");

    AudioEngine& eng = audioEngine ();
    const int nch0 = eng.numChannels ();
    if (nch0 < 1) return fail ("No audio loaded.");

    // Validate the configuration against the current channel count; pairs are
    // applied in configured order, duplicates of a source are collapsed.
    std::vector<std::pair<int,int>> pairs;
    for (const auto& p : settings.splitConfig)
        if (p.first >= 0 && p.first < nch0 && p.second >= 0 && p.second <= 1023)
            pairs.push_back (p);
    if (pairs.empty ())
        return fail ("No usable channels in the split configuration - open "
                     "Split Spectral Selection Configuration first.");

    const juce::Range<int64_t> range = spectralSelSamples (*this);
    if (range.getLength () <= 0) return true;   // nothing to do

    std::vector<int> srcs;
    for (const auto& p : pairs)
        if (std::find (srcs.begin (), srcs.end (), p.first) == srcs.end ())
            srcs.push_back (p.first);

    std::vector<std::vector<float>> ext, res;
    juce::Range<int64_t> extent;
    if (!SpectralMask::splitSpectralSelection (eng, range, d->spectralSel,
                                               &srcs, ext, res, extent))
        return fail ("Splitting the spectral selection failed.");
    if (extent.getLength () <= 0) return true;   // no frames inside the selection

    // Grow to cover every destination channel (new ones start silent).
    int maxDest = nch0 - 1;
    for (const auto& p : pairs)
        maxDest = std::max (maxDest, p.second);
    const int nOut = std::max (nch0, maxDest + 1);

    const int64_t total = eng.numSamples ();
    std::vector<std::vector<float>> newData ((size_t) nOut,
                                             std::vector<float> ((size_t) total, 0.0f));
    for (int ch = 0; ch < nch0; ++ch)
        if (!eng.readSamples (ch, 0, total, newData[(size_t) ch].data ()))
            return fail ("Failed to read audio.");

    // Apply the pairs in configured order: each input's selected band becomes
    // noise-floor fill and its extracted content is mixed into the destination.
    for (const auto& p : pairs)
    {
        const auto& r = res[(size_t) p.first];
        std::copy (r.begin (), r.end(),
                   newData[(size_t) p.first].begin () + (size_t) extent.getStart());
        const auto& e = ext[(size_t) p.first];
        for (size_t i = 0; i < e.size (); ++i)
            newData[(size_t) p.second][(size_t) extent.getStart() + i] += e[i];
    }

    if (!eng.replaceWholeFileUndoable (newData,
                                       "Split spectral selection (configured)"))
        return fail ("Could not apply the split.");

    // Make any freshly created channels visible (leave other choices alone).
    for (int ch = nch0; ch < eng.numChannels (); ++ch)
        if (!isChannelVisible (ch))
            visibleChannels.push_back (ch);
    std::sort (visibleChannels.begin (), visibleChannels.end());
    syncActiveDoc ();

    return true;
}

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
bool Application::noiseSuppress (const std::string& modelId, const std::string& device,
                                 float attenLimDb, bool df3PostFilter,
                                 std::function<bool(float)> onProgress, std::string* errOut,
                                 bool autoDownload, bool copyNoise)
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

    // When copying the noise to new tracks we need each processed channel's
    // original samples: keep them before the pipeline rewrites `data` in place.
    std::vector<std::vector<float>> origCopy ((size_t) nch);
    if (copyNoise)
        for (int ch : visibleChans (*this))
            if (ch >= 0 && ch < nch)
                origCopy[(size_t) ch] = data[(size_t) ch];

    // The pipeline works on this copy; it is only committed on success.
    const bool ok = (modelId == "deepfilternet2" || modelId == "deepfilternet3")
        ? sc::runDeepFilterPipeline (modelId, device, audioEngine().sampleRate (), nch,
                                     data, attenLimDb, df3PostFilter,
                                     onProgress, &err, &visibleChans (*this))
        : sc::runDenseUnetPipeline (modelId, device, audioEngine().sampleRate (), nch,
                                    data, onProgress, &err, &visibleChans (*this));
    if (!ok)
        return fail (err);

    // Channels the pipeline actually processed: everything else passed through
    // unchanged, so only these have a meaningful noise estimate.
    std::vector<int> affected;
    for (int ch : visibleChans (*this))
        if (ch >= 0 && ch < nch) affected.push_back (ch);

    if (!copyNoise || affected.empty ())
    {
        if (!audioEngine().replaceSamples (samples, data,
                                           "Noise suppressed (" + juce::String (modelId) + ")", &err))
            return fail (err);
        return true;
    }

    // Copy noise on new track: the enhanced signal replaces the selection in
    // place, and one appended channel per processed channel holds only the
    // estimated noise (original - enhanced) over that range. One undoable step
    // covers both the content change and the channel-count change.
    std::vector<std::vector<float>> noiseTracks;
    noiseTracks.reserve (affected.size ());
    for (int ch : affected)
    {
        auto& nt = noiseTracks.emplace_back (std::vector<float> ((size_t) samples.getLength ()));
        const auto& orig = origCopy[(size_t) ch];
        const auto& filt = data[(size_t) ch];
        for (size_t i = 0; i < nt.size (); ++i)
            nt[i] = orig[i] - filt[i];
    }

    if (!audioEngine().replaceWithNoiseTracks (samples, data, noiseTracks,
                "Noise suppressed (" + juce::String (modelId) + ") and copied noise to new track(s)"))
        return fail ("Could not apply the noise suppression.");

    // Make the freshly added channels visible (leave other choices alone).
    const int n = audioEngine().numChannels ();
    for (int ch = nch; ch < n; ++ch)
        if (!isChannelVisible (ch))
            visibleChannels.push_back (ch);
    std::sort (visibleChannels.begin (), visibleChannels.end());
    syncActiveDoc ();

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

    // The output must keep the sample rate and channel count, but its length
    // may differ: scripts like "Make Loop" intentionally shorten the file.
    const int64_t want = samples.getLength ();
    const int64_t got  = newData.empty () ? 0 : (int64_t) newData[0].size ();
    bool bad = std::abs (outRate - rate) > 1e-9 || outCh != nch;
    for (int ch = 0; !bad && ch < nch; ++ch)
        if ((int64_t) newData[(size_t) ch].size () != got) bad = true;

    if (bad)
    {
        cleanup ();
        return fail ("The script's output does not match the input: expected "
                     + juce::String (nch) + " channel(s), " + juce::String ((int64_t) rate)
                     + " Hz and consistent per-channel lengths.");
    }

    if (got <= 0)
    {
        cleanup ();
        return fail ("The script's output is empty.");
    }

    if (!prog (0.9f)) { cleanup (); return false; }   // cancelled before commit - silent

    const juce::String desc = "Python script '" + script.getFileName () + "'";
    bool ok;
    if (got == want)
        ok = audioEngine ().replaceSamples (samples, newData, desc, &err);
    else
    {
        // Length-changing output: capture the non-audio state first so undo
        // can restore it, exactly like a manual deletion does.
        const size_t depthBefore = d->undo.depth ();
        std::vector<Marker> oldMarkers = d->markers.markers ();
        auto oldSavedSel = d->savedSelections;
        TimeAnchoredState oldTA = captureTimeAnchored ();   // playhead/sel/spectral/drawings/SCTT

        ok = audioEngine ().replaceRangeWithDifferentLength (samples, newData, desc, &err);
        if (!ok || d->undo.depth () <= depthBefore)
            ;
        else if (got < want)
        {
            // The tail of the processed range is gone: drop / clip anything that
            // referred to it.
            const double aSec = (double) (samples.getStart () + got) / rate;
            const double bSec = (double) samples.getEnd () / rate;
            d->remapForDeletion (aSec, bSec);

            // The cursor/selection follow their anchored positions; the old
            // selection [start, end) now covers exactly the shortened range.
            playhead  = warpDeletion (playhead, aSec, bSec);
            selection = { warpDeletion (selection.getStart (), aSec, bSec),
                          warpDeletion (selection.getEnd (),   aSec, bSec) };

            Patch& p = d->undo.lastRecorded ();
            p.hasSideState = true;
            p.oldMarkers  = std::move (oldMarkers);
            p.newMarkers  = d->markers.markers ();
            p.oldSavedSel = std::move (oldSavedSel);
            p.newSavedSel = d->savedSelections;
            commitTimeAnchoredToPatch (oldTA, p);
            if (onTimeRangeDeleted)
                onTimeRangeDeleted (d, aSec, bSec, p, false);
        }
        else
        {
            // The processed range grew: content from its old end on shifted
            // right by the delta.
            const double aSec    = (double) samples.getEnd () / rate;
            const double lenSecs = (double) (got - want) / rate;
            d->remapForInsertion (aSec, lenSecs);

            playhead  = warpInsertion (playhead, aSec, lenSecs);
            selection = { warpInsertion (selection.getStart (), aSec, lenSecs),
                          warpInsertion (selection.getEnd (),   aSec, lenSecs) };

            Patch& p = d->undo.lastRecorded ();
            p.hasSideState = true;
            p.oldMarkers  = std::move (oldMarkers);
            p.newMarkers  = d->markers.markers ();
            p.oldSavedSel = std::move (oldSavedSel);
            p.newSavedSel = d->savedSelections;
            commitTimeAnchoredToPatch (oldTA, p);
            if (onTimeRangeInserted)
                onTimeRangeInserted (d, aSec, lenSecs, p);
        }
    }

    if (!ok)
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

    const int chBefore = d->engine.numChannels ();
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
    {
        syncVisibleChannelsToEngine (chBefore);
        syncActiveDoc ();
        d->engine.rebuildMipmaps();   // one rebuild for the whole jump
    }
}

void Application::redoSteps (int n)
{
    FileDoc* d = doc();
    if (!d || n <= 0) return;

    const int chBefore = d->engine.numChannels ();
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
    {
        syncVisibleChannelsToEngine (chBefore);
        syncActiveDoc ();
        d->engine.rebuildMipmaps();
    }
}

// The channel count may have changed by an undo/redo (e.g. reverting a split
// that grew the file from mono to stereo): when it did, show every remaining
// track so the views never draw phantom channels or go blank. When the count
// is unchanged, just drop any stale flags defensively.
void Application::syncVisibleChannelsToEngine (int chBefore)
{
    const int nch = audioEngine ().numChannels ();
    if (nch <= 0) { visibleChannels.clear (); return; }
    if (nch != chBefore)
    {
        visibleChannels.clear ();
        for (int ch = 0; ch < nch; ++ch)
            visibleChannels.push_back (ch);
        return;
    }
    visibleChannels.erase (std::remove_if (visibleChannels.begin (),
                                           visibleChannels.end (),
                                           [nch] (int ch) { return ch < 0 || ch >= nch; }),
                           visibleChannels.end ());
}

// Restores the non-audio state recorded in a patch: markers and named saved
// selections are restored directly; shared UI state (FFT snapshot slots) goes
// through onPatchSideStateRestored because it is owned by MainWindow.
void Application::restorePatchSideState (FileDoc* d, const Patch& p, bool isRedo)
{
    d->markers.restore (isRedo ? p.newMarkers : p.oldMarkers);
    d->savedSelections = isRedo ? p.newSavedSel : p.oldSavedSel;

    if (p.hasTimeAnchored)
    {
        // Playhead and selection are Application members: restore them here so
        // the subsequent syncActiveDoc() pushes the restored values into the
        // document instead of clobbering them.
        playhead  = isRedo ? p.newPlayhead : p.oldPlayhead;
        selection = isRedo ? p.newSel : p.oldSel;

        d->spectralSel   = isRedo ? p.newSpecSel : p.oldSpecSel;
        d->drawings      = isRedo ? p.newDrawings : p.oldDrawings;
        d->sctt.segments = isRedo ? p.newScttSegs : p.oldScttSegs;
    }

    d->metaDirty = true;   // restored state differs from what was last saved
    // Markers/selections persist with the unified metadata on Save - no live sidecar.
    if (onPatchSideStateRestored)
        onPatchSideStateRestored (d, p, isRedo);
}

Application::TimeAnchoredState Application::captureTimeAnchored () const
{
    TimeAnchoredState t;
    t.playhead  = playhead;
    t.selection = selection;

    FileDoc* d = doc ();
    if (d != nullptr)
    {
        t.spectralSel   = d->spectralSel;
        t.drawings      = d->drawings;
        t.scttSegments  = d->sctt.segments;
    }
    return t;
}

void Application::commitTimeAnchoredToPatch (const TimeAnchoredState& before, Patch& p)
{
    FileDoc* d = doc ();

    p.hasTimeAnchored = true;
    p.oldPlayhead  = before.playhead;
    p.oldSel       = before.selection;
    p.oldSpecSel   = before.spectralSel;
    p.oldDrawings  = before.drawings;
    p.oldScttSegs  = before.scttSegments;

    p.newPlayhead  = playhead;
    p.newSel       = selection;
    if (d != nullptr)
    {
        p.newSpecSel   = d->spectralSel;
        p.newDrawings  = d->drawings;
        p.newScttSegs  = d->sctt.segments;
    }
}

void Application::undoStep() { undoSteps (1); }
void Application::redoStep() { redoSteps (1); }

bool Application::audioDeviceReady() const
{
    return audioDeviceIsReady.load();
}

void Application::startAudioInitAsync()
{
    bool expected = false;
    if (!audioInitStarted.compare_exchange_strong (expected, true))
        return;   // already started or done - never spawn a second worker

    audioInitThread = std::thread ([this] {
        audioDevice->initialiseWithDefaultDevices (0, 2); // no input, stereo output
        {
            std::lock_guard<std::mutex> g (audioInitLock);
            audioDeviceIsReady.store (true);
        }
        audioInitCv.notify_all();
        scStart::mark ("audio-device-ready(async)");
    });
}

bool Application::ensureAudioReady (int timeoutMs)
{
    if (audioDeviceIsReady.load())
        return true;

    startAudioInitAsync();   // idempotent: starts the worker if not already running

    std::unique_lock<std::mutex> lk (audioInitLock);
    if (timeoutMs < 0)
    {
        audioInitCv.wait (lk, [this] { return audioDeviceIsReady.load(); });
        return true;
    }
    return audioInitCv.wait_for (lk, std::chrono::milliseconds (timeoutMs),
                                 [this] { return audioDeviceIsReady.load(); });
}

void Application::play()
{
    if (!audioEngine().isFileLoaded()) return;

    // Lazy init: make sure the output device is up before advancing transport,
    // otherwise playback would silently skip ahead while no audio thread runs.
    ensureAudioReady();

    // Self-heal: if no output device is running (initialisation can fail when
    // the default device isn't available yet), try again now.
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
