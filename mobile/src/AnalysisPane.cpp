#include "AnalysisPane.h"
#include "MobileStyle.h"
#include "Measures.h"
#include "Telemetry.h"
#include <cmath>

namespace scm
{
// ------------------------------------------------------------------ worker

class AnalysisPane::Worker : public juce::Thread
{
public:
    struct Job
    {
        AnalysisMode mode = AnalysisMode::FftLog;
        double t0 = 0, t1 = 0;
        int channel = 0;
        int fftSize = 8192;
        ScaleCalibration cal;
        int serial = 0;
    };

    Worker (AnalysisPane& p) : juce::Thread ("analysis"), pane (p), document (p.document)
    {
        startThread (juce::Thread::Priority::low);
    }

    ~Worker() override
    {
        signalThreadShouldExit();
        cancel.store (true);
        wake.signal();
        stopThread (4000);
    }

    void submit (const Job& j)
    {
        {
            const juce::ScopedLock sl (lock);
            job = j;
            has = true;
        }
        cancel.store (true);
        wake.signal();
    }

    void pause()
    {
        cancel.store (true);
        const juce::ScopedLock rl (runLock);
        paused = true;
    }

    void resume()
    {
        const juce::ScopedLock rl (runLock);
        paused = false;
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            wake.wait (200);
            Job j;
            {
                const juce::ScopedLock sl (lock);
                if (! has) continue;
                j = job;
                has = false;
            }
            const juce::ScopedLock rl (runLock);
            if (paused) continue;
            cancel.store (false);
            auto* d = document.doc();
            if (d == nullptr || ! d->engine.isFileLoaded() || j.t1 <= j.t0) continue;
            const auto& eng = d->engine;
            const auto progress = [this] (double) { return ! cancel.load() && ! threadShouldExit(); };

            Curve c;
            c.mode = j.mode;
            c.t0 = j.t0;
            c.t1 = j.t1;
            c.channel = j.channel;
            c.serial = j.serial;
            const bool calibrated = j.cal.isCalibratedFor (j.channel);
            const double offset = calibrated ? j.cal.levelOffsetDb (j.channel) : 0.0;
            c.unit = calibrated ? juce::String (j.cal.levelUnitLabel()) : juce::String ("dBFS");
            const ScaleCalibration* calPtr = calibrated ? &j.cal : nullptr;

            if (j.mode == AnalysisMode::ThirdOctave)
            {
                sc::measures::ThirdOctaveParams p;
                p.t0 = j.t0; p.t1 = j.t1;
                p.channels = { j.channel };
                const bool pascal = calibrated && j.cal.unit == ScaleCalibration::Unit::Pascal;
                p.mode = pascal ? sc::measures::ThirdOctaveParams::Mode::Reference
                                : sc::measures::ThirdOctaveParams::Mode::Iec;
                p.fftSize = j.fftSize;
                const auto r = sc::measures::thirdOctave (eng, p, calPtr, progress);
                if (r.cancelled || r.channels.empty()) continue;
                const auto& ch = r.channels.front();
                for (size_t b = 0; b < r.centresHz.size() && b < ch.relDb.size(); ++b)
                {
                    if (b < ch.noData.size() && ch.noData[b]) continue;
                    c.f.push_back (r.centresHz[b]);
                    c.db.push_back ((double) ch.relDb[b] + ch.cal.levelOffsetDb);
                }
                c.fftSize = r.fftSize;
            }
            else
            {
                sc::measures::SpectrumParams p;
                p.t0 = j.t0; p.t1 = j.t1;
                p.channels = { j.channel };
                p.fftSize = j.fftSize;
                const auto r = sc::measures::spectrum (eng, p, calPtr, progress);
                if (r.cancelled || r.channels.empty()) continue;
                const auto& ch = r.channels.front();
                c.f.reserve (ch.relDb.size());
                c.db.reserve (ch.relDb.size());
                for (size_t k = 1; k < ch.relDb.size(); ++k)
                {
                    c.f.push_back ((double) k * r.binHz);
                    c.db.push_back (ch.relDb[k] + ch.cal.levelOffsetDb);
                }
                c.fftSize = r.fftSize;
            }
            if (cancel.load()) continue;

            sc::measures::LevelsParams lp;
            lp.t0 = j.t0; lp.t1 = j.t1;
            lp.channels = { j.channel };
            const auto lv = sc::measures::levels (eng, lp, calPtr, progress);
            if (! lv.cancelled && ! lv.channels.empty())
            {
                const auto& l = lv.channels.front();
                c.rmsDb = calibrated ? l.rmsAbsDb : l.rmsDb;
                c.peakDb = calibrated ? l.peakAbsDb : l.peakDb;
            }
            juce::ignoreUnused (offset);
            if (cancel.load()) continue;

            juce::Component::SafePointer<AnalysisPane> safe (&pane);
            juce::MessageManager::callAsync ([safe, c]() mutable
            {
                if (safe != nullptr) safe->curveReady (std::move (c));
            });
        }
    }

private:
    AnalysisPane& pane;
    MobileDocument& document;
    juce::WaitableEvent wake;
    juce::CriticalSection lock, runLock;
    Job job;
    bool has = false, paused = false;
    std::atomic<bool> cancel { false };
};

// ------------------------------------------------------------------ pane

AnalysisPane::AnalysisPane (MobileDocument& d, TimeViewport& v) : document (d), view (v)
{
    worker = std::make_unique<Worker> (*this);

    modeBox.addItem ("FFT Linear", 1);
    modeBox.addItem ("FFT Log", 2);
    modeBox.addItem ("1/3 Octave", 3);
    modeBox.setSelectedId (2, juce::dontSendNotification);
    modeBox.onChange = [this]
    {
        telemetry::capture ("analysis_settings", { { "mode", analysisModeName ((AnalysisMode) (modeBox.getSelectedId() - 1)) },
                                                   { "fft_size", 1024 << (fftBox.getSelectedId() - 1) } });
        request();
    };
    addAndMakeVisible (modeBox);

    for (int n = 1024, id = 1; n <= 65536; n *= 2, ++id)
        fftBox.addItem ("FFT " + juce::String (n), id);
    fftBox.setSelectedId (4, juce::dontSendNotification);   // 8192
    fftBox.onChange = [this]
    {
        telemetry::capture ("analysis_settings", { { "mode", analysisModeName ((AnalysisMode) (modeBox.getSelectedId() - 1)) },
                                                   { "fft_size", 1024 << (fftBox.getSelectedId() - 1) } });
        request();
    };
    addAndMakeVisible (fftBox);

    storeButton.onClick = [this] { storeCurrent(); };
    addAndMakeVisible (storeButton);

    summary.setFont (style::font (14.0f));
    summary.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (summary);

    document.addChangeListener (this);
    view.addChangeListener (this);
    rebuildChips();
}

AnalysisPane::~AnalysisPane()
{
    document.removeChangeListener (this);
    view.removeChangeListener (this);
    worker.reset();
}

void AnalysisPane::pauseAnalysis() { worker->pause(); }

void AnalysisPane::resumeAnalysis()
{
    worker->resume();
    current = {};
    fHi = std::min (std::max (fHi, 1000.0), document.sampleRate() * 0.5);
    rebuildChips();
    request();
}

void AnalysisPane::setLive (bool on)
{
    live = on;
    if (live) startTimerHz (4);
    else { stopTimer(); request(); }
}

void AnalysisPane::contentChanged()
{
    dirty = true;
    if (! live && isShowing()) request();
}

void AnalysisPane::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (live) return;
    if (isShowing()) request();
    if ((int) chips.size() != (int) document.plots().plots().size()) rebuildChips();
}

void AnalysisPane::timerCallback()
{
    if (live && isShowing()) request();
}

void AnalysisPane::request()
{
    if (! document.hasAudio()) return;
    Worker::Job j;
    j.mode = (AnalysisMode) (modeBox.getSelectedId() - 1);
    j.fftSize = 1024 << (fftBox.getSelectedId() - 1);
    j.cal = document.calibration();
    j.channel = 0;
    const double dur = document.duration();
    if (live)
    {
        j.t1 = dur;
        j.t0 = std::max (0.0, dur - 1.0);
    }
    else if (! document.selection().isEmpty())
    {
        j.t0 = document.selection().getStart();
        j.t1 = document.selection().getEnd();
    }
    else
    {
        j.t0 = std::max (0.0, view.start());
        j.t1 = std::min (dur, view.end());
    }
    if (j.t1 - j.t0 < 1e-3) return;
    j.serial = ++serial;
    worker->submit (j);
}

void AnalysisPane::curveReady (Curve c)
{
    if (c.serial < current.serial) return;
    current = std::move (c);
    // vertical range: 10 dB above the highest point, 100 dB tall
    double mx = -1e9;
    for (double v : current.db) mx = std::max (mx, v);
    for (const auto& p : document.plots().plots())
        if (p.visible) for (double v : p.db) mx = std::max (mx, v);
    if (mx > -900.0) dbTop = std::ceil ((mx + 5.0) / 10.0) * 10.0;
    const juce::String region = live ? juce::String ("last second")
        : juce::String (current.t0, 3) + " - " + juce::String (current.t1, 3) + " s";
    summary.setText ("RMS " + juce::String (current.rmsDb, 1) + " " + current.unit
                     + "   Peak " + juce::String (current.peakDb, 1) + " " + current.unit
                     + "   (" + region + ")", juce::dontSendNotification);
    repaint();
}

void AnalysisPane::storeCurrent()
{
    if (current.f.empty()) return;
    auto& plots = document.plots();
    if (plots.full())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Stored plots",
            "All " + juce::String (StoredPlots::kMaxPlots) + " slots are used: delete a plot first.");
        return;
    }
    StoredPlot p;
    p.mode = current.mode;
    p.t0 = current.t0;
    p.t1 = current.t1;
    p.channel = current.channel;
    p.fftSize = current.fftSize;
    p.unit = current.unit;
    p.freqs = current.f;
    p.db = current.db;
    plots.add (std::move (p));
    telemetry::capture ("plot_stored", { { "mode", analysisModeName (current.mode) },
                                         { "count", (int) plots.plots().size() } });
    document.plotsChanged();
    rebuildChips();
    repaint();
}

void AnalysisPane::rebuildChips()
{
    for (auto* c : chips) removeChildComponent (c);
    chips.clear();
    const auto& list = document.plots().plots();
    for (int i = 0; i < (int) list.size(); ++i)
    {
        auto* b = chips.add (new juce::TextButton (list[(size_t) i].name));
        b->setColour (juce::TextButton::buttonColourId,
                      style::plotColour (list[(size_t) i].colourIndex).withAlpha (list[(size_t) i].visible ? 0.55f : 0.15f));
        b->onClick = [this, i] { showPlotMenu (i); };
        addAndMakeVisible (b);
    }
    storeButton.setEnabled (! document.plots().full());
    resized();
}

void AnalysisPane::showPlotMenu (int index)
{
    const auto& list = document.plots().plots();
    if (! juce::isPositiveAndBelow (index, (int) list.size())) return;
    const auto& p = list[(size_t) index];
    juce::PopupMenu m;
    m.addSectionHeader (p.name + " - " + analysisModeName (p.mode) + ", " + p.unit);
    m.addItem (1, p.visible ? "Hide" : "Show");
    m.addItem (2, "Rename...");
    m.addItem (3, "Delete");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (chips[index]),
                     [this, index] (int r)
    {
        auto& plots = document.plots();
        if (r == 1) plots.setVisible (index, ! plots.plots()[(size_t) index].visible);
        else if (r == 3) plots.remove (index);
        else if (r == 2)
        {
            auto* w = new juce::AlertWindow ("Rename plot", {}, juce::MessageBoxIconType::NoIcon);
            w->addTextEditor ("name", plots.plots()[(size_t) index].name);
            w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton ("Cancel", 0);
            juce::Component::SafePointer<AnalysisPane> safe (this);
            w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, index] (int res)
            {
                if (safe != nullptr && res == 1)
                {
                    safe->document.plots().rename (index, w->getTextEditorContents ("name"));
                    safe->document.plotsChanged();
                    safe->rebuildChips();
                }
            }), true);
            return;
        }
        if (r > 0)
        {
            document.plotsChanged();
            rebuildChips();
            repaint();
        }
    });
}

void AnalysisPane::resized()
{
    auto r = getLocalBounds().reduced (style::kGap);
    auto row = r.removeFromTop (style::kTouch);
    modeBox.setBounds (row.removeFromLeft (std::min (150, row.getWidth() / 3)));
    row.removeFromLeft (style::kGap);
    fftBox.setBounds (row.removeFromLeft (std::min (140, row.getWidth() / 2)));
    row.removeFromLeft (style::kGap);
    storeButton.setBounds (row.removeFromLeft (90));
    r.removeFromTop (style::kGap);
    summary.setBounds (r.removeFromTop (22));
    if (! chips.isEmpty())
    {
        auto chipRow = r.removeFromBottom (36);
        const int w = std::min (110, chipRow.getWidth() / std::max (1, chips.size()));
        for (auto* c : chips) c->setBounds (chipRow.removeFromLeft (w).reduced (2));
    }
}

juce::Rectangle<float> AnalysisPane::plotArea() const
{
    auto r = getLocalBounds().reduced (style::kGap).toFloat();
    r.removeFromTop ((float) style::kTouch + style::kGap + 22.0f);
    if (! chips.isEmpty()) r.removeFromBottom (36.0f);
    r.removeFromLeft (40.0f);    // dB labels
    r.removeFromBottom (18.0f);  // frequency labels
    return r;
}

double AnalysisPane::freqToX (double f) const
{
    const auto p = plotArea();
    const auto mode = (AnalysisMode) (modeBox.getSelectedId() - 1);
    if (mode == AnalysisMode::FftLinear)
        return p.getX() + (f - 0.0) / std::max (1.0, fHi) * p.getWidth();
    const double lo = std::max (1.0, fLo);
    return p.getX() + (std::log (std::max (1e-3, f)) - std::log (lo)) / std::log (fHi / lo) * p.getWidth();
}

double AnalysisPane::xToFreq (double x) const
{
    const auto p = plotArea();
    const double t = (x - p.getX()) / std::max (1.0f, p.getWidth());
    const auto mode = (AnalysisMode) (modeBox.getSelectedId() - 1);
    if (mode == AnalysisMode::FftLinear) return t * fHi;
    const double lo = std::max (1.0, fLo);
    return lo * std::pow (fHi / lo, t);
}

double AnalysisPane::dbToY (double db) const
{
    const auto p = plotArea();
    return p.getY() + (dbTop - db) / dbRange * p.getHeight();
}

void AnalysisPane::drawCurve (juce::Graphics& g, const std::vector<double>& f, const std::vector<double>& db,
                              AnalysisMode mode, juce::Colour c, float thickness)
{
    const auto plot = plotArea();
    g.setColour (c);
    if (mode == AnalysisMode::ThirdOctave)
    {
        const double k = std::pow (2.0, 1.0 / 6.0);
        for (size_t i = 0; i < f.size(); ++i)
        {
            const float x0 = (float) freqToX (f[i] / k), x1 = (float) freqToX (f[i] * k);
            const float y = (float) dbToY (db[i]);
            if (x1 < plot.getX() || x0 > plot.getRight()) continue;
            g.drawRect (juce::Rectangle<float> (x0 + 1.0f, y, std::max (1.0f, x1 - x0 - 2.0f), plot.getBottom() - y)
                            .getIntersection (plot), thickness);
        }
        return;
    }
    juce::Path path;
    bool started = false;
    float lastX = -1e9f, colMin = 0, colMax = 0;
    for (size_t i = 0; i < f.size(); ++i)
    {
        const float x = (float) freqToX (f[i]);
        if (x < plot.getX() - 2.0f || x > plot.getRight() + 2.0f) continue;
        const float y = (float) juce::jlimit ((double) plot.getY(), (double) plot.getBottom(), dbToY (db[i]));
        if (! started) { path.startNewSubPath (x, y); started = true; lastX = x; colMin = colMax = y; continue; }
        if (x - lastX < 0.75f)   // many bins per pixel: draw the column's extent
        {
            colMin = std::min (colMin, y); colMax = std::max (colMax, y);
            continue;
        }
        if (colMax - colMin > 0.5f) { path.lineTo (lastX, colMin); path.lineTo (lastX, colMax); }
        path.lineTo (x, y);
        lastX = x; colMin = colMax = y;
    }
    g.strokePath (path, juce::PathStrokeType (thickness));
}

void AnalysisPane::paint (juce::Graphics& g)
{
    g.fillAll (style::panelBackground);
    const auto plot = plotArea();
    g.setColour (style::plotBackground);
    g.fillRect (plot);
    const auto mode = (AnalysisMode) (modeBox.getSelectedId() - 1);

    // grid + labels
    g.setFont (style::font (11.0f));
    for (double db = std::floor (dbTop / 10.0) * 10.0; db >= dbTop - dbRange; db -= 10.0)
    {
        const float y = (float) dbToY (db);
        g.setColour (style::grid);
        g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
        g.setColour (style::axisLabel);
        g.drawText (juce::String ((int) db), juce::Rectangle<float> (plot.getX() - 40.0f, y - 7.0f, 36.0f, 14.0f),
                    juce::Justification::centredRight);
    }
    std::vector<double> ticks;
    if (mode == AnalysisMode::FftLinear)
    {
        const double rough = fHi / 6.0, p10 = std::pow (10.0, std::floor (std::log10 (rough)));
        const double step = rough / p10 < 2 ? 2 * p10 : rough / p10 < 5 ? 5 * p10 : 10 * p10;
        for (double f = step; f < fHi; f += step) ticks.push_back (f);
    }
    else
        for (double dec = 10.0; dec < 1e5; dec *= 10.0)
            for (double m : { 1.0, 2.0, 5.0 })
                if (m * dec >= fLo && m * dec <= fHi) ticks.push_back (m * dec);
    for (double f : ticks)
    {
        const float x = (float) freqToX (f);
        g.setColour (style::grid);
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        g.setColour (style::axisLabel);
        const auto t = f >= 1000.0 ? juce::String (f / 1000.0, f >= 10000.0 || std::fmod (f, 1000.0) == 0.0 ? 0 : 1) + "k"
                                   : juce::String ((int) f);
        const float lx = juce::jlimit (plot.getX() - 20.0f, plot.getRight() - 40.0f, x - 20.0f);
        g.drawText (t, juce::Rectangle<float> (lx, plot.getBottom() + 2.0f, 40.0f, 14.0f),
                    juce::Justification::centred);
    }

    // stored plots of the same family (FFT on the FFT modes, bands on 1/3 octave)
    g.saveState();
    g.reduceClipRegion (plot.toNearestInt());
    for (const auto& p : document.plots().plots())
    {
        if (! p.visible) continue;
        const bool bands = p.mode == AnalysisMode::ThirdOctave;
        if (bands != (mode == AnalysisMode::ThirdOctave)) continue;
        drawCurve (g, p.freqs, p.db, bands ? AnalysisMode::ThirdOctave : mode,
                   style::plotColour (p.colourIndex).withAlpha (0.85f), 1.5f);
    }
    if (! current.f.empty() && (current.mode == AnalysisMode::ThirdOctave) == (mode == AnalysisMode::ThirdOctave))
        drawCurve (g, current.f, current.db, mode, juce::Colours::white, 1.8f);
    g.restoreState();

    if (! document.hasAudio())
    {
        g.setColour (style::textDim);
        g.drawFittedText ("No audio to analyse", plot.toNearestInt(), juce::Justification::centred, 1);
    }

    // touch cursor: frequency and level of the current curve
    if (cursorOn && ! current.f.empty())
    {
        const double f = xToFreq (cursor.x);
        size_t best = 0;
        double bestD = 1e300;
        for (size_t i = 0; i < current.f.size(); ++i)
            if (const double dd = std::abs (std::log (std::max (1e-3, current.f[i])) - std::log (std::max (1e-3, f))); dd < bestD)
            { bestD = dd; best = i; }
        const float x = (float) freqToX (current.f[best]), y = (float) dbToY (current.db[best]);
        g.setColour (style::selectionEdge);
        g.drawVerticalLine (juce::roundToInt (x), plot.getY(), plot.getBottom());
        g.fillEllipse (x - 4.0f, y - 4.0f, 8.0f, 8.0f);
        const auto text = juce::String (current.f[best], current.f[best] < 100.0 ? 1 : 0) + " Hz   "
                        + juce::String (current.db[best], 1) + " " + current.unit;
        g.setFont (style::font (14.0f, true));
        auto box = juce::Rectangle<float> (0, 0, 170.0f, 22.0f).withPosition (
            juce::jlimit (plot.getX(), plot.getRight() - 170.0f, x + 8.0f), plot.getY() + 4.0f);
        g.setColour (style::barBackground.withAlpha (0.9f));
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour (style::text);
        g.drawText (text, box, juce::Justification::centred);
    }
}

void AnalysisPane::mouseDown (const juce::MouseEvent& e)
{
    if (plotArea().contains (e.position))
    {
        cursorOn = true;
        cursor = e.position;
        repaint();
    }
}

void AnalysisPane::mouseDrag (const juce::MouseEvent& e)
{
    if (! cursorOn) return;
    cursor = e.position;
    repaint();
}

void AnalysisPane::mouseUp (const juce::MouseEvent& e)
{
    if (e.getNumberOfClicks() >= 2) { fLo = 20.0; fHi = document.sampleRate() * 0.5; }
    cursorOn = false;
    repaint();
}
} // namespace scm
