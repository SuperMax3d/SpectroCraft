#pragma once
// SpectroCraft - which file dialogs to show (issue #71)
#include <JuceHeader.h>

namespace sc::files
{
/** True when the operating system's own file dialogs are used for every file
    and folder pick (open, save, export, batch folders...). They are, unless:
    - the environment variable SC_NATIVE_FILE_DIALOGS is "0" (JUCE's own
      browser everywhere: a way out on a machine where the system dialog
      misbehaves);
    - on Windows, the system's file dialog cannot be created: on some systems
      its COM class is not registered, or creating it crashed (checked once,
      guarded; JUCE's dialog would otherwise return nothing, as if cancelled).
    On Linux JUCE itself falls back to its own browser when neither zenity nor
    kdialog is installed. Pass the result as FileChooser's
    useOSNativeDialogBox argument. */
bool useNativeDialogs();

/** The rule above without the Windows check: `envValue` is the variable's
    value ("" when unset), `systemDialogWorks` the result of that check. Pure;
    for the tests. */
bool useNativeDialogsFor (const juce::String& envValue, bool systemDialogWorks);
} // namespace sc::files
