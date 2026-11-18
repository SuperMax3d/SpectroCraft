/*
 * ModelPanel.cpp - the Model panel (issue #40). See include/ModelPanel.h and
 * docs/model-panel.md.
 */
#include "ModelPanel.h"
#include "Application.h"

#include <algorithm>
#include <cmath>

namespace md = sc::model;

namespace
{
    const juce::Colour kBg       = juce::Colour::fromRGB (45, 45, 48);
    const juce::Colour kText     = juce::Colour::fromRGB (225, 225, 230);
    const juce::Colour kDim      = juce::Colour::fromRGB (150, 150, 160);
    const juce::Colour kSelected = juce::Colour::fromRGB (60, 80, 120);
    const juce::Colour kError    = juce::Colour::fromRGB (255, 120, 110);

    // Curve colours of the plot (one per line / state).
    juce::Colour curveColour (int i)
    {
        static const juce::uint32 c[] = { 0xff5ad2ff, 0xffffc83c, 0xff8ce06e, 0xffff7f7f,
                                          0xffc89bff, 0xffff9f40, 0xff40e0c0, 0xffe0e070 };
        return juce::Colour (c[(size_t) i % (sizeof (c) / sizeof (c[0]))]);
    }

    juce::String rangeText (const md::TimeRange& r)
    {
        auto t = [] (double s)
        {
            const int m = (int) (s / 60.0);
            return juce::String (m) + ":" + juce::String (s - 60.0 * m, 3).paddedLeft ('0', 6);
        };
        return t (r.t0) + " - " + t (r.t1);
    }

    bool parseDouble (const juce::String& s, double& out)
    {
        const auto t = s.trim();
        if (t.isEmpty() || ! t.containsOnly ("0123456789.-+eE")) return false;
        out = t.getDoubleValue();
        return std::isfinite (out);
    }

    juce::String ordersText (const md::Component& c)
    {
        juce::StringArray a;
        for (const auto& l : c.lines)
            if (! l.isTrack())
                a.add (std::abs (l.order - std::round (l.order)) < 1e-9 ? juce::String ((int) std::lround (l.order))
                                                                        : juce::String (l.order, 2));
        return a.joinIntoString (", ");
    }

    void styleLabel (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setColour (juce::Label::textColourId, kText);
        l.setFont (juce::Font (juce::FontOptions (12.0f)));
    }
}

// ----------------------------------------------------------- ranges list

class ModelPanel::Ranges : public juce::ListBoxModel
{
public:
    explicit Ranges (ModelPanel& p) : panel (p) {}
    int getNumRows() override
    {
        const auto* c = panel.current();
        return c != nullptr ? (int) c->ranges.size() : 0;
    }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        const auto* c = panel.current();
        if (c == nullptr || row < 0 || row >= (int) c->ranges.size()) return;
        if (selected) g.fillAll (kSelected);
        g.setColour (kText);
        g.setFont (12.0f);
        juce::String text = rangeText (c->ranges[(size_t) row]);
        if (! c->isHarmonic() && c->analysed)
            for (const auto& s : c->states)
                if (std::abs (s.range.t0 - c->ranges[(size_t) row].t0) < 1e-9
                    && std::abs (s.range.t1 - c->ranges[(size_t) row].t1) < 1e-9 && ! c->parameter.isEmpty())
                    text << "   (state at " << juce::String (s.param, 2) << ")";
        g.drawText (text, 6, 0, w - 8, h, juce::Justification::centredLeft);
    }
private:
    ModelPanel& panel;
};

// ---------------------------------------------------------------- plot

// What the selected component learned: level vs parameter for each harmonic
// line (dashed: rising / falling), or the spectrum of each noise state.
class ModelPanel::Plot : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Plot (ModelPanel& p) : panel (p)
    {
        setTooltip ("What the analysis learned: the level of each harmonic against the parameter "
                    "(dashed: while it rises / falls), or the spectrum of each noise state.");
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour::fromRGB (30, 30, 34));
        g.setColour (juce::Colour::fromRGB (70, 70, 78));
        g.drawRect (getLocalBounds());
        const auto* c = panel.current();
        const auto* m = panel.model();
        g.setFont (11.0f);
        if (c == nullptr || m == nullptr || ! c->analysed)
        {
            g.setColour (kDim);
            g.drawText ("Not analysed yet", getLocalBounds(), juce::Justification::centred);
            return;
        }
        const auto area = getLocalBounds().toFloat().reduced (34.0f, 10.0f).withTrimmedBottom (8.0f);

        struct Series { std::vector<double> x, y; juce::Colour col; bool dashed; juce::String label; };
        std::vector<Series> series;
        bool logX = false;
        if (c->isHarmonic())
        {
            int i = 0;
            for (const auto& l : c->lines)
            {
                const auto col = curveColour (i++);
                if (! l.levelDb.empty()) series.push_back ({ l.levelDb.p, l.levelDb.v, col, false, l.label() });
                if (! l.accelDb.empty()) series.push_back ({ l.accelDb.p, l.accelDb.v, col, true, {} });
                if (! l.decelDb.empty()) series.push_back ({ l.decelDb.p, l.decelDb.v, col.darker (0.4f), true, {} });
            }
        }
        else
        {
            logX = true;
            int i = 0;
            for (const auto& s : c->states)
            {
                Series se { {}, {}, curveColour (i++), false,
                            c->parameter.isEmpty() ? juce::String() : juce::String (s.param, 1) };
                for (size_t b = 0; b < s.psdDb.size() && b < m->bandHz.size(); ++b)
                {
                    se.x.push_back (m->bandHz[b]);
                    se.y.push_back (s.psdDb[b]);
                }
                series.push_back (std::move (se));
            }
        }
        double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
        for (const auto& s : series)
            for (size_t i = 0; i < s.x.size(); ++i)
            {
                const double x = logX ? std::log10 (std::max (1.0, s.x[i])) : s.x[i];
                x0 = std::min (x0, x); x1 = std::max (x1, x);
                if (s.y[i] > -190.0) { y0 = std::min (y0, s.y[i]); y1 = std::max (y1, s.y[i]); }
            }
        if (series.empty() || ! (x1 >= x0) || ! (y1 >= y0))
        {
            g.setColour (kDim);
            g.drawText ("Nothing measured", getLocalBounds(), juce::Justification::centred);
            return;
        }
        if (x1 - x0 < 1e-9) { x0 -= 1.0; x1 += 1.0; }
        y0 = std::floor (y0 / 10.0) * 10.0;
        y1 = std::ceil (y1 / 10.0) * 10.0;
        if (y1 - y0 < 10.0) y1 = y0 + 10.0;
        auto X = [&] (double x) { return area.getX() + (float) ((x - x0) / (x1 - x0)) * area.getWidth(); };
        auto Y = [&] (double y) { return area.getBottom() - (float) ((y - y0) / (y1 - y0)) * area.getHeight(); };

        g.setColour (kDim);
        g.drawText (juce::String ((int) y1) + " dB", 0, (int) area.getY() - 6, 32, 12, juce::Justification::centredRight);
        g.drawText (juce::String ((int) y0), 0, (int) area.getBottom() - 6, 32, 12, juce::Justification::centredRight);
        auto xText = [&] (double x) { return logX ? juce::String ((int) std::lround (std::pow (10.0, x))) + " Hz"
                                                  : juce::String (x, 1); };
        g.drawText (xText (x0), (int) area.getX(), (int) area.getBottom() + 2, 80, 12, juce::Justification::centredLeft);
        g.drawText (xText (x1), (int) area.getRight() - 80, (int) area.getBottom() + 2, 80, 12,
                    juce::Justification::centredRight);
        if (! logX && ! c->parameter.isEmpty())
            g.drawText (c->parameter, (int) area.getCentreX() - 80, (int) area.getBottom() + 2, 160, 12,
                        juce::Justification::centred);

        int legend = 0;
        for (const auto& s : series)
        {
            g.setColour (s.col);
            juce::Path path;
            for (size_t i = 0; i < s.x.size(); ++i)
            {
                const double x = logX ? std::log10 (std::max (1.0, s.x[i])) : s.x[i];
                const float px = X (x), py = Y (std::max (y0, s.y[i]));
                if (i == 0) path.startNewSubPath (px, py); else path.lineTo (px, py);
                if (s.x.size() == 1) g.fillEllipse (px - 3.0f, py - 3.0f, 6.0f, 6.0f);
            }
            if (s.dashed)
            {
                juce::Path dashed;
                const float dashes[] = { 4.0f, 3.0f };
                juce::PathStrokeType (1.0f).createDashedStroke (dashed, path, dashes, 2);
                g.fillPath (dashed);
            }
            else
                g.strokePath (path, juce::PathStrokeType (1.5f));
            if (s.label.isNotEmpty() && legend < 8)
            {
                g.drawText (s.label, (int) area.getRight() - 120, (int) area.getY() + 12 * legend, 118, 12,
                            juce::Justification::centredRight);
                ++legend;
            }
        }
    }

private:
    ModelPanel& panel;
};

// ---------------------------------------------------------------- panel

// The chevron left of the report: hides or shows the plot and the Analysis
// settings below it, like the Video panel's button over its tracks (issue #51).
class ModelPanel::CollapseButton : public juce::Component, public juce::SettableTooltipClient
{
public:
    bool collapsed = false;
    std::function<void()> onClick;

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour::fromRGB (38, 42, 50));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 3.0f);
        g.setColour (juce::Colours::white.withAlpha (0.75f));
        const float cx = getWidth() * 0.5f, cy = getHeight() * 0.5f;
        // Pointing down while the bottom is shown (a click hides it), up when hidden.
        g.addTransform (juce::AffineTransform::rotation (collapsed ? -1.5708f : 1.5708f, cx, cy));
        for (int k = 0; k < 2; ++k)
        {
            const float x0 = cx - 4.0f + (float) k * 4.0f;
            juce::Path p;
            p.startNewSubPath (x0, cy - 3.5f);
            p.lineTo (x0 + 3.0f, cy);
            p.lineTo (x0, cy + 3.5f);
            g.strokePath (p, juce::PathStrokeType (1.4f));
        }
    }
    void mouseDown (const juce::MouseEvent&) override { if (onClick != nullptr) onClick(); }
};

ModelPanel::ModelPanel (Application& appIn) : app (appIn)
{
    struct B { juce::Button* b; const char* id; const char* tip; };
    const B buttons[] = {
        { &addHarmBtn, "model-add-harmonic", "Add a harmonic component: lines at multiples of a fundamental that follows a parameter (a fan, an engine)." },
        { &addNoiseBtn, "model-add-noise", "Add a noise component: a broadband spectrum per time range (state), following a parameter." },
        { &analyseAllBtn, "model-analyse-all", "Analyse every component, top to bottom: each one is measured with the components above it taken out." },
        { &exportBtn, "model-export", "Save the model as a JSON file with everything needed to reproduce the simulation." },
        { &importBtn, "model-import", "Replace this file's model with one saved as JSON." },
        { &compareBtn, "model-compare", "New file: the simulation (channel 1) and the original (channel 2) of every range used for the analysis, with their markers and parameters." },
        { &renderSelBtn, "model-render-selection", "Render the simulation of the selected time range into a new channel of this file (undoable)." },
        { &renderCsvBtn, "model-render-csv", "New file: the simulation driven by the parameters of a CSV file (time column + one column per parameter)." },
        { &addRangeBtn, "model-add-range", "Add the current time selection to this component's ranges (for noise, each range is one state)." },
        { &removeRangeBtn, "model-remove-range", "Remove the selected range." },
        { &pickF0Btn, "model-pick-f0", "Click the fundamental on the spectrogram: it snaps to the strongest peak nearby, at that time's parameter value. Esc cancels." },
        { &pickHarmBtn, "model-pick-harmonics", "Click harmonics on the spectrogram to add or remove them; a guide follows the mouse and snaps to the nearest one. Esc or this button again stops." },
        { &addTrackBtn, "model-add-track", "Add a line measured inside stored spectral selections: pick a group (its selections are pieces of the same harmonic) or one selection." },
        { &removeTrackBtn, "model-remove-track", "Remove a track line." },
        { &analyseBtn, "model-analyse", "Analyse this component (the components above it are taken out first)." },
        { &dynamicsTgl, "model-dynamics", "Learn separate levels while the parameter rises, falls and holds (acceleration / deceleration / steady)." },
        { &variationTgl, "model-variation", "Render with the level and frequency variation measured around the learned curves." },
    };
    for (const auto& b : buttons)
    {
        b.b->setComponentID (b.id);
        b.b->setTooltip (b.tip);
    }
    for (auto* b : { &addHarmBtn, &addNoiseBtn, &analyseAllBtn, &exportBtn, &importBtn,
                     &compareBtn, &renderSelBtn, &renderCsvBtn })
        addAndMakeVisible (b);

    auto& header = table.getHeader();
    const int flags = juce::TableHeaderComponent::notResizableOrSortable;
    header.addColumn ("On", 1, 30, 30, -1, flags);
    header.addColumn ("Type", 2, 62, 40, -1, flags);
    header.addColumn ("Name", 3, 110, 60, -1, flags);
    header.addColumn ("Parameter", 4, 100, 60, -1, flags);
    header.addColumn ("State", 5, 70, 40, -1, flags);
    table.setRowHeight (20);
    table.setColour (juce::ListBox::backgroundColourId, juce::Colour::fromRGB (30, 30, 34));
    table.setTooltip ("The components, in analysis order: each one is measured with the ones above it taken out. "
                      "Click On to switch one off; right-click for Analyse, Move Up / Down, Duplicate, Delete.");
    addAndMakeVisible (table);

    // Detail.
    styleLabel (nameLab, "Name");
    styleLabel (paramLab, "Parameter");
    styleLabel (channelLab, "Channel");
    styleLabel (rangesLab, "Ranges");
    styleLabel (f0Lab, "Fundamental");
    styleLabel (f0AtLab, "Hz at");
    styleLabel (ordersLab, "Harmonics");
    styleLabel (tracksLab, "Tracks");
    styleLabel (settingsLab, "Analysis");
    for (auto* l : { &nameLab, &paramLab, &channelLab, &rangesLab, &f0Lab, &f0AtLab, &ordersLab, &tracksLab,
                     &settingsLab, &tracksText, &report })
        content.addAndMakeVisible (l);
    tracksText.setColour (juce::Label::textColourId, kText);
    tracksText.setFont (juce::Font (juce::FontOptions (12.0f)));
    report.setColour (juce::Label::textColourId, kDim);
    report.setFont (juce::Font (juce::FontOptions (12.0f)));
    report.setJustificationType (juce::Justification::topLeft);
    report.setTooltip ("What the last analysis found.");
    collapseBtn = std::make_unique<CollapseButton>();
    collapseBtn->setComponentID ("model-collapse");
    collapseBtn->setTooltip ("Hide or show the plot and the analysis settings");
    collapseBtn->onClick = [this] { setBottomCollapsed (! bottomCollapsed_); };
    content.addAndMakeVisible (*collapseBtn);
    if (app.recentStore != nullptr)
        bottomCollapsed_ = app.recentStore->getBoolValue ("model_bottom_collapsed", false);
    collapseBtn->collapsed = bottomCollapsed_;
    // Rows can be dragged to reorder the components.
    table.addMouseListener (this, true);
    tracksText.setTooltip ("Lines measured inside stored spectral selections.");
    settingsLab.setTooltip ("Analysis settings of the whole model.");

    struct E { juce::TextEditor* e; const char* id; const char* tip; };
    const E editors[] = {
        { &nameEd, "model-name", "The component's name." },
        { &f0Ed, "model-f0", "The fundamental frequency, in Hz, at the parameter value on the right: it scales with the parameter." },
        { &f0AtEd, "model-f0-at", "The parameter value at which the fundamental has the frequency on the left." },
        { &ordersEd, "model-orders", "The harmonics to model, as orders of the fundamental: 1, 2, 3 (decimals allowed: 0.5, 1.5)." },
        { &tolEd, "model-tolerance", "How far from its nominal frequency a harmonic is searched, in % of that frequency." },
        { &steadyEd, "model-steady", "Below this rate (in % of the learned parameter span per second) the parameter counts as steady." },
    };
    for (const auto& e : editors)
    {
        e.e->setComponentID (e.id);
        e.e->setTooltip (e.tip);
        e.e->setFont (juce::Font (juce::FontOptions (13.0f)));
        e.e->onReturnKey = [this] { commitEdits(); };
        e.e->onFocusLost = [this] { commitEdits(); };
        content.addAndMakeVisible (e.e);
    }
    paramCombo.setComponentID ("model-param");
    paramCombo.setTooltip ("The Tables panel parameter the component follows (fan speed, engine speed...). "
                           "(none): the component does not change.");
    channelCombo.setComponentID ("model-channel");
    channelCombo.setTooltip ("The channel the component is measured on.");
    fftCombo.setComponentID ("model-fft");
    fftCombo.setTooltip ("FFT size of the analysis: larger separates close harmonics, smaller follows fast changes.");
    for (int o = 10; o <= 15; ++o) fftCombo.addItem (juce::String (1 << o), o);
    for (auto* c : { &paramCombo, &channelCombo, &fftCombo }) content.addAndMakeVisible (c);
    paramCombo.onChange = [this]
    {
        if (loading_) return;
        if (auto* c = current())
        {
            const int id = paramCombo.getSelectedId();
            const juce::String p = id <= 1 ? juce::String() : paramCombo.getItemText (paramCombo.indexOfItemId (id));
            if (p != c->parameter) { c->parameter = p; changed (true); }
        }
    };
    channelCombo.onChange = [this]
    {
        if (loading_) return;
        if (auto* c = current())
        {
            const int ch = channelCombo.getSelectedId() - 1;
            if (ch >= 0 && ch != c->channel) { c->channel = ch; changed (true); }
        }
    };
    fftCombo.onChange = [this]
    {
        if (loading_) return;
        if (auto* m = model())
        {
            const int o = fftCombo.getSelectedId();
            if (o >= 10 && o != m->fftOrder)
            {
                m->fftOrder = o;
                for (auto& c : m->components) c.analysed = false;
                app.soundModelChanged();
                showStatus ("FFT size changed: Analyse All to measure again.");
                refresh();
            }
        }
    };

    rangesModel = std::make_unique<Ranges> (*this);
    rangesList = std::make_unique<juce::ListBox> ("model-ranges", rangesModel.get());
    rangesList->setRowHeight (18);
    rangesList->setColour (juce::ListBox::backgroundColourId, juce::Colour::fromRGB (30, 30, 34));
    rangesList->setTooltip ("The time ranges measured. For a noise component each range is one state, "
                            "taken at its mean parameter value.");
    content.addAndMakeVisible (*rangesList);

    for (auto* b : { &addRangeBtn, &removeRangeBtn, &pickF0Btn, &pickHarmBtn, &addTrackBtn, &removeTrackBtn,
                     &analyseBtn })
        content.addAndMakeVisible (b);
    for (auto* t : { &dynamicsTgl, &variationTgl })
    {
        t->setColour (juce::ToggleButton::textColourId, kText);
        content.addAndMakeVisible (t);
    }
    plot = std::make_unique<Plot> (*this);
    content.addAndMakeVisible (*plot);
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);

    status.setColour (juce::Label::textColourId, kDim);
    status.setFont (juce::Font (juce::FontOptions (12.0f)));
    status.setTooltip ("The result of the last action.");
    addAndMakeVisible (status);

    // Actions.
    addHarmBtn.onClick = [this] { addComponent (md::Kind::Harmonic); };
    addNoiseBtn.onClick = [this] { addComponent (md::Kind::Noise); };
    analyseAllBtn.onClick = [this] { analyseAll(); };
    analyseBtn.onClick = [this] { analyseSelected(); };
    compareBtn.onClick = [this] { compare(); };
    renderSelBtn.onClick = [this] { renderSelection(); };
    renderCsvBtn.onClick = [this]
    {
        if (! ensureAnalysed()) return;
        chooseFile (false, "Render the model from a CSV", "*.csv;*.tsv;*.txt", [this] (const juce::File& f)
        {
            juce::String err;
            if (app.renderModelCsv (f, &err)) showStatus ("Rendered " + f.getFileName() + " into a new file.");
            else showStatus (err, true);
        });
    };
    exportBtn.onClick = [this]
    {
        if (! ensureAnalysed()) return;
        chooseFile (true, "Export the sound model", "*.json", [this] (const juce::File& f)
        {
            juce::String err;
            const auto target = f.hasFileExtension ("json") ? f : f.withFileExtension ("json");
            if (app.exportModel (target, &err)) showStatus ("Exported " + target.getFileName() + ".");
            else showStatus (err, true);
        });
    };
    importBtn.onClick = [this]
    {
        chooseFile (false, "Import a sound model", "*.json", [this] (const juce::File& f)
        {
            juce::String err;
            if (app.importModel (f, &err)) { showStatus ("Imported " + f.getFileName() + "."); refresh(); }
            else showStatus (err, true);
        });
    };
    addRangeBtn.onClick = [this]
    {
        auto* c = current();
        if (c == nullptr) return;
        if (app.selection.isEmpty()) { showStatus ("Select a time range first.", true); return; }
        c->ranges.push_back ({ app.selection.getStart(), app.selection.getEnd() });
        changed (true);
        rangesList->selectRow ((int) c->ranges.size() - 1);
    };
    removeRangeBtn.onClick = [this]
    {
        auto* c = current();
        const int r = rangesList->getSelectedRow();
        if (c == nullptr || r < 0 || r >= (int) c->ranges.size()) return;
        c->ranges.erase (c->ranges.begin() + r);
        changed (true);
    };
    pickF0Btn.onClick = [this] { togglePick (1); };
    pickHarmBtn.onClick = [this] { togglePick (2); };
    addTrackBtn.onClick = [this] { showTrackMenu (false); };
    removeTrackBtn.onClick = [this] { showTrackMenu (true); };
    dynamicsTgl.onClick = [this]
    {
        if (loading_) return;
        if (auto* c = current()) { c->dynamics = dynamicsTgl.getToggleState(); changed (true); }
    };
    variationTgl.onClick = [this]
    {
        if (loading_) return;
        if (auto* c = current()) { c->variation = variationTgl.getToggleState(); changed (false); }
    };

    refresh();
}

ModelPanel::~ModelPanel()
{
    table.setModel (nullptr);
    rangesList->setModel (nullptr);
}

md::Model* ModelPanel::model() const { return app.soundModel(); }

md::Component* ModelPanel::current() const
{
    auto* m = model();
    return m != nullptr ? m->find (selectedId_) : nullptr;
}

int ModelPanel::selectedIndex() const
{
    auto* m = model();
    return m != nullptr ? m->indexOf (selectedId_) : -1;
}

void ModelPanel::selectIndex (int index)
{
    auto* m = model();
    if (m == nullptr || index < 0 || index >= (int) m->components.size()) selectedId_.clear();
    else selectedId_ = m->components[(size_t) index].id;
    refresh();
}

void ModelPanel::showStatus (const juce::String& text, bool error)
{
    status.setText (text, juce::dontSendNotification);
    status.setColour (juce::Label::textColourId, error ? kError : kDim);
}

void ModelPanel::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
}

void ModelPanel::resized()
{
    auto r = getLocalBounds().reduced (4);
    auto row = [&] (std::initializer_list<juce::Button*> bs)
    {
        auto line = r.removeFromTop (24);
        const int n = (int) bs.size();
        const int w = (line.getWidth() - (n - 1) * 4) / n;
        int i = 0;
        for (auto* b : bs)
        {
            b->setBounds (i == n - 1 ? line : line.removeFromLeft (w));
            if (i < n - 1) line.removeFromLeft (4);
            ++i;
        }
        r.removeFromTop (4);
    };
    row ({ &addHarmBtn, &addNoiseBtn, &analyseAllBtn, &exportBtn, &importBtn });
    row ({ &compareBtn, &renderSelBtn, &renderCsvBtn });
    status.setBounds (r.removeFromBottom (18));
    const int listH = juce::jlimit (60, 200, 26 + 20 * std::max (3, getNumRows()));
    table.setBounds (r.removeFromTop (std::min (listH, r.getHeight() / 3)));
    auto& hdr = table.getHeader();
    hdr.setColumnWidth (3, std::max (60, table.getWidth() - 30 - 62 - 100 - 70 - 18));
    r.removeFromTop (4);
    viewport.setBounds (r);
    layoutDetail();
}

void ModelPanel::layoutDetail()
{
    const auto* c = current();
    const int w = std::max (200, viewport.getWidth() - viewport.getScrollBarThickness() - 2);
    int y = 2;
    const int lab = 74, rowH = 24;
    auto line = [&] (juce::Component& l, juce::Component& field, int fieldW = -1)
    {
        l.setBounds (0, y, lab, rowH - 2);
        field.setBounds (lab, y, fieldW > 0 ? fieldW : w - lab, rowH - 2);
        y += rowH;
    };
    const bool harm = c != nullptr && c->isHarmonic();
    for (auto* comp : content.getChildren()) comp->setVisible (c != nullptr);
    if (c == nullptr)
    {
        content.setSize (w, 10);
        return;
    }
    line (nameLab, nameEd);
    line (paramLab, paramCombo);
    line (channelLab, channelCombo, 70);
    rangesLab.setText (harm ? "Ranges" : "States", juce::dontSendNotification);
    rangesLab.setBounds (0, y, lab, rowH - 2);
    addRangeBtn.setBounds (lab, y, (w - lab) / 2 - 2, rowH - 2);
    removeRangeBtn.setBounds (lab + (w - lab) / 2 + 2, y, (w - lab) - (w - lab) / 2 - 2, rowH - 2);
    y += rowH;
    rangesList->setBounds (lab, y, w - lab, 4 * 18 + 2);
    y += 4 * 18 + 6;

    for (auto* comp : std::initializer_list<juce::Component*> { &f0Lab, &f0Ed, &f0AtLab, &f0AtEd, &pickF0Btn,
                                                                &ordersLab, &ordersEd, &pickHarmBtn, &tracksLab,
                                                                &tracksText, &addTrackBtn, &removeTrackBtn,
                                                                &dynamicsTgl, &variationTgl })
        comp->setVisible (harm);
    if (harm)
    {
        const int fw = (w - lab - 44 - 50 - 8) / 2;
        f0Lab.setBounds (0, y, lab, rowH - 2);
        f0Ed.setBounds (lab, y, fw, rowH - 2);
        f0AtLab.setBounds (lab + fw + 2, y, 42, rowH - 2);
        f0AtEd.setBounds (lab + fw + 44, y, fw, rowH - 2);
        pickF0Btn.setBounds (w - 50, y, 50, rowH - 2);
        y += rowH;
        ordersLab.setBounds (0, y, lab, rowH - 2);
        ordersEd.setBounds (lab, y, w - lab - 108, rowH - 2);
        pickHarmBtn.setBounds (w - 104, y, 104, rowH - 2);
        y += rowH;
        tracksLab.setBounds (0, y, lab, rowH - 2);
        tracksText.setBounds (lab, y, w - lab, rowH - 2);
        y += rowH;
        addTrackBtn.setBounds (lab, y, (w - lab) / 2 - 2, rowH - 2);
        removeTrackBtn.setBounds (lab + (w - lab) / 2 + 2, y, (w - lab) - (w - lab) / 2 - 2, rowH - 2);
        y += rowH;
        dynamicsTgl.setBounds (0, y, w / 2, rowH - 2);
        variationTgl.setBounds (w / 2, y, w - w / 2, rowH - 2);
        y += rowH;
    }
    analyseBtn.setBounds (0, y, w, rowH);
    y += rowH + 4;
    const int lines = std::max (1, report.getText().isEmpty() ? 1 : juce::StringArray::fromLines (report.getText()).size());
    collapseBtn->setBounds (0, y, 18, 18);
    report.setBounds (22, y, w - 22, 15 * lines + 4);
    y += std::max (20, 15 * lines + 6);
    for (auto* comp : std::initializer_list<juce::Component*> { plot.get(), &settingsLab, &fftCombo, &tolEd, &steadyEd })
        comp->setVisible (! bottomCollapsed_);
    if (! bottomCollapsed_)
    {
        plot->setBounds (0, y, w, 150);
        y += 156;
        settingsLab.setBounds (0, y, lab, rowH - 2);
        fftCombo.setBounds (lab, y, 70, rowH - 2);
        tolEd.setBounds (lab + 74, y, 46, rowH - 2);
        steadyEd.setBounds (lab + 124, y, 46, rowH - 2);
        y += rowH + 4;
    }
    content.setSize (w, y);
}

void ModelPanel::refresh()
{
    auto* m = model();
    if (m == nullptr || m->find (selectedId_) == nullptr)
        selectedId_ = (m != nullptr && ! m->components.empty()) ? m->components.front().id : juce::String();
    table.updateContent();
    const int idx = selectedIndex();
    if (idx >= 0) table.selectRow (idx, false, true);
    else table.deselectAllRows();
    table.repaint();
    for (auto* b : { &analyseAllBtn, &exportBtn, &compareBtn, &renderSelBtn, &renderCsvBtn })
        b->setEnabled (m != nullptr && ! m->empty());
    for (auto* b : { &addHarmBtn, &addNoiseBtn, &importBtn })
        b->setEnabled (m != nullptr);
    if (m == nullptr) showStatus ("Open a file to model its sound.");
    else if (m->empty() && status.getText().isEmpty())
        showStatus ("Add a Harmonic or a Noise component, give it time ranges, then Analyse.");
    loadDetail();
    resized();
}

void ModelPanel::loadDetail()
{
    const juce::ScopedValueSetter<bool> guard (loading_, true);
    auto* c = current();
    auto* m = model();
    if (c == nullptr || m == nullptr) return;
    nameEd.setText (c->name, false);
    paramCombo.clear (juce::dontSendNotification);
    paramCombo.addItem ("(none)", 1);
    const auto names = app.modelParameterNames();
    for (int i = 0; i < names.size(); ++i) paramCombo.addItem (names[i], i + 2);
    if (c->parameter.isEmpty()) paramCombo.setSelectedId (1, juce::dontSendNotification);
    else
    {
        int id = names.indexOf (c->parameter) + 2;
        if (id < 2) { paramCombo.addItem (c->parameter, names.size() + 2); id = names.size() + 2; }   // missing: kept, shown
        paramCombo.setSelectedId (id, juce::dontSendNotification);
    }
    channelCombo.clear (juce::dontSendNotification);
    const int nch = std::max (1, app.doc() != nullptr ? app.doc()->engine.numChannels() : 1);
    for (int ch = 0; ch < std::max (nch, c->channel + 1); ++ch) channelCombo.addItem (juce::String (ch + 1), ch + 1);
    channelCombo.setSelectedId (c->channel + 1, juce::dontSendNotification);
    rangesList->updateContent();
    rangesList->repaint();
    f0Ed.setText (juce::String (c->f0Hz, 2), false);
    f0AtEd.setText (juce::String (c->f0AtParam, 2), false);
    f0Ed.setEnabled (true);
    f0AtEd.setEnabled (! c->parameter.isEmpty());
    ordersEd.setText (ordersText (*c), false);
    juce::StringArray tracks;
    for (const auto& l : c->lines) if (l.isTrack()) tracks.add (l.label());
    tracksText.setText (tracks.isEmpty() ? "(none)" : tracks.joinIntoString ("; "), juce::dontSendNotification);
    dynamicsTgl.setToggleState (c->dynamics, juce::dontSendNotification);
    variationTgl.setToggleState (c->variation, juce::dontSendNotification);
    report.setText (c->analysed ? c->report : "Not analysed yet.", juce::dontSendNotification);
    fftCombo.setSelectedId (m->fftOrder, juce::dontSendNotification);
    tolEd.setText (juce::String (m->tolerancePct, 1), false);
    steadyEd.setText (juce::String (m->steadyRatePct, 1), false);
    pickF0Btn.setToggleState (app.modelPick == Application::ModelPick::fundamental && app.modelPickId == c->id,
                              juce::dontSendNotification);
    pickHarmBtn.setToggleState (app.modelPick == Application::ModelPick::harmonics && app.modelPickId == c->id,
                                juce::dontSendNotification);
    pickF0Btn.setButtonText (pickF0Btn.getToggleState() ? "Click..." : "Pick");
    pickHarmBtn.setButtonText (pickHarmBtn.getToggleState() ? "Stop Picking" : "Pick Harmonics");
    plot->repaint();
}

void ModelPanel::changed (bool invalidates)
{
    if (auto* c = current())
        if (invalidates && c->analysed)
        {
            c->analysed = false;
            showStatus (c->name + " changed: analyse it again.");
        }
    app.soundModelChanged();
    refresh();
}

void ModelPanel::commitEdits()
{
    if (loading_) return;
    auto* c = current();
    auto* m = model();
    if (c == nullptr || m == nullptr) return;
    bool any = false, invalid = false;
    const auto name = nameEd.getText().trim();
    if (name.isNotEmpty() && name != c->name) { c->name = name; any = true; }
    double v = 0.0;
    if (parseDouble (f0Ed.getText(), v) && v > 0.0 && std::abs (v - c->f0Hz) > 1e-9) { c->f0Hz = v; invalid = true; }
    if (parseDouble (f0AtEd.getText(), v) && std::abs (v) > 1e-12 && std::abs (v - c->f0AtParam) > 1e-9)
    {
        c->f0AtParam = v;
        invalid = true;
    }
    if (c->isHarmonic())
    {
        std::vector<double> orders;
        juce::StringArray parts;
        parts.addTokens (ordersEd.getText(), ", ;", "");
        parts.removeEmptyStrings();
        for (const auto& p : parts)
            if (parseDouble (p, v) && v > 0.0 && v <= 1000.0
                && std::none_of (orders.begin(), orders.end(), [v] (double o) { return std::abs (o - v) < 1e-9; }))
                orders.push_back (v);
        std::sort (orders.begin(), orders.end());
        std::vector<double> before;
        for (const auto& l : c->lines) if (! l.isTrack()) before.push_back (l.order);
        if (orders != before)
        {
            std::vector<md::HarmonicLine> lines;
            for (double o : orders)
            {
                const auto it = std::find_if (c->lines.begin(), c->lines.end(), [o] (const md::HarmonicLine& l)
                                              { return ! l.isTrack() && std::abs (l.order - o) < 1e-9; });
                if (it != c->lines.end()) lines.push_back (*it);
                else { md::HarmonicLine l; l.order = o; lines.push_back (l); }
            }
            for (const auto& l : c->lines) if (l.isTrack()) lines.push_back (l);
            c->lines = std::move (lines);
            invalid = true;
        }
    }
    if (parseDouble (tolEd.getText(), v) && v > 0.0 && v <= 50.0 && std::abs (v - m->tolerancePct) > 1e-9)
    {
        m->tolerancePct = v;
        for (auto& o : m->components) o.analysed = false;
        any = true;
    }
    if (parseDouble (steadyEd.getText(), v) && v > 0.0 && v <= 100.0 && std::abs (v - m->steadyRatePct) > 1e-9)
    {
        m->steadyRatePct = v;
        for (auto& o : m->components) if (o.dynamics) o.analysed = false;
        any = true;
    }
    if (invalid) changed (true);
    else if (any) changed (false);
    else loadDetail();   // invalid text: show the real values again
}

void ModelPanel::addComponent (md::Kind kind)
{
    auto* m = model();
    if (m == nullptr) return;
    int n = 1;
    for (const auto& c : m->components) if (c.kind == kind) ++n;
    auto& c = m->add (kind, (kind == md::Kind::Harmonic ? "Harmonic " : "Noise ") + juce::String (n));
    // Start from the first parameter, and the current selection as a range.
    const auto names = app.modelParameterNames();
    if (! names.isEmpty()) c.parameter = names[0];
    if (! app.selection.isEmpty()) c.ranges.push_back ({ app.selection.getStart(), app.selection.getEnd() });
    selectedId_ = c.id;
    showStatus (kind == md::Kind::Harmonic
                    ? "Set the fundamental (Pick) and the harmonics, add ranges, then Analyse."
                    : "Add one range per state of the noise, then Analyse.");
    app.soundModelChanged();
    refresh();
}

void ModelPanel::moveSelected (int delta)
{
    auto* m = model();
    const int i = selectedIndex();
    if (m == nullptr || i < 0) return;
    const int j = i + delta;
    if (j < 0 || j >= (int) m->components.size()) return;
    std::swap (m->components[(size_t) i], m->components[(size_t) j]);
    showStatus ("Order changed: Analyse All to update what each component takes out.");
    app.soundModelChanged();
    refresh();
}

bool ModelPanel::ensureAnalysed()
{
    commitEdits();
    auto* m = model();
    if (m == nullptr) return false;
    bool need = false;
    for (const auto& c : m->components)
        need = need || (c.enabled && ! c.analysed);
    if (! need) return true;
    juce::String err;
    const bool ok = app.analyseModel (-1, &err);
    refresh();
    if (! ok)
    {
        showStatus ("The model could not be analysed first: " + err, true);
        return false;
    }
    showStatus ("Every component analysed.");
    return true;
}

void ModelPanel::setBottomCollapsed (bool collapsed)
{
    bottomCollapsed_ = collapsed;
    if (collapseBtn != nullptr) { collapseBtn->collapsed = collapsed; collapseBtn->repaint(); }
    if (app.recentStore != nullptr)
    {
        app.recentStore->setValue ("model_bottom_collapsed", collapsed);
        app.recentStore->saveIfNeeded();
    }
    layoutDetail();
}

void ModelPanel::moveComponent (int from, int to)
{
    auto* m = model();
    if (m == nullptr) return;
    const int n = (int) m->components.size();
    if (from < 0 || from >= n) return;
    to = juce::jlimit (0, n, to);
    const int dest = to > from ? to - 1 : to;   // index once removed from its place
    if (dest == from) return;
    auto c = std::move (m->components[(size_t) from]);
    m->components.erase (m->components.begin() + from);
    m->components.insert (m->components.begin() + dest, std::move (c));
    selectedId_ = m->components[(size_t) dest].id;
    showStatus ("Order changed: Analyse All to update what each component takes out.");
    app.soundModelChanged();
    refresh();
}

int ModelPanel::insertionIndexAt (juce::Point<int> p) const
{
    const int n = const_cast<ModelPanel*> (this)->getNumRows();
    auto& t = const_cast<juce::TableListBox&> (table);
    const int row = t.getRowContainingPosition (p.x, p.y);
    if (row < 0 || row >= n)
        return p.y < t.getHeaderHeight() ? 0 : n;
    const auto r = t.getRowPosition (row, true);
    return p.y > r.getCentreY() ? row + 1 : row;
}

void ModelPanel::mouseDown (const juce::MouseEvent& e)
{
    dragFromRow_ = -1;
    rowDragging_ = false;
    if (e.eventComponent == nullptr || ! table.isParentOf (e.eventComponent) || e.mods.isPopupMenu())
        return;
    const auto p = e.getEventRelativeTo (&table).getPosition();
    if (p.y < table.getHeaderHeight()) return;
    const int row = table.getRowContainingPosition (p.x, p.y);
    if (row >= 0 && row < getNumRows()) dragFromRow_ = row;
}

void ModelPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (dragFromRow_ < 0) return;
    if (! rowDragging_ && std::abs (e.getDistanceFromDragStartY()) < 6) return;
    rowDragging_ = true;
    dropIndex_ = insertionIndexAt (e.getEventRelativeTo (&table).getPosition());
    repaint();
}

void ModelPanel::mouseUp (const juce::MouseEvent& e)
{
    if (rowDragging_ && dragFromRow_ >= 0)
        moveComponent (dragFromRow_, insertionIndexAt (e.getEventRelativeTo (&table).getPosition()));
    dragFromRow_ = -1;
    dropIndex_ = -1;
    rowDragging_ = false;
    repaint();
}

void ModelPanel::paintOverChildren (juce::Graphics& g)
{
    if (! rowDragging_ || dropIndex_ < 0) return;
    // Where the dragged component will land.
    const int n = getNumRows();
    int y = 0;
    if (dropIndex_ < n) y = table.getRowPosition (dropIndex_, true).getY();
    else if (n > 0)     y = table.getRowPosition (n - 1, true).getBottom();
    else                y = table.getHeaderHeight();
    y += table.getY();
    g.setColour (juce::Colour::fromRGB (0, 212, 170));
    g.fillRect (table.getX() + 2, y - 1, table.getWidth() - 4, 2);
}

void ModelPanel::deleteSelected()
{
    auto* m = model();
    const int i = selectedIndex();
    if (m == nullptr || i < 0) return;
    if (app.modelPickId == selectedId_) app.cancelModelPick();
    m->components.erase (m->components.begin() + i);
    selectedId_ = m->components.empty() ? juce::String()
                                         : m->components[(size_t) std::min (i, (int) m->components.size() - 1)].id;
    app.soundModelChanged();
    refresh();
}

void ModelPanel::analyseSelected()
{
    commitEdits();
    const int i = selectedIndex();
    if (i < 0) return;
    juce::String err;
    if (app.analyseModel (i, &err)) showStatus (model()->components[(size_t) i].name + " analysed.");
    else showStatus (err, true);
    refresh();
}

void ModelPanel::analyseAll()
{
    commitEdits();
    juce::String err;
    if (app.analyseModel (-1, &err)) showStatus ("Every component analysed.");
    else showStatus (err, true);
    refresh();
}

void ModelPanel::compare()
{
    if (! ensureAnalysed()) return;
    juce::String err;
    if (app.compareModel (&err)) showStatus ("Compare: channel 1 = simulation, channel 2 = original.");
    else showStatus (err, true);
}

void ModelPanel::renderSelection()
{
    if (! ensureAnalysed()) return;
    if (onRenderSelectionRequested) { onRenderSelectionRequested(); return; }
    juce::String err;
    if (app.renderModelSelection (&err)) showStatus ("Rendered into a new channel.");
    else showStatus (err, true);
}

void ModelPanel::addTrack (const std::vector<juce::String>& selections)
{
    auto* c = current();
    if (c == nullptr || ! c->isHarmonic() || selections.empty()) return;
    md::HarmonicLine l;
    l.selections = selections;
    c->lines.push_back (l);
    changed (true);
}

void ModelPanel::showTrackMenu (bool remove)
{
    auto* c = current();
    const auto* d = app.doc();
    if (c == nullptr || d == nullptr || ! c->isHarmonic()) return;
    juce::PopupMenu menu;
    std::vector<std::vector<juce::String>> choices;
    if (remove)
    {
        for (size_t i = 0; i < c->lines.size(); ++i)
            if (c->lines[i].isTrack())
            {
                choices.push_back ({ juce::String ((int) i) });
                menu.addItem ((int) choices.size(), c->lines[i].label());
            }
        if (choices.empty()) menu.addItem (-1, "No track to remove", false);
    }
    else
    {
        // Groups first: their selections are pieces of one harmonic.
        std::vector<int> groupIds;
        for (const auto& s : d->savedSelections)
            if (s.group > 0 && std::find (groupIds.begin(), groupIds.end(), s.group) == groupIds.end())
                groupIds.push_back (s.group);
        if (! groupIds.empty()) menu.addSectionHeader ("Groups");
        for (int g : groupIds)
        {
            std::vector<juce::String> names;
            for (const auto& s : d->savedSelections) if (s.group == g) names.push_back (s.name);
            choices.push_back (names);
            const auto gname = d->selectionGroups.nameOf (g);
            menu.addItem ((int) choices.size(), (gname.isNotEmpty() ? gname : "Group " + juce::String (g))
                                                    + " (" + juce::String ((int) names.size()) + ")");
        }
        if (! d->savedSelections.empty()) menu.addSectionHeader ("Stored selections");
        for (const auto& s : d->savedSelections)
        {
            choices.push_back ({ s.name });
            menu.addItem ((int) choices.size(), s.name);
        }
        if (choices.empty()) menu.addItem (-1, "No stored selection: store one in the Selection panel", false);
    }
    juce::Component::SafePointer<ModelPanel> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (remove ? &removeTrackBtn : &addTrackBtn),
                        [safe, choices, remove] (int r)
                        {
                            if (safe == nullptr || r <= 0 || r > (int) choices.size()) return;
                            auto& self = *safe;
                            if (! remove) { self.addTrack (choices[(size_t) r - 1]); return; }
                            auto* c = self.current();
                            const int li = choices[(size_t) r - 1].front().getIntValue();
                            if (c == nullptr || li < 0 || li >= (int) c->lines.size()) return;
                            c->lines.erase (c->lines.begin() + li);
                            self.changed (true);
                        });
}

void ModelPanel::togglePick (int mode)
{
    auto* c = current();
    if (c == nullptr) return;
    commitEdits();
    const auto want = mode == 1 ? Application::ModelPick::fundamental : Application::ModelPick::harmonics;
    if (app.modelPick == want && app.modelPickId == c->id) app.cancelModelPick();
    else
    {
        app.startModelPick (want, c->id);
        showStatus (mode == 1 ? "Click the fundamental on the spectrogram (Esc cancels)."
                              : "Click harmonics on the spectrogram to add or remove them (Esc stops).");
    }
    loadDetail();
}

void ModelPanel::chooseFile (bool save, const juce::String& title, const juce::String& patterns,
                             std::function<void (const juce::File&)> then)
{
    chooser_ = std::make_unique<juce::FileChooser> (title, juce::File(), patterns);
    const int flags = save ? (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting)
                           : juce::FileBrowserComponent::openMode;
    juce::Component::SafePointer<ModelPanel> safe (this);
    chooser_->launchAsync (flags | juce::FileBrowserComponent::canSelectFiles, [safe, then] (const juce::FileChooser& fc)
    {
        if (safe == nullptr) return;
        const auto f = fc.getResult();
        if (f != juce::File()) then (f);
    });
}

// ------------------------------------------------------ TableListBoxModel

int ModelPanel::getNumRows()
{
    auto* m = model();
    return m != nullptr ? (int) m->components.size() : 0;
}

void ModelPanel::paintRowBackground (juce::Graphics& g, int, int, int, bool selected)
{
    if (selected) g.fillAll (kSelected);
}

void ModelPanel::paintCell (juce::Graphics& g, int row, int col, int w, int h, bool)
{
    auto* m = model();
    if (m == nullptr || row < 0 || row >= (int) m->components.size()) return;
    const auto& c = m->components[(size_t) row];
    g.setFont (12.0f);
    g.setColour (c.enabled ? kText : kDim);
    switch (col)
    {
        case 1:
        {
            const auto box = juce::Rectangle<float> (8.0f, (h - 12) / 2.0f, 12.0f, 12.0f);
            g.drawRect (box);
            if (c.enabled) g.fillRect (box.reduced (3.0f));
            break;
        }
        case 2: g.drawText (c.isHarmonic() ? "Harmonic" : "Noise", 4, 0, w - 6, h, juce::Justification::centredLeft); break;
        case 3: g.drawText (juce::String (row + 1) + ". " + c.name, 4, 0, w - 6, h, juce::Justification::centredLeft); break;
        case 4: g.drawText (c.parameter.isEmpty() ? "(none)" : c.parameter, 4, 0, w - 6, h, juce::Justification::centredLeft); break;
        case 5: g.drawText (c.analysed ? "analysed" : "to analyse", 4, 0, w - 6, h, juce::Justification::centredLeft); break;
        default: break;
    }
}

void ModelPanel::cellClicked (int row, int col, const juce::MouseEvent& e)
{
    auto* m = model();
    if (m == nullptr || row < 0 || row >= (int) m->components.size()) return;
    selectedId_ = m->components[(size_t) row].id;
    if (e.mods.isPopupMenu()) { refresh(); showRowMenu (row); return; }
    if (col == 1)
    {
        auto& c = m->components[(size_t) row];
        c.enabled = ! c.enabled;
        showStatus (c.name + (c.enabled ? " on." : " off: not rendered, not taken out of the components below "
                                                    "(Analyse All to update them)."));
        app.soundModelChanged();
    }
    refresh();
}

void ModelPanel::selectedRowsChanged (int lastRow)
{
    auto* m = model();
    if (m == nullptr || lastRow < 0 || lastRow >= (int) m->components.size()) return;
    if (m->components[(size_t) lastRow].id == selectedId_) return;
    commitEdits();
    selectedId_ = m->components[(size_t) lastRow].id;
    loadDetail();
    layoutDetail();
}

void ModelPanel::backgroundClicked (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Add Harmonic Component");
        menu.addItem (2, "Add Noise Component");
        menu.addSeparator();
        menu.addItem (3, "Close Panel");
        juce::Component::SafePointer<ModelPanel> safe (this);
        menu.showMenuAsync ({}, [safe] (int r)
        {
            if (safe == nullptr) return;
            if (r == 1) safe->addComponent (md::Kind::Harmonic);
            if (r == 2) safe->addComponent (md::Kind::Noise);
            if (r == 3) safe->requestClose();
        });
    }
}

void ModelPanel::showRowMenu (int row)
{
    auto* m = model();
    if (m == nullptr) return;
    juce::PopupMenu menu;
    menu.addItem (1, "Analyse");
    menu.addSeparator();
    menu.addItem (2, "Move Up", row > 0);
    menu.addItem (3, "Move Down", row + 1 < (int) m->components.size());
    menu.addItem (4, "Duplicate");
    menu.addItem (5, "Delete");
    menu.addSeparator();
    menu.addItem (6, "Close Panel");
    juce::Component::SafePointer<ModelPanel> safe (this);
    menu.showMenuAsync ({}, [safe] (int r)
    {
        if (safe == nullptr) return;
        auto& self = *safe;
        switch (r)
        {
            case 1: self.analyseSelected(); break;
            case 2: self.moveSelected (-1); break;
            case 3: self.moveSelected (+1); break;
            case 4:
                if (auto* mm = self.model())
                    if (const int i = self.selectedIndex(); i >= 0)
                    {
                        auto copy = mm->components[(size_t) i];
                        copy.id = mm->add (copy.kind, {}).id;   // a fresh unique id
                        mm->components.pop_back();
                        copy.name += " copy";
                        mm->components.insert (mm->components.begin() + i + 1, copy);
                        self.selectedId_ = copy.id;
                        self.app.soundModelChanged();
                        self.refresh();
                    }
                break;
            case 5: self.deleteSelected(); break;
            case 6: self.requestClose(); break;
            default: break;
        }
    });
}
