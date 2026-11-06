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

/** Paths of the temp files exchanged with one script run (see makeExchange). */
struct ScriptExchange
{
    juce::File inWav;        // argv[1]: float32 WAV of the range
    juce::File outWav;       // argv[2]: OPTIONAL output WAV (replaces the range)
    juce::File metaJson;     // argv[3]: context JSON (buildMetaJson)
    juce::File resultJson;   // OPTIONAL result.json (markers / text / measures / tables)
    double inputStartSec = 0.0;   // document time of in.wav frame 0 (result.json timeBase "input")

    /** Deletes the four files. */
    void cleanup() const;
};

/** in_<stamp>.wav, out_<stamp>.wav, meta_<stamp>.json, result_<stamp>.json
    inside `dir` (created on demand); stale files of those names are removed. */
ScriptExchange makeExchange (const juce::File& dir, const juce::String& stamp,
                             double inputStartSec = 0.0);

/** Builds the metadata JSON handed to scripts: sample rate, channel count,
    file duration, selection range (seconds), whether the whole file was used,
    the source file name and every marker of the active document. When
    `exchange` is given it also carries "inputWav", "outputWav", "resultJson"
    (paths) and "resultContract" (the result.json schema, see resultContract()). */
juce::String buildMetaJson (double sampleRate, int numChannels, double fileDurationSec,
                            juce::Range<double> selectionSecs, bool wholeFile,
                            const juce::String& fileName,
                            const std::vector<Marker>& markers,
                            const ScriptExchange* exchange = nullptr);

/** Writes `data` (one vector per channel) as a 32-bit float WAV. */
bool writeFloatWav (const juce::File& file, const std::vector<std::vector<float>>& data,
                    double sampleRate);

/** Reads a 32-bit float WAV written by writeFloatWav (or any float WAV). */
bool readFloatWav (const juce::File& file, double* rateOut, int* nchOut,
                   std::vector<std::vector<float>>& out);

// ============================================================================
// Process execution
// ============================================================================

/** Options for runProcessEx. The defaults are what runProcess() uses. */
struct RunOptions
{
    /** Wall-clock limit in milliseconds; 0 = none. On expiry the whole process
        tree is killed (Windows: job object; POSIX: process group, SIGTERM then
        SIGKILL after killGraceMs) and ProcessResult::timedOut is set. */
    int timeoutMs = 0;
    int killGraceMs = 1000;
    /** PYTHONUTF8=1 and PYTHONIOENCODING=utf-8 in the child environment, so a
        Python child writes UTF-8 on its pipes even with a non-UTF-8 ANSI code
        page (Windows). Harmless for any other executable. */
    bool pythonUtf8 = true;
    /** Kill whatever the child left running in its process tree (grandchildren
        still alive when it exits). Windows: JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
        which also covers SpectroCraft crashing mid-run. */
    bool killTreeOnExit = true;
    /** Called on the running thread with each complete stdout line (without
        its "\n" / "\r\n", empty lines skipped) while the child runs, then with
        a final unterminated line, if any, after it exits. The full output is
        still returned in ProcessResult::stdoutText. */
    std::function<void (const juce::String&)> onStdoutLine;
};

struct ProcessResult
{
    bool started = false;     // the executable was found and launched
    bool cancelled = false;   // shouldContinue() returned false: tree killed
    bool timedOut = false;    // RunOptions::timeoutMs elapsed: tree killed
    int exitCode = -1;        // -1 when killed or ended by a signal
    juce::String stdoutText;  // see decodeProcessOutput
    juce::String stderrText;
    double elapsedMs = 0.0;
};

/** Runs the executable `cmd` (a bare name looked up on PATH, or a path; use
    splitCommand() for a command with flags) with `args`, blocking until it
    exits. stdin is the null device (a child reading stdin gets EOF at once
    instead of hanging on, or stealing, the parent's console); stdout/stderr
    are drained while it runs (a verbose child never blocks on a full pipe).
    shouldContinue() is polled every 200 ms: false kills the process tree.
    Paths with spaces and non-ASCII characters are passed intact (UTF-16
    command line with MSVCRT quoting on Windows, UTF-8 argv elsewhere). */
ProcessResult runProcessEx (const std::string& cmd, const juce::StringArray& args,
                            const RunOptions& options,
                            const std::function<bool ()>& shouldContinue = nullptr);

/** Legacy wrapper: runProcessEx with default options (no timeout). Returns
    false when the executable could not be started or was killed by
    cancellation; *exitCodeOut receives the exit code and the output is
    appended to outStdout/outStderr (when non-null). */
bool runProcess (const std::string& cmd, const juce::StringArray& args,
                 const std::function<bool ()>& shouldContinue,
                 int* exitCodeOut = nullptr,
                 juce::String* outStdout = nullptr,
                 juce::String* outStderr = nullptr);

/** Decodes raw child output: UTF-8 when valid, otherwise the ANSI code page
    (Windows) or Latin-1 (elsewhere), so invalid bytes never assert or vanish. */
juce::String decodeProcessOutput (const char* data, size_t size);

// ============================================================================
// Script identity and location
// ============================================================================

/** Lowercase hex SHA-256 of the script file's bytes as they are on disk: what
    the interpreter actually runs (JUCE's replaceWithText writes CRLF line
    endings, so this is not the hash of the editor text). Empty when the file
    cannot be read. */
juce::String scriptSha256 (const juce::File& script);

/** Allow-list check for scripts launched by an external client: the script
    must be an existing *.py file inside `folder` (default scriptFolder()),
    after symbolic links are resolved. On refusal *reasonOut receives a stable
    code the caller can turn into confirm_required{reason}: "not_found",
    "not_python" or "outside_script_folder" (also when a link leads outside).
    On success *resolvedOut receives the link-free path: run and hash THAT
    file, so the check and the run see the same target. */
bool isUnderScriptFolder (const juce::File& script, juce::String* reasonOut = nullptr,
                          const juce::File& folder = scriptFolder(),
                          juce::File* resolvedOut = nullptr);

// ============================================================================
// result.json: optional structured output of a script (contract version 1)
// ============================================================================

/** One marker returned by a script. time/end are in the result's timeBase;
    docTime/docEnd are document seconds. end < 0 = point marker. */
struct ScriptMarker
{
    double time = 0.0;
    double end = -1.0;
    juce::String label;
    double docTime = 0.0;
    double docEnd = -1.0;
};

struct ScriptTable
{
    juce::String name;
    juce::StringArray columns;
    juce::Array<juce::var> rows;   // each row: an array of columns.size() values
};

struct ScriptResult
{
    bool present = false;              // result.json existed
    juce::String error;                // non-empty: present but invalid (nothing else is set)
    juce::String timeBase = "input";   // "input" (seconds from in.wav start) or "document"
    std::vector<ScriptMarker> markers;
    juce::String text;
    juce::var measures;                // free-form JSON object (void when absent)
    std::vector<ScriptTable> tables;
    juce::var raw;                     // the whole parsed object (pass-through to API clients)
};

/** Parses a result.json document (see resultContractText). inputStartSec is
    the document time of in.wav frame 0, used for timeBase "input". The
    returned result has present = true. */
ScriptResult parseScriptResult (const juce::String& json, double inputStartSec = 0.0);

/** Reads `file` when it exists (present = false otherwise). Files over
    16 MiB are rejected as invalid. */
ScriptResult readScriptResult (const juce::File& file, double inputStartSec = 0.0);

/** The result.json contract: as a JSON object (embedded in meta.json as
    "resultContract") and as human-readable help text. */
juce::var resultContract();
const char* resultContractText();

// ============================================================================
// One script run (process + outputs), without touching any document
// ============================================================================

struct ScriptRunReport
{
    juce::File script;
    juce::String scriptSha256;     // of the file that ran (journal field for python.run)
    juce::String interpreter;      // the pythonCmd used
    ProcessResult process;
    bool outWavPresent = false;    // the script wrote out.wav
    ScriptResult result;           // parsed result.json (present = false when absent)
    bool audioChanged = false;     // set by the caller once out.wav was committed
};

/** Runs `pythonCmd script in.wav out.wav meta.json` (pythonCmd may carry
    flags, e.g. "py -3") with `options`, then checks for out.wav and parses
    result.json. Never touches a document: the caller commits out.wav. */
ScriptRunReport runScript (const std::string& pythonCmd, const juce::File& script,
                           const ScriptExchange& exchange, const RunOptions& options,
                           const std::function<bool ()>& shouldContinue = nullptr);

/** Thread-local hook around Application::runPythonScript (whose signature
    stays unchanged): while one of these lives on the calling thread, the run
    uses its options (timeout...) and fills its report (hash, exit code,
    output, result.json). Nesting restores the previous capture. Meant for the
    control API's python.run job; the GUI runs without one (no timeout). */
class ScopedScriptRunCapture
{
public:
    explicit ScopedScriptRunCapture (const RunOptions& options = {});
    ~ScopedScriptRunCapture();

    const RunOptions& options() const { return options_; }
    const ScriptRunReport& report() const { return report_; }
    ScriptRunReport& report() { return report_; }
    bool ran() const { return ran_; }   // a script process was launched under this capture
    void setReport (const ScriptRunReport& r) { report_ = r; ran_ = true; }

    /** The innermost capture alive on the calling thread, or nullptr. */
    static ScopedScriptRunCapture* current();

private:
    RunOptions options_;
    ScriptRunReport report_;
    bool ran_ = false;
    ScopedScriptRunCapture* previous_ = nullptr;

    JUCE_DECLARE_NON_COPYABLE (ScopedScriptRunCapture)
};

/** The text shown when no Python interpreter is found. */
const char* installHelpText();

}}   // namespace sc::py
