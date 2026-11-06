#pragma once
// SpectroCraft - AI control: application / document / history state as JSON.
#include <JuceHeader.h>

class Application;
class FileDoc;

namespace sc::control {

/** {id, path, name, durationSec, channels, sampleRate, unsaved, fullyInMemory,
    undoDepth, redoDepth, contentVersion, metaMode, markers} */
juce::var docSummary (Application& app, const FileDoc& d);

/** The full app.state result (REPORT §6.2). */
juce::var appState (Application& app);

/** history.list (REPORT §6.3): states[] from the undo stacks (never the
    audio data), journal[] (since `sinceSeq`) when includeJournal. */
juce::var historyList (Application& app, FileDoc& d, bool includeJournal, juce::int64 sinceSeq);

/** "embedded" | "external" */
juce::String metaModeName (const FileDoc& d);

/** App version from the generated Version.h (not getApplicationVersion()). */
juce::String appVersion();

/** "windows" | "macos" | "linux" */
juce::String platformName();

/** This process id. */
int processId();

/** session.hello result: {app, version, pid, protocol, platform,
    capabilities{ai, audiocpp, python, ui}, commands, journalSeq}. */
juce::var helloInfo (Application& app, bool hasUi);

} // namespace sc::control
