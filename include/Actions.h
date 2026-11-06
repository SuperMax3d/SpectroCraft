// SpectroCraft - recorded actions (Photoshop-style record/replay).
#pragma once

#include <JuceHeader.h>

#include <string>
#include <utility>
#include <initializer_list>
#include <vector>

class Application;   // forward decl for the headless apply + batch runner

namespace sc {

/** One recorded operation inside an action.
    `type` selects the replay dispatcher (see kKnownActionTypes below);
    `params` is a JSON-serialisable object holding the values that were in
    use when the step was recorded (or edited later). When `prompt` is set,
    interactive replay shows the normal dialog for this operation instead of
    using the stored values; headless/batch replay always uses the stored
    values (there is no UI to prompt through). */
struct ActionStep
{
    juce::String type;
    juce::var params = juce::var();   // JSON object (or null)
    bool prompt = false;

    juce::var toJson() const;
    /** Parses one step object. On failure returns a step with an empty type
        and fills errOut with a human-readable reason. */
    static ActionStep fromJson (const juce::var& v, std::string* errOut = nullptr);
};

/** A named sequence of steps. */
struct Action
{
    juce::String name;
    std::vector<ActionStep> steps;

    /** Serialises this action as a JSON object {name, steps}. */
    juce::var toJson() const;
};

/** The set of user actions. Persisted as a JSON array in the app's recent-
    files store (key "actions_json") so it survives application restarts. */
class ActionStore
{
public:
    /** Adds an empty action. Returns its index, or -1 when the name is empty
        or already used. */
    int add (const juce::String& name);

    bool rename (int index, const juce::String& newName);
    void removeSelected (const std::vector<int>& indices);

    bool validIndex (int index) const { return valid (index); }

    Action* get (int index) { return valid (index) ? &actions_[(size_t) index] : nullptr; }
    const Action* get (int index) const { return valid (index) ? &actions_[(size_t) index] : nullptr; }
    int size() const { return (int) actions_.size(); }
    bool empty() const { return actions_.empty(); }

    /** Serialises the whole store to a JSON array string. */
    juce::String toJson() const;
    /** Replaces the contents with the parsed JSON array. Invalid input is
        ignored (the current contents are kept) and errOut, when set, gets a
        reason. Returns true on success. */
    bool loadFromJson (const juce::String& json, std::string* errOut = nullptr);

private:
    bool valid (int i) const { return i >= 0 && i < (int) actions_.size(); }
    std::vector<Action> actions_;
};

/** The step types the replay dispatcher understands. */
namespace actionTypes {
constexpr char kSilence[]        = "silence";
constexpr char kFadeIn[]         = "fadeIn";
constexpr char kFadeOut[]        = "fadeOut";
constexpr char kNormalize[]      = "normalize";
constexpr char kChangeGain[]     = "changeGain";
constexpr char kReverse[]        = "reverse";
constexpr char kInvert[]         = "invert";
constexpr char kTimeStretch[]    = "timeStretch";
constexpr char kCatTimeScale[]   = "catTimeScale";
constexpr char kFilterEq[]       = "filterEq";
constexpr char kEnvelope[]       = "envelope";
constexpr char kGenSilence[]     = "genSilence";
constexpr char kGenNoise[]       = "genNoise";
constexpr char kGenTone[]        = "genTone";
constexpr char kResample[]       = "resample";
constexpr char kTranscribe[]     = "transcribe";
constexpr char kTrim[]           = "trim";
constexpr char kDelete[]         = "delete";
constexpr char kPython[]         = "python";
constexpr char kNoiseSuppress[]  = "noiseSuppress";   // AI (needs an AI-enabled build)
} // namespace actionTypes

/** Builds a JSON object var from key/value pairs (action step params).
    The vendored JUCE's var has no setProperty(); this wraps a DynamicObject. */
juce::var makeVarObject (std::initializer_list<std::pair<juce::String, juce::var>> props);

/** True when the var holds an int/int64/double value.
    (This JUCE build has no isNumber().) */
inline bool isNumericVar (const juce::var& v) noexcept
{
    return v.isInt () || v.isInt64 () || v.isDouble ();
}

/** Pretty-prints a JSON value with 2-space indentation (for the action
    edit dialog). */
juce::String jsonPrettyPrint (const juce::var& v);

/** Splits "*.wav; *.mp3" into lower-case extensions {".wav", ".mp3"}. */
std::vector<juce::String> parseFormatList (const juce::String& text);

/** True when `type` is one of the known step types. */
bool isKnownActionType (const juce::String& type);

/** Validates a single step's params for its type. Returns true when usable;
    otherwise fills errOut with a description (used by the Edit dialog). */
bool validateStepParams (const ActionStep& s, std::string* errOut);

/** Applies one step to `app`'s ACTIVE document synchronously (call on the
    message thread for interactive use or from a worker thread in batch mode).
    Selection-based steps act on app.selection; whole-file-capable steps fall
    back to the entire file when nothing is selected. Returns false and fills
    errOut on failure. */
bool applyActionStep (Application& app, const ActionStep& s, std::string* errOut,
                      std::function<bool(float)> onProgress = nullptr);

/** Batch runner: applies `action` to every file in `inputFolder` whose
    extension matches one of `formats` (e.g. {".wav", ".mp3"}), saving each
    result into `outputFolder` with the name produced by `renamePattern`
    ("%Filename%" = original file name without extension). When
    includeSubfolders is set, subdirectories are scanned recursively and the
    relative folder structure is recreated inside outputFolder.

    BLOCKING - call from a worker thread. onProgress reports overall 0..1;
    onStatus (message thread) gets one line per finished file; cancelOut, when
    set, stops after the current file. summaryOut receives "done/failed/skipped"
    counts as a human-readable string. Returns false only for setup failures
    (no input folder / no matching files). */
bool runActionBatch (const Action& action,
                     const juce::File& inputFolder,
                     const juce::File& outputFolder,
                     const std::vector<juce::String>& formats,
                     const juce::String& renamePattern,
                     bool includeSubfolders,
                     const juce::File& tempStorePath,
                     std::function<bool(float)> onProgress = nullptr,
                     std::function<void(const juce::String&)> onStatus = nullptr,
                     const std::atomic<bool>* cancelOut = nullptr,
                     std::string* summaryOut = nullptr);

} // namespace sc
