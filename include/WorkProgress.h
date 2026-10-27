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
    // withCancel adds a Cancel button that sets `cancelled` and exits the
    // modal loop. The worker thread must keep owning the dialog's lifetime:
    // it checks `cancelled` between units of work and deletes the dialog.
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

        if (withCancel)
        {
            for (auto* c : getChildren())
                if (auto* b = dynamic_cast<juce::TextButton*> (c))
                {
                    // The AlertWindow's own click listener exits the modal state
                    // when the button is pressed; this flag tells the worker
                    // thread to stop.
                    if (b->getButtonText () == "Cancel")
                        b->onClick = [this] { cancelled.store (true); };
                }
        }
    }

    // Set by the Cancel button / ESC; the worker thread checks it between
    // units of work and then deletes this dialog.
    std::atomic<bool> cancelled { false };

    void setProgress (double v) { progressValue = v; }

    // Update the message text (call on the message thread).
    void setStatusText (const juce::String& s) { setMessage (s); }

    /** Enters modal state and grabs keyboard focus so that ESC reaches the
        Cancel button's shortcut (AlertWindow::keyPressed checks its buttons'
        registered shortcuts). Progress dialogs contain no text fields, so
        taking focus is safe. The worker thread still owns this dialog's
        lifetime: after a cancel it finishes the current unit of work and
        deletes the dialog. */
    void beginModal()
    {
        enterModalState (false);
        setWantsKeyboardFocus (true);   // AlertWindow disables this when it has children
        grabKeyboardFocus ();
    }

private:
    double progressValue = 0.0;
};

} // namespace sc
