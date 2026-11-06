#pragma once
// SpectroCraft - AI control: which files a command may read or write (RAPPORT §5.3).
//
// Paths must be absolute and, after resolving symbolic links, lie under one of
// the allowed roots: setting "control_roots" (";"-separated absolute folders)
// or, by default, the user's home folder + scriptFolder() + the temp folder.
// Outside the roots -> confirm_required {reason:"outside_roots"} unless
// params.confirm. The control folder (tokens) and the settings store
// (SpectroCraft.recent) are refused, confirm or not.
#include "control/CommandRegistry.h"

namespace sc::control {

/** The resolved allowed roots. */
juce::Array<juce::File> allowedRoots (Application& app);

/** Canonical form: symbolic links resolved on every existing component. */
juce::File canonicalFile (const juce::File& f);

/** Returns success (result = {path}) or the refusal outcome. `what` names the
    parameter for the error path ("path", "script"...). */
Outcome checkPath (Application& app, const juce::File& f, bool forWrite, bool confirmed,
                   const juce::String& what = "path");

} // namespace sc::control
