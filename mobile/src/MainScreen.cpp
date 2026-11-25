#include "MainScreen.h"
#include "LiveAudio.h"
#include "MobileDocument.h"
#include "MobileStyle.h"
#include "Recorder.h"
#include "TimeFormat.h"
#include "TimeViewport.h"
#include "VideoCapture.h"

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
    const auto toX = [&r] (float db) { return r.getX() + r.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 80.0f) / 80.0f); };
    const float rms = ctx.audio.inputRmsDb (0), pk = ctx.audio.inputPeakDb (0);
    const auto colour = pk > -1.0f ? style::record : pk > -12.0f ? style::warning : style::ok;
    g.setColour (colour.withAlpha (0.85f));
    g.fillRoundedRectangle (r.withRight (toX (rms)), 3.0f);
    g.setColour (juce::Colours::white);
    g.fillRect (toX (pk) - 1.0f, r.getY(), 2.0f, r.getHeight());
    const auto cal = ctx.currentInputCalibration();
    juce::String text = rms > -150.0f ? juce::String (rms, 1) + " dBFS" : juce::String ("-- dBFS");
    if (cal.valid() && rms > -150.0f)
        text = juce::String (rms + cal::fullScaleDbSpl (cal.fullScalePa), 1) + " dB-SPL";
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
    saveButton.onClick = [this]
    {
        juce::String err;
        if (! ctx.document.save (err)) showMessage ("Save", err);
        if (filesPane) filesPane->refresh();
    };
    for (auto* b : { &layoutW, &layoutS, &layoutWS, &saveButton }) addAndMakeVisible (b);

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

    filesPane = std::make_unique<FilesPane> (ctx);
    analysisPane = std::make_unique<AnalysisPane> (ctx.document, ctx.view);
    generatorPane = std::make_unique<GeneratorPane> (ctx);
    calibrationPane = std::make_unique<CalibrationPane> (ctx);
    inputPane = std::make_unique<InputPane> (ctx);
    editPane = std::make_unique<EditPane> (ctx);
    inputPane->onInputChanged = [this] { calibrationPane->refresh(); };
    editPane->onChannelChanged = [this] (int ch) { waveform.setChannel (ch); spectrogram.setChannel (ch); };
    editPane->onLogFrequencyChanged = [this] (bool log) { spectrogram.setLogFrequency (log); };
    editPane->onDisplayChanged = [this] { ctx.document.spectral().clearCache(); spectrogram.contentChanged(); };
    editPane->onZoomToSelection = [this] { zoomToSelection(); };

    const char* names[] = { "Files", "Analyse", "Generate", "Calibrate", "Input", "Edit" };
    for (int t = 0; t < NumTabs; ++t)
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
        stopRecording();
        showMessage ("Recording", msg);
    };

    selectTab (ctx.props.getIntValue ("tab", Analysis));
    setLayout ((Layout) ctx.props.getIntValue ("layout", 0));
    updateTransport();
    startTimerHz (30);
}

MainScreen::~MainScreen()
{
    stopTimer();
    ctx.document.removeChangeListener (this);
    ctx.document.onAboutToReplace = nullptr;
    ctx.document.onReplaced = nullptr;
    ctx.recorder.onUpdate = nullptr;
    ctx.recorder.onAutoStop = nullptr;
    ctx.audio.setPlaybackSource (nullptr);
}

void MainScreen::audioReady (const juce::String& error)
{
    if (error.isNotEmpty()) showMessage ("Microphone", error);
    inputPane->refresh();
    calibrationPane->refresh();
    updateTransport();
}

void MainScreen::suspended()
{
    // Recording continues in the background (UIBackgroundModes audio); the
    // generator and playback stop.
    if (! ctx.recorder.isRecording())
    {
        stopPlayback();
        ctx.audio.generator().setEnabled (false);
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
    spectrogram.setFrequencyRange (20.0, ctx.document.sampleRate() * 0.5);
    waveform.setChannel (0);
    spectrogram.setChannel (0);
    updateTransport();
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
}

void MainScreen::timerCallback()
{
    // follow the playhead while playing
    if (auto* d = ctx.document.doc(); d != nullptr && d->engine.isPlaying())
    {
        const double t = d->engine.position();
        ctx.document.setPlayhead (t);
        if (t > ctx.view.end()) ctx.view.pan (ctx.view.span() * 0.9);
        if (t >= ctx.document.duration() - 1e-3 && ! d->engine.isLooping()) stopPlayback();
        updateTransport();
    }
}

void MainScreen::toggleRecord()
{
    if (ctx.recorder.isRecording()) stopRecording();
    else startRecording();
}

void MainScreen::startRecording()
{
    if (ctx.document.hasUnsavedChanges() && ctx.document.file() != juce::File())
    {
        juce::String err;
        ctx.document.save (err);   // keep the edits of the open file before the new take
    }
    stopPlayback();
    const int mode = modeBox.getSelectedId();
    ctx.recorder.setMode (mode == 1 ? Recorder::Mode::Unlimited : Recorder::Mode::Loop, (double) mode);
    juce::String err;
    const auto cal = ctx.currentInputCalibration();
    if (! ctx.recorder.start (cal.toScale(), err))
    {
        showMessage ("Recording", err);
        return;
    }
    if (videoButton.getToggleState() && ctx.video.isOpen())
        ctx.video.start (VideoCapture::movieFileFor (ctx.recorder.targetFile()));
    ctx.view.setMinimumVisible (mode == 1 ? 10.0 : (double) mode);
    ctx.view.setRange (0.0, mode == 1 ? 10.0 : (double) mode);
    analysisPane->setLive (true);
    updateTransport();
}

void MainScreen::stopRecording()
{
    juce::File movie;
    if (ctx.video.isRecording()) movie = ctx.video.stop();
    juce::String err;
    const auto saved = ctx.recorder.stop (err, [this, movie]
    {
        if (movie != juce::File())
        {
            const double offset = (double) (ctx.video.firstFrameTime().toMilliseconds()
                                            - ctx.recorder.firstFrameTime().toMilliseconds()) / 1000.0;
            ctx.document.linkVideo (movie, offset);
        }
    });
    waveform.setWriteHead (0.0, false);
    spectrogram.setWriteHead (0.0, false);
    analysisPane->setLive (false);
    ctx.view.setMinimumVisible (0.0);
    ctx.view.setDuration (ctx.document.duration(), false);
    ctx.view.showAll();
    if (saved == juce::File() && err.isNotEmpty()) showMessage ("Recording", err);
    if (filesPane) filesPane->refresh();
    updateTransport();
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
    updateTransport();
}

void MainScreen::stopPlayback()
{
    if (auto* d = ctx.document.doc())
    {
        d->engine.stop();
        const auto sel = ctx.document.selection();
        ctx.document.setPlayhead (sel.isEmpty() ? 0.0 : sel.getStart());
    }
    updateTransport();
}

void MainScreen::toggleVideo()
{
    if (videoButton.getToggleState())
    {
        ctx.video.open ([this] (juce::String err)
        {
            if (err.isNotEmpty())
            {
                videoButton.setToggleState (false, juce::dontSendNotification);
                showMessage ("Video", err);
                return;
            }
            videoPreview = ctx.video.preview();
            if (videoPreview != nullptr) addAndMakeVisible (videoPreview);
            resized();
        });
    }
    else
    {
        if (videoPreview != nullptr) removeChildComponent (videoPreview);
        videoPreview = nullptr;
        if (! ctx.video.isRecording()) ctx.video.close();
        resized();
    }
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
        case Files:     return filesPane.get();
        case Analysis:  return analysisPane.get();
        case Generator: return generatorPane.get();
        case Calibrate: return calibrationPane.get();
        case Input:     return inputPane.get();
        case Edit:      return editPane.get();
        default:        return nullptr;
    }
}

void MainScreen::selectTab (int t)
{
    t = juce::jlimit (0, NumTabs - 1, t);
    if (t == currentTab && panelOpen && getWidth() > 0) panelOpen = false;   // tap again: hide the panel
    else panelOpen = true;
    currentTab = t;
    ctx.props.setValue ("tab", t);
    for (int i = 0; i < NumTabs; ++i)
    {
        tabButtons[i]->setToggleState (panelOpen && i == currentTab, juce::dontSendNotification);
        if (auto* c = tabContent (i))
        {
            if (i == currentTab && panelOpen) addAndMakeVisible (c);
            else removeChildComponent (c);
        }
    }
    if (t == Input && panelOpen) inputPane->refresh();
    resized();
}

void MainScreen::setLayout (Layout l)
{
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
}

void MainScreen::resized()
{
    auto r = getLocalBounds();
   #if JUCE_IOS || JUCE_ANDROID
    if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        r = d->safeAreaInsets.subtractedFrom (r);   // notch, home indicator
   #endif
    const bool landscape = r.getWidth() > r.getHeight() * 1.2;

    // top bar
    auto top = r.removeFromTop (style::kTouch).reduced (style::kGap, 3);
    saveButton.setBounds (top.removeFromRight (64));
    top.removeFromRight (style::kGap);
    for (auto* b : { &layoutWS, &layoutS, &layoutW })
    {
        b->setBounds (top.removeFromRight (b == &layoutWS ? 52 : 40));
        top.removeFromRight (2);
    }
    title.setBounds (top);

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
        const int w = t.getWidth() / NumTabs;
        for (auto* b : tabButtons) b->setBounds (t.removeFromLeft (w).reduced (1, 0));
    }
    if (auto* c = tabContent (currentTab); c != nullptr && panelOpen)
        c->setBounds (panelArea);

    // transport: buttons, then the time and the input meter
    auto tr = r.removeFromBottom (style::kTouch + 34).reduced (style::kGap, 3);
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

    // views
    auto views = r.reduced (0, 2);
    if (layout == Layout::Both)
    {
        waveform.setBounds (views.removeFromTop ((int) (views.getHeight() * 0.34)));
        views.removeFromTop (2);
        spectrogram.setBounds (views);
    }
    else if (layout == Layout::Waveform) waveform.setBounds (views);
    else spectrogram.setBounds (views);

    if (videoPreview != nullptr)
    {
        const int w = std::min (180, views.getWidth() / 3);
        videoPreview->setBounds (views.getRight() - w - 6, views.getY() + 26, w, w * 9 / 16);
        videoPreview->toFront (false);
    }
}
} // namespace scm
