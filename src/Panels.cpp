/**
 * SpectroCraft - Left-side panels implementation
 */

#include "Panels.h"
#include "AlertGate.h"
#include "Diagnostics.h"
#include "WaveformView.h"   // formatFooterTime / parseTimeText (shared time text)
#include "FFTHelpers.h"
#include "OwningAlertWindow.h"   // dialogs with new-ed custom components
#include "WorkProgress.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW (Open Containing Folder)
#endif

// This vendored JUCE's SparseSet has no begin()/end(): expand its ranges
// into a plain vector so panels can iterate their selected rows.
static std::vector<int> sparseRows (const juce::SparseSet<int>& s)
{
    std::vector<int> out;
    for (int i = 0; i < s.getNumRanges (); ++i)
        for (int r = s.getRange (i).getStart (); r < s.getRange (i).getEnd (); ++r)
            out.push_back (r);
    return out;
}

// Forward declarations (defined further down, used by the constructor).
static void fftAxisLimits (int mode, double rate, bool pascal20k, double& fLo, double& fHi);
static int syncSmoothValue (int sm);
static int syncThirdOctOrderLocal (int v);
static void showSelectionMessage (const juce::String& title, const juce::String& msg);

juce::String formatTimeCode (double secs)
{
    if (secs < 0) secs = 0;
    const int m = (int) (secs / 60.0);
    const double s = secs - m * 60.0;
    return juce::String (m) + ":" + juce::String (s, 3).paddedLeft ('0', 6);
}

static double parseTimeCode (const juce::String& text)
{
    // Accepts "12", "12.5", "0:04.536", "1:02", "1:02:03" (the shared time
    // parser); anything else - a header row, stray text - yields -1 so the
    // caller skips it instead of reading it as 0 s / mis-reading h:mm:ss.
    double v = 0.0;
    return parseTimeText (text, v) ? v : -1.0;
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
        // Closes the ACTIVE file (same index Application::closeFile picks);
        // Application fires the callbacks that refresh this list and reset
        // the views to whatever remains.
        if (app.numDocs() > 0)
            closeDoc ((app.activeDocIndex >= 0 && app.activeDocIndex < app.numDocs())
                          ? app.activeDocIndex : 0);
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
    // A background job's worker still uses the documents (it deletes its
    // progress dialog when it is done).
    if (sc::WorkProgressDialog::numActive () > 0) return;
    if (i < 0 || i >= app.numDocs()) return;

    // Same prompt as File > Close (MainWindow::handleCloseFile): never drop
    // unsaved edits or metadata without asking.
    const FileDoc& d = *app.docs[(size_t) i];
    if (app.docIsUnsaved (d))
    {
        const juce::String name = d.name.isNotEmpty () ? d.name : d.file.getFileName ();
        juce::AlertWindow dlg ("Close File",
            "This file has unsaved changes:\n\n" + name +
            "\n\nDo you want to close it and discard the changes?",
            juce::AlertWindow::WarningIcon, this);
        dlg.addButton ("Close", 1);
        dlg.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        if (dlg.runModalLoop () != 1) return;   // keep it open
    }
    if (i < app.numDocs())
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
   #if JUCE_MAC
    const juce::String opener ("open ");
   #else
    const juce::String opener ("xdg-open ");
   #endif
    const juce::String cmd = opener + "\""
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
    const int r = m.showMenu (juce::PopupMenu::Options().withMousePosition());
    if (r == 1) activateDoc (row);
    else if (r == 2) closeDoc (row);
    else if (r == 3) showScaleDialog (row);
    else if (r == 4) openContainingFolder (row);
}

// Field name for channel ch's full-scale editor in the scale dialog.
static juce::String scaleFieldId (int ch) { return juce::String ("fs") + juce::String (ch); }

bool FileListPanel::applyScaleValues (FileDoc& d, ScaleCalibration::Unit unit,
                                      const std::vector<double>& vals)
{
    if (unit == ScaleCalibration::Unit::None || vals.empty())
    {
        d.scale.unit = unit;
        d.scale.fullScaleValue = 0.0;
        d.scale.channelFullScale.clear();
        return true;
    }

    for (const double v : vals)
        if (!(v > 0.0)) return false;   // caller shows the error dialog

    bool same = true;
    for (size_t i = 1; i < vals.size() && same; ++i)
        if (vals[i] != vals[0]) same = false;

    d.scale.unit = unit;
    d.scale.fullScaleValue = vals[0];
    d.scale.channelFullScale.clear();
    if (!same && vals.size() > 1)
        d.scale.channelFullScale = vals;   // per-channel full-scale values
    return true;
}

juce::AlertWindow* FileListPanel::createScaleDialog (FileDoc& d)
{
    auto* dlg = new juce::AlertWindow (
        "Set Unit and Scale",
        "Calibrate the physical units of this file.\n"
        "Full scale (|1.0|) equals the value you enter below.\n"
        "Each channel can have its own full-scale value.",
        juce::AlertWindow::NoIcon);
    dlg->setColour (juce::AlertWindow::backgroundColourId,
                   juce::Colour::fromRGB (45, 45, 48));

    // One shared unit for all channels...
    dlg->addComboBox ("unit", { "None", "Volts", "Pascal", "g" }, "Units:");
    auto* unitCombo = dlg->getComboBoxComponent ("unit");

    // ...and one full-scale field per channel.
    const int nch = std::max (1, d.engine.numChannels());
    for (int ch = 0; ch < nch; ++ch)
        dlg->addTextEditor (
            scaleFieldId (ch),
            d.scale.fullScaleFor (ch) > 0.0 ? juce::String (d.scale.fullScaleFor (ch)) : "1.0",
            "Full scale ch" + juce::String (ch + 1) + ":");

    if (unitCombo == nullptr || dlg->getTextEditor (scaleFieldId (0)) == nullptr)
    {
        delete dlg;
        return nullptr;
    }

    // Item ids are 1..4 in the same order as ScaleCalibration::Unit (0..3).
    unitCombo->setSelectedId ((int) d.scale.unit + 1,
                             juce::NotificationType::dontSendNotification);

    // The value fields only make sense when a unit is selected.
    auto syncEnabled = [dlg, unitCombo] {
        const bool on = (unitCombo->getSelectedId() != 1);
        for (int ch = 0; ; ++ch)
        {
            juce::TextEditor* e = dlg->getTextEditor (scaleFieldId (ch));
            if (e == nullptr) break;
            e->setEnabled (on);
        }
    };
    unitCombo->onChange = syncEnabled;
    syncEnabled();

    dlg->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dlg->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    // Pre-select the first field's text so a new value can be typed in
    // immediately. AlertWindow disables keyboard focus on its editors, so
    // re-enable it first or a focus grab would bounce to a button. The actual
    // grabKeyboardFocus() happens in showScaleDialog() once the window is
    // showing: JUCE ignores (and asserts on) focus grabs for components that
    // are not on screen yet.
    auto* valEdit = dlg->getTextEditor (scaleFieldId (0));
    valEdit->setWantsKeyboardFocus (true);
    valEdit->selectAll();

    return dlg;
}

void FileListPanel::showScaleDialog (int row)
{
    if (row < 0 || row >= app.numDocs()) return;
    FileDoc& d = *app.docs[(size_t) row];

    auto* dlg = createScaleDialog (d);
    if (dlg == nullptr) return;

    // Enter modal state WITHOUT taking keyboard focus (runModalLoop() would
    // otherwise grab it for the window itself), then focus the first enabled
    // field now that the window is showing - or the unit combo while the unit
    // is None and the value fields are disabled.
    dlg->enterModalState (false);
    if (auto* valEdit = dlg->getTextEditor (scaleFieldId (0)); valEdit != nullptr && valEdit->isEnabled())
        valEdit->grabKeyboardFocus();
    else if (auto* combo = dlg->getComboBoxComponent ("unit"))
        combo->grabKeyboardFocus();

    const int result = dlg->runModalLoop();
    juce::ComboBox* unitCombo = dlg->getComboBoxComponent ("unit");

    if (result == 1 && unitCombo != nullptr)
    {
        const auto unit = (ScaleCalibration::Unit) (unitCombo->getSelectedId() - 1);
        const int nch = std::max (1, d.engine.numChannels());
        std::vector<double> vals ((size_t) nch);
        for (int ch = 0; ch < nch; ++ch)
        {
            juce::TextEditor* e = dlg->getTextEditor (scaleFieldId (ch));
            vals[(size_t) ch] = (e != nullptr) ? e->getText().trim().getDoubleValue() : -1.0;
        }

        if (applyScaleValues (d, unit, vals))
        {
            refresh();   // file list shows the new scale under the entry
            if (app.onStateChange) app.onStateChange();   // repaint views with the new units
        }
        else
        {
            juce::AlertWindow err ("Set Unit and Scale",
                                   "Please enter a positive value for every channel's full-scale level.",
                                   juce::AlertWindow::NoIcon);
            err.setColour (juce::AlertWindow::backgroundColourId,
                           juce::Colour::fromRGB (45, 45, 48));
            err.addButton ("OK", 1);
            err.runModalLoop();
        }
    }
    delete dlg;
}

void FileListPanel::mouseDown (const juce::MouseEvent& e)
{
    if (!e.mods.isRightButtonDown()) return;
    const int row = rowAt ((int) e.x, (int) e.y);
    if (row < 0) return;   // empty area below the list
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

// The spectrum, 1/3-octave and curve computations live in the headless
// measurement module (include/Measures.h) so the panel and the measure.*
// commands share one implementation; the panel only adds caching, the region
// taken from the selection and the drawing.
namespace measures = sc::measures;

// Grab-and-drag pans at half cursor speed, like the waveform/spectral views
// (kPanSpeedFactor there): content follows the cursor at 0.5 px per cursor
// pixel so a wide window stays controllable.
static constexpr double kPanSpeedFactor = 0.5;

const double* FFTAnalysisView::thirdOctCentres() noexcept { return measures::iecThirdOctCentres(); }
int FFTAnalysisView::numThirdOctBands() noexcept { return measures::numIecThirdOctBands(); }

const double* FFTAnalysisView::thirdOptCentresRef() noexcept { return measures::refThirdOctCentres(); }
int FFTAnalysisView::numThirdOptBandsRef() noexcept { return measures::numRefThirdOctBands(); }

bool FFTAnalysisView::pascalThirdOct() const
{
    return axisMode_ == 2 && app.scale().unit == ScaleCalibration::Unit::Pascal;
}

const double* FFTAnalysisView::bandCentres() const
{
    return pascalThirdOct() ? measures::refThirdOctCentres() : measures::iecThirdOctCentres();
}

int FFTAnalysisView::numBands() const
{
    return pascalThirdOct() ? measures::numRefThirdOctBands() : measures::numIecThirdOctBands();
}

std::vector<float> FFTAnalysisView::thirdOctBandsRef (const AudioEngine& eng, int ch,
                                                      double t0, double t1, int order)
{
    // Reference-analyzer pipeline, unchanged, now in the measurement module.
    return measures::thirdOctaveRefBands (eng, ch, t0, t1, order);
}

// Only a subset of band centres gets an axis label (every ~3rd band), so the
// 26 grid lines do not crowd the bottom axis.
static bool isThirdOctLabelFreq (double f)
{
    // A small relative tolerance so the decade-based dB-SPL centres (50.12,
    // 7943.3, ...) still match their rounded labels.
    for (double c : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        if (std::abs (f - c) < std::max (1e-6, c * 0.005))
            return true;
    return false;
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
        // Only edits made through AI control get a detail (time + client):
        // the tab looks exactly as before for edits made in the window.
        auto detailOf = [] (const Patch& p) {
            if (p.timestampMs <= 0 || p.source.isEmpty() || p.source == "menu") return juce::String();
            return juce::Time (p.timestampMs).formatted ("%H:%M:%S") + "  " + p.source;
        };
        rows_.push_back ({ "Loaded " + d->name, false, {} });
        for (size_t i = 0; i < u.undoSize(); ++i)
            rows_.push_back ({ u.undoAt(i).description.isEmpty() ? juce::String ("Edit") : u.undoAt(i).description,
                               false, detailOf (u.undoAt (i)) });
        // Future rows in redo order: the redo stack's back() is the next redo,
        // so a click on the first future row (redoSteps(1)) re-applies the
        // entry that row names. (Listing redoAt(0..n) showed them reversed.)
        for (size_t k = 0; k < u.redoSize(); ++k)
        {
            const Patch& p = u.redoAt (u.redoSize() - 1 - k);
            rows_.push_back ({ p.description.isEmpty() ? juce::String ("Edit") : p.description,
                               true, detailOf (p) });
        }
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
    // The right-click "Close Panel" menu is gone; panels close from the tab
    // menu or the Window menu.
    if (e.mods.isRightButtonDown()) return;
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

// The Step column width inside the list area; the step number is drawn
// right-aligned in it and the action text starts just after.
static constexpr int kHistStepColW = 36;

void HistoryPanel::paint (juce::Graphics& g)
{
    auto b = getLocalBounds();
    // Same table style as the Actions panel's TableListBox under the default
    // LookAndFeel_V4 dark scheme: body = widgetBackground RGB(38,50,56) and a
    // header of 60% white over it (light grey) with black bold text and a
    // half-black bottom outline.
    g.fillAll (juce::Colour::fromRGB (38, 50, 56));

    // Header row with the two column titles; applied-count stays right.
    g.setColour (juce::Colours::white.withAlpha (0.6f));
    g.fillRect (0, 0, b.getWidth(), kHeaderH);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillRect (0, kHeaderH - 1, b.getWidth(), 1);
    g.setFont (juce::Font (12.0f, juce::Font::bold));
    g.setColour (juce::Colours::black);
    const int listX = 2;   // matches listArea()
    g.drawText ("Step", juce::Rectangle<int> (listX + 2, 3, kHistStepColW, kHeaderH - 6),
                juce::Justification::centredRight, false);
    g.drawText ("Action", juce::Rectangle<int> (listX + kHistStepColW + 8, 3,
                                                std::max (1, b.getWidth() - listX - kHistStepColW - 176),
                                                kHeaderH - 6),
                juce::Justification::left, false);

    const int totalActions = std::max (0, (int) rows_.size() - 1);
    g.setFont (juce::Font (11.0f, juce::Font::bold));
    g.setColour (juce::Colours::black);
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
            // Same selection colour as the Actions/marker table rows.
            g.setColour (juce::Colour::fromRGB (60, 80, 120));
            g.fillRect (r);
        }

        // Past rows are bright, future (redoable) ones dimmed.
        g.setColour (rows_[(size_t) i].future ? juce::Colour::fromRGB (130, 138, 152)
                                               : juce::Colours::white);

        // Step number in its own column, action text after it.
        auto stepCol = juce::Rectangle<int> (r.getX() + 2, r.getY(),
                                             kHistStepColW, r.getHeight());
        g.drawText (juce::String (i), stepCol, juce::Justification::centredRight,
                    false);
        auto actCol = juce::Rectangle<int> (stepCol.getRight() + 6, r.getY(),
                                            std::max (1, r.getRight() - stepCol.getRight() - 8),
                                            r.getHeight());
        g.drawText (rows_[(size_t) i].text, actCol, juce::Justification::centredLeft,
                    false);
        // Time (and origin, for edits made through AI control) on the right.
        if (rows_[(size_t) i].detail.isNotEmpty() && actCol.getWidth() > 220)
        {
            g.setColour (juce::Colour::fromRGB (150, 160, 175));
            g.drawText (rows_[(size_t) i].detail, actCol.withTrimmedLeft (actCol.getWidth() - 200),
                        juce::Justification::centredRight, false);
        }
    }
}

FFTAnalysisView::FFTAnalysisView (Application& appIn)
    : app (appIn)
{
    // Restore the persisted frequency-analysis state (axis mode + FFT size
    // + curve-mode options).
    axisMode_ = std::clamp (app.settings.fftAxisMode, 0, 5);
    refTime_ = std::clamp (app.settings.fftRefTime, 0, 1);
    smooth_ = syncSmoothValue (app.settings.fftSmooth);
    track_ = std::clamp (app.settings.fftTrack, 0, 2);

    for (int n = 1024; n <= 65536; n *= 2)
        fftSizeCombo.addItem (juce::String (n), n);
    localN = std::clamp (app.settings.fftLocalN, 1024, 65536);
    fftSizeCombo.setSelectedId (localN, juce::dontSendNotification);
    fftSizeCombo.onChange = [this] {
        // Independent from the spectrogram's FFT size.
        localN = fftSizeCombo.getSelectedId();
        app.settings.fftLocalN = localN;   // persisted between sessions
        app.pushActiveDocMeta();           // ...and with this file's metadata
        settingsGen++;               // stored snapshots must be recomputed too
        mags.clear(); cachedN = 0;   // invalidate cached spectrum
        recomputeSnapshots();
        recomputeCurveSlots();
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
        recomputeCurveSlots();
        repaint();
    };

    // Octave order of the dB-SPL bandpass: only visible in 1/3-octave mode
    // when a Pascal full-scale value is set. Values mirror the reference
    // analyzer's Order parameter (the filter itself is 2*order).
    thirdOctOrder_ = syncThirdOctOrderLocal (app.settings.fftThirdOctOrder);
    static const int kOrderValues[9] = { 1, 2, 3, 4, 5, 10, 20, 30, 40 };
    for (int i = 0; i < 9; ++i)
        orderCombo.addItem (juce::String (kOrderValues[i]), i + 1);
    {
        int id = 9;   // default: 40
        for (int i = 0; i < 9; ++i)
            if (thirdOctOrder_ == kOrderValues[i]) { id = i + 1; break; }
        orderCombo.setSelectedId (id, juce::dontSendNotification);
    }
    orderCombo.onChange = [this] {
        static const int kOrd[9] = { 1, 2, 3, 4, 5, 10, 20, 30, 40 };
        thirdOctOrder_ = (orderCombo.getSelectedId() >= 1 && orderCombo.getSelectedId() <= 9)
                         ? kOrd[orderCombo.getSelectedId() - 1] : 40;
        app.settings.fftThirdOctOrder = thirdOctOrder_;   // persisted between sessions
        app.pushActiveDocMeta();           // ...and with this file's metadata
        settingsGen++;               // stored snapshots must be recomputed too
        mags.clear(); cachedN = 0;   // invalidate the live cache (both pipelines)
        thirdOctMags.clear();      // ...and the band cache (order-dependent)
        recomputeSnapshots();
        if (app.onStateChange) app.onStateChange();   // triggers the settings save
        repaint();
    };

    axisCombo.addItem ("FFT Linear", 1);
    axisCombo.addItem ("FFT Log", 2);
    axisCombo.addItem ("1/3 Octave", 3);
    axisCombo.addItem ("Freq vs Time", 4);
    axisCombo.addItem ("Amp vs Time", 5);
    axisCombo.addItem ("Amp vs Freq", 6);
    axisCombo.setSelectedId (axisMode_ + 1, juce::dontSendNotification);
    axisCombo.onChange = [this] {
        axisMode_ = axisCombo.getSelectedId() - 1;
        app.settings.fftAxisMode = axisMode_;   // persisted between sessions
        app.pushActiveDocMeta();           // ...and with this file's metadata
        thirdOctMags.clear();   // band cache is only valid for mode 2
        liveCurves.clear();     // curve cache belongs to the previous mode
        cachedCurveMode = -1;
        if (harmonicRatioActive_) { harmonicRatioActive_ = false; invalidateHarmonicCache(); }
        refreshSnapshotBands(); // re-derive stored snapshots' bands for the new mode
        refreshSnapshotButtonStates();   // buttons now address the other store
        // Auto zoom-fit resets to on for every new mode.
        zoomFitAuto_ = true;
        autoFitT0_ = -1.0, autoFitT1_ = -1.0;
        fitPending_ = true;    // zoom-fit once the new mode's data is computed
                               // (paint() runs resetDisplayRanges then)
        resized();              // row 1/row 3 contents differ per mode
        if (app.onStateChange) app.onStateChange();   // triggers the settings save
    };

    // Curve-mode options. Ref Time only applies to the time-based modes
    // (Freq vs Time / Amp vs Time); Smoothing and Track apply to all three.
    refTimeCombo.addItem ("Relative", 1);
    refTimeCombo.addItem ("Absolute", 2);
    refTimeCombo.setSelectedId (refTime_ + 1, juce::dontSendNotification);
    refTimeCombo.onChange = [this] {
        refTime_ = refTimeCombo.getSelectedId() - 1;
        app.settings.fftRefTime = refTime_;   // persisted between sessions
        app.pushActiveDocMeta();           // ...and with this file's metadata
        if (app.onStateChange) app.onStateChange();
        repaint();
    };

    static const int kSmoothValues[8] = { 0, 3, 5, 7, 9, 11, 21, 51 };
    smoothCombo.addItem ("None", 1);
    smoothCombo.addItem ("3 Frames", 2);
    smoothCombo.addItem ("5 Frames", 3);
    smoothCombo.addItem ("7 Frames", 4);
    smoothCombo.addItem ("9 Frames", 5);
    smoothCombo.addItem ("11 Frames", 6);
    smoothCombo.addItem ("21 Frames", 7);
    smoothCombo.addItem ("51 Frames", 8);
    {
        int id = 1;
        for (int i = 0; i < 8; ++i)
            if (smooth_ == kSmoothValues[i]) { id = i + 1; break; }
        smoothCombo.setSelectedId (id, juce::dontSendNotification);
    }
    smoothCombo.onChange = [this] {
        static const int vals[8] = { 0, 3, 5, 7, 9, 11, 21, 51 };
        const int id = smoothCombo.getSelectedId();
        smooth_ = (id >= 1 && id <= 8) ? vals[id - 1] : 0;
        app.settings.fftSmooth = smooth_;   // persisted between sessions
        app.pushActiveDocMeta();           // ...and with this file's metadata
        cachedCurveSmooth = -1;             // live curve must be re-smoothed
        settingsGen++;                      // stored curves follow the current options
        recomputeCurveSlots();
        if (app.onStateChange) app.onStateChange();
        repaint();
    };

    trackCombo.addItem ("Max", 1);
    trackCombo.addItem ("Total", 2);
    trackCombo.addItem ("Weighted", 3);
    trackCombo.setSelectedId (track_ + 1, juce::dontSendNotification);
    trackCombo.onChange = [this] {
        track_ = trackCombo.getSelectedId() - 1;
        app.settings.fftTrack = track_;   // persisted between sessions
        app.pushActiveDocMeta();           // ...and with this file's metadata
        cachedCurveTrack = -1;            // live curve must be re-tracked
        settingsGen++;                    // stored curves follow the current options
        recomputeCurveSlots();
        if (app.onStateChange) app.onStateChange();
        repaint();
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
    axisCombo.setTooltip ("Frequency axis mode: FFT Linear (linear Hz), FFT Log (logarithmic), "
                          "1/3 Octave (IEC 61260-1 band plot from 50 Hz to 16 kHz), "
                          "Freq vs Time / Amp vs Time (curves over the selection, honouring a "
                          "spectral-selection mask) or Amp vs Freq (per-bin spectrum of the selection).");
    refTimeCombo.setTooltip ("Time scale for the time-based modes: Relative starts at the selection "
                             "start; Absolute uses the file's absolute time.");
    smoothCombo.setTooltip ("Moving-average smoothing applied to the plotted curve: None, 3 or 5 frames.");
    trackCombo.setTooltip ("Max tracks the strongest component per frame/bin (its level and frequency); "
                           "Total tracks the summed signal level and the amplitude-weighted centroid frequency.");

    orderCombo.setTooltip ("Butterworth bandpass order for the dB-SPL 1/3-octave pipeline "
                           "(visible when a Pascal full-scale value is set): higher orders give "
                           "sharper, more selective bands.");
    addAndMakeVisible (&fftSizeCombo);
    addAndMakeVisible (&windowCombo);
    addAndMakeVisible (&orderCombo);
    addAndMakeVisible (&axisCombo);
    addAndMakeVisible (&refTimeCombo);
    addAndMakeVisible (&smoothCombo);
    addAndMakeVisible (&trackCombo);
    for (auto& b : snapBtns) addAndMakeVisible (&b);

    resetDisplayRanges();   // start at the mode defaults for the current file
}

// True when the live curve cache matches the current mode/region/settings
// (same conditions recomputeCurve uses to skip its work).
bool FFTAnalysisView::fitDataReady() const
{
    if (!app.audioEngine().isFileLoaded()) return false;
    if (axisMode_ >= 3) return curveCacheValid ();
    if (axisMode_ == 2) return !thirdOctMags.empty ();
    return cachedN > 1 && !mags.empty ();
}

// Fingerprint of a spectral-selection mask (0 = no mask): part of the curve
// cache key, so redrawing the selection over the same time region recomputes.
static uint64_t spectralMaskHash (const SpectralSelection* sel)
{
    if (sel == nullptr || sel->empty()) return 0;
    uint64_t h = 1469598103934665603ull;   // FNV-1a
    auto mix = [&h] (const void* p, size_t n)
    {
        const auto* b = static_cast<const uint8_t*> (p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    auto mixD = [&mix] (double v) { mix (&v, sizeof (v)); };
    for (const auto& sh : sel->shapes())
    {
        const int head[4] = { (int) sh.kind, sh.subtract ? 1 : 0, sh.cols, sh.rows };
        mix (head, sizeof (head));
        mixD (sh.t0); mixD (sh.t1); mixD (sh.f0); mixD (sh.f1); mixD (sh.strokeRadiusPct);
        for (const auto& pt : sh.pts) { mixD (pt.first); mixD (pt.second); }
        if (!sh.grid.empty()) mix (sh.grid.data(), sh.grid.size());
    }
    return h == 0 ? 1 : h;
}

std::vector<int> FFTAnalysisView::liveChannels() const
{
    const auto& eng = app.audioEngine();
    std::vector<int> out;
    for (int ch : app.visibleChannels)
        if (ch >= 0 && ch < eng.numChannels())
            out.push_back (ch);
    return out;
}

double FFTAnalysisView::currentEnbwBins (int n) const
{
    return measures::enbwBins (windowCombo.getSelectedId() - 1, n);
}

int FFTAnalysisView::levelAxisChannel() const
{
    const auto& chans = (axisMode_ >= 3) ? cachedCurveChans_ : cachedChans_;
    if (!chans.empty()) return chans.front();
    return app.visibleChannels.empty() ? -1 : app.visibleChannels.front();
}

bool FFTAnalysisView::curveCacheValid() const
{
    if (!app.audioEngine().isFileLoaded()) return false;
    double t0, t1;
    if (!app.selection.isEmpty()) { t0 = app.selection.getStart(); t1 = app.selection.getEnd(); }
    else                          { t0 = app.playhead;          t1 = app.playhead; }
    const auto& eng = app.audioEngine();
    const FileDoc* d = app.doc();
    return !liveCurves.empty() && cachedCurveMode == axisMode_
        && cachedCurveTrack == track_ && cachedCurveSmooth == smooth_
        && cachedT0 == t0 && cachedT1 == t1
        && cachedRate == eng.sampleRate() && cachedFile == eng.getFile()
        && cachedEngine_ == &eng && cachedContentVer_ == eng.contentVersion()
        && cachedCurveChans_ == liveChannels()
        && cachedMaskHash_ == spectralMaskHash (d != nullptr ? &d->spectralSel : nullptr);
}

// Fit [lo, hi] to a data range with a total margin of +10% (5% above the max,
// 5% below the min). Degenerate spans are expanded to at least 10% of |centre|
// so flat data cannot collapse the axis.
static void fitDataRange (double dmin, double dmax, double& lo, double& hi)
{
    const double centre = 0.5 * (dmin + dmax);
    double span = std::max (1e-9, dmax - dmin);
    const double minSpan = std::max (1e-6, std::abs (centre) * 0.1);
    if (span < minSpan)
        { dmin = centre - 0.5 * minSpan; dmax = centre + 0.5 * minSpan; span = minSpan; }
    const double m = span * 0.05;
    lo = dmin - m;
    hi = dmax + m;
}

// Fit the vertical axis to the currently displayed data (+10% margin): dB level
// in every mode except Freq vs Time, where it is frequency. Falls back to the
// mode defaults when there is no (valid) data yet.
void FFTAnalysisView::fitVerticalRange()
{
    if (axisMode_ == 3)
    {
        if (harmonicRatioUsable())
        {
            // Fit the ratio window to every displayed transformed curve
            // (live + shown stored), not just the live one.
            double dmin = std::numeric_limits<double>::max();
            double dmax = -std::numeric_limits<double>::max();
            auto consider = [&] (const CurveData& cd)
            {
                for (float r : cd.freqHz)
                    if (std::isfinite (r) && r > 0.0)
                        { dmin = std::min (dmin, (double) r); dmax = std::max (dmax, (double) r); }
            };
            for (const auto& cd : harmonicCache_.live) consider (cd);
            for (int i = 0; i < 8; ++i)
                for (const auto& cd : harmonicCache_.snaps[i]) consider (cd);
            if (dmax > dmin - 1e-9)
                fitDataRange (dmin, dmax, fLo_, fHi_);
            else
                { fLo_ = 0.5; fHi_ = 2.5; }   // sensible default ratio window
            return;
        }

        double dmin = std::numeric_limits<double>::max();
        double dmax = -std::numeric_limits<double>::max();
        for (const auto& cd : liveCurves)
            for (float f : cd.freqHz)
                { dmin = std::min (dmin, (double) f); dmax = std::max (dmax, (double) f); }
        // Include every displayed stored curve as well.
        for (int i = 0; i < 8; ++i)
            if (curveSnaps[i].shown && curveSnaps[i].mode == axisMode_)
                for (const auto& cd : curveSnaps[i].data)
                    for (float f : cd.freqHz)
                        { dmin = std::min (dmin, (double) f); dmax = std::max (dmax, (double) f); }
        if (dmax > dmin - 1e-9)
            fitDataRange (dmin, dmax, fLo_, fHi_);
        else
        {
            const double rate = app.audioEngine().sampleRate();
            fftAxisLimits (axisMode_, rate, pascalThirdOct(), fLo_, fHi_);
        }
    }
    else
    {
        bool have = false;
        double dmin = 0.0, dmax = -kNormFloorDb;
        if (axisMode_ == 2)
        {
            for (const auto& bands : thirdOctMags)
                for (float v : bands)
                    { dmin = std::min (dmin, (double) v); dmax = std::max (dmax, (double) v); }
            // Include every displayed stored snapshot's band curves.
            for (int i = 0; i < 8; ++i)
                if (snapshots[i].shown && !snapshots[i].bands.empty())
                    for (const auto& bands : snapshots[i].bands)
                        for (float v : bands)
                            { dmin = std::min (dmin, (double) v); dmax = std::max (dmax, (double) v); }
            have = !thirdOctMags.empty();
        }
        else if (axisMode_ >= 4)
        {
            // Amp vs Time / Amp vs Freq: level data lives in the live curves.
            for (const auto& cd : liveCurves)
                for (float v : cd.ampDb)
                    { dmin = std::min (dmin, (double) v); dmax = std::max (dmax, (double) v); }
            // Include every displayed stored curve as well.
            for (int i = 0; i < 8; ++i)
                if (curveSnaps[i].shown && curveSnaps[i].mode == axisMode_)
                    for (const auto& cd : curveSnaps[i].data)
                        for (float v : cd.ampDb)
                            { dmin = std::min (dmin, (double) v); dmax = std::max (dmax, (double) v); }
            have = curveCacheValid();
        }
        else if (!mags.empty() && cachedN > 1)
        {
            for (const auto& mag : mags)
                for (int k = 1; k < (int) mag.size(); ++k)
                {
                    const double dB = -kNormFloorDb * (1.0 - (double) mag[k]);
                    dmin = std::min (dmin, dB);
                    dmax = std::max (dmax, dB);
                }
            // Include every displayed stored snapshot as well.
            for (int i = 0; i < 8; ++i)
                if (snapshots[i].shown && snapshots[i].n > 1 && !snapshots[i].mags.empty())
                    for (const auto& mag : snapshots[i].mags)
                        for (int k = 1; k < (int) mag.size(); ++k)
                        {
                            const double dB = -kNormFloorDb * (1.0 - (double) mag[k]);
                            dmin = std::min (dmin, dB);
                            dmax = std::max (dmax, dB);
                        }
            have = true;
        }
        if (have) fitDataRange (dmin, dmax, dbBottom_, dbTop_);
        else      { dbTop_ = 0.0; dbBottom_ = -80.0; }
    }
}

// Fit the horizontal axis to the full extent of the data: the analysed region
// in the time-based modes, otherwise the frequency range actually covered by
// the plotted points (mode defaults when there is no data yet).
void FFTAnalysisView::fitHorizontalRange()
{
    if (isTimeAxisMode())
    {
        // No analysed region yet: nothing to fit.
        if (!haveCurveView_) return;
        curveViewT0_ = cachedT0;
        curveViewT1_ = std::max (cachedT1, cachedT0 + 1e-9);
    }
    else if (axisMode_ == 5)
    {
        // Amp vs Freq: the fixed 2048-point frames span [rate/2048 .. Nyquist].
        double dmin = std::numeric_limits<double>::max();
        double dmax = -std::numeric_limits<double>::max();
        for (const auto& cd : liveCurves)
            if (!cd.x.empty())
                { dmin = std::min (dmin, cd.x.front()); dmax = std::max (dmax, cd.x.back()); }
        if (curveCacheValid() && dmax > dmin)
            { fLo_ = dmin; fHi_ = dmax; }
        else
        {
            const double rate = app.audioEngine().sampleRate();
            fftAxisLimits (axisMode_, rate, pascalThirdOct(), fLo_, fHi_);
        }
    }
    else if (axisMode_ != 2 && !mags.empty() && cachedN > 1)
    {
        // Classic spectrum: the plotted bins span [rate/n .. Nyquist].
        const double rate = app.audioEngine().sampleRate();
        fLo_ = std::max (0.0, rate / (double) cachedN);
        fHi_ = rate * 0.5;
    }
    else
    {
        const double rate = app.audioEngine().sampleRate();
        fftAxisLimits (axisMode_, rate, pascalThirdOct(), fLo_, fHi_);
    }
}

// "Zoom Fit": fit both display ranges to every displayed curve (live + shown
// stored).
void FFTAnalysisView::resetDisplayRanges()
{
    fitVerticalRange();
    fitHorizontalRange();
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
    fftAxisLimits (axisMode_, rate, pascalThirdOct(), fLo_, fHi_);
    settingsGen++;
    recomputeSnapshots();
    recomputeCurveSlots();
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
        fftAxisLimits (axisMode_, rate, pascalThirdOct(), fLo_, fHi_);
    }
    repaint();
}

// Zoom the frequency window around the cursor y position: the vertical axis
// of Freq vs Time mode (frequency increases upward there; a harmonic ratio in
// "Check Harmonic Ratio" mode, which spans [0, 1024] instead of Hz).
void FFTAnalysisView::zoomFreqAtY (float y, double factor)
{
    const auto plot = plotBounds();
    if (plot.getHeight() <= 0 || fHi_ <= fLo_) return;

    const double rate = app.audioEngine().sampleRate();
    const double nyq = rate * 0.5;
    const bool hRatio = harmonicRatioUsable();
    const double vMax = hRatio ? 1024.0 : nyq;
    const double fCursor = std::clamp ((double) yToFreq (y), 1e-9, vMax);

    fLo_ = std::clamp (fCursor + (fLo_ - fCursor) / factor, 0.0, vMax);
    fHi_ = std::clamp (fCursor + (fHi_ - fCursor) / factor, 0.0, vMax);

    if (fHi_ <= fLo_ * 1.0001)
    {
        // Degenerate: fall back to the mode defaults.
        if (hRatio) { fLo_ = 0.5; fHi_ = 2.5; }
        else fftAxisLimits (axisMode_, rate, pascalThirdOct(), fLo_, fHi_);
    }
    repaint();
}

// Zoom the horizontal time window around the cursor x position (time-based
// curve modes). The window always stays inside the analysed region.
void FFTAnalysisView::zoomTimeAtX (int x, double factor)
{
    const auto plot = plotBounds();
    if (plot.getWidth() <= 0 || !(curveViewT1_ - curveViewT0_ > 1e-9)) return;

    const double tCursor = std::clamp ((double) xToTime (x), cachedT0, cachedT1);
    double lo = tCursor + (curveViewT0_ - tCursor) / factor;
    double hi = tCursor + (curveViewT1_ - tCursor) / factor;

    // Keep the window inside the analysed region.
    if (lo < cachedT0) { hi += cachedT0 - lo; lo = cachedT0; }
    if (hi > cachedT1) { lo -= hi - cachedT1; hi = cachedT1; }
    lo = std::clamp (lo, cachedT0, cachedT1);
    hi = std::max (lo + 1e-9, std::min (hi, cachedT1));

    if (hi - lo < 1e-6)   // degenerate: fall back to the full region
        { curveViewT0_ = cachedT0; curveViewT1_ = std::max (cachedT1, cachedT0 + 1e-9); }
    else
        { curveViewT0_ = lo; curveViewT1_ = hi; }
    repaint();
}

// Zoom the level window around the cursor y position.
void FFTAnalysisView::zoomLevelAtY (float y, double factor)
{
    const auto plot = plotBounds();
    if (plot.getHeight() <= 0) return;

    const double dbCursor = std::clamp ((double) yToLevel (y), -240.0, 40.0);
    // Anchor on the cursor like the frequency/time zooms: factor > 1 shrinks
    // the window around it (zoom in), factor < 1 expands it (zoom out).
    double top = dbCursor + (dbTop_ - dbCursor) / factor;
    double bot = dbCursor + (dbBottom_ - dbCursor) / factor;

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
static void fftAxisLimits (int mode, double rate, bool pascal20k, double& fLo, double& fHi)
{
    if (mode == 0) { fLo = 0.0; fHi = rate * 0.5; }
    else if (mode == 2 && rate * 0.5 > 50.0)
        // The dB-SPL pipeline's top band centre is ~19.95 kHz, so extend the
        // window to 20 kHz in that mode.
        { fLo = 50.0; fHi = std::min (rate * 0.5, pascal20k ? 20000.0 : 16000.0); }
    else          { fLo = 20.0; fHi = std::min (rate * 0.5, 20000.0); }
}

// Normalise a persisted smoothing window into one of None / 3 / 5 / 7 / 9 / 11.
// Normalise an octave order to one of the supported values.
static int syncThirdOctOrderLocal (int v)
{
    static const int kOrders[9] = { 1, 2, 3, 4, 5, 10, 20, 30, 40 };
    for (int i = 0; i < 9; ++i)
        if (kOrders[i] == v) return v;
    return 40;
}

static int syncSmoothValue (int sm)
{
    return (sm == 3 || sm == 5 || sm == 7 || sm == 9 || sm == 11
            || sm == 21 || sm == 51)
           ? sm : ((sm >= 1 && sm <= 4) ? 3 : 0);
}

// Core averaging computation shared by the live spectrum and every stored
// snapshot (see measures::regionSpectrumNorm): region [t0, t1], windows of
// exactly n samples at the panel's current window type, bin powers averaged
// and stored normalised vs kNormFloorDb.
void FFTAnalysisView::computeRegionFrom (AudioEngine& eng, const std::vector<int>& chans,
                                         double t0, double t1, int n,
                                         std::vector<std::vector<float>>& outMags)
{
    measures::regionSpectrumNorm (eng, chans, t0, t1, n, windowCombo.getSelectedId() - 1, outMags);
}

void FFTAnalysisView::resyncFromSettings ()
{
    const int newMode   = std::clamp (app.settings.fftAxisMode, 0, 5);
    const int newRefTime = std::clamp (app.settings.fftRefTime, 0, 1);
    const int newSmooth = syncSmoothValue (app.settings.fftSmooth);
    const int newTrack  = std::clamp (app.settings.fftTrack, 0, 2);
    const int newN      = std::clamp (app.settings.fftLocalN, 1024, 65536);
    const int newOrder  = syncThirdOctOrderLocal (app.settings.fftThirdOctOrder);

    if (newMode == axisMode_ && newRefTime == refTime_ && newSmooth == smooth_
        && newTrack == track_ && newN == localN && newOrder == thirdOctOrder_)
        return;   // nothing changed (e.g. a doc with the default settings)

    const bool modeChanged = (newMode != axisMode_);
    axisMode_  = newMode;
    refTime_   = newRefTime;
    smooth_    = newSmooth;
    track_     = newTrack;
    localN     = newN;
    thirdOctOrder_ = newOrder;

    // Update the combos without firing their onChange handlers.
    axisCombo.setSelectedId (axisMode_ + 1, juce::dontSendNotification);
    refTimeCombo.setSelectedId (refTime_ + 1, juce::dontSendNotification);
    {
        static const int kSmoothValues[8] = { 0, 3, 5, 7, 9, 11, 21, 51 };
        int id = 1;
        for (int i = 0; i < 8; ++i)
            if (kSmoothValues[i] == smooth_) { id = i + 1; break; }
        smoothCombo.setSelectedId (id, juce::dontSendNotification);
    }
    trackCombo.setSelectedId (track_ + 1, juce::dontSendNotification);
    fftSizeCombo.setSelectedId (localN, juce::dontSendNotification);
    {
        static const int kOrderValues[9] = { 1, 2, 3, 4, 5, 10, 20, 30, 40 };
        int id = 9;
        for (int i = 0; i < 9; ++i)
            if (kOrderValues[i] == thirdOctOrder_) { id = i + 1; break; }
        orderCombo.setSelectedId (id, juce::dontSendNotification);
    }

    // Invalidate every cache that depends on the settings.
    thirdOctMags.clear();      // band cache is only valid for mode 2
    liveCurves.clear();        // curve cache belongs to the previous mode
    cachedCurveMode = -1;
    cachedCurveTrack = 0;
    cachedCurveSmooth = 0;
    if (harmonicRatioActive_) { harmonicRatioActive_ = false; invalidateHarmonicCache(); }
    mags.clear (); cachedN = 0;
    settingsGen++;             // stored snapshots/curves must be recomputed too
    recomputeSnapshots();
    recomputeCurveSlots();
    if (modeChanged)
    {
        refreshSnapshotBands();      // buttons now address the other store
        refreshSnapshotButtonStates ();
        resetDisplayRanges();        // each mode has its own default window
        resized();                   // row 1/row 3 contents differ per mode
    }
    repaint();
}

void FFTAnalysisView::recomputeIfNeeded()
{
    doRecompute ();

    // A mode change requests a zoom fit, but the combo's onChange fires before
    // this mode's data has been computed; run the fit now that fresh numbers
    // exist. The flag stays set until the cache is ready (e.g. when an
    // expensive recompute is still throttled).
    if (fitPending_ && fitDataReady ())
    {
        fitPending_ = false;
        if (!app.selection.isEmpty())
            autoFitT0_ = cachedT0, autoFitT1_ = cachedT1;
        resetDisplayRanges ();
    }
    else if (zoomFitAuto_ && fitDataReady () && !app.selection.isEmpty()
             && (cachedT0 != autoFitT0_ || cachedT1 != autoFitT1_))
    {
        // Auto zoom-fit: a new or changed time selection refits both display
        // ranges to every displayed curve. Manual range changes disable it.
        autoFitT0_ = cachedT0;
        autoFitT1_ = cachedT1;
        resetDisplayRanges ();
    }
}

void FFTAnalysisView::doRecompute()
{
    const int w = getWidth(), h = getHeight();
    if (w <= 0 || !app.audioEngine().isFileLoaded()) return;

    // Region to analyse: the selection when non-empty, otherwise the playhead
    // position.
    double t0, t1;
    if (!app.selection.isEmpty()) { t0 = app.selection.getStart(); t1 = app.selection.getEnd(); }
    else                          { t0 = app.playhead;          t1 = app.playhead; }

    // The new analysis modes compute per-frame curves instead of an averaged
    // spectrum; they have their own cache and never touch `mags`.
    if (axisMode_ >= 3)
    {
        recomputeCurve (t0, t1);
        return;
    }

    const auto& eng = app.audioEngine();
    const int n = localN;
    const auto file = eng.getFile();
    const int scaleUnit = (int) app.scale().unit;
    // The dB-SPL pipeline runs when a Pascal full-scale value is set: band
    // levels then come from the reference-analyzer computation instead of
    // the averaged amplitude spectrum.
    const bool dbSpl = (axisMode_ == 2 && scaleUnit == (int) ScaleCalibration::Unit::Pascal);

    // Row-1 layout differs between the two pipelines (FFT size / window vs
    // octave order), so re-lay out when the pipeline flips.
    if (dbSpl != cachedPascalMode_) resized();

    // Channel list and content version complete the cache key: a solo switch
    // keeps the channel count, and an edit/recording keeps the region.
    const std::vector<int> chans = liveChannels();
    const int64_t contentVer = eng.contentVersion();

    if (dbSpl)
    {
        const bool upToDate = !thirdOctMags.empty()
            && thirdOctMags.size() == chans.size() && cachedChans_ == chans
            && cachedT0 == t0 && cachedT1 == t1 && cachedRate == eng.sampleRate()
            && cachedFile == file && cachedScaleUnit_ == scaleUnit
            && cachedEngine_ == &eng && cachedContentVer_ == contentVer;
        if (upToDate) return;

        // Throttle repeated expensive computations the same way as below.
        const int64_t now = juce::Time::getMillisecondCounter();
        const double regionLenS = std::max (0.0, (t1 - t0) * eng.sampleRate());
        if (!thirdOctMags.empty() && regionLenS > 65536.0 && lastComputeWasExpensive
            && now - lastComputeMs < 60)
            return;

        // No amplitude spectrum is needed in this mode: the band levels come
        // straight from the reference-analyzer computation. Replace the cache
        // on every recompute — never accumulate stale curves.
        mags.clear (); cachedN = 0;
        thirdOctMags.clear();
        for (int ch : chans)
            thirdOctMags.push_back (thirdOctBandsRef (eng, ch, t0, t1, thirdOctOrder_));

        cachedChans_ = chans;
        cachedEngine_ = &eng;
        cachedContentVer_ = contentVer;
        cachedT0 = t0; cachedT1 = t1;
        cachedRate = eng.sampleRate();
        cachedFile = file;
        cachedScaleUnit_ = scaleUnit;
        cachedPascalMode_ = true;
        lastComputeMs = now;
        lastComputeWasExpensive = (regionLenS > 65536.0);
        return;
    }

    // NOTE: switching files does NOT clear stored snapshots - they are shared
    // across all open files and each one remembers its own source document.

    if (mags.size() == chans.size() && cachedChans_ == chans && cachedN == n && cachedT0 == t0
        && cachedT1 == t1 && cachedRate == eng.sampleRate()
        && cachedFile == file && cachedScaleUnit_ == scaleUnit
        && cachedEngine_ == &eng && cachedContentVer_ == contentVer
        && (axisMode_ != 2 || thirdOctMags.size() == mags.size()))
        return;

    // Throttle only repeated expensive multi-window averaging (e.g. while
    // dragging a wide selection). Single-window updates are cheap enough to
    // run on every click or playhead move, and the first compute of a new
    // region always runs so clicks stay responsive.
    const int64_t now = juce::Time::getMillisecondCounter();
    const double regionLenS = std::max (0.0, (t1 - t0) * eng.sampleRate());
    if (!mags.empty() && regionLenS > (double) n && lastComputeWasExpensive
        && now - lastComputeMs < 60)
        return;

    computeRegionFrom (app.audioEngine(), chans, t0, t1, n, mags);

    // 1/3-octave band values for mode 2 (bin powers summed per band), stored in
    // the view's relative-dB space. Replace the cache on every recompute —
    // never accumulate, or stale curves from earlier clicks would be drawn
    // alongside the live one.
    thirdOctMags.clear();
    if (axisMode_ == 2)
        for (auto& m : mags)
            thirdOctMags.push_back (measures::bandsNormalizedToDb (measures::toThirdOctBands (m, n, eng.sampleRate(),
                                                                          currentEnbwBins (n))));

    cachedN = n; cachedT0 = t0; cachedT1 = t1;
    cachedChans_ = chans;
    cachedEngine_ = &eng;
    cachedContentVer_ = contentVer;
    cachedRate = eng.sampleRate();
    cachedFile = file;
    cachedScaleUnit_ = scaleUnit;
    cachedPascalMode_ = false;
    lastComputeMs = now;
    lastComputeWasExpensive = (regionLenS > (double) n);
}

// ------------------------------------------------- Curve analysis modes
//
// The new axis modes (3 = Freq vs Time, 4 = Amp vs Time, 5 = Amp vs Freq)
// compute per-frame curves over the analysed region instead of an averaged
// spectrum. A spectral selection acts as a mask: only bins inside it count.

// Fixed FFT size of the curve modes (see measures::timeCurve / spectrumCurve).
static constexpr int kTimeModeFFT = measures::kTimeModeFFT;

void FFTAnalysisView::recomputeCurve (double t0, double t1)
{
    const auto& eng = app.audioEngine();
    if (!eng.isFileLoaded())
    {
        liveCurves.clear(); cachedCurveMode = -1;
        invalidateHarmonicCache();
        return;
    }

    const double rate = eng.sampleRate();
    const double durS = eng.duration();
    t0 = std::clamp (t0, 0.0, durS);
    t1 = std::clamp (t1, t0, durS);

    // Keep the horizontal view window inside the analysed region; reset it to
    // the full region whenever the region itself changes.
    if (!haveCurveView_ || cachedT0 != t0 || cachedT1 != t1)
        { curveViewT0_ = t0; curveViewT1_ = t1; haveCurveView_ = true; }
    else
        {
            curveViewT0_ = std::clamp (curveViewT0_, t0, t1);
            curveViewT1_ = std::max (curveViewT0_ + 1e-9, std::clamp (curveViewT1_, t0, t1));
        }

    const auto file = eng.getFile();
    // A spectral selection masks the analysis: only bins inside it count.
    const SpectralSelection* mask = nullptr;
    FileDoc* d = app.doc();
    if (d != nullptr && !d->spectralSel.empty())
        mask = &d->spectralSel;
    const std::vector<int> chans = liveChannels();
    const int64_t contentVer = eng.contentVersion();
    const uint64_t maskHash = spectralMaskHash (mask);

    if (cachedCurveMode == axisMode_ && cachedCurveTrack == track_
        && cachedCurveSmooth == smooth_ && cachedT0 == t0 && cachedT1 == t1
        && cachedRate == rate && cachedFile == file
        && liveCurves.size() == chans.size() && cachedCurveChans_ == chans
        && cachedEngine_ == &eng && cachedContentVer_ == contentVer
        && cachedMaskHash_ == maskHash)
        return;

    // Throttle repeated expensive multi-frame computations (same policy as
    // the classic spectrum path): single-frame updates stay instant.
    const int64_t now = juce::Time::getMillisecondCounter();
    const double regionLenS = std::max (0.0, (t1 - t0) * rate);
    if (!liveCurves.empty() && lastComputeWasExpensive && now - lastComputeMs < 60
        && regionLenS > (double) kTimeModeFFT)
        return;

    std::vector<CurveData> out;
    if (axisMode_ == 5)
        // Fixed frame settings: the FFT size / window type combos are hidden in
        // this mode and no longer affect it.
        measures::spectrumCurve (eng, chans, t0, t1, kTimeModeFFT, 1, track_, mask, out);
    else
        measures::timeCurve (eng, chans, t0, t1, track_, mask, out);

    // Moving-average smoothing on the plotted series. In Amp vs Freq mode only
    // the amplitude is smoothed: the x values are bin centres and must stay
    // exact.
    if (smooth_ > 1)
        for (auto& cd : out)
        {
            smoothSeries (cd.ampDb, smooth_);
            if (axisMode_ != 5) smoothSeries (cd.freqHz, smooth_);
        }

    liveCurves = std::move (out);
    invalidateHarmonicCache();
    cachedCurveMode = axisMode_;
    cachedCurveTrack = track_;
    cachedCurveSmooth = smooth_;
    cachedCurveChans_ = chans;
    cachedEngine_ = &eng;
    cachedContentVer_ = contentVer;
    cachedMaskHash_ = maskHash;
    cachedT0 = t0; cachedT1 = t1;
    cachedRate = rate;
    cachedFile = file;

    lastComputeMs = now;
    lastComputeWasExpensive = regionLenS > (double) kTimeModeFFT * 4;
}

void FFTAnalysisView::smoothSeries (std::vector<float>& v, int window)
{
    measures::smoothSeries (v, window);
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
        if (!(s.n > 0)) continue;
        // Snapshots taken from Pascal-calibrated sources use the dB-SPL
        // pipeline on their own engine (no magnitude curves needed); everything
        // else converts the stored magnitude curves.
        const bool pascalSrc = (axisMode_ == 2) && s.source != nullptr
                               && s.source->scale.unit == ScaleCalibration::Unit::Pascal;
        if (!pascalSrc && s.mags.empty()) continue;
        // Band conversion needs the source file's sample rate, which may
        // differ from the active file's.
        const double rate = (s.source != nullptr)
            ? s.source->engine.sampleRate() : app.audioEngine().sampleRate();
        s.bands.clear();
        s.dbSpl = pascalSrc;
        if (!pascalSrc)
            for (auto& m : s.mags)
                s.bands.push_back (measures::bandsNormalizedToDb (measures::toThirdOctBands (m, s.n, rate,
                                                                         currentEnbwBins (s.n))));
        else
            for (int ch : s.chans)
                if (ch >= 0 && ch < s.source->engine.numChannels())
                    s.bands.push_back (thirdOctBandsRef (s.source->engine, ch, s.t0, s.t1, thirdOctOrder_));
    }
}

int FFTAnalysisView::axisWidth() const
{
    // Linear V/g labels are wider than plain dB numbers.
    if (app.scale().usesLinearLevels()) return 50;
    // "-92.0 dB-SPL" needs more room than a plain dB label, and even the
    // uncalibrated "-120.0 dB" form overflows a narrow axis at 11pt.
    return app.scale().unit == ScaleCalibration::Unit::Pascal ? 68 : 60;
}

juce::Rectangle<int> FFTAnalysisView::plotBounds() const
{
    const auto b = getLocalBounds();
    const int topH = topHeight();   // two control rows, three in time-based modes
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
    if (axisMode_ == 0 || axisMode_ == 5)
        return (float) (fLo_ + t * (fHi_ - fLo_));
    const double llo = std::log (std::max (1e-9, fLo_)), lhi = std::log (fHi_);
    return (float) std::exp (llo + t * (lhi - llo));
}

double FFTAnalysisView::freqToX (double f) const
{
    const auto plot = plotBounds();
    if (plot.getWidth() <= 0 || fHi_ <= fLo_) return (float) plot.getX();
    double t;
    if (axisMode_ == 0 || axisMode_ == 5)
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
    // Row 1: classic modes show the FFT size + window combos; the time-based
    // curve modes replace them with the Ref Time option. Amp vs Freq has no
    // first row at all (fixed 2048-point Hann frames). (Smoothing / Track live
    // on row 3, which only exists in the new analysis modes.)
    const bool hasRow1 = (axisMode_ < 3 || isTimeAxisMode());
    if (hasRow1)
    {
        auto row1 = b.removeFromTop (26);
        if (isTimeAxisMode())
        {
            // The time-based modes replace the FFT/Win options with Ref Time.
            refTimeCombo.setVisible (true);
            fftSizeCombo.setVisible (false);
            windowCombo.setVisible (false);
            refTimeCombo.setBounds (58, row1.getY(), 120, 22);
        }
        else if (axisMode_ == 2 && pascalThirdOct())
        {
            // The dB-SPL pipeline uses fixed 65536-point Hanning frames for its
            // bands; the FFT size / window options stay visible next to the
            // octave order, which sets the bandpass filter steepness.
            refTimeCombo.setVisible (false);
            fftSizeCombo.setVisible (true);
            windowCombo.setVisible (true);
            orderCombo.setVisible (true);
            fftSizeCombo.setBounds (30, row1.getY(), 85, 22);
            windowCombo.setBounds (148, row1.getY(), 90, 22);
            orderCombo.setBounds (279, row1.getY(), 85, 22);
        }
        else
        {
            refTimeCombo.setVisible (false);
            fftSizeCombo.setVisible (true);
            windowCombo.setVisible (true);
            orderCombo.setVisible (false);
            fftSizeCombo.setBounds (30, row1.getY(), 85, 22);
            windowCombo.setBounds (148, row1.getY(), 90, 22);
        }
    }
    else
    {
        refTimeCombo.setVisible (false);
        fftSizeCombo.setVisible (false);
        windowCombo.setVisible (false);
        orderCombo.setVisible (false);
    }

    auto row2 = b.removeFromTop (26);
    // The axis combo grows to show its full text when the panel is wide
    // enough; otherwise it keeps its compact width and the eight snapshot
    // buttons keep their fixed slots (they always take priority over the
    // combo's extra width).
    {
        const int btnPitch = 24, btnW = 23;
        const int buttonsTotal = 8 * btnPitch - (btnPitch - btnW);   // 191 px
        const juce::Font f = getLookAndFeel().getComboBoxFont (axisCombo);
        const int idealW = std::max (90,
            juce::GlyphArrangement::getStringWidthInt (f, axisCombo.getText()) + 36);
        if (b.getWidth() >= 4 + idealW + 6 + buttonsTotal + 8)
        {
            axisCombo.setBounds (4, row2.getY() + 2, idealW, 22);
            for (int i = 0; i < 8; ++i)
                snapBtns[i].setBounds (10 + idealW + i * btnPitch, row2.getY() + 2, btnW, 20);
        }
        else
        {
            axisCombo.setBounds (4, row2.getY() + 2, 90, 22);
            for (int i = 0; i < 8; ++i)
                snapBtns[i].setBounds (98 + i * btnPitch, row2.getY() + 2, btnW, 20);
        }
    }

    // Row 3 (curve modes only): Smoothing + Track.
    if (axisMode_ >= 3)
    {
        auto row3 = b.removeFromTop (26);
        smoothCombo.setVisible (true);
        trackCombo.setVisible (true);
        smoothCombo.setBounds (58, row3.getY(), 95, 22);
        trackCombo.setBounds (215, row3.getY(), 70, 22);
    }
    else
    {
        smoothCombo.setVisible (false);
        trackCombo.setVisible (false);
    }

    repaint();
}

void FFTAnalysisView::paint (juce::Graphics& g)
{
    sc::diag::ScopedPaint diagPaint ("fft", getWidth(), getHeight());   // paint.slow above 50 ms
    const auto fullBounds = getLocalBounds();
    g.fillAll (juce::Colours::black);

    // Top control rows background + labels
    const int topH = topHeight();
    const auto topRow = juce::Rectangle<int> (fullBounds).removeFromTop (topH);
    g.setColour (juce::Colour::fromRGB (45, 45, 48));
    g.fillRect (topRow);
    g.setColour (juce::Colours::white);
    g.setFont (11.0f);
    const bool hasRow1 = (axisMode_ < 3 || isTimeAxisMode());
    if (hasRow1)
    {
        if (isTimeAxisMode())
            g.drawText ("Ref Time", 6, topRow.getY() + 5, 48, 16, juce::Justification::centredLeft, false);
        else if (axisMode_ == 2 && pascalThirdOct())
        {
            g.drawText ("FFT", 6, topRow.getY() + 5, 22, 16, juce::Justification::centredLeft, false);
            g.drawText ("Win", 124, topRow.getY() + 5, 22, 16, juce::Justification::centredLeft, false);
            g.drawText ("Order", 247, topRow.getY() + 5, 30, 16, juce::Justification::centredLeft, false);
        }
        else
        {
            g.drawText ("FFT", 6, topRow.getY() + 5, 22, 16, juce::Justification::centredLeft, false);
            g.drawText ("Win", 124, topRow.getY() + 5, 22, 16, juce::Justification::centredLeft, false);
        }
    }
    if (axisMode_ >= 3)
    {
        const int row3Y = hasRow1 ? 52 : 26;   // third control row
        g.drawText ("Smooth", 6, row3Y + 5, 48, 16, juce::Justification::centredLeft, false);
        g.drawText ("Track", 175, row3Y + 5, 38, 16, juce::Justification::centredLeft, false);
    }

    const int axisR = axisWidth();   // right level-axis width
    const int axisB = 18;   // bottom Hz axis height
    const auto plot = plotBounds();

    if (!app.audioEngine().isFileLoaded())
    {
        g.setColour (juce::Colours::grey);
        g.setFont (12.0f);
        g.drawText ("Load a file to see the frequency analysis", plot, juce::Justification::centred);
        return;
    }

    recomputeIfNeeded();

    // The new analysis modes have their own grid / curve / label pipeline.
    if (axisMode_ >= 3)
    {
        paintCurveModes (g);
        return;
    }

    const double rate = app.audioEngine().sampleRate();
    // Local display ranges (independent of the spectrogram's settings).
    const double fLo = fLo_, fHi = fHi_;
    const int n = cachedN;
    // In the dB-SPL pipeline `mags` is intentionally empty: the band values
    // alone carry the spectrum.
    const bool haveSpectrum = ((n > 1) && !mags.empty()
                               && (axisMode_ != 2 || !thirdOctMags.empty()))
                              || (pascalThirdOct() && !thirdOctMags.empty());

    // Frequency grid ticks for the current axis mode.
    std::vector<double> freqTicks;
    // Linear subdivisions inserted into wide log gaps (freq, hasLabel).
    std::vector<std::pair<double, bool>> subTicks;
    bool thirdOctFallback = false;   // deep zoom between band centres
    buildFreqAxisTicks (freqTicks, subTicks, thirdOctFallback);

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
                                 float w, const double* snapCentres = nullptr, int snapNBands = -1)
        {
            if (bands.empty()) return;
            g.setColour (col);
            juce::Path path;
            bool started = false;
            // Snapshots may come from the other pipeline than the live view
            // (dB-SPL vs classic), so they can carry their own band centres.
            const double* centres = (snapCentres != nullptr) ? snapCentres : bandCentres();
            const int nBands = (snapNBands > 0) ? snapNBands : numBands();
            for (int b = 0; b < nBands && b < (int) bands.size(); ++b)
            {
                const double fc = centres[(size_t) b];
                if (fc > fHi + 1e-6) break;
                const double lo = std::max (fLo, fc * std::pow (2.0, -1.0 / 6.0));
                const double hi = std::min (fHi, fc * std::pow (2.0,  1.0 / 6.0));
                if (hi <= lo) continue;
                const float xLo = (float) freqToX (lo);
                const float xHi = (float) freqToX (hi);
                // Band values are stored in the view's relative-dB space.
                const double dBb = (double) bands[(size_t) b];
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
            if (snapshots[i].shown && snapshots[i].n > 1
                && (!snapshots[i].mags.empty() || !snapshots[i].bands.empty()))
            {
                // In 1/3-octave mode the band arrays are the curves to draw; in
                // the dB-SPL pipeline mags may be empty while bands is full.
                const size_t nChans = (axisMode_ == 2) ? snapshots[i].bands.size()
                                                      : snapshots[i].mags.size();
                for (size_t c = 0; c < nChans && c < 4; ++c)
                    if (axisMode_ == 2)
                    {
                        // Braces are required here: without them the else below
                        // would bind to the inner if and snapshot curves would
                        // never be drawn in linear/log axis modes.
                        if (!snapshots[i].bands.empty())
                            drawBandCurve (snapshots[i].bands[c], snapColours[i], 1.0f,
                                           snapshots[i].dbSpl ? FFTAnalysisView::thirdOptCentresRef() : nullptr,
                                           snapshots[i].dbSpl ? FFTAnalysisView::numThirdOptBandsRef() : -1);
                    }
                    else
                        drawCurve (snapshots[i].mags[c], snapshots[i].n, snapColours[i], 1.0f);
            }

        if (axisMode_ == 2)
            for (size_t c = 0; c < thirdOctMags.size() && c < 4; ++c)
                drawBandCurve (thirdOctMags[c], fftColours[c], 1.2f);
        else
            for (size_t c = 0; c < mags.size() && c < 4; ++c)
                drawCurve (mags[c], n, fftColours[c], 1.2f);

        g.restoreState();
    }

    // Axis labels
    drawFreqXLabels (g, plot, axisB, freqTicks, subTicks, thirdOctFallback);
    // With a scale calibration the grid lines show absolute levels
    // (dB-SPL for Pascal files; linear V/g values for volts/grams files)
    // instead of relative dB.
    // The curves are drawn in relative dB, so the axis follows the calibration
    // of the first displayed channel (per-channel "bkdk" full scales differ).
    const ScaleCalibration& sc = app.scale();
    const int axisCh = levelAxisChannel();
    for (double L = std::floor (dbTop_ / dbStep) * dbStep; L >= dbBottom_ - 1e-9; L -= dbStep)
        g.drawText (ScaleCalibration::formatSpectralLevel (L, sc, axisCh), plot.getRight() + 4,
                    (int) levelToY (L) - 7, axisR - 6, 14, juce::Justification::centredLeft, false);

    // Harmonic reference lines: while active, the reference frequency is
    // harmonic #N of a fundamental and harmonics 1..max(N, maxK) get delicate
    // vertical markers wherever they are inside the visible window. White dots
    // mark where each line crosses the channel curves.
    if (harmonicRefActive_ && haveSpectrum && harmonicAnchorF_ > 0.0)
    {
        const double fFund = harmonicAnchorF_ / std::max (1, harmonicN_);
        const int kMax = std::max (harmonicN_, harmonicMaxK_);
        const double rate = app.audioEngine().sampleRate();
        for (int k = 1; k <= kMax; ++k)
        {
            const double f = (double) k * fFund;
            if (f < fLo || f > fHi) continue;
            const float x = (float) freqToX (f);
            g.setColour (juce::Colour::fromRGBA (140, 230, 255, 160));
            g.drawVerticalLine (x, plot.getY(), plot.getBottom());
            g.setFont (9.0f);
            g.drawText (juce::String (k), x - 8, plot.getY() + 1, 16, 12,
                        juce::Justification::centred, false);

            // White dots where the harmonic line crosses each channel's curve.
            for (size_t c = 0; c < mags.size () && c < 4; ++c)
            {
                const auto& mag = mags[c];
                if (mag.empty ()) continue;
                const int maxBin = std::min ((int) (cachedN / 2), (int) mag.size () - 1);
                if (maxBin < 1 || f > rate * 0.5) continue;   // no FFT data above Nyquist
                const int bin = (int) std::clamp ((double) std::lround (f * cachedN / rate),
                                                 0.0, (double) maxBin);
                const double dBc = -kNormFloorDb * (1.0 - (double) mag[(size_t) bin]);
                g.setColour (juce::Colours::white);
                g.fillEllipse (x - 3.5f, levelToY (dBc) - 3.5f, 7.0f, 7.0f);
            }
        }
    }

    // Cursor readout + rubber band (shared with the curve modes).
    paintCursorReadout (g, plot);
    paintZoomRubberBand (g, plot);
}

// Bottom-axis labels for a frequency-x axis (classic modes + Amp vs Freq).
void FFTAnalysisView::drawFreqXLabels (juce::Graphics& g, const juce::Rectangle<int>& plot,
                                       int axisB, const std::vector<double>& freqTicks,
                                       const std::vector<std::pair<double, bool>>& subTicks,
                                       bool thirdOctFallback) const
{
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
}

// Time tick labels: decimal seconds for short spans, m:ss / h:mm:ss beyond.
static juce::String formatTimeTickLabel (double s)
{
    if (!(s >= 0.0)) return "0";
    if (s < 60.0)
        return juce::String (s, s < 1.0 ? 3 : (s < 10.0 ? 1 : 0));
    const int whole = (int) std::lround (s);
    const int h = whole / 3600;
    const int m = (whole % 3600) / 60;
    const int sec = whole % 60;
    if (h > 0)
        return juce::String (h) + ":" + juce::String (m).paddedLeft ('0', 2)
             + ":" + juce::String (sec).paddedLeft ('0', 2);
    return juce::String (m) + ":" + juce::String (sec).paddedLeft ('0', 2);
}

// Time <-> x mapping for the time-based curve modes. The horizontal view
// window [curveViewT0_, curveViewT1_] (inside the analysed region) spans the
// full plot width; Relative vs Absolute only changes which values the ticks
// are labelled with.
float FFTAnalysisView::timeToX (double tSecs) const
{
    const auto plot = plotBounds();
    if (plot.getWidth() <= 0)
        return (float) plot.getX();
    const double span = std::max (1e-9, curveViewT1_ - curveViewT0_);
    const double t = std::clamp ((tSecs - curveViewT0_) / span, 0.0, 1.0);
    return (float) (plot.getX() + t * plot.getWidth());
}

double FFTAnalysisView::xToTime (int x) const
{
    const auto plot = plotBounds();
    if (plot.getWidth() <= 0)
        return curveViewT0_;
    const double span = std::max (1e-9, curveViewT1_ - curveViewT0_);
    const double t = std::clamp ((double) (x - plot.getX()) / plot.getWidth(), 0.0, 1.0);
    return curveViewT0_ + t * span;
}

// Frequency <-> y mapping for Freq vs Time mode (frequency increases upward).
float FFTAnalysisView::freqToY (double fHz) const
{
    const auto plot = plotBounds();
    if (plot.getHeight() <= 0 || fHi_ <= fLo_)
        return (float) plot.getBottom();
    const double t = std::clamp ((fHz - fLo_) / std::max (1e-9, fHi_ - fLo_), 0.0, 1.0);
    return (float) (plot.getBottom() - t * plot.getHeight());
}

double FFTAnalysisView::yToFreq (float y) const
{
    const auto plot = plotBounds();
    if (plot.getHeight() <= 0 || fHi_ <= fLo_)
        return fLo_;
    const double t = std::clamp ((plot.getBottom() - (double) y) / plot.getHeight(), 0.0, 1.0);
    return fLo_ + t * (fHi_ - fLo_);
}

// Grid, curves and labels for the curve analysis modes (3-5).
void FFTAnalysisView::paintCurveModes (juce::Graphics& g)
{
    const int axisR = axisWidth();   // right-axis width
    const int axisB = 18;            // bottom axis height
    const auto plot = plotBounds();

    // "Check Harmonic Ratio": the vertical window holds ratio values and the
    // curves are drawn from the transformed (divided) data. Falls back to
    // plain Hz when no fundamental curve exists yet.
    const bool hRatio = harmonicRatioUsable();

    const bool haveLive = !liveCurves.empty() && !liveCurves[0].x.empty();
    bool haveSnap = false;
    for (int i = 0; i < 8; ++i)
        if (curveSnaps[i].shown && curveSnaps[i].mode == axisMode_ && !curveSnaps[i].data.empty())
            { haveSnap = true; break; }

    // The horizontal view window (inside the analysed region) spans the plot.
    const double tLo = curveViewT0_;
    const double tHi = std::max (curveViewT1_, curveViewT0_ + 1e-9);

    if (!haveLive && !haveSnap)
    {
        g.setColour (juce::Colours::grey);
        g.setFont (11.0f);
        g.drawText ("No audio data", plot, juce::Justification::centred);
        paintCursorReadout (g, plot);
        paintZoomRubberBand (g, plot);
        return;
    }

    // ---- Grid + axis labels --------------------------------------------
    if (isTimeAxisMode())
    {
        // Time ticks: Relative mode counts from the selection start (0-based),
        // Absolute uses nice steps on the file's absolute time.
        const bool rel = (refTime_ == 0);
        std::vector<double> tTicks;
        const double span = std::max (1e-9, tHi - tLo);
        auto makeTicks = [&] (double lo)
        {
            bool done = false;
            for (double e = std::floor (std::log10 (span / 8.0));
                 !done && e <= std::ceil (std::log10 (span / 3.0)) + 1.0; ++e)
                for (double m : { 1.0, 2.0, 5.0 })
                {
                    const double step = m * std::pow (10.0, e);
                    if (span / step >= 3.0 && span / step <= 8.0)
                    {
                        for (double t = std::ceil ((lo + 1e-9) / step - 1e-9) * step;
                             t <= lo + span + 1e-6; t += step)
                            if (t >= lo - 1e-6)
                                tTicks.push_back (t);
                        done = true;
                        break;
                    }
                }
            if (!done)
                for (int i = 1; i <= 4; ++i)
                    tTicks.push_back (lo + span * (double) i / 5.0);
        };
        // Relative mode counts from the selection start: the visible offset
        // range is [tLo - cachedT0, tHi - cachedT0].
        makeTicks (rel ? (tLo - cachedT0) : tLo);

        g.setColour (juce::Colour::fromRGB (40, 40, 46));
        for (double tv : tTicks)
            g.drawVerticalLine ((float) timeToX (rel ? tLo + tv : tv), plot.getY(), plot.getBottom());

        // Vertical axis: frequency in Freq vs Time mode (a harmonic ratio
        // when "Check Harmonic Ratio" is on), level in Amp vs Time.
        if (axisMode_ == 3)
        {
            std::vector<double> fTicks;
            const double spanF = std::max (1e-9, fHi_ - fLo_);
            bool done = false;
            if (hRatio)
            {
                // Ratio ticks: nice steps in x-units, labelled "1x", "2x"...
                for (double step : { 0.05, 0.1, 0.2, 0.25, 0.5, 1.0, 2.0, 5.0 })
                    if (spanF / step >= 3.0 && spanF / step <= 8.0)
                    {
                        for (double r = std::ceil (fLo_ / step - 1e-9) * step;
                             r <= fHi_ + 1e-6; r += step)
                            if (r >= -1e-9) fTicks.push_back (std::max (0.0, r));
                        done = true;
                        break;
                    }
            }
            else
            {
                for (double e = std::floor (std::log10 (spanF / 8.0));
                     !done && e <= std::ceil (std::log10 (spanF / 3.0)) + 1.0; ++e)
                    for (double m : { 1.0, 2.0, 5.0 })
                    {
                        const double step = m * std::pow (10.0, e);
                        if (spanF / step >= 3.0 && spanF / step <= 8.0)
                        {
                            for (double f = std::ceil (fLo_ / step - 1e-9) * step;
                                 f <= fHi_ + 1e-6; f += step)
                                fTicks.push_back (f);
                            done = true;
                            break;
                        }
                    }
            }
            if (!done)
                for (int i = 1; i <= 4; ++i)
                    fTicks.push_back (fLo_ + spanF * (double) i / 5.0);

            g.setColour (juce::Colour::fromRGB (40, 40, 46));
            for (double f : fTicks)
                g.drawHorizontalLine ((float) freqToY (f), plot.getX(), plot.getRight());

            g.setColour (juce::Colours::lightgrey);
            g.setFont (9.0f);
            for (double f : fTicks)
            {
                juce::String label;
                if (hRatio)
                {
                    // "1x", "2x", "0.5x" ... (trailing zeros stripped).
                    label = juce::String (f, 3);
                    while (label.endsWith ("0")) label = label.dropLastCharacters (1);
                    if (label.endsWith (".")) label = label.dropLastCharacters (1);
                    label += "x";
                }
                else if (f >= 1000.0)
                    label = juce::String (f / 1000.0, std::abs (f - std::round (f)) < 1e-6 ? 0 : 1) + "k";
                else
                    label = juce::String ((int) std::lround (f));
                g.drawText (label, plot.getRight() + 4, (float) freqToY (f) - 7, axisR - 6, 14,
                            juce::Justification::centredLeft, false);
            }
        }
        else   // Amp vs Time: level gridlines + right labels (classic style).
        {
            const double dbSpan = std::max (1e-9, dbTop_ - dbBottom_);
            int dbStep = 12;
            for (int cand : { 2, 4, 6, 10, 12, 20, 30 })
                if (dbSpan / cand >= 3.0 && dbSpan / cand <= 9.0) { dbStep = cand; break; }

            g.setColour (juce::Colour::fromRGB (40, 40, 46));
            for (double L = std::floor (dbTop_ / dbStep) * dbStep; L >= dbBottom_ - 1e-9; L -= dbStep)
                g.drawHorizontalLine ((int) levelToY (L), plot.getX(), plot.getRight());

            const ScaleCalibration& sc = app.scale();
            const int axisCh = levelAxisChannel();   // first displayed channel's calibration
            g.setColour (juce::Colours::lightgrey);
            g.setFont (9.0f);
            for (double L = std::floor (dbTop_ / dbStep) * dbStep; L >= dbBottom_ - 1e-9; L -= dbStep)
                g.drawText (ScaleCalibration::formatSpectralLevel (L, sc, axisCh), plot.getRight() + 4,
                            (int) levelToY (L) - 7, axisR - 6, 14, juce::Justification::centredLeft, false);
        }

        // Bottom time labels.
        g.setColour (juce::Colours::lightgrey);
        g.setFont (9.0f);
        for (double tv : tTicks)
            g.drawText (formatTimeTickLabel (tv), (int) timeToX (rel ? tLo + tv : tv) - 14,
                        plot.getBottom() + 2, 30, axisB - 4, juce::Justification::centred, false);
    }
    else   // Amp vs Freq: frequency-x grid exactly like the classic modes.
    {
        std::vector<double> freqTicks;
        std::vector<std::pair<double, bool>> subTicks;
        bool thirdOctFallback = false;
        buildFreqAxisTicks (freqTicks, subTicks, thirdOctFallback);

        g.setColour (juce::Colour::fromRGB (40, 40, 46));
        for (double f : freqTicks)
            g.drawVerticalLine ((float) freqToX (f), plot.getY(), plot.getBottom());
        for (const auto& st : subTicks)
            g.drawVerticalLine ((float) freqToX (st.first), plot.getY(), plot.getBottom());

        const double dbSpan = std::max (1e-9, dbTop_ - dbBottom_);
        int dbStep = 12;
        for (int cand : { 2, 4, 6, 10, 12, 20, 30 })
            if (dbSpan / cand >= 3.0 && dbSpan / cand <= 9.0) { dbStep = cand; break; }
        for (double L = std::floor (dbTop_ / dbStep) * dbStep; L >= dbBottom_ - 1e-9; L -= dbStep)
            g.drawHorizontalLine ((int) levelToY (L), plot.getX(), plot.getRight());

        drawFreqXLabels (g, plot, axisB, freqTicks, subTicks, thirdOctFallback);

        const ScaleCalibration& sc = app.scale();
        const int axisCh = levelAxisChannel();   // first displayed channel's calibration
        g.setColour (juce::Colours::lightgrey);
        g.setFont (9.0f);
        for (double L = std::floor (dbTop_ / dbStep) * dbStep; L >= dbBottom_ - 1e-9; L -= dbStep)
            g.drawText (ScaleCalibration::formatSpectralLevel (L, sc, axisCh), plot.getRight() + 4,
                        (int) levelToY (L) - 7, axisR - 6, 14, juce::Justification::centredLeft, false);
    }

    // ---- Curves ----------------------------------------------------------
    g.saveState();
    g.reduceClipRegion (plot);

    auto drawCurveSeries = [&](const CurveData& cd, const juce::Colour& col, float w)
    {
        if (cd.x.empty()) return;
        g.setColour (col);
        auto xy = [&] (size_t i) -> std::pair<float, float>
        {
            const float px = isTimeAxisMode() ? timeToX (cd.x[i]) : (float) freqToX (cd.x[i]);
            // Freq vs Time plots the tracked frequency; the other modes plot level.
            const float py = (axisMode_ == 3) ? freqToY (cd.freqHz[i]) : levelToY (cd.ampDb[i]);
            return { px, py };
        };

        if (cd.x.size() == 1)
        {
            const auto p = xy (0);
            g.fillEllipse (p.first - 2.5f, p.second - 2.5f, 5.0f, 5.0f);
            return;
        }

        juce::Path path;
        bool started = false;
        for (size_t i = 0; i < cd.x.size(); ++i)
        {
            const auto p = xy (i);
            if (!started) { path.startNewSubPath (p.first, p.second); started = true; }
            else          path.lineTo (p.first, p.second);
        }
        g.strokePath (path, juce::PathStrokeType (w));
    };

    // Stored curve snapshots first (only those saved in this mode), live on
    // top. In harmonic-ratio mode the transformed curves are drawn instead:
    // their freqHz values hold ratios and [fLo_, fHi_] is the ratio window,
    // so the same freqToY mapping applies.
    for (int i = 0; i < 8; ++i)
    {
        const auto& cs = curveSnaps[i];
        if (!(cs.shown && cs.mode == axisMode_)) continue;
        const auto& src = hRatio ? harmonicCache_.snaps[i] : cs.data;
        for (size_t c = 0; c < src.size() && c < 4; ++c)
            drawCurveSeries (src[c], snapColours[i], 1.0f);
    }
    if (haveLive)
    {
        const auto& lv = hRatio ? harmonicCache_.live : liveCurves;
        for (size_t c = 0; c < lv.size() && c < 4; ++c)
            drawCurveSeries (lv[c], fftColours[c], 1.2f);
    }

    g.restoreState();

    paintCursorReadout (g, plot);
    paintZoomRubberBand (g, plot);
}

// Cursor readout strip + crosshair/snap marker (shared by all modes).
void FFTAnalysisView::paintCursorReadout (juce::Graphics& g, const juce::Rectangle<int>& plot) const
{
    static constexpr int readoutH = 16;   // cursor info strip at the very bottom
    const auto fullBounds = getLocalBounds();

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
}

// Rubber band for a right-drag box zoom (drawn last so nothing covers it).
// A drag that starts on an axis stretches into a full-height/width band
// across that axis strip - the same translucent blue as the waveform and
// spectrogram views; a drag inside the plot keeps the free yellow box.
void FFTAnalysisView::paintZoomRubberBand (juce::Graphics& g, const juce::Rectangle<int>& plot) const
{
    static constexpr int axisB = 18;   // bottom axis height
    if (dragMode != DragMode::ZoomDrag || !haveZoomRect)
        return;

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

// Generate the frequency-axis grid ticks for the current axis mode:
// 1/3-octave band centres, adaptive log ticks with linear gap-fill,
// or a nice linear step. Shared by the classic modes and Amp vs Freq.
void FFTAnalysisView::buildFreqAxisTicks (std::vector<double>& freqTicks,
                                           std::vector<std::pair<double, bool>>& subTicks,
                                           bool& thirdOctFallback) const
{
    freqTicks.clear();
    subTicks.clear();
    thirdOctFallback = false;
    const double fLo = fLo_, fHi = fHi_;   // local display window
    const auto plot = plotBounds();
    if (axisMode_ == 2)
    {
        // One grid line per 1/3-octave band centre.
        const double* centres = bandCentres();
        for (int b = 0; b < numBands() && centres[(size_t) b] <= fHi + 1e-6; ++b)
            if (centres[(size_t) b] >= fLo - 1e-6) freqTicks.push_back (centres[(size_t) b]);
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

}


// Build the cursor readout text for position (x, y). When one of the curves
// passes within a few pixels of the cursor, snapped is set and snapDotX/Y
// hold that point so paint() can draw the marker. Returns an empty string
// when there is nothing to show.
// Cursor readout for the curve analysis modes (3-5). Reports the cursor's
// time/frequency plus each visible channel's value at the nearest data
// point, and snaps to a curve that passes within a few pixels.
juce::String FFTAnalysisView::buildCurveCursorInfo (int x, int y, bool& snapped,
                                                    float& snapDotX, float& snapDotY) const
{
    const auto plot = plotBounds();
    if (x < plot.getX() || x > plot.getRight()
        || y < plot.getY() || y > plot.getBottom())
        return {};

    const ScaleCalibration& sc = app.scale();
    // In harmonic-ratio mode the displayed live curves are the transformed ones.
    const std::vector<CurveData>& lv = harmonicRatioUsable() ? harmonicCache_.live : liveCurves;
    juce::String info;
    int snapCh = -1;          // channel index of a snapped curve (-1 = none)
    double bestDy = 8.0;

    auto pointXY = [&](const CurveData& cd, size_t i) -> std::pair<float, float>
    {
        const float px = isTimeAxisMode() ? timeToX (cd.x[i]) : (float) freqToX (cd.x[i]);
        // Freq vs Time plots the tracked frequency; the other modes plot level.
        const float py = (axisMode_ == 3) ? freqToY (cd.freqHz[i]) : levelToY (cd.ampDb[i]);
        return { px, py };
    };

    // Nearest data point per channel by horizontal distance...
    std::vector<size_t> nearest (lv.size(), 0);
    for (size_t c = 0; c < lv.size() && c < 4; ++c)
    {
        const auto& cd = lv[c];
        if (cd.x.empty()) continue;
        size_t best = 0;
        double bestDx = 1e30;
        for (size_t i = 0; i < cd.x.size(); ++i)
        {
            const float px = isTimeAxisMode() ? timeToX (cd.x[i]) : (float) freqToX (cd.x[i]);
            const double d = std::abs ((double) px - x);
            if (d < bestDx) { bestDx = d; best = i; }
        }
        nearest[c] = best;
    }

    // ...and snap when any curve passes close to the cursor vertically.
    for (size_t c = 0; c < lv.size() && c < 4; ++c)
    {
        const auto& cd = lv[c];
        if (cd.x.empty()) continue;
        const auto pxy = pointXY (cd, nearest[c]);
        const double d = std::abs ((double) pxy.second - y);
        if (d < bestDy) { bestDy = d; snapCh = (int) c; }
    }

    if (snapCh >= 0 && lv[(size_t) snapCh].x.size() > 1)
    {
        snapped = true;
        const auto pxy = pointXY (lv[(size_t) snapCh], nearest[(size_t) snapCh]);
        snapDotX = pxy.first;
        snapDotY = pxy.second;
    }

    // Curve index c maps to visible channel app.visibleChannels[c].
    auto curveCh = [&] (size_t c) -> int
    {
        return (c < app.visibleChannels.size()) ? app.visibleChannels[(size_t) c] : -1;
    };

    if (isTimeAxisMode())
    {
        info = "Cursor:  t=" + formatTimeTickLabel (xToTime ((double) x));
        for (size_t c = 0; c < lv.size() && c < 4; ++c)
        {
            const auto& cd = lv[c];
            if (cd.x.empty()) continue;
            info += ",  Ch" + juce::String ((int) c + 1) + "=";
            if (axisMode_ == 3 && harmonicRatioUsable())
                // Harmonic-ratio mode: the value is a ratio, not Hz.
                info += juce::String (cd.freqHz[nearest[c]], 2) + "x"
                      + ScaleCalibration::formatSpectralLevel (cd.ampDb[nearest[c]], sc,
                                                              curveCh (c));
            else if (axisMode_ == 3)
                info += juce::String (cd.freqHz[nearest[c]] >= 1000.0
                                      ? cd.freqHz[nearest[c]] / 1000.0 : cd.freqHz[nearest[c]], 2)
                      + (cd.freqHz[nearest[c]] >= 1000.0 ? " kHz" : " Hz")
                      + altFreqSuffix (app, cd.freqHz[nearest[c]])
                      + ScaleCalibration::formatSpectralLevel (cd.ampDb[nearest[c]], sc,
                                                              curveCh (c));
            else
                info += ScaleCalibration::formatSpectralLevel (cd.ampDb[nearest[c]], sc,
                                                              curveCh (c));
        }
    }
    else   // Amp vs Freq: cursor frequency + per-channel level at the nearest bin.
    {
        const double f = xToFreq ((float) x);
        info = "Cursor:  " + juce::String (f >= 1000.0 ? f / 1000.0 : f, 2)
             + (f >= 1000.0 ? " kHz" : " Hz");
        for (size_t c = 0; c < liveCurves.size() && c < 4; ++c)
        {
            const auto& cd = liveCurves[c];
            if (cd.x.empty()) continue;
            info += ",  Ch" + juce::String ((int) c + 1) + "="
                  + ScaleCalibration::formatSpectralLevel (cd.ampDb[nearest[c]], sc,
                                                          curveCh (c));
        }
        info += altFreqSuffix (app, f);
    }

    return info;
}

// "| Alt N=value" suffixes for every enabled alternative frequency reference.
juce::String FFTAnalysisView::altFreqSuffix (const Application& appIn, double freqHz)
{
    juce::String s;
    for (const auto& r : appIn.altFreqRefs)
        if (r.enabled && std::abs (r.refMain) > 1e-9)
            s += "   | " + r.name + "=" + juce::String (freqHz * r.altValue / r.refMain, 3);
    return s;
}

juce::String FFTAnalysisView::buildCursorInfo (int x, int y, bool& snapped,
                                               float& snapDotX, float& snapDotY) const
{
    snapped = false;
    snapDotX = 0.0f;
    snapDotY = 0.0f;

    if (axisMode_ >= 3)
        return buildCurveCursorInfo (x, y, snapped, snapDotX, snapDotY);

    const auto plot = plotBounds();
    const int n = cachedN;
    // In the dB-SPL pipeline `mags` is intentionally empty: the band values
    // alone carry the spectrum.
    const bool haveSpectrum = ((n > 1) && !mags.empty()
                               && (axisMode_ != 2 || !thirdOctMags.empty()))
                              || (pascalThirdOct() && !thirdOctMags.empty());
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
        const double* centres = bandCentres();
        for (int i = 0; i < numBands() && i < (int) thirdOctMags[0].size(); ++i)
        {
            const double fc = centres[(size_t) i];
            const double lo = fc * std::pow (2.0, -1.0 / 6.0);
            const double hi = fc * std::pow (2.0,  1.0 / 6.0);
            if (f >= lo && f <= hi) { b = i; break; }
            const double d = std::abs (f - fc);
            if (d < bestD) { bestD = d; b = i; }
        }
        if (b >= 0)
        {
            info = "Cursor:  ~" + juce::String ((int) std::lround (centres[(size_t) b])) + " Hz band"
                 + altFreqSuffix (app, centres[(size_t) b]);
            for (size_t c = 0; c < thirdOctMags.size() && c < 4; ++c)
                if (b < (int) thirdOctMags[c].size())
                    info += ",  Ch" + juce::String ((int) c + 1) + "="
                          // Band values are stored in the view's relative-dB space.
                          + ScaleCalibration::formatSpectralLevel ((double) thirdOctMags[c][(size_t) b], sc,
                                                                  (c < app.visibleChannels.size()) ? app.visibleChannels[(size_t) c] : -1);
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
                 + altFreqSuffix (app, fExact)
                 + "   [snap Ch" + juce::String (snapCh + 1) + "]"
                 + ScaleCalibration::formatSpectralLevel (-kNormFloorDb * (1.0 - (double) mags[snapCh][bin]), sc,
                                                          (snapCh < (int) app.visibleChannels.size()) ? app.visibleChannels[(size_t) snapCh] : -1);
        }
        else
        {
            info = "Cursor:  " + juce::String ((int) std::lround (f)) + " Hz";
            for (size_t c = 0; c < mags.size() && c < 4; ++c)
            {
                if (mags[c].empty()) continue;
                const int bin = binAt (f);
                info += ",  Ch" + juce::String ((int) c + 1) + "="
                      + ScaleCalibration::formatSpectralLevel (-kNormFloorDb * (1.0 - (double) mags[c][bin]), sc,
                                                              (c < app.visibleChannels.size()) ? app.visibleChannels[(size_t) c] : -1);
            }
            info += altFreqSuffix (app, f);
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
// True when slot i holds data for the CURRENT mode: the classic store in
// modes 0-2, or a curve snapshot saved in exactly this mode in 3-5.
bool FFTAnalysisView::slotHasData (int i) const
{
    if (axisMode_ >= 3)
        return curveSnaps[i].mode == axisMode_ && !curveSnaps[i].data.empty();
    return snapshots[i].n > 0;
}

// Update all eight buttons' stored/toggle/tooltip state for the current mode.
void FFTAnalysisView::refreshSnapshotButtonStates()
{
    for (int i = 0; i < 8; ++i)
    {
        const bool stored = slotHasData (i);
        snapBtns[i].setStored (stored);
        const bool shown = axisMode_ >= 3 ? curveSnaps[i].shown : snapshots[i].shown;
        snapBtns[i].setToggleState (stored && shown, juce::dontSendNotification);
        updateSnapshotTooltip (i);
    }
}

void FFTAnalysisView::handleCurveSnapshotClick (int i, juce::ModifierKeys mods)
{
    auto& cs = curveSnaps[i];
    const bool stored = (cs.mode == axisMode_ && !cs.data.empty());

    if (mods.isShiftDown())
    {
        clearCurveSlot (i);
        repaint();
        return;
    }

    if (!stored || mods.isCommandDown())
    {
        // Empty slot: store the current live curve. Stored slot + Ctrl+click:
        // re-store it from the region currently being analysed.
        if (liveCurves.empty() || liveCurves[0].x.empty())
        {
            snapBtns[i].setToggleState (false, juce::dontSendNotification); // nothing to store yet
            return;
        }

        cs.mode = axisMode_;
        cs.t0 = cachedT0;
        cs.t1 = cachedT1;
        cs.gen = settingsGen;
        cs.track = track_;      // metadata: what was captured at store time
        cs.smooth = smooth_;
        cs.data = liveCurves;   // copy of the current live curves (already tracked/smoothed)
        cs.source = app.doc();  // remember which file this came from
        cs.chans = cachedCurveChans_;   // the channels behind liveCurves
        cs.shown = true;        // newly captured data is shown
    }
    else
    {
        // Plain click on a stored slot: only toggle the overlay, never update.
        cs.shown = !cs.shown;
    }
    invalidateHarmonicCache();

    snapBtns[i].setToggleState (cs.shown, juce::dontSendNotification);
    snapBtns[i].setStored (true);
    updateSnapshotTooltip (i);
    repaint();
}

void FFTAnalysisView::handleSnapshotClick (int i, juce::ModifierKeys mods)
{
    if (axisMode_ >= 3)
        return handleCurveSnapshotClick (i, mods);

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
        // In the dB-SPL pipeline the live cache holds band values only (mags is
        // empty); that is still storable - snapshots rebuild their bands from
        // the source engine.
        const bool pascalStore = pascalThirdOct() && !thirdOctMags.empty();
        if (!pascalStore && (!(cachedN > 1) || mags.empty()))
        {
            snapBtns[i].setToggleState (false, juce::dontSendNotification); // nothing to store yet
            return;
        }

        s.t0 = cachedT0;
        s.t1 = cachedT1;
        s.n  = pascalStore ? 65536 : cachedN;   // the dB-SPL pipeline's fixed frame size
        s.gen = settingsGen;
        s.mags = mags;
        s.dbSpl = pascalStore;
        s.source = app.doc();           // remember which file this came from
        s.chans = cachedChans_;  // channels behind the copied live cache
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
    const juce::String num = juce::String (i + 1);

    if (axisMode_ >= 3)
    {
        auto& cs = curveSnaps[i];
        if (cs.mode == axisMode_ && !cs.data.empty())
        {
            const juce::String from = (cs.source != nullptr) ? (" from " + cs.source->name)
                                                             : juce::String();
            snapBtns[i].setTooltip ("Snapshot " + num + ": stores " + formatTimeCode (cs.t0)
                                    + " - " + formatTimeCode (cs.t1) + from
                                    + ". Click shows/hides the stored curve. Ctrl+Click re-stores it from the current selection/cursor. Shift+Click clears it. Right-click: convert to marker / clear.");
        }
        else
            snapBtns[i].setTooltip ("Snapshot " + num + " is empty in this mode. Click stores the currently analysed curve here and shows it. Snapshots are shared across all open files.");
        return;
    }

    auto& s = snapshots[i];
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

void FFTAnalysisView::clearCurveSlot (int i)
{
    auto& cs = curveSnaps[i];
    cs = CurveSnapshot{};
    invalidateHarmonicCache();
    snapBtns[i].setToggleState (false, juce::dontSendNotification);
    snapBtns[i].setStored (false);
    updateSnapshotTooltip (i);
}

// Rebuild curve slot i from its source file over [t0, t1] using the CURRENT
// track / smoothing options (fixed 2048-point Hann frames in Amp vs Freq mode),
// exactly like classic snapshot slots follow the current settings. Clears the
// slot when the source is gone.
void FFTAnalysisView::recomputeCurveSlot (int i)
{
    auto& cs = curveSnaps[i];
    if (!(cs.mode >= 3 && cs.source != nullptr && app.isDocLoaded (cs.source)))
    { clearCurveSlot (i); return; }

    const double durS = cs.source->engine.duration();
    cs.t0 = std::clamp (cs.t0, 0.0, durS);
    cs.t1 = std::clamp (cs.t1, cs.t0, durS);

    // A spectral selection of the source file masks the analysis (same as live).
    const SpectralSelection* mask = nullptr;
    if (!cs.source->spectralSel.empty())
        mask = &cs.source->spectralSel;

    std::vector<CurveData> out;
    if (cs.mode == 5)
        measures::spectrumCurve (cs.source->engine, cs.chans, cs.t0, cs.t1,
                                 kTimeModeFFT, 1, track_, mask, out);
    else
        measures::timeCurve (cs.source->engine, cs.chans, cs.t0, cs.t1, track_, mask, out);

    if (smooth_ > 1)
        for (auto& cd : out)
        {
            smoothSeries (cd.ampDb, smooth_);
            if (cs.mode != 5) smoothSeries (cd.freqHz, smooth_);
        }

    cs.data = std::move (out);
    cs.gen = settingsGen;
    invalidateHarmonicCache();
    snapBtns[i].setToggleState (cs.shown && !cs.data.empty(), juce::dontSendNotification);
    snapBtns[i].setStored (!cs.data.empty());
    updateSnapshotTooltip (i);
}

void FFTAnalysisView::recomputeCurveSlots()
{
    for (int i = 0; i < 8; ++i)
        if (curveSnaps[i].mode >= 3 && curveSnaps[i].gen != settingsGen)
            recomputeCurveSlot (i);
}

void FFTAnalysisView::recordAndAdjustSnapshotsForDeletion (FileDoc* d, double a, double b,
                                                           Patch& p, bool preserveExistingBefore)
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
        if (preserveExistingBefore && p.snapshotSlots[(size_t) i].changed)
            ch.before = p.snapshotSlots[(size_t) i].before;   // keep the original state
        ch.after.n = s2.n;  ch.after.t0 = s2.t0;  ch.after.t1 = s2.t1;
        ch.after.shown = s2.shown;  ch.after.chans = s2.chans;
        p.snapshotSlots[(size_t) i] = ch;
    }

    // The curve snapshots (modes 3-5) get the same treatment: adjust their time
    // segment, then rebuild from the post-deletion source data.
    for (int i = 0; i < 8; ++i)
    {
        auto& cs = curveSnaps[i];
        if (cs.source != d || cs.mode < 3)   // slots are shared across files
            continue;

        CurveSlotChange ch;
        ch.before.t0 = cs.t0;  ch.before.t1 = cs.t1;
        ch.before.mode = cs.mode;  ch.before.track = track_;  ch.before.smooth = smooth_;
        ch.before.shown = cs.shown;  ch.before.chans = cs.chans;

        bool touched = false;
        if (cs.t1 <= a)
            ;                                   // entirely before: unchanged
        else if (cs.t0 >= b)
        { cs.t0 -= len; cs.t1 -= len; touched = true; }   // entirely after: shift left
        else if (cs.t0 < a && cs.t1 > b)
        { cs.t1 -= len; touched = true; }       // straddles the whole deletion
        else if (cs.t0 < a)
        { cs.t1 = a; touched = true; }          // right part deleted -> [t0, a]
        else if (cs.t1 <= b)
        { clearCurveSlot (i); touched = true; }   // fully contained: cleared
        else
        { cs.t0 = a; cs.t1 -= len; touched = true; }   // left part deleted

        if (!touched)
            continue;

        auto& cs2 = curveSnaps[i];             // may have been reset by clearCurveSlot
        ch.changed = true;
        if (preserveExistingBefore && p.curveSlots[(size_t) i].changed)
            ch.before = p.curveSlots[(size_t) i].before;   // keep the original state
        ch.after.mode = cs2.mode;  ch.after.t0 = cs2.t0;  ch.after.t1 = cs2.t1;
        ch.after.track = track_;  ch.after.smooth = smooth_;
        ch.after.shown = cs2.shown;  ch.after.chans = cs2.chans;
        p.curveSlots[(size_t) i] = ch;

        if (cs2.mode >= 3 && app.isDocLoaded (d))
            recomputeCurveSlot (i);   // rebuild from the post-deletion source data
    }
}

void FFTAnalysisView::recordAndAdjustSnapshotsForStretch (FileDoc* d, double a,
                                                          double bOld, double bNew,
                                                          Patch& p)
{
    if (d == nullptr || !(bOld > a))
        return;
    const double k = (bNew - a) / (bOld - a);
    auto W = [a, bOld, bNew, k] (double t) -> double
    {
        if (t < a) return t;
        if (t <= bOld) return a + (t - a) * k;
        return t + (bNew - bOld);
    };

    for (int i = 0; i < 8; ++i)
    {
        auto& s = snapshots[i];
        if (s.source != d)   // slots are shared across files: only adjust this one's
            continue;

        const double nt0 = W (s.t0), nt1 = W (s.t1);
        if (nt0 == s.t0 && nt1 == s.t1)
            continue;   // entirely outside the stretched range: unchanged

        SnapshotSlotChange ch;
        ch.before.t0 = s.t0;  ch.before.t1 = s.t1;
        ch.before.n = s.n;    ch.before.shown = s.shown;
        ch.before.chans = s.chans;
        ch.changed = true;

        s.t0 = nt0;
        s.t1 = nt1;
        ch.after.n = s.n;  ch.after.t0 = s.t0;  ch.after.t1 = s.t1;
        ch.after.shown = s.shown;  ch.after.chans = s.chans;
        p.snapshotSlots[(size_t) i] = ch;

        if (app.isDocLoaded (d))
            recomputeSnapshotSlot (i);   // rebuild from the post-stretch source data
    }

    for (int i = 0; i < 8; ++i)
    {
        auto& cs = curveSnaps[i];
        if (cs.source != d || cs.mode < 3)   // slots are shared across files
            continue;

        const double nt0 = W (cs.t0), nt1 = W (cs.t1);
        if (nt0 == cs.t0 && nt1 == cs.t1)
            continue;   // entirely outside the stretched range: unchanged

        CurveSlotChange ch;
        ch.before.t0 = cs.t0;  ch.before.t1 = cs.t1;
        ch.before.mode = cs.mode;  ch.before.track = track_;  ch.before.smooth = smooth_;
        ch.before.shown = cs.shown;  ch.before.chans = cs.chans;
        ch.changed = true;

        cs.t0 = nt0;
        cs.t1 = nt1;
        ch.after.mode = cs.mode;  ch.after.t0 = cs.t0;  ch.after.t1 = cs.t1;
        ch.after.track = track_;  ch.after.smooth = smooth_;
        ch.after.shown = cs.shown;  ch.after.chans = cs.chans;
        p.curveSlots[(size_t) i] = ch;

        if (app.isDocLoaded (d))
            recomputeCurveSlot (i);   // rebuild from the post-stretch source data
    }
}

void FFTAnalysisView::recordAndAdjustSnapshotsForInsertion (FileDoc* d, double a,
                                                            double lenSecs, Patch& p)
{
    if (d == nullptr || !(lenSecs > 0.0))
        return;

    for (int i = 0; i < 8; ++i)
    {
        auto& s = snapshots[i];
        if (s.source != d)   // slots are shared across files: only adjust this one's
            continue;

        bool touched = false;
        double nt0 = s.t0, nt1 = s.t1;
        if (s.t1 <= a)
            ;                                   // entirely before: unchanged
        else if (s.t0 >= a)
        { nt0 += lenSecs; nt1 += lenSecs; touched = true; }   // entirely after: shift right
        else
        {
            // Straddles the insertion point: keep the part before it - that audio
            // is unchanged, so the stored curve still matches [t0, a].
            nt1 = a;
            touched = (a > s.t0);
        }

        if (!touched)
            continue;

        SnapshotSlotChange ch;
        ch.before.t0 = s.t0;  ch.before.t1 = s.t1;
        ch.before.n = s.n;    ch.before.shown = s.shown;
        ch.before.chans = s.chans;
        ch.changed = true;

        s.t0 = nt0;
        s.t1 = nt1;
        ch.after.n = s.n;  ch.after.t0 = s.t0;  ch.after.t1 = s.t1;
        ch.after.shown = s.shown;  ch.after.chans = s.chans;
        p.snapshotSlots[(size_t) i] = ch;

        if (app.isDocLoaded (d))
            recomputeSnapshotSlot (i);   // rebuild from the post-insertion source data
    }

    for (int i = 0; i < 8; ++i)
    {
        auto& cs = curveSnaps[i];
        if (cs.source != d || cs.mode < 3)   // slots are shared across files
            continue;

        bool touched = false;
        double nt0 = cs.t0, nt1 = cs.t1;
        if (cs.t1 <= a)
            ;                                   // entirely before: unchanged
        else if (cs.t0 >= a)
        { nt0 += lenSecs; nt1 += lenSecs; touched = true; }   // entirely after: shift right
        else
        {
            nt1 = a;
            touched = (a > cs.t0);
        }

        if (!touched)
            continue;

        CurveSlotChange ch;
        ch.before.t0 = cs.t0;  ch.before.t1 = cs.t1;
        ch.before.mode = cs.mode;  ch.before.track = track_;  ch.before.smooth = smooth_;
        ch.before.shown = cs.shown;  ch.before.chans = cs.chans;
        ch.changed = true;

        cs.t0 = nt0;
        cs.t1 = nt1;
        ch.after.mode = cs.mode;  ch.after.t0 = cs.t0;  ch.after.t1 = cs.t1;
        ch.after.track = track_;  ch.after.smooth = smooth_;
        ch.after.shown = cs.shown;  ch.after.chans = cs.chans;
        p.curveSlots[(size_t) i] = ch;

        if (app.isDocLoaded (d))
            recomputeCurveSlot (i);   // rebuild from the post-insertion source data
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

    for (int i = 0; i < 8; ++i)
    {
        const auto& ch = p.curveSlots[(size_t) i];
        if (!ch.changed)
            continue;
        const CurveSnapshotMeta& m = isRedo ? ch.after : ch.before;

        // Never clobber a slot that now holds another file's snapshot.
        auto& cs = curveSnaps[i];
        if (cs.source != nullptr && cs.source != d)
            continue;

        if (m.mode < 3 || !app.isDocLoaded (d))
        { clearCurveSlot (i); continue; }

        cs.t0 = m.t0;  cs.t1 = m.t1;
        cs.mode = m.mode;
        cs.chans = m.chans;  cs.shown = m.shown;
        cs.source = d;
        recomputeCurveSlot (i);
    }
}

void FFTAnalysisView::clearSnapshotsFor (const FileDoc* d)
{
    for (int i = 0; i < 8; ++i)
    {
        if (snapshots[i].source == d)
            clearSnapshotSlot (i);
        if (curveSnaps[i].source == d)
            clearCurveSlot (i);
    }
}

bool FFTAnalysisView::convertSnapshotToMarker (int i)
{
    if (axisMode_ >= 3)
    {
        auto& cs = curveSnaps[i];
        if (!(cs.mode == axisMode_ && !cs.data.empty()) || cs.source == nullptr) return false;

        const double dur = cs.source->engine.duration();
        const double t0 = std::clamp (cs.t0, 0.0, dur);
        const double t1 = std::clamp (cs.t1, t0, dur);
        if (t1 <= t0) return false;

        cs.source->markers.addRegionMarker (t0, t1, "Snapshot " + juce::String (i + 1));
        if (app.doc() == cs.source && app.onStateChange)
            app.onStateChange ();   // refresh the marker list when that file is active
        return true;
    }

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
    juce::PopupMenu m;
    const bool hasData = slotHasData (i) && (axisMode_ >= 3
        ? !curveSnaps[i].data.empty()
        : snapshots[i].n > 1);
    m.addItem (1, "Convert to marker", hasData, false);
    m.addItem (2, "Clear",
               axisMode_ >= 3 ? curveSnaps[i].mode >= 0
                              : (snapshots[i].n > 0 || snapshots[i].shown),
               false);

    // "Clear All": wipe every stored curve of the active store type at once.
    bool anyStored = false;
    if (axisMode_ >= 3)
        for (int s = 0; s < 8 && !anyStored; ++s)
            if (curveSnaps[s].mode >= 0) { anyStored = true; break; }
    else
        for (int s = 0; s < 8 && !anyStored; ++s)
            if (snapshots[s].n > 0 || snapshots[s].shown) { anyStored = true; break; }
    m.addItem (3, "Clear All", anyStored, false);

    const int r = m.showMenu (juce::PopupMenu::Options().withMousePosition());
    if (r == 1) convertSnapshotToMarker (i);
    else if (r == 2)
    {
        if (axisMode_ >= 3) clearCurveSlot (i);
        else                clearSnapshotSlot (i);
        repaint();
    }
    else if (r == 3)
    {
        clearAllStoredCurves();
        repaint();
    }
}

// Right-click context menu. When the click landed on an axis, "Zoom Out
// Full" resets only that range instead of both.
void FFTAnalysisView::showContextMenu (bool onLevelAxis, bool onFreqAxis, int clickX)
{
    const bool haveSpec = app.audioEngine().isFileLoaded()
                          && cachedN > 1 && !mags.empty();

    juce::PopupMenu m;
    // Checkable: the tick shows whether new selections auto zoom-fit. Clicking
    // performs an immediate fit and toggles the auto mode.
    m.addItem (CtxZoomOutFull, "Zoom Fit", true, zoomFitAuto_);
    m.addSeparator();
    m.addItem (100, "Copy");
    if (axisMode_ < 3)   // harmonic references are a classic-mode feature
    {
        m.addSeparator();
        m.addItem (CtxSetHarmonicRefs, "Activate Harmonics", haveSpec);
        m.addItem (CtxCopyHarmonics, "Copy Harmonics",
                   harmonicRefActive_ && haveSpec);
        m.addItem (CtxResetHarmonic, "Clear Harmonics", harmonicRefActive_);
    }
    if (axisMode_ == 3)   // "Check Harmonic Ratio" is a Freq vs Time feature
    {
        bool haveCurveData = (!liveCurves.empty() && !liveCurves[0].x.empty());
        for (int i = 0; i < 8 && !haveCurveData; ++i)
            if (curveSnaps[i].shown && curveSnaps[i].mode == axisMode_
                && !curveSnaps[i].data.empty())
                haveCurveData = true;
        m.addSeparator();
        m.addItem (CtxCheckHarmonicRatio, "Check Harmonic Ratio",
                   haveCurveData, harmonicRatioActive_);
    }
    const int sel = m.showMenu (juce::PopupMenu::Options ().withMousePosition());
    switch (sel)
    {
        case CtxZoomOutFull:
            // Toggles auto zoom-fit, then performs an immediate fit (the old
            // one-shot "Zoom Out Full" behaviour). The right strip is always the
            // vertical axis and the bottom strip the horizontal one (in Freq vs
            // Time the vertical happens to be frequency); a click on the plot or
            // a corner resets both.
            setZoomFitAuto (!zoomFitAuto_);
            if (onLevelAxis && !onFreqAxis)      fitVerticalRange();
            else if (onFreqAxis && !onLevelAxis) fitHorizontalRange();
            else { fitVerticalRange(); fitHorizontalRange(); }
            repaint();
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
        case CtxCheckHarmonicRatio:
            setHarmonicRatioMode (!harmonicRatioActive_);
            break;
    }
}

void FFTAnalysisView::setHarmonicReference (int n, double anchorHz, int maxK)
{
    if (n < 1 || !(anchorHz > 0.0) || !std::isfinite (anchorHz)) return;
    harmonicRefActive_ = true;
    harmonicN_ = n;
    harmonicAnchorF_ = anchorHz;
    harmonicMaxK_ = std::max (1, maxK);
    repaint();
}

void FFTAnalysisView::resetHarmonicReference()
{
    if (!harmonicRefActive_) return;
    harmonicRefActive_ = false;
    harmonicN_ = 1;
    harmonicAnchorF_ = 0.0;
    harmonicMaxK_ = 10;
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

    // Amplitude comes from the first visible channel at the nearest FFT bin.
    const auto& mag = mags.front();
    const int magCh = cachedChans_.empty() ? -1 : cachedChans_.front();

    // Same level formatting as "Copy Spectrum to Clipboard": absolute linear
    // values for volts/grams files, otherwise dB relative to full scale - with
    // that channel's own full-scale value (per-channel "bkdk" calibration).
    const ScaleCalibration& sc = app.scale();
    const double off = sc.levelOffsetDb (magCh);
    auto fmtLevel = [&] (double relDb) -> juce::String
    {
        if (sc.usesLinearLevels())
            return ScaleCalibration::formatLevelValue (sc.fullScaleFor (magCh) * std::pow (10.0, relDb / 20.0), 6);
        return juce::String (relDb + off, 2);
    };
    // Harmonics 1..N at the bin nearest to k * fFund (shared with the
    // measure.harmonics command through the measurement module).
    std::vector<double> relDb (mag.size());
    for (size_t k = 0; k < mag.size(); ++k)
        relDb[k] = measures::normToDb (mag[k]);
    measures::HarmonicsParams hp;
    hp.anchorHz = harmonicAnchorF_;
    hp.anchorN = harmonicN_;
    hp.maxK = harmonicN_;
    hp.search = measures::HarmonicsParams::Search::NearestBin;
    const auto hr = measures::harmonics (relDb, rate, cachedN, hp);

    juce::StringArray lines;
    lines.add ("Harmonic\tFrequency\tAmplitude");
    for (const auto& h : hr.harmonics)
    {
        juce::String line = juce::String (h.k) + "\t" + juce::String (h.expectedHz, 3);
        if (!h.valid)
            line += "\t";   // no FFT data above Nyquist: leave the cell empty
        else
            line += "\t" + fmtLevel (h.relDb);
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

// Builds the "Set Harmonic References" dialog without running it modally
// (shared by showSetHarmonicDialog and the unit tests). Caller owns the window.
juce::AlertWindow* FFTAnalysisView::createSetHarmonicDialog ()
{
    auto* w = new juce::AlertWindow ("Set Harmonic References",
                                     "Set the harmonic number indicated by the mouse (1 = fundamental)\n"
                                     "and how many harmonics to mark with vertical lines.",
                                     juce::AlertWindow::QuestionIcon, this);
    // The reference frequency itself is not a field: it starts at the
    // frequency under the cursor and follows the mouse while active.
    w->addTextEditor ("Harmonic number of the mouse:", "1");
    w->addTextEditor ("Number of harmonics to show:", "10");
    for (const char* name : { "Harmonic number of the mouse:",
                              "Number of harmonics to show:" })
        if (auto* editor = w->getTextEditor (name))
            // Let Enter/Escape reach the OK/Cancel shortcuts even while a
            // field has keyboard focus.
            editor->setEscapeAndReturnKeysConsumed (false);

    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    // AlertWindow disables keyboard focus on its editors; re-enable it and
    // pre-select the default so a plain Enter accepts harmonic #1.
    if (auto* editor = w->getTextEditor ("Harmonic number of the mouse:"))
    {
        editor->setWantsKeyboardFocus (true);
        editor->selectAll();
    }
    return w;
}

void FFTAnalysisView::showSetHarmonicDialog (double clickFreqHz)
{
    auto* w = createSetHarmonicDialog ();

    // Enter modal state WITHOUT taking keyboard focus (see showNewFileDialog).
    w->enterModalState (false);
    if (auto* editor = w->getTextEditor ("Harmonic number of the mouse:"))
        editor->grabKeyboardFocus();

    const int result = w->runModalLoop();
    const juce::String text = w->getTextEditorContents ("Harmonic number of the mouse:")
                                 .trim ().removeCharacters ("., ");
    const juce::String maxText = w->getTextEditorContents ("Number of harmonics to show:")
                                     .trim ().removeCharacters ("., ");
    delete w;
    if (result != 1) return;

    // The reference frequency is the one under the cursor when the menu was
    // opened, falling back to the current anchor; it then follows the mouse.
    double refHz = clickFreqHz;
    if (!(refHz > 0.0) || !std::isfinite (refHz))
        refHz = harmonicAnchorF_;
    if (!(refHz > 0.0) || !std::isfinite (refHz))
        return;   // no usable reference frequency: nothing to anchor to

    const int n = text.isEmpty () ? 0 : text.getIntValue ();
    if (n < 1 || n > 1000)
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Set Harmonic References",
                                            "Please enter an integer between 1 and 1000.",
                                            "", this);
        return;
    }

    const int maxK = maxText.isEmpty () ? 10 : maxText.getIntValue ();
    if (maxK < 1 || maxK > 1000)
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Set Harmonic References",
                                            "Please enter an integer between 1 and 1000 for the\n"
                                            "number of harmonics to show.",
                                            "", this);
        return;
    }

    setHarmonicReference (n, refHz, maxK);
}

// ---------------------------------------------------------------------------
// "Check Harmonic Ratio" mode (Freq vs Time only).
//
// Every displayed curve - the live one plus stored curves 1-8 - is divided by
// the curve with the lowest average frequency (the fundamental), so a signal
// carrying exactly the k-th harmonic of another one plots as a nearly flat
// line at "kx". While active, the vertical window [fLo_, fHi_] holds ratio
// values and the gridlines are labelled 1x, 2x, ...

void FFTAnalysisView::refreshHarmonicCache() const
{
    if (harmonicCache_.computed) return;
    harmonicCache_ = HarmonicRatioCache{};

    // All candidate curves: the live per-channel curves plus every stored
    // curve shown in this mode.
    struct Cand { const CurveData* cd; };
    std::vector<Cand> cands;
    for (size_t c = 0; c < liveCurves.size() && c < 4; ++c)
        if (!liveCurves[c].x.empty()) cands.push_back ({ &liveCurves[c] });
    for (int i = 0; i < 8; ++i)
        if (curveSnaps[i].shown && curveSnaps[i].mode == axisMode_)
            for (size_t c = 0; c < curveSnaps[i].data.size() && c < 4; ++c)
                if (!curveSnaps[i].data[c].x.empty())
                    cands.push_back ({ &curveSnaps[i].data[c] });

    // Average tracked frequency per candidate (finite positive values only).
    auto avgFreq = [] (const CurveData& cd, double& outAvg) -> bool
    {
        double sum = 0.0; int n = 0;
        for (float f : cd.freqHz)
            if (std::isfinite (f) && f > 1e-6) { sum += f; ++n; }
        outAvg = (n > 0) ? sum / (double) n : 0.0;
        return n > 0;
    };

    const CurveData* fund = nullptr;
    double fundAvg = 0.0;
    for (const auto& c : cands)
    {
        double a = 0.0;
        if (!avgFreq (*c.cd, a)) continue;
        if (fund == nullptr || a < fundAvg) { fund = c.cd; fundAvg = a; }
    }

    harmonicCache_.computed = true;
    if (fund == nullptr) return;   // no curve with frequency data: nothing to divide by

    harmonicCache_.hasFundamental = true;
    harmonicCache_.fundAvgHz = fundAvg;

    // The fundamental's tracked frequency at an arbitrary time t: linear
    // interpolation between neighbouring points, clamped outside its range.
    const CurveData& F = *fund;
    auto fundAt = [&] (double t) -> double
    {
        if (F.x.size() == 1) return F.freqHz[0];
        if (t <= F.x.front()) return F.freqHz[0];
        if (t >= F.x.back())  return F.freqHz[F.x.size() - 1];
        size_t lo = 0, hi = F.x.size();
        while (hi - lo > 1)
        {
            const size_t mid = (lo + hi) / 2;
            if (F.x[mid] <= t) lo = mid; else hi = mid;
        }
        const double x0 = F.x[lo], x1 = F.x[hi];
        const double u = (x1 - x0 > 1e-9) ? (t - x0) / (x1 - x0) : 0.0;
        return F.freqHz[lo] + u * ((double) F.freqHz[hi] - F.freqHz[lo]);
    };

    auto transform = [&] (const CurveData& cd, CurveData& out)
    {
        out.x = cd.x;
        out.ampDb = cd.ampDb;
        out.freqHz.resize (cd.x.size());
        for (size_t i = 0; i < cd.x.size(); ++i)
        {
            const double f0 = fundAt (cd.x[i]);
            out.freqHz[i] = (f0 > 1e-6 && std::isfinite (cd.freqHz[i]))
                            ? (float) (cd.freqHz[i] / f0) : 0.0f;
        }
    };

    harmonicCache_.live.resize (liveCurves.size());
    for (size_t c = 0; c < liveCurves.size(); ++c)
        transform (liveCurves[c], harmonicCache_.live[(size_t) c]);
    for (int i = 0; i < 8; ++i)
        if (curveSnaps[i].shown && curveSnaps[i].mode == axisMode_)
        {
            harmonicCache_.snaps[i].resize (curveSnaps[i].data.size());
            for (size_t c = 0; c < curveSnaps[i].data.size(); ++c)
                transform (curveSnaps[i].data[c], harmonicCache_.snaps[i][(size_t) c]);
        }
}

void FFTAnalysisView::setHarmonicRatioMode (bool on)
{
    if (axisMode_ != 3) on = false;   // Freq vs Time mode only
    if (on == harmonicRatioActive_) return;
    harmonicRatioActive_ = on;
    refreshHarmonicCache();
    fitVerticalRange();             // zoom-fit when entering and leaving this mode
    repaint();
}

void FFTAnalysisView::clearAllStoredCurves()
{
    if (axisMode_ >= 3)
        for (int i = 0; i < 8; ++i) clearCurveSlot (i);
    else
        for (int i = 0; i < 8; ++i) clearSnapshotSlot (i);
}

void FFTAnalysisView::panHorizontalByCursorX (int x)
{
    // Half-speed grab of the horizontal range: the delta comes from the
    // previous delivered cursor position and that anchor always advances, so
    // the limits below never leave a dead zone to unwind when the drag
    // reverses. Same scheme as the waveform/spectral middle-drag.
    const auto plot = plotBounds();
    if (plot.getWidth() > 0)
    {
        if (isTimeAxisMode ())
        {
            const double d = kPanSpeedFactor * (xToTime (panLastX) - xToTime (x));
            curveViewT0_ += d;
            curveViewT1_ += d;
            if (curveViewT0_ < cachedT0) { curveViewT1_ += (cachedT0 - curveViewT0_); curveViewT0_ = cachedT0; }
            if (curveViewT1_ > cachedT1) { curveViewT0_ -= (curveViewT1_ - cachedT1); curveViewT1_ = cachedT1; }
            userChangedDisplayRange ();
        }
        else if (axisMode_ == 1)
        {
            // Log axis: pan in log space (half the full cursor step = square
            // root of the ratio between the two cursor positions), so the zoom
            // level is preserved.
            const double fPrev = xToFreq (panLastX), fNow = xToFreq (x);
            if (fPrev > 0.0 && fNow > 0.0)
            {
                const double R = std::sqrt (std::max (1e-30, fPrev / fNow));
                fLo_ *= R;
                fHi_ *= R;
            }
            const double nyq = app.audioEngine().sampleRate() * 0.5;
            if (fLo_ < 1.0) { fHi_ += (1.0 - fLo_); fLo_ = 1.0; }
            if (fHi_ > nyq) { fLo_ -= (fHi_ - nyq); fHi_ = nyq; }
            userChangedDisplayRange ();
        }
        else
        {
            const double d = kPanSpeedFactor * (xToFreq (panLastX) - xToFreq (x));
            fLo_ += d;
            fHi_ += d;
            const double nyq = app.audioEngine().sampleRate() * 0.5;
            if (fLo_ < 0.0) { fHi_ -= fLo_; fLo_ = 0.0; }
            if (fHi_ > nyq) { fLo_ -= (fHi_ - nyq); fHi_ = nyq; }
            userChangedDisplayRange ();
        }
    }
    panLastX = x;
}

void FFTAnalysisView::panVerticalByCursorY (int y)
{
    // Half-speed grab of the vertical range: level (dB) in every mode except
    // Freq vs Time, where the vertical axis is frequency (or a harmonic
    // ratio). Content follows the cursor: dragging down moves the curves down.
    const auto plot = plotBounds();
    if (plot.getHeight() > 0)
    {
        if (axisMode_ == 3)
        {
            const double d = kPanSpeedFactor * (yToFreq ((float) panLastY) - yToFreq ((float) y));
            fLo_ += d;
            fHi_ += d;
            const double vMax = harmonicRatioUsable() ? 1024.0
                                                      : app.audioEngine().sampleRate() * 0.5;
            if (fLo_ < 0.0) { fHi_ -= fLo_; fLo_ = 0.0; }
            if (fHi_ > vMax) { fLo_ -= (fHi_ - vMax); fHi_ = vMax; }
            userChangedDisplayRange ();
        }
        else
        {
            const double d = kPanSpeedFactor * (yToLevel ((float) panLastY) - yToLevel ((float) y));
            dbTop_ += d;
            dbBottom_ += d;
            if (dbTop_ > 40.0) { dbBottom_ -= (dbTop_ - 40.0); dbTop_ = 40.0; }
            if (dbBottom_ < -240.0) { dbTop_ += (-240.0 - dbBottom_); dbBottom_ = -240.0; }
        }
    }
    panLastY = y;
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

    if (!e.mods.isLeftButtonDown() || e.mods.isRightButtonDown())
    {
        if (e.mods.isMiddleButtonDown() && !e.mods.isRightButtonDown())
        {
            // Middle-drag pans both axes at once: the horizontal range (time
            // in the curve modes, frequency in the classic ones) and the
            // vertical one (level, or frequency in Freq vs Time), both at half
            // cursor speed with the same scheme as the axis strips.
            dragMode = DragMode::PanXY;
            dragStartX = e.x;
            dragStartY = e.y;
            panLastX = e.x;
            panLastY = e.y;
        }
        return;
    }

    // Left-drag on the bottom axis pans the horizontal range (frequency in
    // the classic modes, the time view window in the time-based curve modes);
    // left-drag on the right axis pans the vertical range.
    if (e.y > plot.getBottom() && e.x >= plot.getX() && e.x <= plot.getRight())
        dragMode = isTimeAxisMode() ? DragMode::PanTime : DragMode::PanFreq;
    else if (e.x > plot.getRight() && e.y >= plot.getY() && e.y <= plot.getBottom())
        dragMode = (axisMode_ == 3) ? DragMode::PanFreqV : DragMode::PanLevel;
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
            userChangedDisplayRange ();
            repaint();
            break;
        }
        case DragMode::PanFreqV:
        {
            // Freq vs Time: the vertical axis is frequency (or a harmonic
            // ratio in "Check Harmonic Ratio" mode).
            const double f0 = yToFreq ((float) dragStartY);
            const double f1 = yToFreq ((float) e.y);
            const double d = f0 - f1;
            fLo_ += d; fHi_ += d;
            // Keep the window inside [0, Nyquist] (or [0, 1024] in ratio mode).
            const double vMax = harmonicRatioUsable() ? 1024.0
                                                      : app.audioEngine().sampleRate() * 0.5;
            if (fLo_ < 0.0) { fHi_ -= fLo_; fLo_ = 0.0; }
            if (fHi_ > vMax) { fLo_ -= (fHi_ - vMax); fHi_ = vMax; }
            userChangedDisplayRange ();
            repaint();
            break;
        }
        case DragMode::PanLevel:
        {
            const double db0 = yToLevel ((float) dragStartY);
            const double db1 = yToLevel ((float) e.y);
            const double d = db0 - db1;
            dbTop_ += d; dbBottom_ += d;
            userChangedDisplayRange ();
            // Keep the window inside absolute limits.
            if (dbTop_ > 40.0) { dbBottom_ -= (dbTop_ - 40.0); dbTop_ = 40.0; }
            if (dbBottom_ < -240.0) { dbTop_ += (-240.0 - dbBottom_); dbBottom_ = -240.0; }
            repaint();
            break;
        }
        case DragMode::PanTime:
        {
            // Pan the horizontal time window inside the analysed region.
            const double d = xToTime (dragStartX) - xToTime ((int) e.x);
            curveViewT0_ += d;
            curveViewT1_ += d;
            userChangedDisplayRange ();
            if (curveViewT0_ < cachedT0) { curveViewT1_ -= curveViewT0_; curveViewT0_ = cachedT0; }
            if (curveViewT1_ > cachedT1) { curveViewT0_ -= (curveViewT1_ - cachedT1); curveViewT1_ = cachedT1; }
            repaint();
            break;
        }
        case DragMode::PanXY:
        {
            // Middle-drag: both axes, half speed, the delta measured from the
            // last delivered cursor position (the anchor advances), so a
            // clamped edge never accumulates hidden offset and reversing
            // responds at once.
            panHorizontalByCursorX (e.x);
            panVerticalByCursorY (e.y);
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
            // axis only that axis. In the time-based curve modes the bottom
            // axis is the horizontal time window of the analysed region.
            const bool startOnBottomAxis = dragStartY >= plot.getBottom();
            const bool startOnRightAxis  = !startOnBottomAxis && dragStartX > plot.getRight();

            // Horizontal part of the box: frequency range in the classic modes
            // and Amp vs Freq, or the time view window in the time-based modes.
            if (!startOnRightAxis && zoomRect.getWidth() >= 2)
            {
                if (isTimeAxisMode())
                {
                    const double tA = xToTime (std::clamp (zoomRect.getX(), plot.getX(), plot.getRight()));
                    const double tB = xToTime (std::clamp (zoomRect.getRight() - 1, plot.getX(), plot.getRight()));
                    if (tB > tA + 1e-9)
                    {
                        curveViewT0_ = std::clamp (tA, cachedT0, cachedT1);
                        curveViewT1_ = std::max (curveViewT0_ + 1e-9,
                                                 std::clamp (tB, cachedT0, cachedT1));
                    }
                }
                else if (axisMode_ != 2)
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
            }

            // Vertical part of the box: frequency window in Freq vs Time mode,
            // level window everywhere else. Frequency increases upward, so
            // the rect's TOP edge is the high end and its BOTTOM edge the low.
            if (!startOnBottomAxis && zoomRect.getHeight() >= 2)
                {
                    if (axisMode_ == 3)
                    {
                        const double fTop = yToFreq ((float) std::clamp (zoomRect.getY(), plot.getY(), plot.getBottom()));
                        const double fBot = yToFreq ((float) std::clamp (zoomRect.getBottom() - 1, plot.getY(), plot.getBottom()));
                        if (fTop > fBot * 1.0001)
                        {
                            const double nyq = app.audioEngine().sampleRate() * 0.5;
                            fLo_ = std::clamp (fBot, 0.0, nyq);
                            fHi_ = std::clamp (fTop, 0.0, nyq);
                        }
                    }
                    else
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

            // A manual box zoom disables auto zoom-fit.
            userChangedDisplayRange ();
        }
    }

    dragMode = DragMode::None;
    haveZoomRect = false;
    zoomRect = {};
}

void FFTAnalysisView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto plot = plotBounds();
    if (e.y < plot.getY())
        return;   // ignore the control strip above the plot

    // Same direction as the waveform view: wheel up zooms in, down zooms out.
    const double factor = std::pow (1.25, wheel.deltaY);
    if (isTimeAxisMode())
    {
        // Wheel over the vertical axis strip (right side) or Shift+wheel
        // anywhere zooms only that axis - frequency in Freq vs Time, level in
        // Amp vs Time; everywhere else (plot and bottom time ruler) it zooms
        // only the horizontal time window around the cursor, like the other
        // graphs.
        if (e.mods.isShiftDown() || e.x > plot.getRight())
            (axisMode_ == 3) ? zoomFreqAtY ((float) e.y, factor)
                             : zoomLevelAtY ((float) e.y, factor);
        else
            zoomTimeAtX (e.x, factor);
    }
    else if (e.mods.isShiftDown() || e.x > plot.getRight())
        zoomLevelAtY ((float) e.y, factor);
    else
        zoomFreqAtX (e.x, factor);

    // A manual wheel zoom disables auto zoom-fit.
    userChangedDisplayRange ();
}

void FFTAnalysisView::copySpectrumToClipboard()
{
    const juce::String sep = "	";   // tab-separated columns (Excel-friendly)

    const double rate = app.audioEngine().sampleRate();
    // Export the currently visible frequency window.
    const double fLo = fLo_, fHi = fHi_;

    // With a scale calibration the exported values are absolute levels;
    // volts/grams files export linear physical values instead (unit in header).
    // Each column uses its own channel's full-scale value: legacy "bkdk" files
    // carry one calibration per channel.
    const ScaleCalibration& sc = app.scale();
    auto fmtLevel = [&] (double relDb, int ch) -> juce::String
    {
        if (sc.usesLinearLevels())
            return ScaleCalibration::formatLevelValue (sc.fullScaleFor (ch) * std::pow (10.0, relDb / 20.0), 6);
        return juce::String (relDb + sc.levelOffsetDb (ch), 2);
    };
    // Source channel of data column c (live caches are in display order).
    auto chanOf = [] (const std::vector<int>& chans, size_t c) -> int
    {
        return c < chans.size() ? chans[c] : -1;
    };

    // Alternative frequency scales: one extra column per enabled reference,
    // converting every exported Hz value exactly like the axis overlay does
    // (alt(x) = x * altValue / refMain). No columns are added when none is set.
    std::vector<const Application::AltScaleRef*> altRefs;
    for (const auto& r : app.altFreqRefs)
        if (r.enabled && std::abs (r.refMain) > 1e-9)
            altRefs.push_back (&r);

    // Appends ",<value>" for every alternative reference to a data line.
    auto appendAlts = [&] (juce::String& line, double freqHz)
    {
        for (const auto* r : altRefs)
            line += sep + juce::String (Application::altScaleValue (*r, freqHz), 3);
    };

    // Appends the alternative-reference column names to a header.
    auto appendAltHeaders = [&] (juce::String& header, const juce::String& prefix)
    {
        for (const auto* r : altRefs)
            header += sep + prefix + r->name;
    };

    // Curve modes export the points of the currently displayed curve instead
    // of a bin table (only what is on screen): Freq vs Time: time, per
    // channel freq+level; Amp vs Time: time, per channel level+freq;
    // Amp vs Freq: frequency, per channel level.
    if (axisMode_ >= 3 && !liveCurves.empty() && !liveCurves[0].x.empty())
    {
        // Harmonic-ratio mode exports the ratio values instead of Hz.
        const bool hRatio = (axisMode_ == 3) && harmonicRatioUsable();
        const std::vector<CurveData>& lv = hRatio ? harmonicCache_.live : liveCurves;

        juce::StringArray lines;
        const int nCh = std::min ((int) lv.size(), 4);
        juce::String header = isTimeAxisMode() ? "time_s" : "freq_Hz";
        if (!isTimeAxisMode())
            appendAltHeaders (header, "");   // Amp vs Freq: shared frequency axis
        for (int c = 0; c < nCh; ++c)
        {
            const juce::String chName = "ch" + juce::String (c + 1) + "_";
            if (axisMode_ == 3 && hRatio)
            {
                header += sep + chName + "x";
                header += sep + chName + sc.spectralUnitLabel();
            }
            else if (axisMode_ == 3)
            {
                header += sep + chName + "Hz";
                appendAltHeaders (header, chName);
                header += sep + chName + sc.spectralUnitLabel();
            }
            else if (axisMode_ == 4)
            {
                header += sep + chName + sc.spectralUnitLabel() + sep + chName + "Hz";
                appendAltHeaders (header, chName);
            }
            else
                header += sep + chName + sc.spectralUnitLabel();
        }
        lines.add (header);

        // Only the points currently on screen are exported: the visible time
        // window in the time-based modes, the visible frequency window in
        // Amp vs Freq.
        std::vector<size_t> idxs;
        const size_t nPts = lv[0].x.size();
        for (size_t i = 0; i < nPts; ++i)
        {
            const double xv = lv[0].x[i];
            if (isTimeAxisMode())
            {
                if (xv >= curveViewT0_ - 1e-9 && xv <= curveViewT1_ + 1e-9)
                    idxs.push_back (i);
            }
            else if (xv >= fLo_ - 1e-6 && xv <= fHi_ + 1e-6)
                idxs.push_back (i);
        }

        for (size_t k = 0; k < idxs.size(); ++k)
        {
            const size_t i = idxs[k];
            juce::String line = isTimeAxisMode() ? juce::String (lv[0].x[i], 6)
                                                 : juce::String (lv[0].x[i], 3);
            if (!isTimeAxisMode())
                appendAlts (line, lv[0].x[i]);   // Amp vs Freq: shared axis
            for (int c = 0; c < nCh; ++c)
            {
                const auto& cd = lv[(size_t) c];
                if (i >= cd.x.size()) continue;   // defensive: channels are equal-sized
                const int ch = chanOf (cachedCurveChans_, (size_t) c);
                if (axisMode_ == 3 && hRatio)
                {
                    line += sep + juce::String (cd.freqHz[i], 3);
                    line += sep + fmtLevel (cd.ampDb[i], ch);
                }
                else if (axisMode_ == 3)
                {
                    line += sep + juce::String (cd.freqHz[i], 2);
                    appendAlts (line, cd.freqHz[i]);
                    line += sep + fmtLevel (cd.ampDb[i], ch);
                }
                else if (axisMode_ == 4)
                {
                    line += sep + fmtLevel (cd.ampDb[i], ch) + sep + juce::String (cd.freqHz[i], 2);
                    appendAlts (line, cd.freqHz[i]);
                }
                else
                    line += sep + fmtLevel (cd.ampDb[i], ch);
            }
            lines.add (line);
        }

        juce::SystemClipboard::copyTextToClipboard (lines.joinIntoString ("\n"));
        return;
    }

    juce::StringArray lines;
    juce::String header = "freq_Hz";
    appendAltHeaders (header, "");
    for (size_t c = 0; c < mags.size() && c < 4; ++c)
        header += sep + "ch" + juce::String ((int) c + 1) + "_" + sc.spectralUnitLabel();
    lines.add (header);

    if (axisMode_ == 2 && !thirdOctMags.empty())
    {
        // One line per 1/3-octave band centre.
        const double* centres = bandCentres();
        for (size_t b = 0; b < thirdOctMags[0].size() && b < (size_t) numBands(); ++b)
        {
            juce::String line = juce::String (centres[b], 1);
            appendAlts (line, centres[b]);
            for (size_t c = 0; c < thirdOctMags.size() && c < 4; ++c)
                if (b < thirdOctMags[c].size())
                    // Band values are stored in relative-dB space, exactly as painted.
                    line += sep + fmtLevel ((double) thirdOctMags[c][b], chanOf (cachedChans_, c));
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
        appendAlts (line, f);
        for (size_t c = 0; c < mags.size() && c < 4; ++c)
        {
            const int bin = std::min (k, (int) mags[c].size() - 1);
            // Magnitudes are normalised against the fixed floor, so the
            // level is -kNormFloorDb * (1 - mag), exactly as painted.
            line += sep + fmtLevel (-kNormFloorDb * (1.0 - (double) mags[c][(size_t) bin]),
                                    chanOf (cachedChans_, c));
        }
        lines.add (line);
    }

    juce::SystemClipboard::copyTextToClipboard (lines.joinIntoString ("\n"));
}

// ============================================================ ToneListEditor

ToneListEditor::Row::Row()
{
    addAndMakeVisible (&freq);
    addAndMakeVisible (&amp);
    addAndMakeVisible (&clear);
}

namespace {
bool toneSlotValid (const juce::String& freqText)
{
    const double f = freqText.trim ().getDoubleValue ();
    return std::isfinite (f) && f > 0.0;
}
} // namespace

ToneListEditor::ToneListEditor()
{
    for (int i = 0; i < kMaxRows; ++i)
    {
        auto row = std::make_unique<Row>();
        Row* raw = row.get();
        row->clear.setButtonText ("Clear");
        row->clear.onClick = [raw] {
            raw->freq.setText ("", juce::dontSendNotification);
            raw->amp.setText ("", juce::dontSendNotification);
        };
        rows.push_back (std::move (row));
        addAndMakeVisible (raw);
    }

    addBtn.setButtonText ("Add Tone");
    addBtn.onClick = [this] { addRow (440.0, 0.5f); };
    addAndMakeVisible (&addBtn);
    setSize (width (), height());
}

void ToneListEditor::addRow (double freqHz, float amp)
{
    for (auto& row : rows)
        if (!toneSlotValid (row->freq.getText ()) && row->amp.getText ().isEmpty())
        {
            row->freq.setText (juce::String (freqHz, 3), juce::dontSendNotification);
            row->amp.setText (juce::String (amp, 2), juce::dontSendNotification);
            return;
        }
    // All slots are filled: nothing to do.
}

int ToneListEditor::numRows() const
{
    int n = 0;
    for (const auto& row : rows)
        if (toneSlotValid (row->freq.getText())) ++n;
    return n;
}

juce::TextEditor* ToneListEditor::firstFreqEditor() const
{
    for (const auto& row : rows)
        if (toneSlotValid (row->freq.getText()))
            return &row->freq;
    return rows.empty () ? nullptr : &rows[0]->freq;
}

void ToneListEditor::resized()
{
    auto b = getLocalBounds ();
    const int rowW = kFreqW + kGap + kAmpW + kGap + kClearW;
    int y = b.getY();
    for (const auto& row : rows)
    {
        Row& r = *row;
        // The Row itself must cover its children or they get clipped away.
        r.setBounds (b.getX(), y, rowW, kRowH);
        r.freq.setBounds (0, 0, kFreqW, kRowH);
        r.amp.setBounds (kFreqW + kGap, 0, kAmpW, kRowH);
        r.clear.setBounds (kFreqW + kGap + kAmpW + kGap, 0, kClearW, kRowH);
        y += kRowH + kGap;
    }
    addBtn.setBounds (b.removeFromBottom (kAddH));
}

std::vector<std::pair<double, double>> ToneListEditor::getTones() const
{
    std::vector<std::pair<double, double>> out;
    for (const auto& row : rows)
    {
        const juce::String freqText = row->freq.getText ();
        if (!toneSlotValid (freqText)) continue;   // skip empty / invalid slots
        const double a = row->amp.getText ().trim ().getDoubleValue ();
        out.push_back ({ freqText.trim ().getDoubleValue (),
                         std::isfinite (a) ? a : 0.0 });
    }
    return out;
}

// ============================================================ EqCurveEditor

namespace {
constexpr int kEqPointRadius = 3;
juce::Colour eqGridMinor() { return juce::Colour::fromRGB (40, 40, 46); }
juce::Colour eqGridMajor() { return juce::Colour::fromRGB (58, 58, 66); }
}

EqCurveEditor::EqCurveEditor()
{
    setPoints ({ sc::eq::EqPoint { 1000.0, 0.0 } });
    setSize (560, 340);
}

void EqCurveEditor::setPoints (std::vector<sc::eq::EqPoint> pts)
{
    std::sort (pts.begin (), pts.end (),
               [] (const sc::eq::EqPoint& a, const sc::eq::EqPoint& b) { return a.freqHz < b.freqHz; });
    points_ = std::move (pts);
    dragIndex_ = -1;
    pressAddedPoint_ = false;
    repaint ();
}

std::vector<sc::eq::EqPoint> EqCurveEditor::getPoints() const
{
    // Normalise: sort by frequency and merge points that ended up on the same
    // frequency (a fast drag can cross neighbours).
    auto pts = points_;
    std::sort (pts.begin (), pts.end (),
               [] (const sc::eq::EqPoint& a, const sc::eq::EqPoint& b) { return a.freqHz < b.freqHz; });
    std::vector<sc::eq::EqPoint> out;
    for (auto& p : pts)
    {
        if (!out.empty () && std::abs (out.back ().freqHz - p.freqHz) < 1e-9)
            out.back ().gainDb = p.gainDb;   // later point wins
        else
            out.push_back (p);
    }
    return out;
}

void EqCurveEditor::setPoint (int index, double freqHz, double gainDb)
{
    auto pts = getPoints();   // sorted + merged
    if (index < 0 || index >= (int) pts.size ()) return;
    pts[(size_t) index].freqHz = std::isfinite (freqHz) && freqHz > kFreqLo
                                 ? jlimit (kFreqLo, freqHi_, freqHz)
                                 : kFreqLo;
    pts[(size_t) index].gainDb = std::isfinite (gainDb)
                                 ? jlimit (kDbMin, kDbMax, gainDb)
                                 : 0.0;
    setPoints (pts);          // re-sort + merge same-frequency duplicates
}

void EqCurveEditor::setFreqHi (double nyquistHz)
{
    if (std::isfinite (nyquistHz) && nyquistHz > kFreqLo * 1.5)
        freqHi_ = nyquistHz;
    repaint ();
}

int EqCurveEditor::freqToX (double f) const
{
    const int w = getWidth () - kLeftMargin - kRightMargin;
    const double la = std::log10 (kFreqLo);
    const double lb = std::log10 (std::max (kFreqLo * 1.5, freqHi_));
    return (int) std::lround (kLeftMargin + w * (std::log10 (std::max (1e-9, f)) - la) / (lb - la));
}

double EqCurveEditor::xToFreq (int x) const
{
    const int w = getWidth () - kLeftMargin - kRightMargin;
    if (w <= 0) return kFreqLo;
    const double la = std::log10 (kFreqLo);
    const double lb = std::log10 (std::max (kFreqLo * 1.5, freqHi_));
    const double t = jlimit (0.0, 1.0, (double) (x - kLeftMargin) / (double) w);
    return std::pow (10.0, la + t * (lb - la));
}

int EqCurveEditor::dbToY (double db) const
{
    const int h = getHeight () - kTopMargin - kBottomMargin;
    if (h <= 0) return kTopMargin;
    const double t = jlimit (0.0, 1.0, (kDbMax - db) / (kDbMax - kDbMin));
    return (int) std::lround (kTopMargin + h * t);
}

double EqCurveEditor::yToDb (int y) const
{
    const int h = getHeight () - kTopMargin - kBottomMargin;
    if (h <= 0) return 0.0;
    const double t = jlimit (0.0, 1.0, (double) (y - kTopMargin) / (double) h);
    return kDbMax - t * (kDbMax - kDbMin);
}

int EqCurveEditor::findNearestPoint (const juce::Point<int>& p, int maxDistPx) const
{
    double bestD2 = (double) maxDistPx * (double) maxDistPx;
    int best = -1;
    for (size_t i = 0; i < points_.size (); ++i)
    {
        const int dx = freqToX (points_[i].freqHz) - p.x;
        const int dy = dbToY (points_[i].gainDb) - p.y;
        const double d2 = (double) dx * dx + (double) dy * dy;
        if (d2 <= bestD2)
        {
            bestD2 = d2;
            best = (int) i;
        }
    }
    return best;
}

void EqCurveEditor::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.getPosition ();

    // Right-click on a point removes it.
    if (e.mods.isRightButtonDown ())
    {
        const int idx = findNearestPoint (p, kEqPointRadius + 4);
        if (idx >= 0)
            points_.erase (points_.begin () + idx);
        dragIndex_ = -1;
        pressAddedPoint_ = false;
        repaint ();
        return;
    }

    // Double-click on a point opens the exact-coordinate dialog. JUCE's
    // click-count tracking uses consistent message timestamps, so it is
    // more reliable than comparing against wall-clock time captured on
    // the previous down event.
    if (e.getNumberOfClicks() >= 2)
    {
        const int idx = findNearestPoint (p, kEqPointRadius + 4);
        dragIndex_ = -1;
        pressAddedPoint_ = false;
        if (idx >= 0 && onEditPoint)
            onEditPoint (idx);
        repaint ();
        return;
    }

    const int idx = findNearestPoint (p, kEqPointRadius + 4);
    if (idx >= 0)
    {
        dragIndex_ = idx;
        pressAddedPoint_ = false;
    }
    else
    {
        // Clicking empty space adds a new point at the cursor and starts dragging it.
        sc::eq::EqPoint np;
        np.freqHz = jlimit (kFreqLo, freqHi_, xToFreq (p.x));
        np.gainDb = jlimit (kDbMin, kDbMax, yToDb (p.y));
        auto it = std::lower_bound (points_.begin (), points_.end (), np.freqHz,
                                    [] (const sc::eq::EqPoint& a, double f) { return a.freqHz < f; });
        if (it != points_.end () && std::abs (it->freqHz - np.freqHz) < 1e-9)
            it = points_.erase (it);   // replace an existing point on this frequency
        dragIndex_ = (int) (points_.insert (it, np) - points_.begin ());
        pressAddedPoint_ = true;
    }
    repaint ();
}

void EqCurveEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (dragIndex_ < 0 || dragIndex_ >= (int) points_.size ()) return;
    const auto p = e.getPosition ();
    sc::eq::EqPoint& pt = points_[(size_t) dragIndex_];
    pt.freqHz = jlimit (kFreqLo, freqHi_, xToFreq (p.x));
    pt.gainDb = jlimit (kDbMin, kDbMax, yToDb (p.y));
    repaint ();
}

void EqCurveEditor::mouseUp (const juce::MouseEvent&)
{
    dragIndex_ = -1;
    pressAddedPoint_ = false;
}

void EqCurveEditor::paint (juce::Graphics& g)
{
    const int w = getWidth () - kLeftMargin - kRightMargin;
    const int h = getHeight () - kTopMargin - kBottomMargin;
    if (w <= 10 || h <= 10) return;

    g.fillAll (juce::Colour::fromRGB (24, 24, 28));
    g.setFont (10.0f);

    // Vertical grid: minor lines at 2..9 x each decade, major at the decades.
    for (int dec = -1; dec <= 5; ++dec)
    {
        const double base = std::pow (10.0, (double) dec);
        if (base < kFreqLo * 0.5 || base > freqHi_ * 2.0) continue;
        for (int m = 1; m <= 9; ++m)
        {
            const double f = base * (double) m;
            if (f < kFreqLo || f > freqHi_) continue;
            const int x = freqToX (f);
            g.setColour (m == 1 ? eqGridMajor () : eqGridMinor ());
            g.drawLine (x, kTopMargin, x, kTopMargin + h, m == 1 ? 1.0f : 0.5f);
        }
    }

    // Horizontal grid every 10 dB; the 0 dB line is brighter.
    for (int db = -30; db <= 30; db += 10)
    {
        const int y = dbToY ((double) db);
        g.setColour (db == 0 ? eqGridMajor () : eqGridMinor ());
        g.drawLine (kLeftMargin, y, kLeftMargin + w, y, db == 0 ? 1.0f : 0.5f);
    }

    // Axis labels.
    g.setColour (juce::Colour::fromRGB (140, 140, 150));
    for (int dec = -1; dec <= 5; ++dec)
    {
        const double base = std::pow (10.0, (double) dec);
        if (base < kFreqLo || base > freqHi_) continue;
        juce::String label;
        if (base >= 1000.0) label = juce::String ((int) (base / 1000.0)) + "k";
        else                label = juce::String ((int) base);
        g.drawText (label, freqToX (base) - 24, getHeight () - kBottomMargin + 3, 48, 15,
                    juce::Justification::centred);
    }
    for (int db = -30; db <= 30; db += 10)
        g.drawText (juce::String (db), 2, dbToY ((double) db) - 8, kLeftMargin - 6, 15,
                    juce::Justification::centredRight);

    // The curve: straight segments between control points are exact on this
    // (log f, dB) grid; outside the point range the end values hold flat,
    // matching curveGainDb(). Gains outside the visible range clamp to edges.
    if (!points_.empty ())
    {
        g.setColour (juce::Colours::dodgerblue.withAlpha (0.9f));
        const int xFirst = freqToX (points_.front ().freqHz);
        const int yFirst = dbToY (jlimit (kDbMin, kDbMax, points_.front ().gainDb));
        g.drawLine (kLeftMargin, yFirst, xFirst, yFirst, 2.0f);   // flat before the first point
        for (size_t i = 1; i < points_.size (); ++i)
            g.drawLine (freqToX (points_[i - 1].freqHz), dbToY (jlimit (kDbMin, kDbMax, points_[i - 1].gainDb)),
                        freqToX (points_[i].freqHz),     dbToY (jlimit (kDbMin, kDbMax, points_[i].gainDb)),
                        2.0f);
        const int xLast = freqToX (points_.back ().freqHz);
        const int yLast = dbToY (jlimit (kDbMin, kDbMax, points_.back ().gainDb));
        g.drawLine (xLast, yLast, kLeftMargin + w, yLast, 2.0f);   // flat after the last point
    }

    // Control points.
    for (size_t i = 0; i < points_.size (); ++i)
    {
        const int x = freqToX (points_[i].freqHz);
        const int y = dbToY (jlimit (kDbMin, kDbMax, points_[i].gainDb));
        const bool dragging = ((int) i == dragIndex_);
        g.setColour (dragging ? juce::Colours::white : juce::Colour::fromRGB (255, 196, 0));
        g.fillEllipse (x - kEqPointRadius - (dragging ? 1 : 0), y - kEqPointRadius - (dragging ? 1 : 0),
                       2 * (kEqPointRadius + (dragging ? 1 : 0)), 2 * (kEqPointRadius + (dragging ? 1 : 0)));
    }
}

// ============================================================ EnvCurveEditor

namespace {
constexpr int kEnvPointRadius = 3;
juce::Colour envGridMinor() { return juce::Colour::fromRGB (40, 40, 46); }
juce::Colour envGridMajor() { return juce::Colour::fromRGB (58, 58, 66); }

/** A "nice" time step (1/2/5 x 10^k seconds) giving at most `maxDiv`
    divisions across the whole axis. */
double niceTimeStep (double spanSecs, int maxDiv)
{
    const double raw = std::max (spanSecs / maxDiv, 1e-9);
    for (int k = -6; k <= 8; ++k)
        for (double m : { 1.0, 2.0, 5.0 })
        {
            const double cand = m * std::pow (10.0, k);
            if (cand >= raw) return cand;
        }
    return 1e8;
}

/** Time-axis label with just enough decimals for the grid step. */
juce::String formatTimeLabel (double t, double step)
{
    int decimals = 0;
    while (step * std::pow (10.0, decimals) < 0.95 && decimals < 6) ++decimals;
    return juce::String (t, decimals);
}
} // namespace

EnvCurveEditor::EnvCurveEditor()
{
    setPoints ({ sc::env::EnvPoint { 0.0, 0.0 } });
    setSize (560, 340);
}

void EnvCurveEditor::setPoints (std::vector<sc::env::EnvPoint> pts)
{
    std::sort (pts.begin (), pts.end (),
               [] (const sc::env::EnvPoint& a, const sc::env::EnvPoint& b) { return a.tSec < b.tSec; });
    points_ = std::move (pts);
    dragIndex_ = -1;
    pressAddedPoint_ = false;
    repaint ();
}

std::vector<sc::env::EnvPoint> EnvCurveEditor::getPoints() const
{
    // Normalise: sort by time and merge points that ended up on the same
    // instant (keep the first).
    auto out = points_;
    std::sort (out.begin (), out.end (),
               [] (const sc::env::EnvPoint& a, const sc::env::EnvPoint& b) { return a.tSec < b.tSec; });
    std::vector<sc::env::EnvPoint> merged;
    for (const auto& p : out)
        if (merged.empty () || std::abs (p.tSec - merged.back ().tSec) > 1e-9)
            merged.push_back (p);
    return merged;
}

void EnvCurveEditor::setPoint (int index, double tSec, double gainDb)
{
    auto pts = getPoints ();
    if (index < 0 || index >= (int) pts.size ()) return;
    tSec = std::clamp (std::isfinite (tSec) ? tSec : 0.0, 0.0, timeHi_);
    gainDb = std::clamp (std::isfinite (gainDb) ? gainDb : 0.0, kDbMin, kDbMax);
    pts[(size_t) index] = { tSec, gainDb };
    setPoints (std::move (pts));
}

void EnvCurveEditor::setTimeHi (double secs)
{
    if (!std::isfinite (secs) || secs <= 1e-6) return;
    timeHi_ = secs;
    repaint ();
}

int EnvCurveEditor::tToX (double t) const
{
    const int w = getWidth () - kLeftMargin - kRightMargin;
    if (w <= 0 || timeHi_ <= 0.0) return kLeftMargin;
    return kLeftMargin + (int) std::lround ((t / timeHi_) * w);
}

double EnvCurveEditor::xToT (int x) const
{
    const int w = getWidth () - kLeftMargin - kRightMargin;
    if (w <= 0 || timeHi_ <= 0.0) return 0.0;
    return std::clamp ((double) (x - kLeftMargin) / (double) w, 0.0, 1.0) * timeHi_;
}

int EnvCurveEditor::dbToY (double db) const
{
    const int h = getHeight () - kTopMargin - kBottomMargin;
    if (h <= 0) return kTopMargin;
    const double t = jlimit (0.0, 1.0, (kDbMax - db) / (kDbMax - kDbMin));
    return (int) std::lround (kTopMargin + h * t);
}

double EnvCurveEditor::yToDb (int y) const
{
    const int h = getHeight () - kTopMargin - kBottomMargin;
    if (h <= 0) return 0.0;
    const double t = jlimit (0.0, 1.0, (double) (y - kTopMargin) / (double) h);
    return kDbMax - t * (kDbMax - kDbMin);
}

int EnvCurveEditor::findNearestPoint (const juce::Point<int>& p, int maxDistPx) const
{
    double bestD2 = (double) maxDistPx * (double) maxDistPx;
    int best = -1;
    for (size_t i = 0; i < points_.size (); ++i)
    {
        const int dx = tToX (points_[i].tSec) - p.x;
        const int dy = dbToY (points_[i].gainDb) - p.y;
        const double d2 = (double) dx * dx + (double) dy * dy;
        if (d2 <= bestD2)
        {
            bestD2 = d2;
            best = (int) i;
        }
    }
    return best;
}

void EnvCurveEditor::mouseDown (const juce::MouseEvent& e)
{
    const auto p = e.getPosition ();

    // Right-click on a point removes it.
    if (e.mods.isRightButtonDown ())
    {
        const int idx = findNearestPoint (p, kEnvPointRadius + 4);
        if (idx >= 0)
            points_.erase (points_.begin () + idx);
        dragIndex_ = -1;
        pressAddedPoint_ = false;
        repaint ();
        return;
    }

    // Double-click on a point opens the exact-coordinate dialog. JUCE's
    // click-count tracking uses consistent message timestamps, so it is
    // more reliable than comparing against wall-clock time captured on
    // the previous down event.
    if (e.getNumberOfClicks() >= 2)
    {
        const int idx = findNearestPoint (p, kEnvPointRadius + 4);
        dragIndex_ = -1;
        pressAddedPoint_ = false;
        if (idx >= 0 && onEditPoint)
            onEditPoint (idx);
        repaint ();
        return;
    }

    const int idx = findNearestPoint (p, kEnvPointRadius + 4);
    if (idx >= 0)
    {
        dragIndex_ = idx;
        pressAddedPoint_ = false;
    }
    else
    {
        // Clicking empty space adds a new point at the cursor.
        points_.push_back ({ xToT (p.x), yToDb (p.y) });
        std::sort (points_.begin (), points_.end (),
                   [] (const sc::env::EnvPoint& a, const sc::env::EnvPoint& b) { return a.tSec < b.tSec; });
        dragIndex_ = -1;
        pressAddedPoint_ = true;
    }
    repaint ();
}

void EnvCurveEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (dragIndex_ >= 0 && dragIndex_ < (int) points_.size ())
    {
        points_[(size_t) dragIndex_] = { xToT (e.x), yToDb (e.y) };
        repaint ();
    }
}

void EnvCurveEditor::mouseUp (const juce::MouseEvent&)
{
    dragIndex_ = -1;
    pressAddedPoint_ = false;
}

void EnvCurveEditor::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds ();
    g.fillAll (juce::Colour::fromRGB (24, 24, 28));
    g.setFont (10.0f);

    const int w = getWidth () - kLeftMargin - kRightMargin;
    const int h = getHeight () - kTopMargin - kBottomMargin;
    if (w <= 10 || h <= 10) return;

    // Horizontal grid: every 10 dB.
    for (double db = kDbMin; db <= kDbMax + 0.5; db += 10.0)
    {
        const int y = dbToY (db);
        g.setColour (std::abs (db) < 0.5 ? envGridMajor () : envGridMinor ());
        g.drawLine (kLeftMargin, y, kLeftMargin + w, y, 1.0f);
    }

    // Vertical grid: nice time steps.
    const double step = niceTimeStep (timeHi_, 8);
    for (double t = 0.0; t <= timeHi_ + step * 0.5; t += step)
    {
        const int x = tToX (t);
        g.setColour (envGridMinor ());
        g.drawLine (x, kTopMargin, x, kTopMargin + h, 1.0f);
    }

    // Axis labels: every grid line carries its value, and each axis has a
    // unit caption (dB on the left, seconds below).
    g.setColour (juce::Colour::fromRGB (140, 140, 150));
    for (double db = kDbMin; db <= kDbMax + 0.5; db += 10.0)
        g.drawText (juce::String ((int) db), 2, dbToY (db) - 8, kLeftMargin - 6, 16,
                    juce::Justification::centredRight);
    g.drawText ("dB", 2, 0, kLeftMargin - 6, 14, juce::Justification::centredRight);
    for (double t = 0.0; t <= timeHi_ + step * 0.5; t += step)
    {
        const int x = tToX (t);
        if (x >= kLeftMargin && x <= bounds.getRight () - 2)
            g.drawText (formatTimeLabel (t, step), x - 16, kTopMargin + h + 2,
                        32, 14, juce::Justification::centred);
    }
    g.drawText ("Time (s)", kLeftMargin, kTopMargin + h + 17, w, 14,
                juce::Justification::centred);

    // Axis frame.
    g.setColour (juce::Colour::fromRGB (90, 90, 100));
    g.drawRect (kLeftMargin, kTopMargin, w, h);

    if (points_.empty ()) return;

    // The curve: straight segments between control points are exact on this
    // (time, dB) grid; outside the point range the end values hold flat,
    // matching curveGainDb(). Gains outside the visible range clamp to edges.
    if (!points_.empty ())
    {
        g.setColour (juce::Colour::fromRGB (0, 212, 170));
        const int xFirst = tToX (points_.front ().tSec);
        const int yFirst = dbToY (jlimit (kDbMin, kDbMax, points_.front ().gainDb));
        if (xFirst > kLeftMargin)
            g.drawLine (kLeftMargin, yFirst, xFirst, yFirst, 2.0f);   // flat before the first point
        for (size_t i = 1; i < points_.size (); ++i)
            g.drawLine (tToX (points_[i - 1].tSec), dbToY (jlimit (kDbMin, kDbMax, points_[i - 1].gainDb)),
                        tToX (points_[i].tSec),     dbToY (jlimit (kDbMin, kDbMax, points_[i].gainDb)),
                        2.0f);
        const int xLast = tToX (points_.back ().tSec);
        const int yLast = dbToY (jlimit (kDbMin, kDbMax, points_.back ().gainDb));
        if (xLast < kLeftMargin + w)
            g.drawLine (xLast, yLast, kLeftMargin + w, yLast, 2.0f);   // flat after the last point
    }

    // Control points.
    const bool dragging = dragIndex_ >= 0;
    for (size_t i = 0; i < points_.size (); ++i)
    {
        const int x = tToX (points_[i].tSec);
        const int y = dbToY (points_[i].gainDb);
        g.setColour ((int) i == dragIndex_ ? juce::Colours::white
                                           : juce::Colour::fromRGB (0, 212, 170));
        g.fillEllipse (x - kEnvPointRadius - (dragging ? 1 : 0), y - kEnvPointRadius - (dragging ? 1 : 0),
                       2 * (kEnvPointRadius + (dragging ? 1 : 0)), 2 * (kEnvPointRadius + (dragging ? 1 : 0)));
    }
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

    // 256 gives the finest frequency resolution; larger sizes give a finer
    // time resolution of the spectrogram tiles. "Auto" recomputes the size from
    // the current zoom levels on every render (see SpectralEngine::chooseFftSize);
    // its label shows the resolved size, e.g. "Auto (4096)".
    rebuildFftSizeItems (0);
    fftSizeCombo.onChange = [this] {
        const int id = fftSizeCombo.getSelectedId();
        if (id == kFftAutoId)
            app.settings.fftAuto = true;
        else
        {
            app.settings.fftAuto = false;
            app.settings.fftSize = id;   // the last manual size is kept for when Auto is left
        }
        syncAutoControls();
        resized();   // the auto parameter rows only exist while "Auto" is selected
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

    // Harmonic marker rows: small white circles over the harmonics of the
    // frequency under the mouse in the spectrogram (no lines). Persisted.
    // Harmonic marker toggles: "Show harmonics" (Ctrl+H) enables/disables the
    // white harmonic circles; "Show harmonic numbers" adds a small number next
    // to each circle. Both are persisted between sessions.
    showHarmonicsBtn.setClickingTogglesState (true);
    showHarmonicsBtn.setButtonText ("Show harmonics CTRL+H");
    showHarmonicsBtn.setToggleState (app.settings.specHarmVisible, juce::dontSendNotification);
    showHarmonicsBtn.onClick = [this] {
        app.settings.specHarmVisible = showHarmonicsBtn.getToggleState();
        if (app.onStateChange) app.onStateChange();
    };

    showNumbersBtn.setClickingTogglesState (true);
    showNumbersBtn.setButtonText ("Show harmonic numbers");
    showNumbersBtn.setToggleState (app.settings.specHarmNumbers, juce::dontSendNotification);
    showNumbersBtn.onClick = [this] {
        app.settings.specHarmNumbers = showNumbersBtn.getToggleState();
        if (app.onStateChange) app.onStateChange();
    };

    styleLevelEdit (harmMaxEdit);
    harmMaxEdit.setJustification (juce::Justification::centredRight);
    harmMaxEdit.onEnter       = [this] { applyHarmInt (harmMaxEdit.getText(), app.settings.specHarmMaxK, 1, 1000); };
    harmMaxEdit.onFocusLost   = [this] { applyHarmInt (harmMaxEdit.getText(), app.settings.specHarmMaxK, 1, 1000); };

    styleLevelEdit (harmNEdit);
    harmNEdit.setJustification (juce::Justification::centredRight);
    harmNEdit.onEnter       = [this] { applyHarmInt (harmNEdit.getText(), app.settings.specHarmN, 1, 1000); };
    harmNEdit.onFocusLost   = [this] { applyHarmInt (harmNEdit.getText(), app.settings.specHarmN, 1, 1000); };

    // Auto FFT-size parameters: only visible while "Auto" is selected.
    styleLevelEdit (autoSlicesEdit);
    autoSlicesEdit.setJustification (juce::Justification::centredRight);
    autoSlicesEdit.onEnter       = [this] { applyAutoSlicesText (autoSlicesEdit.getText()); };
    autoSlicesEdit.onFocusLost   = [this] { applyAutoSlicesText (autoSlicesEdit.getText()); };

    styleLevelEdit (autoBinsEdit);
    autoBinsEdit.setJustification (juce::Justification::centredRight);
    autoBinsEdit.onEnter       = [this] { applyAutoBinsText (autoBinsEdit.getText()); };
    autoBinsEdit.onFocusLost   = [this] { applyAutoBinsText (autoBinsEdit.getText()); };

    syncLevelControls();
    syncHarmControls();
    syncAutoControls();

    logToggle.setClickingTogglesState (true);
    logToggle.setButtonText (app.settings.logFrequency ? "Logarithmic" : "Linear");
    logToggle.setToggleState (app.settings.logFrequency, juce::dontSendNotification);
    logToggle.onClick = [this] {
        app.settings.logFrequency = logToggle.getToggleState();
        logToggle.setButtonText (app.settings.logFrequency ? "Logarithmic" : "Linear");
        if (app.onStateChange) app.onStateChange();
    };

    resetFreqBtn.setName ("resetFreqBtn");
    resetFreqBtn.setButtonText ("Reset");
    resetFreqBtn.onClick = [this] {
        // Reset every Spectral Controls setting to its default.
        app.settings.fftAuto = true;
        app.settings.fftSize = 2048;
        app.settings.autoTargetTimeSlices = 80.0;
        app.settings.autoBinsPerPixel = 1.25;
        app.settings.logFrequency = true;
        app.settings.minFrequency = 20.0f;
        app.settings.maxFrequency = 20000.0f;
        app.settings.dbMinDb = -120.0f;
        app.settings.dbMaxDb = 0.0f;
        app.settings.specHarmVisible = false;
        app.settings.specHarmMaxK = 10;
        app.settings.specHarmN = 1;
        app.settings.specHarmNumbers = false;
        app.colourMap = ColourMap::Classic;
        app.spectralEngine.setColourMap (app.colourMap);

        // Sync every control back to the defaults.
        colourMapCombo.setSelectedId (1, juce::dontSendNotification);
        logToggle.setToggleState (true, juce::dontSendNotification);
        logToggle.setButtonText ("Logarithmic");
        fftSizeCombo.setSelectedId (kFftAutoId, juce::dontSendNotification);
        syncLevelControls();
        syncHarmControls();
        syncAutoControls();
        preview.repaint();

        // Reset the spectral view's local frequency axis to the defaults.
        if (onResetFreq) onResetFreq();
        resized();   // the auto parameter rows reappear with Auto selected
        if (app.onStateChange) app.onStateChange();
    };

    refreshBtn.setButtonText ("Refresh F5");
    refreshBtn.onClick = [this] { if (onRefresh) onRefresh (); };
    refreshBtn.setTooltip ("Discard every cached tile and force a complete spectrogram "
                           "recomputation. Shortcut: F5 (works from any focus).");

    colourMapCombo.setTooltip ("Colour map used by the spectrogram. 'Classic (Default)' is the "
                                "traditional DAW spectral palette.");
    dBRangeSlider.setTooltip ("Spectral dynamic range in dB (1-160): how far below the top of "
                               "the palette the spectrogram displays. Higher values reveal quieter detail.");
    dbRangeEdit.setTooltip ("Type any dB range greater than 1. Values above 160 apply beyond the slider's reach.");
    dbMinEdit.setTooltip ("Bottom level of the colour window: type a value in the file's unit "
                          "(dB, V or g) to change it; the dB range follows.");
    dbMaxEdit.setTooltip ("Top level of the colour window: type a value in the file's unit "
                          "(dB, V or g) to change it; the dB range follows.");
    showHarmonicsBtn.setTooltip ("Toggle the spectrogram's harmonic markers on/off. Shortcut: "
                                 "Ctrl+H (works from any focus).");
    showNumbersBtn.setTooltip ("Show a small white number next to each harmonic circle with its "
                               "harmonic number.");
    harmMaxEdit.setTooltip ("How many harmonics of the fundamental to display as white circles.");
    harmNEdit.setTooltip ("Which harmonic sits at the frequency under the mouse: fundamental = "
                          "f_mouse / N. Move the mouse over a tone's harmonic and set this so the "
                          "circles land on that tone's whole harmonic series.");
    logToggle.setTooltip ("Toggle the spectrogram's frequency axis between logarithmic and linear scaling.");
    resetFreqBtn.setTooltip ("Reset every Spectral Controls setting to its defaults: colour map, "
                             "dB window, harmonic markers, Auto FFT-size parameters, log/linear axis, "
                             "frequency range and FFT size.");
    refreshBtn.setTooltip ("Force a full recomputation of the spectrogram: discards every cached tile "
                            "and re-renders all tracks from the audio.");
    fftSizeCombo.setTooltip ("FFT size used by the spectrogram tiles. Larger values give finer "
                             "frequency resolution but render more slowly. 'Auto' picks the size "
                             "from the current time/frequency zoom levels on every render.");
    autoSlicesEdit.setTooltip ("Auto FFT size - target time slices: how many FFT frames to fit across "
                               "the visible time range. Higher values give finer time resolution but "
                               "render more slowly.");
    autoBinsEdit.setTooltip ("Auto FFT size - bins per pixel: spectral frequency bins per pixel of the "
                             "frequency axis. Higher values force larger FFT sizes for finer frequency "
                             "resolution, which renders more slowly.");

    colourMapLab.setText ("Colour Map:", juce::dontSendNotification);
    dbRangeLab.setText ("dB Range:", juce::dontSendNotification);
    for (auto* lab : { &colourMapLab, &dbRangeLab })
        lab->setFont (12.0f);

    harmMaxLab.setText ("Number of harmonics to display:", juce::dontSendNotification);
    harmNLab.setText ("Harmonic number under the mouse:", juce::dontSendNotification);
    for (auto* lab : { &harmMaxLab, &harmNLab })
        lab->setFont (11.0f);

    autoSlicesLab.setText ("Target time slices:", juce::dontSendNotification);
    autoBinsLab.setText ("Bins per pixel:", juce::dontSendNotification);
    for (auto* lab : { &autoSlicesLab, &autoBinsLab })
        lab->setFont (11.0f);

    addAndMakeVisible (&colourMapLab);
    addAndMakeVisible (&colourMapCombo);
    addAndMakeVisible (&preview);
    addAndMakeVisible (&dbMinEdit);
    addAndMakeVisible (&dbMaxEdit);
    addAndMakeVisible (&showHarmonicsBtn);
    addAndMakeVisible (&showNumbersBtn);
    addAndMakeVisible (&harmMaxLab);
    addAndMakeVisible (&harmMaxEdit);
    addAndMakeVisible (&harmNLab);
    addAndMakeVisible (&harmNEdit);
    addAndMakeVisible (&autoSlicesLab);
    addAndMakeVisible (&autoSlicesEdit);
    addAndMakeVisible (&autoBinsLab);
    addAndMakeVisible (&autoBinsEdit);
    autoSlicesLab.setVisible (app.settings.fftAuto);
    autoSlicesEdit.setVisible (app.settings.fftAuto);
    autoBinsLab.setVisible (app.settings.fftAuto);
    autoBinsEdit.setVisible (app.settings.fftAuto);
    addAndMakeVisible (&dbRangeLab);
    addAndMakeVisible (&dBRangeSlider);
    addAndMakeVisible (&dbRangeEdit);
    addAndMakeVisible (&logToggle);
    addAndMakeVisible (&resetFreqBtn);
    addAndMakeVisible (&fftSizeCombo);
    addAndMakeVisible (&refreshBtn);
}

void SpectralControlsPanel::refresh()
{
    colourMapCombo.setSelectedId ((int) app.colourMap + 1, juce::dontSendNotification);
    fftSizeCombo.setSelectedId (app.settings.fftAuto ? kFftAutoId : app.settings.fftSize,
                                juce::dontSendNotification);
    syncLevelControls();
    syncHarmControls();
    syncAutoControls();
    logToggle.setToggleState (app.settings.logFrequency, juce::dontSendNotification);
    logToggle.setButtonText (app.settings.logFrequency ? "Logarithmic" : "Linear");
}

void SpectralControlsPanel::rebuildFftSizeItems (int autoSize)
{
    // This JUCE build has no setItemText(), so the Auto label is refreshed by
    // rebuilding the small item list. The current selection is preserved and
    // no change messages are sent.
    const int selId = fftSizeCombo.getSelectedId();   // 0 = nothing selected yet
    fftSizeCombo.clear (juce::dontSendNotification);
    fftSizeCombo.addItem ("256", 256);
    for (int n = 512; n <= 65536; n *= 2)
        fftSizeCombo.addItem (juce::String (n), n);
    juce::String autoLabel = "Auto";
    if (autoSize > 0)
        autoLabel = juce::String ("Auto (") + juce::String (autoSize) + ")";
    fftSizeCombo.addItem (autoLabel, kFftAutoId);
    if (selId != 0)
        fftSizeCombo.setSelectedId (selId, juce::dontSendNotification);
}

void SpectralControlsPanel::setFftSizeSelection (int id)
{
    fftSizeCombo.setSelectedId (id, juce::dontSendNotification);
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

    {   // Auto FFT-size parameter rows: only present while "Auto" is selected.
        const bool showAuto = app.settings.fftAuto;
        autoSlicesLab.setVisible (showAuto);
        autoSlicesEdit.setVisible (showAuto);
        autoBinsLab.setVisible (showAuto);
        autoBinsEdit.setVisible (showAuto);
        if (showAuto)
        {
            auto row = b.removeFromTop (24);
            autoSlicesLab.setBounds (row.removeFromLeft (std::max (0, row.getWidth() - 54)));
            row.removeFromLeft (6);
            autoSlicesEdit.setBounds (row.reduced (0, 3));
        }
        if (showAuto)
        {
            auto row = b.removeFromTop (24);
            autoBinsLab.setBounds (row.removeFromLeft (std::max (0, row.getWidth() - 54)));
            row.removeFromLeft (6);
            autoBinsEdit.setBounds (row.reduced (0, 3));
        }
    }

    {   // Harmonic marker toggles: "Show harmonics CTRL+H" + "Show harmonic numbers".
        auto row = b.removeFromTop (24);
        showHarmonicsBtn.setBounds (row.removeFromLeft (150));
        row.removeFromLeft (6);
        showNumbersBtn.setBounds (row.removeFromLeft (150));
    }
    {
        auto row = b.removeFromTop (24);
        harmMaxLab.setBounds (row.removeFromLeft (std::max (0, row.getWidth() - 54)));
        row.removeFromLeft (6);
        harmMaxEdit.setBounds (row.reduced (0, 3));
    }
    {
        auto row = b.removeFromTop (24);
        harmNLab.setBounds (row.removeFromLeft (std::max (0, row.getWidth() - 54)));
        row.removeFromLeft (6);
        harmNEdit.setBounds (row.reduced (0, 3));
    }

    {   // Refresh button at the bottom: full spectrogram recomputation
        auto row = b.removeFromTop (28);
        refreshBtn.setBounds (row);
    }
}

// The right-click "Close Panel" menu is gone; panels close from the tab
// menu or the Window menu.
void SpectralControlsPanel::mouseDown (const juce::MouseEvent&)
{
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

void SpectralControlsPanel::syncLogToggle()
{
    logToggle.setToggleState (app.settings.logFrequency, juce::dontSendNotification);
    logToggle.setButtonText (app.settings.logFrequency ? "Logarithmic" : "Linear");
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

// Parses a plain integer (optional sign). Used by the harmonic marker fields.
static bool parseHarmInt (const juce::String& text, int& out)
{
    const auto t = text.trim();
    if (t.isEmpty()) return false;
    for (char16_t c : t)
        if ((c < '0' || c > '9') && c != '-' && c != '+') return false;
    out = t.getIntValue();
    return true;
}

bool SpectralControlsPanel::applyHarmInt (const juce::String& text, int& target, int lo, int hi)
{
    const auto t = text.trim();
    if (!t.isEmpty())
    {
        int v;
        if (!parseHarmInt (t, v)) { syncHarmControls(); return false; }
        target = std::clamp (v, lo, hi);
    }
    syncHarmControls();
    if (app.onStateChange) app.onStateChange();
    return true;
}

bool SpectralControlsPanel::applyHarmMaxText (const juce::String& text)
{
    return applyHarmInt (text, app.settings.specHarmMaxK, 1, 1000);
}

bool SpectralControlsPanel::applyHarmNText (const juce::String& text)
{
    return applyHarmInt (text, app.settings.specHarmN, 1, 1000);
}

void SpectralControlsPanel::syncHarmControls()
{
    showHarmonicsBtn.setToggleState (app.settings.specHarmVisible, juce::dontSendNotification);
    showNumbersBtn.setToggleState (app.settings.specHarmNumbers, juce::dontSendNotification);
    harmMaxEdit.setText (juce::String (app.settings.specHarmMaxK));
    harmNEdit.setText (juce::String (app.settings.specHarmN));
}

// Parses a plain decimal number (optional sign, one dot). Used by the auto
// FFT-size fields.
static bool parseAutoDouble (const juce::String& text, double& out)
{
    const auto t = text.trim();
    if (t.isEmpty()) return false;
    for (char16_t c : t)
        if ((c < '0' || c > '9') && c != '-' && c != '+' && c != '.') return false;
    out = t.getDoubleValue();
    return std::isfinite (out);
}

// Whole numbers print without decimals, everything else with three places.
static juce::String formatAutoDouble (double v)
{
    if (std::abs (v - std::round (v)) < 1e-9)
        return juce::String ((int) std::lround (v));
    return juce::String (v, 3);
}

bool SpectralControlsPanel::applyAutoDouble (const juce::String& text, double& target,
                                             double lo, double hi)
{
    const auto t = text.trim();
    if (!t.isEmpty())
    {
        double v;
        if (!parseAutoDouble (t, v)) { syncAutoControls(); return false; }
        target = std::clamp (v, lo, hi);
    }
    syncAutoControls();
    if (app.onStateChange) app.onStateChange();
    return true;
}

bool SpectralControlsPanel::applyAutoSlicesText (const juce::String& text)
{
    return applyAutoDouble (text, app.settings.autoTargetTimeSlices, 1.0, 1000.0);
}

bool SpectralControlsPanel::applyAutoBinsText (const juce::String& text)
{
    return applyAutoDouble (text, app.settings.autoBinsPerPixel, 0.1, 16.0);
}

void SpectralControlsPanel::syncAutoControls()
{
    autoSlicesEdit.setText (formatAutoDouble (app.settings.autoTargetTimeSlices));
    autoBinsEdit.setText (formatAutoDouble (app.settings.autoBinsPerPixel));
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
        ed->selectAll();
    }

    // Enter modal state without taking focus (the window is now showing),
    // then focus the field: a grab before that is ignored by JUCE.
    w->enterModalState (false);
    if (auto* ed = w->getTextEditor ("findQuery"))
        ed->grabKeyboardFocus();

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
        const juce::String text = (columnId == 2) ? owner->markerTimeText (m.start, false)
                                                  : (columnId == 3 ? (m.end < 0 ? "-" : owner->markerTimeText (m.end, false))
                                                                   : m.comment);
        g.drawText (text, 4, 0, width - 8, height, juce::Justification::centredLeft, false);
    }
}

void MarkerListPanel::Model::cellClicked (int rowNumber, int, const juce::MouseEvent& e)
{
    if (rowNumber >= 0 && rowNumber < getNumRows())
        owner->onRowClicked (rowNumber, e);
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

    // Selecting the row does the rest via onSelectionSetChanged: playhead,
    // time selection and view fit (+10% margin).
    selectRow (row);
}

void MarkerListPanel::Model::selectedRowsChanged (int)
{
    owner->onSelectionSetChanged ();
}

void MarkerListPanel::Model::deleteKeyPressed (int)
{
    // DEL deletes the audio data of the active selection - not list rows.
    owner->deleteAudioData ();
}

void MarkerListPanel::deleteAudioData ()
{
    // MainWindow routes this through its destructive-edit path (async
    // materialisation of streamed files + full view refresh).
    if (onDeleteAudioRequested) { onDeleteAudioRequested (); return; }

    std::string err;
    if (!app.del (&err))
        showSelectionMessage ("Delete", juce::String (err.c_str ()));
    else if (app.onStateChange)
        app.onStateChange ();   // the views must drop the deleted range
}

MarkerListPanel::MarkerListPanel (Application& appIn)
    : app (appIn), addBtn ("Add Marker"), deleteBtn ("Delete")
{
    auto& header = table.getHeader();
    // JUCE TableHeaderComponent column IDs must be non-zero. The Comment
    // column is not resizable: resized() stretches it to fill the remainder.
    const int fixedFlags = juce::TableHeaderComponent::notResizableOrSortable;
    header.addColumn ("Title", 1, 120, 60, -1, fixedFlags);
    header.addColumn ("Begin", 2, 75, 40, -1, fixedFlags);
    header.addColumn ("End", 3, 75, 40, -1, fixedFlags);
    header.addColumn ("Comment", 4, 120, 40, -1, fixedFlags);

    // Excel-style multi-selection: Ctrl/Cmd+click toggles rows, Shift+click
    // range-selects from the anchor row. DEL deletes the audio data of the
    // active selection (the context menu's Delete Selected removes rows).
    table.setMultipleSelectionEnabled (true);
    table.setSuppressShiftRangeSelection (true);   // we do the exact-range shift select ourselves

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
        refresh();
        if (app.onStateChange) app.onStateChange();
    };

    deleteBtn.onClick = [this] { deleteSelected (); };

    // The detail editors commit on Return / focus loss (and before the row
    // selection moves), not on every keystroke: per-keystroke commits re-sorted
    // the list under the caret, rewrote the field being typed in, and turned a
    // region into a point as soon as Length was cleared to retype it.
    beginEdit.onTextChange   = [this] { if (!editing) pendingEdits |= kPendingBegin; };
    endEdit.onTextChange     = [this] { if (!editing) pendingEdits |= kPendingEnd; };
    lengthEdit.onTextChange  = [this] { if (!editing) pendingEdits |= kPendingLength; };
    titleEdit.onTextChange   = [this] { if (!editing) pendingEdits |= kPendingTitle; };
    commentEdit.onTextChange = [this] { if (!editing) pendingEdits |= kPendingComment; };
    for (auto* ed : { &beginEdit, &endEdit, &lengthEdit, &titleEdit, &commentEdit })
    {
        ed->onFocusLost = [this] { commitPendingEdits(); };
        ed->onEscapeKey = [this] { pendingEdits = 0; updateDetails(); };   // revert
    }
    // The comment is multi-line: Return inserts a line break there.
    for (auto* ed : { &beginEdit, &endEdit, &lengthEdit, &titleEdit })
        ed->onReturnKey = [this] { commitPendingEdits(); };

    // Right-clicking a row in the table shows the marker context menu. Deep
    // registration is required: clicks on rows are delivered to the row's own
    // component, not to the TableListBox itself.
    table.addMouseListener (this, true);

    addBtn.setTooltip ("Add a marker at the playhead. With an active selection it creates a "
                       "region marker spanning the selection; otherwise a point marker.");
    deleteBtn.setTooltip ("Delete every selected marker from the list and from both views.");
    table.setTooltip ("Double-click a row to edit its details in the fields below. "
                      "Ctrl+click toggles rows, Shift+click range-selects from the last plain click "
                      "and fits all of it in view; DEL deletes the AUDIO of the selected markers' "
                      "time range (undoable) - right-click > Delete Selected removes the markers. "
                      "Right-click for copy/paste.");
    beginEdit.setTooltip ("Begin time of the selected marker, in seconds. Press Return or leave "
                          "the field to apply, Esc to revert.");
    endEdit.setTooltip ("End time of the selected marker, in seconds (leave empty for a point marker). "
                        "Press Return or leave the field to apply.");
    lengthEdit.setTooltip ("Length of the selected region marker. Editing it adjusts the End time "
                           "(and vice versa); leave empty to make it a point marker. "
                           "Press Return or leave the field to apply.");
    titleEdit.setTooltip ("Title shown on the marker line and in the Title column.");
    commentEdit.setTooltip ("Free-text comment stored with the marker (saved into WAV cue chunks).");;

    refresh();
}

static const Marker* findMarkerById (Application& app, int id)
{
    for (const auto& mk : app.markers().markers())
        if (mk.id == id) return &mk;
    return nullptr;
}

// Applies every pending detail edit to the selected marker in one go, then
// re-syncs the editors and the views once. followMarker: re-select the edited
// marker's row (edits re-sort the list by start time); false while the table
// selection is already moving elsewhere.
void MarkerListPanel::commitPendingEdits (bool followMarker)
{
    const int p = pendingEdits;
    pendingEdits = 0;
    if (p == 0) return;
    if (app.doc() != detailsDoc || selectedRow < 0 || selectedRow >= (int) app.markers().count())
    {
        updateDetails();   // nothing to apply the text to: show the real values
        return;
    }
    const int id = app.markers().markers()[(size_t) selectedRow].id;

    if (p & (kPendingBegin | kPendingEnd))
        applyBeginEnd (id, (p & kPendingBegin) != 0, (p & kPendingEnd) != 0);
    if ((p & kPendingLength) != 0 && (p & kPendingEnd) == 0)   // an explicit End wins
        applyLength (id);
    if (p & (kPendingTitle | kPendingComment))
        applyTitleComment (id, (p & kPendingTitle) != 0, (p & kPendingComment) != 0);

    if (followMarker)
    {
        const auto& ms = app.markers().markers();
        for (int r = 0; r < (int) ms.size(); ++r)
            if (ms[(size_t) r].id == id)
            {
                if (r != selectedRow)
                {
                    selectedRow = r;
                    anchorRow = r;
                    juce::SparseSet<int> sel;
                    sel.addRange ({ r, r + 1 });
                    table.setSelectedRows (sel, juce::dontSendNotification);
                }
                break;
            }
    }

    updateDetails();
    table.repaint();
    if (app.onStateChange) app.onStateChange(); // refresh the views' marker overlays
}

// Title / Comment editors below the table edit the selected marker in place.
void MarkerListPanel::applyTitleComment (int id, bool title, bool comment)
{
    const Marker* m = findMarkerById (app, id);
    if (m == nullptr) return;
    if (title)
    {
        const juce::String text = titleEdit.getText().trim();
        app.markers().renameMarker (id, text.isEmpty() ? m->name : text);
    }
    if (comment)
        app.markers().setComment (id, commentEdit.getText().trim());
}

void MarkerListPanel::applyBeginEnd (int id, bool beginDirty, bool endDirty)
{
    const Marker* m = findMarkerById (app, id);
    if (m == nullptr) return;
    const double dur = app.audioEngine().duration();
    const bool wasPoint = m->end < 0;
    const double oldStart = m->start, oldEnd = m->end;

    // Invalid text leaves that bound alone (it used to jump to 0 s).
    bool beginOk = false, endOk = false;
    double begin = beginDirty ? parseMarkerTime (beginEdit.getText(), false, &beginOk) : oldStart;
    if (!beginOk) { beginDirty = false; begin = oldStart; }
    const bool endEmpty = endEdit.getText().trim().isEmpty();
    double end = endDirty ? parseMarkerTime (endEdit.getText(), false, &endOk) : oldEnd;
    if (endDirty && !endEmpty && !endOk) { endDirty = false; end = oldEnd; }

    if (endDirty && endEmpty)
    {
        // Clearing End turns a region into a point marker (see the tooltip).
        if (!wasPoint) app.markers().setPoint (id);
        if (beginDirty && begin >= 0.0 && begin <= dur) app.markers().moveMarker (id, begin);
        return;
    }
    if (!beginDirty && !endDirty) return;

    if (wasPoint && !endDirty)
    {
        if (begin >= 0.0 && begin <= dur)
            app.markers().moveMarker (id, begin);
        return;
    }
    // Region (or a point marker that just got an End).
    end = std::min (end, dur);
    if (begin >= 0.0 && begin <= dur && end > begin + 1e-9)
        app.markers().setRegion (id, begin, end);
}

// Editing the Length field adjusts the End time; clearing it turns a region
// marker back into a point marker. (Editing End updates this box via updateDetails.)
void MarkerListPanel::applyLength (int id)
{
    const Marker* m = findMarkerById (app, id);
    if (m == nullptr) return;

    const juce::String text = lengthEdit.getText();
    bool ok = false;
    const double len = parseMarkerTime (text, true, &ok);
    if (!text.trim().isEmpty() && !ok) return;   // invalid text: keep the marker
    if (!(len > 0.0))
    {
        if (m->end >= 0)
            app.markers().setPoint (id);
        return;
    }
    const double start = m->start;
    const double end = std::min (start + len, app.audioEngine().duration());
    if (end <= start + 1e-9)
        app.markers().setPoint (id); // too short to be a region
    else
        app.markers().setRegion (id, start, end);
}

juce::String MarkerListPanel::markerTimeText (double t, bool duration) const
{
    const double r = std::max (1.0, app.audioEngine().sampleRate());
    if (!duration && app.timeAxisMode == TimeAxisMode::TimeOffset)
        return formatDisplayedTime (t, app.timeAxisMode, app.timeOffsetSecs, r);
    if (app.timeAxisMode == TimeAxisMode::Sample)
        return juce::String ((int64_t) std::llround (t * r));
    return formatTimeCode (t);   // classic m:ss.ddd in the default Time mode
}

double MarkerListPanel::parseMarkerTime (const juce::String& s, bool duration, bool* ok) const
{
    // Displayed value; empty/invalid text becomes 0 (same as parseTimeCode)
    // and reports *ok = false so editors can ignore it.
    double v = 0.0;
    const bool parsed = !s.trim().isEmpty() && parseSignedClock (s, v);
    if (!parsed)
        v = 0.0;
    if (ok != nullptr) *ok = parsed;

    const double r = std::max (1.0, app.audioEngine().sampleRate());
    if (!duration && app.timeAxisMode == TimeAxisMode::TimeOffset)
        return v - app.timeOffsetSecs;
    if (app.timeAxisMode == TimeAxisMode::Sample)
        return v / r;
    return v;
}

void MarkerListPanel::updateDetails()
{
    if (selectedRow < 0 || selectedRow >= (int) app.markers().count())
    {
        pendingEdits = 0;
        beginEdit.setText ("", juce::dontSendNotification);
        endEdit.setText ("", juce::dontSendNotification);
        lengthEdit.setText ("", juce::dontSendNotification);
        titleEdit.setText ("", juce::dontSendNotification);
        commentEdit.setText ("", juce::dontSendNotification);
        return;
    }

    const auto& m = app.markers().markers()[(size_t) selectedRow];
    // Fields with an uncommitted edit keep the typed text: a state refresh
    // from elsewhere must not wipe what the user is typing.
    auto put = [this] (juce::TextEditor& ed, int bit, const juce::String& t)
    {
        if ((pendingEdits & bit) == 0) ed.setText (t, juce::dontSendNotification);
    };
    editing = true;
    put (beginEdit, kPendingBegin, markerTimeText (m.start, false));
    put (endEdit, kPendingEnd, m.end < 0 ? "" : markerTimeText (m.end, false));
    put (lengthEdit, kPendingLength, m.end < 0 ? "" : markerTimeText (m.end - m.start, true));
    put (titleEdit, kPendingTitle, m.name);
    put (commentEdit, kPendingComment, m.comment);
    editing = false;
}

void MarkerListPanel::refresh()
{
    // Adding a marker needs audio to anchor it to - disable the button when
    // no file is loaded (the state sync calls refresh() on open/close).
    addBtn.setEnabled (app.audioEngine().isFileLoaded());
    if (app.doc() != detailsDoc)
    {
        // Another document is active: the selected row index and any
        // uncommitted detail edit belonged to the previous file's markers.
        detailsDoc = app.doc();
        pendingEdits = 0;
        selectedRow = -1;
        anchorRow = -1;
        table.setSelectedRows (juce::SparseSet<int>(), juce::dontSendNotification);
    }
    table.updateContent();
    table.repaint();
    updateDetails();
}

void MarkerListPanel::selectRow (int row)
{
    commitPendingEdits();   // the typed values belong to the current row
    if (row < 0 || row >= (int) app.markers().count())
    {
        selectedRow = -1;
        anchorRow = -1;
        table.setSelectedRows (juce::SparseSet<int>(), juce::dontSendNotification);
        updateDetails();
        return;
    }

    anchorRow = row;
    juce::SparseSet<int> sel;
    sel.addRange ({ row, row + 1 });
    table.setSelectedRows (sel, juce::sendNotification); // -> onSelectionSetChanged
}

int MarkerListPanel::primaryRow() const
{
    return table.getNumSelectedRows () > 0 ? table.getSelectedRow (0) : -1;
}

void MarkerListPanel::onSelectionSetChanged()
{
    // Uncommitted detail edits belong to the previously selected marker.
    commitPendingEdits (false);
    selectedRow = primaryRow();
    updateDetails();

    // The union of all selected markers becomes the time selection, and both
    // views are fitted to it with the usual 10% margin.
    const auto& ms = app.markers().markers();
    double t0 = 0.0, t1 = 0.0;
    bool any = false;
    for (int r : sparseRows (table.getSelectedRows()))
    {
        if (r < 0 || r >= (int) ms.size ()) continue;
        const auto& m = ms [(size_t) r];
        const double end = (m.end > m.start) ? m.end : m.start;
        if (!any)
        {
            t0 = m.start;
            t1 = end;
            any = true;
        }
        else
        {
            t0 = std::min (t0, m.start);
            t1 = std::max (t1, end);
        }
    }
    if (!any) return;

    app.setPlayhead (t0);
    app.setSelection ({ t0, t1 });
    app.ensureTimeRangeVisible (t0, t1);   // +10% margin
}

std::vector<int> MarkerListPanel::selectedRowIndices() const
{
    return sparseRows (table.getSelectedRows());
}

// Excel-style click handling. JUCE already applied the plain-click / Ctrl+click
// part before this runs; Shift+click gets replaced here with the EXACT range
// from the anchor (JUCE's built-in shift behaviour unions instead), and the
// view is fitted to everything selected.
void MarkerListPanel::onRowClicked (int row, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu ())
        return;   // right-click: just show the context menu - keep any multi-selection

    table.setWantsKeyboardFocus (true);
    if (table.isShowing ())   // a focus grab on a hidden table is a no-op (and asserts)
        table.grabKeyboardFocus ();   // so DEL works right after a click

    if (e.mods.isShiftDown ())
    {
        // Excel: the selection becomes EXACTLY anchor..row (JUCE's built-in
        // shift behaviour unions instead), then fit everything selected.
        const int n = app.markers().count();
        const int a = (anchorRow >= 0 && anchorRow < n) ? anchorRow : row;
        juce::SparseSet<int> sel;
        sel.addRange ({ std::min (a, row), std::max (a, row) + 1 });
        table.setSelectedRows (sel, juce::sendNotification);
        fitViewToSelected ();
    }
    else if (e.mods.isCommandDown () || e.mods.isCtrlDown ())
    {
        // JUCE already toggled this row in selectRowsBasedOnModifierKeys before
        // cellClicked fires - just track the anchor here.
        anchorRow = row;
    }
    else
    {
        // Plain click: single selection and move the anchor.
        anchorRow = row;
        juce::SparseSet<int> sel;
        sel.addRange ({ row, row + 1 });
        table.setSelectedRows (sel, juce::sendNotification);
    }
}

void MarkerListPanel::deleteSelected ()
{
    const int n = app.markers().count();
    std::vector<int> ids;
    for (int r : sparseRows (table.getSelectedRows()))
        if (r >= 0 && r < n)
            ids.push_back (app.markers().markers()[(size_t) r].id);

    if (ids.empty()) return;
    pendingEdits = 0;   // the edited marker may be among the removed ones
    for (int id : ids)
        app.markers().removeMarker (id);

    selectedRow = -1;
    anchorRow = -1;
    table.setSelectedRows (juce::SparseSet<int>(), juce::dontSendNotification);
    refresh();
    if (app.onStateChange) app.onStateChange (); // refresh the spectral view's marker overlay
}

void MarkerListPanel::fitViewToSelected ()
{
    const int n = app.markers().count();
    double t0 = 0.0, t1 = 0.0;
    bool any = false;
    for (int r : sparseRows (table.getSelectedRows()))
    {
        if (r < 0 || r >= n) continue;
        const auto& m = app.markers().markers()[(size_t) r];
        const double end = (m.end > m.start) ? m.end : m.start;
        if (!any)
        {
            t0 = std::min (m.start, end);
            t1 = std::max (m.start, end);
            any = true;
        }
        else
        {
            t0 = std::min (t0, m.start);
            t1 = std::max (t1, end);
        }
    }
    if (!any) return;
    app.ensureTimeRangeVisible (t0, t1);   // adds the usual 10% margin
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

    // Right-clicking a row selects it only when nothing is selected yet - an
    // existing multi-selection is left alone so the menu can act on all of it.
    if (row >= 0 && table.getNumSelectedRows () == 0)
    {
        anchorRow = row;
        juce::SparseSet<int> sel;
        sel.addRange ({ row, row + 1 });
        table.setSelectedRows (sel, juce::sendNotification); // -> onSelectionSetChanged
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
    m.addItem (9, "Delete Selected", table.getNumSelectedRows () > 0);
    m.addItem (8, "Fit View to Selected", table.getNumSelectedRows () > 0);
    m.addItem (3, "Find...\tCtrl+F");

    const int id = m.showMenu (juce::PopupMenu::Options().withMousePosition());
    if (id == 1)
        copyAllMarkers();
    else if (id == 2)
        pasteMarkersFromClipboard();
    else if (id == 4)
        exportMarkersCsv();
    else if (id == 5)
        importMarkersCsv();
    else if (id == 9)
        deleteSelected ();
    else if (id == 8)
        fitViewToSelected ();
    else if (id == 3)
        showFindDialog();
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
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
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
        refresh();
        if (app.onStateChange) app.onStateChange(); // refresh the spectral view's marker overlay
    }
    else
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Import failed",
                                            "No markers could be read from the CSV file.",
                                            "", this);
    }

    return added;
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
            table.setSelectedRows (sel, juce::sendNotification); // -> selectedRowsChanged
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
        refresh();
        if (app.onStateChange) app.onStateChange(); // refresh the spectral view's marker overlay
    }

    return added;
}

void MarkerListPanel::resized()
{
    auto b = getLocalBounds().reduced (4, 4);
    table.setBounds (b.removeFromTop (std::max (80, (int) (b.getHeight() * 0.42))));

    // The Comment column fills whatever width is left of the fixed columns.
    auto& hdr = table.getHeader();
    const int fixedW = 120 + 75 + 75;
    hdr.setColumnWidth (1, 120);
    hdr.setColumnWidth (2, 75);
    hdr.setColumnWidth (3, 75);
    hdr.setColumnWidth (4, std::max (40, table.getWidth () - fixedW));

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

// Right-side field widths: [Sel:] [start] [-] [stop] [(] [len] [)]  gap  [Pos: t]
static constexpr int kSelEditW = 90;   // start / stop field width
static constexpr int kLenEditW = 84;   // length field width
static constexpr int kPosLabelW = 30;  // "Pos:" label width
static constexpr int kPosEditW = 96;   // playhead-position field width

StatusBar::RightLayout StatusBar::computeRightLayout (int width) const
{
    // selLabelX = -1 marks "does not fit".
    RightLayout r {-1, 0, 0, 0, 0, 0, 0, -1, 0};
    constexpr int selLabelW = 34, dashSlot = 10, parenSlot = 8, gap = 16;
    const int groupW = selLabelW + kSelEditW + dashSlot + kSelEditW
                     + parenSlot + kLenEditW + parenSlot + gap
                     + kPosLabelW + kPosEditW;
    if (width <= groupW + 8) return r;   // too narrow: fields stay hidden
    const int x0 = width - 8 - groupW;
    r.selLabelX = x0;
    r.startX    = x0 + selLabelW;
    r.dashX     = r.startX + kSelEditW;
    r.stopX     = r.dashX + dashSlot;
    r.openX     = r.stopX + kSelEditW;
    r.lenX      = r.openX + parenSlot;
    r.closeX    = r.lenX + kLenEditW;
    r.posLabelX = r.closeX + parenSlot + gap;
    r.posX      = r.posLabelX + kPosLabelW;
    return r;
}

StatusBar::StatusBar()
{
    for (auto* e : { &startEdit, &stopEdit, &lenEdit, &posEdit })
    {
        e->setFont (12.0f);
        e->setMultiLine (false);
        e->setVisible (false);   // shown once a selection/playhead exists and fits
        e->setColour (juce::TextEditor::backgroundColourId,
                     juce::Colour::fromRGB (60, 60, 70));
        e->setColour (juce::TextEditor::textColourId,
                     juce::Colour::fromRGB (235, 235, 240));
        e->setColour (juce::TextEditor::highlightedTextColourId,
                     juce::Colour::fromRGB (255, 255, 255));
        e->addListener (this);
    }

    // The editors are real children so they render in the live app; headless
    // tests drive them directly without a peer.
    addChildComponent (startEdit);
    addChildComponent (stopEdit);
    addChildComponent (lenEdit);
    addChildComponent (posEdit);

    // Live editing: every parseable change is applied straight away, so a
    // pasted value takes effect without pressing Enter. Unparseable partial
    // text ("-", "2:") is ignored until focus is lost.
    startEdit.onTextChange = [this] { commitField (startEdit); };
    stopEdit.onTextChange  = [this] { commitField (stopEdit); };
    lenEdit.onTextChange   = [this] { commitField (lenEdit); };
    posEdit.onTextChange   = [this] { commitField (posEdit); };

    // Enter normalises the field to the canonical display of its value.
    startEdit.onReturnKey = [this] { reformatField (startEdit); };
    stopEdit.onReturnKey  = [this] { reformatField (stopEdit); };
    lenEdit.onReturnKey   = [this] { reformatField (lenEdit); };
    posEdit.onReturnKey   = [this] { reformatField (posEdit); };
}

void StatusBar::resized()
{
    rightFits_ = computeRightLayout (getWidth()).selLabelX >= 0;
    applyFieldVisibility();
    if (!rightFits_) return;
    const auto L = computeRightLayout (getWidth());
    const int y = 2, h = std::max (14, getHeight() - 5);
    startEdit.setBounds (L.startX, y, kSelEditW, h);
    stopEdit.setBounds (L.stopX, y, kSelEditW, h);
    lenEdit.setBounds (L.lenX, y, kLenEditW, h);
    posEdit.setBounds (L.posX, y, kPosEditW, h);
}

void StatusBar::applyFieldVisibility()
{
    startEdit.setVisible (rightFits_ && selVisible_);
    stopEdit.setVisible  (rightFits_ && selVisible_);
    lenEdit.setVisible   (rightFits_ && selVisible_);
    posEdit.setVisible   (rightFits_ && posVisible_);
}

double StatusBar::fieldTime (const juce::TextEditor& ed) const
{
    if (&ed == &startEdit) return curStart;
    if (&ed == &stopEdit)  return curStop;
    if (&ed == &posEdit)   return curPos;
    return curStop - curStart;
}

void StatusBar::commitField (juce::TextEditor& ed)
{
    double v;
    if (!parseFieldText (ed.getText(), v, &ed == &lenEdit))
        return;   // mid-typing text: leave the selection alone for now

    if (&ed == &posEdit)
    {
        if (onPositionEdited) onPositionEdited (v);
        return;
    }

    double a = curStart, b = curStop;
    if (&ed == &startEdit)      a = jlimit (0.0, curStop, v);
    else if (&ed == &stopEdit)  b = std::max (curStart, v);   // app clamps to file end
    else                        b = curStart + std::max (0.0, v);

    if (onSelectionEdited)
        onSelectionEdited (a, b);
}

void StatusBar::reformatField (juce::TextEditor& ed)
{
    // Canonical display of the current selection edge; also reverts garbage
    // that was typed before focus was lost.
    ed.setText (formatFieldTime (fieldTime (ed), &ed == &lenEdit), false);
}

juce::String StatusBar::formatFieldTime (double v, bool duration) const
{
    if (!duration && timeCtxMode == TimeAxisMode::TimeOffset)
        return formatDisplayedTime (v, timeCtxMode, timeCtxOffset, timeCtxRate);
    // Durations never carry the offset; Sample mode scales them linearly.
    if (timeCtxMode == TimeAxisMode::Sample)
        return juce::String ((int64_t) std::llround (v * timeCtxRate));
    return formatFooterTime (v);
}

bool StatusBar::parseFieldText (const juce::String& s, double& outSecs, bool duration) const
{
    if (!duration && timeCtxMode == TimeAxisMode::TimeOffset)
        return parseDisplayedTime (s, timeCtxMode, timeCtxOffset, timeCtxRate, outSecs);
    if (timeCtxMode == TimeAxisMode::Sample)
        return parseDisplayedTime (s, TimeAxisMode::Sample, 0.0, timeCtxRate, outSecs);
    double v = 0.0;
    if (!parseSignedClock (s, v)) { outSecs = 0.0; return false; }
    outSecs = v;
    return true;
}

void StatusBar::textEditorFocusLost (juce::TextEditor& ed)
{
    if (&ed == &startEdit || &ed == &stopEdit || &ed == &lenEdit
            || &ed == &posEdit)
        reformatField (ed);
}

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
    // Channel numbers: dim = hidden, red = muted; a click shows/hides one.
    if (const auto chips = channelChipRects(); !chips.empty())
    {
        g.setFont (11.0f);
        for (size_t i = 0; i < chips.size(); ++i)
        {
            const bool shown = i < chanShown_.size() && chanShown_[i];
            const bool muted = i < chanMuted_.size() && chanMuted_[i];
            g.setColour (shown ? juce::Colour::fromRGB (72, 74, 84) : juce::Colour::fromRGB (52, 52, 56));
            g.fillRoundedRectangle (chips[i].toFloat(), 3.0f);
            g.setColour (muted ? (shown ? juce::Colour::fromRGB (255, 90, 90) : juce::Colour::fromRGB (150, 70, 70))
                               : (shown ? juce::Colour::fromRGB (235, 235, 240) : juce::Colour::fromRGB (110, 110, 120)));
            g.drawText (channelChipLabel ((int) i), chips[i], juce::Justification::centred, false);
        }
        leftX = chips.back().getRight() + 12;
        g.setFont (12.0f);
    }
    if (!modeText.isEmpty() && leftX < b.getWidth() - 8)
    {
        // Live recording indicator: a solid red dot drawn as graphics so it
        // never depends on font glyph coverage (a "\u25CF" character in the text
        // was missing from some system fonts and corrupted the footer layout).
        if (recActive_)
        {
            g.setColour (juce::Colour::fromRGB (255, 70, 70));
            g.fillEllipse (juce::Rectangle<float> ((float) leftX,
                                                   b.getHeight() * 0.5f - 4.0f,
                                                   8.0f, 8.0f));
            leftX += 14;
        }

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
    if (!controlText_.isEmpty() && leftX < b.getWidth() - 8)
    {
        // AI control indicator (a client is connected to the control server).
        g.setColour (juce::Colour::fromRGB (255, 190, 80));
        const juce::Font f (12.0f);
        const int w = std::min (b.getWidth() - leftX - 8,
                                (int) juce::GlyphArrangement::getStringWidth (f, controlText_) + 8);
        if (w > 10)
        {
            g.drawText (controlText_, leftX, 0, w, b.getHeight(), juce::Justification::left, false);
            leftX += w + 12;
        }
    }
    if (diagActive_ && leftX < b.getWidth() - 8)
    {
        // Diagnostic log on: a small green dot (graphics, not a glyph) and a
        // short label, so the user always knows the log is being written.
        const juce::String label ("Diagnostic log");
        const juce::Font f (12.0f);
        const int w = std::min (b.getWidth() - leftX - 8,
                                (int) juce::GlyphArrangement::getStringWidth (f, label) + 20);
        if (w > 30)
        {
            g.setColour (juce::Colour::fromRGB (110, 200, 130));
            g.fillEllipse (juce::Rectangle<float> ((float) leftX, b.getHeight() * 0.5f - 3.0f, 6.0f, 6.0f));
            g.setColour (juce::Colour::fromRGB (175, 205, 180));
            g.drawText (label, leftX + 10, 0, w - 10, b.getHeight(), juce::Justification::left, false);
            leftX += w + 12;
        }
    }
    if (!mouseText.isEmpty() && leftX < b.getWidth() - 8)
    {
        g.setColour (juce::Colour::fromRGB (205, 205, 215));
        int w = std::max (10, b.getWidth() / 2 + 60 - leftX);
        // Keep the probe text clear of the editable selection fields.
        if (rightFits_ && selVisible_)
            w = std::min (w, computeRightLayout (b.getWidth()).selLabelX - leftX - 8);
        g.drawText (mouseText, leftX, 0, std::max (10, w), b.getHeight(),
                    juce::Justification::left, false);
    }

    if (rightFits_ && selVisible_)
    {
        const auto L = computeRightLayout (b.getWidth());
        g.setColour (juce::Colour::fromRGB (160, 190, 235));
        g.setFont (12.0f);
        g.drawText ("Sel:", L.selLabelX, 0, 34, b.getHeight(), juce::Justification::left, false);
        g.drawText ("-", L.dashX, 0, 10, b.getHeight(), juce::Justification::centred, false);
        g.drawText ("(", L.openX, 0, 8, b.getHeight(), juce::Justification::centred, false);
        g.drawText (")", L.closeX, 0, 8, b.getHeight(), juce::Justification::centred, false);
    }

    if (!playPart.isEmpty())
    {
        g.setColour (juce::Colour::fromRGB (160, 190, 235));
        juce::Font f (12.0f);
        if (rightFits_ && posVisible_)
        {
            // The editable field shows the value; only its label is painted here.
            const auto L = computeRightLayout (b.getWidth());
            g.drawText ("Pos:", L.posLabelX, 0, kPosLabelW, b.getHeight(),
                        juce::Justification::left, false);
        }
        else if (!rightFits_)
        {
            // Too narrow for the field: fall back to plain right-aligned text.
            const int w = std::min (b.getWidth() - 8,
                                    (int) juce::GlyphArrangement::getStringWidth (f, playPart) + 16);
            g.drawText (playPart, b.getWidth() - w, 0, w, b.getHeight(),
                        juce::Justification::right, false);
        }
    }
}

int StatusBar::fileAndRateEnd() const
{
    // Same measurements as the start of paint().
    const int W = getWidth();
    const juce::Font f (juce::FontOptions (12.0f));
    int leftX = 6;
    if (!fileName.isEmpty())
    {
        const int w = std::min (W / 2 - leftX,
                                (int) juce::GlyphArrangement::getStringWidth (f, fileName) + 8);
        if (w > 10) leftX += w + 12;
    }
    if (!ratePart.isEmpty() && leftX < W - 8)
    {
        const int w = std::min (W / 2 + 60 - leftX,
                                (int) juce::GlyphArrangement::getStringWidth (f, ratePart) + 8);
        if (w > 10) leftX += w + 12;
    }
    return leftX;
}

void StatusBar::setChannels (int count, const std::vector<int>& visible, const std::vector<bool>& muted)
{
    count = std::max (0, count);
    std::vector<bool> shown ((size_t) count, false), mut ((size_t) count, false);
    for (const int ch : visible)
        if (ch >= 0 && ch < count) shown[(size_t) ch] = true;
    for (int ch = 0; ch < count && ch < (int) muted.size(); ++ch)
        mut[(size_t) ch] = muted[(size_t) ch];
    if (count == chanCount_ && shown == chanShown_ && mut == chanMuted_) return;
    chanCount_ = count;
    chanShown_ = std::move (shown);
    chanMuted_ = std::move (mut);
    repaint();
}

juce::String StatusBar::channelChipLabel (int ch) const
{
    return chanCount_ == 2 ? juce::String (ch == 0 ? "L" : "R") : juce::String (ch + 1);
}

std::vector<juce::Rectangle<int>> StatusBar::channelChipRects() const
{
    std::vector<juce::Rectangle<int>> r;
    if (chanCount_ <= 0) return r;
    int x = fileAndRateEnd();
    // Never under the editable fields on the right (or past the edge): the
    // numbers that do not fit are left out.
    const int limit = rightFits_ ? computeRightLayout (getWidth()).selLabelX - 8 : getWidth() - 8;
    const juce::Font f (juce::FontOptions (11.0f));   // paint's g.setFont (11.0f)
    const int h = std::max (12, getHeight() - 6);
    const int y = (getHeight() - h) / 2;
    for (int ch = 0; ch < chanCount_; ++ch)
    {
        const int w = std::max (16, (int) std::ceil (juce::GlyphArrangement::getStringWidth (f, channelChipLabel (ch))) + 8);
        if (x + w > limit) break;
        r.push_back ({ x, y, w, h });
        x += w + 3;
    }
    return r;
}

int StatusBar::channelChipAt (juce::Point<int> p) const
{
    const auto chips = channelChipRects();
    for (size_t i = 0; i < chips.size(); ++i)
        if (chips[i].expanded (1, 2).contains (p)) return (int) i;
    return -1;
}

void StatusBar::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    const int ch = channelChipAt (e.getPosition());
    // Ctrl/Cmd or Shift asks for "only this channel" - the same modifier the
    // channel list and the track numbers use for soloing.
    const bool solo = e.mods.isCommandDown () || e.mods.isCtrlDown ()
                      || e.mods.isShiftDown ();
    if (ch >= 0 && onChannelClicked) onChannelClicked (ch, solo);
}

void StatusBar::setSelection (double t0, double t1)
{
    const bool has = (t1 > t0);
    if (has) { curStart = t0; curStop = t1; }
    selVisible_ = has;
    applyFieldVisibility();
    if (!has) return;

    auto sync = [this] (juce::TextEditor& e, double v, bool duration)
    {
        // Never clobber a field the user is currently editing.
        if (!e.hasKeyboardFocus (false))
            e.setText (formatFieldTime (v, duration), false);
    };
    sync (startEdit, curStart, false);
    sync (stopEdit, curStop, false);
    sync (lenEdit, curStop - curStart, true);
}

void StatusBar::setPlayhead (double t)
{
    // The playhead readout follows the active time-axis scale mode.
    curPos = t;
    const juce::String s = "Pos: " + formatDisplayedTime (t, timeCtxMode,
                                                          timeCtxOffset, timeCtxRate);
    if (s != playPart) { playPart = s; repaint(); }
    posVisible_ = true;
    applyFieldVisibility();
    // Never clobber a field the user is currently editing.
    if (!posEdit.hasKeyboardFocus (false))
        posEdit.setText (formatFieldTime (t, false), false);
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
    // Left clicks drive the Excel-style multi-selection; a right click shows
    // a small context menu for the segment under the cursor.
    if (!e.mods.isRightButtonDown ())
    {
        owner->onRowClicked (rowNumber, e);
        return;
    }

    auto* d = owner->app.doc ();
    if (d == nullptr || rowNumber < 0 || rowNumber >= (int) d->sctt.segments.size ()) return;

    // With a multi-selection the menu acts on every selected segment.
    const int selCount = (int) owner->selectedRowIndices ().size ();

    juce::PopupMenu menu;
    menu.addItem (1, "Edit...");
    menu.addItem (2, selCount > 1 ? "Add as markers" : "Add as marker");
    menu.addItem (3, "Find...\tCtrl+F");
    menu.addSeparator();
    menu.addItem (4, "Add entry");
    menu.addItem (5, "Remove selected");
    menu.addItem (6, "Copy Selected");
    menu.addItem (7, "Copy All");
    menu.addItem (8, "Paste");
    // Options() targets the current mouse position - exactly where the user
    // right-clicked.
    const int id = menu.showMenu (juce::PopupMenu::Options ());
    if (id == 1)
        owner->showEditDialog (rowNumber);
    else if (id == 2)
        owner->addMarkersForSelection (rowNumber);
    else if (id == 3)
        owner->showFindDialog ();
    else if (id == 4)
        owner->addEntry();
    else if (id == 5)
        owner->removeSelectedRows (rowNumber);
    else if (id == 6)
        owner->copySelectedRows();
    else if (id == 7)
        owner->copyAllRows();
    else if (id == 8)
        owner->pasteFromClipboard ();
}

void TranscriptionPanel::Model::cellDoubleClicked (int rowNumber, int, const juce::MouseEvent&)
{
    owner->activateRow (rowNumber);
}

std::vector<int> TranscriptionPanel::selectedRowIndices() const
{
    return sparseRows (table.getSelectedRows());
}

void TranscriptionPanel::onRowClicked (int row, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu ())
        return;   // right-click: just show the context menu - keep any multi-selection

    table.setWantsKeyboardFocus (true);
    if (table.isShowing ())   // a focus grab on a hidden table is a no-op (and asserts)
        table.grabKeyboardFocus ();

    auto* d = app.doc ();
    const int n = d != nullptr ? (int) d->sctt.segments.size () : 0;

    if (e.mods.isShiftDown ())
    {
        // Excel: the selection becomes EXACTLY anchor..row (JUCE's built-in
        // shift behaviour unions instead).
        const int a = (anchorRow >= 0 && anchorRow < n) ? anchorRow : row;
        juce::SparseSet<int> sel;
        sel.addRange ({ std::min (a, row), std::max (a, row) + 1 });
        table.setSelectedRows (sel, juce::sendNotification);
    }
    else if (e.mods.isCommandDown () || e.mods.isCtrlDown ())
    {
        // JUCE already toggled this row in selectRowsBasedOnModifierKeys before
        // cellClicked fires - just track the anchor here.
        anchorRow = row;
    }
    else
    {
        // Plain click: single selection and move the anchor.
        anchorRow = row;
        juce::SparseSet<int> sel;
        sel.addRange ({ row, row + 1 });
        table.setSelectedRows (sel, juce::sendNotification);
    }
}

void TranscriptionPanel::activateRow (int rowNumber)
{
    auto* d = app.doc ();
    if (d == nullptr || rowNumber < 0 || rowNumber >= (int) d->sctt.segments.size ()) return;

    // Jump to the segment, select its range and make sure both views show it
    // (with a little breathing room on each side).
    const ScttSegment& s = d->sctt.segments [(size_t) rowNumber];
    app.setPlayhead (s.startSec);
    if (s.endSec > s.startSec)
        app.setSelection ({ s.startSec, s.endSec });
    app.ensureTimeRangeVisible (s.startSec, s.endSec);
    if (app.onStateChange)
        app.onStateChange ();   // push the new cursor/selection to both views
}

void TranscriptionPanel::addMarkerForRow (int rowNumber)
{
    auto* d = app.doc ();
    if (d == nullptr || rowNumber < 0 || rowNumber >= (int) d->sctt.segments.size ()) return;

    createMarkerForSegment (d->sctt.segments [(size_t) rowNumber]);
    refresh ();
    if (app.onStateChange) app.onStateChange ();   // views + markers panel
}

int TranscriptionPanel::addMarkersForSelection (int fallbackRow)
{
    auto* d = app.doc ();
    if (d == nullptr) return 0;

    std::vector<int> rows = selectedRowIndices ();
    if (rows.empty ())
        rows.push_back (fallbackRow);

    int added = 0;
    for (int r : rows)
    {
        if (r < 0 || r >= (int) d->sctt.segments.size ()) continue;
        createMarkerForSegment (d->sctt.segments [(size_t) r]);
        ++added;
    }

    if (added > 0)
    {
        refresh ();
        if (app.onStateChange) app.onStateChange ();   // views + markers panel
    }
    return added;
}

void TranscriptionPanel::createMarkerForSegment (const ScttSegment& s)
{
    juce::String full = juce::String::fromUTF8 (s.text.c_str(), (int) s.text.size()).trim ();
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
}

void TranscriptionPanel::showEditDialog (int rowNumber)
{
    auto* d = app.doc ();
    if (d == nullptr || rowNumber < 0 || rowNumber >= (int) d->sctt.segments.size ()) return;
    const ScttSegment& s = d->sctt.segments [(size_t) rowNumber];

    juce::AlertWindow dlg ("Edit Segment",
                          "Start / end in seconds (m:ss.ddd or plain seconds).",
                          juce::AlertWindow::NoIcon);
    dlg.setColour (juce::AlertWindow::backgroundColourId, juce::Colour::fromRGB (45, 45, 48));
    dlg.addTextEditor ("start", formatTimeCode (s.startSec), "Start:");
    dlg.addTextEditor ("end",   formatTimeCode (s.endSec),   "End:");
    dlg.addTextEditor ("text",  juce::String::fromUTF8 (s.text.c_str(), (int) s.text.size()), "Text:");

    auto* startEd = dlg.getTextEditor ("start");
    dlg.addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dlg.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    if (startEd != nullptr)
    {
        startEd->setWantsKeyboardFocus (true);
        startEd->selectAll();
    }

    // Show the dialog (modal, without taking focus) before focusing the
    // field: JUCE ignores focus grabs on components that are not showing.
    dlg.enterModalState (false);
    if (startEd != nullptr)
        startEd->grabKeyboardFocus();

    const int result = dlg.runModalLoop ();
    auto* endEd  = dlg.getTextEditor ("end");
    auto* textEd = dlg.getTextEditor ("text");
    if (result != 1 || startEd == nullptr) return;

    double t0 = 0.0, t1 = 0.0;
    const juce::String endText = (endEd != nullptr) ? endEd->getText() : juce::String();
    if (!parseTimeText (startEd->getText(), t0) || !parseTimeText (endText, t1))
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Edit Segment",
                                            "Please enter valid times for Start and End.",
                                            "", this);
        return;
    }

    const juce::String text = (textEd != nullptr) ? textEd->getText().trim() : juce::String();
    if (!applySegmentEdit (rowNumber, t0, t1, text))
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                            "Edit Segment",
                                            "Times must be non-negative.",
                                            "", this);
}

bool TranscriptionPanel::applySegmentEdit (int rowNumber, double startSec, double endSec,
                                           const juce::String& text)
{
    auto* d = app.doc ();
    if (d == nullptr || rowNumber < 0 || rowNumber >= (int) d->sctt.segments.size ()) return false;
    if (!std::isfinite (startSec) || !std::isfinite (endSec)) return false;
    if (startSec < 0.0 || endSec < 0.0) return false;

    double a = startSec, b = endSec;
    if (b < a) std::swap (a, b);   // tolerate swapped times

    ScttSegment& s = d->sctt.segments [(size_t) rowNumber];
    s.startSec = a;
    s.endSec   = b;
    s.text     = text.trim().toStdString();

    d->setTranscription (std::vector<ScttSegment> (d->sctt.segments));   // persist
    refresh ();
    if (app.onStateChange) app.onStateChange ();
    return true;
}

int TranscriptionPanel::addEntry()
{
    auto* d = app.doc ();
    if (d == nullptr) return -1;

    const juce::Range<double> sel = app.selection;
    ScttSegment s;
    if (sel.getLength () > 0.0)
    {
        s.startSec = std::max (0.0, sel.getStart());
        s.endSec   = std::max (s.startSec, sel.getEnd());
    }
    else
    {
        s.startSec = std::max (0.0, app.playhead);
        s.endSec   = s.startSec;
    }

    // Insert in sorted order so the list stays chronological.
    auto segs = d->sctt.segments;
    size_t pos = segs.size();
    for (size_t i = 0; i < segs.size (); ++i)
        if (segs[i].startSec > s.startSec) { pos = i; break; }
    segs.insert (segs.begin () + pos, s);

    d->setTranscription (std::move (segs));
    refresh ();
    if (app.onStateChange) app.onStateChange ();

    juce::SparseSet<int> rows;
    rows.addRange ({ (int) pos, (int) pos + 1 });
    table.setSelectedRows (rows, juce::sendNotification);
    return (int) pos;
}

void TranscriptionPanel::removeSelectedRows (int fallbackRow)
{
    auto* d = app.doc ();
    if (d == nullptr || d->sctt.segments.empty ()) return;

    std::vector<int> rows;
    for (int r : sparseRows (table.getSelectedRows()))
        if (r >= 0 && r < (int) d->sctt.segments.size())
            rows.push_back (r);
    if (rows.empty () && fallbackRow >= 0 && fallbackRow < (int) d->sctt.segments.size())
        rows.push_back (fallbackRow);
    if (rows.empty ()) return;

    std::sort (rows.begin (), rows.end (), std::greater<int>());
    auto segs = d->sctt.segments;
    for (int r : rows)
        segs.erase (segs.begin () + r);
    d->setTranscription (std::move (segs));

    table.setSelectedRows (juce::SparseSet<int>(), juce::dontSendNotification);
    refresh ();
    if (app.onStateChange) app.onStateChange ();
}

// Parses "M:SS.mmm" time codes as written by formatTimeCode, or plain
// seconds; used when pasting transcription rows from the clipboard.
static bool parseTimeCodeForPaste (const juce::String& in, double& outSecs)
{
    const juce::String s = in.trim ();

    auto parseDecimal = [] (const juce::String& t, double& vOut) -> bool
    {
        if (t.isEmpty ()) return false;
        double intPart = 0.0, fracPart = 0.0, fracScale = 1.0;
        bool seenDot = false, anyDigit = false;
        for (int i = 0; i < t.length (); ++i)
        {
            const juce_wchar ch = t[(size_t) i];
            if (ch >= '0' && ch <= '9')
            {
                const double d = (double) (ch - '0');
                if (!seenDot)
                    intPart = intPart * 10.0 + d;
                else
                {
                    fracScale *= 0.1;
                    fracPart += d * fracScale;
                }
                anyDigit = true;
            }
            else if (ch == '.' && !seenDot)
                seenDot = true;
            else
                return false;
        }
        if (!anyDigit) return false;
        vOut = intPart + fracPart;
        return true;
    };

    double total = 0.0;
    // int on purpose: indexOf returns -1 when the character is absent.
    const int colonPos = s.indexOf (0, ":");
    if (colonPos >= 0)
    {
        double mins = 0.0, secs = 0.0;
        if (!parseDecimal (s.upToFirstOccurrenceOf (":", false, false), mins)) return false;
        if (!parseDecimal (s.fromFirstOccurrenceOf (":", false, false), secs)) return false;
        total = mins * 60.0 + secs;
    }
    else if (!parseDecimal (s, total))
        return false;

    outSecs = total;
    return true;
}

juce::String TranscriptionPanel::buildSegmentsTsv (const std::vector<int>& rows) const
{
    auto* d = app.doc ();
    juce::String out;
    for (int r : rows)
    {
        if (d == nullptr || r < 0 || r >= (int) d->sctt.segments.size ()) continue;
        const ScttSegment& s = d->sctt.segments [(size_t) r];
        // One line per segment: Excel rows must not contain newlines.
        juce::String txt = juce::String (s.text)
                               .replaceCharacter ('\r', ' ')
                               .replaceCharacter ('\n', ' ');
        if (!out.isEmpty ()) out += "\n";
        out += formatTimeCode (s.startSec) + "\t" + formatTimeCode (s.endSec) + "\t" + txt;
    }
    return "Start\tEnd\tText\n" + out;
}

void TranscriptionPanel::copySelectedRows()
{
    auto* d = app.doc ();
    if (d == nullptr || d->sctt.segments.empty ()) return;

    std::vector<int> rows;
    for (int r : sparseRows (table.getSelectedRows()))
        if (r >= 0 && r < (int) d->sctt.segments.size ())
            rows.push_back (r);
    if (!rows.empty ())
        juce::SystemClipboard::copyTextToClipboard (buildSegmentsTsv (rows));
}

void TranscriptionPanel::copyAllRows()
{
    auto* d = app.doc ();
    if (d == nullptr || d->sctt.segments.empty ()) return;

    std::vector<int> rows (d->sctt.segments.size());
    for (size_t i = 0; i < rows.size (); ++i) rows[i] = (int) i;
    juce::SystemClipboard::copyTextToClipboard (buildSegmentsTsv (rows));
}

void TranscriptionPanel::pasteFromClipboard()
{
    auto* d = app.doc ();
    if (d == nullptr) return;

    // The Windows clipboard can be briefly held by another process, in which
    // case the read silently comes back empty. Retry a few times before
    // treating it as a genuinely empty clipboard.
    juce::String text = juce::SystemClipboard::getTextFromClipboard();
    for (int i = 0; i < 8 && text.isEmpty (); ++i)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (4);
        text = juce::SystemClipboard::getTextFromClipboard();
    }

    std::vector<ScttSegment> newSegs;
    bool sawHeader = false;

    const juce::StringArray lines = juce::StringArray::fromLines (text);
    for (int i = 0; i < lines.size (); ++i)
    {
        const juce::String line = lines[i].trim ();
        if (line.isEmpty ()) continue;

        // Optional Excel header row: "Start\tEnd\tText".
        if (!sawHeader && line.startsWithIgnoreCase ("Start"))
        { sawHeader = true; continue; }

        const int t1 = line.indexOf (0, "\t");
        if (t1 < 0) continue;   // not a data row - skip it
        const juce::String startStr = line.upToFirstOccurrenceOf ("\t", false, false);
        const juce::String rest     = line.fromFirstOccurrenceOf ("\t", false, false);

        double st = 0.0;
        if (!parseTimeCodeForPaste (startStr, st)) continue;   // malformed - skip the row

        juce::String endStr, txtPart;
        const int t2 = rest.indexOf (0, "\t");
        if (t2 >= 0)
        {
            endStr  = rest.upToFirstOccurrenceOf ("\t", false, false);
            txtPart = rest.fromFirstOccurrenceOf ("\t", false, false);
        }
        else
            txtPart = rest;

        double en = st;   // no/invalid end column -> point segment at the start
        if (!endStr.trim ().isEmpty ())
        {
            if (!parseTimeCodeForPaste (endStr, en)) en = st;
            if (en < st) std::swap (st, en);
        }

        ScttSegment s;
        s.startSec = st;
        s.endSec   = en;
        s.text     = txtPart.trim ().toStdString ();
        newSegs.push_back (s);
    }

    if (newSegs.empty ())
    {
        // An empty clipboard is a silent no-op; only complain when there was
        // text on the clipboard but nothing parseable in it.
        if (!text.trim ().isEmpty ())
            showSelectionMessage ("Paste",
                                  "No transcription rows found on the clipboard.\n"
                                  "Expected tab-separated Start, End and Text columns.");
        return;
    }

    // Insert everything in sorted order and remember where each row landed.
    auto segs = d->sctt.segments;
    std::vector<int> newRows;
    for (const ScttSegment& s : newSegs)
    {
        size_t pos = segs.size ();
        for (size_t i = 0; i < segs.size (); ++i)
            if (segs[i].startSec > s.startSec) { pos = i; break; }
        segs.insert (segs.begin () + pos, s);
        newRows.push_back ((int) pos);
    }

    d->setTranscription (std::move (segs));
    refresh ();
    if (app.onStateChange) app.onStateChange ();

    juce::SparseSet<int> rows;
    for (int r : newRows)
        rows.addRange ({ r, r + 1 });
    table.setSelectedRows (rows, juce::sendNotification);
}

TranscriptionPanel::TranscriptionPanel (Application& appIn)
    : app (appIn), runBtn ("Transcribe..."), folderBtn ("Transcribe Folder...")
{
    auto& header = table.getHeader();
    // JUCE TableHeaderComponent column IDs must be non-zero.
    header.addColumn ("#", 1, 40);
    header.addColumn ("Start", 2, 85);
    header.addColumn ("End", 3, 85);
    header.addColumn ("Text", 4, 160);

    addAndMakeVisible (&table);
    addAndMakeVisible (&runBtn);
    addAndMakeVisible (&folderBtn);

    runBtn.onClick = [this] { if (onRunRequested) onRunRequested (); };
    folderBtn.onClick = [this] { if (onRunFolderRequested) onRunFolderRequested (); };

    // Multi-line selection like the marker / selection / drawing lists:
    // plain click = one row, Ctrl/Cmd+click = toggle a row, Shift+click =
    // exact range from the anchor. Right-clicks never disturb it.
    table.setMultipleSelectionEnabled (true);
    table.setSuppressShiftRangeSelection (true);   // we do the exact-range shift select ourselves

    runBtn.setTooltip ("Run Whisper transcription on the whole file. The first use downloads "
                       "the model weights to %APPDATA%/SpectroCraft/models.");
    folderBtn.setTooltip ("Batch-transcribe every .wav in a chosen folder tree with Whisper. "
                          "Files are updated in place and never opened in the editor.");
    table.setTooltip ("Click a row to select it; Ctrl/Cmd+click toggles rows, Shift+click\n"
                      "selects the exact range from the anchor. Double-click jumps to the\n"
                      "segment and selects its range. Right-click edits / adds markers /\n"
                      "searches (Find) or adds / removes entries without disturbing a\n"
                      "multi-selection. Ctrl+F searches, F3 / Ctrl+F3 find next / previous.");

    // Deep registration: right-clicks in the table's empty area (below the
    // last segment - or everywhere when no transcription exists) reach this
    // panel so the context menu can offer "Add entry" there too. Row and
    // header clicks are filtered out in mouseDown().
    table.addMouseListener (this, true);

    refresh();
}

// Right-click on empty panel area or the table's empty background: offer to
// add an entry, copy / paste rows, search and close the panel. Row right-
// clicks are handled by the table's own context menu in Model::cellClicked;
// header clicks keep JUCE's built-in column menu.
void TranscriptionPanel::mouseDown (const juce::MouseEvent& e)
{
    if (!e.mods.isRightButtonDown() || e.mods.isLeftButtonDown()) return;

    const Point<int> local = getLocalPoint (e.eventComponent, Point<int> (e.x, e.y));
    if (table.getBounds ().contains (local))
    {
        // The header has its own context menu; rows have theirs in cellClicked.
        if (table.getHeader ().getBounds ().contains (local)) return;
        if (table.getRowContainingPosition (local.getX (), local.getY ()) >= 0) return;
    }

    juce::PopupMenu m;
    m.addItem (1, "Add entry");
    m.addItem (2, "Copy Selected");
    m.addItem (3, "Copy All");
    m.addItem (4, "Paste");
    m.addItem (6, "Find...\tCtrl+F");
    const int id = m.showMenu (juce::PopupMenu::Options().withMousePosition());
    if (id == 1)
        addEntry();
    else if (id == 2)
        copySelectedRows();
    else if (id == 3)
        copyAllRows();
    else if (id == 4)
        pasteFromClipboard();
    else if (id == 6)
        showFindDialog ();
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
    runBtn.setBounds (btnRow.getX (), btnRow.getY () + 2, 120, 26);
    folderBtn.setBounds (runBtn.getRight () + 8, btnRow.getY () + 2, 165, 26);

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
            activateRow (row);
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
    else   // comment (fills the remainder of the table)
    {
        g.setColour (juce::Colours::lightgrey);
        g.drawText (s.comment, 4, 0, width - 8, height, juce::Justification::centredLeft, false);
    }
}

void SelectionPanel::Model::cellClicked (int rowNumber, int, const juce::MouseEvent& e)
{
    if (rowNumber >= 0 && rowNumber < getNumRows())
        owner->onRowClicked (rowNumber, e);
}

void SelectionPanel::Model::cellDoubleClicked (int rowNumber, int, const juce::MouseEvent&)
{
    // Double-clicking any cell recalls that row's stored selection.
    owner->doRecall (rowNumber);
}

void SelectionPanel::Model::selectedRowsChanged (int)
{
    owner->onSelectionSetChanged ();
}

void SelectionPanel::Model::deleteKeyPressed (int)
{
    // DEL deletes the audio data of the active selection - not list rows.
    owner->deleteAudioData ();
}

SelectionPanel::SelectionPanel (Application& appIn)
    : app (appIn),
      selListDoc (appIn.doc()),   // rows belong to the doc open at construction
      saveBtn ("Store"), recallBtn ("Recall"), deleteBtn ("Delete")
{
    // Names let headless unit tests locate the buttons by name.
    // setComponentID (not setName): findChildWithID matches the component ID.
    saveBtn.setComponentID ("sel-store-btn");
    recallBtn.setComponentID ("sel-recall-btn");
    deleteBtn.setComponentID ("sel-delete-btn");
    auto& header = table.getHeader();
    // JUCE TableHeaderComponent column IDs must be non-zero. The Comment
    // column is not resizable: resized() stretches it to fill the remainder.
    const int fixedFlags = juce::TableHeaderComponent::notResizableOrSortable;
    header.addColumn ("Name", 1, 120, 60, -1, fixedFlags);
    header.addColumn ("Comment", 2, 95, 40, -1, fixedFlags);

    savedLab.setText ("Saved selections", juce::dontSendNotification);

    addAndMakeVisible (&savedLab);
    addAndMakeVisible (&table);
    addAndMakeVisible (&saveBtn);
    addAndMakeVisible (&recallBtn);
    addAndMakeVisible (&deleteBtn);

    // Excel-style multi-selection: Ctrl/Cmd+click toggles rows, Shift+click
    // range-selects from the anchor row and fits all of it in view, DEL
    // removes every selected entry.
    table.setMultipleSelectionEnabled (true);
    table.setSuppressShiftRangeSelection (true);   // we do the exact-range shift select ourselves

    saveBtn.onClick = [this] { doSave (); };
    recallBtn.onClick = [this] { doRecall (primaryRow()); };
    deleteBtn.onClick = [this] { deleteSelected (); };

    saveBtn.setTooltip ("In Time Range mode this stores the current time range; otherwise it "
                        "stores the live spectral selection - or, with none active, the time-range "
                        "selection as a full-height band - under an auto-assigned name like 'Selection 01'.");
    recallBtn.setTooltip ("Replace the current spectral selection with the first selected saved one. "
                          "Clicking or double-clicking its row does the same.");
    deleteBtn.setTooltip ("Remove every selected saved selection from this file's list. DEL instead "
                          "deletes the audio data of the active selection.");

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

    // The row selection belongs to the document whose list is shown: reset it
    // when switching files so stale rows never point at another file's entries.
    if (d != selListDoc)
    {
        selListDoc = d;
        anchorRow = -1;
        table.setSelectedRows (juce::SparseSet<int>(), juce::dontSendNotification);
    }

    // Store is available with a spectral selection OR a time-range selection
    // (the latter is stored as a full-height band).
    saveBtn.setEnabled (app.hasSpectralSelection () || app.selection.getLength () > 0.0);

    const int nRows = d != nullptr ? (int) d->savedSelections.size () : 0;
    const bool haveRow = table.getNumSelectedRows () > 0 && primaryRow () < nRows;
    recallBtn.setEnabled (haveRow);
    deleteBtn.setEnabled (haveRow);
}

void SelectionPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));

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

    // The button strip is pinned to the bottom; the saved-selections list
    // takes all of the space above it (the old Clone & Multiply section was
    // moved into the right-click menu so the list gets more room).
    const int rowH = 28;
    auto btnRow = b.removeFromBottom (rowH + 8);

    savedLab.setBounds (b.getX (), b.getY () + 2, b.getWidth (), 16);
    table.setBounds (b.getX (), savedLab.getBottom () + 4,
                    b.getWidth (), std::max (60, btnRow.getY () - savedLab.getBottom () - 8));

    // The Comment column fills whatever width is left of the Name column.
    auto& hdr = table.getHeader();
    const int nameW = std::min (140, std::max (60, table.getWidth () / 3));
    hdr.setColumnWidth (1, nameW);
    hdr.setColumnWidth (2, std::max (40, table.getWidth () - nameW));

    const int gap = 8;
    const int bw = std::max (52, (btnRow.getWidth () - gap * 2) / 3);
    saveBtn.setBounds (btnRow.getX (), btnRow.getY () + 2, bw, rowH);
    recallBtn.setBounds (saveBtn.getRight () + gap, btnRow.getY () + 2, bw, rowH);
    deleteBtn.setBounds (recallBtn.getRight () + gap, btnRow.getY () + 2, bw, rowH);
}

void SelectionPanel::doSave()
{
    auto* d = app.doc ();
    if (d == nullptr) return;

    SpectralSelection toSave;
    const bool haveTimeSel = app.selection.getLength () > 0.0;

    if (app.selMode == SpectralSelMode::TimeRange && haveTimeSel)
    {
        // The Time Range tool operates on the time selection: store it even
        // when a recalled spectral selection is still live, so storing a
        // sequence of different ranges doesn't re-save the first one.
        const double nyq = app.audioEngine().sampleRate() * 0.5;
        toSave.addRect (app.selection.getStart (), app.selection.getEnd (),
                        0.0, nyq, false);
    }
    else if (!d->spectralSel.empty ())
        toSave = d->spectralSel;   // deep copy of the current shapes
    else if (haveTimeSel)
    {
        // No spectral selection: store the time range as a full-height band so
        // it can be recalled/edited like any other spectral selection.
        const double nyq = app.audioEngine().sampleRate() * 0.5;
        toSave.addRect (app.selection.getStart (), app.selection.getEnd (),
                        0.0, nyq, false);
    }
    else return;   // nothing to store

    SavedSpectralSelection s;
    s.name = nextSelectionName ();   // "Selection 01", "Selection 02", ...
    s.data = std::move (toSave);
    d->addSavedSelection (std::move (s));

    // The list persists with the file's unified metadata on Save.
    refresh();
}

void SelectionPanel::doRecall (int row)
{
    auto* d = app.doc ();
    if (d == nullptr || row < 0 || row >= (int) d->savedSelections.size ()) return;
    const auto& sel = d->savedSelections [(size_t) row];

    SpectralSelection merged;
    merged.appendShapesFrom (sel.data);
    app.setSpectralSelectionFrom (merged);

    // Make sure both views show the recalled selection's time range.
    if (!sel.data.empty())
    {
        const auto ext = sel.data.timeExtent();
        app.ensureTimeRangeVisible (ext.getStart(), ext.getEnd());
    }
    refresh();
}

void SelectionPanel::recallSelected ()
{
    auto* d = app.doc ();
    if (d == nullptr) return;

    SpectralSelection merged;
    double t0 = 0.0, t1 = 0.0;
    bool any = false;
    for (int r : sparseRows (table.getSelectedRows()))
    {
        if (r < 0 || r >= (int) d->savedSelections.size ()) continue;
        const auto& s = d->savedSelections [(size_t) r];
        if (s.data.empty ()) continue;

        merged.appendShapesFrom (s.data);

        const auto ext = s.data.timeExtent();
        if (!any)
        {
            t0 = ext.getStart ();
            t1 = ext.getEnd ();
            any = true;
        }
        else
        {
            t0 = std::min (t0, ext.getStart());
            t1 = std::max (t1, ext.getEnd());
        }
    }
    if (!any) return;

    // The union becomes the ACTUAL live spectral selection (not an overlay),
    // and both views are fitted to its time range with the usual 10% margin.
    app.setSpectralSelectionFrom (merged);
    app.ensureTimeRangeVisible (t0, t1);
}

void SelectionPanel::deleteAudioData ()
{
    // MainWindow routes this through its destructive-edit path (async
    // materialisation of streamed files + full view refresh).
    if (onDeleteAudioRequested) { onDeleteAudioRequested (); return; }

    std::string err;
    if (!app.del (&err))
        showSelectionMessage ("Delete", juce::String (err.c_str ()));
    else if (app.onStateChange)
        app.onStateChange ();   // the views must drop the deleted range
}

// Right-click: select the row under the cursor when nothing is selected yet
// (an existing multi-selection is left alone), then show the context menu.
void SelectionPanel::mouseDown (const juce::MouseEvent& e)
{
    if (!e.mods.isRightButtonDown() || e.mods.isLeftButtonDown()) return;

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

    int row = -1;
    for (int r = 0; r < table.getNumRows(); ++r)
        if (table.getCellPosition (1, r, true).contains ((int) pInTable.x, (int) pInTable.y))
        {
            row = r;
            break;
        }

    if (row >= 0 && table.getNumSelectedRows () == 0)
    {
        anchorRow = row;
        juce::SparseSet<int> sel;
        sel.addRange ({ row, row + 1 });
        table.setSelectedRows (sel, juce::sendNotification); // -> onSelectionSetChanged
    }

    showLoadContextMenu ();
}

void SelectionPanel::showLoadContextMenu()
{
    const int nSel = table.getNumSelectedRows ();
    const bool hasLive = app.hasSpectralSelection ();

    juce::PopupMenu m;
    m.addItem (10, "Recall", nSel > 0);
    m.addItem (12, "Edit...", nSel > 0);
    m.addItem (11, "Delete Selected", nSel > 0);
    m.addSeparator();
    m.addItem (20, "Clone Harmonics by Numbers...", hasLive);
    m.addItem (21, app.isClonePicking () ? "Stop Picking" : "Clone Harmonics by Mouse", hasLive);
    m.addSeparator();
    m.addItem (30, "Fit View to Selected", nSel > 0);

    const int id = m.showMenu (juce::PopupMenu::Options().withMousePosition());
    if (id == 10)
        doRecall (primaryRow());
    else if (id == 12)
        showEditDialog ();
    else if (id == 11)
        deleteSelected ();
    else if (id == 20)
        showCloneNumbersDialog();
    else if (id == 21)
        toggleClonePick ();
    else if (id == 30)
        fitViewToSelected ();
}

bool SelectionPanel::loadSelectionsFromFile (const juce::File& f)
{
    auto* d = app.doc ();
    if (d == nullptr || !f.existsAsFile ()) return false;
    if (!f.getFileExtension().equalsIgnoreCase (".json")) return false;

    std::vector<SavedSpectralSelection> list;
    if (!deserialiseSavedSelections (f.loadFileAsString (), list)) return false;

    d->savedSelections = std::move (list);
    d->metaDirty = true;   // the imported list is not on disk yet
    // The imported list persists with the file's unified metadata on Save.
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
    deleteSelected ();
}

int SelectionPanel::primaryRow() const
{
    return table.getNumSelectedRows () > 0 ? table.getSelectedRow (0) : -1;
}

void SelectionPanel::onSelectionSetChanged()
{
    if (table.getNumSelectedRows () > 0)
        recallSelected ();   // union -> live spectral selection + view fit
    refresh ();   // button enablement follows the selection set
}

std::vector<int> SelectionPanel::selectedRowIndices() const
{
    return sparseRows (table.getSelectedRows());
}

// Excel-style click handling. JUCE already applied the plain-click /
// Ctrl+click part before this runs; Shift+click gets replaced here with the
// EXACT range from the anchor (JUCE's built-in shift behaviour unions
// instead), and the view is fitted to everything selected.
void SelectionPanel::onRowClicked (int row, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu ())
        return;   // right-click: just show the context menu - keep any multi-selection

    table.setWantsKeyboardFocus (true);
    if (table.isShowing ())   // a focus grab on a hidden table is a no-op (and asserts)
        table.grabKeyboardFocus ();   // so DEL works right after a click

    if (e.mods.isShiftDown ())
    {
        // Excel: the selection becomes EXACTLY anchor..row (JUCE's built-in
        // shift behaviour unions instead), then fit everything selected.
        auto* d = app.doc ();
        const int n = d != nullptr ? (int) d->savedSelections.size () : 0;
        const int a = (anchorRow >= 0 && anchorRow < n) ? anchorRow : row;
        juce::SparseSet<int> sel;
        sel.addRange ({ std::min (a, row), std::max (a, row) + 1 });
        table.setSelectedRows (sel, juce::sendNotification);
        fitViewToSelected ();
    }
    else if (e.mods.isCommandDown () || e.mods.isCtrlDown ())
    {
        // JUCE already toggled this row in selectRowsBasedOnModifierKeys before
        // cellClicked fires - just track the anchor here.
        anchorRow = row;
    }
    else
    {
        // Plain click: single selection and move the anchor.
        anchorRow = row;
        juce::SparseSet<int> sel;
        sel.addRange ({ row, row + 1 });
        table.setSelectedRows (sel, juce::sendNotification);
    }
}

void SelectionPanel::deleteSelected ()
{
    auto* d = app.doc ();
    if (d == nullptr) return;

    std::vector<int> rows;
    for (int r : sparseRows (table.getSelectedRows()))
        if (r >= 0 && r < (int) d->savedSelections.size ())
            rows.push_back (r);
    if (rows.empty()) return;

    // Erase from the bottom up so earlier indices stay valid.
    std::sort (rows.begin (), rows.end (), std::greater<int>());
    for (int r : rows)
        d->savedSelections.erase (d->savedSelections.begin () + r);
    d->metaDirty = true;   // the list no longer matches what is on disk

    anchorRow = -1;
    table.setSelectedRows (juce::SparseSet<int>(), juce::dontSendNotification);
    // The list persists with the file's unified metadata on Save.
    refresh();
}

void SelectionPanel::fitViewToSelected ()
{
    auto* d = app.doc ();
    if (d == nullptr) return;

    double t0 = 0.0, t1 = 0.0;
    bool any = false;
    for (int r : sparseRows (table.getSelectedRows()))
    {
        if (r < 0 || r >= (int) d->savedSelections.size ()) continue;
        const auto ext = d->savedSelections [(size_t) r].data.timeExtent();
        if (!any)
        {
            t0 = ext.getStart ();
            t1 = ext.getEnd ();
            any = true;
        }
        else
        {
            t0 = std::min (t0, ext.getStart());
            t1 = std::max (t1, ext.getEnd());
        }
    }
    if (!any) return;
    app.ensureTimeRangeVisible (t0, t1);   // adds the usual 10% margin
}

juce::String SelectionPanel::nextSelectionName() const
{
    auto* d = app.doc ();
    const int n = d != nullptr ? (int) d->savedSelections.size () : 0;
    for (int i = 1; i <= n + 1; ++i)
    {
        const juce::String cand = "Selection " + juce::String (i).paddedLeft ('0', 2);
        bool used = false;
        if (d != nullptr)
            for (const auto& e : d->savedSelections)
                if (e.name == cand) { used = true; break; }
        if (!used) return cand;
    }
    return "Selection " + juce::String (n + 1).paddedLeft ('0', 2);
}

void SelectionPanel::showEditDialog()
{
    auto* d = app.doc ();
    const int row = primaryRow();
    if (d == nullptr || row < 0 || row >= (int) d->savedSelections.size ()) return;
    const SavedSpectralSelection& s = d->savedSelections [(size_t) row];

    juce::AlertWindow dlg ("Edit Selection",
                          "Name and comment:",
                          juce::AlertWindow::NoIcon);
    dlg.setColour (juce::AlertWindow::backgroundColourId,
                   juce::Colour::fromRGB (45, 45, 48));

    // This JUCE build's addTextEditor returns void; fetch the editors by name.
    dlg.addTextEditor ("name", s.name, "Name:");
    dlg.addTextEditor ("comment", s.comment, "Comment:");
    auto* nameEd = dlg.getTextEditor ("name");
    dlg.addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dlg.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    if (nameEd != nullptr)
    {
        nameEd->setWantsKeyboardFocus (true);
        nameEd->selectAll();
    }

    // Show the dialog (modal, without taking focus) before focusing the
    // field: JUCE ignores focus grabs on components that are not showing.
    dlg.enterModalState (false);
    if (nameEd != nullptr)
        nameEd->grabKeyboardFocus();

    if (dlg.runModalLoop () != 1 || nameEd == nullptr) return;
    auto* commentEd = dlg.getTextEditor ("comment");
    const juce::String comment = commentEd != nullptr ? commentEd->getText ().trim () : s.comment;
    applyNameComment (nameEd->getText (), comment);
}

void SelectionPanel::applyNameComment (const juce::String& name, const juce::String& comment)
{
    auto* d = app.doc ();
    const int row = primaryRow();
    if (d == nullptr || row < 0 || row >= (int) d->savedSelections.size ()) return;

    // An empty title keeps the old name; an empty comment is fine.
    const juce::String trimmedName = name.trim ();
    if (!trimmedName.isEmpty ())
        d->savedSelections [(size_t) row].name = trimmedName;
    d->savedSelections [(size_t) row].comment = comment;

    refresh();   // repaint the table; persists with the file's metadata on Save
}

void SelectionPanel::showCloneNumbersDialog()
{
    auto* d = app.doc ();
    if (d == nullptr || d->spectralSel.empty ())
    {
        showSelectionMessage ("Clone Harmonics by Numbers",
                              "There is no spectral selection to clone.");
        return;
    }

    juce::AlertWindow dlg ("Clone Harmonics by Numbers",
                          "Multipliers of the current selection, e.g. 0.5, 2, 5:",
                          juce::AlertWindow::NoIcon);
    dlg.setColour (juce::AlertWindow::backgroundColourId,
                   juce::Colour::fromRGB (45, 45, 48));

    // This JUCE build's addTextEditor returns void; fetch the editor by name.
    dlg.addTextEditor ("mults", "2, 3", "Multipliers:");
    auto* ed = dlg.getTextEditor ("mults");
    dlg.addButton ("Clone", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dlg.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    if (ed != nullptr)
    {
        ed->setWantsKeyboardFocus (true);
        ed->selectAll();
    }

    // Show the dialog (modal, without taking focus) before focusing the
    // field: JUCE ignores focus grabs on components that are not showing.
    dlg.enterModalState (false);
    if (ed != nullptr)
        ed->grabKeyboardFocus();

    if (dlg.runModalLoop () != 1 || ed == nullptr) return;

    std::string err;
    const auto mults = Application::parseMultiplierList (ed->getText (), &err);
    if (!app.cloneAndMultiplySelection (mults, &err))
        showSelectionMessage ("Clone Harmonics by Numbers", juce::String (err));
}

void SelectionPanel::toggleClonePick ()
{
    auto* d = app.doc ();
    if (!app.isClonePicking () && (d == nullptr || d->spectralSel.empty()))
    {
        showSelectionMessage ("Clone Harmonics by Mouse",
                              "There is no spectral selection to clone.");
        return;
    }

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

    // Switching scripts keeps the edits of the one being left (only Run used
    // to write the editor text to disk, so every other path lost them).
    scriptBox.onChange = [this]
    {
        if (!saveCurrentScriptIfModified ())
        {   // could not save: stay on the edited script instead of dropping it
            if (currentIdx >= 0)
                scriptBox.setSelectedItemIndex (currentIdx, juce::dontSendNotification);
            return;
        }
        loadCurrentScript ();
    };
    addBtn.setButtonText ("Add");
    removeBtn.setButtonText ("Remove");
    runBtn.setButtonText ("Run");

    addBtn.onClick     = [this] { showAddDialog (); };
    removeBtn.onClick  = [this] { confirmRemove (); };
    runBtn.onClick     = [this] { requestRun (); };

    refreshScripts ();
}

// Right-click menu: "Reset to original" for built-in example scripts.
void PythonPanel::mouseDown (const juce::MouseEvent& e)
{
    if (!e.mods.isRightButtonDown()) return;
    juce::PopupMenu m;
    const int resetId = 1;
    if (canResetCurrentScript ())
        m.addItem (resetId, "Reset to original");
    const int sel = m.showMenu (juce::PopupMenu::Options().withMousePosition());
    if (sel == resetId) confirmResetToOriginal ();
}

bool PythonPanel::canResetCurrentScript () const
{
    if (currentIdx < 0 || currentIdx >= (int) scripts.size ()) return false;
    return !sc::py::defaultScriptText (scripts[(size_t) currentIdx].getFileName()).isEmpty ();
}

void PythonPanel::resetCurrentScriptToOriginal ()
{
    if (!canResetCurrentScript ()) return;
    const juce::File f = scripts[(size_t) currentIdx];
    if (!f.replaceWithText (sc::py::defaultScriptText (f.getFileName())))
    {
        showSelectionMessage ("Reset Script", "Could not write '" + f.getFullPathName () + "'.");
        return;
    }
    loadCurrentScript ();   // reload the editor text from disk
}

bool PythonPanel::saveCurrentScriptIfModified ()
{
    if (currentIdx < 0 || currentIdx >= (int) scripts.size ()) return true;
    const juce::String text = editor.getText ();
    if (text == loadedText) return true;   // nothing typed since the last load/save

    const juce::File f = scripts[(size_t) currentIdx];
    if (!f.replaceWithText (text))
    {
        showSelectionMessage ("Python", "Could not save '" + f.getFullPathName () + "'.");
        return false;
    }
    loadedText = text;
    return true;
}

void PythonPanel::confirmResetToOriginal ()
{
    if (!canResetCurrentScript ()) return;
    auto* w = new juce::AlertWindow ("Reset Script",
        "Are you sure you want to reset this script to the default value?",
        juce::AlertWindow::QuestionIcon, this);
    w->setColour (juce::AlertWindow::backgroundColourId,
                 juce::Colour::fromRGB (45, 45, 48));
    w->addButton ("Reset", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    const int r = w->runModalLoop ();
    delete w;
    if (r == 1) resetCurrentScriptToOriginal ();
}

void PythonPanel::refreshScripts ()
{
    const juce::String keepPath = currentIdx >= 0 && currentIdx < (int) scripts.size ()
                                       ? scripts[(size_t) currentIdx].getFullPathName() : "";

    scripts = sc::py::listScripts (scriptDir);
    currentIdx = -1;
    for (int i = 0; i < (int) scripts.size (); ++i)
        if (scripts[(size_t) i].getFullPathName () == keepPath) { currentIdx = i; break; }

    // Restore the last-used script from a previous session, or fall back to
    // the first script in the list so the panel never starts empty.
    if (currentIdx < 0)
    {
        const juce::String saved = app.recentStore->getValue ("python_last_script");
        for (int i = 0; i < (int) scripts.size (); ++i)
            if (!saved.isEmpty () && scripts[(size_t) i].getFileName () == saved)
            { currentIdx = i; break; }
    }
    if (currentIdx < 0 && !scripts.empty ()) currentIdx = 0;

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
        loadedText = {};
        return;
    }

    currentIdx = idx;
    editor.setText (scripts[(size_t) idx].loadFileAsString (),
                    juce::dontSendNotification);
    loadedText = editor.getText ();   // baseline for saveCurrentScriptIfModified

    // Remember this script as the last-used one for the next session.
    app.recentStore->setValue ("python_last_script",
                               scripts[(size_t) idx].getFileName());
    app.recentStore->save ();
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
    }

    // Show the dialog (modal, without taking focus) before focusing the
    // field: JUCE ignores focus grabs on components that are not showing.
    w->enterModalState (false);
    if (nameEdit != nullptr)
        nameEdit->grabKeyboardFocus();

    const int result = w->runModalLoop ();
    // Read the name BEFORE deleting the dialog: nameEdit is one of its children.
    const juce::String typed = w->getTextEditorContents ("name");
    delete w;
    if (result != 1) return;

    // Sanitise: strip path separators / invalid characters, force a .py suffix.
    juce::String name = typed.trim ();
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
        err->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        err->runModalLoop ();   // enterModalState() + delete never showed it
        delete err;
        return;
    }

    // Keep the edits of the current script before the editor switches away.
    if (!saveCurrentScriptIfModified ()) return;
    if (!f.replaceWithText (sc::py::newScriptTemplate ()))
    {
        showSelectionMessage ("New Script", "Could not create '" + f.getFullPathName () + "'.");
        return;
    }
    refreshScripts ();

    for (int i = 0; i < (int) scripts.size (); ++i)
        if (scripts[(size_t) i].getFullPathName () == f.getFullPathName ())
        {
            currentIdx = i;
            // This JUCE build uses 0-based combo positions.
            scriptBox.setSelectedItemIndex (i, juce::NotificationType::dontSendNotification);
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
    if (!f.deleteFile ())
    {
        showSelectionMessage ("Delete Script", "Could not delete '" + f.getFullPathName () + "'.");
        return;
    }
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
        info->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        info->runModalLoop ();   // enterModalState() + delete never showed it
        delete info;
        return;
    }

    if (currentIdx < 0 || currentIdx >= (int) scripts.size ())
    {
        auto* info = new juce::AlertWindow ("Python", "Select a script first.",
                                            juce::AlertWindow::InfoIcon, this);
        info->setColour (juce::AlertWindow::backgroundColourId,
                         juce::Colour::fromRGB (45, 45, 48));
        info->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        info->runModalLoop ();   // enterModalState() + delete never showed it
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


// ============================================================= Drawing panel

static void drawEyeIcon (juce::Graphics& g, juce::Rectangle<float> r, bool visible)
{
    // Simple almond eye: ellipse outline + pupil. Hidden items are drawn in
    // grey with a diagonal strike line through them.
    const auto col = visible ? juce::Colours::white : juce::Colour (0xFF8A8A8A);
    g.setColour (col);
    g.drawEllipse (r.reduced (1.5f), 1.4f);
    const float pr = r.getHeight () * 0.20f;
    g.fillEllipse (juce::Rectangle<float> (r.getCentreX () - pr, r.getCentreY () - pr,
                                           2.0f * pr, 2.0f * pr));
    if (!visible)
        g.drawLine (r.getX () + 1.0f, r.getBottom () - 1.0f,
                    r.getRight () - 1.0f, r.getY () + 1.0f, 1.4f);
}

void EyeButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    const auto b = getLocalBounds ().toFloat ();
    g.setColour (down ? juce::Colour (0xFF2A3148)
                      : (over ? juce::Colour (0xFF46537A)
                             : juce::Colour (0xFF39415A)));
    g.fillRoundedRectangle (b, 3.0f);
    drawEyeIcon (g, b.reduced (6.0f), shown);
}

// Custom colour picker: this JUCE build has no ColourChooser widget, so the
// dialog is an AlertWindow with R/G/B sliders (named components get auto-
// drawn labels) and a live preview swatch on top. Returns the chosen colour,
// or `initial` when cancelled.
static juce::Colour showColourPickerDialog (juce::Colour initial)
{
    class Swatch : public juce::Component
    {
    public:
        juce::Colour c;
        void paint (juce::Graphics& g) override
        {
            g.setColour (c);
            g.fillAll ();
        }
    };

    // Owning: the swatch and sliders below are added with new.
    sc::OwningAlertWindow w ("Drawing colour", "Choose the ink colour.",
                        juce::AlertWindow::NoIcon);
    w.setColour (juce::AlertWindow::backgroundColourId, juce::Colour::fromRGB (45, 45, 48));

    auto* sw = new Swatch ();
    sw->c = initial;
    sw->setSize (200, 24);
    w.addCustomComponent (sw);   // unnamed: no label is drawn above it

    std::vector<juce::Slider*> sliders;
    const char* names[3] = { "Red", "Green", "Blue" };
    int inits[3] = { initial.getRed (), initial.getGreen (), initial.getBlue () };
    for (int i = 0; i < 3; ++i)
    {
        auto* s = new juce::Slider ();
        s->setRange (0.0, 255.0, 1.0);
        s->setValue ((double) inits[i]);
        s->setSize (240, 20);
        s->setName (names[i]);   // AlertWindow draws this as a label above the slider
        w.addCustomComponent (s);
        sliders.push_back (s);
    }

    auto updateSwatch = [&] ()
    {
        sw->c = juce::Colour::fromRGB ((juce::uint8) sliders[0]->getValue (),
                                       (juce::uint8) sliders[1]->getValue (),
                                       (juce::uint8) sliders[2]->getValue ());
        sw->repaint ();
    };
    for (auto* s : sliders)
        s->onValueChange = [&updateSwatch] () { updateSwatch (); };

    w.addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    if (w.runModalLoop () == 1)
        return juce::Colour::fromRGB ((juce::uint8) sliders[0]->getValue (),
                                      (juce::uint8) sliders[1]->getValue (),
                                      (juce::uint8) sliders[2]->getValue ());
    return initial;
}

DrawingPanel::Model::Model (DrawingPanel* o) : owner (o) {}

int DrawingPanel::Model::getNumRows()
{
    auto* d = owner->app.doc ();
    return d != nullptr ? (int) d->drawings.size () : 0;
}

void DrawingPanel::Model::paintRowBackground (juce::Graphics& g, int, int width, int height,
                                              bool selected)
{
    if (selected)
    {
        g.setColour (juce::Colour::fromRGB (60, 80, 120));
        g.fillRect (0, 0, width, height);
    }
}

void DrawingPanel::Model::paintCell (juce::Graphics& g, int rowNumber, int columnId,
                                     int width, int height, bool)
{
    auto* d = owner->app.doc ();
    if (d == nullptr || rowNumber < 0 || rowNumber >= (int) d->drawings.size ()) return;
    const DrawingItem& it = d->drawings [(size_t) rowNumber];

    g.setFont (11.0f);
    if (columnId == 1)   // per-item visibility eye icon
    {
        drawEyeIcon (g, juce::Rectangle<float> ((float) width / 2.0f - 8.0f,
                                                (float) height / 2.0f - 6.0f,
                                                16.0f, 12.0f),
                     !it.hidden);
    }
    else if (columnId == 2)   // type
    {
        g.setColour (juce::Colours::white);
        g.drawText (DrawingItem::kindName (it.kind), 4, 0, width - 8, height,
                    juce::Justification::centredLeft, false);
    }
    else   // comment (fills the remainder of the table)
    {
        g.setColour (juce::Colours::lightgrey);
        g.drawText (it.comment, 4, 0, width - 8, height, juce::Justification::centredLeft, false);
    }
}

void DrawingPanel::Model::cellClicked (int rowNumber, int columnId, const juce::MouseEvent& e)
{
    if (rowNumber < 0 || rowNumber >= getNumRows ()) return;
    // The eye icon toggles visibility on LEFT clicks only; a right-click there
    // just shows the context menu like any other cell.
    if (columnId == 1 && !e.mods.isRightButtonDown())
    {
        // The list box already auto-selected this row before calling us, which
        // scheduled a view fit - cancel it so the viewed range stays put.
        owner->cancelPendingFit ();
        owner->app.toggleDrawingHidden (rowNumber);
        return;
    }
    owner->onRowClicked (rowNumber, e);
}

void DrawingPanel::Model::cellDoubleClicked (int rowNumber, int, const juce::MouseEvent&)
{
    if (rowNumber < 0 || rowNumber >= getNumRows ()) return;
    owner->anchorRow = rowNumber;
    juce::SparseSet<int> sel;
    sel.addRange ({ rowNumber, rowNumber + 1 });
    owner->table.setSelectedRows (sel, juce::sendNotification);   // -> fitViewToSelected
}

void DrawingPanel::Model::selectedRowsChanged (int)
{
    owner->scheduleFit ();
}

DrawingPanel::~DrawingPanel()
{
    fitState_->alive = false;   // pending fits must never touch freed memory
}

void DrawingPanel::scheduleFit ()
{
    auto state = fitState_;     // lambda keeps the state alive past destruction
    const int token = ++state->token;
    juce::MessageManager::callAsync ([this, state, token]
    {
        if (state->alive && state->token == token)
            fitViewToSelected ();
    });
}

void DrawingPanel::cancelPendingFit ()
{
    ++fitState_->token;   // any scheduled fit now sees a stale token and is discarded
}

DrawingPanel::DrawingPanel (Application& appIn)
    : app (appIn),
      listDoc (appIn.doc()),   // rows belong to the doc open at construction
      fitState_ (std::make_shared<FitState>())
{
    // IDs let headless unit tests locate the buttons by name.
    showAllBtn.setComponentID ("draw-showall-btn");
    colourBtn.setComponentID ("draw-colour-btn");
    deleteBtn.setComponentID ("draw-delete-btn");

    auto& header = table.getHeader ();
    const int fixedFlags = juce::TableHeaderComponent::notResizableOrSortable;
    header.addColumn ("Show", 1, 40, 30, -1, fixedFlags);
    header.addColumn ("Type", 2, 95, 60, -1, fixedFlags);
    header.addColumn ("Comment", 3, 120, 40, -1, fixedFlags);

    // Defaults for NEW drawings: persisted in the session settings.
    widthSlider.setRange (0.5, 10.0, 0.5);
    widthSlider.setValue (app.settings.drawWidth);
    textSizeSlider.setRange (6.0, 72.0, 1.0);
    textSizeSlider.setValue (app.settings.drawTextSize);

    // Initial label text (the slider handlers only update on real changes).
    widthLab.setText (juce::String (app.settings.drawWidth, 1), juce::dontSendNotification);
    sizeLab.setText (juce::String ((int) std::lround (app.settings.drawTextSize)), juce::dontSendNotification);

    showAllBtn.onClick = [this] { toggleShowAll (); };
    deleteBtn.onClick = [this] { doDelete (); };
    colourBtn.onClick = [this]
    {
        const juce::int32 argb0 = app.settings.drawColourARGB;
        const auto c = showColourPickerDialog (juce::Colour::fromRGBA ((juce::uint8) ((argb0 >> 16) & 0xFF),
                                                                 (juce::uint8) ((argb0 >> 8) & 0xFF),
                                                                 (juce::uint8) (argb0 & 0xFF),
                                                                 (juce::uint8) ((argb0 >> 24) & 0xFF)));
        app.settings.drawColourARGB = c.getARGB ();
        app.saveSettings ();
        repaint ();
    };

    widthSlider.onValueChange = [this] ()
    {
        const float v = (float) widthSlider.getValue ();
        if (v == app.settings.drawWidth)
            return;   // spurious peer notification: nothing changed
        app.settings.drawWidth = v;
        widthLab.setText (juce::String (app.settings.drawWidth, 1), juce::dontSendNotification);
        app.saveSettings ();
    };
    textSizeSlider.onValueChange = [this] ()
    {
        const float v = (float) textSizeSlider.getValue ();
        if (v == app.settings.drawTextSize)
            return;   // spurious peer notification: nothing changed
        app.settings.drawTextSize = v;
        sizeLab.setText (juce::String ((int) std::lround (app.settings.drawTextSize)), juce::dontSendNotification);
        app.saveSettings ();
    };

    // Tooltips explain what each default applies to.
    const juce::String widthTip ("Line width in pixels for new arrows, rectangles and freehand strokes.");
    const juce::String sizeTip  ("Font size in points for new text items.");
    showAllBtn.setTooltip ("Show or hide all drawings of this file.");
    widthSlider.setTooltip (widthTip);
    widthLab.setTooltip (widthTip);
    colourBtn.setTooltip ("Ink colour used by new drawings.");
    textSizeSlider.setTooltip (sizeTip);
    sizeLab.setTooltip (sizeTip);

    addAndMakeVisible (&showAllBtn);
    addAndMakeVisible (&widthSlider);
    addAndMakeVisible (&widthLab);
    addAndMakeVisible (&colourBtn);
    addAndMakeVisible (&textSizeSlider);
    addAndMakeVisible (&sizeLab);
    addAndMakeVisible (&table);
    addAndMakeVisible (&deleteBtn);

    table.setMultipleSelectionEnabled (true);
    table.setSuppressShiftRangeSelection (true);   // we do the exact-range shift select ourselves

    // Right-clicking a row in the table shows the panel context menu. Deep
    // registration is required: clicks on rows are delivered to the row's own
    // component, not to the TableListBox.
    table.addMouseListener (this, true);

    refresh ();
}

void DrawingPanel::refresh()
{
    table.updateContent ();
    table.repaint ();

    auto* d = app.doc ();

    // The row selection belongs to the document whose list is shown: reset it
    // when switching files so stale rows never point at another file's entries.
    if (d != listDoc)
    {
        listDoc = d;
        anchorRow = -1;
        juce::SparseSet<int> none;
        table.setSelectedRows (none, juce::sendNotification);
    }

    showAllBtn.shown = app.showAllDrawings;
    showAllBtn.repaint ();
    deleteBtn.setEnabled (table.getNumSelectedRows () > 0);
}

void DrawingPanel::onRowClicked (int row, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu ())
        return;   // right-click: just show the context menu - keep any multi-selection

    table.setWantsKeyboardFocus (true);
    if (table.isShowing ())   // a focus grab on a hidden table is a no-op (and asserts)
        table.grabKeyboardFocus ();   // so clicks keep the selection live

    if (e.mods.isShiftDown ())
    {
        // Excel: the selection becomes EXACTLY anchor..row, then fit it.
        auto* d = app.doc ();
        const int n = d != nullptr ? (int) d->drawings.size () : 0;
        const int a = (anchorRow >= 0 && anchorRow < n) ? anchorRow : row;
        juce::SparseSet<int> sel;
        sel.addRange ({ std::min (a, row), std::max (a, row) + 1 });
        table.setSelectedRows (sel, juce::sendNotification);   // -> fitViewToSelected
    }
    else if (e.mods.isCommandDown () || e.mods.isCtrlDown ())
    {
        // JUCE already toggled this row in selectRowsBasedOnModifierKeys before
        // cellClicked fires - just track the anchor here.
        anchorRow = row;
    }
    else
    {
        // Plain click: single selection and move the anchor.
        anchorRow = row;
        juce::SparseSet<int> sel;
        sel.addRange ({ row, row + 1 });
        table.setSelectedRows (sel, juce::sendNotification);
    }
}

void DrawingPanel::deleteSelected ()
{
    auto* d = app.doc ();
    if (d == nullptr) return;

    std::vector<int> rows;
    for (int r : sparseRows (table.getSelectedRows ()))
        if (r >= 0 && r < (int) d->drawings.size ())
            rows.push_back (r);
    if (rows.empty()) return;

    app.deleteSelectedDrawings (rows);   // fires onDrawingsChanged -> refresh + repaint
}

void DrawingPanel::doDelete()
{
    deleteSelected ();
}

void DrawingPanel::fitViewToSelected ()
{
    auto* d = app.doc ();
    if (d == nullptr) return;

    double t0 = 1e300, f0 = 1e300, t1 = -1e300, f1 = -1e300;
    bool any = false;
    for (int r : sparseRows (table.getSelectedRows ()))
        if (r >= 0 && r < (int) d->drawings.size ())
        {
            const auto& it = d->drawings [(size_t) r];
            t0 = std::min (t0, it.t0); f0 = std::min (f0, it.f0);
            t1 = std::max (t1, it.t1); f1 = std::max (f1, it.f1);
            any = true;
        }
    if (any)
        app.ensureDrawingVisible (t0, f0, t1, f1);   // adds the 10% margin itself
}

int DrawingPanel::primaryRow() const
{
    return table.getNumSelectedRows () > 0 ? table.getSelectedRow (0) : -1;
}

std::vector<int> DrawingPanel::selectedRowIndices() const
{
    std::vector<int> out;
    for (int r : sparseRows (table.getSelectedRows ()))
        out.push_back (r);
    return out;
}

void DrawingPanel::toggleShowAll()
{
    // Fires onStateChange: repaints the views and refreshPanels() syncs this
    // panel's button state.
    app.setShowAllDrawings (!app.showAllDrawings);
}

void DrawingPanel::mouseDown (const juce::MouseEvent& e)
{
    if (!e.mods.isRightButtonDown () || e.mods.isLeftButtonDown ()) return;

    // The event position is relative to the component that was actually hit
    // (a row child of the table); walk up the parent chain to get it in
    // table coordinates.
    float px = e.getPosition ().x;
    float py = e.getPosition ().y;
    for (auto* c = e.eventComponent; c != nullptr && c != &table;
         c = c->getParentComponent ())
    {
        px += static_cast<float> (c->getX ());
        py += static_cast<float> (c->getY ());
    }
    const auto pInTable = juce::Point<float> (px, py);

    int row = -1;
    for (int r = 0; r < table.getNumRows (); ++r)
        if (table.getCellPosition (1, r, true).contains ((int) pInTable.x, (int) pInTable.y))
        {
            row = r;
            break;
        }

    if (row >= 0 && table.getNumSelectedRows () == 0)
    {
        anchorRow = row;
        juce::SparseSet<int> sel;
        sel.addRange ({ row, row + 1 });
        table.setSelectedRows (sel, juce::sendNotification); // -> fitViewToSelected
    }

    showContextMenu (row);
}

void DrawingPanel::showContextMenu (int)
{
    const int nSel = table.getNumSelectedRows ();

    juce::PopupMenu m;
    m.addItem (10, "New Text\tT");
    m.addItem (11, "New Arrow\tA");
    m.addItem (12, "New Rectangle\tR");
    m.addItem (13, "New Free Drawing\tD");
    m.addSeparator ();
    m.addItem (20, app.showAllDrawings ? "Hide All Drawings" : "Show All Drawings");
    m.addSeparator ();
    m.addItem (30, "Edit...", nSel > 0);
    m.addItem (31, "Delete Selected", nSel > 0);


    const int id = m.showMenu (juce::PopupMenu::Options ().withMousePosition ());
    if (id == 10)      { if (app.onArmNewDrawing) app.onArmNewDrawing (DrawingItem::Kind::Text); }
    else if (id == 11) { if (app.onArmNewDrawing) app.onArmNewDrawing (DrawingItem::Kind::Arrow); }
    else if (id == 12) { if (app.onArmNewDrawing) app.onArmNewDrawing (DrawingItem::Kind::Rectangle); }
    else if (id == 13) { if (app.onArmNewDrawing) app.onArmNewDrawing (DrawingItem::Kind::Freehand); }
    else if (id == 20) toggleShowAll ();
    else if (id == 30) showEditDialog (primaryRow ());
    else if (id == 31) deleteSelected ();
}

void DrawingPanel::showEditDialog (int row)
{
    auto* d = app.doc ();
    if (d == nullptr || row < 0 || row >= (int) d->drawings.size ()) return;
    const DrawingItem& it = d->drawings [(size_t) row];

    // Owning: the swatch and sliders below are added with new.
    sc::OwningAlertWindow dlg ("Edit drawing",
                          "Change this drawing's appearance and note.",
                          juce::AlertWindow::NoIcon);
    dlg.setColour (juce::AlertWindow::backgroundColourId, juce::Colour::fromRGB (45, 45, 48));

    juce::TextEditor* textEd = nullptr;
    if (it.kind == DrawingItem::Kind::Text)
        dlg.addTextEditor ("text", it.text, "Text:");
    dlg.addTextEditor ("comment", it.comment, "Comment:");
    textEd      = dlg.getTextEditor ("text");
    auto* commentEd = dlg.getTextEditor ("comment");

    // Live preview swatch + R/G/B sliders for the ink colour.
    class Swatch : public juce::Component
    {
    public:
        juce::Colour c;
        void paint (juce::Graphics& g) override
        {
            g.setColour (c);
            g.fillAll ();
        }
    };
    auto* sw = new Swatch ();
    sw->c = it.colour ();
    sw->setSize (200, 24);
    dlg.addCustomComponent (sw);

    std::vector<juce::Slider*> sliders;
    const char* names[3] = { "Red", "Green", "Blue" };
    int inits[3] = { it.colour ().getRed (), it.colour ().getGreen (), it.colour ().getBlue () };
    for (int i = 0; i < 3; ++i)
    {
        auto* s = new juce::Slider ();
        s->setRange (0.0, 255.0, 1.0);
        s->setValue ((double) inits[i]);
        s->setSize (240, 20);
        s->setName (names[i]);   // AlertWindow draws this as a label above the slider
        dlg.addCustomComponent (s);
        sliders.push_back (s);
    }

    auto* widthSl = new juce::Slider ();
    widthSl->setRange (0.5, 10.0, 0.5);
    widthSl->setValue ((double) it.widthPx);
    widthSl->setSize (240, 20);
    widthSl->setName ("Width");
    dlg.addCustomComponent (widthSl);

    auto* sizeSl = new juce::Slider ();
    sizeSl->setRange (6.0, 72.0, 1.0);
    sizeSl->setValue ((double) it.textSize);
    sizeSl->setSize (240, 20);
    sizeSl->setName ("Text size");
    dlg.addCustomComponent (sizeSl);

    auto updateSwatch = [&] ()
    {
        sw->c = juce::Colour::fromRGB ((juce::uint8) sliders[0]->getValue (),
                                       (juce::uint8) sliders[1]->getValue (),
                                       (juce::uint8) sliders[2]->getValue ());
        sw->repaint ();
    };
    for (auto* s : sliders)
        s->onValueChange = [&updateSwatch] () { updateSwatch (); };

    dlg.addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dlg.addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    if (dlg.runModalLoop () != 1) return;

    DrawingItem updated = it;   // kind + geometry stay where the user placed them
    if (textEd != nullptr)
        updated.text = textEd->getText ();
    updated.comment    = commentEd->getText ();
    updated.colourARGB = juce::Colour::fromRGB ((juce::uint8) sliders[0]->getValue (),
                                                (juce::uint8) sliders[1]->getValue (),
                                                (juce::uint8) sliders[2]->getValue ()).getARGB ();
    updated.widthPx  = std::clamp ((float) widthSl->getValue (), 0.5f, 10.0f);
    updated.textSize = std::clamp ((float) sizeSl->getValue (), 6.0f, 72.0f);

    applyDrawingEdit (row, updated);   // -> app.applyDrawingEdit -> refresh + repaint
}

void DrawingPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));
}

void DrawingPanel::resized()
{
    const auto r = getLocalBounds ();

    // Top row: master visibility switch + new-drawing defaults.
    showAllBtn.setBounds (6, 4, 26, 22);
    widthSlider.setBounds (40, 8, 110, 16);
    widthLab.setBounds (154, 8, 34, 16);
    colourBtn.setBounds (194, 4, 26, 22);
    textSizeSlider.setBounds (228, 8, 110, 16);
    sizeLab.setBounds (342, 8, 34, 16);

    table.setBounds (6, 34, r.getWidth () - 12, r.getHeight () - 34 - 30);
    deleteBtn.setBounds (6, r.getHeight () - 28, 110, 24);
}

// ============================================================================
// Tables panel: imported CSV tables with per-parameter alignment, overlays
// and a dedicated track.
// ============================================================================

static bool tablesParseDouble (const juce::String& s, double& out)
{
    const auto t = s.trim();
    if (t.isEmpty()) return false;
    // This JUCE build has no createFloatFromString(): validate the characters
    // first, then parse with getDoubleValue().
    bool haveDigit = false;
    for (char16_t c : t)
    {
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')
        {
            haveDigit = haveDigit || (c >= '0' && c <= '9');
            continue;
        }
        break;
    }
    if (!haveDigit) return false;
    const double v = t.getDoubleValue();
    if (!std::isfinite ((float) v)) return false;
    out = v;
    return true;
}

// ------------------------------------------------------------------ Clipboard --
// This JUCE build has no juce::Clipboard singleton: use the static
// SystemClipboard methods instead.
static void tablesSetClipboardText (const juce::String& text)
{
    juce::SystemClipboard::copyTextToClipboard (text);
}

static juce::String tablesGetClipboardText()
{
    return juce::SystemClipboard::getTextFromClipboard ();
}

/** AlertWindow numeric prompt: one labelled field, OK/Cancel. */
static bool tablesPromptNumber (juce::Component* parent, const juce::String& title,
                                const juce::String& message, const juce::String& initial,
                                double& out)
{
    auto* w = new juce::AlertWindow (title, message, juce::AlertWindow::QuestionIcon, parent);
    w->addTextEditor ("value", initial);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    if (auto* editor = w->getTextEditor ("value"))
    {
        editor->setWantsKeyboardFocus (true);
        editor->selectAll();
        editor->setEscapeAndReturnKeysConsumed (false);
    }
    w->enterModalState (false);
    if (auto* editor = w->getTextEditor ("value"))
        editor->grabKeyboardFocus();

    const int result = w->runModalLoop();
    const juce::String text = w->getTextEditorContents ("value");
    delete w;
    if (result != 1) return false;
    double v = 0.0;
    if (!tablesParseDouble (text, v))
    {
        juce::AlertWindow bad ("Invalid value",
                               "The entered text could not be read as a number.",
                               juce::AlertWindow::WarningIcon, parent);
        bad.addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        bad.enterModalState (false);
        bad.runModalLoop ();
        return false;
    }
    out = v;
    return true;
}

/** Prompts for a text value (rename dialogs). `name` is pre-filled and
    updated on success. Returns false when cancelled or the name is empty. */
static bool tablesPromptString (juce::Component* parent, const juce::String& title,
                                const juce::String& message, juce::String& name)
{
    auto* w = new juce::AlertWindow (title, message, juce::AlertWindow::QuestionIcon, parent);
    w->addTextEditor ("name", name);
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

    if (auto* editor = w->getTextEditor ("name"))
    {
        editor->setWantsKeyboardFocus (true);
        editor->selectAll();
        editor->setEscapeAndReturnKeysConsumed (false);
    }
    w->enterModalState (false);
    if (auto* editor = w->getTextEditor ("name"))
        editor->grabKeyboardFocus();

    const int result = w->runModalLoop();
    const juce::String text = w->getTextEditorContents ("name").trim ();
    delete w;
    if (result != 1) return false;
    if (text.isEmpty())
    {
        juce::AlertWindow bad ("Invalid name", "The name cannot be empty.",
                               juce::AlertWindow::WarningIcon, parent);
        bad.addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        bad.enterModalState (false);
        bad.runModalLoop ();
        return false;
    }
    name = text;
    return true;
}

/** CSV-quotes a field when needed (commas, quotes, newlines). */
static juce::String csvQuoteField (const juce::String& s)
{
    if (s.containsAnyOf (",\"\n\r"))
        return "\"" + s.replace ("\"", "\"\"") + "\"";
    return s;
}

// ------------------------------------------------------------------ Plot --

TablesPlotView::TablesPlotView (Application& appIn) : app (appIn) {}

void TablesPlotView::setFocusTableIndex (int idx)
{
    if (idx == focusIdx_) return;
    focusIdx_ = idx;
    resetView();   // another table: another duration and another value range
    repaint();
}

void TablesPlotView::setFocusParamIndex (int idx)
{
    if (idx == focusParam_) return;
    focusParam_ = idx;
    resetView();   // this parameter's alignment maps the playhead elsewhere
    repaint();   // the playhead may map elsewhere under that parameter's alignment
}

// ----------------------------------------------------------------- view state
std::pair<double, double> TablesPlotView::viewWindow() const
{
    const double dur = tableDuration();
    if (dur <= 0.0) return { 0.0, 0.0 };
    // No pan yet (or the table changed under an old window): the whole table.
    if (! (viewT1_ > viewT0_) || viewT0_ < 0.0 || viewT1_ > dur) return { 0.0, dur };
    return { viewT0_, viewT1_ };
}

bool TablesPlotView::autoValueRange (double& lo, double& hi) const
{
    // Auto-scale the value axis over all visible parameters of this table.
    // Empty cells (NaN) never influence the range. 10% padding, like the plot
    // has always drawn.
    double mn = 0.0, mx = 0.0;
    bool have = false;
    auto* d = app.doc ();
    if (d == nullptr || focusIdx_ < 0 || focusIdx_ >= (int) d->tables.size ()) return false;
    for (const auto& p : d->tables[(size_t) focusIdx_].params)
    {
        if (!p.visible || p.values.empty()) continue;
        double a = 0.0, b = 0.0;
        if (!sc::tables::finiteMinMax (p.values, a, b)) continue;
        if (!have) { mn = mx = a; have = true; }
        mn = std::min (mn, a);
        mx = std::max (mx, b);
    }
    if (!have || !(mx >= mn)) return false;
    const double pad = std::max ((mx - mn) * 0.1, 1e-9);
    lo = mn - pad;
    hi = mx + pad;
    return true;
}

std::pair<double, double> TablesPlotView::getViewTime() const { return viewWindow(); }

std::pair<double, double> TablesPlotView::getValueRange() const
{
    double lo, hi;
    if (! autoValueRange (lo, hi)) return { 0.0, 0.0 };
    if (haveVal_ && valHi_ > valLo_) return { valLo_, valHi_ };
    return { lo, hi };
}

void TablesPlotView::resetView()
{
    viewT0_ = viewT1_ = 0.0;
    valLo_ = valHi_ = 0.0;
    haveVal_ = false;
    selectDrag_ = false;
    panDrag_ = false;
}

double TablesPlotView::yToValue (float y) const
{
    double mn, mx;
    if (! autoValueRange (mn, mx)) return 0.0;
    if (haveVal_ && valHi_ > valLo_) { mn = valLo_; mx = valHi_; }
    const auto pr = plotRect();
    if (pr.getHeight() <= 1 || ! (mx > mn)) return mn;
    return mn + ((double) y - (double) pr.getY()) / (double) pr.getHeight() * (mx - mn);
}

void TablesPlotView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // Zoom the axis under the cursor: wheel = table time, Shift + wheel = the
    // value axis. The window never grows past the data, which is what makes
    // the middle-drag above able to move it at all.
    static constexpr double kZoomStep = 1.25;   // same direction as the other views: wheel down = zoom out

    const double dur = tableDuration();
    const auto pr = plotRect();
    if (dur <= 0.0 || pr.getWidth() <= 2 || pr.getHeight() <= 2) return;
    const double factor = wheel.deltaY > 0 ? kZoomStep : (1.0 / kZoomStep);

    if (e.mods.isShiftDown())
    {
        double lo, hi;
        if (! autoValueRange (lo, hi) || ! (hi > lo)) return;
        double vlo = lo, vhi = hi;
        if (haveVal_ && valHi_ > valLo_) { vlo = valLo_; vhi = valHi_; }
        const double c = yToValue ((float) e.y);
        double n0 = c - (c - vlo) * factor;
        double n1 = c + (vhi - c) * factor;
        if (n1 - n0 > hi - lo) { n0 = lo; n1 = hi; }
        if (n1 - n0 < (hi - lo) * 1e-6) return;
        if (n0 < lo) { n1 += (lo - n0); n0 = lo; }
        if (n1 > hi) { n0 -= (n1 - hi); n1 = hi; }
        valLo_ = n0; valHi_ = n1; haveVal_ = true;
        repaint();
        return;
    }

    auto [v0, v1] = viewWindow();
    const double c = xToTableTime (e.x);
    double n0 = c - (c - v0) * factor;
    double n1 = c + (v1 - c) * factor;
    if (n1 - n0 > dur) { n0 = 0.0; n1 = dur; }
    if (n1 - n0 < dur * 1e-6) return;
    if (n0 < 0.0) { n1 -= n0; n0 = 0.0; }
    if (n1 > dur) { n0 -= (n1 - dur); n1 = dur; }
    viewT0_ = n0; viewT1_ = n1;
    repaint();
}

void TablesPlotView::panViewByCursor (int x, int y)
{
    // Half-speed grab of both axes: the delta comes from the previous
    // delivered cursor position and that anchor always advances, so the clamps
    // below never leave a dead zone to unwind when the drag reverses - the
    // same scheme as the waveform/spectral/dedicated-track middle-drag.
    static constexpr double kPanSpeedFactor = 0.5;

    const double dur = tableDuration();
    const auto pr = plotRect();

    if (dur > 0.0 && pr.getWidth() > 1)
    {
        auto [v0, v1] = viewWindow();
        const double span = v1 - v0;
        const double d = kPanSpeedFactor * (double) (panLastX_ - x) * span / (double) (pr.getWidth() - 1);
        if (d != 0.0)
        {
            v0 += d; v1 += d;
            if (v0 < 0.0) { v1 -= v0; v0 = 0.0; }
            if (v1 > dur) { v0 -= (v1 - dur); v1 = dur; }
            viewT0_ = v0; viewT1_ = v1;
        }
    }
    panLastX_ = x;

    double lo, hi;
    if (autoValueRange (lo, hi) && hi > lo && pr.getHeight() > 1)
    {
        double vlo = lo, vhi = hi;
        if (haveVal_ && valHi_ > valLo_) { vlo = valLo_; vhi = valHi_; }
        const double d = kPanSpeedFactor * (double) (y - panLastY_) * (vhi - vlo)
                       / (double) pr.getHeight();
        if (d != 0.0)
        {
            vlo += d; vhi += d;
            if (vlo < lo) { vhi += (lo - vlo); vlo = lo; }
            if (vhi > hi) { vlo -= (vhi - hi); vhi = hi; }
            valLo_ = vlo; valHi_ = vhi; haveVal_ = true;
        }
    }
    panLastY_ = y;
    repaint();
}

juce::Rectangle<int> TablesPlotView::plotRect() const
{
    const int axisH = 14;   // time labels at the bottom
    return { 2, 2, std::max (1, getWidth() - 4), std::max (1, getHeight() - 4 - axisH) };
}

std::pair<double, double> TablesPlotView::focusTimeMap() const
{
    auto* d = app.doc ();
    if (d == nullptr || focusIdx_ < 0 || focusIdx_ >= (int) d->tables.size ()) return { 0.0, 1.0 };
    const auto& t = d->tables[(size_t) focusIdx_];
    if (focusParam_ >= 0 && focusParam_ < (int) t.params.size())
        return t.effectiveTimeMap (t.params[(size_t) focusParam_]);
    return { t.offsetSecs, t.scaleTime };
}

double TablesPlotView::docToTableTime (double docSecs) const
{
    const auto [off, scale] = focusTimeMap();
    return std::abs (scale) > 1e-12 ? (docSecs - off) / scale : docSecs - off;
}

double TablesPlotView::tableToDocTime (double tTableSecs) const
{
    const auto [off, scale] = focusTimeMap();
    return off + scale * tTableSecs;
}

int TablesPlotView::playheadX() const
{
    const double dur = tableDuration();
    if (playheadDoc_ < 0.0 || dur <= 0.0) return -1;
    const double t = docToTableTime (playheadDoc_);
    const auto [v0, v1] = viewWindow();
    if (!(t >= v0 && t <= v1)) return -1;   // outside the window being drawn
    return (int) std::floor (tableTimeToX (t));
}

void TablesPlotView::setPlayhead (double docSecs)
{
    playheadDoc_ = docSecs;
    if (playheadX() != shownPlayheadX_) repaint();   // only when the line moves
}

void TablesPlotView::setSelection (const juce::Range<double>& s)
{
    if (s == selectionDoc_) return;
    selectionDoc_ = s;
    repaint();
}

double TablesPlotView::tableDuration() const
{
    auto* d = app.doc ();
    if (d == nullptr || focusIdx_ < 0 || focusIdx_ >= (int) d->tables.size ()) return 0.0;
    return d->tables[(size_t) focusIdx_].duration();
}

// Column centre i of the curves area shows the table time at that column of
// the current view window (the whole table until something is panned).
double TablesPlotView::xToTableTime (int x) const
{
    const auto [v0, v1] = viewWindow();
    const double span = v1 - v0;
    const auto pr = plotRect();
    if (span <= 0.0 || pr.getWidth() <= 1) return v0;
    return v0 + std::clamp (((double) x - pr.getX()) / (double) (pr.getWidth() - 1), 0.0, 1.0) * span;
}

float TablesPlotView::tableTimeToX (double tTableSecs) const
{
    const auto [v0, v1] = viewWindow();
    const double span = v1 - v0;
    const auto pr = plotRect();
    if (span <= 0.0 || pr.getWidth() <= 1) return (float) pr.getX();
    return (float) (pr.getX() + 0.5 + (tTableSecs - v0) / span * (double) (pr.getWidth() - 1));
}

void TablesPlotView::paint (juce::Graphics& g)
{
    // Recorded even when the plot ends up empty, so setPlayhead() only
    // repaints when the line would actually move.
    shownPlayheadX_ = playheadX();
    const auto bounds = getLocalBounds();
    g.fillAll (juce::Colour::fromRGB (24, 24, 28));
    g.setColour (juce::Colour::fromRGB (70, 70, 80));
    g.drawRect (bounds.reduced (0.5f), 1);

    auto* d = app.doc ();
    if (d == nullptr || focusIdx_ < 0 || focusIdx_ >= (int) d->tables.size ())
    {
        g.setColour (juce::Colours::grey);
        g.setFont (12.0f);
        g.drawText ("Select a table or parameter to preview it here.", bounds,
                    juce::Justification::centred, true);
        return;
    }

    const auto& t = d->tables[(size_t) focusIdx_];
    const double dur = t.duration();
    if (dur <= 0.0 || getWidth() < 10) return;

    // Value axis: the auto-scaled range of every visible parameter (empty
    // cells never influence it), or the window a middle-drag moved - which is
    // always kept inside that range.
    double mn = 0.0, mx = 0.0;
    if (! autoValueRange (mn, mx)) return;
    if (haveVal_ && valHi_ > valLo_) { mn = valLo_; mx = valHi_; }

    // Horizontal: the table-time window a middle-drag moved, otherwise the
    // whole table.
    const auto [viewT0, viewT1] = viewWindow();
    const double viewSpan = viewT1 - viewT0;
    if (viewSpan <= 0.0) return;

    const int axisH = 14;   // time labels at the bottom
    const auto pr = plotRect();
    const int nLab = 4;

    // Grid like the Frequency Analysis graph: vertical lines at the time-label
    // positions and horizontal lines on a nice value step.
    g.setColour (juce::Colour::fromRGB (40, 40, 46));
    for (int i = 0; i <= nLab; ++i)
        g.drawVerticalLine ((float) (pr.getX() + pr.getWidth() * i / nLab),
                            (float) pr.getY(), (float) pr.getBottom());
    {
        const double vSpan = std::max (1e-9, mx - mn);
        const double raw = vSpan / 5.0;
        const double mag = std::pow (10.0, std::floor (std::log10 (raw)));
        double vStep = 10.0 * mag;
        for (double m : { 1.0, 2.0, 5.0 })
            if (m * mag >= raw) { vStep = m * mag; break; }
        for (double L = std::ceil (mn / vStep) * vStep; L <= mx + 1e-9; L += vStep)
        {
            const int y = pr.getY() + (int) std::lround ((mx - L) / (mx - mn) * pr.getHeight());
            g.drawHorizontalLine ((int) y, (float) pr.getX(), (float) pr.getRight());
        }
    }

    for (const auto& p : t.params)
    {
        if (!p.visible || p.values.size() < 2) continue;
        juce::Path path;
        bool started = false;
        for (int i = 0; i < pr.getWidth(); ++i)
        {
            const double tt = viewT0 + ((double) i / (pr.getWidth() - 1)) * viewSpan;
            const double v = sc::tables::paramValueAt (t, p, tt);
            // Empty cells break the curve: not drawn, never treated as zero.
            if (!std::isfinite (v)) { started = false; continue; }
            const float y = pr.getY() + (float) ((mx - v) / (mx - mn)) * (float) pr.getHeight();
            const float x = (float) (pr.getX() + i) + 0.5f;   // = tableTimeToX (tt)
            if (!started) { path.startNewSubPath (x, y); started = true; }
            else          path.lineTo (x, y);
        }
        g.setColour (sc::tables::tableColourAt (p.colorIndex, app.settings.tableCustomColours));
        g.strokePath (path, juce::PathStrokeType (1.6f));
    }

    // Time axis: a few labels in seconds, of the window currently shown.
    g.setFont (9.0f);
    g.setColour (juce::Colours::grey);
    for (int i = 0; i <= nLab; ++i)
        g.drawText (juce::String (viewT0 + viewSpan * ((double) i / nLab), 2),
                    juce::Rectangle<int> (pr.getX() + pr.getWidth() * i / nLab - 18,
                                          getHeight() - axisH + 1, 36, 12),
                    juce::Justification::centred, true);

    // The document playhead in this table's time: a red line like the one in
    // the tracks, following playback and every click that moves it.
    if (shownPlayheadX_ >= 0)
    {
        g.setColour (juce::Colours::red.withAlpha (0.95f));
        g.fillRect (shownPlayheadX_, pr.getY(), 2, pr.getHeight());
    }

    // The shared time selection in this table's own time (through the focused
    // parameter's alignment map): the same translucent blue band with light-
    // blue edges as the waveform/spectral views.
    if (!selectionDoc_.isEmpty())
    {
        const double dur = tableDuration();
        if (dur > 0.0)
        {
            const double s0 = docToTableTime (selectionDoc_.getStart());
            const double s1 = docToTableTime (selectionDoc_.getEnd());
            const float x1 = tableTimeToX (std::clamp (std::min (s0, s1), viewT0, viewT1));
            const float x2 = tableTimeToX (std::clamp (std::max (s0, s1), viewT0, viewT1));
            if (x2 > x1)
            {
                g.setColour (juce::Colour::fromRGBA (70, 130, 255, 80));
                const float lo = std::max ((float) pr.getX(), x1);
                const float hi = std::min ((float) pr.getRight(), x2);
                if (hi > lo)
                    g.fillRect (lo, (float) pr.getY(), hi - lo, (float) pr.getHeight());
                g.setColour (juce::Colour::fromRGB (96, 150, 255));
                if (x1 >= (float) pr.getX() && x1 <= (float) pr.getRight())
                    g.drawVerticalLine (x1, (float) pr.getY(), (float) pr.getBottom());
                if (x2 >= (float) pr.getX() && x2 <= (float) pr.getRight())
                    g.drawVerticalLine (x2, (float) pr.getY(), (float) pr.getBottom());
            }
        }
    }

    // Delicate white vertical cursor line, like the Frequency Analysis graph.
    if (hoverPos_.x >= pr.getX() && hoverPos_.x <= pr.getRight())
    {
        g.setColour (juce::Colour::fromRGBA (255, 255, 255, 90));
        g.drawVerticalLine ((float) hoverPos_.x, (float) pr.getY(), (float) pr.getBottom());
    }

    // Hover readout: the unscaled value of every visible parameter at the
    // hovered table time, stacked from the top like the dedicated track's
    // labels.
    if (!hoverLabels_.empty() && hoverPos_.x >= 0)
    {
        g.setFont (12.0f);
        const juce::Font f (12.0f);
        int y = pr.getY() + 4;
        for (const auto& [txt, col] : hoverLabels_)
        {
            const auto sz = (int) juce::GlyphArrangement::getStringWidth (f, txt);
            juce::Rectangle<int> box ((int) hoverPos_.x + 12, y, sz + 8, 15);
            g.setColour (juce::Colour::fromRGBA (20, 20, 24, 200));
            g.fillRoundedRectangle (box.toFloat(), 3.0f);
            g.setColour (col);
            g.drawText (txt, box, juce::Justification::centredLeft, true);
            y += 17;
        }
    }
}

void TablesPlotView::mouseDown (const juce::MouseEvent& e)
{
    pointGesture_ = false;
    selectDrag_ = false;
    panDrag_ = false;

    // Middle button: grab-and-drag pan of both axes (table time + value
    // range), half cursor speed, same scheme as the other views' middle-drag.
    if (e.mods.isMiddleButtonDown() && ! e.mods.isLeftButtonDown()
        && ! e.mods.isRightButtonDown())
    {
        if (tableDuration() <= 0.0) return;
        panDrag_ = true;
        panLastX_ = e.x;
        panLastY_ = e.y;
        return;
    }

    if (!e.mods.isLeftButtonDown() || e.mods.isRightButtonDown()) return;
    if (tableDuration() <= 0.0) return;

    pressX_ = e.x;
    pressY_ = e.y;
    pressTTab_ = xToTableTime (e.x);

    // The press is reported as before (it moves the playhead, or feeds a
    // click-based alignment); what a following drag does depends on whether an
    // alignment owns this plot (see mouseDrag).
    if (onPointClicked)
    {
        pointGesture_ = true;
        onPointClicked (pressTTab_);
    }
}

void TablesPlotView::mouseDrag (const juce::MouseEvent& e)
{
    if (panDrag_)
    {
        // Panning: the value readout is about the cursor, not the view, so it
        // is dropped while the grab is active.
        hoverPos_ = { -1, -1 };
        hoverLabels_.clear ();
        panViewByCursor (e.x, e.y);
        return;
    }

    hoverPos_ = { e.x, e.y };   // the value readout follows the drag too
    collectHoverLabels (e.x, e.y);
    repaint();

    // Only the drag of a press that was reported (same gesture, same rule).
    if (! pointGesture_ || tableDuration() <= 0.0) return;

    if (! alignmentPending_)
    {
        // No alignment waiting: a drag selects a time range, like the waveform
        // and the dedicated track. A plain click (or a drag that never left
        // the threshold) just leaves the playhead where the press put it.
        if (! selectDrag_ && std::abs (e.x - pressX_) < kSelectDragPx) return;
        selectDrag_ = true;

        const double a = pressTTab_, b = xToTableTime (e.x);
        const double d0 = tableToDocTime (std::min (a, b));
        const double d1 = tableToDocTime (std::max (a, b));
        if (onSelectTimeRange && std::abs (d1 - d0) > 1e-12)
            onSelectTimeRange ({ std::min (d0, d1), std::max (d0, d1) });
        return;
    }

    // A click-based alignment owns the gesture: dragging scrubs to the exact
    // point, as it always has.
    if (onPointDragged) onPointDragged (xToTableTime (e.x));
}

void TablesPlotView::mouseUp (const juce::MouseEvent&)
{
    pointGesture_ = false;
    selectDrag_ = false;
    panDrag_ = false;
}

void TablesPlotView::mouseDoubleClick (const juce::MouseEvent&)
{
    resetView();   // the whole table again, auto-scaled vertically
    repaint();
}

void TablesPlotView::mouseMove (const juce::MouseEvent& e)
{
    hoverPos_ = { e.x, e.y };
    collectHoverLabels (e.x, e.y);
    repaint();
}

void TablesPlotView::mouseExit (const juce::MouseEvent&)
{
    hoverPos_ = { -1, -1 };
    hoverLabels_.clear ();
    repaint();
}

void TablesPlotView::collectHoverLabels (int x, int y)
{
    hoverLabels_.clear ();
    auto* d = app.doc ();
    if (d == nullptr || focusIdx_ < 0 || focusIdx_ >= (int) d->tables.size ()) return;
    const auto& t = d->tables[(size_t) focusIdx_];
    if (t.duration() <= 0.0) return;

    const double tt = xToTableTime (x);
    for (const auto& p : t.params)
    {
        if (!p.visible || p.values.size() < 2) continue;
        const double v = sc::tables::paramValueAt (t, p, tt);   // unscaled value
        if (!std::isfinite (v)) continue;   // empty cell: no label
        hoverLabels_.push_back ({ p.name + "  " + juce::String (v, 4),
                                  sc::tables::tableColourAt (p.colorIndex,
                                                             app.settings.tableCustomColours) });
    }
}

// ----------------------------------------------------------------- Panel --

// ------------------------------------------------- parameter colour picker --

namespace
{
// Validates a 0..255 byte field: digits only (optional leading minus), then
// clamps. Returns -1 for anything else.
int tablesParseColourByte (const juce::String& s)
{
    if (s.isEmpty () || s.length () > 4) return -1;
    for (int i = 0; i < s.length (); ++i)
    {
        const auto ch = s[i];
        if (!(juce::CharacterFunctions::isDigit (ch) || (i == 0 && ch == L'-'))) return -1;
    }
    return std::clamp (s.getIntValue (), 0, 255);
}
} // namespace

TablesColourDialog::TablesColourDialog (const juce::String& title, int currentIdx,
                                        std::vector<juce::Colour> customs)
    : DocumentWindow (title, juce::Colour::fromRGB (45, 45, 48),
                      DocumentWindow::allButtons),
      colours_ (sc::tables::kPalette, sc::tables::kPalette + 12)
{
    for (const auto& c : customs) colours_.push_back (c);

    grid_.onSelect = [this] (int idx) { selectSwatch (idx); };

    newBtn_.setButtonText ("New");
    deleteBtn_.setButtonText ("Delete");
    resetBtn_.setButtonText ("Reset");
    newBtn_.onClick = [this] { doNew (); };
    deleteBtn_.onClick = [this] { doDelete (); };
    resetBtn_.onClick = [this] { doReset (); };

    rLbl_.setText ("R", juce::dontSendNotification);
    gLbl_.setText ("G", juce::dontSendNotification);
    bLbl_.setText ("B", juce::dontSendNotification);
    for (auto* l : { &rLbl_, &gLbl_, &bLbl_ })
    {
        l->setFont (12.0f);
        l->setJustificationType (juce::Justification::centredLeft);
        l->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    }

    for (auto* ed : { &rEd_, &gEd_, &bEd_ })
    {
        ed->setFont (12.0f);
        ed->setColour (juce::TextEditor::backgroundColourId, juce::Colour::fromRGB (38, 38, 42));
        ed->setColour (juce::TextEditor::textColourId, juce::Colours::lightgrey);
        // Let Enter/Escape reach the OK/cancel handling even while a field
        // has keyboard focus.
        ed->setEscapeAndReturnKeysConsumed (false);
        ed->onTextChange = [this] { rgbFieldChanged (); };
    }

    okBtn_.setButtonText ("OK");
    cancelBtn_.setButtonText ("Cancel");
    okBtn_.onClick = [this] { doOk (); };
    cancelBtn_.onClick = [this] { doCancel (); };

    juce::Component::addAndMakeVisible (&grid_);
    juce::Component::addAndMakeVisible (&newBtn_);
    juce::Component::addAndMakeVisible (&deleteBtn_);
    juce::Component::addAndMakeVisible (&resetBtn_);
    juce::Component::addAndMakeVisible (&rLbl_);
    juce::Component::addAndMakeVisible (&gLbl_);
    juce::Component::addAndMakeVisible (&bLbl_);
    juce::Component::addAndMakeVisible (&rEd_);
    juce::Component::addAndMakeVisible (&gEd_);
    juce::Component::addAndMakeVisible (&bEd_);
    juce::Component::addAndMakeVisible (&okBtn_);
    juce::Component::addAndMakeVisible (&cancelBtn_);

    selected_ = std::clamp (currentIdx, 0, (int) colours_.size () - 1);
    layout();
    selectSwatch (selected_);
}

TablesColourDialog::SwatchGrid::SwatchGrid (TablesColourDialog* ownerIn)
    : owner_ (ownerIn)
{
}

int TablesColourDialog::SwatchGrid::swatchAt (juce::Point<int> p) const
{
    for (int i = 0; i < (int) owner_->colours_.size (); ++i)
        if (juce::Rectangle<int> (8 + (i % kCols) * kPitchX,
                                  4 + (i / kCols) * kPitchY, kSwW, kSwH).contains (p))
            return i;
    return -1;
}

void TablesColourDialog::SwatchGrid::paint (juce::Graphics& g)
{
    const auto& cols = owner_->colours_;
    for (int i = 0; i < (int) cols.size (); ++i)
    {
        const auto r = juce::Rectangle<float> ((float) (8 + (i % kCols) * kPitchX),
                                               (float) (4 + (i / kCols) * kPitchY),
                                               (float) kSwW, (float) kSwH);
        g.setColour (cols[(size_t) i]);
        g.fillRoundedRectangle (r, 3.0f);
        if (i == owner_->selected_)
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (r.reduced (1.5f), 2.0f, 2);
        }
    }
}

void TablesColourDialog::SwatchGrid::mouseDown (const juce::MouseEvent& e)
{
    const int idx = swatchAt ({ e.x, e.y });
    if (idx >= 0 && onSelect) onSelect (idx);
}

juce::Colour TablesColourDialog::swatchColourAt (int i) const
{
    return (i >= 0 && i < (int) colours_.size ()) ? colours_[(size_t) i] : juce::Colours::black;
}

void TablesColourDialog::selectForTest (int idx) { selectSwatch (idx); }
void TablesColourDialog::newForTest ()           { doNew (); }
void TablesColourDialog::deleteForTest ()        { doDelete (); }
void TablesColourDialog::resetForTest ()         { doReset (); }
void TablesColourDialog::okForTest ()            { doOk (); }
void TablesColourDialog::cancelForTest ()        { doCancel (); }

void TablesColourDialog::setRgbForTest (int r, int g, int b)
{
    if (selected_ < 12 || selected_ >= (int) colours_.size ()) return;
    rEd_.setText (juce::String (std::clamp (r, 0, 255)), juce::dontSendNotification);
    gEd_.setText (juce::String (std::clamp (g, 0, 255)), juce::dontSendNotification);
    bEd_.setText (juce::String (std::clamp (b, 0, 255)), juce::dontSendNotification);
    rgbFieldChanged ();
}

void TablesColourDialog::layout()
{
    const int rows = ((int) colours_.size () + kCols - 1) / kCols;
    const int gridW = kCols * kPitchX;
    const int sideW = 74;   // right-hand New/Delete/Reset column
    setSize (12 + gridW + 10 + sideW + 12,
             12 + rows * kPitchY + 8 + 24 + 8 + 26 + 10);
}

void TablesColourDialog::selectSwatch (int idx)
{
    if (idx < 0 || idx >= (int) colours_.size ()) return;
    selected_ = idx;
    const auto c = colours_[(size_t) idx];
    rEd_.setText (juce::String ((int) c.getRed()), juce::dontSendNotification);
    gEd_.setText (juce::String ((int) c.getGreen()), juce::dontSendNotification);
    bEd_.setText (juce::String ((int) c.getBlue()), juce::dontSendNotification);
    // Only custom swatches are RGB-editable; the built-in palette is fixed.
    const bool editable = idx >= 12;
    rEd_.setEnabled (editable);
    gEd_.setEnabled (editable);
    bEd_.setEnabled (editable);
    deleteBtn_.setEnabled (idx >= 12 && colours_.size () > 12);
    resetBtn_.setEnabled (colours_.size () > 12);
    newBtn_.setEnabled ((int) colours_.size () < 12 + kMaxCustoms);
    grid_.repaint ();
}

void TablesColourDialog::rgbFieldChanged ()
{
    if (selected_ < 12 || selected_ >= (int) colours_.size ()) return;
    const int r = tablesParseColourByte (rEd_.getText ());
    const int g = tablesParseColourByte (gEd_.getText ());
    const int b = tablesParseColourByte (bEd_.getText ());
    if (r < 0 || g < 0 || b < 0) return;   // invalid text: leave the swatch alone
    colours_[(size_t) selected_] = juce::Colour::fromRGB ((juce::uint8) r, (juce::uint8) g,
                                                         (juce::uint8) b);
    grid_.repaint ();
}

void TablesColourDialog::doNew ()
{
    if ((int) colours_.size () >= 12 + kMaxCustoms) return;
    // Start the new swatch from the current selection so it can be tweaked.
    colours_.push_back (colours_[(size_t) selected_]);
    selectSwatch ((int) colours_.size () - 1);
    layout();   // may add a grid row: resize the window
}

void TablesColourDialog::doDelete ()
{
    if (selected_ < 12 || selected_ >= (int) colours_.size ()) return;
    colours_.erase (colours_.begin () + selected_);
    selectSwatch (std::min (selected_, (int) colours_.size () - 1));
    layout();
}

void TablesColourDialog::doReset ()
{
    if (colours_.size () <= 12) return;
    colours_.resize (12);
    selectSwatch (std::min (selected_, 11));
    layout();
}

void TablesColourDialog::doOk ()
{
    result_ = selected_;
    customsResult_.assign (colours_.begin () + 12, colours_.end ());
    exitModalState (1);
}

void TablesColourDialog::doCancel ()
{
    result_ = -1;
    customsResult_.clear ();
    exitModalState (-1);
}

bool TablesColourDialog::keyPressed (const juce::KeyPress& k)
{
    // Enter accepts, Escape cancels - even while a field has focus (the
    // editors do not consume those keys).
    if (k.isKeyCode (juce::KeyPress::returnKey))
    {
        doOk ();
        return true;
    }
    if (k.getKeyCode () == juce::KeyPress::escapeKey)
    {
        doCancel ();
        return true;
    }
    return DocumentWindow::keyPressed (k);
}

void TablesColourDialog::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));
}

void TablesColourDialog::resized()
{
    auto r = getLocalBounds ();
    const int rows = ((int) colours_.size () + kCols - 1) / kCols;
    grid_.setBounds (12, 12, kCols * kPitchX, rows * kPitchY);

    // Right-hand management column.
    const int sideX = r.getWidth () - 12 - 74;
    newBtn_.setBounds (sideX, 12, 74, 26);
    deleteBtn_.setBounds (sideX, 44, 74, 26);
    resetBtn_.setBounds (sideX, 76, 74, 26);

    // RGB row under the grid.
    const int rgbTop = 12 + rows * kPitchY + 8;
    rLbl_.setBounds (12, rgbTop, 14, 22);
    rEd_.setBounds (28, rgbTop, 52, 22);
    gLbl_.setBounds (90, rgbTop, 14, 22);
    gEd_.setBounds (106, rgbTop, 52, 22);
    bLbl_.setBounds (168, rgbTop, 14, 22);
    bEd_.setBounds (184, rgbTop, 52, 22);

    // Bottom buttons.
    const int btnsTop = r.getHeight () - 36;
    okBtn_.setBounds (r.getWidth () - 82, btnsTop, 70, 26);
    cancelBtn_.setBounds (r.getWidth () - 164, btnsTop, 70, 26);
}

void TablesPanel::showColourPicker (int tableIdx, int paramIdx)
{
    auto* d = app.doc ();
    if (d == nullptr || tableIdx < 0 || tableIdx >= (int) d->tables.size ()) return;
    const auto& t = d->tables[(size_t) tableIdx];
    if (paramIdx < 0 || paramIdx >= (int) t.params.size()) return;

    TablesColourDialog dlg ("Parameter Colour",
                            t.params[(size_t) paramIdx].colorIndex,
                            app.settings.tableCustomColours);
    if (dlg.runModalLoop () == 1 && dlg.resultIndex () >= 0)
    {
        d->tables[(size_t) tableIdx].params[(size_t) paramIdx].colorIndex = dlg.resultIndex ();
        d->metaDirty = true;   // the colour persists with the metadata
        if (app.onTablesChanged) app.onTablesChanged ();
        if (app.settings.tableCustomColours != dlg.resultCustoms ())
        {
            app.settings.tableCustomColours = dlg.resultCustoms ();
            app.saveSettings ();   // custom palette persists between sessions
        }
    }
}

void TablesPanel::timerCallback()
{
    flushPendingColourCycle ();
}

void TablesPanel::flushPendingColourCycle()
{
    if (pendingColourTable_ < 0) return;
    const int ti = pendingColourTable_, pi = pendingColourParam_;
    pendingColourTable_ = -1;
    auto* d = app.doc ();
    if (d == nullptr || ti < 0 || ti >= (int) d->tables.size ()) return;
    auto& t = d->tables[(size_t) ti];
    if (pi < 0 || pi >= (int) t.params.size()) return;
    const int total = 12 + (int) app.settings.tableCustomColours.size ();
    t.params[(size_t) pi].colorIndex = (t.params[(size_t) pi].colorIndex + 1) % total;
    d->metaDirty = true;   // the colour persists with the metadata
    if (app.onTablesChanged) app.onTablesChanged ();
}

TablesPanel::Model::Model (TablesPanel* o) : owner (o) {}

int TablesPanel::Model::getNumRows()
{
    return (int) owner->rows_.size();
}

void TablesPanel::Model::paintRowBackground (juce::Graphics& g, int, int width, int height,
                                             bool selected)
{
    if (selected)
    {
        g.setColour (juce::Colour::fromRGB (60, 80, 120));
        g.fillRect (0, 0, width, height);
    }
}

void TablesPanel::Model::paintCell (juce::Graphics& g, int rowNumber, int columnId,
                                   int width, int height, bool)
{
    if (rowNumber < 0 || rowNumber >= (int) owner->rows_.size ()) return;
    const auto ref = owner->rows_[(size_t) rowNumber];
    auto* d = owner->app.doc ();
    if (d == nullptr || ref.tableIdx < 0 || ref.tableIdx >= (int) d->tables.size ()) return;

    const auto& t = d->tables[(size_t) ref.tableIdx];
    const bool isTable = ref.paramIdx < 0;
    const bool visible = isTable ? t.visible
                                 : (ref.paramIdx < (int) t.params.size()
                                    && t.params[(size_t) ref.paramIdx].visible);

    g.setFont (11.0f);
    if (columnId == 1)   // visibility eye icon
        drawEyeIcon (g, juce::Rectangle<float> ((float) width / 2.0f - 8.0f,
                                                (float) height / 2.0f - 6.0f,
                                                16.0f, 12.0f), visible);
    else if (columnId == 2 && !isTable
             && ref.paramIdx < (int) t.params.size())   // colour square: parameters only
    {
        g.setColour (sc::tables::tableColourAt (t.params[(size_t) ref.paramIdx].colorIndex,
                                                owner->app.settings.tableCustomColours));
        g.fillRoundedRectangle (juce::Rectangle<float> ((float) width / 2.0f - 7.0f,
                                                        (float) height / 2.0f - 6.0f,
                                                        14.0f, 12.0f),
                                2.0f);
    }
    else   // name; parameters are indented under their table
    {
        const juce::String name = isTable ? t.name
                                          : (ref.paramIdx < (int) t.params.size()
                                             ? t.params[(size_t) ref.paramIdx].name : "");
        g.setColour (isTable ? juce::Colours::white : juce::Colours::lightgrey);
        g.drawText (name, isTable ? 4 : 20, 0, width - (isTable ? 8 : 24), height,
                    juce::Justification::centredLeft, false);
    }
}

void TablesPanel::Model::cellClicked (int rowNumber, int columnId, const juce::MouseEvent& e)
{
    if (rowNumber < 0 || rowNumber >= getNumRows ()) return;
    auto* d = owner->app.doc ();
    if (d == nullptr) return;

    // Left clicks on the eye icon / colour square change that property only.
    if (!e.mods.isRightButtonDown() && columnId <= 2)
    {
        const auto ref = owner->rows_[(size_t) rowNumber];
        if (ref.tableIdx >= 0 && ref.tableIdx < (int) d->tables.size())
        {
            auto& t = d->tables[(size_t) ref.tableIdx];
            bool handled = false;
            if (columnId == 1)
            {
                // Shift+click = "solo": only this eye's item stays visible - or
                // everything turns off when that state was already active.
                if (e.mods.isShiftDown())
                {
                    if (ref.paramIdx < 0) owner->soloTable (ref.tableIdx);
                    else owner->soloParameter (ref.tableIdx, ref.paramIdx);
                    return;   // the solo* methods handle dirty/refresh/callback
                }
                if (ref.paramIdx < 0) t.visible = !t.visible;
                else if (ref.paramIdx < (int) t.params.size())
                    t.params[(size_t) ref.paramIdx].visible = !t.params[(size_t) ref.paramIdx].visible;
                handled = true;
            }
            else if (columnId == 2 && ref.paramIdx >= 0
                     && ref.paramIdx < (int) t.params.size())
            {
                // Tables carry no colour: only parameter squares react here.
                // A single click cycles the palette - deferred until JUCE's
                // double-click window has passed; a double-click opens the
                // picker WITHOUT cycling (the pending cycle is cancelled).
                if (e.getNumberOfClicks () >= 2)
                {
                    // Cancel any pending single-click cycle: a double-click must
                    // not change the colour, it only opens the picker.
                    owner->stopTimer();
                    owner->pendingColourTable_ = -1;
                    owner->showColourPicker (ref.tableIdx, ref.paramIdx);
                }
                else
                {
                    owner->pendingColourTable_ = ref.tableIdx;
                    owner->pendingColourParam_ = ref.paramIdx;
                    // Apply the cycle once JUCE's 400 ms double-click window has
                    // passed; a second click inside that window stops the timer.
                    owner->startTimer (450);
                }
                return;   // no immediate dirty/refresh: the picker's OK or the
                          // deferred timer applies the change
            }
            if (!handled)
            {
                owner->onRowClicked (rowNumber, e);   // table-row colour cell selects the row
                return;
            }
            d->metaDirty = true;   // visibility/colour persist with the metadata
            if (owner->app.onTablesChanged) owner->app.onTablesChanged ();
            return;
        }
    }
    owner->onRowClicked (rowNumber, e);
}

// listDoc starts null so the first refresh() takes the doc-change branch and
// default-selects row 0: a freshly loaded file's tables must show in the
// preview without any click (the doc may already be active at construction).
TablesPanel::TablesPanel (Application& appIn)
    : app (appIn), listDoc (nullptr), plot (appIn)
{
    masterShowBtn.setComponentID ("tables-showall-btn");

    auto& header = table.getHeader ();
    const int fixedFlags = juce::TableHeaderComponent::notResizableOrSortable;
    header.addColumn ("Show", 1, 40, 30, -1, fixedFlags);
    header.addColumn ("Colour", 2, 56, 30, -1, fixedFlags);
    header.addColumn ("Name", 3, 120, 40, -1, fixedFlags);

    modeCombo.addItem ("Overlay on Waveform", 1);
    modeCombo.addItem ("Overlay on Spectrogram", 2);
    modeCombo.addItem ("Dedicated track", 3);
    modeCombo.setSelectedItemIndex (app.tablesMode(), juce::dontSendNotification);
    modeCombo.onChange = [this] { app.setTablesMode (modeCombo.getSelectedItemIndex ()); };

    masterShowBtn.onClick = [this] { app.setShowAllTables (!app.showAllTables); };
    masterShowBtn.onShiftClick = [this] (const juce::MouseEvent&) { toggleAllVisibility (); };

    // A click in the plot feeds a click-based alignment that still needs
    // plot points; otherwise it moves the playhead there. Decided once per
    // gesture: the drag that follows obeys the same rule (dragging scrubs).
    plot.onPointClicked = [this] (double tTab)
    {
        plotGestureSeeks_ = !(havePending_ && (int) pending_.tablePts.size() < pending_.numPoints);
        if (plotGestureSeeks_)
            seekToTableTime (tTab);
        else
            handlePlotClick (tTab);
    };
    plot.onPointDragged = [this] (double tTab)
    {
        if (plotGestureSeeks_) seekToTableTime (tTab);
    };
    // A drag on the preview plot (or its time axis) selects a time range in
    // the document; the plot draws the band like every other view.
    plot.onSelectTimeRange = [this] (const juce::Range<double>& r)
    {
        app.setSelection (r);   // clamped to the file; every view shows it
    };

    // The splitter trades height between the list and the plot; the chosen
    // plot height is remembered between sessions.
    if (app.recentStore != nullptr)
        plotHeight_ = app.recentStore->getIntValue ("tables_plot_height", -1);
    splitter.onDrag = [this] (int dy)
    {
        if (splitDragBase_ < 0) splitDragBase_ = effectivePlotHeight();
        // Dragging down shortens the plot; never <= 0 (that means "default").
        plotHeight_ = std::max (1, splitDragBase_ - dy);
        resized();
        plotHeight_ = effectivePlotHeight();   // store what the limits allowed
    };
    splitter.onDragEnd = [this]
    {
        splitDragBase_ = -1;
        if (app.recentStore != nullptr && plotHeight_ > 0)
        {
            app.recentStore->setValue ("tables_plot_height", plotHeight_);
            app.recentStore->save();
        }
    };

    addAndMakeVisible (&masterShowBtn);
    addAndMakeVisible (&modeCombo);
    addAndMakeVisible (&statusLabel);
    addAndMakeVisible (&table);
    addAndMakeVisible (&splitter);
    addAndMakeVisible (&plot);

    table.setMultipleSelectionEnabled (true);
    table.setSuppressShiftRangeSelection (true);   // we do the exact-range shift select ourselves

    // Right-clicking a row shows the panel context menu (deep registration:
    // clicks on rows are delivered to the row's own component).
    table.addMouseListener (this, true);

    refresh ();
}

TablesPanel::~TablesPanel()
{
    stopTimer();   // never let the colour-cycle callback outlive the panel
}

void TablesPanel::refresh()
{
    auto* d = app.doc ();

    rows_.clear();
    if (d != nullptr)
        for (int ti = 0; ti < (int) d->tables.size(); ++ti)
        {
            rows_.push_back ({ ti, -1 });
            for (int pi = 0; pi < (int) d->tables[(size_t) ti].params.size(); ++pi)
                rows_.push_back ({ ti, pi });
        }

    // Refresh the row count first: setSelectedRows() clamps against
    // totalItems, which is only valid after updateContent().
    table.updateContent ();

    // The row selection belongs to the document whose tables are shown.
    if (d != listDoc)
    {
        listDoc = d;
        anchorRow = -1;
        juce::SparseSet<int> none;
        table.setSelectedRows (none, juce::dontSendNotification);

        // Default to the first row so a freshly loaded file's tables show in
        // the preview without any click; an empty doc keeps no selection.
        if (!rows_.empty())
        {
            anchorRow = 0;
            juce::SparseSet<int> sel;
            sel.addRange ({ 0, 1 });
            table.setSelectedRows (sel, juce::sendNotification);
        }
    }

    masterShowBtn.shown = app.showAllTables;
    masterShowBtn.repaint ();
    modeCombo.setSelectedItemIndex (app.tablesMode(), juce::dontSendNotification);

    // The preview follows the primary selected row's table (its parameter's
    // own alignment, if it is a parameter row, maps the playhead).
    const int pr = primaryRow();
    plot.setFocusTableIndex (pr >= 0 ? rows_[(size_t) pr].tableIdx : -1);
    plot.setFocusParamIndex (pr >= 0 ? rows_[(size_t) pr].paramIdx : -1);
    plot.setSelection (app.selection);   // the blue band follows the shared selection

    updateStatus ();
    table.repaint ();
    plot.repaint ();   // colours / visibility may have changed under the preview
}

void TablesPanel::onRowClicked (int row, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu ()) return;   // right-click: keep any multi-selection

    table.setWantsKeyboardFocus (true);
    if (table.isShowing ())   // a focus grab on a hidden table is a no-op (and asserts)
        table.grabKeyboardFocus ();

    if (e.mods.isShiftDown ())
    {
        const int n = (int) rows_.size();
        const int a = (anchorRow >= 0 && anchorRow < n) ? anchorRow : row;
        juce::SparseSet<int> sel;
        sel.addRange ({ std::min (a, row), std::max (a, row) + 1 });
        table.setSelectedRows (sel, juce::sendNotification);
    }
    else if (e.mods.isCommandDown () || e.mods.isCtrlDown ())
    {
        anchorRow = row;   // JUCE already toggled this row before cellClicked fired
    }
    else
    {
        anchorRow = row;
        juce::SparseSet<int> sel;
        sel.addRange ({ row, row + 1 });
        table.setSelectedRows (sel, juce::sendNotification);
    }

    // Keep the preview on the newly primary row's table.
    const auto& focusRow = rows_[(size_t) std::clamp (row, 0, (int) rows_.size() - 1)];
    plot.setFocusTableIndex (focusRow.tableIdx);
    plot.setFocusParamIndex (focusRow.paramIdx);
}

TablesPanel::RowRef TablesPanel::rowRef (int row) const
{
    return (row >= 0 && row < (int) rows_.size()) ? rows_[(size_t) row] : RowRef {};
}

bool TablesPanel::isTableRow (int row) const
{
    const auto ref = rowRef (row);
    return ref.tableIdx >= 0 && ref.paramIdx < 0;
}

int TablesPanel::primaryRow() const
{
    int r = table.getNumSelectedRows () > 0 ? table.getSelectedRow (0) : -1;
    if (r < 0 || r >= (int) rows_.size()) return -1;   // stale selection guard
    return r;
}

std::vector<int> TablesPanel::selectedRowIndices() const
{
    std::vector<int> out;
    for (int r : sparseRows (table.getSelectedRows ()))
        if (r >= 0 && r < (int) rows_.size())
            out.push_back (r);
    return out;
}

std::vector<std::pair<int, int>> TablesPanel::selectedParamRows() const
{
    auto* d = app.doc ();
    if (d == nullptr) return {};

    std::vector<std::pair<int, int>> out;
    for (int r : sparseRows (table.getSelectedRows ()))
    {
        if (r < 0 || r >= (int) rows_.size()) continue;
        const auto ref = rows_[(size_t) r];
        if (ref.tableIdx < 0 || ref.tableIdx >= (int) d->tables.size ()) continue;

        std::vector<int> pis;
        if (ref.paramIdx >= 0)
            pis.push_back (ref.paramIdx);
        else   // a table row means all of its parameters
            for (int pi = 0; pi < (int) d->tables[(size_t) ref.tableIdx].params.size (); ++pi)
                pis.push_back (pi);

        for (int pi : pis)
        {
            if (pi < 0 || pi >= (int) d->tables[(size_t) ref.tableIdx].params.size ()) continue;
            const auto key = std::make_pair (ref.tableIdx, pi);
            if (std::find (out.begin (), out.end (), key) == out.end ())
                out.push_back (key);   // deduplicated, selection order kept
        }
    }
    return out;
}

void TablesPanel::removeSelected()
{
    auto* d = app.doc ();
    if (d == nullptr) return;

    // Remove selected parameters first, then whole tables.
    std::vector<std::pair<int, int>> params;   // (tableIdx, paramIdx)
    std::vector<int> tableRows;
    for (int r : sparseRows (table.getSelectedRows ()))
    {
        if (r < 0 || r >= (int) rows_.size()) continue;
        const auto ref = rows_[(size_t) r];
        if (ref.paramIdx >= 0) params.push_back ({ ref.tableIdx, ref.paramIdx });
        else                   tableRows.push_back (ref.tableIdx);
    }

    std::sort (params.begin(), params.end (),
               [] (const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [ti, pi] : params)
        if (ti >= 0 && ti < (int) d->tables.size() && pi >= 0 && pi < (int) d->tables[(size_t) ti].params.size())
            d->tables[(size_t) ti].params.erase (d->tables[(size_t) ti].params.begin () + pi);

    std::sort (tableRows.rbegin(), tableRows.rend());
    for (int ti : tableRows)
        if (ti >= 0 && ti < (int) d->tables.size())
            d->tables.erase (d->tables.begin () + ti);

    // Drop tables that lost every parameter.
    while (!d->tables.empty() && d->tables.back().params.empty())
        d->tables.pop_back ();

    if (params.empty() && tableRows.empty()) return;
    d->metaDirty = true;   // the table list persists with the file metadata
    juce::SparseSet<int> none;
    table.setSelectedRows (none, juce::sendNotification);
    refresh ();
    if (app.onTablesChanged) app.onTablesChanged ();
}

void TablesPanel::downsamplePrimaryTable()
{
    auto* d = app.doc ();
    const int pr = primaryRow();
    if (d == nullptr || pr < 0) return;
    const auto ref = rowRef (pr);
    if (ref.tableIdx < 0 || ref.tableIdx >= (int) d->tables.size()) return;

    auto& t = d->tables[(size_t) ref.tableIdx];
    double rate = t.sampleRate;
    if (!tablesPromptNumber (this, "Down sample",
                             "New sample rate in Hz for \"" + t.name + "\"\n"
                             "(current: " + juce::String (t.sampleRate, 1) + " Hz).\n"
                             "Rows are removed only - no interpolation or added data.",
                             "10", rate))
        return;
    if (!(rate > 0.0)) return;

    sc::tables::downsampleTable (t, rate);
    d->metaDirty = true;
    refresh ();
    if (app.onTablesChanged) app.onTablesChanged ();
}

void TablesPanel::copySelection()
{
    auto* d = app.doc ();
    const int pr = primaryRow();
    if (d == nullptr || pr < 0) return;

    // Tab-separated text with a leading document-time column ("time_s"): the
    // clipboard round-trips through paste, because importTable() recognises
    // the time header and re-positions the rows on the timeline.
    juce::String csv;
    if (isTableRow (pr))
    {
        // Whole table: header row of parameter names + one row per sample.
        const auto& t = d->tables[(size_t) rows_[(size_t) pr].tableIdx];
        csv += "time_s";
        for (size_t i = 0; i < t.params.size(); ++i)
            csv += "\t" + csvQuoteField (t.params[i].name);
        int nRows = 0;
        if (!t.params.empty())
            nRows = (int) t.params[0].values.size();
        for (int r = 0; r < nRows && !t.params.empty(); ++r)
        {
            csv += "\n" + juce::String (t.docTimeOfSample (t.params[0], r), 6);
            for (size_t i = 0; i < t.params.size(); ++i)
                if ((size_t) r < t.params[i].values.size())
                {
                    const double v = t.params[i].values[(size_t) r];
                    // Empty cells copy as blank fields, never as zero.
                    csv += "\t" + (std::isfinite (v) ? juce::String (v, 6)
                                                     : juce::String());
                }
        }
    }
    else
    {
        // Selected parameters: one column each, from their own tables. The
        // time column follows the first selected parameter's effective map.
        struct Col { const sc::tables::TableData* table; const sc::tables::ParamData* param; };
        std::vector<Col> cols;
        for (int r : selectedRowIndices())
        {
            const auto ref = rowRef (r);
            if (ref.paramIdx < 0) continue;
            if (ref.tableIdx >= 0 && ref.tableIdx < (int) d->tables.size()
                && ref.paramIdx < (int) d->tables[(size_t) ref.tableIdx].params.size())
                cols.push_back ({ &d->tables[(size_t) ref.tableIdx],
                                  &d->tables[(size_t) ref.tableIdx].params[(size_t) ref.paramIdx] });
        }
        if (cols.empty()) return;

        csv += "time_s";
        for (const auto& c : cols)
            csv += "\t" + csvQuoteField (c.param->name);
        int nRows = INT_MAX;
        for (const auto& c : cols) nRows = std::min (nRows, (int) c.param->values.size());
        for (int r = 0; r < nRows; ++r)
        {
            csv += "\n" + juce::String (cols[0].table->docTimeOfSample (*cols[0].param, r), 6);
            for (const auto& c : cols)
            {
                const double v = c.param->values[(size_t) r];
                // Empty cells copy as blank fields, never as zero.
                csv += "\t" + (std::isfinite (v) ? juce::String (v, 6)
                                                 : juce::String());
            }
        }
    }

    tablesSetClipboardText (csv);
}

void TablesPanel::pasteToNewTable()
{
    const juce::String text = tablesGetClipboardText ();
    std::string err;
    if (!app.addTableFromCsv ("Clipboard", text, &err))
    {
        statusLabel.setText ("Import failed: " + juce::String (err), juce::dontSendNotification);
        return;
    }
    // The new table is the last one: select it in the list and point the
    // preview at it so its plot shows without a click. addTableFromCsv has
    // already notified the panel in the app; make sure the rows are current
    // first so this also works when nothing else is listening.
    auto* d = app.doc ();
    if (d == nullptr || d->tables.empty()) return;
    const int newIdx = (int) d->tables.size() - 1;
    bool listed = false;
    for (const auto& r : rows_)
        if (r.tableIdx == newIdx && r.paramIdx < 0) listed = true;
    if (!listed) refresh();
    selectTable (newIdx);
}

void TablesPanel::selectTable (int tableIdx)
{
    for (int i = 0; i < (int) rows_.size(); ++i)
        if (rows_[(size_t) i].tableIdx == tableIdx && rows_[(size_t) i].paramIdx < 0)
        {
            anchorRow = i;
            juce::SparseSet<int> sel;
            sel.addRange ({ i, i + 1 });
            table.setSelectedRows (sel, juce::sendNotification);
            plot.setFocusTableIndex (tableIdx);
            plot.setFocusParamIndex (-1);
            return;
        }
}

// ================================================== Manual alignment window

ManualAlignWindow::ManualAlignWindow (const juce::String& title, const juce::String& message)
    : DocumentWindow (title, juce::Colour::fromRGB (45, 45, 48),
                      DocumentWindow::allButtons)
{
    msgLabel.setFont (12.0f);
    msgLabel.setJustificationType (juce::Justification::centredLeft);
    msgLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    msgLabel.setText (message, juce::dontSendNotification);

    okBtn.setButtonText ("OK");
    cancelBtn.setButtonText ("Cancel");
    okBtn.onClick = [this] { if (onOk) onOk (); exitModalState (1); };
    cancelBtn.onClick = [this] { exitModalState (-1); };

    juce::Component::addAndMakeVisible (&msgLabel);
    juce::Component::addAndMakeVisible (&okBtn);
    juce::Component::addAndMakeVisible (&cancelBtn);
}

juce::TextEditor* ManualAlignWindow::addRow (const juce::String& label,
                                             const juce::String& tooltip,
                                             const juce::String& initial)
{
    auto* lbl = new juce::Label ();
    lbl->setFont (12.0f);
    lbl->setJustificationType (juce::Justification::centredLeft);
    lbl->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    lbl->setText (label, juce::dontSendNotification);

    auto* ed = new juce::TextEditor ();
    ed->setFont (12.0f);
    ed->setColour (juce::TextEditor::backgroundColourId, juce::Colour::fromRGB (38, 38, 42));
    ed->setColour (juce::TextEditor::textColourId, juce::Colours::lightgrey);
    ed->setText (initial, juce::dontSendNotification);
    // Let Enter/Escape reach the OK/Cancel shortcuts even while a field has
    // keyboard focus.
    ed->setEscapeAndReturnKeysConsumed (false);

    if (!tooltip.isEmpty ())
    {
        lbl->setTooltip (tooltip);
        ed->setTooltip (tooltip);
    }

    juce::Component::addAndMakeVisible (lbl);
    juce::Component::addAndMakeVisible (ed);
    rows_.push_back ({ lbl, ed });
    return ed;
}

juce::String ManualAlignWindow::rowText (int i) const
{
    if (i < 0 || i >= (int) rows_.size () || rows_[(size_t) i].editor == nullptr)
        return juce::String ();
    return rows_[(size_t) i].editor->getText ();
}

void ManualAlignWindow::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));
}

void ManualAlignWindow::resized()
{
    auto r = getLocalBounds ();

    // Bottom button row (anchored to the bottom of the window, not y=0).
    const int btnH = 26;
    auto bot = r.removeFromBottom (btnH + 10);
    okBtn.setBounds (bot.getRight () - 74, bot.getY (), 68, btnH);
    cancelBtn.setBounds (okBtn.getX () - 8 - 78, bot.getY (), 78, btnH);

    // Message area at the top.
    auto msgArea = r.removeFromTop (26);
    msgLabel.setBounds (msgArea.withTrimmedLeft (10).withTrimmedRight (10));

    // Labeled field rows: label on the left, editor on the right.
    const int rowPitch = 27;
    const int lblW = 158;
    auto area = r.withTrimmedLeft (10).withTrimmedRight (10);
    for (size_t i = 0; i < rows_.size (); ++i)
    {
        auto rowRect = juce::Rectangle<int> (area.getX(),
                                             area.getY() + (int) i * rowPitch,
                                             area.getWidth(), 22);
        if (rows_[(size_t) i].label != nullptr)
            rows_[(size_t) i].label->setBounds (rowRect.removeFromLeft (lblW));
        if (rows_[(size_t) i].editor != nullptr)
            rows_[(size_t) i].editor->setBounds (rowRect);
    }
}

bool ManualAlignWindow::keyPressed (const juce::KeyPress& k)
{
    // Enter accepts, Escape cancels - even while a field has focus (the
    // editors do not consume those keys).
    if (k.isKeyCode (juce::KeyPress::returnKey))
    {
        if (onOk) onOk ();
        exitModalState (1);
        return true;
    }
    if (k.getKeyCode () == juce::KeyPress::escapeKey)
    {
        exitModalState (-1);
        return true;
    }
    return DocumentWindow::keyPressed (k);
}

void ManualAlignWindow::userTriedToCloseWindow()
{
    exitModalState (-1);
}

void TablesPanel::showManualAlignDialog (int scopeTable, int scopeParam, int targetView)
{
    auto* d = app.doc ();
    if (d == nullptr || scopeTable < 0 || scopeTable >= (int) d->tables.size ()) return;
    const bool paramLevel = scopeParam >= 0;

    auto& t = d->tables[(size_t) scopeTable];
    sc::tables::ParamData* p = paramLevel && scopeParam < (int) t.params.size()
                               ? &t.params[(size_t) scopeParam] : nullptr;

    // Time alignment is shared at the table level, so a parameter dialog shows
    // (and edits) its effective map - the one actually used for drawing.
    const double showOff = p != nullptr ? t.offsetSecs + p->offsetSecs : t.offsetSecs;
    const double showSc  = p != nullptr ? t.scaleTime * p->scaleTime   : t.scaleTime;

    auto* w = new ManualAlignWindow (
        juce::String ("Align ") + (paramLevel ? "Parameter" : "Table"),
        "Manual Alignment:");

    w->addRow ("Time offset (s)",
               "Shift along the timeline: doc time = offset + scale x table time (+ = later). Shared by all three display modes.",
               juce::String (showOff, 4));
    w->addRow ("Time scale",
               "Stretch factor of table time relative to doc time (must be > 0). Shared by all three display modes.",
               juce::String (showSc, 4));

    if (paramLevel && p != nullptr)
    {
        w->addRow ("Normalized (0/1)",
                   "Map the parameter into its own 0..1 range before scaling.",
                   p->normalized ? "1" : "0");
        w->addRow ("Inverted (0/1)",
                   "Flip the values around zero.",
                   p->inverted ? "1" : "0");
        w->addRow ("Absolute (0/1)",
                   "Use the absolute value of every sample.",
                   p->absolute ? "1" : "0");

        struct ModeInfo { const char* name; int target; };
        static const ModeInfo modes[3] = {
            { "Waveform", 0 }, { "Spectrogram", 1 }, { "Dedicated track", 2 }
        };
        for (const auto& mi : modes)
        {
            w->addRow (juce::String (mi.name) + ": value offset",
                       juce::String (mi.name) + ": added to every fitted value after the transforms.",
                       juce::String (p->valueOffset[mi.target], 4));
            w->addRow (juce::String (mi.name) + ": value scale",
                       juce::String (mi.name) + ": multiplied with every fitted value after the transforms.",
                       juce::String (p->valueScale[mi.target], 4));
        }
    }

    // Size the window from its rows: message area + row pitch per field + buttons.
    const int contentH = 26 + w->numRows () * 27 + 10 + 26;
    w->setSize (356, contentH);
    w->setResizable (false, false);

    // Centre on the main window's current position instead of appearing in
    // the screen corner.
    if (juce::Component* top = getTopLevelComponent ())
    {
        const auto tb = top->getBounds ();
        w->setBounds (tb.getCentreX () - 356 / 2, tb.getCentreY () - contentH / 2,
                      356, contentH);
    }
    else
        w->centreWithSize (356, contentH);

    bool okPressed = false;
    w->onOk = [&okPressed] { okPressed = true; };
    w->enterModalState (false);
    // Child order: message, OK, Cancel, then label/editor pairs per row -
    // focus the first editor so a plain Enter accepts the defaults.
    if (w->getNumChildComponents () > 4)
    {
        auto* first = dynamic_cast<juce::TextEditor*> (w->getChildComponent (4));
        if (first != nullptr) first->grabKeyboardFocus();
    }
    const int result = w->runModalLoop ();

    if (result == 1)
    {
        double off = 0.0, sc = 1.0;
        tablesParseDouble (w->rowText (0), off);
        tablesParseDouble (w->rowText (1), sc);
        if (!(sc > 0.0)) sc = 1.0;

        // Time alignment is shared at the table level: applying it to any row
        // re-aligns every parameter of that table together.
        t.offsetSecs = off;
        t.scaleTime = sc;
        for (auto& q : t.params)
        {
            q.offsetSecs = 0.0;
            q.scaleTime = 1.0;
        }

        if (paramLevel && p != nullptr)
        {
            // The chosen Align target decides which overlay view shows this
            // parameter (waveform / spectrogram) or only the dedicated track.
            p->alignTarget = std::clamp (targetView, 0, 2);
            double v = 0.0;
            tablesParseDouble (w->rowText (2), v); p->normalized = v != 0.0;
            tablesParseDouble (w->rowText (3), v); p->inverted   = v != 0.0;
            tablesParseDouble (w->rowText (4), v); p->absolute   = v != 0.0;

            // Per-display-mode vertical maps: rows 5..10 are the offset/scale
            // pairs for Waveform, Spectrogram and Dedicated track in order.
            for (int i = 0; i < 3; ++i)
            {
                double vo = 0.0;
                tablesParseDouble (w->rowText (5 + i * 2), vo);
                p->valueOffset[i] = vo;
                double vs = 1.0;
                if (tablesParseDouble (w->rowText (6 + i * 2), vs))
                    p->valueScale[i] = vs;
            }
        }
    }
    delete w;

    d->metaDirty = true;   // alignment settings persist with the file metadata
    refresh ();
    if (app.onTablesChanged) app.onTablesChanged ();
}

void TablesPanel::startPointAlign (int numPoints, int scopeTable, int scopeParam, int targetView)
{
    auto* d = app.doc ();
    if (d == nullptr || scopeTable < 0 || scopeTable >= (int) d->tables.size ()) return;
    if (numPoints != 1 && numPoints != 2) return;

    pending_ = PendingAlign {};
    pending_.numPoints = numPoints;
    pending_.scopeTable = scopeTable;
    pending_.scopeParam = scopeParam;
    pending_.targetView = targetView == 1 ? 1 : 0;
    havePending_ = true;

    plot.setFocusTableIndex (scopeTable);
    plot.setFocusParamIndex (scopeParam);
    plot.setAlignmentPending (true);   // the plot's clicks belong to this gesture
    updateStatus ();
}

void TablesPanel::updateStatus()
{
    if (!havePending_)
    {
        statusLabel.setText ("", juce::dontSendNotification);
        return;
    }
    const char* viewName = pending_.targetView == 0 ? "waveform" : "spectrogram";
    if ((int) pending_.tablePts.size() < pending_.numPoints)
        statusLabel.setText ("Click " + juce::String (pending_.numPoints)
                             + " point(s) on the table plot below...",
                             juce::dontSendNotification);
    else
        statusLabel.setText ("Now click " + juce::String ((size_t) pending_.numPoints - pending_.docPts.size())
                             + " in the " + viewName + "...",
                             juce::dontSendNotification);
}

void TablesPanel::handlePlotClick (double tTableSecs)
{
    if (!havePending_) return;
    pending_.tablePts.push_back (tTableSecs);
    if ((int) pending_.tablePts.size() < pending_.numPoints)
    {
        updateStatus ();
        return;
    }

    // All plot points collected: arm the target view for the doc-side clicks.
    if (app.onArmTableAlignClick) app.onArmTableAlignClick (pending_.targetView);
    updateStatus ();
}

void TablesPanel::handleDocClick (double tDocSecs, double docValue)
{
    if (!havePending_ || pending_.tablePts.size() < (size_t) pending_.numPoints) return;
    pending_.docPts.push_back (tDocSecs);
    pending_.docVals.push_back (docValue);
    if ((int) pending_.docPts.size() < pending_.numPoints)
    {
        updateStatus ();
        return;
    }

    // Complete: compute the mapping and apply it to the scope.
    auto* d = app.doc ();
    const int ti = pending_.scopeTable, pi = pending_.scopeParam;
    if (d != nullptr && ti >= 0 && ti < (int) d->tables.size())
    {
        auto& t = d->tables[(size_t) ti];
        double off = 0.0, sc = 1.0;
        const double a1 = pending_.tablePts[0], b1 = pending_.docPts[0];
        if (pending_.numPoints == 2)
        {
            const double a2 = pending_.tablePts[1], b2 = pending_.docPts[1];
            sc = std::abs (a2 - a1) > 1e-9 ? (b2 - b1) / (a2 - a1) : 1.0;
            if (!(sc > 0.0)) sc = 1.0;
        }
        off = b1 - sc * a1;

        // Time alignment is shared at the table level: aligning any parameter
        // (or the table itself) re-aligns every parameter of that table.
        t.offsetSecs = off;
        t.scaleTime = sc;
        for (auto& q : t.params)
        {
            q.offsetSecs = 0.0;
            q.scaleTime = 1.0;
        }

        // Vertical part (two points only): solve the target mode's value
        // scale/offset so the fitted values at the two table points land on
        // the clicked doc-side positions - instead of staying full-scale.
        if (pending_.numPoints == 2 && pi >= 0 && pi < (int) t.params.size())
        {
            auto& pp = t.params[(size_t) pi];
            double mn = 0.0, mx = 0.0;
            // Empty cells (NaN) must not distort the fitted range.
            if (sc::tables::finiteMinMax (pp.values, mn, mx))
            {
                const double f1 = sc::tables::fittedOverlayValue (
                    sc::tables::paramValueAt (t, pp, pending_.tablePts[0]), pp, mn, mx);
                const double f2 = sc::tables::fittedOverlayValue (
                    sc::tables::paramValueAt (t, pp, pending_.tablePts[1]), pp, mn, mx);
                if (std::isfinite (f1) && std::isfinite (f2))
                {
                    const int tgt = std::clamp (pending_.targetView, 0, 1);
                    if (!sc::tables::solveValueMap (f1, f2,
                                                    pending_.docVals[0], pending_.docVals[1],
                                                    pp.valueScale[tgt], pp.valueOffset[tgt]))
                        // Degenerate: keep the current scale and land point one exactly.
                        pp.valueOffset[tgt] = pending_.docVals[0] - pp.valueScale[tgt] * f1;
                }
            }
        }

        if (pi >= 0 && pi < (int) t.params.size())
            t.params[(size_t) pi].alignTarget = pending_.targetView;
    }

    havePending_ = false;
    pending_ = PendingAlign {};
    plot.setAlignmentPending (false);   // the plot selects again
    if (app.onArmTableAlignClick) app.onArmTableAlignClick (-1);   // disarm the views
    if (d != nullptr) d->metaDirty = true;   // alignment persists with the metadata
    refresh ();
    if (app.onTablesChanged) app.onTablesChanged ();
}

void TablesPanel::cancelPendingAlignment()
{
    havePending_ = false;
    pending_ = PendingAlign {};
    plot.setAlignmentPending (false);
    if (app.onArmTableAlignClick) app.onArmTableAlignClick (-1);   // disarm the views
    updateStatus ();
}

void TablesPanel::mouseDown (const juce::MouseEvent& e)
{
    if (!e.mods.isRightButtonDown () || e.mods.isLeftButtonDown ()) return;

    // The event position is relative to the component that was actually hit
    // (a row child of the table); walk up the parent chain to get it in
    // table coordinates.
    float px = e.getPosition ().x;
    float py = e.getPosition ().y;
    for (auto* c = e.eventComponent; c != nullptr && c != &table;
         c = c->getParentComponent ())
    {
        px += static_cast<float> (c->getX ());
        py += static_cast<float> (c->getY ());
    }
    const auto pInTable = juce::Point<float> (px, py);

    int row = -1;
    for (int r = 0; r < table.getNumRows (); ++r)
        if (table.getCellPosition (1, r, true).contains ((int) pInTable.x, (int) pInTable.y))
        {
            row = r;
            break;
        }

    if (row >= 0 && table.getNumSelectedRows () == 0)
    {
        anchorRow = row;
        juce::SparseSet<int> sel;
        sel.addRange ({ row, row + 1 });
        table.setSelectedRows (sel, juce::sendNotification);
    }

    showContextMenu (row);
}

// ---- Shift+click visibility ("solo") operations --------------------------

/** Number of parameters currently visible on screen (their table's eye and
    their own eye are both on), optionally skipping one parameter. */
static int countVisibleParams (const FileDoc& d, int skipTable = -1, int skipParam = -1)
{
    int n = 0;
    for (int i = 0; i < (int) d.tables.size(); ++i)
    {
        if (!d.tables[(size_t) i].visible) continue;
        for (int j = 0; j < (int) d.tables[(size_t) i].params.size(); ++j)
        {
            // skipParam < 0 skips the whole table, otherwise just that parameter
            if (i == skipTable && (skipParam < 0 || j == skipParam)) continue;
            if (d.tables[(size_t) i].params[(size_t) j].visible) ++n;
        }
    }
    return n;
}

static bool everythingVisible (const Application& app, const FileDoc& d)
{
    if (!app.showAllTables) return false;
    for (const auto& t : d.tables)
    {
        if (!t.visible) return false;
        for (const auto& p : t.params)
            if (!p.visible) return false;
    }
    return true;
}

void TablesPanel::hideEverything ()
{
    auto* d = app.doc ();
    if (d == nullptr) return;
    app.setShowAllTables (false);
    for (auto& t : d->tables)
    {
        t.visible = false;
        for (auto& p : t.params) p.visible = false;
    }
    d->metaDirty = true;
    refresh ();
    if (app.onTablesChanged) app.onTablesChanged ();
}

void TablesPanel::soloParameter (int ti, int pi)
{
    auto* d = app.doc ();
    if (d == nullptr || ti < 0 || ti >= (int) d->tables.size()) return;
    auto& t = d->tables[(size_t) ti];
    if (pi < 0 || pi >= (int) t.params.size()) return;
    auto& p = t.params[(size_t) pi];

    // Already the only visible parameter? Then turn everything off.
    if (app.showAllTables && t.visible && p.visible
        && countVisibleParams (*d, ti, pi) == 0)
    {
        hideEverything ();
        return;
    }

    app.setShowAllTables (true);
    for (auto& t2 : d->tables)
    {
        const bool isTarget = (&t2 == &t);
        t2.visible = isTarget;
        if (!isTarget) continue;   // keep the other tables' inner states intact
        for (auto& q : t2.params)
            q.visible = (&q == &p);
    }
    d->metaDirty = true;
    refresh ();
    if (app.onTablesChanged) app.onTablesChanged ();
}

void TablesPanel::soloTable (int ti)
{
    auto* d = app.doc ();
    if (d == nullptr || ti < 0 || ti >= (int) d->tables.size()) return;
    auto& t = d->tables[(size_t) ti];

    // Already "this table with all its parameters, and nothing else"? Off.
    bool thisTableFullyOn = app.showAllTables && t.visible;
    for (const auto& q : t.params)
        if (!q.visible) { thisTableFullyOn = false; break; }
    if (thisTableFullyOn && countVisibleParams (*d, ti, -1) == 0)
    {
        hideEverything ();
        return;
    }

    app.setShowAllTables (true);
    for (auto& t2 : d->tables)
        t2.visible = (&t2 == &t);
    for (auto& q : t.params)
        q.visible = true;
    d->metaDirty = true;
    refresh ();
    if (app.onTablesChanged) app.onTablesChanged ();
}

void TablesPanel::toggleAllVisibility ()
{
    auto* d = app.doc ();
    if (d == nullptr) return;

    if (everythingVisible (app, *d)) { hideEverything (); return; }

    app.setShowAllTables (true);
    for (auto& t : d->tables)
    {
        t.visible = true;
        for (auto& q : t.params) q.visible = true;
    }
    d->metaDirty = true;
    refresh ();
    if (app.onTablesChanged) app.onTablesChanged ();
}

void TablesPanel::renamePrimaryTable ()
{
    auto* d = app.doc ();
    const int pr = primaryRow();
    if (d == nullptr || pr < 0) return;
    const auto ref = rowRef (pr);
    if (ref.tableIdx < 0 || ref.tableIdx >= (int) d->tables.size()) return;

    auto& t = d->tables[(size_t) ref.tableIdx];
    juce::String name = t.name;
    if (!tablesPromptString (this, "Rename Table",
                             "New name for the table:", name))
        return;
    t.name = name;
    d->metaDirty = true;
    refresh ();
    if (app.onTablesChanged) app.onTablesChanged ();
}

void TablesPanel::renamePrimaryParameter ()
{
    auto* d = app.doc ();
    const int pr = primaryRow();
    if (d == nullptr || pr < 0) return;
    const auto ref = rowRef (pr);
    if (ref.tableIdx < 0 || ref.paramIdx < 0
        || ref.tableIdx >= (int) d->tables.size()) return;
    auto& t = d->tables[(size_t) ref.tableIdx];
    if (ref.paramIdx >= (int) t.params.size()) return;

    auto& p = t.params[(size_t) ref.paramIdx];
    juce::String name = p.name;
    if (!tablesPromptString (this, "Rename Parameter",
                             "New name for the parameter:", name))
        return;
    p.name = name;
    d->metaDirty = true;
    refresh ();
    if (app.onTablesChanged) app.onTablesChanged ();
}

void TablesPanel::showContextMenu (int row)
{
    const int nSel = table.getNumSelectedRows ();
    const auto prRef = rowRef (primaryRow());
    const bool haveTableScope  = prRef.tableIdx >= 0 && prRef.paramIdx < 0;
    const bool haveParamScope  = prRef.tableIdx >= 0 && prRef.paramIdx >= 0;

    juce::PopupMenu m;
    m.addItem (1, "Paste to new table");
    m.addSeparator ();
    m.addItem (2, "Remove Selected", nSel > 0);
    if (nSel > 0)
        m.addItem (3, "Copy", true);

    // Rename the item under the mouse (primary row).
    if (haveTableScope)
    {
        m.addItem (6, "Rename Table...");
        m.addItem (8, "Down sample...");
    }
    else if (haveParamScope)
        m.addItem (7, "Rename Parameter...");

    // Auto align: exactly two parameters from different tables selected. The
    // parameter under the mouse is aligned onto the other by correlation.
    {
        const auto sel = selectedRowIndices();
        if (sel.size() == 2)
        {
            const auto a = rowRef (sel[0]), b = rowRef (sel[1]);
            if (a.paramIdx >= 0 && b.paramIdx >= 0 && a.tableIdx != b.tableIdx)
            {
                auto* d = app.doc ();
                if (d != nullptr
                    && a.tableIdx < (int) d->tables.size() && b.tableIdx < (int) d->tables.size())
                {
                    const juce::String label = "Auto Align " + d->tables[(size_t) a.tableIdx].name
                                              + " / " + d->tables[(size_t) a.tableIdx].params[(size_t) a.paramIdx].name
                                              + " with " + d->tables[(size_t) b.tableIdx].name
                                              + " / " + d->tables[(size_t) b.tableIdx].params[(size_t) b.paramIdx].name;
                    m.addItem (4, label);
                }
            }
        }
    }

    // Time alignment is shared at the table level: right-clicking a parameter
    // row offers it (and re-aligns the whole table). Table rows themselves no
    // longer carry an Align menu.
    if (haveParamScope)
    {
        juce::PopupMenu align;
        auto makeTarget = [] (int baseId) -> juce::PopupMenu
        {
            juce::PopupMenu s;
            s.addItem (baseId + 0, "Manual");
            s.addItem (baseId + 1, "1 Point");
            s.addItem (baseId + 2, "2 Points");
            return s;
        };
        align.addSubMenu ("Waveform", makeTarget (21));
        align.addSubMenu ("Spectrogram", makeTarget (24));
        juce::PopupMenu ded;
        ded.addItem (27, "Manual");   // dedicated track: value mapping only
        align.addSubMenu ("Dedicated", ded);
        m.addSubMenu ("Align", align);
    }



    const int id = m.showMenu (juce::PopupMenu::Options ().withMousePosition ());
    switch (id)
    {
        case 1: pasteToNewTable (); break;
        case 2: removeSelected (); break;
        case 3: copySelection (); break;
        case 4:
        {
            // Correlate the two selected parameters and shift the one under
            // the mouse (primary) onto the other.
            const auto sel = selectedRowIndices();
            if (sel.size() == 2)
            {
                const auto a = rowRef (sel[0]), b = rowRef (sel[1]);
                auto* d = app.doc ();
                if (d != nullptr && a.paramIdx >= 0 && b.paramIdx >= 0
                    && a.tableIdx < (int) d->tables.size() && b.tableIdx < (int) d->tables.size())
                {
                    const auto& refT = d->tables[(size_t) b.tableIdx];
                    auto& aliT = d->tables[(size_t) a.tableIdx];
                    if (b.paramIdx < (int) refT.params.size() && a.paramIdx < (int) aliT.params.size())
                    {
                        const double lag = sc::tables::autoAlignLag (refT, refT.params[(size_t) b.paramIdx],
                                                                    aliT, aliT.params[(size_t) a.paramIdx]);
                        auto& p = aliT.params[(size_t) a.paramIdx];
                        if (a.tableIdx == b.tableIdx)
                        {
                            // Both curves share the table's time map, so only
                            // this parameter's own offset can separate them.
                            p.offsetSecs += lag;
                        }
                        else
                        {
                            // Time alignment is shared at the table level: the
                            // parameter under the mouse lands on its effective
                            // map shifted by the lag, and every other parameter
                            // of its table adopts that same map.
                            const double effOff = aliT.offsetSecs + p.offsetSecs;
                            const double effSc  = aliT.scaleTime * p.scaleTime;
                            aliT.offsetSecs = effOff + lag;
                            aliT.scaleTime = effSc;
                            for (auto& q : aliT.params)
                            {
                                q.offsetSecs = 0.0;
                                q.scaleTime = 1.0;
                            }
                        }
                        d->metaDirty = true;
                        refresh ();
                        if (app.onTablesChanged) app.onTablesChanged ();
                    }
                }
            }
            break;
        }
        case 6: renamePrimaryTable (); break;
        case 7: renamePrimaryParameter (); break;
        case 8: downsamplePrimaryTable (); break;

        // Parameter-level Align submenu: Waveform = 21..23, Spectrogram = 24..26.
        case 21: showManualAlignDialog (prRef.tableIdx, prRef.paramIdx, 0); break;
        case 22: startPointAlign (1, prRef.tableIdx, prRef.paramIdx, 0); break;
        case 23: startPointAlign (2, prRef.tableIdx, prRef.paramIdx, 0); break;
        case 24: showManualAlignDialog (prRef.tableIdx, prRef.paramIdx, 1); break;
        case 25: startPointAlign (1, prRef.tableIdx, prRef.paramIdx, 1); break;
        case 26: startPointAlign (2, prRef.tableIdx, prRef.paramIdx, 1); break;

        // Dedicated > Manual: value mapping; routes the parameter to the
        // dedicated track only (hidden from both overlays).
        case 27: showManualAlignDialog (prRef.tableIdx, prRef.paramIdx, 2); break;

        default: break;
    }
}

void TablesPanel::paint (juce::Graphics& g)
{
    // Same dark grey as the other panels.
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));
}

void TablesPanel::resized()
{
    const auto r = getLocalBounds ();

    // Top row: master visibility switch + display-mode dropdown.
    masterShowBtn.setBounds (6, 4, 26, 22);
    modeCombo.setBounds (38, 5, r.getWidth () - 44, 20);

    statusLabel.setBounds (6, 28, r.getWidth () - 12, 16);

    // List, splitter (the 6 px gap), plot.
    const int plotH = effectivePlotHeight();
    table.setBounds (6, 46, r.getWidth () - 12, std::max (0, r.getHeight () - 46 - plotH - 8));
    splitter.setBounds (6, r.getHeight () - plotH - 8, r.getWidth () - 12, 6);
    plot.setBounds (6, r.getHeight () - plotH - 2, r.getWidth () - 12, plotH);
}

int TablesPanel::effectivePlotHeight() const
{
    constexpr int kMinPlotH = 50, kMinListH = 40;
    const int h = getHeight ();
    const int want = plotHeight_ > 0 ? plotHeight_ : std::max (70, h / 4);
    // Room left under the header rows for the list and the 6 px splitter.
    const int maxPlot = std::max (kMinPlotH, h - 46 - 8 - kMinListH);
    return std::clamp (want, kMinPlotH, maxPlot);
}

void TablesPanel::seekToTableTime (double tTableSecs)
{
    app.setPlayhead (plot.tableToDocTime (tTableSecs));   // clamps to the file; updates every view
}

void TablesSplitter::paint (juce::Graphics& g)
{
    // A short dotted grip in the middle; the cursor says the rest.
    g.setColour (juce::Colour::fromRGB (96, 102, 114));
    const int cy = getHeight () / 2, cx = getWidth () / 2;
    for (int i = -2; i <= 2; ++i)
        g.fillRect (cx + i * 8 - 2, cy, 4, 1);
}


// ============================================================= Actions panel

ActionsPanel::Model::Model (ActionsPanel* ownerIn) : owner (ownerIn) {}

int ActionsPanel::Model::getNumRows() { return (int) owner->app.actions.size (); }

void ActionsPanel::Model::paintRowBackground (juce::Graphics& g, int, int width,
                                              int height, bool selected)
{
    if (selected)
    {
        // Same selection colour as the marker list.
        g.setColour (juce::Colour::fromRGB (60, 80, 120));
        g.fillRect (0, 0, width, height);
    }
}

void ActionsPanel::Model::paintCell (juce::Graphics& g, int rowNumber, int columnId,
                                     int width, int height, bool)
{
    if (rowNumber < 0 || rowNumber >= getNumRows()) return;
    const auto* act = owner->app.actions.get (rowNumber);
    if (act == nullptr) return;

    g.setFont (11.0f);
    if (columnId == 1)   // Name
    {
        int x = 4;
        if (owner->isRecording() && owner->recordingIndex () == rowNumber)
        {
            g.setColour (juce::Colours::red);
            g.fillEllipse (x, height / 2 - 4, 8, 8);
            x += 12;
        }
        g.setColour (juce::Colours::white);
        g.drawText (act->name, x, 0, width - x - 4, height,
                    juce::Justification::centredLeft, false);
    }
    else                 // Size
    {
        const int n = (int) act->steps.size ();
        g.setColour (juce::Colours::lightgrey);
        g.drawText (n == 0 ? "-" : juce::String (n) + " step" + (n == 1 ? "" : "s"),
                    4, 0, width - 8, height, juce::Justification::centredLeft, false);
    }
}

void ActionsPanel::Model::cellClicked (int rowNumber, int, const juce::MouseEvent& e)
{
    if (rowNumber >= 0 && rowNumber < getNumRows())
        owner->onRowClicked (rowNumber, e);
}

void ActionsPanel::Model::selectedRowsChanged (int)
{
    owner->syncSelectionFromTable ();
}

ActionsPanel::ActionsPanel (Application& a) : app (a)
{
    newBtn.setButtonText ("New");
    recordBtn.setButtonText ("Record");
    runBtn.setButtonText ("Run");
    batchBtn.setButtonText ("Batch");

    for (auto* b : { &newBtn, &recordBtn, &runBtn, &batchBtn })
        addAndMakeVisible (*b);

    newBtn.onClick = [this] { handleNew (); };
    recordBtn.onClick = [this] { handleRecord (); };
    runBtn.onClick = [this] { handleRun (); };
    batchBtn.onClick = [this] { handleBatch (); };

    // Same table style as the marker list: light header with Name/Size columns.
    auto& header = table.getHeader();
    const int fixedFlags = juce::TableHeaderComponent::notResizableOrSortable;
    header.addColumn ("Name", 1, 200, 60, -1, fixedFlags);
    header.addColumn ("Size", 2, 80, 40, -1, fixedFlags);

    // Excel-style multi-selection: Ctrl/Cmd+click toggles rows, Shift+click
    // range-selects from the anchor row.
    table.setMultipleSelectionEnabled (true);
    table.setSuppressShiftRangeSelection (true);   // we do the exact-range shift select ourselves

    addAndMakeVisible (&table);

    // Right-clicking a row shows the panel context menu. Deep registration is
    // required: clicks on rows are delivered to the row's own component, not
    // to the TableListBox itself.
    table.addMouseListener (this, true);
}

void ActionsPanel::syncSelectionFromTable()
{
    std::vector<int> rows;
    for (int r : sparseRows (table.getSelectedRows()))
        if (r >= 0 && r < app.actions.size ()) rows.push_back (r);

    bool same = rows.size () == selected_.size ();
    if (same)
        for (size_t i = 0; i < rows.size (); ++i)
            if (rows[i] != selected_[(size_t) i]) { same = false; break; }
    if (!same)
    {
        selected_ = std::move (rows);
        repaint();
    }
}

void ActionsPanel::applySelectionToTable()
{
    juce::SparseSet<int> sel;
    for (int i : selected_)
        if (i >= 0 && i < app.actions.size ()) sel.addRange ({ i, i + 1 });
    table.setSelectedRows (sel, juce::dontSendNotification);
}

void ActionsPanel::onRowClicked (int row, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu ()) return;   // right-click: keep any multi-selection

    table.setWantsKeyboardFocus (true);
    if (table.isShowing ())   // a focus grab on a hidden table is a no-op (and asserts)
        table.grabKeyboardFocus ();

    if (e.mods.isShiftDown())
    {
        // Excel: the selection becomes EXACTLY anchor..row (JUCE's built-in
        // shift behaviour unions instead).
        const int n = app.actions.size ();
        const int a = (anchor_ >= 0 && anchor_ < n) ? anchor_ : row;
        juce::SparseSet<int> sel;
        sel.addRange ({ std::min (a, row), std::max (a, row) + 1 });
        table.setSelectedRows (sel, juce::sendNotification);
    }
    else if (e.mods.isCtrlDown())
    {
        // JUCE already toggled this row in selectRowsBasedOnModifierKeys before
        // cellClicked fires - just track the anchor here.
        anchor_ = row;
    }
    else
    {
        selected_ = { row };
        anchor_ = row;
        applySelectionToTable();
    }
}

void ActionsPanel::clearSelection()
{
    selected_.clear ();
    anchor_ = -1;
    applySelectionToTable();
}

void ActionsPanel::resized()
{
    const int gap = 4;
    const int bw = (std::max (80, getWidth()) - 8 - 3 * gap) / 4;
    newBtn.setBounds (4, 2, bw, kHeaderH - 6);
    recordBtn.setBounds (newBtn.getRight() + gap, 2, bw, kHeaderH - 6);
    runBtn.setBounds (recordBtn.getRight() + gap, 2, bw, kHeaderH - 6);
    batchBtn.setBounds (runBtn.getRight() + gap, 2, bw, kHeaderH - 6);

    table.setBounds (0, kHeaderH, getWidth(), std::max (0, getHeight() - kHeaderH));
}

void ActionsPanel::paint (juce::Graphics& g)
{
    // The table provides the list styling (same as the marker panel); this
    // only paints the button-row background.
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));
}

void ActionsPanel::mouseDown (const juce::MouseEvent& e)
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
    const auto pInTable = juce::Point<int> ((int) px, (int) py);

    // JUCE 9's TableListBox has no getRowNumberAtPosition; find the row by
    // testing each cell position (action lists are small).
    int row = -1;
    for (int r = 0; r < table.getNumRows(); ++r)
        if (table.getCellPosition (1, r, true).contains (pInTable))
        {
            row = r;
            break;
        }

    // Right-clicking a row selects it only when nothing is selected yet - an
    // existing multi-selection is left alone so the menu can act on all of it.
    if (row >= 0 && table.getNumSelectedRows () == 0)
    {
        anchor_ = row;
        selected_ = { row };
        applySelectionToTable();
    }

    showContextMenu ();
}

void ActionsPanel::showContextMenu ()
{
    juce::PopupMenu m;
    const bool hasSel = !selected_.empty ();
    // NOTE: the 4th argument is "checked" - it must stay false or JUCE draws
    // a checkmark next to the item.
    m.addItem (1, "Remove Selected", hasSel, false);
    m.addItem (2, "Rename",          hasSel, false);
    m.addItem (3, "Edit",            hasSel, false);


    switch (m.showMenu (juce::PopupMenu::Options().withMousePosition()))
    {
        case 1:
            app.actions.removeSelected (selected_);
            clearSelection();
            app.saveActions ();
            table.updateContent ();
            break;

        case 2:
        {
            const int p = primaryIndex();
            if (p < 0) return;
            auto* w = new juce::AlertWindow ("Rename Action", "New name:",
                                             juce::AlertWindow::QuestionIcon, this);
            w->addTextEditor ("name", app.actions.get (p)->name);
            w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));
            if (auto* ed = w->getTextEditor ("name"))
                ed->setWantsKeyboardFocus (true);
            w->enterModalState (false);
            const int r = w->runModalLoop();
            const juce::String name = w->getTextEditorContents ("name");
            delete w;
            if (r != 1) return;

            if (!app.actions.rename (p, name))
                sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                                   "Rename Action",
                                                   "That name is empty or already in use.", "", this);
            else
            {
                app.saveActions ();
                table.updateContent ();   // refresh the Name cell text
            }
            break;
        }

        case 3:
        {
            const int p = primaryIndex();
            if (p < 0) return;
            auto* w = new ActionEditWindow (app, p);
            w->onSaved = [this] { table.updateContent (); };
            w->onCloseRequested = [w] { juce::MessageManager::callAsync ([w] { delete w; }); };
            w->centreWithSize (680, 540);
            w->setVisible (true);
            break;
        }

        default:
            break;
    }
}

void ActionsPanel::handleNew()
{
    auto* w = new juce::AlertWindow ("New Action", "Name of the new action:",
                                     juce::AlertWindow::QuestionIcon, this);
    w->addTextEditor ("name", "");
    w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));
    if (auto* ed = w->getTextEditor ("name"))
        ed->setWantsKeyboardFocus (true);
    w->enterModalState (false);
    const int r = w->runModalLoop();
    const juce::String name = w->getTextEditorContents ("name");
    delete w;
    if (r != 1) return;

    const int idx = app.actions.add (name);
    if (idx < 0)
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                           "New Action",
                                           "That name is empty or already in use.", "", this);
        return;
    }

    selected_ = { idx };
    anchor_ = idx;
    applySelectionToTable();
    table.updateContent ();
    app.saveActions ();
    repaint();
}

void ActionsPanel::handleRecord()
{
    if (app.actionRec.active)
    {
        app.actionRec.active = false;
        recordBtn.setButtonText ("Record");
        repaint();
        return;
    }

    int idx = primaryIndex();
    if (idx < 0)
    {
        auto* w = new juce::AlertWindow ("Record Action",
                                         "No action is selected - name the NEW action to record into:",
                                         juce::AlertWindow::QuestionIcon, this);
        w->addTextEditor ("name", "");
        w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));
        if (auto* ed = w->getTextEditor ("name"))
            ed->setWantsKeyboardFocus (true);
        w->enterModalState (false);
        const int r = w->runModalLoop();
        const juce::String name = w->getTextEditorContents ("name");
        delete w;
        if (r != 1) return;

        idx = app.actions.add (name);
        if (idx < 0)
        {
            sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                               "Record Action",
                                               "That name is empty or already in use.", "", this);
            return;
        }
        selected_ = { idx };
        anchor_ = idx;
        applySelectionToTable();
        table.updateContent ();
        app.saveActions ();
    }

    app.actionRec.active = true;
    app.actionRec.actionIndex = idx;
    recordBtn.setButtonText ("Stop Rec");
    repaint();
}

void ActionsPanel::handleRun()
{
    const int idx = primaryIndex();
    if (idx < 0)
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                           "Run Action",
                                           "Select an action in the list first.", "", this);
        return;
    }
    if (app.actions.get (idx)->steps.empty ())
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                           "Run Action",
                                           "The selected action has no recorded steps.", "", this);
        return;
    }
    if (onRunRequested) onRunRequested (idx);
}

void ActionsPanel::handleBatch()
{
    const int idx = primaryIndex();
    if (idx < 0)
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                           "Action Batch",
                                           "Select an action in the list first.", "", this);
        return;
    }
    if (app.actions.get (idx)->steps.empty ())
    {
        sc::alert::showMessageBox (juce::AlertWindow::WarningIcon,
                                           "Action Batch",
                                           "The selected action has no recorded steps.", "", this);
        return;
    }
    if (onBatchRequested) onBatchRequested (idx);
}

// ============================================================= Action JSON edit window

ActionEditWindow::ActionEditWindow (Application& a, int index)
    : app (a), index_ (index),
      DocumentWindow ("Edit Action", juce::Colour::fromRGB (45, 45, 48),
                      juce::DocumentWindow::allButtons)
{
    const auto* act = app.actions.get (index);

    editor.setMultiLine (true);
    editor.setFont (juce::Font ("Consolas", 12.0f, juce::Font::plain));
    editor.setColour (juce::TextEditor::backgroundColourId, juce::Colour::fromRGB (38, 38, 42));
    editor.setColour (juce::TextEditor::textColourId, juce::Colours::lightgrey);
    editor.setText (act != nullptr ? sc::jsonPrettyPrint (act->toJson()) : "{}", juce::NotificationType::dontSendNotification);

    errorLabel.setFont (11.0f);
    errorLabel.setJustificationType (juce::Justification::left);
    errorLabel.setColour (juce::Label::textColourId, juce::Colours::orange);

    okBtn.setButtonText ("OK");
    cancelBtn.setButtonText ("Cancel");
    okBtn.onClick = [this] { okClicked (); };
    cancelBtn.onClick = [this] { if (onCloseRequested) onCloseRequested (); };

    juce::Component::addAndMakeVisible (&editor);
    juce::Component::addAndMakeVisible (&errorLabel);
    juce::Component::addAndMakeVisible (&okBtn);
    juce::Component::addAndMakeVisible (&cancelBtn);

    setResizable (true, true);
}

void ActionEditWindow::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));
}

void ActionEditWindow::resized()
{
    auto r = getLocalBounds();
    const int btnH = 28;
    auto bot = r.removeFromBottom (btnH + 6);
    okBtn.setBounds (bot.getRight () - 150, 4, 68, btnH);
    cancelBtn.setBounds (okBtn.getX () - 8 - 78, 4, 78, btnH);

    const int errH = 34;
    errorLabel.setBounds (r.removeFromBottom (errH).withTrimmedLeft (10)
                         .withTrimmedRight (10));

    editor.setBounds (r.withTrimmedLeft (6).withTrimmedTop (6).withTrimmedRight (6));
}

void ActionEditWindow::userTriedToCloseWindow()
{
    if (onCloseRequested) onCloseRequested ();
}

void ActionEditWindow::okClicked()
{
    const juce::String text = editor.getText ();
    const juce::var root = juce::JSON::parse (text);
    if (!root.isObject())
    {
        errorLabel.setText ("Invalid JSON: the document must be a single object.", juce::NotificationType::dontSendNotification);
        return;
    }

    const auto nameVar = root.getProperty ("name", juce::var());
    if (!nameVar.isString () || nameVar.toString().trim().isEmpty ())
    {
        errorLabel.setText ("\"name\" must be a non-empty string.", juce::NotificationType::dontSendNotification);
        return;
    }
    const juce::String newName = nameVar.toString().trim ();

    for (int i = 0; i < app.actions.size (); ++i)
        if (i != index_ && app.actions.get (i)->name == newName)
        {
            errorLabel.setText ("An action named \"" + newName + "\" already exists.", juce::NotificationType::dontSendNotification);
            return;
        }

    const auto stepsVar = root.getProperty ("steps", juce::var());
    if (!stepsVar.isVoid () && !stepsVar.isArray ())
    {
        errorLabel.setText ("\"steps\" must be an array of step objects.", juce::NotificationType::dontSendNotification);
        return;
    }

    std::vector<sc::ActionStep> newSteps;
    for (int i = 0; i < stepsVar.size (); ++i)
    {
        std::string err;
        const auto s = sc::ActionStep::fromJson (stepsVar[(size_t) i], &err);
        if (s.type.isEmpty ())
        {
            errorLabel.setText ("step #" + juce::String (i + 1) + ": "
                                + juce::String::fromUTF8 (err.c_str()), juce::NotificationType::dontSendNotification);
            return;
        }

        std::string verr;
        if (!sc::validateStepParams (s, &verr))
        {
            errorLabel.setText ("step #" + juce::String (i + 1) + " (" + s.type + "): "
                                + juce::String::fromUTF8 (verr.c_str()), juce::NotificationType::dontSendNotification);
            return;
        }
        newSteps.push_back (std::move (s));
    }

    auto* act = app.actions.get (index_);
    if (act == nullptr)
    {
        errorLabel.setText ("The action no longer exists.", juce::NotificationType::dontSendNotification);
        return;
    }
    act->name = newName;
    act->steps = std::move (newSteps);
    app.saveActions ();

    if (onSaved) onSaved ();
    if (onCloseRequested) onCloseRequested ();
}

// ============================================================= Action batch window

ActionBatchWindow::ActionBatchWindow (Application& a, int actionIndex)
    : app (a), actionIndex_ (actionIndex),
      DocumentWindow ("Action Batch", juce::Colour::fromRGB (45, 45, 48),
                      juce::DocumentWindow::allButtons)
{
    const auto* act = app.actions.get (actionIndex);

    inputLbl.setText ("Input Folder:", juce::NotificationType::dontSendNotification);
    outputLbl.setText ("Output Folder:", juce::NotificationType::dontSendNotification);
    formatsLbl.setText ("Input Formats:", juce::NotificationType::dontSendNotification);
    renameLbl.setText ("Rename:", juce::NotificationType::dontSendNotification);
    statusLabel.setFont (11.0f);
    statusLabel.setJustificationType (juce::Justification::left);

    for (auto* f : { &inputField, &outputField, &formatsField, &renameField })
        f->setColour (juce::TextEditor::backgroundColourId, juce::Colour::fromRGB (38, 38, 42));

    formatsField.setText ("*.wav; *.mp3", false);
    renameField.setText ("%Filename%.wav", juce::NotificationType::dontSendNotification);

    sameOutBox.setButtonText ("Same output folder");
    subfoldersBox.setButtonText ("Include subfolders");
    sameOutBox.onClick = [this] { sameOutToggled (); };

    browseInBtn.setButtonText ("...");
    browseOutBtn.setButtonText ("...");
    runBtn.setButtonText ("Run");
    closeBtn.setButtonText ("Close");
    browseInBtn.onClick = [this] { browseInput (); };
    browseOutBtn.onClick = [this] { browseOutput (); };
    runBtn.onClick = [this] { runClicked (); };
    closeBtn.onClick = [this] { tryClose (); };

    juce::Component::addAndMakeVisible (&inputLbl);
    juce::Component::addAndMakeVisible (&inputField);
    juce::Component::addAndMakeVisible (&browseInBtn);
    juce::Component::addAndMakeVisible (&sameOutBox);
    juce::Component::addAndMakeVisible (&outputLbl);
    juce::Component::addAndMakeVisible (&outputField);
    juce::Component::addAndMakeVisible (&browseOutBtn);
    juce::Component::addAndMakeVisible (&subfoldersBox);
    juce::Component::addAndMakeVisible (&formatsLbl);
    juce::Component::addAndMakeVisible (&formatsField);
    juce::Component::addAndMakeVisible (&renameLbl);
    juce::Component::addAndMakeVisible (&renameField);
    juce::Component::addAndMakeVisible (&statusLabel);
    juce::Component::addAndMakeVisible (&runBtn);
    juce::Component::addAndMakeVisible (&closeBtn);

    sameOutToggled ();   // initial state: output enabled, not mirrored

    setResizable (true, true);
}

void ActionBatchWindow::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));
}

void ActionBatchWindow::resized()
{
    auto r = getLocalBounds().withTrimmedLeft (12).withTrimmedTop (10);
    const int rowH = 30;
    const int lblW = 96;
    const int btnW = 40;

    // Input folder row.
    {
        auto row = r.removeFromTop (rowH);
        inputLbl.setBounds (row.removeFromLeft (lblW));
        browseInBtn.setBounds (row.removeFromRight (btnW + 8).withTrimmedBottom (4));
        inputField.setBounds (row.withTrimmedBottom (4));
    }

    // "Same output folder" checkbox row.
    sameOutBox.setBounds (r.removeFromTop (rowH).withX (lblW));

    // Output folder row.
    {
        auto row = r.removeFromTop (rowH);
        outputLbl.setBounds (row.removeFromLeft (lblW));
        browseOutBtn.setBounds (row.removeFromRight (btnW + 8).withTrimmedBottom (4));
        outputField.setBounds (row.withTrimmedBottom (4));
    }

    // Include subfolders checkbox row.
    subfoldersBox.setBounds (r.removeFromTop (rowH).withX (lblW));

    // Input formats row.
    {
        auto row = r.removeFromTop (rowH);
        formatsLbl.setBounds (row.removeFromLeft (lblW));
        formatsField.setBounds (row.withTrimmedBottom (4));
    }

    // Rename row.
    {
        auto row = r.removeFromTop (rowH);
        renameLbl.setBounds (row.removeFromLeft (lblW));
        renameField.setBounds (row.withTrimmedBottom (4));
    }

    // Status row.
    statusLabel.setBounds (r.removeFromTop (rowH));

    // Bottom buttons: Run left of Close, both right-aligned.
    const auto btnRow = r.removeFromBottom (38);
    closeBtn.setBounds (btnRow.getRight () - 90, btnRow.getY () + 4, 82, 30);
    runBtn.setBounds (closeBtn.getX () - 10 - 82, closeBtn.getY (), 82, 30);
}

void ActionBatchWindow::sameOutToggled()
{
    const bool same = sameOutBox.getToggleState ();
    outputField.setEnabled (!same);
    if (same)
        outputField.setText (inputField.getText (), juce::NotificationType::dontSendNotification);
}

void ActionBatchWindow::browseInput()
{
    juce::FileChooser fc ("Choose the input folder...",
                          inputField.getText ().isEmpty () ? juce::File ()
                                                           : juce::File (inputField.getText()));
    if (fc.browseForDirectory ())
        inputField.setText (fc.getResult().getFullPathName (), juce::NotificationType::dontSendNotification);
}

void ActionBatchWindow::browseOutput()
{
    if (sameOutBox.getToggleState ()) return;   // mirrored - nothing to browse for
    juce::FileChooser fc ("Choose the output folder...",
                          outputField.getText ().isEmpty () ? juce::File ()
                                                           : juce::File (outputField.getText()));
    if (fc.browseForDirectory ())
        outputField.setText (fc.getResult().getFullPathName (), juce::NotificationType::dontSendNotification);
}

void ActionBatchWindow::tryClose()
{
    if (running_.load ())
    {
        statusLabel.setText ("A batch is still running - wait for it to finish.", juce::NotificationType::dontSendNotification);
        return;
    }
    if (onCloseRequested) onCloseRequested ();
}

void ActionBatchWindow::userTriedToCloseWindow()
{
    tryClose ();
}

void ActionBatchWindow::runClicked()
{
    if (running_.load ()) return;

    const auto in = juce::File (inputField.getText ().trim ());
    const bool sameOut = sameOutBox.getToggleState ();
    const auto outDir = sameOut ? in : juce::File (outputField.getText ().trim ());
    const auto formats = sc::parseFormatList (formatsField.getText ());
    const auto renamePattern = renameField.getText ().trim ();

    if (!in.isDirectory ())
    {
        statusLabel.setText ("The input folder does not exist.", juce::NotificationType::dontSendNotification);
        return;
    }
    if (!sameOut && !outDir.isDirectory ())
    {
        statusLabel.setText ("The output folder does not exist.", juce::NotificationType::dontSendNotification);
        return;
    }
    if (formats.empty ())
    {
        statusLabel.setText ("Enter at least one input format, e.g. *.wav", juce::NotificationType::dontSendNotification);
        return;
    }

    const auto* act = app.actions.get (actionIndex_);
    if (act == nullptr || act->steps.empty ())
    {
        statusLabel.setText ("The action has no steps to run.", juce::NotificationType::dontSendNotification);
        return;
    }

    // Isolated recent-store for the headless per-file applications so the
    // user's settings are never touched.
    const auto tempStore = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("SpectroCraftBatch.recent");
    tempStore.deleteFile ();

    running_.store (true);
    runBtn.setEnabled (false);
    statusLabel.setText ("Starting batch...", juce::NotificationType::dontSendNotification);

    auto* prog = new sc::WorkProgressDialog ("Action Batch",
                                             "Running action \"" + act->name + "\" ...",
                                             true, this);
    prog->setProgress (0.0);
    prog->beginModal ();

    // Copy the action: the user can delete/rename it from the panel while
    // this batch is running; the worker must not touch app.actions.
    const sc::Action actionCopy = *act;

    std::thread ([this, prog, in, outDir, formats, renamePattern,
                  subfolders = subfoldersBox.getToggleState (), tempStore, actionCopy] {
        std::string summary;
        const bool ok = sc::runActionBatch (actionCopy,
                                            in, outDir, formats, renamePattern, subfolders,
                                            tempStore,
                                            [prog] (float f) -> bool {
                                                juce::MessageManager::callAsync ([prog, f] {
                                                    prog->setProgress ((double) f);
                                                });
                                                return !prog->cancelled.load ();
                                            },
                                            [prog] (const juce::String& s) {
                                                juce::MessageManager::callAsync ([prog, s] {
                                                    prog->setStatusText (s);
                                                });
                                            },
                                            &prog->cancelled, &summary);

        juce::MessageManager::callAsync ([this, prog, ok, summary] {
            delete prog;
            running_.store (false);
            runBtn.setEnabled (true);
            statusLabel.setText (juce::String::fromUTF8 (summary.c_str()), juce::NotificationType::dontSendNotification);
        });
    }).detach ();
}
