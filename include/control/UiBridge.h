#pragma once
// SpectroCraft - AI control: what a command may ask of the open window.
//
// Implemented by MainWindow; nullptr in the --json CLI and in unit tests.
// Rule: no command ever calls menuItemSelected, runModalLoop, a FileChooser or
// showEditError - the bridge only exposes non-blocking UI operations.
#include <JuceHeader.h>
#include <functional>

namespace sc::control {

class UiBridge
{
public:
    virtual ~UiBridge() = default;

    /** After a mutating command: purge tile caches, resync views/panels. */
    virtual void refreshAfterEdit() = 0;
    /** Sets both views' time window. */
    virtual void applyTimeWindow (double t0, double t1) = 0;
    virtual void zoomToSelection() = 0;
    virtual void zoomToFit() = 0;
    virtual void zoomOutFull() = 0;
    /** Vertical zoom of both views (factor > 1 zooms in). */
    virtual void zoomVertical (double factor) = 0;
    /** Shows/hides the waveform, the spectrogram or the left panels. */
    virtual bool setViewVisible (const juce::String& what, bool visible) = 0;
    virtual bool isViewVisible (const juce::String& what) const = 0;
    /** Window > panels (index into the panel list). */
    virtual juce::StringArray panelNames() const = 0;
    virtual bool setPanelVisible (int index, bool visible) = 0;
    virtual bool isPanelOpen (int index) const = 0;

    /** Non-blocking progress dialog for a job (same as the menus use, so the
        app's own "busy" checks see it). Returns an opaque handle. */
    virtual void* beginJobDialog (const juce::String& title, const juce::String& status) = 0;
    virtual void setJobProgress (void* handle, double progress, const juce::String& status) = 0;
    /** True when the user pressed Cancel in that dialog. */
    virtual bool jobDialogCancelled (void* handle) const = 0;
    virtual void endJobDialog (void* handle) = 0;

    /** Status bar indicator: number of connected control clients. */
    virtual void setControlClients (int count) = 0;
};

} // namespace sc::control
