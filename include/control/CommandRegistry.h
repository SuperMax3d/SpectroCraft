#pragma once
// SpectroCraft - AI control: the single command registry.
//
// Every externally drivable feature is one Spec { id, params, flags, handler }
// registered here. Four facades consume it: the loopback control server
// (open app), the --json CLI mode (headless), action replay and, later, the
// menus. See docs/ai-control/RAPPORT.md §5-§6 for the protocol.
//
// Threading: invoke() and every handler run on the JUCE message thread.
// Long work goes to a job (JobManager) that commits back to the message thread.
#include <JuceHeader.h>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

class Application;
class FileDoc;

namespace sc::control {

class ControlState;
class UiBridge;

/** Protocol version announced by session.hello / app.describe (experimental). */
constexpr int kProtocolVersion = 1;

/** Command flags (Spec::flags). */
enum Flag : uint32_t
{
    None        = 0,
    Mutates     = 1u << 0,   // changes document content: envelope gets undo/redo/content state
    NeedsDoc    = 1u << 1,   // needs a loaded document (doc field or the active one)
    LongRunning = 1u << 2,   // may return {"job": ...} instead of a result
    NeedsUi     = 1u << 3,   // needs the open app's window (MainWindow): needs_ui in the CLI
    Destructive = 1u << 4,   // may lose data: the handler asks for params.confirm
    AiBuildOnly = 1u << 5,   // unsupported_build without SPECTRO_ENABLE_AI
    WindowsOnly = 1u << 6,   // unsupported_platform elsewhere
    NoDocSwitch = 1u << 7,   // the handler addresses ctx.doc itself: never switch the active doc
    Internal    = 1u << 8,   // diagnostics/test helpers (category "debug")
    Query       = 1u << 9,   // pure read: allowed while a mutating job runs, not journaled on success
    ActiveDocOnly = 1u << 10 // acts on what the window shows (player, views): doc must be the active one
};

juce::StringArray flagNames (uint32_t flags);

/** Parameter value types of the restricted JSON-Schema subset. */
enum class PType { Number, Integer, Boolean, String, Array, Object, Any, Range };

/** One declared parameter. The same table drives validation (validate())
    and the generated JSON Schema (schemaFor()): there is no second list. */
struct ParamSpec
{
    juce::String name;
    PType type = PType::Any;
    bool required = false;
    std::optional<double> minimum, maximum;
    juce::StringArray enumValues;          // String only
    juce::String doc;
    PType itemType = PType::Any;           // Array only
    std::optional<double> itemMinimum;     // Array of numbers/integers only
    std::vector<ParamSpec> properties;     // Object only (closed when non-empty)
    juce::var defaultValue;                // documentation only

    // Builder helpers: P("db", PType::Number).req().range(-96, 96).describe("Gain in dB")
    ParamSpec& req() { required = true; return *this; }
    ParamSpec& range (double lo, double hi) { minimum = lo; maximum = hi; return *this; }
    ParamSpec& min (double lo) { minimum = lo; return *this; }
    ParamSpec& oneOf (const juce::StringArray& e) { enumValues = e; return *this; }
    ParamSpec& describe (const juce::String& d) { doc = d; return *this; }
    ParamSpec& items (PType t, std::optional<double> lo = {}) { itemType = t; itemMinimum = lo; return *this; }
    ParamSpec& props (std::vector<ParamSpec> p) { properties = std::move (p); return *this; }
    ParamSpec& def (const juce::var& v) { defaultValue = v; return *this; }
};

inline ParamSpec P (const juce::String& name, PType type) { ParamSpec s; s.name = name; s.type = type; return s; }

/** Closed error code set (RAPPORT §6.1). */
namespace err {
constexpr const char* unauthorized        = "unauthorized";
constexpr const char* unknownCmd          = "unknown_cmd";
constexpr const char* invalidParams       = "invalid_params";
constexpr const char* noDocument          = "no_document";
constexpr const char* unknownDoc          = "unknown_doc";
constexpr const char* busy                = "busy";
constexpr const char* timeout             = "timeout";
constexpr const char* cancelled           = "cancelled";
constexpr const char* confirmRequired     = "confirm_required";
constexpr const char* needsUi             = "needs_ui";
constexpr const char* unsupportedBuild    = "unsupported_build";
constexpr const char* unsupportedPlatform = "unsupported_platform";
constexpr const char* modelNotInstalled   = "model_not_installed";
constexpr const char* jobNotFound         = "job_not_found";
constexpr const char* shuttingDown        = "shutting_down";
constexpr const char* failed              = "failed";
}

struct Error
{
    juce::String code, message;
    juce::var details;   // object or void
};

/** Result of a handler (and of a finished job). */
struct Outcome
{
    bool ok = true;
    juce::var result;    // object (or void = {})
    Error error;

    static Outcome success (const juce::var& r = {}) { Outcome o; o.result = r; return o; }
    static Outcome failure (const juce::String& code, const juce::String& message,
                            const juce::var& details = {})
    {
        Outcome o; o.ok = false; o.error = { code, message, details }; return o;
    }
    /** Handler started a job: the response is {"job": id, "state": "running"}. */
    static Outcome job (const juce::String& jobId);
    bool isJob() const;
    juce::String jobId() const;
};

/** Everything a handler may use. */
struct Context
{
    Application& app;
    ControlState& state;
    UiBridge* ui = nullptr;           // nullptr in the CLI and in tests
    const juce::var& params;          // validated params object
    FileDoc* doc = nullptr;           // resolved target (active document during the handler)
    juce::String source;              // "cli", "api:<client>", "menu", "test"
    juce::String cmd;                 // the command id being run (jobs and journal entries use it)
    /** Set by a handler that started a job on a non-active target: the
        document stays active afterwards (the job works on the active one). */
    bool keepDocSwitch = false;

    juce::var get (const char* key) const;
    bool has (const char* key) const;
    double num (const char* key, double def) const;
    int integer (const char* key, int def) const;
    bool flag (const char* key, bool def) const;
    juce::String str (const char* key, const juce::String& def = {}) const;
    bool confirmed() const { return flag ("confirm", false); }
};

using Handler = std::function<Outcome (Context&)>;

struct Spec
{
    juce::String id;                 // "domain.command"
    juce::String title;
    juce::String category;           // domain ("doc", "history", "step", ...)
    std::vector<ParamSpec> params;
    juce::var resultSchema;          // optional (documentation)
    uint32_t flags = None;
    juce::StringArray aliases;
    Handler handler;
};

/** One NDJSON request line, parsed. */
struct Request
{
    juce::var id;                    // echoed back (string or number)
    juce::String cmd;
    juce::var params;                // object (void -> {})
    juce::var doc;                   // void = active; int = FileDoc::id; string = path
    int timeoutMs = 30000;           // 0 = unlimited (CLI jobs only)

    /** Parses a request object. False + err (invalid_params) when malformed. */
    static bool parse (const juce::var& v, Request& out, Error& errOut);
};

/** Who is calling (journal source, alerts, UI). */
struct InvokeOptions
{
    UiBridge* ui = nullptr;
    juce::String source = "cli";
    bool insideJob = false;          // a job worker commits a nested call: skip busy checks
};

class CommandRegistry
{
public:
    /** The process-wide registry, populated with every built-in command. */
    static CommandRegistry& instance();

    void add (Spec s);
    const Spec* find (const juce::String& idOrAlias) const;
    const std::vector<Spec>& all() const { return specs; }

    /** {protocol, commands:[{id, aliases, title, category, flags, params, result}]} */
    juce::var describe (const juce::String& category = {}, bool withSchemas = true,
                        bool includeInternal = false) const;

    /** Restricted JSON Schema (draft 2020-12) for a parameter table. */
    static juce::var schemaFor (const std::vector<ParamSpec>& params);

    /** Validates params against the table (additionalProperties:false).
        On failure fills err (code invalid_params, details.path/expected). */
    static bool validate (const std::vector<ParamSpec>& params, const juce::var& value,
                          Error& err, const juce::String& pathPrefix = {});

    /** A minimal params object that satisfies every required entry (tests). */
    static juce::var minimalParams (const std::vector<ParamSpec>& params);

    /** Runs one request on the message thread and returns the full response
        envelope ({id, ok, result|error, alerts, ...}). Never throws. */
    juce::var invoke (Application& app, const Request& req, const InvokeOptions& opts = {});

    /** Convenience for in-process callers/tests. */
    juce::var invoke (Application& app, const juce::String& cmd, const juce::var& params = {},
                      const juce::var& doc = {}, const InvokeOptions& opts = {});

private:
    CommandRegistry();
    std::vector<Spec> specs;
};

// ---- JSON helpers --------------------------------------------------------------
/** Object builder: obj({{"a", 1}, {"b", "x"}}). */
juce::var obj (std::initializer_list<std::pair<juce::String, juce::var>> props = {});
/** Sets a property on an object var (no-op when v is not an object). */
void set (juce::var& v, const juce::Identifier& key, const juce::var& value);
juce::var arr (std::initializer_list<juce::var> items = {});
juce::var errorVar (const Error& e);
/** Single-line JSON (NDJSON). */
juce::String toLine (const juce::var& v);
/** UTC ISO-8601 with milliseconds: 2026-11-05T14:21:09.002Z */
juce::String isoNow();
juce::String isoTime (juce::int64 msSinceEpoch);

// Built-in command groups (src/control/commands/*.cpp).
void registerCoreCommands (CommandRegistry&);
void registerHistoryCommands (CommandRegistry&);
void registerStepCommands (CommandRegistry&);
void registerJobCommands (CommandRegistry&);
void registerViewCommands (CommandRegistry&);
void registerTransportCommands (CommandRegistry&);
void registerAiCommands (CommandRegistry&);

} // namespace sc::control
