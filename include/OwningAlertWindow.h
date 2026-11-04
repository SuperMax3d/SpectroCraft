// SpectroCraft - AlertWindow that owns the components given to addCustomComponent()
#pragma once

#include <JuceHeader.h>

namespace sc {

/** juce::AlertWindow never deletes the components passed to
    addCustomComponent() (see juce_AlertWindow.h): dialogs that hand them over
    with `new` leaked them on every open. This AlertWindow deletes its custom
    components when it is destroyed. (MainWindow.cpp has its own copy in an
    anonymous namespace.) */
class OwningAlertWindow : public juce::AlertWindow
{
public:
    using juce::AlertWindow::AlertWindow;

    ~OwningAlertWindow() override
    {
        for (int i = getNumCustomComponents(); --i >= 0;)
            delete removeCustomComponent (i);
    }
};

} // namespace sc
