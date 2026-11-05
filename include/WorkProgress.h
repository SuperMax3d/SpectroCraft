// SpectroCraft - cancellable progress dialog for long-running worker jobs.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>

namespace sc {

/** Modal progress bar with an optional Cancel button. The WORKER thread owns
    the dialog's lifetime: it checks `cancelled` between units of work and
    deletes the dialog (deleting exits the modal state). */
class WorkProgressDialog : public juce::AlertWindow
{
public:
    // withCancel adds a Cancel button that sets `cancelled`; the dialog stays
    // modal (showing "Cancelling...") until the worker deletes it. The worker
    // thread must keep owning the dialog's lifetime: it checks `cancelled`
    // between units of work and deletes the dialog.
    WorkProgressDialog (const juce::String& title, const juce::String& message,
                        bool withCancel = false, Component* centreAround = nullptr)
        : juce::AlertWindow (title, message, juce::AlertWindow::NoIcon, centreAround)
    {
        // Use the AlertWindow's own progress bar so its layout code accounts
        // for it: when setStatusText() changes the message, updateLayout()
        // repositions text, bar and buttons together. A manually added child
        // bar plus a setSize() hack breaks that invariant - on the first
        // status change the Cancel button drops down onto the bar strip.
        addProgressBarComponent (progressValue);

        if (withCancel)
            addButton ("Cancel", 2, juce::KeyPress (juce::KeyPress::escapeKey));

        for (auto* c : getChildren())
        {
            if (auto* pb = dynamic_cast<juce::ProgressBar*> (c))
            {
                // The default LookAndFeel tables define no ProgressBar colours,
                // which resolve to transparent - set them explicitly so the bar
                // is visible on the dark dialog background.
                pb->setColour (juce::ProgressBar::backgroundColourId, juce::Colour (0xff232327));
                pb->setColour (juce::ProgressBar::foregroundColourId, juce::Colours::dodgerblue);

                // The bar's self-polling timer only starts when visibility
                // *changes*; toggle it once so updates are picked up from the start.
                pb->setVisible (false);
                pb->setVisible (true);
            }
        }

        // ESC must never dismiss the dialog on its own: AlertWindow's default
        // (escapeKeyCancels) calls exitModalState() when no button claims the
        // key, which re-enabled the whole UI while the worker was still
        // running (close/switch the document under it, start a second job).
        setEscapeKeyCancels (false);

        if (withCancel)
        {
            hasCancel = true;
            for (auto* c : getChildren())
                if (auto* b = dynamic_cast<juce::TextButton*> (c))
                {
                    // Replaces AlertWindow's own onClick (which would call
                    // exitModalState): the dialog stays modal until the worker
                    // has really stopped and deletes it.
                    if (b->getButtonText () == "Cancel")
                        b->onClick = [this] { requestCancel (); };
                }
        }

        numActiveRef().fetch_add (1);
    }

    ~WorkProgressDialog() override
    {
        numActiveRef().fetch_sub (1);
    }

    // Set by the Cancel button / ESC; the worker thread checks it between
    // units of work and then deletes this dialog.
    std::atomic<bool> cancelled { false };

    /** Number of progress dialogs alive, i.e. background jobs in flight (the
        worker deletes its dialog once it has finished). UI code must not
        close/switch documents or quit while this is non-zero. */
    static int numActive() { return numActiveRef().load(); }

    /** Cancel requested by the user (button, ESC, window close): flags the
        worker and shows "Cancelling..." but keeps the dialog modal - the
        worker deletes it once it has actually stopped. */
    void requestCancel()
    {
        if (!hasCancel || cancelled.exchange (true)) return;
        setMessage ("Cancelling...");
        if (auto* b = getButton ("Cancel"))
            b->setEnabled (false);
    }

    void setProgress (double v) { progressValue = v; }

    // Update the message text (call on the message thread). Ignored once a
    // cancel was requested so "Cancelling..." stays visible.
    void setStatusText (const juce::String& s) { if (!cancelled.load()) setMessage (s); }

    // Native window close (e.g. Alt+F4 on Windows) would exit the modal state
    // with AlertWindow's default; treat it like the Cancel button instead.
    void userTriedToCloseWindow() override { requestCancel (); }

    /** Enters modal state and grabs keyboard focus so that ESC reaches the
        Cancel button's shortcut (AlertWindow::keyPressed checks its buttons'
        registered shortcuts). Progress dialogs contain no text fields, so
        taking focus is safe. The worker thread still owns this dialog's
        lifetime: after a cancel it finishes the current unit of work and
        deletes the dialog - only then does the modal state end. */
    void beginModal()
    {
        enterModalState (false);
        setWantsKeyboardFocus (true);   // AlertWindow disables this when it has children
        grabKeyboardFocus ();
    }

private:
    static std::atomic<int>& numActiveRef()
    {
        static std::atomic<int> n { 0 };
        return n;
    }

    double progressValue = 0.0;
    bool hasCancel = false;
};

} // namespace sc
