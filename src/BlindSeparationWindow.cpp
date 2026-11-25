// SpectroCraft - Effects > Blind Source Separation... (issue #68); see include/BlindSeparationWindow.h
#include "BlindSeparationWindow.h"
#include "Application.h"
#include "AlertGate.h"
#include "WorkProgress.h"
#include "MergeFiles.h"
#include <thread>

namespace
{
const juce::Colour kBg (45, 45, 48), kInk (225, 225, 230);

int defaultIterations (sc::bss::Method m)
{
    switch (m)
    {
        case sc::bss::Method::auxIva:    return 50;
        case sc::bss::Method::ilrma:     return 100;
        case sc::bss::Method::cacgmm:    return 40;
        case sc::bss::Method::fastMnmf2: return 100;
    }
    return 50;
}

int defaultBases (sc::bss::Method m) { return m == sc::bss::Method::fastMnmf2 ? 16 : 8; }
} // namespace

BlindSeparationView::BlindSeparationView (Application& a) : app (a)
{
    setComponentID ("bss-view");
    for (const auto& m : sc::bss::methods())
        method_.addItem (m.name, (int) m.method + 1);
    method_.onChange = [this]
    {
        const auto m = method();
        iterations_.setValue (defaultIterations (m), juce::dontSendNotification);
        bases_.setValue (defaultBases (m), juce::dontSendNotification);
        updateRows();
    };
    addAndMakeVisible (method_);

    desc_.setMultiLine (true, true);
    desc_.setReadOnly (true);
    desc_.setCaretVisible (false);
    desc_.setFont (juce::FontOptions (12.5f));
    desc_.setColour (juce::TextEditor::backgroundColourId, juce::Colour (35, 35, 38));
    desc_.setColour (juce::TextEditor::textColourId, kInk);
    desc_.setColour (juce::TextEditor::outlineColourId, juce::Colour (72, 72, 78));
    addAndMakeVisible (desc_);

    for (int n = 512; n <= 16384; n *= 2)
        fft_.addItem (juce::String (n), n);
    auto slider = [] (juce::Slider& s, double lo, double hi, double step, const juce::String& suffix)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setRange (lo, hi, step);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
        s.setTextValueSuffix (suffix);
    };
    slider (iterations_, 5, 500, 5, "");
    slider (bases_, 2, 32, 1, "");
    slider (block_, 10, 600, 5, " s");
    extract_.addItem ("Beamformer (MVDR)", 1);
    extract_.addItem ("Mask on the reference", 2);
    output_.addItem ("New file", 1);
    output_.addItem ("Replace the current file", 2);
    output_.addItem ("Add as new tracks", 3);

    addRow ("sources", "Sources", sources_,
            "How many sources to separate: at most the number of channels.");
    addRow ("fft", "FFT size", fft_,
            "Analysis window (samples). Longer: better in reverberant rooms and for low "
            "frequencies; shorter: for sources that move.");
    addRow ("iterations", "Iterations", iterations_,
            "More: a finer separation, proportionally slower.");
    addRow ("bases", "NMF bases", bases_,
            "Spectral shapes per source: few for simple tonal sources, more for rich ones.");
    addRow ("ref", "Reference", ref_,
            "Each source comes out as this microphone hears it.");
    addRow ("extract", "Extraction", extract_,
            "Beamformer: cleaner, keeps the sound natural; Mask: stronger suppression, can sound watery.");
    addRow ("noise", "", noise_,
            "An extra class gathers the diffuse noise (airflow, hiss) so it does not leak into the sources.");
    addRow ("block", "Block length", block_,
            "Long recordings are processed in blocks of this length (bounded memory); "
            "each block continues the previous one.");
    addRow ("output", "Output", output_,
            "New file: the sources in a new document. Replace: they replace the current file. "
            "Add: they become new tracks over the range.");

    range_.setFont (juce::FontOptions (12.0f));
    range_.setColour (juce::Label::textColourId, juce::Colour (0xffb0b0b8));
    addAndMakeVisible (range_);
    status_.setFont (juce::FontOptions (12.0f));
    status_.setColour (juce::Label::textColourId, kInk);
    addAndMakeVisible (status_);
    separate_.onClick = [this] { if (onSeparate) onSeparate(); };
    close_.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (separate_);
    addAndMakeVisible (close_);

    refresh();
    load();
    updateRows();
}

void BlindSeparationView::addRow (const juce::String& id, const juce::String& text, juce::Component& c,
                                  const juce::String& tip)
{
    Row r;
    r.id = id;
    r.label = std::make_unique<juce::Label> (juce::String(), text);
    r.label->setFont (juce::FontOptions (12.5f));
    r.label->setColour (juce::Label::textColourId, kInk);
    r.label->setJustificationType (juce::Justification::centredRight);
    r.control = &c;
    if (auto* t = dynamic_cast<juce::SettableTooltipClient*> (&c))
        t->setTooltip (tip);
    addAndMakeVisible (*r.label);
    addAndMakeVisible (c);
    rows_.push_back (std::move (r));
}

sc::bss::Method BlindSeparationView::method() const
{
    return (sc::bss::Method) juce::jlimit (0, 3, method_.getSelectedId() - 1);
}

void BlindSeparationView::setMethod (sc::bss::Method m)
{
    method_.setSelectedId ((int) m + 1, juce::sendNotificationSync);
}

bool BlindSeparationView::isRowShown (const juce::String& id) const
{
    for (const auto& r : rows_)
        if (r.id == id) return r.control->isVisible();
    return false;
}

void BlindSeparationView::updateRows()
{
    const auto& info = sc::bss::infoOf (method());
    desc_.setText (juce::String (info.name) + ": " + info.description + "\n\nPros: " + info.pros
                   + "\nCons: " + info.cons, false);
    for (auto& r : rows_)
    {
        bool show = true;
        if (r.id == "bases") show = info.usesNmf;
        if (r.id == "extract" || r.id == "noise") show = method() == sc::bss::Method::cacgmm;
        r.label->setVisible (show);
        r.control->setVisible (show);
    }
    resized();
}

void BlindSeparationView::refresh()
{
    const FileDoc* d = app.doc();
    const int ch = d != nullptr && d->engine.isFileLoaded() ? d->engine.numChannels() : 0;
    const int keepSources = sources_.getSelectedId(), keepRef = ref_.getSelectedId();
    sources_.clear (juce::dontSendNotification);
    ref_.clear (juce::dontSendNotification);
    for (int n = 1; n <= std::max (1, ch); ++n)
    {
        sources_.addItem (juce::String (n) + (n == 1 ? " source" : " sources"), n);
        ref_.addItem ("Channel " + juce::String (n), n);
    }
    sources_.setSelectedId (keepSources > 0 && keepSources <= ch ? keepSources : std::max (1, ch), juce::dontSendNotification);
    ref_.setSelectedId (keepRef > 0 && keepRef <= ch ? keepRef : 1, juce::dontSendNotification);

    juce::String r;
    if (ch < 2)
        r = "Needs a file with at least two channels (microphones).";
    else
    {
        const double dur = d->engine.duration();
        const auto sel = app.selection;
        r = sel.getLength() > 1e-6
                ? "Selection " + juce::String (sel.getStart(), 2) + " s - " + juce::String (sel.getEnd(), 2) + " s ("
                      + juce::String (sel.getLength(), 2) + " s)"
                : "Whole file (" + juce::String (dur, 2) + " s)";
        r += ", " + juce::String (ch) + " channels, " + juce::String (d->engine.sampleRate(), 0) + " Hz";
    }
    range_.setText (r, juce::dontSendNotification);
    separate_.setEnabled (ch >= 2);
}

sc::bss::Params BlindSeparationView::params() const
{
    sc::bss::Params p;
    p.method = method();
    p.sources = sources_.getSelectedId();
    p.fftSize = std::max (512, fft_.getSelectedId());
    p.iterations = (int) iterations_.getValue();
    p.nmfBases = (int) bases_.getValue();
    p.refChannel = std::max (0, ref_.getSelectedId() - 1);
    p.blockSeconds = block_.getValue();
    p.noiseClass = noise_.getToggleState();
    p.beamformer = extract_.getSelectedId() != 2;
    return p;
}

int BlindSeparationView::outputMode() const
{
    return juce::jlimit (1, 3, output_.getSelectedId());
}

void BlindSeparationView::load()
{
    const FileDoc* d = app.doc();
    const double rate = d != nullptr && d->engine.isFileLoaded() ? d->engine.sampleRate() : 48000.0;
    const auto v = app.recentStore != nullptr ? juce::JSON::parse (app.recentStore->getValue ("bss_params"))
                                              : juce::var();
    const int m = juce::jlimit (0, 3, (int) v.getProperty ("method", 0));
    method_.setSelectedId (m + 1, juce::dontSendNotification);
    const int fftDefault = rate >= 32000.0 ? 4096 : 2048;
    const int fft = (int) v.getProperty ("fft", fftDefault);
    fft_.setSelectedId (fft_.indexOfItemId (fft) >= 0 ? fft : fftDefault, juce::dontSendNotification);
    iterations_.setValue ((int) v.getProperty ("iterations", defaultIterations ((sc::bss::Method) m)), juce::dontSendNotification);
    bases_.setValue ((int) v.getProperty ("bases", defaultBases ((sc::bss::Method) m)), juce::dontSendNotification);
    block_.setValue ((double) v.getProperty ("block", 60.0), juce::dontSendNotification);
    noise_.setToggleState ((bool) v.getProperty ("noise", true), juce::dontSendNotification);
    extract_.setSelectedId ((bool) v.getProperty ("beamformer", true) ? 1 : 2, juce::dontSendNotification);
    output_.setSelectedId (juce::jlimit (1, 3, (int) v.getProperty ("output", 1)), juce::dontSendNotification);
}

void BlindSeparationView::remember() const
{
    if (app.recentStore == nullptr)
        return;
    auto* o = new juce::DynamicObject();
    const auto p = params();
    o->setProperty ("method", (int) p.method);
    o->setProperty ("fft", p.fftSize);
    o->setProperty ("iterations", p.iterations);
    o->setProperty ("bases", p.nmfBases);
    o->setProperty ("block", p.blockSeconds);
    o->setProperty ("noise", p.noiseClass);
    o->setProperty ("beamformer", p.beamformer);
    o->setProperty ("output", outputMode());
    app.recentStore->setValue ("bss_params", juce::JSON::toString (juce::var (o), true));
}

void BlindSeparationView::resized()
{
    auto b = getLocalBounds().reduced (12, 10);
    {
        auto row = b.removeFromTop (26);
        method_.setBounds (row.withTrimmedLeft (130 + 8));
    }
    b.removeFromTop (6);
    desc_.setBounds (b.removeFromTop (96));
    b.removeFromTop (8);
    for (auto& r : rows_)
    {
        if (! r.control->isVisible()) continue;
        auto row = b.removeFromTop (26);
        r.label->setBounds (row.removeFromLeft (130));
        row.removeFromLeft (8);
        r.control->setBounds (row.reduced (0, 1));
        b.removeFromTop (4);
    }
    auto buttons = b.removeFromBottom (30);
    close_.setBounds (buttons.removeFromRight (90));
    buttons.removeFromRight (8);
    separate_.setBounds (buttons.removeFromRight (110));
    b.removeFromBottom (6);
    status_.setBounds (b.removeFromBottom (22));
    range_.setBounds (b.removeFromBottom (22));
}

void BlindSeparationView::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
    g.setColour (kInk);
    g.setFont (juce::FontOptions (12.5f));
    g.drawText ("Method", juce::Rectangle<int> (12, 10, 130, 26), juce::Justification::centredRight);
}

// ---------------------------------------------------------------- window

BlindSeparationWindow::BlindSeparationWindow (Application& a)
    : juce::DocumentWindow ("Blind Source Separation", kBg, juce::DocumentWindow::closeButton), app (a)
{
    view_ = std::make_unique<BlindSeparationView> (app);
    view_->onSeparate = [this] { start(); };
    view_->onClose = [this] { if (onCloseRequested) onCloseRequested(); };
    setUsingNativeTitleBar (true);
    setContentNonOwned (view_.get(), false);
    setResizable (true, false);
    centreWithSize (600, 560);
    setVisible (true);
}

BlindSeparationWindow::~BlindSeparationWindow()
{
    alive_->store (false);
    clearContentComponent();
}

void BlindSeparationWindow::userTriedToCloseWindow()
{
    if (! running_ && onCloseRequested) onCloseRequested();
}

void BlindSeparationWindow::closeButtonPressed()
{
    userTriedToCloseWindow();
}

void BlindSeparationWindow::start()
{
    if (running_)
        return;
    view_->refresh();
    FileDoc* d = app.doc();
    if (d == nullptr || ! d->engine.isFileLoaded() || d->engine.numChannels() < 2)
    {
        view_->setStatus ("Needs a file with at least two channels.");
        return;
    }
    AudioEngine* eng = &d->engine;
    const double rate = eng->sampleRate();
    const auto sel = app.selection;
    const bool hasSel = sel.getLength() > 1e-6;
    const double s0 = hasSel ? sel.getStart() : 0.0;
    const double s1 = hasSel ? sel.getEnd() : eng->duration();
    const int64_t startFrame = (int64_t) std::llround (s0 * rate);
    const int64_t frames = std::min<int64_t> ((int64_t) std::llround ((s1 - s0) * rate), eng->numSamples() - startFrame);
    const auto p = view_->params();
    const int outMode = view_->outputMode();
    const juce::String name = d->name.upToLastOccurrenceOf (".", false, false) + "_sources";
    view_->remember();
    if (frames < (int64_t) p.fftSize * 2)
    {
        view_->setStatus ("The range is too short for this FFT size.");
        return;
    }

    running_ = true;
    view_->separateButton().setEnabled (false);
    view_->setStatus ("Separating...");
    auto* prog = new sc::WorkProgressDialog ("Blind Source Separation",
                                             juce::String (sc::bss::infoOf (p.method).name) + ": separating "
                                                 + juce::String (sc::bss::sourcesFor (p, eng->numChannels())) + " sources...",
                                             true, nullptr);
    prog->setProgress (0.0);
    prog->beginModal();

    std::thread ([this, guard = alive_, prog, eng, rate, startFrame, frames, p, outMode, name, s0, docAtStart = d]
    {
        std::vector<std::vector<float>> in, out;
        std::string err;
        bool ok = eng->readRange (startFrame, frames, in);
        if (! ok) err = "Could not read the range.";
        else
        {
            auto last = std::make_shared<std::atomic<int>> (-1);
            ok = sc::bss::separate (in, rate, p, out, &err, [prog, last] (double f)
            {
                const int pct = (int) (f * 1000.0);
                if (last->exchange (pct) != pct)
                    juce::MessageManager::callAsync ([prog, f] { prog->setProgress (f); });
                return ! prog->cancelled.load();
            });
        }
        in.clear();
        auto* res = new sc::MergeResult();
        res->rate = rate;
        res->channels = (int) out.size();
        res->data = std::move (out);
        juce::MessageManager::callAsync ([this, guard, prog, res, ok, err, outMode, name, s0, docAtStart]
        {
            delete prog;
            std::unique_ptr<sc::MergeResult> r (res);
            if (! guard->load())
                return;
            running_ = false;
            view_->separateButton().setEnabled (true);
            if (! ok)
            {
                view_->setStatus (err == "cancelled" ? juce::String ("Cancelled.")
                                                     : "Separation failed: " + juce::String (err));
                return;
            }
            // The user may have switched documents meanwhile: a new file then.
            const bool sameDoc = docAtStart != nullptr && app.doc() == docAtStart;
            const int mode = sameDoc ? outMode : 1;
            std::string e;
            if (! app.applyStemSeparationResult (*r, mode, s0, name, &e, "Blind source separation"))
            {
                view_->setStatus ("Could not apply the result: " + juce::String (e));
                return;
            }
            view_->setStatus ("Separated " + juce::String (r->channels) + " sources"
                              + (mode == 1 ? " into a new file." : mode == 2 ? " (replaced the file)." : " (new tracks)."));
            if (onCloseRequested) onCloseRequested();   // done: close, like Stem Separation
        });
    }).detach();
}
