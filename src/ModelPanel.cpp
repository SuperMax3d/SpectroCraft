// SpectroCraft - Model panel implementation.
#include <JuceHeader.h>
#include "Application.h"
#include "ModelSession.h"
#include "ModelPanel.h"
#include "OwningAlertWindow.h"

#include <algorithm>
#include <cmath>

namespace {

/** Number with just enough decimals (trailing zeros trimmed). */
juce::String fmtNum (double v)
{
    if (!std::isfinite (v)) return "?";
    juce::String s (v, 4);
    while (s.endsWith ("0")) s = s.substring (0, s.length () - 1);
    if (s.endsWith ("."))   s = s.substring (0, s.length () - 1);
    return s;
}

/** A "nice" step (1/2/5 x 10^k) giving at most `maxDiv` divisions across span. */
double niceStep (double span, int maxDiv)
{
    const double raw = std::max (span / maxDiv, 1e-9);
    for (int k = -6; k <= 8; ++k)
        for (double m : { 1.0, 2.0, 5.0 })
        {
            const double cand = m * std::pow (10.0, k);
            if (cand >= raw) return cand;
        }
    return 1e8;
}

/** Compact "label: field" row used by the model dialogs. */
class McField : public juce::Component
{
public:
    McField (const juce::String& label, const juce::String& text)
    {
        lbl.setJustificationType (juce::Justification::centredRight);
        lbl.setText (label, juce::dontSendNotification);
        ed.setText (text, juce::dontSendNotification);
        addAndMakeVisible (&lbl);
        addAndMakeVisible (&ed);
    }

    juce::TextEditor* editor() { return &ed; }

    void resized() override
    {
        lbl.setBounds (0, 0, kLabelW, getHeight());
        ed.setBounds (kLabelW + 2, 0, getWidth () - kLabelW - 2, getHeight());
    }

private:
    static constexpr int kLabelW = 58;
    juce::Label lbl;
    juce::TextEditor ed;
};

juce::Colour mcGridMinor() { return juce::Colour::fromRGB (40, 40, 46); }
juce::Colour mcGridMajor() { return juce::Colour::fromRGB (58, 58, 66); }
constexpr int kMcPointRadius = 3;

} // namespace

// ============================================================ ModelCurveEditor

ModelCurveEditor::ModelCurveEditor()
{
    setSize (400, 260);
}

void ModelCurveEditor::setRanges (double xLo, double xHi, double yLo, double yHi)
{
    if (!std::isfinite (xLo) || !std::isfinite (xHi) || xHi <= xLo) return;
    if (!std::isfinite (yLo) || !std::isfinite (yHi) || yHi <= yLo) return;
    xLo_ = xLo;  xHi_ = xHi;
    yLo_ = yLo;  yHi_ = yHi;
    repaint ();
}

void ModelCurveEditor::setPoints (std::vector<sc::model::CurvePoint> pts)
{
    std::sort (pts.begin (), pts.end (),
               [] (const sc::model::CurvePoint& a, const sc::model::CurvePoint& b) { return a.x < b.x; });
    points_ = std::move (pts);
    dragIndex_ = -1;
    repaint ();
}

std::vector<sc::model::CurvePoint> ModelCurveEditor::getPoints() const
{
    // Normalise: sort by x and merge points that ended up on the same abscissa.
    auto out = points_;
    std::sort (out.begin (), out.end (),
               [] (const sc::model::CurvePoint& a, const sc::model::CurvePoint& b) { return a.x < b.x; });
    std::vector<sc::model::CurvePoint> merged;
    for (const auto& p : out)
        if (merged.empty () || std::abs (p.x - merged.back ().x) > 1e-9)
            merged.push_back (p);
    return merged;
}

void ModelCurveEditor::setPoint (int index, double x, double y)
{
    auto pts = getPoints ();
    if (index < 0 || index >= (int) pts.size ()) return;
    x = std::clamp (std::isfinite (x) ? x : xLo_, xLo_, xHi_);
    y = std::clamp (std::isfinite (y) ? y : yLo_, yLo_, yHi_);
    pts[(size_t) index] = { x, y };
    setPoints (std::move (pts));
}

int ModelCurveEditor::valueToX (double v) const
{
    const int w = getWidth () - kLeftMargin - kRightMargin;
    if (w <= 0 || xHi_ <= xLo_) return kLeftMargin;
    const double t = jlimit (0.0, 1.0, (v - xLo_) / (xHi_ - xLo_));
    return kLeftMargin + (int) std::lround (t * w);
}

double ModelCurveEditor::xToValue (int x) const
{
    const int w = getWidth () - kLeftMargin - kRightMargin;
    if (w <= 0 || xHi_ <= xLo_) return xLo_;
    const double t = jlimit (0.0, 1.0, (double) (x - kLeftMargin) / (double) w);
    return xLo_ + t * (xHi_ - xLo_);
}

int ModelCurveEditor::valueToY (double v) const
{
    const int h = getHeight () - kTopMargin - kBottomMargin;
    if (h <= 0 || yHi_ <= yLo_) return kTopMargin;
    const double t = jlimit (0.0, 1.0, (yHi_ - v) / (yHi_ - yLo_));
    return (int) std::lround (kTopMargin + h * t);
}

double ModelCurveEditor::yToValue (int y) const
{
    const int h = getHeight () - kTopMargin - kBottomMargin;
    if (h <= 0 || yHi_ <= yLo_) return yLo_;
    const double t = jlimit (0.0, 1.0, (double) (y - kTopMargin) / (double) h);
    return yHi_ - t * (yHi_ - yLo_);
}

int ModelCurveEditor::findNearestPoint (const juce::Point<int>& p, int maxDistPx) const
{
    double bestD2 = (double) maxDistPx * (double) maxDistPx;
    int best = -1;
    for (size_t i = 0; i < points_.size (); ++i)
    {
        const int dx = valueToX (points_[i].x) - p.x;
        const int dy = valueToY (points_[i].y) - p.y;
        const double d2 = (double) dx * dx + (double) dy * dy;
        if (d2 <= bestD2) { bestD2 = d2; best = (int) i; }
    }
    return best;
}

void ModelCurveEditor::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.getPosition ();

    // Right-click on a point removes it.
    if (e.mods.isRightButtonDown ())
    {
        const int idx = findNearestPoint (p, kMcPointRadius + 4);
        if (idx >= 0) points_.erase (points_.begin () + idx);
        dragIndex_ = -1;
        repaint ();
        return;
    }

    // Double-click on a point opens the exact-coordinate dialog.
    if (e.getNumberOfClicks() >= 2)
    {
        const int idx = findNearestPoint (p, kMcPointRadius + 4);
        dragIndex_ = -1;
        if (idx >= 0 && onEditPoint) onEditPoint (idx);
        repaint ();
        return;
    }

    const int idx = findNearestPoint (p, kMcPointRadius + 4);
    if (idx >= 0)
        dragIndex_ = idx;
    else
    {
        // Clicking empty space adds a new point at the cursor.
        points_.push_back ({ xToValue (p.x), yToValue (p.y) });
        std::sort (points_.begin (), points_.end (),
                   [] (const sc::model::CurvePoint& a, const sc::model::CurvePoint& b) { return a.x < b.x; });
        dragIndex_ = -1;
    }
    repaint ();
}

void ModelCurveEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (dragIndex_ >= 0 && dragIndex_ < (int) points_.size ())
    {
        points_[(size_t) dragIndex_] = { xToValue (e.x), yToValue (e.y) };
        repaint ();
    }
}

void ModelCurveEditor::mouseUp (const juce::MouseEvent&)
{
    dragIndex_ = -1;
}

void ModelCurveEditor::paint (juce::Graphics& g)
{
    const int W = getWidth (), H = getHeight ();
    g.fillAll (juce::Colour::fromRGB (26, 27, 31));
    if (W <= kLeftMargin + kRightMargin || H <= kTopMargin + kBottomMargin) return;

    // Grid: nice steps on both axes.
    const double xStep = niceStep (xHi_ - xLo_, 8);
    for (double v = std::ceil (xLo_ / xStep) * xStep; v <= xHi_ + 1e-9; v += xStep)
    {
        const int x = valueToX (v);
        g.setColour (mcGridMinor());
        g.drawLine (x, kTopMargin, x, H - kBottomMargin);
        g.setColour (juce::Colour::fromRGB (150, 155, 165));
        g.setFont (9.5f);
        g.drawText (fmtNum (v), x - 24, H - kBottomMargin + 3, 48, 14, juce::Justification::centred);
    }
    const double yStep = niceStep (yHi_ - yLo_, 6);
    for (double v = std::ceil (yLo_ / yStep) * yStep; v <= yHi_ + 1e-9; v += yStep)
    {
        const int y = valueToY (v);
        g.setColour (mcGridMinor());
        g.drawLine (kLeftMargin, y, W - kRightMargin, y);
        g.setColour (juce::Colour::fromRGB (150, 155, 165));
        g.setFont (9.5f);
        g.drawText (fmtNum (v), 2, y - 8, kLeftMargin - 6, 14, juce::Justification::centredRight);
    }

    // The curve: flat hold before the first point and after the last one
    // (matching RemapCurve::apply's endpoint clamping).
    if (!points_.empty ())
    {
        g.setColour (juce::Colours::dodgerblue.withAlpha (0.9f));
        const int xFirst = valueToX (points_.front ().x);
        const int yFirst = valueToY (jlimit (yLo_, yHi_, points_.front ().y));
        g.drawLine (kLeftMargin, yFirst, xFirst, yFirst, 2.0f);
        for (size_t i = 1; i < points_.size (); ++i)
            g.drawLine (valueToX (points_[i - 1].x), valueToY (jlimit (yLo_, yHi_, points_[i - 1].y)),
                        valueToX (points_[i].x),     valueToY (jlimit (yLo_, yHi_, points_[i].y)),
                        2.0f);
        const int xLast = valueToX (points_.back ().x);
        const int yLast = valueToY (jlimit (yLo_, yHi_, points_.back ().y));
        g.drawLine (xLast, yLast, W - kRightMargin, yLast, 2.0f);
    }

    // Control points.
    for (size_t i = 0; i < points_.size (); ++i)
    {
        const int x = valueToX (points_[i].x);
        const int y = valueToY (jlimit (yLo_, yHi_, points_[i].y));
        const bool dragging = ((int) i == dragIndex_);
        g.setColour (dragging ? juce::Colours::white : juce::Colour::fromRGB (255, 196, 0));
        g.fillEllipse (x - kMcPointRadius - (dragging ? 1 : 0), y - kMcPointRadius - (dragging ? 1 : 0),
                       2 * (kMcPointRadius + (dragging ? 1 : 0)), 2 * (kMcPointRadius + (dragging ? 1 : 0)));
    }
}

// ============================================================ curve dialog

/** Modal editor for one RemapCurve: adjustable axis ranges plus the point
    editor. Returns true when OK was pressed (curve then holds the new state). */
static bool showModelCurveDialog (juce::Component* parent, const juce::String& title,
                                  sc::model::RemapCurve& curve)
{
    auto* w = new sc::OwningAlertWindow (title,
        "Drag points to shape the remap; click empty space to add a point, double-click a "
        "point to type its exact coordinates and right-click it to remove it. The input is "
        "clamped to the first/last point's x and interpolated between points.",
        juce::AlertWindow::QuestionIcon, parent);

    auto* rangeRow = new juce::Component();
    rangeRow->setSize (380, 62);
    auto* fXLo = new McField ("X min:", fmtNum (curve.axisXLo));
    auto* fXHi = new McField ("X max:", fmtNum (curve.axisXHi));
    auto* fYLo = new McField ("Y min:", fmtNum (curve.axisYLo));
    auto* fYHi = new McField ("Y max:", fmtNum (curve.axisYHi));
    fXLo->setBounds (0, 0, 186, 28);
    fXHi->setBounds (194, 0, 186, 28);
    fYLo->setBounds (0, 32, 186, 28);
    fYHi->setBounds (194, 32, 186, 28);
    rangeRow->addAndMakeVisible (fXLo);
    rangeRow->addAndMakeVisible (fXHi);
    rangeRow->addAndMakeVisible (fYLo);
    rangeRow->addAndMakeVisible (fYHi);

    auto* ed = new ModelCurveEditor();
    ed->setSize (380, 250);

    auto applyRanges = [ed, fXLo, fXHi, fYLo, fYHi]
    {
        ed->setRanges (fXLo->editor()->getText().getDoubleValue(),
                       fXHi->editor()->getText().getDoubleValue(),
                       fYLo->editor()->getText().getDoubleValue(),
                       fYHi->editor()->getText().getDoubleValue());
    };
    for (auto* f : { fXLo, fXHi, fYLo, fYHi })
        f->editor()->onTextChange = [applyRanges] { applyRanges (); };

    w->addCustomComponent (rangeRow);
    w->addCustomComponent (ed);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    applyRanges ();
    ed->setPoints (curve.points);

    w->enterModalState (false);
    const int result = w->runModalLoop ();
    if (result == 1)
    {
        curve.points = ed->getPoints ();
        // Accept a range only when it is finite and non-degenerate; otherwise
        // keep the previous one.
        auto accept = [] (double oldLo, double oldHi, const juce::String& loText,
                          const juce::String& hiText) -> std::pair<double, double>
        {
            const double a = loText.getDoubleValue (), b = hiText.getDoubleValue ();
            return (std::isfinite (a) && std::isfinite (b) && b > a) ? std::make_pair (a, b)
                                                                    : std::make_pair (oldLo, oldHi);
        };
        const auto xr = accept (curve.axisXLo, curve.axisXHi, fXLo->editor()->getText(), fXHi->editor()->getText());
        const auto yr = accept (curve.axisYLo, curve.axisYHi, fYLo->editor()->getText(), fYHi->editor()->getText());
        curve.axisXLo = xr.first;  curve.axisXHi = xr.second;
        curve.axisYLo = yr.first;  curve.axisYHi = yr.second;
    }
    delete w;
    return result == 1;
}

// ============================================================ cell dialogs

namespace {

/** Row list of a harmonic cell's measured peaks. */
class PickListModel : public juce::ListBoxModel
{
public:
    std::vector<sc::model::HarmonicPick>* picks = nullptr;

    int getNumRows() override { return picks ? (int) picks->size () : 0; }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override
    {
        if (picks == nullptr || row < 0 || row >= (int) picks->size ()) return;
        const auto& p = (*picks)[(size_t) row];
        const juce::String t = fmtNum (p.freqHz) + " Hz \u00b1" + fmtNum (p.widthHz)
                             + "   @ " + fmtNum (p.timeSecs) + " s   ch" + juce::String (p.channel + 1);
        g.setColour (sel ? juce::Colours::white : juce::Colour::fromRGB (205, 210, 220));
        g.setFont (11.0f);
        g.drawText (t, 4, 0, w - 8, h, juce::Justification::centredLeft);
    }
};

/** Edit one harmonic pick: frequency, half-width, time and channel. */
static bool editPickDialog (juce::Component* parent, sc::model::HarmonicPick& p)
{
    auto* w = new sc::OwningAlertWindow ("Edit Pick", "Measured peak of this cell:",
                                         juce::AlertWindow::QuestionIcon, parent);
    auto* fFreq  = new McField ("Freq (Hz):", fmtNum (p.freqHz));
    auto* fWidth = new McField ("Width (Hz):", fmtNum (p.widthHz));
    auto* fTime  = new McField ("Time (s):",   fmtNum (p.timeSecs));
    auto* fCh    = new McField ("Channel:",    juce::String (p.channel + 1));
    for (auto* f : { fFreq, fWidth, fTime, fCh })
    {
        f->setSize (240, 28);
        w->addCustomComponent (f);
    }
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));
    fFreq->editor()->setWantsKeyboardFocus (true);

    w->enterModalState (false);
    const int r = w->runModalLoop ();
    if (r == 1)
    {
        double v;
        v = fFreq->editor()->getText().getDoubleValue();   if (std::isfinite (v) && v > 0.0) p.freqHz = v;
        v = fWidth->editor()->getText().getDoubleValue();  if (std::isfinite (v) && v > 0.0) p.widthHz = v;
        v = fTime->editor()->getText().getDoubleValue();   if (std::isfinite (v)) p.timeSecs = std::max (0.0, v);
        v = fCh->editor()->getText().getDoubleValue();     if (std::isfinite (v) && v >= 1.0) p.channel = (int) v - 1;
    }
    delete w;
    return r == 1;
}

/** Structural editor for one cell of the selected component (display coords). */
static void showModelCellDialog (juce::Component* parent, Application& app,
                                 sc::model::Component& comp, int dispRow, int dispCol)
{
    auto* s = app.modelSession.get ();
    if (s == nullptr) return;
    const int rows = comp.numRows (), cols = comp.numCols ();
    if (dispRow < 0 || dispRow >= rows || dispCol < 0 || dispCol >= cols) return;
    const int storageRow = rows - 1 - dispRow;   // display row 0 = top = highest Y band
    auto& cell = comp.cells[(size_t) (storageRow * cols + dispCol)];

    const juce::String title = "Cell (" + juce::String (dispRow + 1) + ", "
                             + juce::String (dispCol + 1) + ")";

    if (comp.type == 0)   // ------------------------------------------------ harmonic
    {
        auto* w = new sc::OwningAlertWindow (title,
            "Harmonic cell: measured peaks and the reference speed used for normalisation.",
            juce::AlertWindow::QuestionIcon, parent);

        // Reference-speed row: checkbox + field.
        auto* refRow = new juce::Component();
        refRow->setSize (340, 28);
        auto* fromFirst = new juce::ToggleButton ("From first pick");
        fromFirst->setBounds (0, 0, 150, 26);
        fromFirst->setToggleState (cell.refSpeedFromFirstPick, juce::dontSendNotification);
        auto* fRef = new McField ("Ref Hz:", fmtNum (cell.refSpeed));
        fRef->setBounds (154, 0, 186, 28);
        refRow->addAndMakeVisible (fromFirst);
        refRow->addAndMakeVisible (fRef);

        auto* list = new juce::ListBox ("Picks", nullptr);
        PickListModel pickModel;
        pickModel.picks = &cell.picks;
        list->setModel (&pickModel);
        list->setSize (340, 120);

        auto* btnRow = new juce::Component();
        btnRow->setSize (340, 28);
        auto* addBtn = new juce::TextButton ("Add");
        auto* editBtn = new juce::TextButton ("Edit...");
        auto* delBtn = new juce::TextButton ("Remove");
        addBtn->setBounds (0, 0, 76, 26);
        editBtn->setBounds (84, 0, 90, 26);
        delBtn->setBounds (182, 0, 90, 26);
        btnRow->addAndMakeVisible (addBtn);
        btnRow->addAndMakeVisible (editBtn);
        btnRow->addAndMakeVisible (delBtn);

        w->addCustomComponent (refRow);
        w->addCustomComponent (list);
        w->addCustomComponent (btnRow);
        w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

        auto syncRefUi = [fromFirst, fRef]
        {
            const bool on = fromFirst->getToggleState ();
            fromFirst->setButtonText (on ? "From first pick" : "Use Ref Hz");
            fRef->editor()->setEnabled (!on);
        };
        fromFirst->onClick = [syncRefUi, s] { syncRefUi (); s->dirty = true; };
        fRef->editor()->onTextChange = [s] { s->dirty = true; };

        addBtn->onClick = [list, &cell, s]
        {
            cell.picks.push_back (sc::model::HarmonicPick {});   // defaults: 0 Hz etc.
            list->updateContent ();
            s->dirty = true;
        };
        editBtn->onClick = [w, list, &cell, s]
        {
            const int idx = list->getSelectedRow ();
            if (idx < 0 || idx >= (int) cell.picks.size ()) return;
            if (editPickDialog (w, cell.picks[(size_t) idx]))
            {
                list->updateContent ();
                s->dirty = true;
            }
        };
        delBtn->onClick = [list, &cell, s]
        {
            const int idx = list->getSelectedRow ();
            if (idx < 0 || idx >= (int) cell.picks.size ()) return;
            cell.picks.erase (cell.picks.begin () + idx);
            list->updateContent ();
            s->dirty = true;
        };

        syncRefUi ();
        w->enterModalState (false);
        const int r = w->runModalLoop ();
        if (r == 1)
        {
            cell.refSpeedFromFirstPick = fromFirst->getToggleState ();
            if (!cell.refSpeedFromFirstPick)
            {
                const double v = fRef->editor()->getText().getDoubleValue ();
                if (std::isfinite (v)) cell.refSpeed = v;
            }
            s->dirty = true;
        }
        delete w;
    }
    else   // ------------------------------------------------------------- noise
    {
        auto* w = new sc::OwningAlertWindow (title,
            "Noise cell: the source time range whose raw spectrum is cached.",
            juce::AlertWindow::QuestionIcon, parent);

        auto* tRow = new juce::Component();
        tRow->setSize (340, 28);
        auto* fT0 = new McField ("T0 (s):", fmtNum (cell.t0));
        auto* fT1 = new McField ("T1 (s):", fmtNum (cell.t1));
        fT0->setBounds (0, 0, 168, 28);
        fT1->setBounds (172, 0, 168, 28);
        tRow->addAndMakeVisible (fT0);
        tRow->addAndMakeVisible (fT1);

        auto* useSelBtn = new juce::TextButton ("Use Selection");
        useSelBtn->setBounds (0, 0, 340, 26);
        const bool haveSelection = app.doc () != nullptr && !app.selection.isEmpty ();
        useSelBtn->setEnabled (haveSelection);

        auto* cRow = new juce::Component();
        cRow->setSize (340, 28);
        auto* fCh = new McField ("Channel:", juce::String (cell.channel + 1));
        auto* fFft = new McField ("FFT size:", juce::String (cell.fftSize));
        fCh->setBounds (0, 0, 168, 28);
        fFft->setBounds (172, 0, 168, 28);
        cRow->addAndMakeVisible (fCh);
        cRow->addAndMakeVisible (fFft);

        auto* winCombo = new juce::ComboBox();
        winCombo->addItem ("Rectangular", 1);
        winCombo->addItem ("Hann", 2);
        winCombo->addItem ("Hamming", 3);
        winCombo->setBounds (0, 0, 340, 26);
        winCombo->setSelectedId (cell.windowType + 1, juce::dontSendNotification);

        w->addCustomComponent (tRow);
        w->addCustomComponent (useSelBtn);
        w->addCustomComponent (cRow);
        w->addCustomComponent (winCombo);
        if (cell.detached)
        {
            auto* detLbl = new juce::Label();
            detLbl->setSize (340, 18);
            detLbl->setText ("Range deleted from its file - cached layers kept.",
                             juce::dontSendNotification);
            w->addCustomComponent (detLbl);
        }
        w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

        fT0->editor()->onTextChange = [s] { s->dirty = true; };
        fT1->editor()->onTextChange = [s] { s->dirty = true; };
        useSelBtn->onClick = [&cell, fT0, fT1, &app, s]
        {
            if (app.doc () == nullptr || app.selection.isEmpty ()) return;
            cell.t0 = app.selection.getStart ();
            cell.t1 = app.selection.getEnd ();
            fT0->editor()->setText (fmtNum (cell.t0), juce::dontSendNotification);
            fT1->editor()->setText (fmtNum (cell.t1), juce::dontSendNotification);
            s->dirty = true;
        };
        fCh->editor()->onTextChange  = [s] { s->dirty = true; };
        fFft->editor()->onTextChange = [s] { s->dirty = true; };
        winCombo->onChange       = [s] { s->dirty = true; };

        w->enterModalState (false);
        const int r = w->runModalLoop ();
        if (r == 1)
        {
            double v;
            v = fT0->editor()->getText().getDoubleValue ();   if (std::isfinite (v)) cell.t0 = std::max (0.0, v);
            v = fT1->editor()->getText().getDoubleValue ();   if (std::isfinite (v) && v > cell.t0) cell.t1 = v;
            v = fCh->editor()->getText().getDoubleValue ();   if (std::isfinite (v) && v >= 1.0) cell.channel = (int) v - 1;
            v = fFft->editor()->getText().getDoubleValue ();
            if (std::isfinite (v) && v >= 64.0 && v <= 262144.0) cell.fftSize = (int) v;
            const int wid = winCombo->getSelectedId ();
            if (wid >= 1 && wid <= 3) cell.windowType = wid - 1;
            s->dirty = true;
        }
        delete w;
    }
}

} // namespace

// ============================================================ ModelTableGrid

void ModelTableGrid::paint (juce::Graphics& g)
{
    const int W = getWidth (), H = getHeight ();
    g.fillAll (juce::Colour::fromRGB (30, 32, 36));
    if (comp == nullptr || comp->numCols () < 1 || comp->numRows () < 1) return;

    const int cols = comp->numCols (), rows = comp->numRows ();
    const int leftM = 46, botM = 18;
    const int cw  = std::max (52, (W - leftM) / cols);
    const int chh = std::max (26, (H - botM) / rows);

    // Cell backgrounds + preview text. Display row 0 is the top (highest Y band).
    for (int dr = 0; dr < rows; ++dr)
    {
        const int sr = rows - 1 - dr;   // storage row (axis order: row 0 = lowest band)
        for (int dc = 0; dc < cols; ++dc)
        {
            const auto& cell = comp->cells[(size_t) (sr * cols + dc)];
            juce::Rectangle<int> r (leftM + dc * cw, dr * chh, cw, chh);
            g.setColour (cell.isEmptyForType (comp->type)
                         ? juce::Colour::fromRGB (24, 25, 28)
                         : juce::Colour::fromRGB (38, 42, 48));
            g.fillRect (r);

            juce::String txt;
            if (comp->type == 0)
                txt = "H" + (cell.picks.empty () ? juce::String ()
                                                 : (" " + juce::String ((int) cell.picks.size ()) + "p"));
            else
                txt = cell.detached ? "N det"
                                    : ("N " + fmtNum (cell.t0) + "-" + fmtNum (cell.t1));
            if (!txt.isEmpty ())
            {
                g.setColour (juce::Colours::lightgrey.withAlpha (0.75f));
                g.setFont (9.5f);
                g.drawText (txt, r.getX () + 3, r.getY () + 2, cw - 8, 12, juce::Justification::left);
            }
            g.setColour (juce::Colours::grey.withAlpha (0.55f));
            g.setFont (9.0f);
            g.drawText ("[Edit]", r.getX () + 3, r.getBottom () - 14, cw - 8, 12, juce::Justification::left);
        }
    }

    // Grid lines.
    g.setColour (juce::Colour::fromRGB (70, 74, 80));
    for (int i = 0; i <= cols; ++i)
        g.drawLine (leftM + i * cw, 0, leftM + i * cw, rows * chh);
    for (int i = 0; i <= rows; ++i)
        g.drawLine (leftM, i * chh, leftM + cols * cw, i * chh);

    // Y breakpoint labels: descending top-to-bottom on the left. Breakpoint i
    // sits between storage rows i and i+1, i.e. at display y = (rows-1-i)*chh.
    g.setColour (juce::Colour::fromRGB (150, 155, 165));
    g.setFont (9.5f);
    for (size_t i = 0; i < comp->yBreaks.size (); ++i)
        g.drawText (fmtNum (comp->yBreaks[i]), 2, (rows - 1 - (int) i) * chh - 8, leftM - 6, 14,
                    juce::Justification::centredRight);

    // X breakpoint labels: ascending left-to-right at the bottom. Breakpoint j
    // sits between storage cols j and j+1, i.e. at display x = leftM+(j+1)*cw.
    for (size_t j = 0; j < comp->xBreaks.size (); ++j)
        g.drawText (fmtNum (comp->xBreaks[j]), leftM + ((int) j + 1) * cw - 24, rows * chh + 3, 48, 14,
                    juce::Justification::centred);
}

void ModelTableGrid::mouseDown (const juce::MouseEvent& e)
{
    if (comp == nullptr || onEditCell == nullptr) return;
    const int cols = comp->numCols (), rows = comp->numRows ();
    const int leftM = 46, botM = 18;
    const int cw  = std::max (52, (getWidth () - leftM) / cols);
    const int chh = std::max (26, (getHeight () - botM) / rows);
    if (e.x < leftM || e.y >= rows * chh) return;
    const int dc = (e.x - leftM) / cw;
    const int dr = e.y / chh;
    if (dc < 0 || dc >= cols || dr < 0 || dr >= rows) return;
    onEditCell (dr, dc);
}

// ============================================================ ModelPanel

namespace {

juce::String joinBreaks (const std::vector<double>& b)
{
    juce::String s;
    for (size_t i = 0; i < b.size (); ++i)
    {
        if (i > 0) s << ", ";
        s << fmtNum (b[i]);
    }
    return s;
}

/** Parses "1, 2.5, 3" style text into a sorted, de-duplicated break list. */
std::vector<double> parseBreaks (const juce::String& text)
{
    std::vector<double> out;
    for (const auto& tok : juce::StringArray::fromTokens (text, ",", " \t"))
    {
        const double v = tok.trim ().getDoubleValue ();
        if (std::isfinite (v)) out.push_back (v);
    }
    std::sort (out.begin (), out.end ());
    std::vector<double> dedup;
    for (double v : out)
        if (dedup.empty () || std::abs (v - dedup.back ()) > 1e-9)
            dedup.push_back (v);
    return dedup;
}

/** Rebuilds a component's cell array after its breakpoints changed, keeping
    the overlapping cells in storage order. Call AFTER assigning new breaks. */
void resizeCellsPreserving (sc::model::Component& c, int oldRows, int oldCols)
{
    const int nRows = c.numRows (), nCols = c.numCols ();
    auto old = std::move (c.cells);
    c.cells.clear ();
    c.cells.resize ((size_t) nRows * nCols);
    // Defensive: the stored grid may be smaller than oldRows*oldCols when a
    // component was created before its cells were sized; never read past it.
    const size_t oldSize = old.size ();
    for (int r = 0; r < nRows && r < oldRows; ++r)
        for (int cc = 0; cc < nCols && cc < oldCols; ++cc)
        {
            const size_t src = (size_t) (r * oldCols + cc);
            if (src >= oldSize) break;
            c.cells[(size_t) (r * nCols + cc)] = std::move (old[src]);
        }
}

/** The Save As flow shared by the Save and Save As buttons. */
void saveAsFlow (ModelPanel& panel)
{
    auto* s = panel.session ();
    if (s == nullptr) return;
    juce::FileChooser fc ("Save Model Session...",
        s->filePath.isNotEmpty () ? juce::File (s->filePath).getParentDirectory()
                                   : juce::File::getSpecialLocation (juce::File::userHomeDirectory),
        "*.json", false);
    if (!fc.browseForFileToSave (true)) return;
    panel.saveSessionFile (fc.getResult ());
}

} // namespace

ModelPanel::ModelPanel (Application& app_) : app (app_)
{
    setName ("Model");
    setSize (300, 520);

    btnNew.setButtonText ("New");
    btnOpen.setButtonText ("Open...");
    btnSave.setButtonText ("Save");
    btnSaveAs.setButtonText ("Save As...");
    btnNew.setTooltip ("Start a new model session.");
    btnOpen.setTooltip ("Open a saved model session (.json).");
    btnSave.setTooltip ("Save the model session to its file (or choose one when unsaved).");
    btnSaveAs.setTooltip ("Save the model session to a chosen .json file.");

    btnAddComp.setButtonText ("+ Add");
    btnAddComp.setTooltip ("Add a new harmonic component to the session.");

    matchPctLbl.setText ("Match %", juce::dontSendNotification);
    inclDbLbl.setText ("Incl dB", juce::dontSendNotification);
    overDbLbl.setText ("Over dB", juce::dontSendNotification);
    for (auto* l : { &matchPctLbl, &inclDbLbl, &overDbLbl })
        l->setFont (juce::Font (9.0f));

    listHdr.setText ("Components - right-click to rename / move / delete", juce::dontSendNotification);
    inputsHdr.setText ("Inputs - expressions over table parameters", juce::dontSendNotification);
    tableHdr.setText ("Table structure", juce::dontSendNotification);
    for (auto* l : { &listHdr, &inputsHdr, &tableHdr })
        l->setFont (juce::Font (9.5f));

    nameLbl.setText ("Name", juce::dontSendNotification);
    typeLbl.setText ("Type", juce::dontSendNotification);
    commentLbl.setText ("Comment", juce::dontSendNotification);
    xBreaksLbl.setText ("X:", juce::dontSendNotification);
    yBreaksLbl.setText ("Y:", juce::dontSendNotification);

    typeCombo.addItem ("Harmonic", 1);
    typeCombo.addItem ("Noise", 2);

    btnAddAmp.setButtonText ("+ AMP");
    btnRemoveAmp.setButtonText ("- AMP");
    btnAddCol.setButtonText ("+ Col");
    btnDelCol.setButtonText ("- Col");
    btnAddRow.setButtonText ("+ Row");
    btnDelRow.setButtonText ("- Row");

    // Stable component ids so unit tests can reach the widgets headlessly.
    nameEd.setComponentID ("modelName");
    typeCombo.setComponentID ("modelType");
    commentEd.setComponentID ("modelComment");
    btnAddAmp.setComponentID ("addAmp");
    btnRemoveAmp.setComponentID ("removeAmp");
    xBreaksEd.setComponentID ("xBreaks");
    yBreaksEd.setComponentID ("yBreaks");
    btnAddCol.setComponentID ("addCol");
    btnDelCol.setComponentID ("delCol");
    btnAddRow.setComponentID ("addRow");
    btnDelRow.setComponentID ("delRow");
    matchPctEd.setComponentID ("matchPct");
    inclDbEd.setComponentID ("inclDb");
    overDbEd.setComponentID ("overDb");
    btnAddComp.setComponentID ("addComp");

    // ---- session toolbar -------------------------------------------------
    btnNew.onClick = [this] { newSession (); };
    btnOpen.onClick = [this]
    {
        auto* s = session ();
        juce::FileChooser fc ("Open Model Session...",
            (s != nullptr && !s->filePath.isEmpty ()) ? juce::File (s->filePath).getParentDirectory()
                                                      : juce::File::getSpecialLocation (juce::File::userHomeDirectory),
            "*.json", false);
        if (fc.browseForFileToOpen ()) openSessionFile (fc.getResult ());
    };
    btnSave.onClick = [this]
    {
        auto* s = session ();
        if (s == nullptr) return;
        if (!s->filePath.isEmpty ()) saveSessionFile (juce::File (s->filePath));
        else                        saveAsFlow (*this);
    };
    btnSaveAs.onClick = [this] { saveAsFlow (*this); };

    btnAddComp.onClick = [this]
    {
        auto* s = session ();
        if (s == nullptr) return;
        // First unused name: Component 1, Component 2, ...
        juce::String base = "Component ";
        int n = 1;
        for (;; ++n)
        {
            const juce::String nm = base + juce::String (n);
            bool taken = false;
            for (const auto& c : s->components)
                if (c.name == nm) { taken = true; break; }
            if (!taken)
            {
                s->addComponent (nm, 0);   // 0 = harmonic
                selectedComp_ = (int) s->components.size () - 1;
                break;
            }
        }
        refresh ();
    };

    // ---- session settings -------------------------------------------------
    auto bindSetting = [this] (juce::TextEditor& ed, double* field)
    {
        ed.onTextChange = [this, &ed, field]
        {
            const double v = ed.getText ().getDoubleValue ();
            if (!std::isfinite (v)) return;
            auto* s = session ();
            if (s == nullptr || *field == v) return;
            *field = v;
            s->dirty = true;
        };
    };
    bindSetting (matchPctEd, &session ()->matchTolerancePct);
    bindSetting (inclDbEd,   &session ()->inclusionThresholdDb);
    bindSetting (overDbEd,   &session ()->overSubtractToleranceDb);

    // ---- component editor --------------------------------------------------
    nameEd.onTextChange = [this]
    {
        auto* s = session ();
        if (s == nullptr || selectedComp_ < 0 || selectedComp_ >= (int) s->components.size ()) return;
        auto& c = s->components[(size_t) selectedComp_];
        const juce::String t = nameEd.getText ();
        if (c.name != t) { c.name = t; s->dirty = true; compList.updateContent (); }
    };
    commentEd.onTextChange = [this]
    {
        auto* s = session ();
        if (s == nullptr || selectedComp_ < 0 || selectedComp_ >= (int) s->components.size ()) return;
        auto& c = s->components[(size_t) selectedComp_];
        const juce::String t = commentEd.getText ();
        if (c.comment != t) { c.comment = t; s->dirty = true; }
    };
    typeCombo.onChange = [this]
    {
        auto* s = session ();
        if (s == nullptr || selectedComp_ < 0 || selectedComp_ >= (int) s->components.size ()) return;
        const int id = typeCombo.getSelectedId ();
        if (id < 1 || id > 2) return;
        auto& c = s->components[(size_t) selectedComp_];
        if (c.type != id - 1) { c.type = id - 1; s->dirty = true; grid.repaint (); }
    };

    btnAddAmp.onClick = [this]
    {
        auto* s = session ();
        if (s == nullptr || selectedComp_ < 0 || selectedComp_ >= (int) s->components.size ()) return;
        auto& c = s->components[(size_t) selectedComp_];
        c.amplitudes.push_back (sc::model::ModelInput {});
        c.amplitudes.back ().defaultValue = 1.0;
        s->dirty = true;
        rebuildInputRows ();
        updateEditor ();
    };
    btnRemoveAmp.onClick = [this]
    {
        auto* s = session ();
        if (s == nullptr || selectedComp_ < 0 || selectedComp_ >= (int) s->components.size ()) return;
        auto& c = s->components[(size_t) selectedComp_];
        if (c.amplitudes.size () <= 1) return;   // keep at least one amplitude
        c.amplitudes.pop_back ();
        s->dirty = true;
        rebuildInputRows ();
        updateEditor ();
    };

    auto bindBreaksField = [this] (juce::TextEditor& ed, bool isX)
    {
        ed.onTextChange = [this, &ed, isX]
        {
            auto* s = session ();
            if (s == nullptr || selectedComp_ < 0 || selectedComp_ >= (int) s->components.size ()) return;
            auto& c = s->components[(size_t) selectedComp_];
            std::vector<double>& which = isX ? c.xBreaks : c.yBreaks;
            const auto nb = parseBreaks (ed.getText ());
            bool same = nb.size () == which.size ();
            if (same)
                for (size_t i = 0; i < nb.size (); ++i)
                    if (std::abs (nb[i] - which[i]) > 1e-9) { same = false; break; }
            if (same) return;
            const int oldRows = c.numRows (), oldCols = c.numCols ();
            which = nb;
            resizeCellsPreserving (c, oldRows, oldCols);
            s->dirty = true;
            xBreaksEd.setText (joinBreaks (c.xBreaks), juce::dontSendNotification);
            yBreaksEd.setText (joinBreaks (c.yBreaks), juce::dontSendNotification);
            grid.repaint ();
        };
    };
    bindBreaksField (xBreaksEd, true);
    bindBreaksField (yBreaksEd, false);

    auto changeStructure = [this] (bool isX, bool adding)
    {
        auto* s = session ();
        if (s == nullptr || selectedComp_ < 0 || selectedComp_ >= (int) s->components.size ()) return;
        auto& c = s->components[(size_t) selectedComp_];
        std::vector<double>& which = isX ? c.xBreaks : c.yBreaks;
        const int oldRows = c.numRows (), oldCols = c.numCols ();
        if (adding)
        {
            if (which.empty()) which.push_back (1.0);
            else
            {
                const double gap = which.size () >= 2 ? which.back () - which[which.size () - 2] : 1.0;
                which.push_back (which.back () + gap);
            }
        }
        else
        {
            if (which.empty ()) return;
            which.pop_back ();
        }
        resizeCellsPreserving (c, oldRows, oldCols);
        s->dirty = true;
        xBreaksEd.setText (joinBreaks (c.xBreaks), juce::dontSendNotification);
        yBreaksEd.setText (joinBreaks (c.yBreaks), juce::dontSendNotification);
        grid.repaint ();
    };
    btnAddCol.onClick  = [changeStructure] { changeStructure (true, true); };
    btnDelCol.onClick  = [changeStructure] { changeStructure (true, false); };
    btnAddRow.onClick  = [changeStructure] { changeStructure (false, true); };
    btnDelRow.onClick  = [changeStructure] { changeStructure (false, false); };

    grid.onEditCell = [this] (int dr, int dc)
    {
        auto* s = session ();
        if (s == nullptr || selectedComp_ < 0 || selectedComp_ >= (int) s->components.size ()) return;
        showModelCellDialog (this, app, s->components[(size_t) selectedComp_], dr, dc);
        grid.repaint ();
    };

    // ---- visibility ---------------------------------------------------------
    addAndMakeVisible (&btnNew);
    addAndMakeVisible (&btnOpen);
    addAndMakeVisible (&btnSave);
    addAndMakeVisible (&btnSaveAs);
    addAndMakeVisible (&sessionNameLbl);
    addAndMakeVisible (&matchPctLbl);
    addAndMakeVisible (&inclDbLbl);
    addAndMakeVisible (&overDbLbl);
    addAndMakeVisible (&matchPctEd);
    addAndMakeVisible (&inclDbEd);
    addAndMakeVisible (&overDbEd);
    addAndMakeVisible (&listHdr);
    addAndMakeVisible (&btnAddComp);
    addAndMakeVisible (&compList);
    addAndMakeVisible (&nameLbl);
    addAndMakeVisible (&typeLbl);
    addAndMakeVisible (&commentLbl);
    addAndMakeVisible (&nameEd);
    addAndMakeVisible (&typeCombo);
    addAndMakeVisible (&commentEd);
    addAndMakeVisible (&inputsHdr);
    addAndMakeVisible (&btnAddAmp);
    addAndMakeVisible (&btnRemoveAmp);
    addAndMakeVisible (&tableHdr);
    addAndMakeVisible (&xBreaksLbl);
    addAndMakeVisible (&yBreaksLbl);
    addAndMakeVisible (&xBreaksEd);
    addAndMakeVisible (&yBreaksEd);
    addAndMakeVisible (&btnAddCol);
    addAndMakeVisible (&btnDelCol);
    addAndMakeVisible (&btnAddRow);
    addAndMakeVisible (&btnDelRow);
    addAndMakeVisible (&grid);

    syncSettingsFields ();
    refresh ();
}

// ---- state queries ----------------------------------------------------------

int ModelPanel::numComponentsShown() const
{
    auto* s = session ();
    return s ? (int) s->components.size () : 0;
}

juce::String ModelPanel::componentShownName (int index) const
{
    auto* s = session ();
    if (s == nullptr || index < 0 || index >= (int) s->components.size ()) return juce::String ();
    return s->components[(size_t) index].name;
}

int ModelPanel::selectedComponentIndex() const { return selectedComp_; }

// ---- refresh ------------------------------------------------------------------

void ModelPanel::refresh()
{
    auto* s = session ();
    if (s == nullptr) return;
    selectedComp_ = std::clamp (selectedComp_, -1, (int) s->components.size () - 1);
    compList.updateContent ();
    updateEditor ();
    juce::String name = s->filePath.isEmpty () ? "(unsaved session)" : juce::File (s->filePath).getFileName ();
    if (s->dirty) name += " *";
    sessionNameLbl.setText (name, juce::dontSendNotification);
}

void ModelPanel::syncSettingsFields()
{
    auto* s = session ();
    if (s == nullptr) return;
    matchPctEd.setText (fmtNum (s->matchTolerancePct), juce::dontSendNotification);
    inclDbEd.setText (fmtNum (s->inclusionThresholdDb), juce::dontSendNotification);
    overDbEd.setText (fmtNum (s->overSubtractToleranceDb), juce::dontSendNotification);
}

void ModelPanel::updateEditor()
{
    auto* s = session ();
    const bool has = s != nullptr && selectedComp_ >= 0 && selectedComp_ < (int) s->components.size ();

    nameEd.setEnabled (has);
    typeCombo.setEnabled (has);
    commentEd.setEnabled (has);
    btnAddAmp.setEnabled (has);
    btnRemoveAmp.setEnabled (has);
    xBreaksEd.setEnabled (has);
    yBreaksEd.setEnabled (has);
    btnAddCol.setEnabled (has);
    btnDelCol.setEnabled (has);
    btnAddRow.setEnabled (has);
    btnDelRow.setEnabled (has);

    if (!has)
    {
        grid.setComponent (nullptr);
        rebuildInputRows ();
        return;
    }

    auto& c = s->components[(size_t) selectedComp_];
    nameEd.setText (c.name, juce::dontSendNotification);
    typeCombo.setSelectedId (c.type + 1, juce::dontSendNotification);
    commentEd.setText (c.comment, juce::dontSendNotification);

    rebuildInputRows ();
    for (int n = 0; n < (int) inputExprs_.size (); ++n)
    {
        auto* mi = inputForKind (c, n);
        if (mi == nullptr) continue;
        inputExprs_[(size_t) n]->setText (mi->expression, juce::dontSendNotification);
        validateExprColour (inputExprs_[(size_t) n].get (), c);
    }

    xBreaksEd.setText (joinBreaks (c.xBreaks), juce::dontSendNotification);
    yBreaksEd.setText (joinBreaks (c.yBreaks), juce::dontSendNotification);
    grid.setComponent (&c);
}

// ---- input rows -----------------------------------------------------------------

sc::model::ModelInput* ModelPanel::inputForKind (sc::model::Component& c, int kind) const
{
    switch (kind)
    {
        case 0: return &c.inputX;
        case 1: return &c.inputY;
        case 2: return &c.inputSpeed;
        default:
            if (kind >= 3 && kind - 3 < (int) c.amplitudes.size ())
                return &c.amplitudes[(size_t) (kind - 3)];
            return nullptr;
    }
}

int ModelPanel::kindForEditor (juce::TextEditor* ed) const
{
    for (size_t i = 0; i < inputExprs_.size (); ++i)
        if (inputExprs_[i].get () == ed) return (int) i;
    return -1;
}

void ModelPanel::rebuildInputRows()
{
    auto* s = session ();
    const int idx = selectedComp_;
    const bool has = s != nullptr && idx >= 0 && idx < (int) s->components.size ();
    const int want = has ? 3 + (int) s->components[(size_t) idx].amplitudes.size () : 3;

    while ((int) inputExprs_.size () < want)
    {
        const int n = (int) inputExprs_.size ();   // kind: 0=X 1=Y 2=SPEED, 3+i=AMP i+1
        auto lbl = std::make_unique<juce::Label>();
        auto ed  = std::make_unique<juce::TextEditor>();
        auto btn = std::make_unique<juce::TextButton>("Curve");

        const juce::String idBase = n < 3 ? juce::String (("inputX", "inputY", "inputSpeed")[(size_t) n])
                                          : juce::String ("amp") + juce::String (n - 2);
        ed->setComponentID (idBase);
        btn->setComponentID (n < 3 ? juce::String (("curveX", "curveY", "curveSpeed")[(size_t) n])
                                   : juce::String ("ampCurve") + juce::String (n - 2));

        ed->onTextChange = [this, edp = ed.get()] { onInputExprChanged (edp); };
        btn->onClick = [this, edp = ed.get()] { onInputCurveClicked (edp); };

        addAndMakeVisible (lbl.get());
        addAndMakeVisible (ed.get());
        addAndMakeVisible (btn.get());
        inputLabels_.push_back (std::move (lbl));
        inputExprs_.push_back (std::move (ed));
        inputCurveBtns_.push_back (std::move (btn));
    }
    while ((int) inputExprs_.size () > want)
    {
        removeChildComponent (inputLabels_.back ().get());     delete inputLabels_.back ().release ();
        removeChildComponent (inputExprs_.back ().get());      delete inputExprs_.back ().release ();
        removeChildComponent (inputCurveBtns_.back ().get());  delete inputCurveBtns_.back ().release ();
        inputLabels_.pop_back ();
        inputExprs_.pop_back ();
        inputCurveBtns_.pop_back ();
    }

    // Row captions.
    for (size_t n = 0; n < inputLabels_.size (); ++n)
    {
        const juce::String cap = n == 0 ? "X" : n == 1 ? "Y" : n == 2 ? "SPEED"
                                                      : ("AMP " + juce::String ((int) n - 2));
        inputLabels_[n]->setText (cap, juce::dontSendNotification);
    }
}

void ModelPanel::validateExprColour (juce::TextEditor* ed, const sc::model::Component&)
{
    if (ed == nullptr) return;
    sc::model::Expression ex;
    juce::String err;
    bool ok = sc::model::Expression::parse (ed->getText (), ex, &err);
    if (ok && !ex.empty ())
        for (const auto& p : ex.params ())
        {
            bool found = false, ambig = false;
            sc::model::resolveParam (app.activeTables (), p, 0.0, &found, &ambig);
            if (!found) { ok = false; break; }
        }
    ed->setColour (juce::TextEditor::textColourId, ok ? juce::Colours::white : juce::Colours::red);
}

void ModelPanel::onInputExprChanged (juce::TextEditor* ed)
{
    auto* s = session ();
    if (s == nullptr || selectedComp_ < 0 || selectedComp_ >= (int) s->components.size ()) return;
    auto& c = s->components[(size_t) selectedComp_];
    const int kind = kindForEditor (ed);
    auto* mi = inputForKind (c, kind);
    if (mi == nullptr) return;
    const juce::String text = ed->getText ().trim ();
    if (mi->expression != text) { mi->expression = text; s->dirty = true; }
    validateExprColour (ed, c);
}

void ModelPanel::onInputCurveClicked (juce::TextEditor* ed)
{
    auto* s = session ();
    if (s == nullptr || selectedComp_ < 0 || selectedComp_ >= (int) s->components.size ()) return;
    auto& c = s->components[(size_t) selectedComp_];
    const int kind = kindForEditor (ed);
    auto* mi = inputForKind (c, kind);
    if (mi == nullptr) return;
    const juce::String name = kind == 0 ? "X" : kind == 1 ? "Y" : kind == 2 ? "SPEED"
                                                          : ("AMPLITUDE " + juce::String (kind - 2));
    if (showModelCurveDialog (this, name + " curve", mi->curve))
        s->dirty = true;
}

// ---- session operations -----------------------------------------------------------

bool ModelPanel::confirmDiscard()
{
    juce::AlertWindow dlg ("Unsaved model session",
        "You have unsaved changes to the model session.\nDiscard them?",
        juce::AlertWindow::WarningIcon, this);
    dlg.addButton ("Discard", 1);
    dlg.addButton ("Cancel", 0);
    return dlg.runModalLoop () == 1;
}

bool ModelPanel::newSession (bool skipConfirm)
{
    auto* s = session ();
    if (s == nullptr) return false;
    if (!skipConfirm && s->dirty && !confirmDiscard ()) return false;
    s->components.clear ();
    s->filePath = juce::String ();
    s->matchTolerancePct = 3.0;
    s->inclusionThresholdDb = -60.0;
    s->overSubtractToleranceDb = 1.0;
    s->dirty = false;
    selectedComp_ = -1;
    syncSettingsFields ();
    refresh ();
    return true;
}

bool ModelPanel::openSessionFile (const juce::File& f, bool skipConfirm)
{
    auto* s = session ();
    if (s == nullptr) return false;
    if (!f.existsAsFile ())
    {
        juce::AlertWindow dlg ("Open model session", "The file does not exist.",
                               juce::AlertWindow::WarningIcon, this);
        dlg.addButton ("OK", 1);
        dlg.runModalLoop ();
        return false;
    }
    if (!skipConfirm && s->dirty && !confirmDiscard ()) return false;
    juce::String err;
    if (!sc::model::ModelSession::loadFromFile (*s, f, &err))
    {
        juce::AlertWindow dlg ("Open model session", "Could not load the session:\n" + err,
                               juce::AlertWindow::WarningIcon, this);
        dlg.addButton ("OK", 1);
        dlg.runModalLoop ();
        return false;
    }
    selectedComp_ = -1;
    syncSettingsFields ();
    refresh ();
    return true;
}

bool ModelPanel::saveSessionFile (const juce::File& target)
{
    auto* s = session ();
    if (s == nullptr) return false;
    juce::File t = target;
    if (!t.hasFileExtension ("json")) t = t.withFileExtension ("json");
    const bool ok = sc::model::ModelSession::saveToFile (*s, t);
    if (!ok)
    {
        juce::AlertWindow dlg ("Save model session", "Could not save the session to:\n" + t.getFullPathName (),
                               juce::AlertWindow::WarningIcon, this);
        dlg.addButton ("OK", 1);
        dlg.runModalLoop ();
        return false;
    }
    s->filePath = t.getFullPathName ();
    s->dirty = false;
    refresh ();
    return true;
}

// ---- component list model -----------------------------------------------------------

void ModelPanel::ListModel::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel)
{
    auto* s = panel->session ();
    if (s == nullptr || row < 0 || row >= (int) s->components.size ()) return;
    const auto& c = s->components[(size_t) row];

    if (sel)
    {
        g.setColour (juce::Colour::fromRGB (60, 80, 120));
        g.fillRect (0, 0, w, h);
    }

    // Drag-handle affordance (reordering is offered via the right-click menu).
    g.setColour (juce::Colour::fromRGB (150, 155, 165));
    for (int i = -1; i <= 1; ++i)
        g.fillEllipse (7.0f, h * 0.5f + i * 3.2f - 1.0f, 2.0f, 2.0f);

    const bool isHarmonic = c.type == 0;
    g.setColour (isHarmonic ? juce::Colours::orange : juce::Colours::dodgerblue);
    g.setFont (juce::Font (10.5f, juce::Font::bold));
    g.drawText (isHarmonic ? "H" : "N", 16, 0, 14, h, juce::Justification::centred);

    g.setColour (sel ? juce::Colours::white : juce::Colour::fromRGB (215, 220, 230));
    g.setFont (11.5f);
    g.drawText (c.name.isEmpty () ? "(unnamed)" : c.name, 34, 0, w - 40, h, juce::Justification::centredLeft);
}

void ModelPanel::ListModel::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    auto* s = panel->session ();
    if (s == nullptr || row < 0 || row >= (int) s->components.size ()) return;

    if (!e.mods.isRightButtonDown ())
    {
        panel->selectedComp_ = row;
        panel->updateEditor ();
        return;
    }

    juce::PopupMenu menu;
    menu.addItem (1, "Rename...");
    menu.addItem (2, "Move Up",   row > 0);
    menu.addItem (3, "Move Down", row < (int) s->components.size () - 1);
    menu.addSeparator ();
    menu.addItem (4, "Delete");

    const int r = menu.showMenu (juce::PopupMenu::Options ().withMousePosition ());
    if (r == 0) return;

    auto& c = s->components[(size_t) row];
    switch (r)
    {
        case 1:   // Rename...
        {
            auto* w = new sc::OwningAlertWindow ("Rename Component", "New name:",
                                                 juce::AlertWindow::QuestionIcon, panel);
            auto* f = new McField ("Name:", c.name);
            f->setSize (260, 28);
            w->addCustomComponent (f);
            w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));
            f->editor()->setWantsKeyboardFocus (true);
            f->editor()->selectAll ();
            w->enterModalState (false);
            const int rr = w->runModalLoop ();
            if (rr == 1)
            {
                c.name = f->editor()->getText ().trim ();
                s->dirty = true;
            }
            delete w;
            panel->refresh ();
            break;
        }
        case 2:   // Move Up
            s->moveComponent ((size_t) row, (size_t) (row - 1));
            s->dirty = true;
            panel->selectedComp_ = row - 1;
            panel->refresh ();
            break;
        case 3:   // Move Down
            s->moveComponent ((size_t) row, (size_t) (row + 1));
            s->dirty = true;
            panel->selectedComp_ = row + 1;
            panel->refresh ();
            break;
        case 4:   // Delete
            s->removeComponent ((size_t) row);
            s->dirty = true;
            panel->refresh ();
            break;
    }
}

// ---- paint / layout -------------------------------------------------------------------

void ModelPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));
}

void ModelPanel::resized()
{
    const int W = getWidth ();
    const int m = 6;
    int y = m;

    // Toolbar row.
    btnNew.setBounds (m, y, 48, 26);
    btnOpen.setBounds (58, y, 60, 26);
    btnSave.setBounds (120, y, 52, 26);
    btnSaveAs.setBounds (174, y, 74, 26);
    sessionNameLbl.setFont (10.5f);
    sessionNameLbl.setBounds (W - m - 132, y + 4, 132, 18);

    // Session settings row: Match % / Incl dB / Over dB.
    y += 30;
    const int gw = (W - 2 * m) / 3;
    matchPctLbl.setBounds (m, y + 6, gw - 52, 14);
    matchPctEd.setBounds (m + gw - 52, y, 50, 24);
    inclDbLbl.setBounds (m + gw, y + 6, gw - 52, 14);
    inclDbEd.setBounds (m + 2 * gw - 52, y, 50, 24);
    overDbLbl.setBounds (m + 2 * gw, y + 6, gw - 52, 14);
    overDbEd.setBounds (W - m - 50, y, 50, 24);

    // Component list.
    y += 30;
    listHdr.setBounds (m, y + 3, W - 2 * m - 58, 14);
    btnAddComp.setBounds (W - m - 52, y, 52, 20);
    const int listH = std::clamp ((int) (getHeight () * 0.22), 72, 160);
    compList.setBounds (m, y + 16, W - 2 * m, listH);

    // Selected-component editor.
    y += 16 + listH + 4;
    nameLbl.setBounds (m, y + 5, 34, 14);
    nameEd.setBounds (m + 38, y, W - 2 * m - 38, 24);

    y += 26;
    typeLbl.setBounds (m, y + 5, 34, 14);
    typeCombo.setBounds (m + 38, y, 90, 24);
    commentLbl.setBounds (m + 134, y + 5, 56, 14);
    commentEd.setBounds (m + 194, y, W - m - 194, 24);

    // Input expression rows.
    y += 28;
    inputsHdr.setBounds (m, y, W - 2 * m, 14);
    const int iy0 = y + 16;
    for (size_t n = 0; n < inputLabels_.size (); ++n)
    {
        const int ry = iy0 + (int) n * 24;
        inputLabels_[n]->setBounds (m, ry + 5, 56, 14);
        inputExprs_[n]->setBounds (m + 60, ry, W - 2 * m - 60 - 70, 24);
        inputCurveBtns_[n]->setBounds (W - m - 68, ry, 68, 24);
    }
    y = iy0 + (int) inputLabels_.size () * 24;

    btnAddAmp.setBounds (m, y, 70, 24);
    btnRemoveAmp.setBounds (m + 76, y, 70, 24);
    y += 28;

    // Table structure row.
    tableHdr.setBounds (m, y, W - 2 * m, 14);
    y += 16;
    btnAddCol.setBounds (m, y, 58, 24);
    btnDelCol.setBounds (m + 62, y, 58, 24);
    btnAddRow.setBounds (m + 124, y, 58, 24);
    btnDelRow.setBounds (m + 186, y, 58, 24);
    y += 28;

    // Breakpoints row.
    const int half = W / 2;
    xBreaksLbl.setBounds (m, y + 5, 18, 14);
    xBreaksEd.setBounds (m + 20, y, half - m - 30, 24);
    yBreaksLbl.setBounds (half + 6, y + 5, 18, 14);
    yBreaksEd.setBounds (half + 26, y, W - half - m - 26, 24);
    y += 30;

    // Cell grid fills the rest.
    grid.setBounds (m, y, W - 2 * m, std::max (84, getHeight () - m - y));
}
