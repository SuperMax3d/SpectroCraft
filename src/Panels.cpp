/**
 * SpectroCraft - Left-side panels implementation
 */

#include "Panels.h"
#include "FFTHelpers.h"
#include <algorithm>
#include <cctype>
#include <cmath>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW (Open Containing Folder)
#endif

// Forward declaration (defined further down, used by the constructor).
static void fftAxisLimits (int mode, double rate, double& fLo, double& fHi);

juce::String formatTimeCode (double secs)
{
    if (secs < 0) secs = 0;
    const int m = (int) (secs / 60.0);
    const double s = secs - m * 60.0;
    return juce::String (m) + ":" + juce::String (s, 3).paddedLeft ('0', 6);
}

// ------------------------------------------------- Draggable left-panel tabs

namespace {

/** Small floating label that follows the cursor while a tab is dragged.
*/
class TabDragGhost : public juce::Component
{
public:
    explicit TabDragGhost (const juce::String& text)
        : label (text)
    {
        setInterceptsMouseClicks (false, false);   // purely visual
        const int w = std::max (60, 24 + (int) text.length () * 7);
        setSize (w, 26);
    }

    void setHighlighted (bool h)
    {
        if (highlighted != h) { highlighted = h; repaint(); }
    }

private:
    bool highlighted = false;
    juce::String label;

    void paint (juce::Graphics& g) override
    {
        const juce::Rectangle<int> r = getLocalBounds ().reduced (1);
        g.setColour (juce::Colour::fromRGB (58, 58, 66).withAlpha (0.94f));
        g.fillRect (r);
        g.setColour (highlighted ? juce::Colour::fromRGB (120, 170, 255)
                                 : juce::Colour::fromRGB (160, 160, 170));
        g.drawRect (r);
        g.setColour (juce::Colour (255, 255, 255));
        g.drawText (label, getLocalBounds ().reduced (8, 2), juce::Justification::centred);
    }

    JUCE_DECLARE_NON_COPYABLE (TabDragGhost)
};

} // namespace

DragTabButton::DragTabButton (const juce::String& name, juce::TabbedButtonBar& ownerBar,
                              juce::TabbedComponent* ownerTabbed, TabDragContext* ctx)
    : TabBarButton (name, ownerBar), context (ctx), ownerTabbed_ (ownerTabbed) {}

DragTabButton::~DragTabButton()
{
    delete ghost;
}

void DragTabButton::mouseDown (const juce::MouseEvent& e)
{
    dragArmed = true;
    dragging  = false;
    startLocalPos = juce::Point<float> ((float) e.position.x, (float) e.position.y);
    Button::mouseDown (e);
}

void DragTabButton::mouseDrag (const juce::MouseEvent& e)
{
    if (!dragging && dragArmed)
    {
        const float dx = (float) e.position.x - startLocalPos.x;
        const float dy = (float) e.position.y - startLocalPos.y;
        if (dx * dx + dy * dy > 36.0f)   // ~6 px movement threshold
            beginDrag ();
    }

    if (dragging)
    {
        // Keep the ghost under the cursor and highlight a valid drop target.
        const auto global = localPointToGlobal (
            juce::Point<int> ((int) e.position.x, (int) e.position.y));
        if (auto* parent = ghost != nullptr ? ghost->getParentComponent () : nullptr)
        {
            const auto topTL = parent->localPointToGlobal (juce::Point<int> (0, 0));
            ghost->setBounds ((int) global.x - topTL.x + 14,
                             (int) global.y - topTL.y + 16,
                             ghost->getWidth(), ghost->getHeight());
        }
        DragTabButton* target = findHovered (context, global);
        if (ghost)
            static_cast<TabDragGhost*> (ghost)->setHighlighted (target != nullptr && target != this);
    }
    else
    {
        Button::mouseDrag (e);   // plain hover: keep the default pressed look
    }
}

void DragTabButton::mouseUp (const juce::MouseEvent& e)
{
    if (dragging)
    {
        endDrag (e);            // drop handled ourselves; no tab click fires
        return;
    }
    dragArmed = false;
    Button::mouseUp (e);        // normal click -> selects the tab as usual
}

void DragTabButton::beginDrag()
{
    dragging = true;
    auto* top = getTopLevelComponent();
    if (!top) return;
    ghost = new TabDragGhost (getButtonText());
    top->addChildComponent (ghost);   // added last -> painted above the other children
}

void DragTabButton::endDrag (const juce::MouseEvent& e)
{
    dragging  = false;
    dragArmed = false;

    const auto global = localPointToGlobal (
        juce::Point<int> ((int) e.position.x, (int) e.position.y));

    if (context && context->onMove && ownerTabbed_)
    {
        DragTabButton* target = findHovered (context, global);
        if (target != nullptr && target != this)
        {
            auto& toBar = target->getTabbedButtonBar();
            const auto tl = target->localPointToGlobal (juce::Point<int> (0, 0));
            int insertIdx = toBar.indexOfTabButton (target);
            if (global.x > tl.x + target->getWidth() / 2) ++insertIdx;   // drop on the right half

            context->onMove (ownerTabbed_, getTabbedButtonBar().indexOfTabButton (this),
                             static_cast<DragTabButton*> (target)->ownerTabbed_, insertIdx);
        }
    }

    delete ghost;
    ghost = nullptr;
}

DragTabButton* DragTabButton::findHovered (TabDragContext* ctx, const juce::Point<int>& globalPos)
{
    if (!ctx) return nullptr;
    for (auto* bar : ctx->bars)
    {
        if (!bar) continue;
        auto& bb = bar->getTabbedButtonBar();
        for (int i = 0; i < bb.getNumTabs (); ++i)
        {
            auto* btn = bb.getTabButton (i);
            if (!btn) continue;
            const auto tl = btn->localPointToGlobal (juce::Point<int> (0, 0));
            juce::Rectangle<int> r (tl.x, tl.y, btn->getWidth(), btn->getHeight());
            if (r.contains (globalPos))
                return static_cast<DragTabButton*> (btn);
        }
    }
    return nullptr;
}

DragTabBar::DragTabBar (juce::TabbedButtonBar::Orientation orientation, TabDragContext* ctx)
    : juce::TabbedComponent (orientation), context (ctx) {}

juce::TabBarButton* DragTabBar::createTabButton (const juce::String& tabName, int tabIndex)
{
    return new DragTabButton (tabName, getTabbedButtonBar(), this, context);
}

static double parseTimeCode (const juce::String& text)
{
    // Accepts "12", "12.5", "0:04.536", "1:02"
    const int colon = text.indexOfChar (':');
    if (colon >= 0)
    {
        const double m = text.substring (0, colon).getDoubleValue();
        const double s = text.substring (colon + 1).getDoubleValue();
        return m * 60.0 + s;
    }
    return text.getDoubleValue();
}

// Rectangle helper (JUCE 9 has no withXOffset)
static juce::Rectangle<int> xoffset (const juce::Rectangle<int>& r, int dx)
{
    return juce::Rectangle<int> (r.getX() + dx, r.getY(), std::max (0, r.getWidth() - dx),
                                 r.getHeight());
}

// ================================================================= FileListPanel

FileListPanel::FileListPanel (Application& appIn)
    : app (appIn), openBtn ("Open..."), closeBtn ("Close")
{
    openBtn.onClick = [this] { if (onOpenRequested) onOpenRequested(); };
    closeBtn.onClick = [this] {
        // Closes the ACTIVE file; Application fires the callbacks that refresh
        // this list and reset the views to whatever remains.
        app.closeFile();
    };

    openBtn.setTooltip ("Open an audio file (WAV, AIFF, FLAC, MP3 or Ogg). "
                        "You can also drag & drop files onto the window - every "
                        "dropped file is added to this list.");
    closeBtn.setTooltip ("Close the active file. Its playhead, selection and "
                         "stored snapshots are discarded; other open files keep their state.");
    addAndMakeVisible (&openBtn);
    addAndMakeVisible (&closeBtn);
}

void FileListPanel::refresh()
{
    closeBtn.setEnabled (app.numDocs() > 0);
    repaint();
}

juce::Rectangle<int> FileListPanel::rowRect (int i) const
{
    return { 2, kListTop + (i - listScroll) * kRowH, getWidth() - 4, kRowH };
}

int FileListPanel::rowAt (int x, int y) const
{
    for (int i = 0; i < app.numDocs(); ++i)
        if (rowRect (i).contains (x, y))
            return i;
    return -1;
}

void FileListPanel::activateDoc (int i)
{
    if (i >= 0 && i < app.numDocs())
        app.activate (i);   // fires onActiveDocChanged: views + panels sync to it
}

void FileListPanel::closeDoc (int i)
{
    if (i >= 0 && i < app.numDocs())
        app.closeDoc (i);
}

void FileListPanel::openContainingFolder (int row)
{
    if (row < 0 || row >= app.numDocs()) return;
    const juce::File f = app.docs[(size_t) row]->file;
    if (f.getFullPathName().isEmpty()) return;   // brand-new document: no folder yet
    const juce::File folder = f.getParentDirectory();
#if JUCE_WINDOWS
    // Opens Windows Explorer at the folder.
    ShellExecuteW (nullptr, L"open", folder.getFullPathName().toWideCharPointer(),
                   nullptr, nullptr, SW_SHOWNORMAL);
#else
    const juce::String cmd = (JUCE_MACOS ? "open " : "xdg-open ") + "\""
                            + folder.getFullPathName() + "\"";
    std::system (cmd.toRawUTF8());
#endif
}

void FileListPanel::showFileMenu (int row)
{
    const FileDoc& d = *app.docs[(size_t) row];
    juce::PopupMenu m;
    m.addItem (1, "Activate", true, false);
    m.addItem (2, "Close", true, false);
    m.addItem (3, "Set Unit and Scale...", true, false);
    // Brand-new documents have no folder to open yet.
    m.addItem (4, "Open Containing Folder",
               !d.file.getFullPathName().isEmpty(), false);
    m.addSeparator();
    m.addItem (5, "Close Panel");
    const int r = m.showMenu (juce::PopupMenu::Options().withMousePosition());
    if (r == 1) activateDoc (row);
    else if (r == 2) closeDoc (row);
    else if (r == 3) showScaleDialog (row);
    else if (r == 4) openContainingFolder (row);
    else if (r == 5 && onCloseRequested) onCloseRequested();
}

juce::AlertWindow* FileListPanel::createScaleDialog (FileDoc& d)
{
    auto* dlg = new juce::AlertWindow (
        "Set Unit and Scale",
        "Calibrate the physical units of this file.\n"
        "Full scale (|1.0|) equals the value you enter below.",
        juce::AlertWindow::NoIcon);
    dlg->setColour (juce::AlertWindow::backgroundColourId,
                   juce::Colour::fromRGB (45, 45, 48));

    dlg->addComboBox ("unit", { "None", "Volts", "Pascal", "g" }, "Units:");
    auto* unitCombo = dlg->getComboBoxComponent ("unit");

    dlg->addTextEditor (
        "value",
        d.scale.fullScaleValue > 0.0 ? juce::String (d.scale.fullScaleValue) : "1.0",
        "Full scale:");
    auto* valEdit = dlg->getTextEditor ("value");

    if (unitCombo == nullptr || valEdit == nullptr)
    {
        delete dlg;
        return nullptr;
    }

    // Item ids are 1..4 in the same order as ScaleCalibration::Unit (0..3).
    unitCombo->setSelectedId ((int) d.scale.unit + 1,
                             juce::NotificationType::dontSendNotification);

    // The value field only makes sense when a unit is selected.
    auto syncEnabled = [unitCombo, valEdit] {
        valEdit->setEnabled (unitCombo->getSelectedId() != 1);
    };
    unitCombo->onChange = syncEnabled;
    syncEnabled();

    dlg->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dlg->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    // Focus the field with its text pre-selected so a new value can be typed
    // in immediately. AlertWindow disables keyboard focus on its editors, so
    // re-enable it first or grabKeyboardFocus() would bounce to a button.
    valEdit->setWantsKeyboardFocus (true);
    valEdit->selectAll();
    valEdit->grabKeyboardFocus();

    return dlg;
}

void FileListPanel::showScaleDialog (int row)
{
    if (row < 0 || row >= app.numDocs()) return;
    FileDoc& d = *app.docs[(size_t) row];

    auto* dlg = createScaleDialog (d);
    const int result = dlg->runModalLoop();
    juce::ComboBox* unitCombo = dlg->getComboBoxComponent ("unit");
    juce::TextEditor* valEdit = dlg->getTextEditor ("value");

    if (result == 1 && unitCombo != nullptr && valEdit != nullptr)
    {
        const auto unit = (ScaleCalibration::Unit) (unitCombo->getSelectedId() - 1);
        const double v = valEdit->getText().trim().getDoubleValue();
        if (unit == ScaleCalibration::Unit::None || v > 0.0)
        {
            d.scale.unit = unit;
            d.scale.fullScaleValue = (unit == ScaleCalibration::Unit::None) ? 0.0 : v;
            refresh();   // file list shows the new scale under the entry
            if (app.onStateChange) app.onStateChange();   // repaint views with the new units
        }
        else
        {
            juce::AlertWindow err ("Set Unit and Scale",
                                   "Please enter a positive value for the full-scale level.",
                                   juce::AlertWindow::NoIcon);
            err.setColour (juce::AlertWindow::backgroundColourId,
                           juce::Colour::fromRGB (45, 45, 48));
            err.addButton ("OK", 1);
            err.runModalLoop();
        }
    }
    delete dlg;
}

// Right-click "Close Panel" menu shared by every left-column panel. Returns
// true when the user picked the item.
static bool showClosePanelMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "Close Panel");
    return m.showMenu (juce::PopupMenu::Options().withMousePosition()) == 1;
}

void FileListPanel::mouseDown (const juce::MouseEvent& e)
{
    if (!e.mods.isRightButtonDown()) return;
    const int row = rowAt ((int) e.x, (int) e.y);
    if (row < 0)
    {
        // Empty area below the list: offer to close the panel itself.
        if (showClosePanelMenu() && onCloseRequested) onCloseRequested();
        return;
    }
    showFileMenu (row);
}

void FileListPanel::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (!e.mods.isLeftButtonDown()) return;
    const int row = rowAt ((int) e.x, (int) e.y);
    if (row >= 0) activateDoc (row);
}

void FileListPanel::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (app.numDocs() <= 1 || juce::approximatelyEqual (wheel.deltaY, 0.0f)) return;
    // Negative deltaY = wheel pushed down = scroll the list forward.
    listScroll += (wheel.deltaY < 0.0f ? 1 : -1)
                 * std::max (1, std::abs ((int) wheel.deltaY));
    repaint();
}

void FileListPanel::resized()
{
    auto b = getLocalBounds().reduced (4, 4);
    openBtn.setBounds (b.removeFromTop (26).removeFromLeft (90));
    closeBtn.setBounds (openBtn.getRight() + 8, openBtn.getY(), 70, 26);
}

void FileListPanel::paint (juce::Graphics& g)
{
    auto b = getLocalBounds();
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));

    const auto listArea = juce::Rectangle<int> (2, kListTop,
                                                std::max (1, b.getWidth() - 4),
                                                std::max (1, b.getHeight() - kListTop));

    if (app.numDocs() == 0)
    {
        g.setColour (juce::Colours::grey);
        g.setFont (11.0f);
        g.drawText ("No files open", listArea, juce::Justification::centred);
        return;
    }

    const int totalRows = app.numDocs();
    const int visibleRows = std::max (1, listArea.getHeight() / kRowH);
    listScroll = std::clamp (listScroll, 0, std::max (0, totalRows - visibleRows));

    g.setFont (11.0f);
    for (int i = 0; i < totalRows; ++i)
    {
        auto r = rowRect (i).getIntersection (listArea);
        if (r.isEmpty()) continue;

        const FileDoc& d = *app.docs[(size_t) i];
        const bool active = (i == app.activeDocIndex);

        if (active)
        {
            g.setColour (juce::Colour::fromRGB (48, 72, 100));
            g.fillRect (r);
        }

        // Line 1: file name. Line 2: its actual properties.
        const auto line1 = r.removeFromTop (kRowH / 2);
        g.setColour (active ? juce::Colours::white : juce::Colour::fromRGB (190, 195, 205));
        g.drawText (d.name, line1.reduced (4, 1), juce::Justification::centredLeft, false);

        juce::String props = juce::String ((int) d.engine.sampleRate()) + " Hz | "
                           + juce::String (d.engine.numChannels()) + " ch | "
                           + formatTimeCode (d.engine.duration());
        if (!d.scale.shortDescription().isEmpty())
            props += " | " + d.scale.shortDescription();
        g.setColour (juce::Colour::fromRGB (130, 135, 145));
        g.drawText (props, r.reduced (4, 1), juce::Justification::centredLeft, false);
    }
}

// ================================================================= FFTAnalysisView

// Standard IEC 61260-1 1/3-octave centre frequencies, 50 Hz .. 16 kHz.
static const double kThirdOctCenters[] = {
    50.0, 63.0, 80.0, 100.0, 125.0, 160.0, 200.0, 250.0, 315.0, 400.0,
    500.0, 630.0, 800.0, 1000.0, 1250.0, 1600.0, 2000.0, 2500.0, 3150.0,
    4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0, 16000.0
};
static constexpr int kNumThirdOctBands = (int) (sizeof (kThirdOctCenters) / sizeof (kThirdOctCenters[0]));

const double* FFTAnalysisView::thirdOctCentres() noexcept { return kThirdOctCenters; }
int FFTAnalysisView::numThirdOctBands() noexcept { return kNumThirdOctBands; }

// Only a subset of band centres gets an axis label (every ~3rd band), so the
// 26 grid lines do not crowd the bottom axis.
static bool isThirdOctLabelFreq (double f)
{
    for (double c : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        if (std::abs (f - c) < 1e-6)
            return true;
    return false;
}

// Average an FFT magnitude curve into 1/3-octave bands.
// mag holds dB-normalized values in [0,1] for an n-point FFT at the given rate;
// returns one normalized value per band whose centre is <= Nyquist. Power is
// averaged over all bins inside each band (band edges = centre * 2^(+-1/6)).
// Cap on the number of averaged FFT windows per channel for wide selections.
static constexpr int64_t kMaxAvgWindows = 2048;

static std::vector<float> toThirdOctBands (const std::vector<float>& mag, int n,
                                           double rate)
{
    const double nyq = rate * 0.5;
    // Same fixed normalization floor as the live curve (see kNormFloorDb).
    const double dBMin = -160.0;
    std::vector<float> out;
    for (int b = 0; b < kNumThirdOctBands && kThirdOctCenters[b] <= nyq; ++b)
    {
        const double fc = kThirdOctCenters[b];
        const double lo = fc * std::pow (2.0, -1.0 / 6.0);
        const double hi = fc * std::pow (2.0,  1.0 / 6.0);
        int kLo = (int) std::ceil (lo * n / rate);
        int kHi = (int) std::floor (hi * n / rate);
        kLo = std::max (1, kLo);
        kHi = std::min (n / 2, kHi);

        double sumPow = 0.0;
        int cnt = 0;
        for (int k = kLo; k <= kHi && k < (int) mag.size(); ++k)
        {
            const double dB = dBMin * (1.0 - (double) mag[k]);   // undo normalization
            const double m = std::pow (10.0, dB / 20.0);
            sumPow += m * m;
            ++cnt;
        }
        const double bm = cnt > 0 ? std::sqrt (sumPow / (double) cnt) : 0.0;
        const double dBb = 20.0 * std::log10 (std::max (1e-12, bm));
        out.push_back ((float) std::clamp ((dBb - dBMin) / (-dBMin), 0.0, 1.0));
    }
    return out;
}

// Curve colours for the live spectrum (one per visible channel).
static const juce::Colour fftColours[] = {
    juce::Colour::fromRGB (255, 48, 144),   // pink
    juce::Colour::fromRGB (48, 192, 255),   // cyan
    juce::Colour::fromRGB (120, 255, 120),  // green
    juce::Colour::fromRGB (255, 220, 60)    // yellow
};

// Snapshot slot colours: button i+1 wears snapColours[i] as its background and
// its stored curve is drawn in the same colour.
static const juce::Colour snapColours[] = {
    juce::Colour::fromRGB (255, 0, 0),      // red
    juce::Colour::fromRGB (255, 127, 0),    // orange
    juce::Colour::fromRGB (255, 255, 0),    // yellow
    juce::Colour::fromRGB (109, 242, 0),    // light green
    juce::Colour::fromRGB (65, 189, 58),    // dark green
    juce::Colour::fromRGB (2, 238, 255),    // teal
    juce::Colour::fromRGB (45, 152, 252),   // blue
    juce::Colour::fromRGB (255, 5, 247)     // magenta
};

SnapshotButton::SnapshotButton() : juce::Button (juce::String())
{
}

void SnapshotButton::paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted,
                                  bool /*shouldDrawButtonAsDown*/)
{
    auto b = getLocalBounds().reduced (1);
    if (b.isEmpty()) return;

    const bool on = getToggleState();

    // The background always shows the colour of the curve this button
    // reproduces; toggled ON is the most obvious state.
    juce::Colour fill;
    if (on)            fill = col;
    else if (stored)   fill = col.darker (0.45f);
    else               fill = col.darker (0.72f);
    if (shouldDrawButtonAsHighlighted && !on)
        fill = fill.brighter (0.18f);

    g.setColour (fill);
    g.fillRect (b);

    g.setColour (on ? juce::Colours::white
                    : (stored ? col.brighter (0.35f)
                             : juce::Colour::fromRGB (90, 90, 100)));
    g.drawRect (b, on ? 2 : 1);

    g.setColour (on ? fill.contrasting()
                   : (stored ? juce::Colour::fromRGB (215, 215, 225)
                            : juce::Colour::fromRGB (160, 160, 170)));
    g.setFont (11.0f);
    g.drawText (getButtonText(), b, juce::Justification::centred, false);
}

void SnapshotButton::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isRightButtonDown() && onRightClick)
    { onRightClick (); return; }
    juce::Button::mouseDown (e);
}

// ------------------------------------------------- History panel

HistoryPanel::HistoryPanel (Application& appIn)
    : app (appIn)
{
    refresh();
}

void HistoryPanel::refresh()
{
    rows_.clear();
    currentState_ = 0;

    if (FileDoc* d = app.doc())
    {
        ::UndoManager& u = d->undo;
        // Row 0 is the state right after the file was loaded; one row per
        // recorded action follows, oldest first. Redoable actions come last.
        rows_.push_back ({ "Loaded " + d->name, false });
        for (size_t i = 0; i < u.undoSize(); ++i)
            rows_.push_back ({ u.undoAt(i).description.isEmpty() ? juce::String ("Edit") : u.undoAt(i).description,
                               false });
        for (size_t i = 0; i < u.redoSize(); ++i)
            rows_.push_back ({ u.redoAt(i).description.isEmpty() ? juce::String ("Edit") : u.redoAt(i).description,
                               true });
        currentState_ = (int) u.undoSize();
    }

    repaint();
}

juce::Rectangle<int> HistoryPanel::listArea() const
{
    auto b = getLocalBounds();
    return { 2, kHeaderH, std::max (1, b.getWidth() - 4), std::max (1, b.getHeight() - kHeaderH) };
}

juce::Rectangle<int> HistoryPanel::rowRect (int i) const
{
    const auto list = listArea();
    return { list.getX(), list.getY() + (i - scroll_) * kRowH, list.getWidth(), kRowH };
}

int HistoryPanel::rowAt (int x, int y) const
{
    const auto list = listArea();
    if (!list.contains (x, y)) return -1;
    const int i = scroll_ + (y - list.getY()) / kRowH;
    return (i >= 0 && i < (int) rows_.size()) ? i : -1;
}

void HistoryPanel::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isRightButtonDown())
    {
        if (showClosePanelMenu() && onCloseRequested) onCloseRequested();
        return;
    }
    const int r = rowAt (e.x, e.y);
    if (r < 0 || !onJumpToState) return;
    onJumpToState (r);   // MainWindow performs the jump and refreshes everything
}

void HistoryPanel::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (juce::approximatelyEqual (wheel.deltaY, 0.0f)) return;
    scroll_ += (wheel.deltaY < 0.0f ? 1 : -1) * std::max (1, std::abs ((int) wheel.deltaY));
    repaint();
}

void HistoryPanel::paint (juce::Graphics& g)
{
    auto b = getLocalBounds();
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));

    // Header: title left, applied-count right.
    g.setFont (12.0f);
    g.setColour (juce::Colours::white);
    g.drawText ("History", juce::Rectangle<int> (8, 3, 160, kHeaderH - 6),
                juce::Justification::left, false);

    const int totalActions = std::max (0, (int) rows_.size() - 1);
    g.setFont (11.0f);
    g.setColour (juce::Colour::fromRGB (170, 170, 180));
    g.drawText (juce::String (currentState_) + " of " + juce::String (totalActions) + " applied",
                juce::Rectangle<int> (b.getWidth() - 168, 4, 160, kHeaderH - 8),
                juce::Justification::right, false);

    const auto list = listArea();
    if (app.doc() == nullptr)
    {
        g.setColour (juce::Colours::grey);
        g.setFont (11.0f);
        g.drawText ("No file open", list, juce::Justification::centred);
        return;
    }

    const int totalRows = (int) rows_.size();
    const int visibleRows = std::max (1, list.getHeight() / kRowH);
    scroll_ = std::clamp (scroll_, 0, std::max (0, totalRows - visibleRows));

    g.setFont (11.0f);
    for (int i = 0; i < totalRows; ++i)
    {
        auto r = rowRect (i).getIntersection (list);
        if (r.isEmpty()) continue;

        // Highlight only this row's band — fillAll() would paint the whole
        // clip region and hide every row drawn before it.
        const bool current = (i == currentState_);
        if (current)
        {
            g.setColour (juce::Colour::fromRGB (58, 64, 80));
            g.fillRect (r);
        }

        // Past rows are bright, future (redoable) ones dimmed.
        g.setColour (rows_[(size_t) i].future ? juce::Colour::fromRGB (120, 120, 128)
                                               : juce::Colours::white);
        g.drawText (juce::String (i) + ". " + rows_[(size_t) i].text,
                    r.reduced (6, 1), juce::Justification::left, false);
    }
}

FFTAnalysisView::FFTAnalysisView (Application& appIn)
    : app (appIn)
{
    // Restore the persisted frequency-analysis state (axis mode + FFT size).
    axisMode_ = std::clamp (app.settings.fftAxisMode, 0, 2);

    for (int n = 1024; n <= 65536; n *= 2)
        fftSizeCombo.addItem (juce::String (n), n);
    localN = std::clamp (app.settings.fftLocalN, 1024, 65536);
    fftSizeCombo.setSelectedId (localN, juce::dontSendNotification);
    fftSizeCombo.onChange = [this] {
        // Independent from the spectrogram's FFT size.
        localN = fftSizeCombo.getSelectedId();
        app.settings.fftLocalN = localN;   // persisted between sessions
        settingsGen++;               // stored snapshots must be recomputed too
        mags.clear(); cachedN = 0;   // invalidate cached spectrum
        recomputeSnapshots();
        if (app.onStateChange) app.onStateChange();   // triggers the settings save
        repaint();
    };

    // JUCE ComboBox item IDs must be non-zero.
    windowCombo.addItem ("Rectangular", 1);
    windowCombo.addItem ("Hann", 2);
    windowCombo.addItem ("Hamming", 3);
    windowCombo.setSelectedId (std::clamp (app.settings.windowType, 0, 2) + 1,
                               juce::dontSendNotification);
    windowCombo.onChange = [this] {
        settingsGen++;               // stored snapshots must be recomputed too
        mags.clear(); cachedN = 0;
        recomputeSnapshots();
        repaint();
    };

    axisCombo.addItem ("FFT Linear", 1);
    axisCombo.addItem ("FFT Log", 2);
    axisCombo.addItem ("1/3 Octave", 3);
    axisCombo.setSelectedId (axisMode_ + 1, juce::dontSendNotification);
    axisCombo.onChange = [this] {
        axisMode_ = axisCombo.getSelectedId() - 1;
        app.settings.fftAxisMode = axisMode_;   // persisted between sessions
        thirdOctMags.clear();   // band cache is only valid for mode 2
        refreshSnapshotBands(); // re-derive stored snapshots' bands for the new mode
        resetDisplayRanges();   // each mode has its own default frequency window
        if (app.onStateChange) app.onStateChange();   // triggers the settings save
    };

    for (int i = 0; i < 8; ++i)
    {
        // The button's background always shows the colour of the curve it
        // reproduces (same mapping as the overlay drawing in paint()).
        snapBtns[i].setCurveColour (snapColours[i]);
        snapBtns[i].setButtonText (juce::String (i + 1));
        snapBtns[i].setClickingTogglesState (true);
        snapBtns[i].onClick = [this, i] { handleSnapshotClick (i); };
        snapBtns[i].onRightClick = [this, i] { showSnapshotMenu (i); };
        updateSnapshotTooltip (i);
    }

    fftSizeCombo.setTooltip ("FFT size of this analysis (independent of the spectrogram's). "
                             "Stored snapshots are recomputed at the new size.");
    windowCombo.setTooltip ("Window function applied to each analysed frame: Rectangular, Hann or Hamming. "
                            "Stored snapshots are recomputed with the new window.");
    axisCombo.setTooltip ("Frequency axis mode: FFT Linear (linear Hz), FFT Log (logarithmic) or "
                          "1/3 Octave (IEC 61260-1 band plot from 50 Hz to 16 kHz).");

    addAndMakeVisible (&fftSizeCombo);
    addAndMakeVisible (&windowCombo);
    addAndMakeVisible (&axisCombo);
    for (auto& b : snapBtns) addAndMakeVisible (&b);

    resetDisplayRanges();   // start at the mode defaults for the current file
}

// Reset the local display ranges: frequency window to the axis-mode default,
// level window to 0 .. -80 dB.
void FFTAnalysisView::resetDisplayRanges()
{
    const double rate = app.audioEngine().sampleRate();
    fftAxisLimits (axisMode_, rate, fLo_, fHi_);
    dbTop_ = 0.0;
    dbBottom_ = -80.0;
    repaint();
}

void FFTAnalysisView::onSampleRateChanged()
{
    if (!app.audioEngine().isFileLoaded()) return;
    // After a resample the old frequency window may extend beyond the new
    // Nyquist (the curve would be squashed into part of the plot), and any
    // stored snapshots were computed at the old rate. Reset to this mode's
    // defaults for the current rate and force every snapshot to recompute
    // from its source engine.
    const double rate = app.audioEngine().sampleRate();
    fftAxisLimits (axisMode_, rate, fLo_, fHi_);
    settingsGen++;
    recomputeSnapshots();
    repaint();
}

// Level-window mapping: dB value -> pixel y inside the plot area.
float FFTAnalysisView::levelToY (double db) const
{
    const auto plot = plotBounds();
    const double span = std::max (1e-9, dbTop_ - dbBottom_);
    return (float) (plot.getY() + (dbTop_ - db) / span * plot.getHeight());
}

double FFTAnalysisView::yToLevel (float y) const
{
    const auto plot = plotBounds();
    const double t = std::clamp ((double) (y - plot.getY()) / std::max (1, plot.getHeight()), 0.0, 1.0);
    return dbTop_ + t * (dbBottom_ - dbTop_);
}

// Zoom the frequency window around the cursor x position.
void FFTAnalysisView::zoomFreqAtX (int x, double factor)
{
    const auto plot = plotBounds();
    if (plot.getWidth() <= 0 || fHi_ <= fLo_) return;

    const double rate = app.audioEngine().sampleRate();
    const double nyq = rate * 0.5;
    const double fCursor = std::clamp ((double) xToFreq (x), 1e-9, nyq);

    if (axisMode_ == 1)
    {
        // Log axis: zoom in log space.
        const double llo = std::log (fLo_), lhi = std::log (fHi_), lf = std::log (fCursor);
        fLo_ = std::exp (std::clamp (lf + (llo - lf) / factor, 0.0, std::log (nyq)));
        fHi_ = std::exp (std::clamp (lf + (lhi - lf) / factor, 0.0, std::log (nyq)));
    }
    else
    {
        // Linear axis (and 1/3-octave): zoom in linear Hz.
        fLo_ = std::clamp (fCursor + (fLo_ - fCursor) / factor, 0.0, nyq);
        fHi_ = std::clamp (fCursor + (fHi_ - fCursor) / factor, 0.0, nyq);
    }

    if (fHi_ <= fLo_ * 1.0001)
    {
        // Degenerate: fall back to the mode defaults.
        fftAxisLimits (axisMode_, rate, fLo_, fHi_);
    }
    repaint();
}

// Zoom the level window around the cursor y position.
void FFTAnalysisView::zoomLevelAtY (float y, double factor)
{
    const auto plot = plotBounds();
    if (plot.getHeight() <= 0) return;

    const double dbCursor = std::clamp ((double) yToLevel (y), -240.0, 40.0);
    double top = dbTop_ + (dbTop_ - dbCursor) / factor;
    double bot = dbBottom_ + (dbBottom_ - dbCursor) / factor;

    // Keep the window span in a sane range and stay inside absolute limits.
    const double span = std::clamp (top - bot, 10.0, 320.0);
    top = std::clamp (top, -240.0 + span * 0.5, 40.0);
    bot = top - span;
    if (bot < -240.0) { bot = -240.0; top = bot + span; }

    dbTop_ = top;
    dbBottom_ = bot;
    repaint();
}

// Frequency-axis limits for the current mode.
static void fftAxisLimits (int mode, double rate, double& fLo, double& fHi)
{
    if (mode == 0) { fLo = 0.0; fHi = rate * 0.5; }
    else if (mode == 2 && rate * 0.5 > 50.0)
        { fLo = 50.0; fHi = std::min (rate * 0.5, 16000.0); }   // 1/3-octave range
    else          { fLo = 20.0; fHi = std::min (rate * 0.5, 20000.0); }
}

// Core averaging computation shared by the live spectrum and every stored
// snapshot: analyse region [t0, t1] with windows of exactly n samples at the
// current window type. A region wider than one window is covered by several
// evenly spaced windows whose linear magnitudes are averaged (the classic
// "average over the selection" behaviour); a point or short region gets a
// single window centred on it.
void FFTAnalysisView::computeRegionFrom (AudioEngine& eng, const std::vector<int>& chans,
                                         double t0, double t1, int n,
                                         std::vector<std::vector<float>>& outMags)
{
    const double rate = eng.sampleRate();
    const double dur = eng.duration();

    // Where to place the FFT windows (sample offsets).
    std::vector<int64_t> winStarts;
    {
        const double regionLenS = std::max (0.0, (t1 - t0) * rate);   // in samples
        if (regionLenS <= (double) n)
        {
            int64_t s = (int64_t) ((t0 + t1) * 0.5 * rate) - n / 2;   // centre on the region
            const int64_t maxStart = std::max<int64_t> (0, (int64_t) (dur * rate) - n);
            winStarts.push_back (std::clamp<int64_t> (s, 0, maxStart));
        }
        else
        {
            // Cap the work: at most kMaxAvgWindows windows and ~4M samples.
            const int numW = (int) std::min<int64_t> ({ kMaxAvgWindows,
                                                        (int64_t) std::ceil (regionLenS / n),
                                                        std::max<int64_t> (1, 4000000 / n) });
            for (int i = 0; i < numW; ++i)
            {
                const double s = t0 * rate + ((double) i * (regionLenS - n) / (numW - 1));
                winStarts.push_back ((int64_t) std::clamp (s, 0.0, dur * rate - n));
            }
        }
    }

    outMags.clear();
    const int order = (int) std::log2 ((double) n);

    // Window function, computed once and shared by all channels.
    std::vector<float> win ((size_t) n, 1.0f);
    const int wt = windowCombo.getSelectedId() - 1;
    if (wt == 1 || wt == 2)
    {
        for (int i = 0; i < n; ++i)
            win[(size_t) i] = (wt == 1)
                ? 0.5f * (1.0f - std::cos (2.0 * juce::MathConstants<double>::pi * i / n))
                : 0.54f - 0.46f * std::cos (2.0 * juce::MathConstants<double>::pi * i / (n - 1));
    }

    // Fixed normalization floor: the stored magnitudes do not depend on the
    // visible level window, so zooming out never changes the curve data.
    const double dBMin = -FFTAnalysisView::kNormFloorDb;
    for (int ch : chans)
    {
        if (ch < 0 || ch >= eng.numChannels()) continue;

        // Average the linear magnitudes of all windows in this channel.
        std::vector<double> acc ((size_t) (n / 2 + 1), 0.0);
        std::vector<float> samples ((size_t) n, 0.0f);   // zero-filled: partial reads at the file end stay silent
        std::vector<float> buf ((size_t) n * 2, 0.0f);
        for (int64_t s : winStarts)
        {
            std::fill (samples.begin(), samples.end(), 0.0f);
            if (!eng.readSamples (ch, s, n, samples.data())) continue;
            for (int i = 0; i < n; ++i)
                buf[i] = samples[i] * win[(size_t) i];
            scfft::realForward (buf.data(), order);      // AVX2 when available via scfft
            for (int k = 0; k <= n / 2; ++k)
            {
                const float re = buf[k * 2], im = buf[k * 2 + 1];
                acc[(size_t) k] += std::sqrt ((double) re * re + (double) im * im);
            }
        }

        std::vector<float> mag ((size_t) (n / 2 + 1));
        for (int k = 0; k <= n / 2; ++k)
        {
            const double m = acc[(size_t) k] / ((double) winStarts.size() * n);
            const double dB = 20.0 * std::log10 (std::max (1e-12, m));
            mag[k] = (float) std::clamp ((dB - dBMin) / (-dBMin), 0.0, 1.0);
        }
        outMags.push_back (std::move (mag));
    }
}

void FFTAnalysisView::recomputeIfNeeded()
{
    const int w = getWidth(), h = getHeight();
    if (w <= 0 || !app.audioEngine().isFileLoaded()) return;

    // Region to analyse: the selection when non-empty, otherwise the playhead
    // position.
    double t0, t1;
    if (!app.selection.isEmpty()) { t0 = app.selection.getStart(); t1 = app.selection.getEnd(); }
    else                          { t0 = app.playhead;          t1 = app.playhead; }

    const int n = localN;
    const auto file = app.audioEngine().getFile();

    // NOTE: switching files does NOT clear stored snapshots - they are shared
    // across all open files and each one remembers its own source document.

    if (mags.size() == app.visibleChannels.size() && cachedN == n && cachedT0 == t0
        && cachedT1 == t1 && cachedRate == app.audioEngine().sampleRate()
        && cachedFile == file
        && (axisMode_ != 2 || thirdOctMags.size() == mags.size()))
        return;

    // Throttle only repeated expensive multi-window averaging (e.g. while
    // dragging a wide selection). Single-window updates are cheap enough to
    // run on every click or playhead move, and the first compute of a new
    // region always runs so clicks stay responsive.
    const int64_t now = juce::Time::getMillisecondCounter();
    const double regionLenS = std::max (0.0, (t1 - t0) * app.audioEngine().sampleRate());
    if (!mags.empty() && regionLenS > (double) n && lastComputeWasExpensive
        && now - lastComputeMs < 60)
        return;

    computeRegion (t0, t1, n, mags);

    // 1/3-octave band values for mode 2 (power-averaged per band).
    // Replace the cache on every recompute — never accumulate, or stale
    // curves from earlier clicks would be drawn alongside the live one.
    thirdOctMags.clear();
    if (axisMode_ == 2)
        for (auto& m : mags)
            thirdOctMags.push_back (toThirdOctBands (m, n, app.audioEngine().sampleRate()));

    cachedN = n; cachedT0 = t0; cachedT1 = t1;
    cachedRate = app.audioEngine().sampleRate();
    cachedFile = file;
    lastComputeMs = now;
    lastComputeWasExpensive = (regionLenS > (double) n);
}

void FFTAnalysisView::recomputeSnapshots()
{
    // Snapshots are shared across all open files: each one is recomputed from
    // the engine of the file it was stored from (not necessarily the active
    // one), at its own time segment and the current FFT size / window type.
    for (int i = 0; i < 8; ++i)
    {
        auto& s = snapshots[i];
        if (!(s.n > 0 && s.gen != settingsGen)) continue;

        // If the source file has been closed, drop the slot (closeFile normally
        // clears it already - this is defensive).
        if (!s.source || !app.isDocLoaded (s.source))
        { clearSnapshotSlot (i); continue; }

        computeRegionFrom (s.source->engine, s.chans, s.t0, s.t1, localN, s.mags);
        s.n = localN;
        s.gen = settingsGen;
    }
    refreshSnapshotBands();
}

void FFTAnalysisView::refreshSnapshotBands()
{
    for (int i = 0; i < 8; ++i)
    {
        auto& s = snapshots[i];
        if (!(s.n > 0 && !s.mags.empty())) continue;
        // Band conversion needs the source file's sample rate, which may
        // differ from the active file's.
        const double rate = (s.source != nullptr)
            ? s.source->engine.sampleRate() : app.audioEngine().sampleRate();
        s.bands.clear();
        for (auto& m : s.mags)
            s.bands.push_back (toThirdOctBands (m, s.n, rate));
    }
}

int FFTAnalysisView::axisWidth() const
{
    // Linear V/g labels are wider than plain dB numbers.
    return app.scale().usesLinearLevels() ? 50 : 34;
}

juce::Rectangle<int> FFTAnalysisView::plotBounds() const
{
    const auto b = getLocalBounds();
    const int topH = 52;      // two control rows
    const int axisR = axisWidth(), axisB = 18, readoutH = 16;
    return { 2, topH + 2,
             std::max (1, b.getWidth() - axisR - 4),
             std::max (1, b.getHeight() - topH - axisB - readoutH - 4) };
}

float FFTAnalysisView::xToFreq (int x) const
{
    const auto plot = plotBounds();
    if (plot.getWidth() <= 0 || fHi_ <= fLo_) return (float) fLo_;
    const double t = std::clamp ((double) (x - plot.getX()) / (double) plot.getWidth(), 0.0, 1.0);
    if (axisMode_ == 0)
        return (float) (fLo_ + t * (fHi_ - fLo_));
    const double llo = std::log (std::max (1e-9, fLo_)), lhi = std::log (fHi_);
    return (float) std::exp (llo + t * (lhi - llo));
}

double FFTAnalysisView::freqToX (double f) const
{
    const auto plot = plotBounds();
    if (plot.getWidth() <= 0 || fHi_ <= fLo_) return (float) plot.getX();
    double t;
    if (axisMode_ == 0)
        t = (f - fLo_) / std::max (1e-9, fHi_ - fLo_);
    else
    {
        const double llo = std::log (std::max (1e-9, fLo_)), lhi = std::log (fHi_);
        t = (std::log (std::max (1.0, f)) - llo) / (lhi - llo);
    }
    return plot.getX() + t * plot.getWidth();
}

void FFTAnalysisView::resized()
{
    auto b = getLocalBounds();
    // Row 1: FFT size combo + window combo. Row 2: linear toggle + snapshots.
    auto row1 = b.removeFromTop (26);
    fftSizeCombo.setBounds (30, row1.getY(), 85, 22);
    windowCombo.setBounds (148, row1.getY(), 90, 22);

    auto row2 = b.removeFromTop (26);
    axisCombo.setBounds (4, row2.getY() + 2, 90, 22);
    for (int i = 0; i < 8; ++i)
        snapBtns[i].setBounds (98 + i * 24, row2.getY() + 2, 23, 20);

    repaint();
}

void FFTAnalysisView::paint (juce::Graphics& g)
{
    const auto fullBounds = getLocalBounds();
    g.fillAll (juce::Colours::black);

    // Top control rows background + labels
    const int topH = 52;
    const auto topRow = juce::Rectangle<int> (fullBounds).removeFromTop (topH);
    g.setColour (juce::Colour::fromRGB (45, 45, 48));
    g.fillRect (topRow);
    g.setColour (juce::Colours::white);
    g.setFont (11.0f);
    g.drawText ("FFT", 6, topRow.getY() + 5, 22, 16, juce::Justification::centredLeft, false);
    g.drawText ("Win", 124, topRow.getY() + 5, 22, 16, juce::Justification::centredLeft, false);

    const int axisR = axisWidth();   // right level-axis width
    const int axisB = 18;   // bottom Hz axis height
    const int readoutH = 16; // cursor info strip at the very bottom
    const auto plot = plotBounds();

    if (!app.audioEngine().isFileLoaded())
    {
        g.setColour (juce::Colours::grey);
        g.setFont (12.0f);
        g.drawText ("Load a file to see the frequency analysis", plot, juce::Justification::centred);
        return;
    }

    recomputeIfNeeded();

    const double rate = app.audioEngine().sampleRate();
    // Local display ranges (independent of the spectrogram's settings).
    const double fLo = fLo_, fHi = fHi_;
    const int n = cachedN;
    const bool haveSpectrum = (n > 1) && !mags.empty()
                              && (axisMode_ != 2 || !thirdOctMags.empty());

    // Frequency grid ticks for the current axis mode
    std::vector<double> freqTicks;
    // Linear subdivisions inserted into wide log gaps (freq, hasLabel).
    std::vector<std::pair<double, bool>> subTicks;
    bool thirdOctFallback = false;   // deep zoom between band centres
    if (axisMode_ == 2)
    {
        // One grid line per 1/3-octave band centre.
        for (int b = 0; b < kNumThirdOctBands && kThirdOctCenters[b] <= fHi + 1e-6; ++b)
            if (kThirdOctCenters[b] >= fLo - 1e-6) freqTicks.push_back (kThirdOctCenters[b]);
        // Deep zooms can land between band centres: fall back to even
        // divisions so the axis never disappears.
        if (freqTicks.empty())
        {
            thirdOctFallback = true;
            for (int i = 1; i <= 4; ++i)
                freqTicks.push_back (fLo + std::max (1e-9, fHi - fLo) * (double) i / 5.0);
        }
    }
    else if (axisMode_ == 1)
    {
        // Adaptive log ticks so the axis never disappears when zoomed in:
        // every digit per decade at mid zooms (thinned to 1-2-5 when that
        // would crowd the labels), and a nice linear step inside sub-decade
        // ranges where no whole decades are visible.
        const double spanDec = std::log10 (std::max (1e-9, fHi / fLo));
        if (spanDec < 0.7)
        {
            const double span = std::max (1e-9, fHi - fLo);
            bool done = false;
            for (double e = std::floor (std::log10 (span / 8.0));
                 !done && e <= std::ceil (std::log10 (span / 3.0)) + 1.0; ++e)
                for (double m : { 1.0, 2.0, 5.0 })
                {
                    const double step = m * std::pow (10.0, e);
                    if (span / step >= 3.0 && span / step <= 8.0)
                    {
                        for (double f = std::ceil (fLo / step - 1e-9) * step;
                             f <= fHi + 1e-6; f += step)
                            freqTicks.push_back (f);
                        done = true;
                        break;
                    }
                }
            if (!done)
                for (int i = 1; i <= 4; ++i) freqTicks.push_back (fLo + span * (double) i / 5.0);
        }
        else
        {
            auto collect = [&](const int* digs, int n)
            {
                std::vector<double> t;
                for (double p = std::ceil (std::log10 (fLo)) - 1.0;
                     p <= std::floor (std::log10 (fHi)); ++p)
                    for (int i = 0; i < n; ++i)
                    {
                        const double f = (double) digs[i] * std::pow (10.0, p);
                        if (f >= fLo - 1e-6 && f <= fHi + 1e-6) t.push_back (f);
                    }
                return t;
            };
            static const int allD[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
            static const int fewD[] = { 1, 2, 5 };
            auto ticks = collect (allD, 9);
            if ((int) ticks.size() > 14) ticks = collect (fewD, 3);

            // Gap-fill: where two adjacent structural points are far apart on
            // screen — e.g. the partial decade between 1k and 2k when zoomed
            // to [100 Hz, 2 kHz] — add nice linear subdivisions so sub-decade
            // regions never look empty.
            const double gapThresh = std::max (24.0, plot.getHeight() * 0.18);
            auto fillGap = [&] (double a, double b)
            {
                if (b <= a * 1.0001) return;
                const double gapPx = std::abs ((double) freqToX (b) - (double) freqToX (a));
                if (gapPx < gapThresh) return;
                const double span = b - a;
                bool done = false;
                for (double e = std::floor (std::log10 (span / 8.0));
                     !done && e <= std::ceil (std::log10 (span / 3.0)) + 1.0; ++e)
                    for (double m : { 1.0, 2.0, 5.0 })
                    {
                        const double step = m * std::pow (10.0, e);
                        if (span / step >= 3.0 && span / step <= 8.0)
                        {
                            const int nDiv = (int) std::lround (span / step);
                            const bool label = gapPx / (nDiv + 1) >= 16.0;
                            for (double f = std::ceil (a / step - 1e-9) * step;
                                 f < b - step * 1e-3; f += step)
                                if (f > a + step * 1e-3) subTicks.emplace_back (f, label);
                            done = true;
                            break;
                        }
                    }
            };
            double prev = fLo;
            for (double f : ticks) { fillGap (prev, f); prev = f; }
            fillGap (prev, fHi);

            freqTicks.insert (freqTicks.end(), ticks.begin(), ticks.end());
        }
    }
    else
    {
        // Linear mode: a nice step giving 3..9 ticks, starting at the first
        // grid point at or above fLo so every tick lands inside the plot.
        const double span = std::max (1e-9, fHi - fLo);
        bool done = false;
        for (double e = std::floor (std::log10 (span / 9.0));
             !done && e <= std::ceil (std::log10 (span / 3.0)) + 1.0; ++e)
            for (double m : { 1.0, 2.0, 5.0 })
            {
                const double step = m * std::pow (10.0, e);
                if (span / step >= 3.0 && span / step <= 9.0)
                {
                    for (double f = std::ceil (fLo / step - 1e-9) * step;
                         f <= fHi + 1e-6; f += step)
                        freqTicks.push_back (f);
                    done = true;
                    break;
                }
            }
        if (!done)
            for (int i = 1; i <= 4; ++i) freqTicks.push_back (fLo + span * (double) i / 5.0);
    }

    // Grid: frequency ticks + level lines. The level grid step adapts to the
    // visible window so zooming in/out always shows a few readable lines.
    const double dbSpan = std::max (1e-9, dbTop_ - dbBottom_);
    int dbStep = 12;
    for (int cand : { 2, 4, 6, 10, 12, 20, 30 })
        if (dbSpan / cand >= 3.0 && dbSpan / cand <= 9.0) { dbStep = cand; break; }

    g.setColour (juce::Colour::fromRGB (40, 40, 46));
    for (double f : freqTicks)
        g.drawVerticalLine ((float) freqToX (f), plot.getY(), plot.getBottom());
    for (const auto& st : subTicks)
        g.drawVerticalLine ((float) freqToX (st.first), plot.getY(), plot.getBottom());
    for (double L = std::floor (dbTop_ / dbStep) * dbStep; L >= dbBottom_ - 1e-9; L -= dbStep)
        g.drawHorizontalLine ((int) levelToY (L), plot.getX(), plot.getRight());

    // Curves
    if (!haveSpectrum)
    {
        g.setColour (juce::Colours::grey);
        g.setFont (11.0f);
        g.drawText ("No audio data", plot, juce::Justification::centred);
    }
    else
    {
        // Clip the curves to the plot area: with a zoomed level window parts of
        // the curve can sit above/below the visible range. (This JUCE build has
        // no add/removeClipRegion, so save/restore the state around it.)
        g.saveState();
        g.reduceClipRegion (plot);

        // Draw in frequency space: bin k corresponds to f = k * rate / n.
        // (The magnitude vector only has n/2+1 entries — never index it by x!)
        // Each curve is mapped with the FFT size it was computed at, so stored
        // snapshots do not shift when the live analysis uses a different size.
        auto drawCurve = [&](const std::vector<float>& mag, int curveN, const juce::Colour& col,
                             float w)
        {
            g.setColour (col);
            juce::Path path;
            bool started = false;
            for (int k = 1; k <= curveN / 2 && k < (int) mag.size(); ++k)
            {
                const double f = (double) k * rate / (double) curveN;
                if (f < fLo || f > fHi) continue;
                const float x = (float) freqToX (f);
                // Stored magnitudes are normalized against the fixed floor.
                const double dB = -kNormFloorDb * (1.0 - (double) mag[k]);
                const float y = levelToY (dB);
                if (!started) { path.startNewSubPath (x, y); started = true; }
                else          path.lineTo (x, y);
            }
            g.strokePath (path, juce::PathStrokeType (w));
        };

        // In 1/3-octave mode each band is drawn as a flat step spanning its
        // frequency edges with vertical connectors at the boundaries — the
        // classic 1/3-octave band-plot look, not a smooth curve.
        auto drawBandCurve = [&](const std::vector<float>& bands, const juce::Colour& col,
                                 float w)
        {
            if (bands.empty()) return;
            g.setColour (col);
            juce::Path path;
            bool started = false;
            for (int b = 0; b < kNumThirdOctBands && b < (int) bands.size(); ++b)
            {
                const double fc = kThirdOctCenters[b];
                if (fc > fHi + 1e-6) break;
                const double lo = std::max (fLo, fc * std::pow (2.0, -1.0 / 6.0));
                const double hi = std::min (fHi, fc * std::pow (2.0,  1.0 / 6.0));
                if (hi <= lo) continue;
                const float xLo = (float) freqToX (lo);
                const float xHi = (float) freqToX (hi);
                const double dBb = -kNormFloorDb * (1.0 - (double) bands[(size_t) b]);
                const float y   = levelToY (dBb);
                if (!started) { path.startNewSubPath (xLo, y); started = true; }
                else          path.lineTo (xLo, y);    // vertical connector at the shared boundary
                path.lineTo (xHi, y);                  // flat top of this band
            }
            g.strokePath (path, juce::PathStrokeType (w));
        };

        // Stored snapshot overlays first: full slot colour, just a little
        // thinner than the live curves. Live curves go on top.
        for (int i = 0; i < 8; ++i)
            if (snapshots[i].shown && snapshots[i].n > 1 && !snapshots[i].mags.empty())
                for (size_t c = 0; c < snapshots[i].mags.size() && c < 4; ++c)
                    if (axisMode_ == 2)
                    {
                        // Braces are required here: without them the else below
                        // would bind to the inner if and snapshot curves would
                        // never be drawn in linear/log axis modes.
                        if (!snapshots[i].bands.empty())
                            drawBandCurve (snapshots[i].bands[c], snapColours[i], 1.0f);
                    }
                    else
                        drawCurve (snapshots[i].mags[c], snapshots[i].n, snapColours[i], 1.0f);

        if (axisMode_ == 2)
            for (size_t c = 0; c < mags.size() && c < 4; ++c)
                drawBandCurve (thirdOctMags[c], fftColours[c], 1.2f);
        else
            for (size_t c = 0; c < mags.size() && c < 4; ++c)
                drawCurve (mags[c], n, fftColours[c], 1.2f);

        g.restoreState();
    }

    // Axis labels
    g.setColour (juce::Colours::lightgrey);
    g.setFont (9.0f);
    for (double f : freqTicks)
    {
        if (axisMode_ == 2 && !thirdOctFallback && !isThirdOctLabelFreq (f))
            continue;   // avoid crowding
        const float x = (float) freqToX (f);
        juce::String label;
        if (f >= 1000.0)
            label = juce::String (f / 1000.0, std::abs (f - std::round (f)) < 1e-6 ? 0 : 1) + "k";
        else
            label = juce::String ((int) std::lround (f));
        g.drawText (label, (int) x - 12, plot.getBottom() + 2, 26, axisB - 4, juce::Justification::centred, false);
    }
    for (const auto& st : subTicks)
    {
        if (!st.second) continue;
        const float x = (float) freqToX (st.first);
        juce::String label;
        if (st.first >= 1000.0)
            label = juce::String (st.first / 1000.0, std::abs (st.first - std::round (st.first)) < 1e-6 ? 0 : 1) + "k";
        else
            label = juce::String ((int) std::lround (st.first));
        g.drawText (label, (int) x - 12, plot.getBottom() + 2, 26, axisB - 4, juce::Justification::centred, false);
    }
    // With a scale calibration the grid lines show absolute levels
    // (dB-SPL for Pascal files; linear V/g values for volts/grams files)
    // instead of relative dB.
    const ScaleCalibration& sc = app.scale();
    for (double L = std::floor (dbTop_ / dbStep) * dbStep; L >= dbBottom_ - 1e-9; L -= dbStep)
        g.drawText (ScaleCalibration::formatSpectralLevel (L, sc), plot.getRight() + 4,
                    (int) levelToY (L) - 7, axisR - 6, 14, juce::Justification::centredLeft, false);

    // Harmonic reference lines: while active, the frequency under the cursor
    // is harmonic #N of a fundamental and harmonics 1..N get delicate
    // vertical markers wherever they are inside the visible window.
    if (harmonicRefActive_ && haveSpectrum && harmonicAnchorF_ > 0.0)
    {
        const double fFund = harmonicAnchorF_ / std::max (1, harmonicN_);
        for (int k = 1; k <= harmonicN_; ++k)
        {
            const double f = (double) k * fFund;
            if (f < fLo || f > fHi) continue;
            const float x = (float) freqToX (f);
            g.setColour (juce::Colour::fromRGBA (140, 230, 255, 160));
            g.drawVerticalLine (x, plot.getY(), plot.getBottom());
            g.setFont (9.0f);
            g.drawText (juce::String (k), x - 8, plot.getY() + 1, 16, 12,
                        juce::Justification::centred, false);
        }
    }

    // Cursor readout (crosshair + optional snap marker)
    bool snapped = false;
    float snapDotX = 0.0f, snapDotY = 0.0f;
    const juce::String info = buildCursorInfo (cursorX, cursorY, snapped, snapDotX, snapDotY);
    if (!info.isEmpty())
    {
        g.setColour (juce::Colours::white);
        g.setFont (10.0f);
        g.drawText (info, juce::Rectangle<int> (4, fullBounds.getHeight() - readoutH,
                                                fullBounds.getWidth() - 8, readoutH),
                    juce::Justification::left);

        // Cursor crosshair + snap marker
        g.setColour (juce::Colour::fromRGBA (255, 255, 255, 90));
        g.drawVerticalLine ((float) cursorX, plot.getY(), plot.getBottom());
        if (snapped)
        {
            g.setColour (juce::Colours::white);
            g.fillEllipse (snapDotX - 3.5f, snapDotY - 3.5f, 7.0f, 7.0f);
        }
    }

    // Rubber band for a right-drag box zoom (drawn last so nothing covers it).
    // A drag that starts on an axis stretches into a full-height/width band
    // across that axis strip - the same translucent blue as the waveform and
    // spectrogram views; a drag inside the plot keeps the free yellow box.
    if (dragMode == DragMode::ZoomDrag && haveZoomRect)
    {
        auto zr = zoomRect;
        const bool startOnFreqAxis  = dragStartY >= plot.getBottom();
        const bool startOnLevelAxis = !startOnFreqAxis && dragStartX > plot.getRight();

        if (startOnFreqAxis || startOnLevelAxis)
        {
            if (startOnFreqAxis)
                zr = juce::Rectangle<int> (zr.getX(), plot.getBottom(), zr.getWidth(), axisB);
            else
                zr = juce::Rectangle<int> (plot.getRight(), zr.getY(), axisWidth(), zr.getHeight());

            g.setColour (juce::Colour::fromRGBA (70, 130, 255, 80));
            g.fillRect (zr);
            g.setColour (juce::Colour::fromRGB (96, 150, 255).withAlpha (0.9f));
            g.drawRect (zr, 1);
        }
        else
        {
            g.setColour (juce::Colour::fromRGBA (255, 255, 0, 48));
            g.fillRoundedRectangle (zoomRect.toFloat(), 3.0f);
            g.setColour (juce::Colour::fromRGBA (255, 255, 0, 160));
            g.drawRoundedRectangle (zoomRect.toFloat(), 3.0f, 1.0f);
        }
    }
}

// Build the cursor readout text for position (x, y). When one of the curves
// passes within a few pixels of the cursor, snapped is set and snapDotX/Y
// hold that point so paint() can draw the marker. Returns an empty string
// when there is nothing to show.
juce::String FFTAnalysisView::buildCursorInfo (int x, int y, bool& snapped,
                                               float& snapDotX, float& snapDotY) const
{
    snapped = false;
    snapDotX = 0.0f;
    snapDotY = 0.0f;

    const auto plot = plotBounds();
    const int n = cachedN;
    const bool haveSpectrum = (n > 1) && !mags.empty()
                              && (axisMode_ != 2 || !thirdOctMags.empty());
    if (!haveSpectrum || x < plot.getX() || x > plot.getRight()
        || y < plot.getY() || y > plot.getBottom())
        return {};

    const double rate = app.audioEngine().sampleRate();
    const ScaleCalibration& sc = app.scale();
    const double f = xToFreq (x);
    juce::String info;
    int snapCh = -1;          // channel index of a snapped curve (-1 = none)

    if (axisMode_ == 2 && !thirdOctMags.empty())
    {
        // Report the band containing the cursor frequency (or nearest centre).
        int b = -1;
        double bestD = 1e30;
        for (int i = 0; i < kNumThirdOctBands && i < (int) thirdOctMags[0].size(); ++i)
        {
            const double fc = kThirdOctCenters[i];
            const double lo = fc * std::pow (2.0, -1.0 / 6.0);
            const double hi = fc * std::pow (2.0,  1.0 / 6.0);
            if (f >= lo && f <= hi) { b = i; break; }
            const double d = std::abs (f - fc);
            if (d < bestD) { bestD = d; b = i; }
        }
        if (b >= 0)
        {
            info = "Cursor:  ~" + juce::String ((int) std::lround (kThirdOctCenters[b])) + " Hz band";
            for (size_t c = 0; c < thirdOctMags.size() && c < 4; ++c)
                if (b < (int) thirdOctMags[c].size())
                    info += ",  Ch" + juce::String ((int) c + 1) + "="
                          + ScaleCalibration::formatSpectralLevel (-kNormFloorDb + thirdOctMags[c][(size_t) b] * kNormFloorDb, sc);
        }
    }
    else
    {
        // Bin index for a frequency: bin = f * n / rate (bin 0 is DC).
        auto binAt = [&](double freq) -> int
        {
            const int maxBin = std::min ((int) (n / 2), (int) mags[0].size() - 1);
            return (int) std::clamp ((double) std::lround (freq * n / rate),
                                     0.0, (double) maxBin);
        };

        // Snap: if one of the curves passes within a few pixels of the
        // cursor, report that channel's exact bin value first.
        double bestDy = 8.0;
        for (size_t c = 0; c < mags.size() && c < 4; ++c)
        {
            if (mags[c].empty()) continue;
            const int maxBin = std::min ((int) (n / 2), (int) mags[c].size() - 1);
            const int bin = (int) std::clamp ((double) std::lround (f * n / rate),
                                             0.0, (double) maxBin);
            const double dBc = -kNormFloorDb * (1.0 - (double) mags[c][bin]);
            const float yc = levelToY (dBc);
            if (std::abs ((double) yc - y) < bestDy)
            {
                bestDy = std::abs ((double) yc - y);
                snapCh = (int) c;
                snapDotX = (float) freqToX ((double) bin * rate / n);
                snapDotY = yc;
            }
        }

        if (snapCh >= 0)
        {
            snapped = true;
            const int bin = binAt (f);
            const double fExact = (double) bin * rate / n;   // exact bin centre
            info = "Cursor:  " + juce::String (fExact >= 1000.0 ? fExact / 1000.0 : fExact, 2)
                 + (fExact >= 1000.0 ? " kHz" : " Hz")
                 + "   [snap Ch" + juce::String (snapCh + 1) + "]"
                 + ScaleCalibration::formatSpectralLevel (-kNormFloorDb * (1.0 - (double) mags[snapCh][bin]), sc);
        }
        else
        {
            info = "Cursor:  " + juce::String ((int) std::lround (f)) + " Hz";
            for (size_t c = 0; c < mags.size() && c < 4; ++c)
            {
                if (mags[c].empty()) continue;
                const int bin = binAt (f);
                info += ",  Ch" + juce::String ((int) c + 1) + "="
                      + ScaleCalibration::formatSpectralLevel (-kNormFloorDb * (1.0 - (double) mags[c][bin]), sc);
            }
        }
    }

    return info;
}

void FFTAnalysisView::mouseMove (const juce::MouseEvent& e)
{
    cursorX = e.x; cursorY = e.y;
    // Harmonic reference lines follow the mouse: re-anchor to the frequency
    // under the cursor while it is horizontally inside the plot.
    if (harmonicRefActive_)
    {
        const auto plot = plotBounds();
        if (e.x >= plot.getX() && e.x <= plot.getRight())
            harmonicAnchorF_ = xToFreq (e.x);
    }
    repaint();
}

void FFTAnalysisView::mouseExit (const juce::MouseEvent&)
{
    cursorX = -1; cursorY = -1;
    repaint();
}

void FFTAnalysisView::setFollowPlayhead (bool f)
{
    if (f != followPlayhead_) { followPlayhead_ = f; cachedT0 = -1.0; } // force recompute
}

// Snapshot buttons: click stores the current curve in the slot and toggles its
// overlay visibility; Ctrl/Shift+click clears the slot entirely.
void FFTAnalysisView::handleSnapshotClick (int i, juce::ModifierKeys mods)
{
    auto& s = snapshots[i];
    const bool stored = (s.n > 0);

    if (mods.isShiftDown())
    {
        // Shift+click clears the slot.
        clearSnapshotSlot (i);
        repaint();
        return;
    }

    if (!stored || mods.isCommandDown())
    {
        // Empty slot: store it. Stored slot + Ctrl+click: re-store the time
        // segment currently being analysed. The live cache is exactly this
        // region at the current settings, so a copy suffices; later changes
        // of FFT size / window type recompute it from t0..t1 (from its source
        // file's engine - snapshots are shared across all open files).
        if (!(cachedN > 1) || mags.empty())
        {
            snapBtns[i].setToggleState (false, juce::dontSendNotification); // nothing to store yet
            return;
        }

        s.t0 = cachedT0;
        s.t1 = cachedT1;
        s.n = cachedN;
        s.gen = settingsGen;
        s.mags = mags;
        s.source = app.doc();           // remember which file this came from
        s.chans = app.visibleChannels;  // channels visible at store time
        refreshSnapshotBands();
        s.shown = true;   // newly captured data is shown
    }
    else
    {
        // Plain click on a stored slot: only toggle the overlay, never update.
        s.shown = !s.shown;
    }

    snapBtns[i].setToggleState (s.shown, juce::dontSendNotification);
    snapBtns[i].setStored (true);
    updateSnapshotTooltip (i);
    repaint();
}

void FFTAnalysisView::updateSnapshotTooltip (int i)
{
    auto& s = snapshots[i];
    const juce::String num = juce::String (i + 1);
    if (s.n > 0)
    {
        const juce::String from = (s.source != nullptr) ? (" from " + s.source->name)
                                                         : juce::String();
        snapBtns[i].setTooltip ("Snapshot " + num + ": stores " + formatTimeCode (s.t0)
                                + " - " + formatTimeCode (s.t1) + from
                                + ". Click shows/hides the stored curve. Ctrl+Click re-stores it from the current selection/cursor. Shift+Click clears it. Right-click: convert to marker / clear.");
    }
    else
        snapBtns[i].setTooltip ("Snapshot " + num + " is empty. Click stores the currently analysed time segment here and shows its curve. Snapshots are shared across all open files.");
}

void FFTAnalysisView::clearSnapshotSlot (int i)
{
    auto& s = snapshots[i];
    s = Snapshot{};
    snapBtns[i].setToggleState (false, juce::dontSendNotification);
    snapBtns[i].setStored (false);
    updateSnapshotTooltip (i);
}

void FFTAnalysisView::recomputeSnapshotSlot (int i)
{
    auto& s = snapshots[i];
    if (!(s.n > 0 && s.source != nullptr && app.isDocLoaded (s.source)))
    { clearSnapshotSlot (i); return; }

    computeRegionFrom (s.source->engine, s.chans, s.t0, s.t1, localN, s.mags);
    s.n = localN;
    s.gen = settingsGen;
    refreshSnapshotBands();
    snapBtns[i].setToggleState (s.shown, juce::dontSendNotification);
    snapBtns[i].setStored (true);
    updateSnapshotTooltip (i);
}

void FFTAnalysisView::recordAndAdjustSnapshotsForDeletion (FileDoc* d, double a, double b, Patch& p)
{
    if (d == nullptr || !(b > a))
        return;
    const double len = b - a;

    for (int i = 0; i < 8; ++i)
    {
        auto& s = snapshots[i];
        if (s.source != d)   // slots are shared across files: only adjust this one's
            continue;

        SnapshotSlotChange ch;
        ch.before.t0 = s.t0;  ch.before.t1 = s.t1;
        ch.before.n = s.n;    ch.before.shown = s.shown;
        ch.before.chans = s.chans;

        bool touched = false;
        if (s.t1 <= a)
            ;                                   // entirely before: unchanged
        else if (s.t0 >= b)
        { s.t0 -= len; s.t1 -= len; touched = true; }   // entirely after: shift left
        else if (s.t0 < a && s.t1 > b)
        { s.t1 -= len; touched = true; }       // straddles the whole deletion:
                                                // both halves stay contiguous -> [t0, t1-len]
        else if (s.t0 < a)
        { s.t1 = a; touched = true; }          // right part deleted -> [t0, a]
        else if (s.t1 <= b)
        { clearSnapshotSlot (i); touched = true; }   // fully contained: cleared
        else
        { s.t0 = a; s.t1 -= len; touched = true; }   // left part deleted:
                                                // old [b, t1] -> new [a, t1-len]

        if (!touched)
            continue;

        auto& s2 = snapshots[i];               // may have been reset by clearSnapshotSlot
        ch.changed = true;
        ch.after.n = s2.n;  ch.after.t0 = s2.t0;  ch.after.t1 = s2.t1;
        ch.after.shown = s2.shown;  ch.after.chans = s2.chans;
        p.snapshotSlots[(size_t) i] = ch;
    }
}

void FFTAnalysisView::restoreSnapshotSlots (FileDoc* d, const Patch& p, bool isRedo)
{
    if (d == nullptr)
        return;

    for (int i = 0; i < 8; ++i)
    {
        const auto& ch = p.snapshotSlots[(size_t) i];
        if (!ch.changed)
            continue;
        const SnapshotMeta& m = isRedo ? ch.after : ch.before;

        // Never clobber a slot that now holds another file's snapshot.
        auto& s = snapshots[i];
        if (s.source != nullptr && s.source != d)
            continue;

        if (m.n <= 0 || !app.isDocLoaded (d))
        { clearSnapshotSlot (i); continue; }

        s.t0 = m.t0;  s.t1 = m.t1;
        s.chans = m.chans;  s.shown = m.shown;
        s.source = d;
        s.n = m.n;   // mark valid so recomputeSnapshotSlot rebuilds the curve
        recomputeSnapshotSlot (i);
    }
}

void FFTAnalysisView::clearSnapshotsFor (const FileDoc* d)
{
    for (int i = 0; i < 8; ++i)
        if (snapshots[i].source == d)
            clearSnapshotSlot (i);
}

bool FFTAnalysisView::convertSnapshotToMarker (int i)
{
    auto& s = snapshots[i];
    if (!(s.n > 1 && !s.mags.empty()) || s.source == nullptr) return false;

    const double dur = s.source->engine.duration();
    const double t0 = std::clamp (s.t0, 0.0, dur);
    const double t1 = std::clamp (s.t1, t0, dur);
    if (t1 <= t0) return false;

    // The marker is created in the snapshot's SOURCE file (which may not be
    // the active one); it appears in that file's marker list when it becomes
    // active.
    s.source->markers.addRegionMarker (t0, t1, "Snapshot " + juce::String (i + 1));

    if (app.doc() == s.source && app.onStateChange)
        app.onStateChange ();   // refresh the marker list when that file is active
    return true;
}

void FFTAnalysisView::showSnapshotMenu (int i)
{
    auto& s = snapshots[i];
    juce::PopupMenu m;
    const bool hasData = (s.n > 1 && !s.mags.empty());
    m.addItem (1, "Convert to marker", hasData, false);
    m.addItem (2, "Clear", s.n > 0 || s.shown, false);

    const int r = m.showMenu (juce::PopupMenu::Options().withMousePosition());
    if (r == 1) convertSnapshotToMarker (i);
    else if (r == 2)
    { clearSnapshotSlot (i); repaint(); }
}

// Right-click context menu. When the click landed on an axis, "Zoom Out
// Full" resets only that range instead of both.
void FFTAnalysisView::showContextMenu (bool onLevelAxis, bool onFreqAxis, int clickX)
{
    const bool haveSpec = app.audioEngine().isFileLoaded()
                          && cachedN > 1 && !mags.empty();

    juce::PopupMenu m;
    m.addItem (CtxZoomOutFull, "Zoom Out Full");
    m.addSeparator();
    m.addItem (100, "Copy Spectrum to Clipboard");
    m.addSeparator();
    m.addItem (CtxSetHarmonicRefs, "Set Harmonic References", haveSpec);
    m.addItem (CtxCopyHarmonics, "Copy Harmonics",
               harmonicRefActive_ && haveSpec);
    m.addItem (CtxResetHarmonic, "Reset Harmonic", harmonicRefActive_);
    m.addSeparator();
    m.addItem (101, "Close Panel");

    const int sel = m.showMenu (juce::PopupMenu::Options ().withMousePosition());
    switch (sel)
    {
        case CtxZoomOutFull:
            if (onLevelAxis && !onFreqAxis) { dbTop_ = 0.0; dbBottom_ = -80.0; repaint(); }
            else if (onFreqAxis && !onLevelAxis)
            {
                const double rate = app.audioEngine().sampleRate();
                fftAxisLimits (axisMode_, rate, fLo_, fHi_);
                repaint();
            }
            else resetDisplayRanges();   // plot or corner: reset both windows
            break;
        case 100:
            copySpectrumToClipboard();
            break;
        case CtxSetHarmonicRefs:
            showSetHarmonicDialog (xToFreq (clickX));
            break;
        case CtxCopyHarmonics:
            copyHarmonicsToClipboard();
            break;
        case CtxResetHarmonic:
            resetHarmonicReference();
            break;
        case 101:
            if (onCloseRequested) onCloseRequested();
            break;
    }
}

void FFTAnalysisView::setHarmonicReference (int n, double anchorHz)
{
    if (n < 1 || !(anchorHz > 0.0) || !std::isfinite (anchorHz)) return;
    harmonicRefActive_ = true;
    harmonicN_ = n;
    harmonicAnchorF_ = anchorHz;
    repaint();
}

void FFTAnalysisView::resetHarmonicReference()
{
    if (!harmonicRefActive_) return;
    harmonicRefActive_ = false;
    harmonicN_ = 1;
    harmonicAnchorF_ = 0.0;
    repaint();
}

juce::String FFTAnalysisView::buildHarmonicsTable() const
{
    if (!harmonicRefActive_ || harmonicN_ < 1 || cachedN <= 1 || mags.empty())
        return {};

    const double rate = app.audioEngine().sampleRate();
    const double fFund = harmonicAnchorF_ / (double) harmonicN_;
    if (!(fFund > 0.0) || !std::isfinite (fFund))
        return {};

    // Same level formatting as "Copy Spectrum to Clipboard": absolute linear
    // values for volts/grams files, otherwise dB relative to full scale.
    const ScaleCalibration& sc = app.scale();
    const double off = sc.levelOffsetDb();
    auto fmtLevel = [&] (double relDb) -> juce::String
    {
        if (sc.usesLinearLevels())
            return ScaleCalibration::formatLevelValue (sc.fullScaleValue * std::pow (10.0, relDb / 20.0), 6);
        return juce::String (relDb + off, 2);
    };

    // Amplitude comes from the first visible channel at the nearest FFT bin.
    const auto& mag = mags.front();
    const int maxBin = std::min ((int) (cachedN / 2), (int) mag.size() - 1);

    juce::StringArray lines;
    lines.add ("Harmonic\tFrequency\tAmplitude");
    for (int k = 1; k <= harmonicN_; ++k)
    {
        const double f = (double) k * fFund;
        juce::String line = juce::String (k) + "\t" + juce::String (f, 3);
        if (maxBin < 1 || f > rate * 0.5)
            line += "\t";   // no FFT data above Nyquist: leave the cell empty
        else
        {
            const int bin = (int) std::clamp ((double) std::lround (f * cachedN / rate),
                                              0.0, (double) maxBin);
            const double dB = -kNormFloorDb * (1.0 - (double) mag[(size_t) bin]);
            line += "\t" + fmtLevel (dB);
        }
        lines.add (line);
    }
    return lines.joinIntoString ("\r\n");
}

void FFTAnalysisView::copyHarmonicsToClipboard()
{
    const juce::String table = buildHarmonicsTable();
    if (!table.isEmpty())
        juce::SystemClipboard::copyTextToClipboard (table);
}

void FFTAnalysisView::showSetHarmonicDialog (double clickFreqHz)
{
    auto* w = new juce::AlertWindow ("Set Harmonic References",
                                     "The frequency under the cursor is\n"
                                     + juce::String (clickFreqHz, 1) + " Hz.\n\n"
                                     "Which harmonic number is it? (1 = fundamental)\n"
                                     "Harmonics 1..N will be marked with vertical lines that\n"
                                     "follow the mouse while you move it.",
                                     juce::AlertWindow::QuestionIcon, this);
    w->addTextEditor ("harmonic", "1");
    if (auto* editor = w->getTextEditor ("harmonic"))
        // Let Enter/Escape reach the OK/Cancel shortcuts even while a
        // field has keyboard focus.
        editor->setEscapeAndReturnKeysConsumed (false);

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    // AlertWindow disables keyboard focus on its editors; re-enable it and
    // pre-select the default so a plain Enter accepts harmonic #1.
    if (auto* editor = w->getTextEditor ("harmonic"))
    {
        editor->setWantsKeyboardFocus (true);
        editor->selectAll();
    }

    // Enter modal state WITHOUT taking keyboard focus (see showNewFileDialog).
    w->enterModalState (false);
    if (auto* editor = w->getTextEditor ("harmonic"))
        editor->grabKeyboardFocus();

    const int result = w->runModalLoop();
    const juce::String text = w->getTextEditorContents ("harmonic")
                                 .trim ().removeCharacters ("., ");
    delete w;
    if (result != 1) return;

    const int n = text.isEmpty () ? 0 : text.getIntValue ();
    if (n < 1 || n > 1000)
    {
        juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Set Harmonic References",
                                            "Please enter an integer between 1 and 1000.",
                                            "", this);
        return;
    }

    setHarmonicReference (n, clickFreqHz);
}

void FFTAnalysisView::mouseDown (const juce::MouseEvent& e)
{
    const auto plot = plotBounds();

    if (e.mods.isRightButtonDown())
    {
        dragMode = DragMode::ZoomDrag;
        haveZoomRect = false;
        zoomRect = {};
        dragStartX = e.x;
        dragStartY = e.y;
        return;
    }

    if (!e.mods.isLeftButtonDown() || e.mods.isRightButtonDown()) return;

    // Left-drag on the bottom Hz axis pans the frequency range; left-drag on
    // the right level axis pans the level window.
    if (e.y > plot.getBottom() && e.x >= plot.getX() && e.x <= plot.getRight())
        dragMode = DragMode::PanFreq;
    else if (e.x > plot.getRight() && e.y >= plot.getY() && e.y <= plot.getBottom())
        dragMode = DragMode::PanLevel;
    else
        return;   // plain clicks in the plot do nothing here

    dragStartX = e.x;
    dragStartY = e.y;
}

void FFTAnalysisView::mouseDrag (const juce::MouseEvent& e)
{
    switch (dragMode)
    {
        case DragMode::PanFreq:
        {
            const double f0 = xToFreq (dragStartX);
            const double f1 = xToFreq (e.x);
            if (axisMode_ == 1 && f0 > 0.0 && f1 > 0.0)
            {
                // Log axis: pan in log space.
                const double r = f0 / f1;
                fLo_ *= r; fHi_ *= r;
            }
            else
            {
                const double d = f0 - f1;
                fLo_ += d; fHi_ += d;
            }
            // Keep the window inside [0, Nyquist].
            const double nyq = app.audioEngine().sampleRate() * 0.5;
            if (fLo_ < 0.0) { fHi_ -= fLo_; fLo_ = 0.0; }
            if (fHi_ > nyq) { fLo_ -= (fHi_ - nyq); fHi_ = nyq; }
            repaint();
            break;
        }
        case DragMode::PanLevel:
        {
            const double db0 = yToLevel ((float) dragStartY);
            const double db1 = yToLevel ((float) e.y);
            const double d = db0 - db1;
            dbTop_ += d; dbBottom_ += d;
            // Keep the window inside absolute limits.
            if (dbTop_ > 40.0) { dbBottom_ -= (dbTop_ - 40.0); dbTop_ = 40.0; }
            if (dbBottom_ < -240.0) { dbTop_ += (-240.0 - dbBottom_); dbBottom_ = -240.0; }
            repaint();
            break;
        }
        case DragMode::ZoomDrag:
        {
            const int x1 = std::clamp (e.x, 0, getWidth());
            const int y1 = std::clamp (e.y, 0, getHeight());
            if (!haveZoomRect && (std::abs (x1 - dragStartX) >= 4 || std::abs (y1 - dragStartY) >= 4))
                haveZoomRect = true;
            if (haveZoomRect)
            {
                zoomRect = juce::Rectangle<int> (std::min (dragStartX, x1), std::min (dragStartY, y1),
                                                 std::abs (x1 - dragStartX) + 1,
                                                 std::abs (y1 - dragStartY) + 1);
                repaint();
            }
            break;
        }
        default:
            break;
    }
}

void FFTAnalysisView::mouseUp (const juce::MouseEvent& e)
{
    if (dragMode == DragMode::ZoomDrag)
    {
        const auto plot = plotBounds();
        if (!haveZoomRect)
            showContextMenu (e.x > plot.getRight(), e.y > plot.getBottom(), e.x);
        else
        {
            // The drag START position selects which axes get zoomed (same as
            // the waveform/spectrogram views): inside the plot both, on an
            // axis only that axis.
            const bool startOnFreqAxis  = dragStartY >= plot.getBottom();
            const bool startOnLevelAxis = !startOnFreqAxis && dragStartX > plot.getRight();

            // Horizontal part of the box zooms the frequency range...
            if (!startOnLevelAxis && zoomRect.getWidth() >= 2 && axisMode_ != 2)
            {
                const double fA = xToFreq (std::clamp (zoomRect.getX(), plot.getX(), plot.getRight()));
                const double fB = xToFreq (std::clamp (zoomRect.getRight() - 1, plot.getX(), plot.getRight()));
                if (fB > fA * 1.0001)
                {
                    const double nyq = app.audioEngine().sampleRate() * 0.5;
                    if (axisMode_ == 1)
                    {
                        fLo_ = std::clamp (std::exp (std::log (fA)), 1e-9, nyq);
                        fHi_ = std::clamp (std::exp (std::log (fB)), 1e-9, nyq);
                    }
                    else
                    {
                        fLo_ = std::clamp (fA, 0.0, nyq);
                        fHi_ = std::clamp (fB, 0.0, nyq);
                    }
                }
            }
            // ...and the vertical part zooms the level window.
            if (!startOnFreqAxis && zoomRect.getHeight() >= 2)
            {
                const double dbA = yToLevel ((float) std::clamp (zoomRect.getY(), plot.getY(), plot.getBottom()));
                const double dbB = yToLevel ((float) std::clamp (zoomRect.getBottom() - 1, plot.getY(), plot.getBottom()));
                if (dbA > dbB)
                {
                    const double span = std::clamp (dbA - dbB, 10.0, 320.0);
                    dbTop_ = std::clamp (dbA, -240.0 + span * 0.5, 40.0);
                    dbBottom_ = dbTop_ - span;
                }
            }
        }
    }

    dragMode = DragMode::None;
    haveZoomRect = false;
    zoomRect = {};
}

void FFTAnalysisView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto plot = plotBounds();
    if (!plot.contains (e.x, e.y) && e.x <= plot.getRight() && e.y <= plot.getBottom())
        return;   // ignore the control strip

    const double factor = std::pow (1.25, -wheel.deltaY);
    if (e.mods.isShiftDown() || e.x > plot.getRight())
        zoomLevelAtY ((float) e.y, factor);
    else
        zoomFreqAtX (e.x, factor);
}

void FFTAnalysisView::copySpectrumToClipboard()
{
    const double rate = app.audioEngine().sampleRate();
    // Export the currently visible frequency window.
    const double fLo = fLo_, fHi = fHi_;

    // With a scale calibration the exported values are absolute levels;
    // volts/grams files export linear physical values instead (unit in header).
    const ScaleCalibration& sc = app.scale();
    const double off = sc.levelOffsetDb();
    auto fmtLevel = [&] (double relDb) -> juce::String
    {
        if (sc.usesLinearLevels())
            return ScaleCalibration::formatLevelValue (sc.fullScaleValue * std::pow (10.0, relDb / 20.0), 6);
        return juce::String (relDb + off, 2);
    };

    juce::StringArray lines;
    juce::String header = "freq_Hz";
    for (size_t c = 0; c < mags.size() && c < 4; ++c)
        header += ",ch" + juce::String ((int) c + 1) + "_" + sc.spectralUnitLabel();
    lines.add (header);

    if (axisMode_ == 2 && !thirdOctMags.empty())
    {
        // One line per 1/3-octave band centre.
        for (size_t b = 0; b < thirdOctMags[0].size(); ++b)
        {
            juce::String line = juce::String (kThirdOctCenters[b], 1);
            for (size_t c = 0; c < thirdOctMags.size() && c < 4; ++c)
                if (b < thirdOctMags[c].size())
                    // Magnitudes are normalised against the fixed floor, so
                    // the level is -kNormFloorDb * (1 - mag), exactly as painted.
                    line += "," + fmtLevel (-kNormFloorDb * (1.0 - (double) thirdOctMags[c][b]));
            lines.add (line);
        }
        juce::SystemClipboard::copyTextToClipboard (lines.joinIntoString ("\n"));
        return;
    }

    for (int k = 1; k <= cachedN / 2; ++k)
    {
        const double f = (double) k * rate / (double) cachedN;
        if (f < fLo || f > fHi) continue;
        juce::String line = juce::String (f, 3);
        for (size_t c = 0; c < mags.size() && c < 4; ++c)
        {
            const int bin = std::min (k, (int) mags[c].size() - 1);
            // Magnitudes are normalised against the fixed floor, so the
            // level is -kNormFloorDb * (1 - mag), exactly as painted.
            line += "," + fmtLevel (-kNormFloorDb * (1.0 - (double) mags[c][(size_t) bin]));
        }
        lines.add (line);
    }

    juce::SystemClipboard::copyTextToClipboard (lines.joinIntoString ("\n"));
}

// ============================================================ SpectralControlsPanel

SpectralControlsPanel::SpectralControlsPanel (Application& appIn)
    : app (appIn), preview (appIn)
{
    const ColourMap maps[] = { ColourMap::Classic, ColourMap::Viridis, ColourMap::Plasma,
                               ColourMap::Inferno, ColourMap::Magma, ColourMap::Hot,
                               ColourMap::Cool, ColourMap::Grayscale };
    const char* names[] = { "Classic (Default)", "Viridis", "Plasma", "Inferno",
                            "Magma", "Hot", "Cool", "Grayscale" };
    for (int i = 0; i < 8; ++i)
        colourMapCombo.addItem (names[i], (int) maps[i] + 1);

    colourMapCombo.onChange = [this] {
        app.colourMap = (ColourMap) (colourMapCombo.getSelectedId() - 1);
        app.spectralEngine.setColourMap (app.colourMap);
        preview.repaint();
        if (app.onStateChange) app.onStateChange();
    };

    // 512 gives the finest frequency resolution; larger sizes give a finer
    // time resolution of the spectrogram tiles.
    fftSizeCombo.addItem ("512", 512);
    for (int n = 1024; n <= 65536; n *= 2)
        fftSizeCombo.addItem (juce::String (n), n);
    fftSizeCombo.onChange = [this] {
        app.settings.fftSize = fftSizeCombo.getSelectedId();
        if (app.onStateChange) app.onStateChange();
    };

    // The slider covers 1..160 dB; the text field next to it accepts any range
    // greater than 1 dB (values above 160 apply beyond the slider's reach).
    dBRangeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    dBRangeSlider.setRange (1.0, 160.0, 1.0);
    dBRangeSlider.setValue ((double) app.settings.dbRange(), juce::dontSendNotification);
    dBRangeSlider.onValueChange = [this] {
        if (!applyDbRangeValue (dBRangeSlider.getValue()))
            syncLevelControls();   // e.g. the slider's 1 dB end: revert to current range
    };

    auto styleLevelEdit = [] (LevelEdit& e)
    {
        e.setFont (11.0f);
        e.setColour (juce::TextEditor::backgroundColourId, juce::Colour::fromRGB (45, 45, 48));
        e.setColour (juce::TextEditor::textColourId, juce::Colours::lightgrey);
        e.setColour (juce::TextEditor::outlineColourId, juce::Colour::fromRGB (72, 72, 78));
    };

    styleLevelEdit (dbRangeEdit);
    dbRangeEdit.onEnter     = [this] { applyDbRangeText (dbRangeEdit.getText()); };
    dbRangeEdit.onFocusLost = [this] { applyDbRangeText (dbRangeEdit.getText()); };

    styleLevelEdit (dbMinEdit);
    dbMinEdit.onEnter       = [this] { applyDbMinText (dbMinEdit.getText()); };
    dbMinEdit.onFocusLost   = [this] { applyDbMinText (dbMinEdit.getText()); };

    styleLevelEdit (dbMaxEdit);
    dbMaxEdit.setJustification (juce::Justification::centredRight);
    dbMaxEdit.onEnter       = [this] { applyDbMaxText (dbMaxEdit.getText()); };
    dbMaxEdit.onFocusLost   = [this] { applyDbMaxText (dbMaxEdit.getText()); };

    syncLevelControls();

    logToggle.setClickingTogglesState (true);
    logToggle.setButtonText (app.settings.logFrequency ? "Logarithmic" : "Linear");
    logToggle.setToggleState (app.settings.logFrequency, juce::dontSendNotification);
    logToggle.onClick = [this] {
        app.settings.logFrequency = logToggle.getToggleState();
        logToggle.setButtonText (app.settings.logFrequency ? "Logarithmic" : "Linear");
        if (app.onStateChange) app.onStateChange();
    };

    resetFreqBtn.setButtonText ("Reset Freq");
    resetFreqBtn.onClick = [this] {
        // Reset the spectral view's local frequency axis to the defaults.
        app.settings.minFrequency = 20.0f;
        app.settings.maxFrequency = 20000.0f;
        if (onResetFreq) onResetFreq();
        if (app.onStateChange) app.onStateChange();
    };

    colourMapCombo.setTooltip ("Colour map used by the spectrogram. 'Classic (Default)' is the "
                                "traditional DAW spectral palette.");
    dBRangeSlider.setTooltip ("Spectral dynamic range in dB (1-160): how far below the top of "
                               "the palette the spectrogram displays. Higher values reveal quieter detail.");
    dbRangeEdit.setTooltip ("Type any dB range greater than 1. Values above 160 apply beyond the slider's reach.");
    dbMinEdit.setTooltip ("Bottom level of the colour window: type a value in the file's unit "
                          "(dB, V or g) to change it; the dB range follows.");
    dbMaxEdit.setTooltip ("Top level of the colour window: type a value in the file's unit "
                          "(dB, V or g) to change it; the dB range follows.");
    logToggle.setTooltip ("Toggle the spectrogram's frequency axis between logarithmic and linear scaling.");
    resetFreqBtn.setTooltip ("Reset the spectrogram's vertical (frequency) zoom to its default range.");
    fftSizeCombo.setTooltip ("FFT size used by the spectrogram tiles. Larger values give finer "
                             "frequency resolution but render more slowly.");

    colourMapLab.setText ("Colour Map:", juce::dontSendNotification);
    dbRangeLab.setText ("dB Range:", juce::dontSendNotification);
    for (auto* lab : { &colourMapLab, &dbRangeLab })
        lab->setFont (12.0f);

    addAndMakeVisible (&colourMapLab);
    addAndMakeVisible (&colourMapCombo);
    addAndMakeVisible (&preview);
    addAndMakeVisible (&dbMinEdit);
    addAndMakeVisible (&dbMaxEdit);
    addAndMakeVisible (&dbRangeLab);
    addAndMakeVisible (&dBRangeSlider);
    addAndMakeVisible (&dbRangeEdit);
    addAndMakeVisible (&logToggle);
    addAndMakeVisible (&resetFreqBtn);
    addAndMakeVisible (&fftSizeCombo);
}

void SpectralControlsPanel::refresh()
{
    colourMapCombo.setSelectedId ((int) app.colourMap + 1, juce::dontSendNotification);
    fftSizeCombo.setSelectedId (app.settings.fftSize, juce::dontSendNotification);
    syncLevelControls();
    logToggle.setToggleState (app.settings.logFrequency, juce::dontSendNotification);
    logToggle.setButtonText (app.settings.logFrequency ? "Logarithmic" : "Linear");
}

void SpectralControlsPanel::resized()
{
    auto b = getLocalBounds().reduced (6, 4);

    {   // Colour map row
        auto row = b.removeFromTop (28);
        colourMapLab.setBounds (row.removeFromLeft (76));
        colourMapCombo.setBounds (row.reduced (0, 0));
    }

    preview.setBounds (b.removeFromTop (56));
    {   // editable level labels over the gradient's left/right ends
        dbMinEdit.setBounds (preview.getX() + 1, preview.getY(), 72, 18);
        dbMaxEdit.setBounds (preview.getRight() - 73, preview.getY(), 72, 18);
    }
    b.reduce (0, 4);

    {   // dB range row: slider (1-160) + free-text field for any value > 1
        auto row = b.removeFromTop (28);
        dbRangeLab.setBounds (row.removeFromLeft (76));
        dbRangeEdit.setBounds (row.removeFromRight (56));
        dBRangeSlider.setBounds (row);
    }

    {   // Log + Reset Freq + FFT size row
        auto row = b.removeFromTop (28);
        logToggle.setBounds (row.removeFromLeft (104));
        resetFreqBtn.setBounds (row.removeFromRight (86));
        fftSizeCombo.setBounds (row.reduced (6, 0));
    }
}

// Right-click on empty panel area: offer to close the panel.
void SpectralControlsPanel::mouseDown (const juce::MouseEvent& e)
{
    if (!e.mods.isRightButtonDown()) return;
    if (showClosePanelMenu() && onCloseRequested) onCloseRequested();
}

void SpectralControlsPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));
}

// ------------------------------------------------- spectral level window helpers

static bool parseDbNumber (const juce::String& text, double& out)
{
    const auto t = text.trim();
    if (t.isEmpty()) return false;
    bool hasDigit = false;
    for (char16_t c : t)
    {
        if (c >= '0' && c <= '9')      { hasDigit = true; continue; }
        if (c == '.' || c == '+' || c == '-' || c == 'e' || c == 'E' || c == ' ') continue;
        return false;
    }
    if (!hasDigit) return false;
    out = t.getDoubleValue();
    return std::isfinite (out);
}

static juce::String formatDbValue (double v)
{
    juce::String s (v, 2);
    while (s.endsWith ("0") && s.contains ("."))
        s = s.substring (0, s.length() - 1);
    if (s.endsWith ("."))
        s = s.substring (0, s.length() - 1);
    return s;
}

bool SpectralControlsPanel::parseSpectralLevel (const juce::String& text, const ScaleCalibration& sc,
                                                double& outRelativeDb)
{
    auto t = text.trim();
    if (t.isEmpty()) return false;

    // Strip a trailing unit token if the user kept it ("dB", "dB-SPL", " V", " g").
    for (const juce::String& u : { sc.unitSuffix(), sc.levelUnitLabel() })
        if (u.isNotEmpty() && t.endsWith (u))
            t = t.substring (0, t.length() - u.length()).trim();

    double v;
    if (!parseDbNumber (t, v)) return false;

    if (!sc.isCalibrated())
        outRelativeDb = v;                       // plain relative dB
    else if (sc.usesLinearLevels())
    {
        if (v <= 0.0) return false;              // linear levels must be positive
        outRelativeDb = 20.0 * std::log10 (v / sc.fullScaleValue);
    }
    else
        outRelativeDb = v - sc.levelOffsetDb();  // absolute dB readout -> relative

    return true;
}

bool SpectralControlsPanel::applyDbRangeValue (double rangeDb)
{
    if (!std::isfinite (rangeDb) || rangeDb <= 1.0) return false;
    app.settings.dbMinDb = (float) ((double) app.settings.dbMaxDb - rangeDb);
    syncLevelControls();
    if (app.onStateChange) app.onStateChange();
    return true;
}

bool SpectralControlsPanel::applyDbRangeText (const juce::String& text)
{
    double v;
    if (!parseDbNumber (text, v)) { syncLevelControls(); return false; }
    return applyDbRangeValue (v);
}

bool SpectralControlsPanel::applyDbMinText (const juce::String& text)
{
    double rel;
    if (!parseSpectralLevel (text, app.scale(), rel)) { syncLevelControls(); return false; }
    const double maxDb = (double) app.settings.dbMaxDb;
    if (!(maxDb - rel > 1.0)) { syncLevelControls(); return false; }   // window must stay > 1 dB
    app.settings.dbMinDb = (float) rel;
    syncLevelControls();
    if (app.onStateChange) app.onStateChange();
    return true;
}

bool SpectralControlsPanel::applyDbMaxText (const juce::String& text)
{
    double rel;
    if (!parseSpectralLevel (text, app.scale(), rel)) { syncLevelControls(); return false; }
    const double minDb = (double) app.settings.dbMinDb;
    if (!(rel - minDb > 1.0)) { syncLevelControls(); return false; }   // window must stay > 1 dB
    app.settings.dbMaxDb = (float) rel;
    syncLevelControls();
    if (app.onStateChange) app.onStateChange();
    return true;
}

void SpectralControlsPanel::syncLevelControls()
{
    const double range = (double) app.settings.dbRange();
    dBRangeSlider.setValue (juce::jmax (1.0, juce::jmin (160.0, range)), juce::dontSendNotification);
    dbRangeEdit.setText (formatDbValue (range));
    const auto& sc = app.scale();
    dbMinEdit.setText (ScaleCalibration::formatSpectralLevel ((double) app.settings.dbMinDb, sc));
    dbMaxEdit.setText (ScaleCalibration::formatSpectralLevel ((double) app.settings.dbMaxDb, sc));
    preview.repaint();
}

// ---------------------------------------------------------------- Find dialog
// Shared by the Markers and Transcription panels (Ctrl+F / F3 / Ctrl+F3).
bool showFindDialog (juce::Component* parent, const juce::String& title,
                     const juce::String& lastNeedle, juce::String& needle)
{
    auto* w = new juce::AlertWindow ("Find", "Search for text:",
                                     juce::AlertWindow::QuestionIcon, parent);
    w->addTextEditor ("findQuery", lastNeedle);
    if (auto* ed = w->getTextEditor ("findQuery"))
        // Let Enter/Escape reach the OK/Cancel shortcuts even while the field
        // has keyboard focus.
        ed->setEscapeAndReturnKeysConsumed (false);

    w->addButton ("Find Next", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    // Enter modal state WITHOUT taking keyboard focus: runModalLoop() would
    // otherwise re-enter the modal state and grab focus for the window itself,
    // stealing it from the text field.
    w->enterModalState (false);
    if (auto* ed = w->getTextEditor ("findQuery"))
    {
        ed->setWantsKeyboardFocus (true);
        ed->grabKeyboardFocus();
        ed->selectAll();
    }

    const int result = w->runModalLoop ();
    needle = w->getTextEditorContents ("findQuery").trim();
    delete w;
    return result == 1 && !needle.isEmpty ();
}

// Shared find loop for the Markers and Transcription panels. `match` reports
// whether row r matches; on success, `onFound` selects/scrolls/jumps to it.
// The first find after a query change starts at the top (forward) or bottom
// (backward); later finds continue from the last match, wrapping around.
static bool findRowInDirection (int n, int anchor, int dir,
                                const std::function<bool (int)>& match,
                                const std::function<void (int)>& onFound,
                                int& newAnchor)
{
    if (n == 0) return false;

    int idx = (anchor < 0) ? (dir > 0 ? 0 : n - 1) : anchor + dir;
    for (int i = 0; i < n; ++i, idx += dir)
    {
        const int row = ((idx % n) + n) % n;
        if (match (row))
        {
            newAnchor = row;
            onFound (row);
            return true;
        }
    }

    newAnchor = -1;   // no match: the next find restarts from top/bottom
    return false;
}

// ================================================================= MarkerListPanel

MarkerListPanel::Model::Model (MarkerListPanel* ownerIn) : owner (ownerIn) {}

int MarkerListPanel::Model::getNumRows() { return (int) owner->app.markers().count(); }

void MarkerListPanel::Model::paintRowBackground (juce::Graphics& g, int, int width, int height,
                                                 bool selected)
{
    if (selected)
    {
        g.setColour (juce::Colour::fromRGB (60, 80, 120));
        g.fillRect (0, 0, width, height);
    }
}

void MarkerListPanel::Model::paintCell (juce::Graphics& g, int rowNumber, int columnId,
                                        int width, int height, bool)
{
    if (rowNumber < 0 || rowNumber >= getNumRows()) return;
    const auto& m = owner->app.markers().markers()[(size_t) rowNumber];

    g.setFont (11.0f);
    if (columnId == 1)   // Title
    {
        g.setColour (juce::Colours::white);
        g.drawText (m.name, 4, 0, width - 8, height, juce::Justification::centredLeft, false);
    }
    else
    {
        g.setColour (juce::Colours::lightgrey);
        const juce::String text = (columnId == 2) ? formatTimeCode (m.start)
                                                  : (columnId == 3 ? (m.end < 0 ? "-" : formatTimeCode (m.end))
                                                                   : m.comment);
        g.drawText (text, 4, 0, width - 8, height, juce::Justification::centredLeft, false);
    }
}

void MarkerListPanel::Model::cellDoubleClicked (int rowNumber, int, const juce::MouseEvent&)
{
    if (rowNumber >= 0 && rowNumber < getNumRows())
        owner->activateRow (rowNumber);
}

void MarkerListPanel::activateRow (int row)
{
    if (app.doc () == nullptr || row < 0
        || row >= (int) app.markers().markers().size ()) return;

    selectedRow = row;
    updateDetails(); // Title/Comment are edited in the detail editors below

    // Double-clicking a marker sets the time selection to its range and makes
    // sure both views actually show it.
    const Marker& m = app.markers().markers()[(size_t) row];
    const double end = (m.end > m.start) ? m.end : m.start;
    app.setSelection ({ m.start, end });
    app.ensureTimeRangeVisible (m.start, end);
    if (app.onStateChange)
        app.onStateChange(); // push the new selection to both views
}

void MarkerListPanel::Model::selectedRowsChanged (int row)
{
    if (row >= 0 && row < getNumRows())
    {
        owner->selectedRow = row;
        owner->updateDetails();
    }
}

void MarkerListPanel::Model::deleteKeyPressed (int row)
{
    if (row >= 0 && row < getNumRows())
    {
        const int id = owner->app.markers().markers()[(size_t) row].id;
        owner->app.markers().removeMarker (id);
        owner->app.markers().saveSidecar (owner->app.audioEngine().getFile());
        owner->selectedRow = -1;
        owner->refresh();
    }
}

MarkerListPanel::MarkerListPanel (Application& appIn)
    : app (appIn), addBtn ("Add Marker"), deleteBtn ("Delete")
{
    auto& header = table.getHeader();
    // JUCE TableHeaderComponent column IDs must be non-zero.
    header.addColumn ("Title", 1, 120);
    header.addColumn ("Begin", 2, 75);
    header.addColumn ("End", 3, 75);
    header.addColumn ("Comment", 4, 120);

    addAndMakeVisible (&table);
    addAndMakeVisible (&addBtn);
    addAndMakeVisible (&deleteBtn);
    addAndMakeVisible (&beginEdit);
    addAndMakeVisible (&endEdit);
    addAndMakeVisible (&lengthEdit);
    addAndMakeVisible (&titleEdit);
    addAndMakeVisible (&commentEdit);

    // Names let unit tests (and future code) find the editors without exposing
    // the private members.
    beginEdit.setName  ("marker-begin-edit");
    endEdit.setName    ("marker-end-edit");
    lengthEdit.setName ("marker-length-edit");
    titleEdit.setName  ("marker-title-edit");
    commentEdit.setName("marker-comment-edit");

    beginEdit.setMultiLine (false);
    endEdit.setMultiLine (false);
    lengthEdit.setMultiLine (false);
    titleEdit.setMultiLine (false);
    // The comment editor is multi-line and stretches to the bottom of the panel;
    // Return inserts a line break instead of committing.
    commentEdit.setMultiLine (true);
    commentEdit.setReturnKeyStartsNewLine (true);

    addBtn.onClick = [this] {
        // Region marker spanning the current selection if there is one,
        // otherwise a point marker at the cursor.
        app.addMarkerAtCursor();
        app.markers().saveSidecar (app.audioEngine().getFile());
        refresh();
        if (app.onStateChange) app.onStateChange();
    };

    deleteBtn.onClick = [this] {
        if (selectedRow >= 0 && selectedRow < (int) app.markers().count())
        {
            const int id = app.markers().markers()[(size_t) selectedRow].id;
            app.markers().removeMarker (id);
            app.markers().saveSidecar (app.audioEngine().getFile());
            selectedRow = -1;
            refresh();
            if (app.onStateChange) app.onStateChange(); // refresh the spectral view's marker overlay
        }
    };

    beginEdit.onTextChange   = [this] { onBeginEndChanged (true); };
    endEdit.onTextChange     = [this] { onBeginEndChanged (false); };
    lengthEdit.onTextChange  = [this] { onLengthChanged(); };
    titleEdit.onTextChange   = [this] { onTitleCommentChanged (true); };
    commentEdit.onTextChange = [this] { onTitleCommentChanged (false); };

    // Right-clicking a row in the table shows the marker context menu. Deep
    // registration is required: clicks on rows are delivered to the row's own
    // component, not to the TableListBox itself.
    table.addMouseListener (this, true);

    addBtn.setTooltip ("Add a marker at the playhead. With an active selection it creates a "
                       "region marker spanning the selection; otherwise a point marker.");
    deleteBtn.setTooltip ("Delete the selected marker from the list and from both views.");
    table.setTooltip ("Double-click a row to edit its details in the fields below. "
                      "Right-click for copy/paste of markers. Press Delete to remove the selected marker.");
    beginEdit.setTooltip ("Begin time of the selected marker, in seconds.");
    endEdit.setTooltip ("End time of the selected marker, in seconds (leave empty for a point marker).");
    lengthEdit.setTooltip ("Length of the selected region marker. Editing it adjusts the End time "
                           "(and vice versa); leave empty to make it a point marker.");
    titleEdit.setTooltip ("Title shown on the marker line and in the Title column.");
    commentEdit.setTooltip ("Free-text comment stored with the marker (saved into WAV cue chunks).");;

    refresh();
}

// Title / Comment editors below the table edit the selected marker in place.
void MarkerListPanel::onTitleCommentChanged (bool isTitle)
{
    if (editing || selectedRow < 0 || selectedRow >= (int) app.markers().count()) return;
    const auto& m = app.markers().markers()[(size_t) selectedRow];
    const juce::String text = (isTitle ? titleEdit : commentEdit).getText().trim();

    if (isTitle)
        app.markers().renameMarker (m.id, text.isEmpty() ? m.name : text);
    else
        app.markers().setComment (m.id, text);

    editing = true;
    updateDetails();
    editing = false;
    table.repaint();
    app.markers().saveSidecar (app.audioEngine().getFile());
    if (app.onStateChange) app.onStateChange(); // refresh the spectral view's marker overlay
}

void MarkerListPanel::onBeginEndChanged (bool isBegin)
{
    if (editing || selectedRow < 0 || selectedRow >= (int) app.markers().count()) return;
    const auto& m = app.markers().markers()[(size_t) selectedRow];
    double begin = parseTimeCode (beginEdit.getText());
    double end = parseTimeCode (endEdit.getText());

    const double dur = app.audioEngine().duration();

    if (m.end < 0) // point marker: begin moves it; typing an end turns it into a region
    {
        if (isBegin && begin >= 0.0 && begin <= dur)
            app.markers().moveMarker (m.id, begin);
        else if (!isBegin && begin >= 0.0 && end > begin + 1e-9)
            app.markers().setRegion (m.id, std::min (begin, dur), std::min (end, dur));
    }
    else
    {
        if (begin >= 0.0 && end > begin && end <= dur + 1e-6)
            app.markers().setRegion (m.id, begin, std::min (end, dur));
    }

    editing = true;
    updateDetails();
    editing = false;
    table.repaint();
    app.markers().saveSidecar (app.audioEngine().getFile());
    if (app.onStateChange) app.onStateChange(); // refresh the spectral view's marker overlay
}

// Editing the Length field adjusts the End time; clearing it turns a region
// marker back into a point marker. (Editing End updates this box via updateDetails.)
void MarkerListPanel::onLengthChanged()
{
    if (editing || selectedRow < 0 || selectedRow >= (int) app.markers().count()) return;
    const auto& m = app.markers().markers()[(size_t) selectedRow];

    double len = parseTimeCode (lengthEdit.getText()); // empty text parses to 0.0
    if (!(len > 0.0))
    {
        if (m.end >= 0)
            app.markers().setPoint (m.id);
    }
    else
    {
        double end = m.start + len;
        const double dur = app.audioEngine().duration();
        if (end > dur) end = dur;
        if (end <= m.start + 1e-9)
            app.markers().setPoint (m.id); // too short to be a region
        else
            app.markers().setRegion (m.id, m.start, end);
    }

    editing = true;
    updateDetails();
    editing = false;
    table.repaint();
    app.markers().saveSidecar (app.audioEngine().getFile());
    if (app.onStateChange) app.onStateChange(); // refresh the spectral view's marker overlay
}

void MarkerListPanel::updateDetails()
{
    if (selectedRow < 0 || selectedRow >= (int) app.markers().count())
    {
        beginEdit.setText ("", juce::dontSendNotification);
        endEdit.setText ("", juce::dontSendNotification);
        lengthEdit.setText ("", juce::dontSendNotification);
        titleEdit.setText ("", juce::dontSendNotification);
        commentEdit.setText ("", juce::dontSendNotification);
        return;
    }

    const auto& m = app.markers().markers()[(size_t) selectedRow];
    editing = true;
    beginEdit.setText (formatTimeCode (m.start), juce::dontSendNotification);
    endEdit.setText (m.end < 0 ? "" : formatTimeCode (m.end), juce::dontSendNotification);
    lengthEdit.setText (m.end < 0 ? "" : formatTimeCode (m.end - m.start),
                        juce::dontSendNotification);
    titleEdit.setText (m.name, juce::dontSendNotification);
    commentEdit.setText (m.comment, juce::dontSendNotification);
    editing = false;
}

void MarkerListPanel::refresh()
{
    table.updateContent();
    table.repaint();
    updateDetails();
}

void MarkerListPanel::selectRow (int row)
{
    selectedRow = (row >= 0 && row < (int) app.markers().count()) ? row : -1;
    updateDetails();
}

// Right-click on the marker table: select the row under the cursor and show
// the copy/paste context menu.
void MarkerListPanel::mouseDown (const juce::MouseEvent& e)
{
    if (!e.mods.isRightButtonDown() || e.mods.isLeftButtonDown())
        return;

    // The event position is relative to the component that was actually hit
    // (a row child of the table); walk up the parent chain to get it in
    // table coordinates.
    float px = e.getPosition().x;
    float py = e.getPosition().y;
    for (auto* c = e.eventComponent; c != nullptr && c != &table;
         c = c->getParentComponent())
    {
        px += static_cast<float> (c->getX());
        py += static_cast<float> (c->getY());
    }
    const auto pInTable = juce::Point<float> (px, py);

    // JUCE 9's TableListBox has no getRowNumberAtPosition; find the row by
    // testing each cell position (marker lists are small).
    int row = -1;
    for (int r = 0; r < table.getNumRows(); ++r)
        if (table.getCellPosition (1, r, true).contains ((int) pInTable.x, (int) pInTable.y))
        {
            row = r;
            break;
        }

    if (row >= 0 && row != selectedRow)
    {
        juce::SparseSet<int> sel;
        sel.addRange ({ row, row + 1 });
        table.setSelectedRows (sel, juce::sendNotificationSync); // -> selectedRowsChanged
    }

    showMarkerContextMenu();
}

void MarkerListPanel::showMarkerContextMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "Copy All Markers", app.markers().count() > 0);
    m.addItem (2, "Paste Markers from Clipboard",
               !juce::SystemClipboard::getTextFromClipboard().trim().isEmpty());
    m.addItem (4, "Export Markers as CSV...", app.markers().count() > 0);
    m.addItem (5, "Import Markers from CSV...", app.audioEngine().isFileLoaded());
    m.addSeparator();
    m.addItem (6, "Remove All Markers", app.markers().count() > 0);
    m.addItem (3, "Find...\tCtrl+F");
    m.addSeparator();
    m.addItem (7, "Close Panel");

    const int id = m.showMenu (juce::PopupMenu::Options().withMousePosition());
    if (id == 1)
        copyAllMarkers();
    else if (id == 2)
        pasteMarkersFromClipboard();
    else if (id == 4)
        exportMarkersCsv();
    else if (id == 5)
        importMarkersCsv();
    else if (id == 6)
        removeAllMarkers();
    else if (id == 3)
        showFindDialog();
    else if (id == 7 && onCloseRequested) onCloseRequested();
}

// useOSNativeDialogBox = false: the native Win32 IFileDialog is unavailable on some
// systems; JUCE's built-in browser is pure GDI and works everywhere.
static constexpr bool kUseNativeFileChooser = false;

void MarkerListPanel::exportMarkersCsv()
{
    if (app.markers().count() == 0)
        return;

    juce::FileChooser chooser ("Export Markers as CSV",
        app.audioEngine().isFileLoaded() ? app.audioEngine().getFile().getParentDirectory()
                                         : juce::File::getSpecialLocation (juce::File::userHomeDirectory),
        "*.csv", kUseNativeFileChooser);

    if (!chooser.browseForFileToSave (true))
        return;

    auto file = chooser.getResult();
    if (!file.hasFileExtension (".csv"))
        file = file.withFileExtension (".csv");

    if (!app.markers().saveCsv (file))
        juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Export failed",
                                            "Could not write the CSV file.", "", this);
}

int MarkerListPanel::importMarkersCsv()
{
    if (!app.audioEngine().isFileLoaded())
        return 0;

    juce::FileChooser chooser ("Import Markers from CSV",
        app.audioEngine().getFile().getParentDirectory(),
        "*.csv", kUseNativeFileChooser);

    if (!chooser.browseForFileToOpen())
        return 0;

    const int added = app.markers().loadCsv (chooser.getResult(),
                                             app.audioEngine().duration());
    if (added > 0)
    {
        app.markers().saveSidecar (app.audioEngine().getFile());
        refresh();
        if (app.onStateChange) app.onStateChange(); // refresh the spectral view's marker overlay
    }
    else
    {
        juce::AlertWindow::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Import failed",
                                            "No markers could be read from the CSV file.",
                                            "", this);
    }

    return added;
}

void MarkerListPanel::removeAllMarkers()
{
    const int n = (int) app.markers().count();
    if (n == 0)
        return;

    juce::AlertWindow w ("Remove all markers?",
                         "This removes all " + juce::String (n) + " marker(s) from this file.",
                         juce::AlertWindow::WarningIcon, this);
    w.addButton ("Remove", 1);
    w.addButton ("Cancel", 0);

    if (w.runModalLoop() != 1)
        return;

    app.markers().clear();
    app.markers().saveSidecar (app.audioEngine().getFile());
    refresh();
    if (app.onStateChange) app.onStateChange(); // refresh the spectral view's marker overlay
}

// ---------------------------------------------------------------- Find (Ctrl+F / F3 / Ctrl+F3)
void MarkerListPanel::setFindQuery (const juce::String& q)
{
    const juce::String t = q.trim ();
    if (t != searchNeedle) { searchNeedle = t; searchAnchorRow = -1; }
}

bool MarkerListPanel::findNext()     { return findInDirection (+1); }
bool MarkerListPanel::findPrevious() { return findInDirection (-1); }

void MarkerListPanel::showFindDialog()
{
    juce::String needle;
    // :: prefix: the member of this same name would otherwise hide the free
    // function during unqualified lookup.
    if (!::showFindDialog (this, "Find in Markers", searchNeedle, needle))
        return;
    setFindQuery (needle);
    findNext ();
}

bool MarkerListPanel::findInDirection (int dir)
{
    const auto& ms = app.markers().markers();
    if (searchNeedle.isEmpty ()) return false;

    return findRowInDirection ((int) ms.size(), searchAnchorRow, dir,
        [this, &ms] (int row) {
            const Marker& m = ms [(size_t) row];
            return m.name.containsIgnoreCase (searchNeedle)
                || m.comment.containsIgnoreCase (searchNeedle);
        },
        [this] (int row) {
            juce::SparseSet<int> sel;
            sel.addRange ({ row, row + 1 });
            table.setSelectedRows (sel, juce::sendNotificationSync); // -> selectedRowsChanged
            table.scrollToEnsureRowIsOnscreen (row);

            const Marker& m = app.markers().markers() [(size_t) row];
            app.setPlayhead (m.start);
            if (m.end > m.start)
                app.setSelection ({ m.start, m.end });
            if (app.onStateChange) app.onStateChange ();   // push to both views
        },
        searchAnchorRow);
}

// Copies every marker as tab-separated lines (one per line, CRLF between rows):
//   Title <tab> Begin <tab> End <tab> Comment
// so the text can be pasted straight into a spreadsheet. Tabs/newlines inside
// fields are flattened to spaces to keep each marker on one row.
void MarkerListPanel::copyAllMarkers()
{
    auto flatten = [] (const juce::String& s) {
        return s.replace ("\r\n", " ").replace ("\n", " ")
                .replace ("\r", " ").replace ("\t", " ");
    };

    juce::String out;
    for (const auto& m : app.markers().markers())
    {
        if (!out.isEmpty())
            out << "\r\n";
        out << flatten (m.name) << '\t'
            << formatTimeCode (m.start) << '\t'
            << (m.end < 0 ? juce::String ("-") : formatTimeCode (m.end)) << '\t'
            << flatten (m.comment);
    }

    if (!out.isEmpty())
        juce::SystemClipboard::copyTextToClipboard (out);
}

// Pastes tab-separated marker lines from the clipboard. Each line is
//   Title <tab> Begin <tab> End <tab> Comment
// with an empty or "-" End creating a point marker.
int MarkerListPanel::pasteMarkersFromClipboard()
{
    if (!app.audioEngine().isFileLoaded())
        return 0;

    const juce::String text = juce::SystemClipboard::getTextFromClipboard();
    if (text.trim().isEmpty())
        return 0;

    juce::StringArray lines;
    lines.addLines (text);

    const double dur = app.audioEngine().duration();
    int added = 0;

    for (const auto& line : lines)
    {
        if (line.trim().isEmpty())
            continue;

        juce::StringArray f;
        f.addTokens (line, "\t", "");
        if (f.size() < 2) // need at least Title + Begin
            continue;

        const juce::String name = f[0].trim();
        double begin = parseTimeCode (f[1]);
        if (!(begin >= 0.0))
            continue;
        begin = std::min (begin, dur);

        double end = -1.0;
        if (f.size() > 2)
        {
            const juce::String eTok = f[2].trim();
            if (!eTok.isEmpty() && eTok != "-")
            {
                const double e = parseTimeCode (eTok);
                if (e > begin + 1e-9)
                    end = std::min (e, dur);
            }
        }

        juce::String comment;
        if (f.size() > 3)
            comment = f[3].trim();

        const juce::String label = name.isEmpty() ? "Marker" : name;
        const int id = (end < 0.0) ? app.markers().addPointMarker (begin, label)
                                   : app.markers().addRegionMarker (begin, end, label);
        if (!comment.isEmpty())
            app.markers().setComment (id, comment);
        ++added;
    }

    if (added > 0)
    {
        app.markers().saveSidecar (app.audioEngine().getFile());
        refresh();
        if (app.onStateChange) app.onStateChange(); // refresh the spectral view's marker overlay
    }

    return added;
}

void MarkerListPanel::resized()
{
    auto b = getLocalBounds().reduced (4, 4);
    table.setBounds (b.removeFromTop (std::max (80, (int) (b.getHeight() * 0.42))));

    addBtn.setBounds (b.removeFromTop (26));
    deleteBtn.setTopLeftPosition (addBtn.getRight() + 8, b.getY());
    deleteBtn.setBounds (deleteBtn.getX(), b.getY(), 70, 26);
    b.reduce (0, 4);

    // Detail grid: one label + editor per row. The Comments editor is
    // multi-line, starts on the same row as its label and extends to the
    // bottom of the panel.
    auto row1 = b.removeFromTop (24);
    beginEdit.setBounds (xoffset (row1, 60));

    auto row2 = b.removeFromTop (24);
    endEdit.setBounds (xoffset (row2, 60));

    auto row3 = b.removeFromTop (24);
    lengthEdit.setBounds (xoffset (row3, 60));

    auto row4 = b.removeFromTop (24);
    titleEdit.setBounds (xoffset (row4, 60));

    commentEdit.setBounds (xoffset (b, 60));
}

void MarkerListPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));
    g.setColour (juce::Colours::white);
    g.setFont (12.0f);

    auto b = getLocalBounds().reduced (4, 4);
    const int tableH = std::max (80, (int) (b.getHeight() * 0.42));
    b.removeFromTop (tableH + 30); // skip table + buttons rows
    g.drawText ("Begin:", 6, b.getY(), 52, 20, juce::Justification::centredLeft, false);
    auto row2 = b.reduced (0, 24);
    g.drawText ("End:", 6, row2.getY(), 52, 20, juce::Justification::centredLeft, false);
    auto row3 = b.reduced (0, 48);
    g.drawText ("Length:", 6, row3.getY(), 52, 20, juce::Justification::centredLeft, false);
    auto row4 = b.reduced (0, 72);
    g.drawText ("Title:", 6, row4.getY(), 52, 20, juce::Justification::centredLeft, false);
    auto row5 = b.reduced (0, 96);
    // "Comments:" is wider than the other labels; give it room so it is not
    // clipped (the text still ends before the editor column starts).
    g.drawText ("Comments:", 6, row5.getY(), 72, 20, juce::Justification::centredLeft, false);
}

// ---------------------------------------------------------------- Status bar

void StatusBar::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds();
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));
    g.setColour (juce::Colour::fromRGB (90, 90, 100));
    g.drawLine (0.0f, 0.0f, (float) b.getWidth(), 0.0f);

    g.setFont (12.0f);
    int leftX = 6;
    if (!fileName.isEmpty())
    {
        // The active file's name sits at the far left; its sample rate follows
        // it, and the mouse probe text starts to their right when present.
        g.setColour (juce::Colour::fromRGB (235, 235, 240));
        const juce::Font f (12.0f);
        const int w = std::min (b.getWidth() / 2 - leftX,
                                (int) juce::GlyphArrangement::getStringWidth (f, fileName) + 8);
        if (w > 10)
        {
            g.drawText (fileName, leftX, 0, w, b.getHeight(),
                        juce::Justification::left, false);
            leftX += w + 12;
        }
    }
    if (!ratePart.isEmpty() && leftX < b.getWidth() - 8)
    {
        g.setColour (juce::Colour::fromRGB (205, 205, 215));
        const juce::Font f (12.0f);
        const int w = std::min (b.getWidth() / 2 + 60 - leftX,
                                (int) juce::GlyphArrangement::getStringWidth (f, ratePart) + 8);
        if (w > 10)
        {
            g.drawText (ratePart, leftX, 0, w, b.getHeight(),
                        juce::Justification::left, false);
            leftX += w + 12;
        }
    }
    if (!modeText.isEmpty() && leftX < b.getWidth() - 8)
    {
        // The active spectral tool name, in a distinct colour so it reads as
        // a mode indicator rather than probe data.
        g.setColour (juce::Colour::fromRGB (120, 170, 255));
        const juce::Font f (12.0f);
        const int w = std::min (b.getWidth() - leftX - 8,
                                (int) juce::GlyphArrangement::getStringWidth (f, modeText) + 8);
        if (w > 10)
        {
            g.drawText (modeText, leftX, 0, w, b.getHeight(),
                        juce::Justification::left, false);
            leftX += w + 12;
        }
    }
    if (!mouseText.isEmpty() && leftX < b.getWidth() - 8)
    {
        g.setColour (juce::Colour::fromRGB (205, 205, 215));
        const int w = std::max (10, b.getWidth() / 2 + 60 - leftX);
        g.drawText (mouseText, leftX, 0, w, b.getHeight(),
                    juce::Justification::left, false);
    }

    const juce::String right = selPart.isEmpty() ? playPart
                              : (playPart.isEmpty() ? selPart
                                                   : selPart + "     " + playPart);
    if (!right.isEmpty())
    {
        g.setColour (juce::Colour::fromRGB (160, 190, 235));
        juce::Font f (12.0f);
        const int w = std::min (b.getWidth() - 8,
                                (int) juce::GlyphArrangement::getStringWidth (f, right) + 16);
        g.drawText (right, b.getWidth() - w, 0, w, b.getHeight(),
                    juce::Justification::right, false);
    }
}

void StatusBar::setSelection (double t0, double t1)
{
    const juce::String s = (t1 > t0)
        ? "Sel: " + juce::String (t0, 3) + " - " + juce::String (t1, 3) + " s (" + juce::String (t1 - t0, 3) + " s)"
        : juce::String();
    if (s != selPart) { selPart = s; repaint(); }
}

void StatusBar::setPlayhead (double t)
{
    const juce::String s = "Pos: " + juce::String (t, 3) + " s";
    if (s != playPart) { playPart = s; repaint(); }
}

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
// ------------------------------------------------- Transcription panel

TranscriptionPanel::Model::Model (TranscriptionPanel* ownerIn) : owner (ownerIn) {}

int TranscriptionPanel::Model::getNumRows()
{
    auto* d = owner->app.doc ();
    return d != nullptr ? (int) d->sctt.segments.size () : 0;
}

void TranscriptionPanel::Model::paintRowBackground (juce::Graphics& g, int, int width, int height,
                                                    bool selected)
{
    if (selected)
    {
        g.setColour (juce::Colour::fromRGB (60, 80, 120));
        g.fillRect (0, 0, width, height);
    }
}

void TranscriptionPanel::Model::paintCell (juce::Graphics& g, int rowNumber, int columnId,
                                           int width, int height, bool)
{
    auto* d = owner->app.doc ();
    if (d == nullptr || rowNumber < 0 || rowNumber >= (int) d->sctt.segments.size ()) return;
    const ScttSegment& s = d->sctt.segments [(size_t) rowNumber];

    g.setFont (11.0f);
    if (columnId == 1)   // index
    {
        g.setColour (juce::Colours::lightgrey);
        g.drawText (juce::String (rowNumber + 1), 4, 0, width - 8, height,
                    juce::Justification::centredLeft, false);
    }
    else if (columnId == 2 || columnId == 3)   // start / end
    {
        g.setColour (juce::Colours::lightgrey);
        const double t = (columnId == 2) ? s.startSec : s.endSec;
        g.drawText (formatTimeCode (t), 4, 0, width - 8, height,
                    juce::Justification::centredLeft, false);
    }
    else   // text (single line, truncated by drawText)
    {
        g.setColour (juce::Colours::white);
        g.drawText (s.text, 4, 0, width - 8, height, juce::Justification::centredLeft, false);
    }
}

void TranscriptionPanel::Model::cellClicked (int rowNumber, int, const juce::MouseEvent& e)
{
    // Left clicks only change the selection; a right click shows a small
    // context menu for the segment under the cursor.
    if (!e.mods.isRightButtonDown ()) return;

    auto* d = owner->app.doc ();
    if (d == nullptr || rowNumber < 0 || rowNumber >= (int) d->sctt.segments.size ()) return;

    juce::PopupMenu menu;
    menu.addItem (1, "Add as marker");
    menu.addItem (2, "Find...\tCtrl+F");
    // Options() targets the current mouse position - exactly where the user
    // right-clicked.
    const int id = menu.showMenu (juce::PopupMenu::Options ());
    if (id == 1)
        owner->addMarkerForRow (rowNumber);
    else if (id == 2)
        owner->showFindDialog ();
}

void TranscriptionPanel::Model::cellDoubleClicked (int rowNumber, int, const juce::MouseEvent&)
{
    auto* d = owner->app.doc ();
    if (d == nullptr || rowNumber < 0 || rowNumber >= (int) d->sctt.segments.size ()) return;

    // Jump to the segment and select its range.
    const ScttSegment& s = d->sctt.segments [(size_t) rowNumber];
    owner->app.setPlayhead (s.startSec);
    if (s.endSec > s.startSec)
        owner->app.setSelection ({ s.startSec, s.endSec });
    if (owner->app.onStateChange)
        owner->app.onStateChange ();   // push the new cursor/selection to both views
}

void TranscriptionPanel::addMarkerForRow (int rowNumber)
{
    auto* d = app.doc ();
    if (d == nullptr || rowNumber < 0 || rowNumber >= (int) d->sctt.segments.size ()) return;

    const ScttSegment& s = d->sctt.segments [(size_t) rowNumber];

    juce::String full = juce::String (s.text).trim ();
    juce::String title = full;
    if (title.length () > 80)
        title = title.substring (0, 79) + "...";
    if (title.isEmpty ())
        title = "Marker";

    const int id = (s.endSec > s.startSec)
        ? app.markers ().addRegionMarker (s.startSec, s.endSec, title)
        : app.markers ().addPointMarker (s.startSec, title);
    if (!full.isEmpty ())
        app.markers ().setComment (id, full);   // keep the full text

    app.markers ().saveSidecar (app.audioEngine ().getFile ());
    refresh ();
    if (app.onStateChange) app.onStateChange ();   // views + markers panel
}

TranscriptionPanel::TranscriptionPanel (Application& appIn)
    : app (appIn), runBtn ("Transcribe..."), clearBtn ("Clear")
{
    auto& header = table.getHeader();
    // JUCE TableHeaderComponent column IDs must be non-zero.
    header.addColumn ("#", 1, 40);
    header.addColumn ("Start", 2, 85);
    header.addColumn ("End", 3, 85);
    header.addColumn ("Text", 4, 160);

    addAndMakeVisible (&table);
    addAndMakeVisible (&runBtn);
    addAndMakeVisible (&clearBtn);

    runBtn.onClick = [this] { if (onRunRequested) onRunRequested (); };
    clearBtn.onClick = [this] {
        auto* d = app.doc ();
        if (d != nullptr && !d->sctt.empty ())
        {
            d->setTranscription ({});
            refresh();
            if (app.onStateChange) app.onStateChange ();
        }
    };

    runBtn.setTooltip ("Run Whisper transcription on the whole file. The first use downloads "
                       "the model weights to %APPDATA%/SpectroCraft/models.");
    clearBtn.setTooltip ("Remove all transcription segments from this file (the audio is not modified).");
    table.setTooltip ("Double-click a row to jump to it and select its range.\n"
                      "Right-click a row to add it as a marker or search (Find).\n"
                      "Ctrl+F searches, F3 / Ctrl+F3 find next / previous.");

    refresh();
}

// Right-click on empty panel area: offer to close the panel. (Row right-
// clicks are handled by the table's own context menu in Model::cellClicked.)
void TranscriptionPanel::mouseDown (const juce::MouseEvent& e)
{
    if (!e.mods.isRightButtonDown() || e.mods.isLeftButtonDown()) return;
    if (showClosePanelMenu() && onCloseRequested) onCloseRequested();
}

void TranscriptionPanel::refresh()
{
    table.updateContent ();
    table.repaint ();
}

void TranscriptionPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));
}

void TranscriptionPanel::resized()
{
    auto b = getLocalBounds ().reduced (4, 4);

    // The button strip is pinned to the bottom of the panel; the segment
    // list takes all of the space above it.
    auto btnRow = b.removeFromBottom (30);
    runBtn.setBounds (btnRow.getX (), btnRow.getY () + 2, 130, 26);
    clearBtn.setBounds (runBtn.getRight () + 8, btnRow.getY () + 2, 90, 26);

    table.setBounds (b.reduced (0, 4));   // small gap between list and buttons
}

// ---------------------------------------------------------------- Find (Ctrl+F / F3 / Ctrl+F3)
void TranscriptionPanel::setFindQuery (const juce::String& q)
{
    const juce::String t = q.trim ();
    if (t != searchNeedle) { searchNeedle = t; searchAnchorRow = -1; }
}

bool TranscriptionPanel::findNext()     { return findInDirection (+1); }
bool TranscriptionPanel::findPrevious() { return findInDirection (-1); }

void TranscriptionPanel::showFindDialog()
{
    juce::String needle;
    // :: prefix: the member of this same name would otherwise hide the free
    // function during unqualified lookup.
    if (!::showFindDialog (this, "Find in Transcription", searchNeedle, needle))
        return;
    setFindQuery (needle);
    findNext ();
}

bool TranscriptionPanel::findInDirection (int dir)
{
    auto* d = app.doc ();
    if (d == nullptr || searchNeedle.isEmpty ()) return false;

    const auto& segs = d->sctt.segments;
    return findRowInDirection ((int) segs.size (), searchAnchorRow, dir,
        [this, &segs] (int row) {
            return juce::String (segs [(size_t) row].text).containsIgnoreCase (searchNeedle);
        },
        [this, d] (int row) {
            table.selectRow (row);
            table.scrollToEnsureRowIsOnscreen (row);

            // Jump to the segment like a double-click.
            const ScttSegment& s = d->sctt.segments [(size_t) row];
            app.setPlayhead (s.startSec);
            if (s.endSec > s.startSec)
                app.setSelection ({ s.startSec, s.endSec });
            if (app.onStateChange) app.onStateChange ();
        },
        searchAnchorRow);
}
#endif // SC_ENABLE_AI

// ------------------------------------------------- Selection panel

static void showSelectionMessage (const juce::String& title, const juce::String& msg)
{
    juce::AlertWindow dlg (title, msg, juce::AlertWindow::WarningIcon);
    dlg.setColour (juce::AlertWindow::backgroundColourId,
                   juce::Colour::fromRGB (45, 45, 48));
    dlg.addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dlg.runModalLoop ();
}

SelectionPanel::Model::Model (SelectionPanel* ownerIn) : owner (ownerIn) {}

int SelectionPanel::Model::getNumRows()
{
    auto* d = owner->app.doc ();
    return d != nullptr ? (int) d->savedSelections.size () : 0;
}

void SelectionPanel::Model::paintRowBackground (juce::Graphics& g, int, int width, int height,
                                                bool selected)
{
    if (selected)
    {
        g.setColour (juce::Colour::fromRGB (60, 80, 120));
        g.fillRect (0, 0, width, height);
    }
}

void SelectionPanel::Model::paintCell (juce::Graphics& g, int rowNumber, int columnId,
                                       int width, int height, bool)
{
    auto* d = owner->app.doc ();
    if (d == nullptr || rowNumber < 0 || rowNumber >= (int) d->savedSelections.size ()) return;
    const SavedSpectralSelection& s = d->savedSelections [(size_t) rowNumber];

    g.setFont (11.0f);
    if (columnId == 1)   // name
    {
        g.setColour (juce::Colours::white);
        g.drawText (s.name, 4, 0, width - 8, height, juce::Justification::centredLeft, false);
    }
    else   // shape count
    {
        g.setColour (juce::Colours::lightgrey);
        g.drawText (juce::String (s.data.count ()), 4, 0, width - 8, height,
                    juce::Justification::centredRight, false);
    }
}

void SelectionPanel::Model::cellDoubleClicked (int rowNumber, int, const juce::MouseEvent&)
{
    owner->doRecall (rowNumber);
}

SelectionPanel::SelectionPanel (Application& appIn)
    : app (appIn),
      saveBtn ("Store"), recallBtn ("Recall"), deleteBtn ("Delete"),
      diskToggle ("Save to disk"), applyMultBtn ("Apply"), pickBtn ("Pick points...")
{
    auto& header = table.getHeader();
    // JUCE TableHeaderComponent column IDs must be non-zero.
    header.addColumn ("Name", 1, 160);
    header.addColumn ("Shapes", 2, 70);

    savedLab.setText ("Saved selections", juce::dontSendNotification);
    cloneLab.setText ("Clone & Multiply", juce::dontSendNotification);
    multHintLab.setText ("Multipliers of the current selection, e.g. 0.5, 2, 5",
                        juce::dontSendNotification);

    addAndMakeVisible (&savedLab);
    addAndMakeVisible (&table);
    addAndMakeVisible (&saveBtn);
    addAndMakeVisible (&recallBtn);
    addAndMakeVisible (&deleteBtn);
    addAndMakeVisible (&diskToggle);
    addAndMakeVisible (&cloneLab);
    addAndMakeVisible (&multHintLab);
    addAndMakeVisible (&multEdit);
    addAndMakeVisible (&applyMultBtn);
    addAndMakeVisible (&pickBtn);

    saveBtn.onClick = [this] { doSave (); };
    recallBtn.onClick = [this] { doRecall (table.getSelectedRow ()); };
    deleteBtn.onClick = [this] { doDelete (); };
    diskToggle.setClickingTogglesState (true);
    diskToggle.onClick = [this] { toggleDisk (); };
    applyMultBtn.onClick = [this] { doApplyMultipliers (); };
    pickBtn.onClick = [this] { togglePick (); };   // plain button; label driven by refresh()

    saveBtn.setTooltip ("Store the current spectral selection under a name. Saving again with "
                        "the same name updates that entry.");
    recallBtn.setTooltip ("Replace the current spectral selection with the selected saved one "
                         "(double-clicking a row does the same).");
    deleteBtn.setTooltip ("Remove the selected saved selection from this file's list.");
    diskToggle.setTooltip ("Keep the list in sync with \"<file>.selections.json\" next to the audio "
                          "file so it survives restarts. Off = in-memory only for this session.");
    multHintLab.setTooltip ("Each number becomes a clone of the current selection whose frequencies "
                           "(and thickness) are multiplied by that factor - e.g. 2 and 5 select the "
                           "2nd and 5th harmonics of a fundamental band.");
    applyMultBtn.setTooltip ("Append clones of the current selection at the given frequency multipliers.");
    pickBtn.setTooltip ("Interactive mode: click a reference point in the spectrogram, then click "
                       "each target frequency. Each click adds clones scaled by (clicked / reference). "
                       "Esc or this button ends the session.");

    // Right-clicking a row in the table shows the panel context menu (Load
    // from disk / Close Panel). Deep registration is required: clicks on rows
    // are delivered to the row's own component, not to the TableListBox.
    table.addMouseListener (this, true);

    refresh();
}

void SelectionPanel::refresh()
{
    table.updateContent ();
    table.repaint ();

    auto* d = app.doc ();
    const bool hasSel = app.hasSpectralSelection ();
    saveBtn.setEnabled (hasSel);
    applyMultBtn.setEnabled (hasSel);
    // The button must stay enabled while a picking session is active so the
    // user can click "Stop picking" to end it (Esc works too).
    pickBtn.setEnabled (d != nullptr && hasSel);

    const int selRow = table.getSelectedRow ();
    const bool haveRow = d != nullptr && selRow >= 0 && selRow < (int) d->savedSelections.size ();
    recallBtn.setEnabled (haveRow);
    deleteBtn.setEnabled (haveRow);

    // Keep the toggle/button states in step with the app without firing clicks.
    diskToggle.setToggleState (d != nullptr && d->selectionsOnDisk,
                               juce::dontSendNotification);
    pickBtn.setButtonText (app.isClonePicking () ? "Stop picking" : "Pick points...");
}

void SelectionPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));

    // Separator between the saved-selections and clone & multiply sections.
    const int y = multHintLab.getY () - 6;
    if (y > table.getBottom ())
    {
        g.setColour (juce::Colour::fromRGB (70, 70, 76));
        g.drawLine (4, y, getWidth () - 4, y);
    }

    // Highlight while a .selections.json drag hovers over the panel.
    if (dragOver)
    {
        g.setColour (juce::Colour::fromRGB (96, 150, 255).withAlpha (0.9f));
        g.drawRect (getLocalBounds (), 2);
    }
}

void SelectionPanel::resized()
{
    auto b = getLocalBounds ().reduced (4, 4);

    // Fixed-height rows from the bottom up; the table takes what is left.
    const int rowH = 28;
    auto pickRow   = b.removeFromBottom (rowH + 6);
    auto applyRow  = b.removeFromBottom (rowH + 6);
    auto hintRow   = b.removeFromBottom (18);
    auto cloneRow  = b.removeFromBottom (20);
    auto btnRow    = b.removeFromBottom (rowH + 8);

    savedLab.setBounds (b.getX (), b.getY () + 2, b.getWidth (), 16);
    table.setBounds (b.getX (), savedLab.getBottom () + 4,
                    b.getWidth (), std::max (60, btnRow.getY () - savedLab.getBottom () - 8));

    const int gap = 8;
    const int bw = std::max (52, (btnRow.getWidth () - gap * 3) / 4);
    saveBtn.setBounds (btnRow.getX (), btnRow.getY () + 2, bw, rowH);
    recallBtn.setBounds (saveBtn.getRight () + gap, btnRow.getY () + 2, bw, rowH);
    deleteBtn.setBounds (recallBtn.getRight () + gap, btnRow.getY () + 2, bw, rowH);
    diskToggle.setBounds (deleteBtn.getRight () + gap, btnRow.getY () + 2,
                         std::max (70, btnRow.getWidth () - (deleteBtn.getRight () - btnRow.getX ()) - gap),
                         rowH);

    cloneLab.setBounds (cloneRow.getX (), cloneRow.getY () + 1, cloneRow.getWidth (), 16);
    multHintLab.setBounds (hintRow.getX (), hintRow.getY (), hintRow.getWidth (), 14);

    const int applyW = 70;
    multEdit.setBounds (applyRow.getX (), applyRow.getY () + 2,
                       std::max (80, applyRow.getWidth () - applyW - gap), rowH);
    applyMultBtn.setBounds (multEdit.getRight () + gap, applyRow.getY () + 2, applyW, rowH);

    pickBtn.setBounds (pickRow.getX (), pickRow.getY () + 2, pickRow.getWidth (), rowH);
}

void SelectionPanel::doSave()
{
    auto* d = app.doc ();
    if (d == nullptr || d->spectralSel.empty ()) return;

    juce::AlertWindow dlg ("Save Selection",
                          "Name for this spectral selection:",
                          juce::AlertWindow::NoIcon);
    dlg.setColour (juce::AlertWindow::backgroundColourId,
                   juce::Colour::fromRGB (45, 45, 48));

    // This JUCE build's addTextEditor returns void; fetch the editor by name.
    dlg.addTextEditor ("name", "", "Name:");
    auto* ed = dlg.getTextEditor ("name");
    dlg.addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dlg.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    if (ed != nullptr)
    {
        ed->setWantsKeyboardFocus (true);
        ed->selectAll();
        ed->grabKeyboardFocus();
    }

    if (dlg.runModalLoop () != 1 || ed == nullptr) return;
    const juce::String name = ed->getText ().trim ();
    if (name.isEmpty ())
    {
        showSelectionMessage ("Save Selection", "Please enter a name.");
        return;
    }

    SavedSpectralSelection s;
    s.name = name;
    s.data = d->spectralSel;   // deep copy of the current shapes

    bool replaced = false;
    for (auto& e : d->savedSelections)
        if (e.name == name) { e = std::move (s); replaced = true; break; }
    if (!replaced)
        d->savedSelections.push_back (std::move (s));

    if (d->selectionsOnDisk)
        d->saveSelectionsToDisk ();
    refresh();
}

void SelectionPanel::doRecall (int row)
{
    auto* d = app.doc ();
    if (d == nullptr || row < 0 || row >= (int) d->savedSelections.size ()) return;
    const auto& sel = d->savedSelections [(size_t) row];
    app.setSpectralSelectionFrom (sel.data);

    // Make sure both views show the recalled selection's time range.
    if (!sel.data.empty())
    {
        const auto ext = sel.data.timeExtent();
        app.ensureTimeRangeVisible (ext.getStart(), ext.getEnd());
    }
    refresh();
}

void SelectionPanel::mouseDown (const juce::MouseEvent& e)
{
    if (!e.mods.isRightButtonDown() || e.mods.isLeftButtonDown()) return;
    showLoadContextMenu ();
}

void SelectionPanel::showLoadContextMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "Load from disk...");
    m.addSeparator();
    m.addItem (2, "Close Panel");
    const int id = m.showMenu (juce::PopupMenu::Options().withMousePosition());
    if (id == 1) loadFromDisk ();
    else if (id == 2 && onCloseRequested) onCloseRequested();
}

bool SelectionPanel::loadFromDisk()
{
    auto* d = app.doc ();
    if (d == nullptr) return false;

    // Prefer the sidecar next to the audio file; otherwise let the user pick.
    juce::File source = savedSelectionsSidecarPath (d->file);
    if (!source.existsAsFile ())
    {
        juce::FileChooser chooser ("Load a saved-selections file",
                                   d->file, "*.json", kUseNativeFileChooser);
        if (!chooser.browseForFileToOpen ()) return false;
        source = chooser.getResult ();
    }

    const bool ok = loadSelectionsFromFile (source);
    if (!ok)
        showSelectionMessage ("Load from disk",
                              "Could not read saved selections from:\n"
                              + source.getFullPathName());
    return ok;
}

bool SelectionPanel::loadSelectionsFromFile (const juce::File& f)
{
    auto* d = app.doc ();
    if (d == nullptr || !f.existsAsFile ()) return false;
    if (!f.getFileExtension().equalsIgnoreCase (".json")) return false;

    std::vector<SavedSpectralSelection> list;
    if (!deserialiseSavedSelections (f.loadFileAsString (), list)) return false;

    d->savedSelections = std::move (list);
    // Loading the canonical sidecar next to the audio file turns on syncing.
    if (f == savedSelectionsSidecarPath (d->file))
        d->selectionsOnDisk = true;
    refresh ();
    return true;
}

bool SelectionPanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (juce::File (f).getFileExtension().equalsIgnoreCase (".json"))
            return true;
    return false;
}

void SelectionPanel::filesDropped (const juce::StringArray& files, int, int)
{
    dragOver = false;   // fileDragExit may not follow a drop
    repaint ();

    bool loaded = false;
    for (const auto& f : files)
        loaded |= loadSelectionsFromFile (juce::File (f));

    if (!loaded)
        showSelectionMessage ("Load from disk",
                              "Could not read saved selections from the dropped file(s).\n"
                              "Expected a .selections.json document.");
}

void SelectionPanel::fileDragEnter (const juce::StringArray&, int, int)
{
    dragOver = true;
    repaint ();
}

void SelectionPanel::fileDragExit (const juce::StringArray&)
{
    dragOver = false;
    repaint ();
}

void SelectionPanel::doDelete()
{
    auto* d = app.doc ();
    if (d == nullptr) return;
    const int row = table.getSelectedRow ();
    if (row < 0 || row >= (int) d->savedSelections.size ()) return;

    d->savedSelections.erase (d->savedSelections.begin () + row);
    if (d->selectionsOnDisk)
        d->saveSelectionsToDisk ();
    refresh();
}

void SelectionPanel::doApplyMultipliers()
{
    std::string err;
    const auto mults = Application::parseMultiplierList (multEdit.getText (), &err);
    if (mults.empty ())
    {
        showSelectionMessage ("Clone & Multiply", juce::String (err));
        return;
    }
    if (!app.cloneAndMultiplySelection (mults, &err))
    {
        showSelectionMessage ("Clone & Multiply", juce::String (err));
        return;
    }
    refresh();
}

void SelectionPanel::toggleDisk()
{
    auto* d = app.doc ();
    if (d == nullptr) return;
    const bool on = diskToggle.getToggleState ();
    if (on)
        d->saveSelectionsToDisk ();   // sets selectionsOnDisk=true when it succeeds
    else
        d->selectionsOnDisk = false;  // stop syncing; the sidecar file stays as-is
    refresh();
}

void SelectionPanel::togglePick()
{
    if (app.isClonePicking ())
        app.endClonePick ();
    else
        app.startClonePick ();   // fires onStateChange -> syncSharedState -> refresh()
}

// ------------------------------------------------- Python script panel

PythonPanel::PythonPanel (Application& appIn, const juce::File& folder)
    : app (appIn), scriptDir (folder.getFullPathName ().isEmpty () ? sc::py::scriptFolder() : folder)
{
    sc::py::ensureExampleScripts (scriptDir);   // creates the folder + examples

    addAndMakeVisible (&scriptBox);
    addAndMakeVisible (&addBtn);
    addAndMakeVisible (&removeBtn);
    addAndMakeVisible (&runBtn);

    editor.setMultiLine (true);
    editor.setScrollbarsShown (true);
    editor.setFont (juce::Font ("Consolas", 13.0f, juce::Font::plain));
    addAndMakeVisible (&editor);

    scriptBox.onChange = [this] { loadCurrentScript (); };
    addBtn.onClick     = [this] { showAddDialog (); };
    removeBtn.onClick  = [this] { confirmRemove (); };
    runBtn.onClick     = [this] { requestRun (); };

    refreshScripts ();
}

// Right-click on empty panel area: offer to close the panel.
void PythonPanel::mouseDown (const juce::MouseEvent& e)
{
    if (!e.mods.isRightButtonDown()) return;
    if (showClosePanelMenu() && onCloseRequested) onCloseRequested();
}

void PythonPanel::refreshScripts ()
{
    const juce::String keepPath = currentIdx >= 0 && currentIdx < (int) scripts.size ()
                                       ? scripts[(size_t) currentIdx].getFullPathName() : "";

    scripts = sc::py::listScripts (scriptDir);
    currentIdx = -1;
    for (int i = 0; i < (int) scripts.size (); ++i)
        if (scripts[(size_t) i].getFullPathName () == keepPath) { currentIdx = i; break; }

    scriptBox.clear (juce::dontSendNotification);
    for (int i = 0; i < (int) scripts.size (); ++i)
        scriptBox.addItem (scripts[(size_t) i].getFileName (), i + 1);
    if (currentIdx >= 0)   // setSelectedItemIndex takes a 0-based position
        scriptBox.setSelectedItemIndex (currentIdx, juce::dontSendNotification);

    loadCurrentScript ();
}

void PythonPanel::loadCurrentScript ()
{
    const int idx = scriptBox.getSelectedId () - 1;
    if (idx < 0 || idx >= (int) scripts.size ())
    {
        editor.setText ("", juce::dontSendNotification);
        return;
    }

    currentIdx = idx;
    editor.setText (scripts[(size_t) idx].loadFileAsString (),
                    juce::dontSendNotification);
}

void PythonPanel::showAddDialog ()
{
    auto* w = new juce::AlertWindow ("New Script", "Script name:",
                                     juce::AlertWindow::NoIcon, this);
    w->setColour (juce::AlertWindow::backgroundColourId,
                 juce::Colour::fromRGB (45, 45, 48));
    w->addTextEditor ("name", "NewScript");
    auto* nameEdit = w->getTextEditor ("name");
    w->addButton ("Create", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    if (nameEdit != nullptr)
    {
        nameEdit->setWantsKeyboardFocus (true);
        nameEdit->selectAll();
        nameEdit->grabKeyboardFocus();
    }

    const int result = w->runModalLoop ();
    delete w;
    if (result != 1 || nameEdit == nullptr) return;

    // Sanitise: strip path separators / invalid characters, force a .py suffix.
    juce::String name = nameEdit->getText ().trim ();
    for (int i = 0; i < name.length (); ++i)
    {
        const juce_wchar c = name[i];
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9')
            && c != ' ' && c != '_' && c != '-')
            name.replaceCharacter (c, '_');
    }
    name = name.trim ();
    if (name.isEmpty()) name = "NewScript";
    if (!name.endsWithIgnoreCase (".py")) name << ".py";

    const juce::File f = scriptDir.getChildFile (name);
    if (f.existsAsFile ())
    {
        auto* err = new juce::AlertWindow ("New Script",
                                           "'" + name + "' already exists.",
                                           juce::AlertWindow::WarningIcon, this);
        err->setColour (juce::AlertWindow::backgroundColourId,
                        juce::Colour::fromRGB (45, 45, 48));
        err->addButton ("OK", 1);
        err->enterModalState ();
        delete err;
        return;
    }

    f.replaceWithText (sc::py::newScriptTemplate ());
    refreshScripts ();

    for (int i = 0; i < (int) scripts.size (); ++i)
        if (scripts[(size_t) i].getFullPathName () == f.getFullPathName ())
        {
            currentIdx = i;
            scriptBox.setSelectedItemIndex (i + 1, juce::NotificationType::dontSendNotification);
            loadCurrentScript ();
            break;
        }
}

void PythonPanel::confirmRemove ()
{
    if (currentIdx < 0 || currentIdx >= (int) scripts.size ()) return;
    const juce::File f = scripts[(size_t) currentIdx];

    auto* w = new juce::AlertWindow ("Delete Script",
                                     "Delete '" + f.getFileName () + "'?\nThis cannot be undone.",
                                     juce::AlertWindow::QuestionIcon, this);
    w->setColour (juce::AlertWindow::backgroundColourId,
                 juce::Colour::fromRGB (45, 45, 48));
    w->addButton ("Delete", 1);
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    const int result = w->runModalLoop ();
    delete w;

    if (result != 1) return;
    f.deleteFile ();
    refreshScripts ();
}

void PythonPanel::requestRun ()
{
    if (!app.audioEngine ().isFileLoaded ())
    {
        auto* info = new juce::AlertWindow ("Python", "Load an audio file first.",
                                            juce::AlertWindow::InfoIcon, this);
        info->setColour (juce::AlertWindow::backgroundColourId,
                         juce::Colour::fromRGB (45, 45, 48));
        info->addButton ("OK", 1);
        info->enterModalState ();
        delete info;
        return;
    }

    if (currentIdx < 0 || currentIdx >= (int) scripts.size ())
    {
        auto* info = new juce::AlertWindow ("Python", "Select a script first.",
                                            juce::AlertWindow::InfoIcon, this);
        info->setColour (juce::AlertWindow::backgroundColourId,
                         juce::Colour::fromRGB (45, 45, 48));
        info->addButton ("OK", 1);
        info->enterModalState ();
        delete info;
        return;
    }

    if (onRunRequested != nullptr)
        onRunRequested (scripts[(size_t) currentIdx], editor.getText ());
}

void PythonPanel::resized ()
{
    auto b = getLocalBounds ().reduced (4, 4);

    scriptBox.setBounds (b.removeFromTop (26));

    const int btnY = b.getY ();
    addBtn.setBounds (b.getX (), btnY + 2, 50, 24);
    removeBtn.setBounds (addBtn.getRight () + 8, btnY + 2, 70, 24);
    runBtn.setBounds (removeBtn.getRight () + 8, btnY + 2, 56, 24);
    b.removeFromTop (30);

    editor.setBounds (b);
}
