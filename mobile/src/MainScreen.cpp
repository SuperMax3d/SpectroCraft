#include "MainScreen.h"
#include "LiveAudio.h"
#include "MobileDocument.h"
#include "MobileStyle.h"
#include "Recorder.h"
#include "SafeArea.h"
#include "Telemetry.h"
#include "TimeFormat.h"
#include "TimeViewport.h"
#include "VideoCapture.h"
#include "VideoSync.h"

namespace scm
{
// ------------------------------------------------------------------ meter

LevelMeter::LevelMeter (AppContext& c) : ctx (c)
{
    startTimerHz (30);
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (style::barBackground);
    g.fillRoundedRectangle (r, 3.0f);

    // Issue #79: a level only while recording (the input) or playing (the
    // file); nothing otherwise.
    const bool rec = ctx.recorder.isRecording();
    const auto* d = ctx.document.doc();
    const bool playing = ! rec && d != nullptr && d->engine.isPlaying();
    if (! rec && ! playing)
        return;
    float rms, pk;
    double splOffset = 0.0;
    bool spl = false;
    if (rec)
    {
        rms = ctx.audio.inputRmsDb (0);
        pk = ctx.audio.inputPeakDb (0);
        const auto cal = ctx.currentInputCalibration();
        if (cal.valid()) { spl = true; splOffset = cal::fullScaleDbSpl (cal.fullScalePa); }
    }
    else
    {
        rms = ctx.audio.playbackRmsDb();
        pk = ctx.audio.playbackPeakDb();
        const auto cal = ctx.document.calibration();
        if (cal.isCalibratedFor (0) && cal.unit == ScaleCalibration::Unit::Pascal) { spl = true; splOffset = cal.levelOffsetDb (0); }
    }
    const auto toX = [&r] (float db) { return r.getX() + r.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 80.0f) / 80.0f); };
    const auto colour = pk > -1.0f ? style::record : pk > -12.0f ? style::warning : style::ok;
    g.setColour (colour.withAlpha (0.85f));
    g.fillRoundedRectangle (r.withRight (toX (rms)), 3.0f);
    g.setColour (juce::Colours::white);
    g.fillRect (toX (pk) - 1.0f, r.getY(), 2.0f, r.getHeight());
    juce::String text = rms > -150.0f ? juce::String (rms, 1) + " dBFS" : juce::String ("-- dBFS");
    if (spl && rms > -150.0f)
        text = juce::String (rms + splOffset, 1) + " dB-SPL";
    g.setFont (style::font (12.0f, true));
    g.setColour (style::text);
    g.drawText (text, r.reduced (4.0f, 0.0f), juce::Justification::centredRight);
}

// ------------------------------------------------------------------ screen

MainScreen::MainScreen (AppContext& c)
    : ctx (c), waveform (c.document, c.view), spectrogram (c.document, c.view), meter (c)
{
    setOpaque (true);

    title.setFont (style::font (17.0f, true));
    addAndMakeVisible (title);
    layoutW.onClick = [this] { setLayout (Layout::Waveform); };
    layoutS.onClick = [this] { setLayout (Layout::Spectrogram); };
    layoutWS.onClick = [this] { setLayout (Layout::Both); };
    saveButton.onClick = [this] { saveDocument(); };
    for (auto* b : { &layoutW, &layoutS, &layoutWS, &saveButton }) addAndMakeVisible (b);
    menuButton.onClick = [this] { showMainMenu(); };
    addChildComponent (menuButton);

    // tablet top bar (applyPanels shows them in the tablet form only)
    panelsButton.onClick = [this] { togglePanels(); };
    undoButton.onClick = [this] { runCommand (screen::Command::Undo); };
    redoButton.onClick = [this] { runCommand (screen::Command::Redo); };
    for (auto* b : { &panelsButton, &undoButton, &redoButton }) addChildComponent (b);

    // tablet splitters: drag to share the room, double tap for the default
    panelsSplitter.onDrag = [this] (int pos) { screen::dragPanelsSplitter (tablet, safeBounds(), form, pos); resized(); };
    groupSplitter.onDrag = [this] (int pos) { screen::dragGroupSplitter (tablet, safeBounds(), form, pos); resized(); };
    panelsSplitter.onDragEnd = groupSplitter.onDragEnd = [this] { saveTabletState(); };
    panelsSplitter.onDoubleTap = [this]
    {
        if (form == screen::Form::TabletLandscape) tablet.landscapeColumn = screen::TabletState::kDefaultLandscapeColumn;
        else tablet.portraitPanels = screen::TabletState::kDefaultPortraitPanels;
        saveTabletState();
        resized();
    };
    groupSplitter.onDoubleTap = [this]
    {
        if (form == screen::Form::TabletLandscape) tablet.landscapeSplit = screen::TabletState::kDefaultLandscapeSplit;
        else tablet.portraitSplit = screen::TabletState::kDefaultPortraitSplit;
        saveTabletState();
        resized();
    };
    addChildComponent (panelsSplitter);
    addChildComponent (groupSplitter);

    // a hardware keyboard's shortcuts reach the screen (keyPressed)
    setWantsKeyboardFocus (true);

    addAndMakeVisible (waveform);
    addAndMakeVisible (spectrogram);

    recordButton.setColour (juce::TextButton::buttonColourId, style::record.darker (0.3f));
    recordButton.onClick = [this] { toggleRecord(); };
    playButton.onClick = [this] { togglePlay(); };
    stopButton.onClick = [this] { stopPlayback(); };
    videoButton.setClickingTogglesState (true);
    videoButton.onClick = [this] { toggleVideo(); };
    modeBox.addItem ("Unlimited", 1);
    modeBox.addItem ("Loop 10 s", 10);
    modeBox.addItem ("Loop 30 s", 30);
    modeBox.addItem ("Loop 1 min", 60);
    modeBox.addItem ("Loop 5 min", 300);
    modeBox.addItem ("Loop 15 min", 900);
    modeBox.setSelectedId (ctx.props.getIntValue ("recordMode", 1), juce::dontSendNotification);
    modeBox.onChange = [this] { ctx.props.setValue ("recordMode", modeBox.getSelectedId()); };
    timeLabel.setFont (style::font (15.0f, true));
    timeLabel.setJustificationType (juce::Justification::centred);
    for (auto* comp : std::initializer_list<juce::Component*> { &recordButton, &playButton, &stopButton,
                                                                &videoButton, &modeBox, &timeLabel, &meter })
        addAndMakeVisible (comp);
    addChildComponent (videoPane);
    videoPane.setCollapsed (ctx.props.getBoolValue ("videoCollapsed", false));
    videoPane.onCollapsedChanged = [this] (bool hidden) { ctx.props.setValue ("videoCollapsed", hidden); };
    videoPane.onLayoutChanged = [this] { resized(); };

    filesPane = std::make_unique<FilesPane> (ctx);
    analysisPane = std::make_unique<AnalysisPane> (ctx.document, ctx.view);
    generatorPane = std::make_unique<GeneratorPane> (ctx);
    calibrationPane = std::make_unique<CalibrationPane> (ctx);
    inputPane = std::make_unique<InputPane> (ctx);
    editPane = std::make_unique<EditPane> (ctx);
    soundMapPane = std::make_unique<SoundMapPane> (ctx);
    soundMapPane->startTake = [this] (const juce::File& f)
    {
        if (ctx.recorder.isRecording()) return false;
        // the map's takes run on their own: keep the edits of an open file
        // (an unsaved take keeps its recovery copy)
        if (ctx.document.hasUnsavedChanges() && ctx.document.file() != juce::File())
        {
            juce::String err;
            ctx.document.save (err);
        }
        return startRecording (f, false);
    };
    soundMapPane->stopTake = [this] { return stopRecording(); };
    soundMapPane->useCamera = [this] (bool on, std::function<void (juce::String)> done) { setMapCamera (on, std::move (done)); };
    soundMapPane->openTake = [this] (const juce::File& f)
    {
        if (ctx.recorder.isRecording()) { showMessage ("Sound map", "Stop the recording first."); return; }
        juce::String err;
        if (ctx.document.hasUnsavedChanges() && ctx.document.file() != juce::File()) ctx.document.save (err);
        if (! ctx.document.open (f, err)) { showMessage ("Sound map", err); return; }
        if (filesPane) filesPane->refresh();
        selectTab (screen::Analysis);
    };
    inputPane->onInputChanged = [this] { calibrationPane->refresh(); };
    editPane->onChannelChanged = [this] (int ch) { waveform.setChannel (ch); spectrogram.setChannel (ch); };
    editPane->onLogFrequencyChanged = [this] (bool log) { spectrogram.setLogFrequency (log); };
    spectrogram.onFrequencyScaleChosen = [this] (double lo, double hi)
    {
        // kept for the next files and the next launches, until it is set again
        ctx.props.setValue ("spectroMinHz", lo);
        ctx.props.setValue ("spectroMaxHz", hi >= ctx.document.sampleRate() * 0.5 - 0.5 ? 0.0 : hi);
    };
    editPane->onDisplayChanged = [this] { ctx.document.spectral().clearCache(); spectrogram.contentChanged(); };
    editPane->onZoomToSelection = [this] { zoomToSelection(); };
    editPane->onOpenFile = [this] (const juce::File& f)
    {
        juce::String err;
        if (ctx.document.hasUnsavedChanges() && ctx.document.file() != juce::File())
            ctx.document.save (err);   // keep the edits of the file we leave
        if (! ctx.document.open (f, err)) { showMessage ("Crop", err); return; }
        if (filesPane) filesPane->refresh();
        showMessage ("Crop", "The selection is now a new recording: " + f.getFileNameWithoutExtension() + ".");
    };

    const char* names[] = { "Files", "Analyse", "Generate", "Calibrate", "Input", "Edit", "Map" };
    for (int t = 0; t < screen::NumTabs; ++t)
    {
        auto* b = tabButtons.add (new juce::TextButton (names[t]));
        b->setClickingTogglesState (false);
        b->onClick = [this, t] { selectTab (t); };
        addAndMakeVisible (b);
    }

    ctx.document.addChangeListener (this);
    ctx.document.onAboutToReplace = [this]
    {
        ctx.audio.setPlaybackSource (nullptr);
        spectrogram.pauseRendering();
        analysisPane->pauseAnalysis();
    };
    ctx.document.onReplaced = [this] { documentReplaced(); };

    ctx.recorder.onUpdate = [this]
    {
        ctx.view.setDuration (ctx.document.duration(), true);
        const bool wrapped = ctx.recorder.wrapped();
        waveform.setWriteHead (ctx.recorder.writeHeadSeconds(), wrapped);
        spectrogram.setWriteHead (ctx.recorder.writeHeadSeconds(), wrapped);
        spectrogram.contentChanged();
        waveform.contentChanged();
        analysisPane->contentChanged();
        updateTransport();
    };
    ctx.recorder.onAutoStop = [this] (const juce::String& msg)
    {
        telemetry::capture ("recording_memory_limit", { { "minutes", juce::roundToInt (ctx.recorder.maxSeconds() / 60.0) } });
        stopRecording();
        showMessage ("Recording", msg);
    };

    loadTabletState();
    selectTab (ctx.props.getIntValue ("tab", screen::Analysis));
    setLayout ((Layout) ctx.props.getIntValue ("layout", 0));
    updateTransport();
    refreshVideo();
    startTimerHz (30);

    // A take left unsaved when the app was closed comes back (issue #79).
    juce::Component::SafePointer<MainScreen> safe (this);
    juce::MessageManager::callAsync ([safe]
    {
        if (safe == nullptr || safe->ctx.document.hasAudio() || ! safe->ctx.document.recoverTake()) return;
        showMessage ("Recording", safe->ctx.document.displayName()
                                    + " was not saved when the app closed: it is open again. Save it to keep it.");
    });
}

MainScreen::~MainScreen()
{
    // the pane first, while the rest of the screen is whole (it releases its camera through us)
    if (soundMapPane != nullptr)
    {
        soundMapPane->useCamera = nullptr;
        soundMapPane->startTake = nullptr;
        soundMapPane->stopTake = nullptr;
        soundMapPane->openTake = nullptr;
        removeChildComponent (soundMapPane.get());
        soundMapPane.reset();
    }
    if (mapCamera && ! ctx.video.isRecording()) ctx.video.close();
    mapCamera = false;
    stopTimer();
    ctx.document.removeChangeListener (this);
    ctx.document.onAboutToReplace = nullptr;
    ctx.document.onReplaced = nullptr;
    ctx.recorder.onUpdate = nullptr;
    ctx.recorder.onAutoStop = nullptr;
    ctx.audio.setPlaybackSource (nullptr);
    videoPane.onLayoutChanged = nullptr;
    videoPane.setLive (nullptr);
}

void MainScreen::audioReady (const juce::String& error)
{
    if (error.isNotEmpty()) showMessage ("Microphone", error);
    inputPane->refresh();
    calibrationPane->refresh();
    updateTransport();
}

void MainScreen::resumed()
{
    if (soundMapPane) soundMapPane->resumed();
}

void MainScreen::suspended()
{
    if (soundMapPane) soundMapPane->suspended();   // releases the Map's camera
    // an unsaved take edited since it stopped: its recovery copy follows
    if (ctx.document.isUnsavedTake() && ctx.document.canUndo() && ! ctx.recorder.isRecording())
        ctx.document.writeRecovery();
    // Recording continues in the background (UIBackgroundModes audio); the
    // generator and playback stop.
    if (! ctx.recorder.isRecording())
    {
        stopPlayback();
        ctx.audio.generator().setEnabled (false);
        if (videoButton.getToggleState())   // the camera is released too
        {
            videoButton.setToggleState (false, juce::dontSendNotification);
            toggleVideo();
            ctx.video.close (true);   // now: timers wait in the background
        }
    }
}

void MainScreen::documentReplaced()
{
    spectrogram.resumeRendering();
    analysisPane->resumeAnalysis();
    if (auto* d = ctx.document.doc())
        ctx.audio.setPlaybackSource (&d->engine);
    ctx.view.setDuration (ctx.document.duration(), ctx.recorder.isRecording());
    if (! ctx.recorder.isRecording()) ctx.view.showAll();
    {
        // the scale last set on purpose (0 = up to the file's Nyquist frequency)
        const double nyquist = ctx.document.sampleRate() * 0.5, hi = ctx.props.getDoubleValue ("spectroMaxHz", 0.0);
        spectrogram.setFrequencyRange (ctx.props.getDoubleValue ("spectroMinHz", 20.0), hi > 0.0 ? std::min (hi, nyquist) : nyquist);
    }
    waveform.setChannel (0);
    spectrogram.setChannel (0);
    updateTransport();
    if (! ctx.recorder.isRecording()) reviewMovie = true;   // an opened file shows its own movie
    refreshVideo();
    resized();
}

void MainScreen::changeListenerCallback (juce::ChangeBroadcaster*)
{
    const bool dirty = ctx.document.hasUnsavedChanges();
    title.setText (ctx.document.displayName() + (dirty ? " *" : ""), juce::dontSendNotification);
    if (! ctx.recorder.isRecording())
    {
        ctx.view.setDuration (ctx.document.duration(), false);
        spectrogram.contentChanged();
        waveform.contentChanged();
    }
    updateTransport();
    refreshVideo();   // the take's movie is linked when it is saved
}

void MainScreen::timerCallback()
{
    // The safe area can change after the first layout (see SafeArea.h).
    if (safearea::insetsFor (*this) != appliedInsets)
        resized();

    // A hardware keyboard's keys go to the focused component and up to its
    // parents: keep the focus inside the screen when nothing holds it (a text
    // field keeps it while it is edited). Not over a menu or a message box,
    // nor when the app is in the background: on the desktop that would raise
    // the window.
    if (juce::Component::getCurrentlyFocusedComponent() == nullptr && isShowing()
        && juce::Component::getNumCurrentlyModalComponents() == 0 && juce::Process::isForegroundProcess())
        grabKeyboardFocus();

    // Follow the playhead while playing. The engine stops by itself at the end
    // of the file; the playhead then stays there and the next Play starts over.
    auto* d = ctx.document.doc();
    const bool playing = d != nullptr && d->engine.isPlaying();
    if (playing || wasPlaying)
    {
        const double t = d != nullptr ? d->engine.position() : 0.0;
        ctx.document.followPlayback (t);
        if (playing && t > ctx.view.end()) ctx.view.pan (ctx.view.span() * 0.9);
        waveform.repaint();
        spectrogram.repaint();
        updateTransport();
    }
    wasPlaying = playing;

    if (videoPane.isVisible())
        videoPane.sync (ctx.document.playhead(), playing);
}

void MainScreen::toggleRecord()
{
    if (ctx.recorder.isRecording()) { stopRecording(); return; }
    // Issue #79: nothing is saved without Save. A take or edits not saved
    // yet: Save / Discard / Cancel before the new take replaces them.
    juce::Component::SafePointer<MainScreen> safe (this);
    filesPane->confirmDiscard ([safe]
    {
        if (safe != nullptr && ! safe->ctx.recorder.isRecording()) safe->startRecording();
    });
}

bool MainScreen::startRecording (const juce::File& saveTo, bool withVideoIfOn)
{
    stopPlayback();
    // a map take is always one unlimited take of the map's length (not the REC mode box's loop)
    const int mode = saveTo != juce::File() ? 1 : modeBox.getSelectedId();
    ctx.recorder.setMode (mode == 1 ? Recorder::Mode::Unlimited : Recorder::Mode::Loop, (double) mode);
    juce::String err;
    const auto cal = ctx.currentInputCalibration();
    if (! ctx.recorder.start (cal.toScale(), err, saveTo))
    {
        telemetry::reportError ("recording: could not start");
        showMessage ("Recording", err);
        return false;
    }
    // the input's compensation (issue #80) goes with its recordings, as its calibration does
    if (const auto comp = miccomp::forInput (ctx.inputKey()); comp.valid())
        ctx.document.setMicCompensation (comp);
    const bool withVideo = withVideoIfOn && videoButton.getToggleState() && ctx.video.isOpen() && ! mapCamera;
    if (withVideo)
        ctx.video.start (VideoCapture::movieFileFor (ctx.recorder.targetFile()));
    reviewMovie = false;
    refreshVideo();
    telemetry::capture ("recording_started", { { "mode", mode == 1 ? "unlimited" : "loop" },
                                               { "loop_s", mode == 1 ? 0 : mode },
                                               { "channels", ctx.audio.numInputs() },
                                               { "sample_rate", (int) ctx.audio.sampleRate() },
                                               { "calibrated", cal.valid() },
                                               { "video", withVideo } });
    ctx.view.setMinimumVisible (mode == 1 ? 10.0 : (double) mode);
    ctx.view.setRange (0.0, mode == 1 ? 10.0 : (double) mode);
    analysisPane->setLive (true);
    updateTransport();
    return true;
}

juce::File MainScreen::stopRecording()
{
    juce::File movie;
    if (ctx.video.isRecording()) movie = ctx.video.stop();
    juce::String err;
    const auto saved = ctx.recorder.stop (err, [this, movie]
    {
        if (movie != juce::File())
        {
            // the file's first frame: a wrapped loop dropped its start
            const double offset = (double) (ctx.video.firstFrameTime().toMilliseconds()
                                            - ctx.recorder.fileStartTime().toMilliseconds()) / 1000.0;
            ctx.document.linkVideo (movie, offset);
        }
    });
    waveform.setWriteHead (0.0, false);
    spectrogram.setWriteHead (0.0, false);
    analysisPane->setLive (false);
    ctx.view.setMinimumVisible (0.0);
    ctx.view.setDuration (ctx.document.duration(), false);
    ctx.view.showAll();
    telemetry::capture ("recording_stopped", { { "duration_s", juce::roundToInt (ctx.document.duration()) },
                                               { "mode", ctx.recorder.mode() == Recorder::Mode::Loop ? "loop" : "unlimited" },
                                               { "saved", saved != juce::File() },
                                               { "video", movie != juce::File() } });
    if (saved == juce::File() && err.isNotEmpty())
    {
        telemetry::reportError ("recording: the take could not be saved");
        showMessage ("Recording", err);
    }
    if (! videoButton.getToggleState() && ctx.video.isOpen() && ! mapCamera)
        ctx.video.close();   // Video was turned off during the take: the filming ends with it
                             // (not the Map tab's camera: it takes the picture after the take)
    if (filesPane) filesPane->refresh();
    updateTransport();
    refreshVideo();
    if (saved != juce::File() && err.isNotEmpty())
        showMessage ("Recording", err);   // saved elsewhere than planned
    return saved;
}

void MainScreen::togglePlay()
{
    auto* d = ctx.document.doc();
    if (d == nullptr || ! d->engine.isFileLoaded() || ctx.recorder.isRecording()) return;
    auto& eng = d->engine;
    if (eng.isPlaying()) { eng.pause(); updateTransport(); return; }
    const auto sel = ctx.document.selection();
    if (! sel.isEmpty())
    {
        eng.setLoopRange (sel);
        eng.setLooping (false);
        if (ctx.document.playhead() < sel.getStart() || ctx.document.playhead() >= sel.getEnd())
            eng.setPosition (sel.getStart());
    }
    else if (ctx.document.playhead() >= ctx.document.duration() - 1e-3)
        eng.setPosition (0.0);
    eng.play();
    reviewMovie = true;
    refreshVideo();
    updateTransport();
}

void MainScreen::stopPlayback()
{
    if (auto* d = ctx.document.doc())
    {
        d->engine.stop();
        const auto sel = ctx.document.selection();
        ctx.document.setPlayhead (sel.isEmpty() ? 0.0 : sel.getStart(), false);   // not a placement by hand
    }
    reviewMovie = false;
    refreshVideo();
    updateTransport();
}

void MainScreen::toggleVideo()
{
    if (videoButton.getToggleState())
    {
        reviewMovie = false;   // the camera's picture, to frame the next take
        ctx.video.open ([this] (juce::String err)
        {
            telemetry::capture ("video_enabled", { { "ok", err.isEmpty() } });
            if (err.isNotEmpty())
            {
                telemetry::reportError ("video: the camera did not open");
                videoButton.setToggleState (false, juce::dontSendNotification);
                showMessage ("Video", err);
            }
            refreshVideo();   // the preview, unless the Map tab shows it
        });
    }
    else if (! ctx.video.isRecording())
    {
        // The preview goes now; the camera stops a moment later (VideoCapture::close).
        videoPane.setLive (nullptr);
        if (! mapCamera) ctx.video.close();   // the Map tab keeps the camera it uses
    }
    refreshVideo();
}

void MainScreen::refreshVideo()
{
    const bool rec = ctx.recorder.isRecording();
    const auto movie = ctx.document.linkedVideo();
    videoPane.setMovie (! rec && movie.existsAsFile() ? movie : juce::File(), ctx.document.linkedVideoOffset());
    // the camera's preview is the Map tab's while it uses the camera
    videoPane.setLive (videoButton.getToggleState() && ctx.video.isOpen() && ! mapCamera ? ctx.video.preview() : nullptr);
    videoPane.setPreferMovie (reviewMovie);
    videoPane.setFilming (ctx.video.isRecording());
    const bool show = videoPane.hasContent() && ! mapFullScreen();
    if (show != videoPane.isVisible())
    {
        videoPane.setVisible (show);
        resized();
    }
}

void MainScreen::setMapCamera (bool on, std::function<void (juce::String)> done)
{
    if (on)
    {
        mapCamera = true;
        refreshVideo();   // the Map tab shows the preview now
        juce::Component::SafePointer<MainScreen> safe (this);
        ctx.video.open ([safe, done] (juce::String err)
        {
            if (safe == nullptr) return;
            if (err.isEmpty() && ! safe->mapCamera)   // released while it opened (the Video button kept it)
            {
                if (! safe->videoButton.getToggleState() && ! safe->ctx.video.isRecording()) safe->ctx.video.close();
                err = "Cancelled.";
            }
            if (done) done (err);
        });
        return;
    }
    mapCamera = false;
    // The Video button keeps the camera (an open still under way answers it);
    // otherwise it goes.
    if (! videoButton.getToggleState() && ! ctx.video.isRecording())
        ctx.video.close();
    refreshVideo();   // the Video button's preview comes back
    if (done) done ({});
}

void MainScreen::updateTransport()
{
    const bool rec = ctx.recorder.isRecording();
    const bool audio = ctx.document.hasAudio();
    const bool playing = audio && ctx.document.doc()->engine.isPlaying();
    recordButton.setButtonText (rec ? "STOP REC" : "REC");
    recordButton.setColour (juce::TextButton::buttonColourId, rec ? style::record : style::record.darker (0.5f));
    playButton.setButtonText (playing ? "Pause" : "Play");
    playButton.setEnabled (audio && ! rec);
    stopButton.setEnabled (audio && ! rec);
    modeBox.setEnabled (! rec);
    saveButton.setEnabled (audio && ! rec);
    undoButton.setEnabled (audio && ! rec && ctx.document.canUndo());
    redoButton.setEnabled (audio && ! rec && ctx.document.canRedo());
    for (auto* b : tabButtons) b->setEnabled (true);

    juce::String t;
    if (rec)
    {
        t = "REC " + formatFooterTime (ctx.recorder.elapsed());
        if (ctx.recorder.mode() == Recorder::Mode::Loop)
            t << "  (keeps " << formatFooterTime (ctx.recorder.loopSeconds()) << ")";
        else
            t << "  / max " << juce::String (ctx.recorder.maxSeconds() / 60.0, 0) << " min";
    }
    else if (audio)
        t = formatFooterTime (ctx.document.playhead()) + " / " + formatFooterTime (ctx.document.duration());
    timeLabel.setText (t, juce::dontSendNotification);
    timeLabel.setColour (juce::Label::textColourId, rec ? style::record.brighter (0.3f) : style::text);
}

void MainScreen::zoomToSelection()
{
    const auto s = ctx.document.selection();
    if (! s.isEmpty()) ctx.view.setRange (s.getStart(), s.getEnd());
}

juce::Component* MainScreen::tabContent (int t)
{
    switch (t)
    {
        case screen::Files:     return filesPane.get();
        case screen::Analysis:  return analysisPane.get();
        case screen::Generator: return generatorPane.get();
        case screen::Calibrate: return calibrationPane.get();
        case screen::Input:     return inputPane.get();
        case screen::Edit:      return editPane.get();
        case screen::Map:       return soundMapPane.get();
        default:                return nullptr;
    }
}

bool MainScreen::panelShown (int t) const
{
    if (screen::isTablet (form))
    {
        if (mapFullScreen()) return t == screen::Map;   // the other group is under the map
        return tablet.panelsVisible && tablet.tab[screen::groupOf (t)] == t;
    }
    return panelOpen && t == currentTab;
}

void MainScreen::applyPanels()
{
    const bool isTablet = screen::isTablet (form);
    for (int i = 0; i < screen::NumTabs; ++i)
    {
        const bool shown = panelShown (i);
        tabButtons[i]->setToggleState (shown, juce::dontSendNotification);
        tabButtons[i]->setVisible (! isTablet || tablet.panelsVisible);
        if (auto* c = tabContent (i))
        {
            if (shown && c->getParentComponent() != this) addAndMakeVisible (c);
            else if (! shown && c->getParentComponent() == this) removeChildComponent (c);
        }
    }
    for (auto* b : { &panelsButton, &undoButton, &redoButton }) b->setVisible (isTablet);
    // phone: the menu holds the views, the spectrogram's options and Save (issue #79)
    for (auto* b : { &layoutW, &layoutS, &layoutWS, &saveButton }) b->setVisible (isTablet);
    menuButton.setVisible (! isTablet);
    editPane->showSpectrogramOptions (isTablet);
    panelsButton.setToggleState (tablet.panelsVisible, juce::dontSendNotification);
    panelsSplitter.setVisible (isTablet && tablet.panelsVisible && ! mapFullScreen());
    groupSplitter.setVisible (isTablet && tablet.panelsVisible && ! mapFullScreen());
    if (panelShown (screen::Input)) inputPane->refresh();
}

void MainScreen::selectTab (int t)
{
    t = juce::jlimit (0, screen::NumTabs - 1, t);
    if (screen::isTablet (form))
    {
        screen::tapTab (tablet, t);   // each group keeps a panel open
        saveTabletState();
    }
    else
    {
        if (t == currentTab && panelOpen && getWidth() > 0) panelOpen = false;   // tap again: hide the panel
        else panelOpen = true;
        currentTab = t;
        ctx.props.setValue ("tab", t);
    }
    applyPanels();
    resized();
}

void MainScreen::showTab (int t)
{
    t = juce::jlimit (0, screen::NumTabs - 1, t);
    currentTab = t;
    panelOpen = true;
    screen::tapTab (tablet, t);
    applyPanels();
    resized();
}

void MainScreen::showMainMenu()
{
    juce::PopupMenu m, range;
    m.addItem (1, "Waveform", true, layout == Layout::Waveform);
    m.addItem (2, "Spectrogram", true, layout == Layout::Spectrogram);
    m.addItem (3, "Waveform and Spectrogram", true, layout == Layout::Both);
    m.addSeparator();
    const bool log = editPane->logFrequency();
    m.addItem (4, "Spectrogram Log", true, log);
    m.addItem (5, "Spectrogram Linear", true, ! log);
    m.addSeparator();
    for (int db : { 60, 90, 120, 150 })
        range.addItem (100 + db, juce::String (db) + " dB", true, editPane->displayRangeDb() == db);
    m.addSubMenu ("Spectrogram Range", range);
    m.addSeparator();
    const bool canSave = ctx.document.hasAudio() && ! ctx.recorder.isRecording();
    m.addItem (6, "Save", canSave);
    m.addItem (7, "Save As...", canSave);
    juce::Component::SafePointer<MainScreen> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
        if (r == 1) safe->setLayout (Layout::Waveform);
        else if (r == 2) safe->setLayout (Layout::Spectrogram);
        else if (r == 3) safe->setLayout (Layout::Both);
        else if (r == 4 || r == 5) safe->editPane->setLogFrequency (r == 4);
        else if (r == 6) safe->saveDocument();
        else if (r == 7) safe->saveDocumentAs();
        else if (r > 100) safe->editPane->setDisplayRangeDb (r - 100);
    });
}

void MainScreen::saveDocument()
{
    juce::String err;
    if (! ctx.document.save (err)) showMessage ("Save", err);
    if (filesPane) filesPane->refresh();
}

void MainScreen::saveDocumentAs()
{
    auto* w = new juce::AlertWindow ("Save As", "Name of the recording (saved in the recordings folder):",
                                     juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("name", ctx.document.displayName());
    w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainScreen> safe (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w] (int r)
    {
        if (safe == nullptr || r != 1) return;
        auto name = juce::File::createLegalFileName (w->getTextEditorContents ("name").trim());
        if (name.endsWithIgnoreCase (".wav")) name = name.dropLastCharacters (4);
        if (name.isEmpty()) return;
        const auto f = MobileDocument::recordingsFolder().getChildFile (name + ".wav");
        const auto write = [safe, f]
        {
            if (safe == nullptr) return;
            juce::String err;
            if (! safe->ctx.document.saveAs (f, err)) showMessage ("Save As", err);
            if (safe->filesPane) safe->filesPane->refresh();
        };
        if (f.existsAsFile() && f != safe->ctx.document.file())
        {
            auto options = juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::QuestionIcon,
                "Save As", f.getFileName() + " already exists. Replace it?", "Replace", "Cancel");
            juce::AlertWindow::showAsync (options, [write] (int ok) { if (ok == 1) write(); });
        }
        else
            write();
    }), true);
}

void MainScreen::togglePanels()
{
    if (screen::isTablet (form))
    {
        tablet.panelsVisible = ! tablet.panelsVisible;
        saveTabletState();
        telemetry::capture ("tablet_panels", { { "visible", tablet.panelsVisible } });
    }
    else
        panelOpen = ! panelOpen;
    applyPanels();
    resized();
}

void MainScreen::loadTabletState()
{
    auto& p = ctx.props;
    for (int g = 0; g < 2; ++g)
    {
        const int t = p.getIntValue ("tabletTab" + juce::String (g), tablet.tab[g]);
        if (juce::isPositiveAndBelow (t, (int) screen::NumTabs) && screen::groupOf (t) == g) tablet.tab[g] = t;
    }
    tablet.panelsVisible = p.getBoolValue ("tabletPanels", true);
    const auto fraction = [&p] (const char* key, double def)
    {
        const double v = p.getDoubleValue (key, def);
        return v > 0.0 && v < 1.0 ? v : def;
    };
    tablet.landscapeColumn = fraction ("tabletLandscapeColumn", screen::TabletState::kDefaultLandscapeColumn);
    tablet.landscapeSplit = fraction ("tabletLandscapeSplit", screen::TabletState::kDefaultLandscapeSplit);
    tablet.portraitPanels = fraction ("tabletPortraitPanels", screen::TabletState::kDefaultPortraitPanels);
    tablet.portraitSplit = fraction ("tabletPortraitSplit", screen::TabletState::kDefaultPortraitSplit);
}

void MainScreen::saveTabletState()
{
    auto& p = ctx.props;
    p.setValue ("tabletTab0", tablet.tab[0]);
    p.setValue ("tabletTab1", tablet.tab[1]);
    p.setValue ("tabletPanels", tablet.panelsVisible);
    p.setValue ("tabletLandscapeColumn", tablet.landscapeColumn);
    p.setValue ("tabletLandscapeSplit", tablet.landscapeSplit);
    p.setValue ("tabletPortraitPanels", tablet.portraitPanels);
    p.setValue ("tabletPortraitSplit", tablet.portraitSplit);
}

bool MainScreen::keyPressed (const juce::KeyPress& key)
{
    const auto c = screen::commandForKey (key);
    if (c == screen::Command::None) return false;
    runCommand (c);
    return true;
}

void MainScreen::runCommand (screen::Command c)
{
    using C = screen::Command;
    const bool rec = ctx.recorder.isRecording();
    const bool audio = ctx.document.hasAudio();
    // While recording, the take owns the document: only the transport, the
    // zoom and the panels answer.
    if (rec && c != C::Stop && c != C::Record && c != C::ZoomIn && c != C::ZoomOut
            && c != C::ZoomAll && c != C::TogglePanels)
        return;
    const auto edit = [] (const char* box, bool ok, const juce::String& err, const char* op)
    {
        if (! ok && err.isNotEmpty()) showMessage (box, err);
        else if (ok) telemetry::capture ("edit", { { "op", op }, { "from", "keyboard" } });
    };
    const auto anchor = [this]
    {
        const double t = ctx.document.playhead();
        return t >= ctx.view.start() && t <= ctx.view.end() ? t : ctx.view.start() + ctx.view.span() * 0.5;
    };
    switch (c)
    {
        case C::PlayPause: togglePlay(); break;
        case C::Stop:      if (rec) stopRecording(); else stopPlayback(); break;
        case C::Record:    toggleRecord(); break;
        case C::GoToStart:
        case C::GoToEnd:
            if (audio)
            {
                const double t = c == C::GoToStart ? 0.0 : ctx.document.duration();
                ctx.document.setPlayhead (t);
                if (t < ctx.view.start() || t > ctx.view.end())
                    ctx.view.setRange (t - (c == C::GoToEnd ? ctx.view.span() : 0.0), t + (c == C::GoToStart ? ctx.view.span() : 0.0));
            }
            break;
        case C::SelectAll:      if (audio) ctx.document.setSelection ({ 0.0, ctx.document.duration() }); break;
        case C::ClearSelection: ctx.document.setSelection ({}); break;
        case C::Delete:
            if (audio && ! ctx.document.selection().isEmpty())
            {
                juce::String err;
                edit ("Delete", ctx.document.deleteSelection (err), err, "delete");
            }
            break;
        case C::Undo: if (audio && ctx.document.undo()) telemetry::capture ("edit", { { "op", "undo" } }); break;
        case C::Redo: if (audio && ctx.document.redo()) telemetry::capture ("edit", { { "op", "redo" } }); break;
        case C::Save: if (audio) saveButton.triggerClick(); break;
        case C::ZoomIn:  ctx.view.zoom (anchor(), 2.0); break;
        case C::ZoomOut: ctx.view.zoom (anchor(), 0.5); break;
        case C::ZoomAll: ctx.view.showAll(); break;
        case C::TogglePanels: togglePanels(); break;
        case C::None: break;
    }
    updateTransport();
}

void MainScreen::setLayout (Layout l)
{
    if (l != layout && getWidth() > 0)
        telemetry::capture ("layout_changed", { { "view", l == Layout::Both ? "both" : l == Layout::Waveform ? "waveform" : "spectrogram" } });
    layout = l;
    ctx.props.setValue ("layout", (int) l);
    waveform.setVisible (l != Layout::Spectrogram);
    spectrogram.setVisible (l != Layout::Waveform);
    waveform.setShowRuler (true);
    spectrogram.setShowRuler (l == Layout::Spectrogram);
    layoutW.setToggleState (l == Layout::Waveform, juce::dontSendNotification);
    layoutS.setToggleState (l == Layout::Spectrogram, juce::dontSendNotification);
    layoutWS.setToggleState (l == Layout::Both, juce::dontSendNotification);
    resized();
}

void MainScreen::paint (juce::Graphics& g)
{
    g.fillAll (style::appBackground);
    if (! screen::isTablet (form) || ! tablet.panelsVisible || mapFullScreen()) return;
    // the panel groups, framed like the desktop's docked groups: a tab strip
    // on a darker bar above each panel
    for (int i = 0; i < 2; ++i)
    {
        g.setColour (style::barBackground);
        g.fillRect (frame.tabs[i]);
        g.setColour (style::panelBackground);
        g.fillRect (frame.panel[i]);
        g.setColour (style::border);
        g.drawRect (frame.tabs[i].getUnion (frame.panel[i]), 1);
    }
}

void MainScreen::resized()
{
    // notch or Dynamic Island, status bar, home indicator (see SafeArea.h)
    appliedInsets = safearea::insetsFor (*this);
    const auto r = safeBounds();
    if (const auto f = screen::formFor (r); f != form)
    {
        form = f;
        applyPanels();
    }
    static bool reported = false;   // the form the app starts in, once per launch
    if (! reported && ! r.isEmpty())
    {
        reported = true;
        telemetry::capture ("screen_form", { { "form", form == screen::Form::Phone ? "phone"
                                                     : form == screen::Form::TabletLandscape ? "tablet_landscape"
                                                                                              : "tablet_portrait" } });
    }
    if (screen::isTablet (form)) layoutTablet (r);
    else layoutPhone (r);
    repaint();
}

void MainScreen::layoutPhone (juce::Rectangle<int> r)
{
    const bool landscape = r.getWidth() > r.getHeight() * 1.2;

    // top bar: the file's name, the menu at the right
    auto top = r.removeFromTop (style::kTouch).reduced (style::kGap, 3);
    menuButton.setBounds (top.removeFromRight (52));
    top.removeFromRight (style::kGap);
    title.setBounds (top);

    // the Map tab: everything below the top bar but the tabs
    const bool full = mapFullScreen() && soundMapPane != nullptr;
    showMapFullScreen (full);
    if (full)
    {
        auto tabsRow = r.removeFromBottom (style::kTouch).reduced (2, 2);
        const int tw = tabsRow.getWidth() / screen::NumTabs;
        for (int tab : screen::kPhoneTabOrder) tabButtons[tab]->setBounds (tabsRow.removeFromLeft (tw).reduced (1, 0));
        soundMapPane->setBounds (r);
        return;
    }

    juce::Rectangle<int> panelArea, tabsArea;
    if (landscape)
    {
        auto side = r.removeFromRight (juce::jlimit (300, 460, (int) (r.getWidth() * 0.42)));
        tabsArea = side.removeFromBottom (style::kTouch);
        panelArea = panelOpen ? side : juce::Rectangle<int>();
        if (! panelOpen) r.setRight (side.getRight());
    }
    else
    {
        tabsArea = r.removeFromBottom (style::kTouch);
        if (panelOpen) panelArea = r.removeFromBottom ((int) (r.getHeight() * 0.42));
    }

    // tabs
    {
        auto t = tabsArea.reduced (2, 2);
        const int w = t.getWidth() / screen::NumTabs;
        for (int tab : screen::kPhoneTabOrder) tabButtons[tab]->setBounds (t.removeFromLeft (w).reduced (1, 0));
    }
    if (auto* c = tabContent (currentTab); c != nullptr && panelOpen)
        c->setBounds (panelArea);

    layoutTransport (r.removeFromBottom (screen::transportHeight (false)).reduced (style::kGap, 3), false);
    layoutViews (r);
}

void MainScreen::layoutTablet (juce::Rectangle<int> r)
{
    frame = screen::tabletFrame (r, form, tablet);

    // top bar: [panels] name ... [undo][redo]  [W][S][W+S]  [Save]
    auto top = frame.topBar.reduced (style::kGap, 3);
    panelsButton.setBounds (top.removeFromLeft (52));
    top.removeFromLeft (style::kGap * 2);
    saveButton.setBounds (top.removeFromRight (72));
    top.removeFromRight (style::kGap * 2);
    for (auto* b : { &layoutWS, &layoutS, &layoutW })
    {
        b->setBounds (top.removeFromRight (b == &layoutWS ? 56 : 44));
        top.removeFromRight (2);
    }
    top.removeFromRight (style::kGap * 2);
    redoButton.setBounds (top.removeFromRight (52));
    top.removeFromRight (2);
    undoButton.setBounds (top.removeFromRight (52));
    top.removeFromRight (style::kGap);
    title.setBounds (top);

    // the Map tab: everything below the top bar, every tab in one row under it
    const bool full = mapFullScreen() && soundMapPane != nullptr;
    showMapFullScreen (full);
    if (full)
    {
        auto area = r.withTrimmedTop (frame.topBar.getBottom() - r.getY());
        auto tabsRow = area.removeFromBottom (style::kTouch).reduced (3, 3);
        const int tw = std::min (140, tabsRow.getWidth() / (int) screen::NumTabs);
        for (int tab : screen::kPhoneTabOrder) tabButtons[tab]->setBounds (tabsRow.removeFromLeft (tw).reduced (1, 0));
        soundMapPane->setBounds (area);
        return;
    }

    // the two panel groups, each with its tab strip (tabs from the left, as
    // on the desktop)
    for (int g = 0; g < 2; ++g)
    {
        const auto& tabs = screen::tabsOfGroup (g);
        auto strip = frame.tabs[g].reduced (3, 3);
        const int w = std::min (140, strip.getWidth() / (int) tabs.size());
        for (int t : tabs)
            tabButtons[t]->setBounds (strip.removeFromLeft (w).reduced (1, 0));
        if (auto* c = tabContent (tablet.tab[g]); c != nullptr && tablet.panelsVisible)
            c->setBounds (frame.panel[g].reduced (1, 0).withTrimmedBottom (1));
    }
    panelsSplitter.setVertical (form == screen::Form::TabletLandscape);
    panelsSplitter.setBounds (frame.panelsSplitter);
    groupSplitter.setVertical (form == screen::Form::TabletPortrait);
    groupSplitter.setBounds (frame.groupSplitter);

    layoutTransport (frame.transport.reduced (style::kGap, 3), frame.transportSingleRow);
    layoutViews (frame.views);
}

void MainScreen::showMapFullScreen (bool full)
{
    for (auto* c : std::initializer_list<juce::Component*> { &recordButton, &playButton, &stopButton, &videoButton,
                                                            &modeBox, &timeLabel, &meter })
        c->setVisible (! full);
    waveform.setVisible (! full && layout != Layout::Spectrogram);
    spectrogram.setVisible (! full && layout != Layout::Waveform);
    videoPane.setVisible (! full && videoPane.hasContent());
}

void MainScreen::layoutTransport (juce::Rectangle<int> tr, bool singleRow)
{
    if (singleRow)
    {
        // REC, Play, Stop, mode, Video, then the time and the input meter
        const int unit = juce::jlimit (72, 104, tr.getWidth() / 11);
        recordButton.setBounds (tr.removeFromLeft (unit + unit / 3)); tr.removeFromLeft (4);
        playButton.setBounds (tr.removeFromLeft (unit)); tr.removeFromLeft (4);
        stopButton.setBounds (tr.removeFromLeft (unit)); tr.removeFromLeft (4);
        modeBox.setBounds (tr.removeFromLeft (unit * 3 / 2 + 8).reduced (0, 2)); tr.removeFromLeft (4);
        videoButton.setBounds (tr.removeFromLeft (unit)); tr.removeFromLeft (style::kGap * 2);
        meter.setBounds (tr.removeFromRight (std::min (360, tr.getWidth() / 2)).withSizeKeepingCentre (std::min (360, tr.getWidth() / 2), 22));
        timeLabel.setBounds (tr);
        return;
    }
    // the phone's: buttons, then the time and the input meter
    auto status = tr.removeFromBottom (26);
    timeLabel.setBounds (status.removeFromLeft (status.getWidth() / 2));
    meter.setBounds (status.reduced (0, 3));
    tr.removeFromBottom (3);
    const int unit = (tr.getWidth() - 16) / 6;
    recordButton.setBounds (tr.removeFromLeft (unit + unit / 3)); tr.removeFromLeft (4);
    playButton.setBounds (tr.removeFromLeft (unit)); tr.removeFromLeft (4);
    stopButton.setBounds (tr.removeFromLeft (unit)); tr.removeFromLeft (4);
    videoButton.setBounds (tr.removeFromRight (unit)); tr.removeFromRight (4);
    modeBox.setBounds (tr.reduced (0, 2));
}

void MainScreen::layoutViews (juce::Rectangle<int> views)
{
    // the video first (its own part, sized to the picture), then the
    // waveform and the spectrogram
    views = views.reduced (0, 2);
    if (videoPane.isVisible())
    {
        const auto split = video::split (views, videoPane.pictureAspect(), videoPane.isCollapsed());
        videoPane.setBounds (split.pane);
        views = split.rest;
    }
    if (layout == Layout::Both)
    {
        waveform.setBounds (views.removeFromTop ((int) (views.getHeight() * 0.23)));
        views.removeFromTop (2);
        spectrogram.setBounds (views);
    }
    else if (layout == Layout::Waveform) waveform.setBounds (views);
    else spectrogram.setBounds (views);
}
} // namespace scm
