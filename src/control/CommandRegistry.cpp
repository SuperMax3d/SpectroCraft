// SpectroCraft - AI control: command registry, validation, schema generation
// and the invoke() pipeline (see include/control/CommandRegistry.h).
#include "control/CommandRegistry.h"
#include "control/ControlState.h"
#include "control/UiBridge.h"
#include "Application.h"
#include "AlertGate.h"
#include "AudioCpp.h"
#include "WorkProgress.h"

#include <cmath>
#include <ctime>

namespace sc::control {

// ------------------------------------------------------------------ JSON helpers

juce::var obj (std::initializer_list<std::pair<juce::String, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& kv : props)
        o->setProperty (kv.first, kv.second);
    return juce::var (o);
}

void set (juce::var& v, const juce::Identifier& key, const juce::var& value)
{
    if (auto* o = v.getDynamicObject())
        o->setProperty (key, value);
}

juce::var arr (std::initializer_list<juce::var> items)
{
    juce::Array<juce::var> a;
    for (const auto& i : items) a.add (i);
    return a;
}

juce::var errorVar (const Error& e)
{
    auto v = obj ({ { "code", e.code }, { "message", e.message } });
    if (isObj (e.details)) set (v, "details", e.details);
    return v;
}

juce::String toLine (const juce::var& v)
{
    // Compact, one line, UTF-8 (no newline can appear: strings escape \n).
    return juce::JSON::toString (v, juce::JSON::FormatOptions{}.withSpacing (juce::JSON::Spacing::none));
}

juce::String isoNow()
{
    return isoTime (juce::Time::currentTimeMillis());
}

juce::String isoTime (juce::int64 ms)
{
    // UTC with milliseconds; JUCE's toISO8601 prints local time + offset, so
    // format from the UTC broken-down time ourselves.
    const std::time_t secs = (std::time_t) (ms / 1000);
    std::tm utc {};
#if JUCE_WINDOWS
    gmtime_s (&utc, &secs);
#else
    gmtime_r (&secs, &utc);
#endif
    return juce::String::formatted ("%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                                    utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                                    utc.tm_hour, utc.tm_min, utc.tm_sec, (int) (ms % 1000));
}

juce::StringArray flagNames (uint32_t f)
{
    juce::StringArray s;
    if (f & Mutates)     s.add ("Mutates");
    if (f & NeedsDoc)    s.add ("NeedsDoc");
    if (f & LongRunning) s.add ("LongRunning");
    if (f & NeedsUi)     s.add ("NeedsUi");
    if (f & Destructive) s.add ("Destructive");
    if (f & AiBuildOnly) s.add ("AiBuildOnly");
    if (f & NeedsAudioCpp) s.add ("NeedsAudioCpp");
    if (f & Internal)    s.add ("Internal");
    if (f & Query)       s.add ("Query");
    if (f & ActiveDocOnly) s.add ("ActiveDocOnly");
    return s;
}

// ------------------------------------------------------------------ Outcome / Context

Outcome Outcome::job (const juce::String& jobId)
{
    return success (obj ({ { "job", jobId }, { "state", "running" } }));
}

bool Outcome::isJob() const
{
    return ok && isObj (result) && result.hasProperty ("job")
           && result.getProperty ("state", {}).toString() == "running";
}

juce::String Outcome::jobId() const
{
    return result.getProperty ("job", {}).toString();
}

static bool isNum (const juce::var& v) { return v.isInt() || v.isInt64() || v.isDouble(); }

juce::var Context::get (const char* key) const { return params.getProperty (key, juce::var()); }
bool Context::has (const char* key) const { return isObj (params) && params.hasProperty (key); }
double Context::num (const char* key, double def) const
{
    const auto v = get (key);
    return isNum (v) ? (double) v : def;
}
int Context::integer (const char* key, int def) const
{
    const auto v = get (key);
    return isNum (v) ? (int) std::llround ((double) v) : def;
}
bool Context::flag (const char* key, bool def) const
{
    const auto v = get (key);
    return v.isBool() ? (bool) v : def;
}
juce::String Context::str (const char* key, const juce::String& def) const
{
    const auto v = get (key);
    return v.isString() ? v.toString() : def;
}

// ------------------------------------------------------------------ schema + validation

static juce::String typeName (PType t)
{
    switch (t)
    {
        case PType::Number:  return "number";
        case PType::Integer: return "integer";
        case PType::Boolean: return "boolean";
        case PType::String:  return "string";
        case PType::Array:   return "array";
        case PType::Object:  return "object";
        case PType::Range:   return "\"all\" or {start, end}";
        case PType::Any:     break;
    }
    return "any";
}

static juce::var schemaOf (const ParamSpec& p)
{
    auto s = obj();
    switch (p.type)
    {
        case PType::Number:
        case PType::Integer:
            set (s, "type", typeName (p.type));
            if (p.minimum) set (s, "minimum", *p.minimum);
            if (p.maximum) set (s, "maximum", *p.maximum);
            break;
        case PType::Boolean:
            set (s, "type", "boolean");
            break;
        case PType::String:
        {
            set (s, "type", "string");
            if (!p.enumValues.isEmpty())
            {
                juce::Array<juce::var> e;
                for (auto& x : p.enumValues) e.add (x);
                set (s, "enum", e);
            }
            break;
        }
        case PType::Array:
        {
            set (s, "type", "array");
            if (p.itemType != PType::Any)
            {
                ParamSpec it; it.type = p.itemType; it.minimum = p.itemMinimum;
                set (s, "items", schemaOf (it));
            }
            break;
        }
        case PType::Object:
            if (p.properties.empty())
                set (s, "type", "object");
            else
                s = CommandRegistry::schemaFor (p.properties);
            break;
        case PType::Range:
        {
            auto se = obj ({ { "type", "object" },
                             { "properties", obj ({ { "start", obj ({ { "type", "number" }, { "minimum", 0 } }) },
                                                    { "end",   obj ({ { "type", "number" }, { "minimum", 0 } }) } }) },
                             { "required", arr ({ "start", "end" }) },
                             { "additionalProperties", false } });
            set (s, "oneOf", arr ({ obj ({ { "type", "string" }, { "enum", arr ({ "all" }) } }), se }));
            break;
        }
        case PType::Any:
            break;
    }
    if (p.doc.isNotEmpty()) set (s, "description", p.doc);
    if (!p.defaultValue.isVoid()) set (s, "default", p.defaultValue);
    return s;
}

juce::var CommandRegistry::schemaFor (const std::vector<ParamSpec>& params)
{
    auto props = obj();
    juce::Array<juce::var> required;
    for (const auto& p : params)
    {
        set (props, p.name, schemaOf (p));
        if (p.required) required.add (p.name);
    }
    auto s = obj ({ { "type", "object" }, { "properties", props } });
    if (!required.isEmpty()) set (s, "required", required);
    set (s, "additionalProperties", false);
    return s;
}

static juce::String joinPath (const juce::String& prefix, const juce::String& name)
{
    return prefix.isEmpty() ? name : prefix + "." + name;
}

static bool fail (Error& e, const juce::String& path, const juce::String& expected, const juce::String& msg)
{
    e.code = err::invalidParams;
    e.message = msg;
    e.details = obj ({ { "path", path }, { "expected", expected } });
    return false;
}

static bool checkValue (const ParamSpec& p, const juce::var& v, Error& e, const juce::String& path)
{
    auto bounds = [&] (double x) {
        if (!std::isfinite (x))
            return fail (e, path, typeName (p.type), path + " must be a finite number");
        if (p.minimum && x < *p.minimum)
            return fail (e, path, ">= " + juce::String (*p.minimum), path + " must be >= " + juce::String (*p.minimum));
        if (p.maximum && x > *p.maximum)
            return fail (e, path, "<= " + juce::String (*p.maximum), path + " must be <= " + juce::String (*p.maximum));
        return true;
    };

    switch (p.type)
    {
        case PType::Number:
            if (!isNum (v)) return fail (e, path, "number", path + " must be a number");
            return bounds ((double) v);
        case PType::Integer:
        {
            if (!isNum (v)) return fail (e, path, "integer", path + " must be an integer");
            const double x = (double) v;
            if (std::floor (x) != x) return fail (e, path, "integer", path + " must be an integer");
            return bounds (x);
        }
        case PType::Boolean:
            if (!v.isBool()) return fail (e, path, "boolean", path + " must be true or false");
            return true;
        case PType::String:
            if (!v.isString()) return fail (e, path, "string", path + " must be a string");
            if (!p.enumValues.isEmpty() && !p.enumValues.contains (v.toString()))
                return fail (e, path, "one of [" + p.enumValues.joinIntoString (", ") + "]",
                             path + " must be one of [" + p.enumValues.joinIntoString (", ") + "]");
            return true;
        case PType::Array:
        {
            if (!v.isArray()) return fail (e, path, "array", path + " must be an array");
            if (p.itemType == PType::Any) return true;
            ParamSpec it; it.type = p.itemType; it.minimum = p.itemMinimum;
            for (int i = 0; i < v.size(); ++i)
                if (!checkValue (it, v[i], e, path + "[" + juce::String (i) + "]"))
                    return false;
            return true;
        }
        case PType::Object:
            if (!isObj (v)) return fail (e, path, "object", path + " must be an object");
            if (p.properties.empty()) return true;
            return CommandRegistry::validate (p.properties, v, e, path);
        case PType::Range:
        {
            if (v.isString())
            {
                if (v.toString() == "all") return true;
                return fail (e, path, typeName (p.type), path + " must be \"all\" or {start, end}");
            }
            if (!isObj (v))
                return fail (e, path, typeName (p.type), path + " must be \"all\" or {start, end}");
            std::vector<ParamSpec> se { P ("start", PType::Number).req().min (0),
                                        P ("end", PType::Number).req().min (0) };
            if (!CommandRegistry::validate (se, v, e, path)) return false;
            if ((double) v.getProperty ("end", 0) < (double) v.getProperty ("start", 0))
                return fail (e, path + ".end", ">= start", path + ".end must be >= start");
            return true;
        }
        case PType::Any:
            return true;
    }
    return true;
}

bool CommandRegistry::validate (const std::vector<ParamSpec>& params, const juce::var& value,
                                Error& e, const juce::String& prefix)
{
    if (!value.isVoid() && !isObj (value))
        return fail (e, prefix.isEmpty() ? "params" : prefix, "object", "params must be an object");

    if (auto* o = value.getDynamicObject())
        for (const auto& kv : o->getProperties())
        {
            const juce::String name = kv.name.toString();
            const ParamSpec* spec = nullptr;
            for (const auto& p : params)
                if (p.name == name) { spec = &p; break; }
            if (spec == nullptr)
            {
                // "confirm" is accepted by every command at the top level (the
                // MCP adapter adds it); handlers that need it read it.
                if (prefix.isEmpty() && name == "confirm" && kv.value.isBool())
                    continue;
                juce::StringArray names;
                for (const auto& p : params) names.add (p.name);
                return fail (e, joinPath (prefix, name),
                             names.isEmpty() ? juce::String ("no parameters")
                                             : "one of [" + names.joinIntoString (", ") + "]",
                             "unknown parameter \"" + joinPath (prefix, name) + "\"");
            }
            if (!checkValue (*spec, kv.value, e, joinPath (prefix, name)))
                return false;
        }

    for (const auto& p : params)
        if (p.required && !(isObj (value) && value.hasProperty (p.name)))
            return fail (e, joinPath (prefix, p.name), typeName (p.type),
                         "missing required parameter \"" + joinPath (prefix, p.name) + "\"");
    return true;
}

static juce::var minimalValue (const ParamSpec& p)
{
    switch (p.type)
    {
        case PType::Number:
        case PType::Integer:
        {
            double x = p.minimum ? *p.minimum : 0.0;
            if (p.maximum && x > *p.maximum) x = *p.maximum;
            if (p.type == PType::Integer) return (int) std::ceil (x);
            return x;
        }
        case PType::Boolean: return false;
        case PType::String:  return p.enumValues.isEmpty() ? juce::var ("x") : juce::var (p.enumValues[0]);
        case PType::Array:   return juce::Array<juce::var>();
        case PType::Object:  return CommandRegistry::minimalParams (p.properties);
        case PType::Range:   return "all";
        case PType::Any:     return juce::var();
    }
    return {};
}

juce::var CommandRegistry::minimalParams (const std::vector<ParamSpec>& params)
{
    auto o = obj();
    for (const auto& p : params)
        if (p.required) set (o, p.name, minimalValue (p));
    return o;
}

// ------------------------------------------------------------------ Request

bool Request::parse (const juce::var& v, Request& out, Error& e)
{
    if (!isObj (v))
        return fail (e, "", "object", "a request must be a JSON object");
    out.id = v.getProperty ("id", juce::var());
    const auto cmd = v.getProperty ("cmd", juce::var());
    if (!cmd.isString() || cmd.toString().isEmpty())
        return fail (e, "cmd", "string", "missing \"cmd\"");
    out.cmd = cmd.toString();
    out.params = v.getProperty ("params", juce::var());
    if (!out.params.isVoid() && !isObj (out.params))
        return fail (e, "params", "object", "\"params\" must be an object");
    out.doc = v.getProperty ("doc", juce::var());
    if (!out.doc.isVoid() && !isNum (out.doc) && !out.doc.isString())
        return fail (e, "doc", "integer document id or file path", "\"doc\" must be an id or a path");
    const auto t = v.getProperty ("timeoutMs", juce::var());
    if (!t.isVoid())
    {
        if (!isNum (t) || (double) t < 0)
            return fail (e, "timeoutMs", "integer >= 0", "\"timeoutMs\" must be >= 0");
        out.timeoutMs = (int) std::min (2147483647.0, (double) t);
    }
    return true;
}

// ------------------------------------------------------------------ registry

CommandRegistry& CommandRegistry::instance()
{
    static CommandRegistry r;
    return r;
}

CommandRegistry::CommandRegistry()
{
    registerCoreCommands (*this);
    registerHistoryCommands (*this);
    registerStepCommands (*this);
    registerJobCommands (*this);
    registerViewCommands (*this);
    registerTransportCommands (*this);
    registerAiCommands (*this);
    registerMeasureCommands (*this);
    registerEditCommands (*this);
    registerPythonCommands (*this);
}

void CommandRegistry::add (Spec s)
{
    jassert (find (s.id) == nullptr);
    specs.push_back (std::move (s));
}

const Spec* CommandRegistry::find (const juce::String& idOrAlias) const
{
    for (const auto& s : specs)
        if (s.id == idOrAlias) return &s;
    for (const auto& s : specs)
        if (s.aliases.contains (idOrAlias)) return &s;
    return nullptr;
}

juce::var CommandRegistry::describe (const juce::String& category, bool withSchemas,
                                     bool includeInternal) const
{
    juce::Array<juce::var> cmds;
    for (const auto& s : specs)
    {
        if (category.isNotEmpty() && s.category != category) continue;
        if ((s.flags & Internal) && !includeInternal && category != s.category) continue;
        juce::Array<juce::var> flags, aliases;
        for (auto& f : flagNames (s.flags)) flags.add (f);
        for (auto& a : s.aliases) aliases.add (a);
        auto c = obj ({ { "id", s.id }, { "aliases", aliases }, { "title", s.title },
                        { "category", s.category }, { "flags", flags } });
        if (withSchemas)
        {
            set (c, "params", schemaFor (s.params));
            set (c, "result", s.resultSchema.isVoid() ? obj ({ { "type", "object" } }) : s.resultSchema);
        }
        cmds.add (c);
    }
    return obj ({ { "protocol", kProtocolVersion }, { "commands", cmds } });
}

// ------------------------------------------------------------------ invoke

namespace {

int docIndexOf (Application& app, const FileDoc* d)
{
    for (int i = 0; i < (int) app.docs.size(); ++i)
        if (app.docs[(size_t) i].get() == d) return i;
    return -1;
}

FileDoc* findDoc (Application& app, const juce::var& ref)
{
    if (ref.isVoid()) return app.doc();
    if (isNum (ref))
    {
        const int id = (int) ref;
        for (auto& d : app.docs) if (d->id == id) return d.get();
        return nullptr;
    }
    const juce::String s = ref.toString();
    if (s.isEmpty()) return app.doc();
    if (!juce::File::isAbsolutePath (s)) return nullptr;
    const juce::File f (s);
    for (auto& d : app.docs)
        if (d->file == f || d->file.getFullPathName() == s) return d.get();
    return nullptr;
}

/** Makes `target` the active document for the duration of a handler without
    the view reset of Application::activate(); restores the previous one. */
class DocSwitch
{
public:
    DocSwitch (Application& a, FileDoc* target) : app (a), original (a.doc())
    {
        if (target == nullptr || target == original) return;
        const int idx = docIndexOf (app, target);
        if (idx < 0) return;
        app.syncActiveDoc();
        viewStart = app.viewStart; viewEnd = app.viewEnd;
        selectionRow = app.selectionRow;
        app.activeDocIndex = idx;
        app.selection = target->selection;
        app.playhead = target->playhead;
        app.visibleChannels = target->visibleChannels;
        app.selectionRow = -1;
        switched = true;
    }
    ~DocSwitch() { restore(); }

    void restore()
    {
        if (!switched) return;
        switched = false;
        app.syncActiveDoc();   // keep what the handler did to the target's copy
        const int idx = docIndexOf (app, original);
        if (idx < 0) return;   // the original was closed by the handler
        app.activeDocIndex = idx;
        app.selection = original->selection;
        app.playhead = original->playhead;
        app.visibleChannels = original->visibleChannels;
        app.selectionRow = selectionRow;
        app.viewStart = viewStart; app.viewEnd = viewEnd;
        app.spectralEngine.clearCache();   // tiles are keyed by sample position only
        if (app.onStateChange) app.onStateChange();
    }

    bool didSwitch() const { return switched; }

    /** Keep the target active for good (a job will work on it). */
    void makePermanent()
    {
        if (!switched) return;
        switched = false;
        app.syncActiveDoc();
        const int idx = app.activeDocIndex;
        app.activeDocIndex = docIndexOf (app, original);   // let activate() save + leave the original
        if (app.activeDocIndex >= 0)
        {
            app.selection = original->selection;
            app.playhead = original->playhead;
            app.visibleChannels = original->visibleChannels;
            app.viewStart = viewStart; app.viewEnd = viewEnd;
        }
        app.activate (idx);   // full switch: views, player source, onActiveDocChanged
    }

private:
    Application& app;
    FileDoc* original;
    double viewStart = 0, viewEnd = 0;
    int selectionRow = -1;
    bool switched = false;
};

/** Response envelope -> Outcome. */
Outcome outcomeOf (const juce::var& r)
{
    if ((bool) r.getProperty ("ok", false))
        return Outcome::success (r.getProperty ("result", {}));
    const auto e = r.getProperty ("error", {});
    return Outcome::failure (e.getProperty ("code", err::failed).toString(),
                             e.getProperty ("message", {}).toString(), e.getProperty ("details", {}));
}

juce::var sanitizedParams (const juce::var& params)
{
    if (!isObj (params)) return obj();
    auto copy = params.getDynamicObject()->clone();
    copy->removeProperty ("token");
    return juce::var (copy.release());
}

} // namespace

juce::var CommandRegistry::invoke (Application& app, const juce::String& cmd, const juce::var& params,
                                   const juce::var& doc, const InvokeOptions& opts)
{
    Request r;
    r.cmd = cmd;
    r.params = params;
    r.doc = doc;
    return invoke (app, r, opts);
}

juce::var CommandRegistry::invoke (Application& app, const Request& req, const InvokeOptions& opts)
{
    JUCE_ASSERT_MESSAGE_THREAD
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    ControlState& st = app.control();
    UiBridge* ui = opts.ui != nullptr ? opts.ui : st.ui;

    auto respondError = [&] (const juce::String& code, const juce::String& msg, const juce::var& details = {}) {
        auto r = obj ({ { "id", req.id }, { "ok", false }, { "error", errorVar ({ code, msg, details }) } });
        // Refusals are journaled too, so "what did the client try" is answerable.
        st.journal.append ("command", obj ({ { "cmd", req.cmd }, { "source", opts.source }, { "ok", false },
                                             { "error", code } }));
        return r;
    };

    // Edits made with the mouse since the last command land in the journal
    // first (their own history entries), then this command.
    st.pollDocuments();

    const Spec* spec = find (req.cmd);
    if (spec == nullptr)
        return respondError (err::unknownCmd, "unknown command \"" + req.cmd + "\"",
                             obj ({ { "cmd", req.cmd }, { "hint", "call app.describe" } }));

    const juce::var params = req.params.isVoid() ? obj() : req.params;
    if (toLine (params).getNumBytesAsUTF8() > 256 * 1024)
        return respondError (err::invalidParams, "params exceed 256 KiB",
                             obj ({ { "path", "params" }, { "expected", "<= 262144 bytes" } }));
    {
        Error e;
        if (!validate (spec->params, params, e))
            return respondError (e.code, spec->id + ": " + e.message, e.details);
    }

#if !(defined(SC_ENABLE_AI) && SC_ENABLE_AI)
    if (spec->flags & AiBuildOnly)
        return respondError (err::unsupportedBuild, spec->id + " needs an AI-enabled build (SPECTRO_ENABLE_AI)");
#endif
    if ((spec->flags & NeedsAudioCpp) && !sc::acpp::platformSupported())
        return respondError (err::unsupportedPlatform,
                             spec->id + ": audio.cpp publishes no CLI build for this platform",
                             obj ({ { "platform", juce::String (sc::acpp::platformTag()) },
                                    { "os", juce::SystemStats::getOperatingSystemName() } }));
    if ((spec->flags & NeedsUi) && ui == nullptr)
        return respondError (err::needsUi, spec->id + " needs the open application window",
                             obj ({ { "hint", "run it against the open app (--control), not --json" } }));

    FileDoc* target = findDoc (app, req.doc);
    if (!req.doc.isVoid() && target == nullptr)
        return respondError (err::unknownDoc, "no open document matches " + toLine (req.doc),
                             obj ({ { "doc", req.doc }, { "hint", "call doc.list" } }));
    if ((spec->flags & NeedsDoc) && (target == nullptr || !target->engine.isFileLoaded()))
        return respondError (err::noDocument, "no document is open",
                             obj ({ { "hint", "call doc.open first" } }));

    if ((spec->flags & ActiveDocOnly) && target != nullptr && target != app.doc())
        return respondError (err::invalidParams, spec->id + " acts on the active document only",
                             obj ({ { "path", "doc" }, { "expected", "the active document" },
                                    { "hint", "call doc.activate first" } }));

    if (!opts.insideJob && !opts.fromMenu)
    {
        // Anything that changes state waits while the window is inside a
        // modal loop or a background operation runs: JUCE delivers callAsync
        // inside modal loops, i.e. in the middle of a suspended menu handler
        // or next to a worker that still uses the active document.
        if (!(spec->flags & Query) && spec->category != "job")
        {
            if (juce::Component::getCurrentlyModalComponent() != nullptr)
                return respondError (err::busy, "a modal dialog is open in the application",
                                     obj ({ { "modalActive", true } }));
            if (sc::WorkProgressDialog::numActive() > 0)
                return respondError (err::busy, "a background operation is running in the application",
                                     obj ({ { "progressDialog", true } }));
        }
        // While a mutating job works on the active document, nothing but pure
        // reads and job control may change the app state under it (the job's
        // worker addresses "the active document").
        juce::String jobCmd;
        const juce::String running = st.jobs.runningMutating (&jobCmd);
        if (running.isNotEmpty() && !(spec->flags & Query) && spec->category != "job")
            return respondError (err::busy, "a mutating job is running",
                                 obj ({ { "job", running }, { "cmd", jobCmd } }));
    }

    // Document state before (envelope + journal).
    const size_t undoBefore = target != nullptr ? target->undo.undoSize() : 0;
    const size_t redoBefore = target != nullptr ? target->undo.redoSize() : 0;
    const juce::int64 contentBefore = target != nullptr ? (juce::int64) target->engine.contentVersion() : 0;
    const int targetId = target != nullptr ? target->id : -1;

    Outcome out;
    juce::StringArray alerts;
    bool docSwitched = false, activated = false;

    // REPORT §5.2 step 4: a mutating command on a streamed (not fully in
    // memory) document first loads it into memory on a worker - like
    // MainWindow::runDestructiveEdit - then runs on the message thread.
    // (history.* never: an undoable edit has already loaded the file.)
    const bool streamedDetour = (spec->flags & Mutates) && !opts.insideJob && target != nullptr
                                && spec->category != "history"
                                && target->engine.isFileLoaded() && !target->engine.fullyInMemory();
    if (streamedDetour)
    {
        Context jctx { app, st, ui, params, target, opts.source, spec->id };
        Application* appP = &app;
        FileDoc* doc = target;
        Request inner = req;
        if (inner.doc.isVoid()) inner.doc = target->id;   // the document being loaded, whatever is active then
        InvokeOptions io = opts;
        io.insideJob = true;
        const auto id = startJob (jctx, spec->id, "Loading " + target->name + " into memory", true,
            [appP, doc, inner, io] (JobControl& jc) -> Outcome {
                std::string e;
                if (!doc->engine.ensureEditable (&e, [&jc] (float f) { jc.progress (0.9 * f, "loading into memory"); },
                                                 jc.cancelFlag()))
                    return Outcome::failure (jc.cancelled() ? err::cancelled : err::failed, juce::String (e));
                juce::var r;
                if (!jc.onMessageThread ([&] {
                        r = appP->isDocLoaded (doc)
                                ? CommandRegistry::instance().invoke (*appP, inner, io)
                                : obj ({ { "ok", false }, { "error", errorVar ({ err::unknownDoc, "the document was closed", {} }) } });
                    }))
                    return Outcome::failure (err::shuttingDown, "the application is shutting down");
                Outcome o = outcomeOf (r);
                // The command itself became a job (e.g. a long step): follow it.
                if (o.isJob())
                {
                    const auto innerId = o.jobId();
                    auto& jobs = appP->control().jobs;
                    while (!jobs.isFinished (innerId))
                    {
                        if (jc.cancelled()) jobs.cancel (innerId);
                        juce::Thread::sleep (20);
                    }
                    if (auto io2 = jobs.outcome (innerId)) o = *io2;
                    else o = Outcome::failure (err::jobNotFound, "inner job " + innerId + " vanished");
                }
                if (o.ok && isObj (o.result)) set (o.result, "materialised", true);
                return o;
            });
        out = Outcome::job (id);
    }
    else
    {
        DocSwitch sw (app, (spec->flags & NoDocSwitch) ? nullptr : target);
        docSwitched = sw.didSwitch();
        sc::alert::ScopedCapture capture;
        // Patches recorded by the handler carry this command, its params and client.
        sc::undoctx::Scoped opScope (sc::undoctx::Op { spec->id, sanitizedParams (params), opts.source });
        Context ctx { app, st, ui, params, target, opts.source, spec->id };
        ++st.invokeDepth;
        const juce::String prevSource = st.invokeSource;
        st.invokeSource = opts.source;
        try
        {
            out = spec->handler (ctx);
        }
        catch (const std::exception& ex)
        {
            out = Outcome::failure (err::failed, juce::String ("exception: ") + ex.what());
        }
        catch (...)
        {
            out = Outcome::failure (err::failed, "unexpected exception");
        }
        st.invokeSource = prevSource;
        --st.invokeDepth;
        alerts = capture.take();
        if (ctx.keepDocSwitch && sw.didSwitch())
        {
            sw.makePermanent();
            activated = true;
        }
    }

    const bool targetAlive = target != nullptr && app.isDocLoaded (target);
    const double ms = juce::Time::getMillisecondCounterHiRes() - t0;

    // Journal: every command except successful pure reads (clients poll
    // app.state); metadata only, never the token.
    if (!(out.ok && (spec->flags & Query)))
    {
        auto e = obj ({ { "cmd", spec->id }, { "source", opts.source }, { "ok", out.ok },
                        { "durationMs", (int) ms }, { "params", sanitizedParams (params) } });
        if (targetId >= 0) set (e, "doc", targetId);
        if (!out.ok) set (e, "error", out.error.code);
        if (out.isJob()) set (e, "job", out.jobId());
        // New undo patches are journaled by pollDocuments() (below) as
        // "history" entries that carry this command id, its params and client.
        juce::ignoreUnused (contentBefore);
        st.journal.append ("command", e);
    }
    st.pollDocuments();   // the patches this command recorded, opened/closed docs, metadata

    if (out.ok && !out.isJob() && (spec->flags & Mutates) && ui != nullptr)
        ui->refreshAfterEdit();

    // doc.changed: documents opened/closed/activated/saved, selection or view moved.
    if (out.ok && !(spec->flags & Query)
        && (spec->category == "doc" || spec->category == "selection" || spec->category == "view"
            || spec->category == "markers"))
    {
        auto data = obj ({ { "what", spec->id }, { "source", opts.source } });
        if (targetId >= 0) set (data, "doc", targetId);
        if (FileDoc* a = app.doc()) set (data, "active", a->id);
        st.emit ("doc.changed", data);
    }

    auto r = obj ({ { "id", req.id }, { "ok", out.ok } });
    if (out.ok) set (r, "result", out.result.isVoid() ? obj() : out.result);
    else        set (r, "error", errorVar (out.error));
    juce::Array<juce::var> al;
    for (auto& a : alerts) al.add (a);
    set (r, "alerts", al);
    if ((spec->flags & Mutates) && targetAlive)
    {
        const size_t undoAfter = target->undo.undoSize();
        const size_t redoAfter = target->undo.redoSize();
        set (r, "undoDepth", (int) undoAfter);
        set (r, "redoDepth", (int) redoAfter);
        const bool historyCmd = spec->category == "history";
        set (r, "redoDiscarded", (!historyCmd && redoAfter < redoBefore) ? (int) (redoBefore - redoAfter) : 0);
        set (r, "contentVersion", (juce::int64) target->engine.contentVersion());
        if (target->undo.canUndo()) set (r, "patchSeq", (juce::int64) target->undo.lastRecorded().seq);
        set (r, "unsaved", app.docIsUnsaved (*target));
        juce::ignoreUnused (undoBefore);
    }
    if (spec->flags & Mutates) set (r, "journalSeq", st.journal.lastSeq());
    if (docSwitched) set (r, "docSwitched", true);
    if (activated)   set (r, "activated", true);
    set (r, "durationMs", (int) ms);
    return r;
}

} // namespace sc::control
