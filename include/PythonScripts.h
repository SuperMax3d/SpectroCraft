#pragma once
// SpectroCraft - Python script panel support (script folder management,
// interpreter probing, temp-file data exchange with user scripts).
#include <JuceHeader.h>
#include <vector>
#include <functional>

struct Marker;   // MarkerManager.h

namespace sc { namespace py {

/** The per-user script folder: %APPDATA%/SpectroCraft/python (or the
    equivalent on other platforms). Created on demand. */
juce::File scriptFolder();

/** Creates `dir` if needed and writes the six example scripts
    ("Dynamic processing.py", "Threshold.py", "Modulation.py",
    "Add Echo.py", "Sync And Merge.py", "Make Loop.py") into it -
    but only when a file does not exist yet, so user edits are never clobbered. */
void ensureExampleScripts (const juce::File& dir);

/** The original text of a built-in example script, looked up by file name
    ("Dynamic processing.py", "Threshold.py", ...); empty for user scripts. */
juce::String defaultScriptText (const juce::String& fileName);

/** All *.py files in `dir`, sorted by name. */
std::vector<juce::File> listScripts (const juce::File& dir = scriptFolder());

/** The template body written into scripts created via the panel's Add button. */
/** Returns the boilerplate written by the panel's Add button. */
juce::String newScriptTemplate ();

/** Probes PATH for a Python interpreter ("python", "python3" or "py -3").
    On success sets *cmdOut to the executable plus any extra arguments and
    returns true. */
bool findPython (std::string* cmdOut, std::string* errOut = nullptr);

/** Splits an interpreter command such as "py -3" (as returned by findPython)
    into the executable and its extra arguments. Double-quoted tokens may
    contain spaces (e.g. "\"C:/Program Files/Python/python.exe\" -u"). */
juce::StringArray splitCommand (const std::string& cmd);

/** Builds the metadata JSON handed to scripts: sample rate, channel count,
    file duration, selection range (seconds), whether the whole file was used,
    the source file name and every marker of the active document. */
juce::String buildMetaJson (double sampleRate, int numChannels, double fileDurationSec,
                            juce::Range<double> selectionSecs, bool wholeFile,
                            const juce::String& fileName,
                            const std::vector<Marker>& markers);

/** Writes `data` (one vector per channel) as a 32-bit float WAV. */
bool writeFloatWav (const juce::File& file, const std::vector<std::vector<float>>& data,
                    double sampleRate);

/** Reads a 32-bit float WAV written by writeFloatWav (or any float WAV). */
bool readFloatWav (const juce::File& file, double* rateOut, int* nchOut,
                   std::vector<std::vector<float>>& out);

/** Runs the executable `cmd` (e.g. "python" or "py"; use splitCommand() for a
    command with flags) with `args`. Blocks until the process exits;
    shouldContinue() is polled every 200 ms and a false return kills the
    process. stdout/stderr are read while the process runs (so a verbose child
    never blocks on a full pipe) and appended, decoded as UTF-8, to
    outStdout/outStderr (when non-null); on success *exitCodeOut receives the
    exit code. Returns false when the
    executable could not be started or was killed by cancellation. */
bool runProcess (const std::string& cmd, const juce::StringArray& args,
                 const std::function<bool ()>& shouldContinue,
                 int* exitCodeOut = nullptr,
                 juce::String* outStdout = nullptr,
                 juce::String* outStderr = nullptr);

/** The text shown when no Python interpreter is found. */
const char* installHelpText();

}}   // namespace sc::py
