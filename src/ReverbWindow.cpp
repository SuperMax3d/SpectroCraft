// SpectroCraft - Effects > Reverberation Analysis...; see include/ReverbWindow.h
#include "ReverbWindow.h"
#include "Application.h"
#include "FileDialogs.h"

namespace
{
const juce::Colour kBg (45, 45, 48), kInk (225, 225, 230), kDim (150, 150, 158), kPanel (35, 35, 38), kLine (72, 72, 78);
const juce::Colour kCurve (0, 212, 170), kFit (255, 179, 0), kWarn (255, 179, 0);

juce::String timeText (const sc::room::Fit& f)
{
    if (! f.measured()) return "-";
    return juce::String (f.seconds, 2) + (f.reliable ? "" : " ?");
}

juce::String number (double v, int decimals)
{
    return std::isnan (v) ? juce::String ("-") : juce::String (v, decimals);
}

juce::String notesFor (const sc::room::BandResult& b)
{
    juce::StringArray n;
    if (! b.noiseFound) n.add ("no decay above the noise");
    if (b.filterLimited) n.add ("too short for this band (the filter rings longer)");
    if (b.t20.measured() && ! b.t20.reliable && ! b.filterLimited && b.noiseFound) n.add ("noise too close for T20");
    else if (b.t30.measured() && ! b.t30.reliable && ! b.filterLimited && b.noiseFound) n.add ("noise too close for T30");
    if (! std::isnan (b.curvaturePct) && std::abs (b.curvaturePct) > 10.0)
        n.add ("not one slope (T30 / T20 " + juce::String (b.curvaturePct > 0.0 ? "+" : "") + juce::String (juce::roundToInt (b.curvaturePct)) + " %)");
    return n.joinIntoString ("; ");
}
} // namespace

//==============================================================================
/** The decay curve of one band, with the T20 line (and where the fits read it). */
class ReverbView::DecayPlot : public juce::Component
{
public:
    void show (const sc::room::BandResult* b)
    {
        has_ = b != nullptr;
        if (has_) band_ = *b;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (kPanel);
        auto r = getLocalBounds().toFloat().reduced (6.0f);
        r.removeFromLeft (30.0f);
        r.removeFromBottom (16.0f);
        g.setColour (kLine);
        g.drawRect (r, 1.0f);
        g.setFont (juce::FontOptions (11.0f));
        if (! has_ || band_.decayDb.size() < 2)
        {
            g.setColour (kDim);
            g.drawText ("The decay of the chosen band", r, juce::Justification::centred);
            return;
        }
        const double floorDb = -70.0;
        // the time axis: to a little after the curve leaves the plot
        size_t last = band_.decayDb.size() - 1;
        for (size_t i = 1; i < band_.decayDb.size(); ++i)
            if (band_.decayDb[i] <= (float) floorDb) { last = std::min (band_.decayDb.size() - 1, i + i / 8 + 1); break; }
        const double seconds = (double) last * band_.decayStepSeconds;
        const auto at = [&] (double t, double db)
        {
            return juce::Point<float> (r.getX() + (float) (t / seconds) * r.getWidth(),
                                       r.getY() + (float) (juce::jlimit (floorDb, 0.0, db) / floorDb) * r.getHeight());
        };
        for (int db = 0; db >= -70; db -= 10)
        {
            const auto p = at (0.0, db);
            g.setColour (kLine.withAlpha (0.6f));
            g.drawHorizontalLine ((int) p.y, r.getX(), r.getRight());
            g.setColour (kDim);
            g.drawText (juce::String (db), juce::Rectangle<float> (r.getX() - 30.0f, p.y - 6.0f, 26.0f, 12.0f), juce::Justification::centredRight);
        }
        const double tick = seconds > 4.0 ? 1.0 : seconds > 1.5 ? 0.5 : seconds > 0.6 ? 0.2 : 0.1;
        for (double t = 0.0; t <= seconds + 1.0e-9; t += tick)
        {
            const auto p = at (t, floorDb);
            g.setColour (kLine.withAlpha (0.6f));
            g.drawVerticalLine ((int) p.x, r.getY(), r.getBottom());
            g.setColour (kDim);
            g.drawText (juce::String (t, tick < 0.5 ? 1 : (tick < 1.0 ? 1 : 0)), juce::Rectangle<float> (p.x - 20.0f, r.getBottom() + 2.0f, 40.0f, 12.0f),
                        juce::Justification::centred);
        }
        juce::Path curve;
        double t5 = -1.0;
        for (size_t i = 0; i <= last; ++i)
        {
            const double t = (double) i * band_.decayStepSeconds;
            const auto p = at (t, band_.decayDb[i]);
            if (i == 0) curve.startNewSubPath (p); else curve.lineTo (p);
            if (t5 < 0.0 && band_.decayDb[i] <= -5.0f) t5 = t;
        }
        g.saveState();
        g.reduceClipRegion (r.toNearestInt());
        g.setColour (kCurve);
        g.strokePath (curve, juce::PathStrokeType (1.6f));
        if (band_.t20.measured() && t5 >= 0.0)   // the T20 line, from -5 to -25 dB (drawn on to -60)
        {
            const double perDb = band_.t20.seconds / 60.0;
            g.setColour (kFit.withAlpha (band_.t20.reliable ? 1.0f : 0.5f));
            g.drawLine ({ at (t5, -5.0), at (t5 + 20.0 * perDb, -25.0) }, 2.0f);
            const float dashes[] = { 4.0f, 4.0f };
            g.drawDashedLine ({ at (t5 + 20.0 * perDb, -25.0), at (t5 + 55.0 * perDb, -60.0) }, dashes, 2, 1.0f);
        }
        g.restoreState();
        g.setColour (kInk);
        g.drawText (sc::room::bandName (band_) + ": decay (dB) against time (s); line: T20 " + timeText (band_.t20) + " s",
                    r.reduced (6.0f, 3.0f), juce::Justification::topRight);
    }

private:
    bool has_ = false;
    sc::room::BandResult band_;
};

//==============================================================================
ReverbView::ReverbView (Application& a) : app (a)
{
    setComponentID ("reverb-view");
    method_.addItem ("A sweep and the silence after it", (int) Method::Sweep);
    method_.addItem ("An impulse (balloon, clapper)", (int) Method::Impulse);
    method_.setSelectedId ((int) Method::Sweep, juce::dontSendNotification);
    method_.onChange = [this] { updateFields(); };
    bandsBox_.addItem ("Octaves", 1);
    bandsBox_.addItem ("1/3 octaves", 2);
    bandsBox_.setSelectedId (1, juce::dontSendNotification);
    for (auto* e : { &startHz_, &endHz_, &seconds_ })
    {
        e->setInputRestrictions (9, "0123456789.");
        e->setColour (juce::TextEditor::backgroundColourId, kPanel);
        e->setColour (juce::TextEditor::textColourId, kInk);
        e->setColour (juce::TextEditor::outlineColourId, kLine);
        addAndMakeVisible (*e);
    }
    setSweep ({});
    for (auto* l : { &methodLabel_, &bandsLabel_, &channelLabel_, &sweepLabel_, &range_, &status_ })
    {
        l->setColour (juce::Label::textColourId, kInk);
        l->setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (*l);
    }
    range_.setColour (juce::Label::textColourId, kDim);
    status_.setMinimumHorizontalScale (1.0f);
    status_.setJustificationType (juce::Justification::topLeft);
    for (auto* c : std::initializer_list<juce::Component*> { &method_, &bandsBox_, &channel_, &analyse_, &export_, &close_, &table_ })
        addAndMakeVisible (*c);
    analyse_.onClick = [this] { analyse(); };
    export_.onClick = [this] { exportCsv(); };
    close_.onClick = [this] { if (onClose) onClose(); };
    export_.setEnabled (false);

    table_.setModel (this);
    table_.setColour (juce::ListBox::backgroundColourId, kPanel);
    table_.setRowHeight (20);
    auto& h = table_.getHeader();
    const int fixed = juce::TableHeaderComponent::visible;
    h.addColumn ("Band", 1, 70, 60, 90, fixed);
    h.addColumn ("EDT (s)", 2, 62, 50, 80, fixed);
    h.addColumn ("T20 (s)", 3, 62, 50, 80, fixed);
    h.addColumn ("T30 (s)", 4, 62, 50, 80, fixed);
    h.addColumn ("C50 (dB)", 5, 62, 50, 80, fixed);
    h.addColumn ("C80 (dB)", 6, 62, 50, 80, fixed);
    h.addColumn ("D50", 7, 48, 40, 70, fixed);
    h.addColumn ("Ts (ms)", 8, 56, 40, 80, fixed);
    h.addColumn ("INR (dB)", 9, 60, 50, 80, fixed);
    h.addColumn ("Notes", 10, 260, 100, 900, fixed);
    h.setStretchToFitActive (true);

    plot_ = std::make_unique<DecayPlot>();
    addAndMakeVisible (*plot_);
    refresh();
    updateFields();
}

ReverbView::~ReverbView()
{
    table_.setModel (nullptr);
}

void ReverbView::setMethod (Method m)
{
    method_.setSelectedId ((int) m, juce::dontSendNotification);
    updateFields();
}

ReverbView::Method ReverbView::method() const
{
    return method_.getSelectedId() == (int) Method::Impulse ? Method::Impulse : Method::Sweep;
}

void ReverbView::setSweep (const sc::room::SweepSpec& s)
{
    startHz_.setText (juce::String (s.startHz), false);
    endHz_.setText (juce::String (s.endHz), false);
    seconds_.setText (juce::String (s.seconds), false);
}

sc::room::SweepSpec ReverbView::sweep() const
{
    sc::room::SweepSpec s;
    s.startHz = startHz_.getText().getDoubleValue();
    s.endHz = endHz_.getText().getDoubleValue();
    s.seconds = seconds_.getText().getDoubleValue();
    return s;
}

void ReverbView::setThirdOctaves (bool thirds)
{
    bandsBox_.setSelectedId (thirds ? 2 : 1, juce::dontSendNotification);
}

void ReverbView::updateFields()
{
    const bool sweepFields = method() == Method::Sweep;
    for (auto* c : std::initializer_list<juce::Component*> { &sweepLabel_, &startHz_, &endHz_, &seconds_ })
        c->setEnabled (sweepFields);
}

void ReverbView::refresh()
{
    const FileDoc* d = app.doc();
    const int ch = d != nullptr && d->engine.isFileLoaded() ? d->engine.numChannels() : 0;
    const int keep = channel_.getSelectedId();
    channel_.clear (juce::dontSendNotification);
    for (int n = 1; n <= std::max (1, ch); ++n) channel_.addItem ("Channel " + juce::String (n), n);
    channel_.setSelectedId (keep > 0 && keep <= ch ? keep : 1, juce::dontSendNotification);

    juce::String r;
    if (ch < 1)
        r = "Open the recording to analyse.";
    else
    {
        const auto sel = app.selection;
        r = sel.getLength() > 1e-6
                ? "Selection " + juce::String (sel.getStart(), 2) + " s - " + juce::String (sel.getEnd(), 2) + " s ("
                      + juce::String (sel.getLength(), 2) + " s)"
                : "Whole file (" + juce::String (d->engine.duration(), 2) + " s)";
        // a sound map's reverberation take says which sweep it holds
        sc::room::SweepSpec spec;
        if (sc::room::SweepSpec::fromVar (d->soundMapPoint["sweep"], spec))
        {
            setMethod (Method::Sweep);
            setSweep (spec);
            r << "; the sweep is the one this take says it holds";
        }
    }
    range_.setText (r, juce::dontSendNotification);
    analyse_.setEnabled (ch >= 1);
}

bool ReverbView::analyse()
{
    refresh();
    bands_.clear();
    response_ = {};
    const auto fail = [this] (const juce::String& why)
    {
        status_.setText (why, juce::dontSendNotification);
        status_.setColour (juce::Label::textColourId, kWarn);
        export_.setEnabled (false);
        table_.updateContent();
        table_.repaint();
        plot_->show (nullptr);
        return false;
    };
    const FileDoc* d = app.doc();
    if (d == nullptr || ! d->engine.isFileLoaded()) return fail ("Open the recording to analyse.");
    const auto& eng = d->engine;
    const double fs = eng.sampleRate();
    const int channel = juce::jlimit (0, eng.numChannels() - 1, channel_.getSelectedId() - 1);
    int64_t from = 0, count = eng.numSamples();
    if (const auto sel = app.selection; sel.getLength() > 1e-6)
    {
        from = juce::jlimit ((int64_t) 0, eng.numSamples(), (int64_t) std::llround (sel.getStart() * fs));
        count = juce::jlimit ((int64_t) 0, eng.numSamples() - from, (int64_t) std::llround (sel.getLength() * fs));
    }
    if (count < 16) return fail ("Nothing to analyse.");
    if (count > (int64_t) 1 << 23) return fail ("Too long to analyse at once: select one sweep and the silence after it.");
    std::vector<float> samples ((size_t) count);
    if (! eng.readSamples (channel, from, count, samples.data())) return fail ("The file could not be read.");

    struct Wait   // a few seconds for a long recording
    {
        Wait() { juce::MouseCursor::showWaitCursor(); }
        ~Wait() { juce::MouseCursor::hideWaitCursor(); }
    } wait;
    if (method() == Method::Sweep)
    {
        const auto spec = sweep();
        if (! spec.valid()) return fail ("The sweep needs a start frequency, an end frequency at least twice as high, and a length (0.5 s or more).");
        response_ = sc::room::fromSweep (samples.data(), count, fs, spec);
    }
    else
        response_ = sc::room::fromImpulse (samples.data(), count, fs);
    if (! response_.ok()) return fail (response_.problem);

    sc::room::Options o;
    o.bands = bandsBox_.getSelectedId() == 2 ? sc::room::Bands::ThirdOctave : sc::room::Bands::Octave;
    o.lowestHz = 25.0;
    o.highestHz = 20000.0;   // what the sample rate and the sweep cover
    bands_ = sc::room::analyse (response_, o);
    if (bands_.empty()) return fail ("No band could be analysed.");

    int reliable = 0;
    for (const auto& b : bands_) if (b.nominalHz > 0.0 && b.t20.reliable) ++reliable;
    juce::String s = "The response starts " + juce::String ((double) from / fs + response_.startInRecording + response_.direct / fs, 3)
                     + " s into the file and lasts " + juce::String ((double) (response_.h.size() - (size_t) response_.direct) / fs, 2)
                     + " s. T20 is reliable in " + juce::String (reliable) + " of " + juce::String ((int) bands_.size() - 1)
                     + " bands. A time followed by ? is not reliable (see Notes).";
    status_.setText (s, juce::dontSendNotification);
    status_.setColour (juce::Label::textColourId, kInk);
    export_.setEnabled (true);
    table_.updateContent();
    table_.repaint();
    // the 1 kHz band to start with (the whole response when it is not there)
    int row = 0;
    for (size_t i = 0; i < bands_.size(); ++i) if (std::abs (bands_[i].nominalHz - 1000.0) < 1e-6) row = (int) i;
    table_.selectRow (row);
    plot_->show (&bands_[(size_t) row]);
    return true;
}

juce::String ReverbView::cellText (int row, int column) const
{
    if (! juce::isPositiveAndBelow (row, (int) bands_.size())) return {};
    const auto& b = bands_[(size_t) row];
    switch (column)
    {
        case 1:  return sc::room::bandName (b);
        case 2:  return timeText (b.edt);
        case 3:  return timeText (b.t20);
        case 4:  return timeText (b.t30);
        case 5:  return number (b.c50, 1);
        case 6:  return number (b.c80, 1);
        case 7:  return number (b.d50, 2);
        case 8:  return std::isnan (b.centreTimeMs) ? juce::String ("-") : juce::String (juce::roundToInt (b.centreTimeMs));
        case 9:  return juce::String (juce::roundToInt (b.inrDb));
        case 10: return notesFor (b);
        default: break;
    }
    return {};
}

void ReverbView::paintRowBackground (juce::Graphics& g, int row, int, int, bool selected)
{
    g.fillAll (selected ? juce::Colour (45, 116, 218).withAlpha (0.55f) : (row % 2 == 0 ? kPanel : kPanel.brighter (0.05f)));
}

void ReverbView::paintCell (juce::Graphics& g, int row, int column, int width, int height, bool)
{
    const auto text = cellText (row, column);
    const bool doubtful = (column >= 2 && column <= 4 && text.endsWith ("?")) || column == 10;
    g.setColour (doubtful ? kWarn : kInk);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (text, 6, 0, width - 8, height, column == 1 || column == 10 ? juce::Justification::centredLeft : juce::Justification::centredRight, true);
}

void ReverbView::selectedRowsChanged (int lastRow)
{
    plot_->show (juce::isPositiveAndBelow (lastRow, (int) bands_.size()) ? &bands_[(size_t) lastRow] : nullptr);
}

void ReverbView::exportCsv()
{
    if (bands_.empty()) return;
    const FileDoc* d = app.doc();
    const auto start = d != nullptr && d->file != juce::File()
                           ? d->file.getSiblingFile (d->file.getFileNameWithoutExtension() + "-reverberation.csv")
                           : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("reverberation.csv");
    chooser_ = std::make_unique<juce::FileChooser> ("Export the reverberation table", start, "*.csv", sc::files::useNativeDialogs());
    juce::Component::SafePointer<ReverbView> safe (this);
    chooser_->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                           [safe] (const juce::FileChooser& fc)
    {
        if (safe == nullptr) return;
        const auto f = fc.getResult();
        if (f == juce::File()) return;
        const bool ok = f.withFileExtension ("csv").replaceWithText (safe->csv());
        safe->status_.setText (ok ? "Saved " + f.withFileExtension ("csv").getFileName() + "." : "The CSV could not be written.", juce::dontSendNotification);
    });
}

void ReverbView::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
}

void ReverbView::resized()
{
    auto r = getLocalBounds().reduced (10);
    auto row = r.removeFromTop (24);
    methodLabel_.setBounds (row.removeFromLeft (72));
    method_.setBounds (row.removeFromLeft (230));
    row.removeFromLeft (12);
    channelLabel_.setBounds (row.removeFromLeft (58));
    channel_.setBounds (row.removeFromLeft (110));
    row.removeFromLeft (12);
    bandsLabel_.setBounds (row.removeFromLeft (44));
    bandsBox_.setBounds (row.removeFromLeft (110));
    r.removeFromTop (6);
    row = r.removeFromTop (24);
    sweepLabel_.setBounds (row.removeFromLeft (230));
    for (auto* e : { &startHz_, &endHz_, &seconds_ })
    {
        e->setBounds (row.removeFromLeft (70));
        row.removeFromLeft (6);
    }
    r.removeFromTop (6);
    range_.setBounds (r.removeFromTop (20));
    r.removeFromTop (4);
    auto buttons = r.removeFromBottom (28);
    close_.setBounds (buttons.removeFromRight (90));
    buttons.removeFromRight (8);
    export_.setBounds (buttons.removeFromRight (120));
    analyse_.setBounds (buttons.removeFromLeft (110));
    r.removeFromBottom (6);
    status_.setBounds (r.removeFromBottom (36));
    r.removeFromBottom (4);
    plot_->setBounds (r.removeFromBottom (juce::jmax (120, r.getHeight() * 2 / 5)));
    r.removeFromBottom (6);
    table_.setBounds (r);
}

//==============================================================================
ReverbWindow::ReverbWindow (Application& app)
    : juce::DocumentWindow ("Reverberation Analysis", kBg, juce::DocumentWindow::closeButton)
{
    view_ = std::make_unique<ReverbView> (app);
    view_->onClose = [this] { if (onCloseRequested) onCloseRequested(); };
    setUsingNativeTitleBar (true);
    setContentNonOwned (view_.get(), false);
    setResizable (true, false);
    centreWithSize (860, 640);
    setVisible (true);
}

ReverbWindow::~ReverbWindow()
{
    clearContentComponent();
}

void ReverbWindow::closeButtonPressed()
{
    if (onCloseRequested) onCloseRequested();
}
