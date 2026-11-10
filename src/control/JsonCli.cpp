// SpectroCraft - AI control: headless JSON mode (see include/control/JsonCli.h).
#include "control/JsonCli.h"
#include "control/CommandRegistry.h"
#include "control/ControlState.h"
#include "control/StateJson.h"
#include "Application.h"
#include "AlertGate.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>

namespace sc::control {

namespace {

juce::var lineError (const juce::var& id, const juce::String& code, const juce::String& msg,
                     const juce::var& details = {})
{
    return obj ({ { "id", id }, { "ok", false }, { "error", errorVar ({ code, msg, details }) } });
}

/** Pumps the message loop until the job finished or the timeout elapsed;
    then builds the inlined response. */
juce::var waitForJob (Application& app, const Request& req, const juce::var& startResponse,
                      const juce::String& jobId)
{
    auto& jobs = app.control().jobs;
    auto* mm = juce::MessageManager::getInstance();
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    bool timedOut = false;
    while (!jobs.isFinished (jobId))
    {
        mm->runDispatchLoopUntil (5);
        if (req.timeoutMs > 0 && juce::Time::getMillisecondCounterHiRes() - t0 > req.timeoutMs)
        {
            timedOut = true;
            jobs.cancel (jobId);
            // Give the job a bounded time to notice the cancel and stop.
            const double t1 = juce::Time::getMillisecondCounterHiRes();
            while (!jobs.isFinished (jobId) && juce::Time::getMillisecondCounterHiRes() - t1 < 15000.0)
                mm->runDispatchLoopUntil (5);
            break;
        }
    }
    // Deliver the job's own completion callbacks (journal entry): the worker
    // posts them right after marking the job finished.
    mm->runDispatchLoopUntil (20);

    if (timedOut)
        return lineError (req.id, err::timeout,
                          "job " + jobId + " did not finish within " + juce::String (req.timeoutMs) + " ms (cancelled)",
                          obj ({ { "job", jobId }, { "cancelled", true },
                                 { "finished", jobs.isFinished (jobId) } }));

    const auto o = jobs.outcome (jobId);
    auto r = obj ({ { "id", req.id } });
    if (!o.has_value())
        return lineError (req.id, err::failed, "job " + jobId + " vanished");
    set (r, "ok", o->ok);
    if (o->ok) set (r, "result", o->result.isVoid() ? obj() : o->result);
    else       set (r, "error", errorVar (o->error));
    set (r, "job", jobId);
    set (r, "alerts", startResponse.getProperty ("alerts", juce::Array<juce::var>()));

    // Envelope state for mutating commands, as after a short command.
    const Spec* spec = CommandRegistry::instance().find (req.cmd);
    if (spec != nullptr && (spec->flags & Mutates))
    {
        FileDoc* d = nullptr;
        const auto docRef = req.doc;
        for (auto& p : app.docs)
            if ((docRef.isVoid() && p.get() == app.doc())
                || (!docRef.isVoid() && ((docRef.isInt() || docRef.isInt64() || docRef.isDouble())
                                         ? p->id == (int) docRef
                                         : p->file.getFullPathName() == docRef.toString())))
                d = p.get();
        if (d != nullptr)
        {
            set (r, "undoDepth", (int) d->undo.undoSize());
            set (r, "redoDepth", (int) d->undo.redoSize());
            set (r, "contentVersion", (juce::int64) d->engine.contentVersion());
        }
        set (r, "journalSeq", app.control().journal.lastSeq());
    }
    set (r, "durationMs", (int) (juce::Time::getMillisecondCounterHiRes() - t0));
    return r;
}

void setEnv (const char* name, const char* value)
{
#if JUCE_WINDOWS
    _putenv_s (name, value);
#else
    ::setenv (name, value, 1);
#endif
}

} // namespace

int runJsonSession (Application& app,
                    const std::function<bool (std::string&)>& readLine,
                    const std::function<void (const juce::String&)>& writeLine,
                    JsonSessionOptions opts)
{
    auto& reg = CommandRegistry::instance();
    int worst = 0;
    bool first = true;
    std::string raw;
    while (readLine (raw))
    {
        const bool isFirst = first;
        first = false;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();   // CRLF input
        if (raw.size() > (size_t) kMaxLineBytes)
        {
            writeLine (toLine (lineError ({}, err::invalidParams, "line exceeds 1 MiB",
                                          obj ({ { "path", "" }, { "expected", "<= 1048576 bytes" } }))));
            worst = 2;
            if (!opts.continueOnError) break;
            continue;
        }
        const juce::String text = juce::String::fromUTF8 (raw.data(), (int) raw.size()).trim();
        if (text.isEmpty()) continue;

        juce::var v;
        const auto parsed = juce::JSON::parse (text, v);
        if (parsed.failed() || !isObj (v))
        {
            writeLine (toLine (lineError ({}, err::invalidParams,
                                          "unreadable JSON: " + (parsed.failed() ? parsed.getErrorMessage()
                                                                                 : juce::String ("not an object")),
                                          obj ({ { "path", "" }, { "expected", "one JSON object per line" } }))));
            worst = 2;
            if (!opts.continueOnError) break;
            continue;
        }

        // Option line: {"continueOnError": true} (first line, no cmd).
        if (!v.hasProperty ("cmd") && v.hasProperty ("continueOnError"))
        {
            if (isFirst) opts.continueOnError = (bool) v.getProperty ("continueOnError", false);
            continue;
        }

        Request req;
        req.timeoutMs = opts.defaultTimeoutMs;
        Error perr;
        if (!Request::parse (v, req, perr))
        {
            writeLine (toLine (lineError (v.getProperty ("id", {}), perr.code, perr.message, perr.details)));
            worst = std::max (worst, 1);
            if (!opts.continueOnError) break;
            continue;
        }

        juce::var resp;
        if (req.cmd == "session.hello")   // no token in the CLI: the caller owns the process
            resp = obj ({ { "id", req.id }, { "ok", true }, { "result", helloInfo (app, false) } });
        else
        {
            InvokeOptions io;
            io.source = opts.source;
            resp = reg.invoke (app, req, io);
            const auto result = resp.getProperty ("result", {});
            if ((bool) resp.getProperty ("ok", false) && isObj (result)
                && result.getProperty ("state", {}).toString() == "running"
                && result.hasProperty ("job"))
                resp = waitForJob (app, req, resp, result.getProperty ("job", {}).toString());
        }

        writeLine (toLine (resp));
        if (!(bool) resp.getProperty ("ok", false))
        {
            worst = std::max (worst, 1);
            if (!opts.continueOnError) break;
        }
    }
    return worst;
}

juce::String commandsMarkdown()
{
    juce::String md;
    md << "# SpectroCraft control protocol commands\n\n"
       << "Generated by `SpectroCraft --list-commands --markdown` (protocol " << kProtocolVersion
       << ", experimental). See docs/ai-control/REPORT.md §6.\n\n";
    juce::String lastCat;
    std::vector<const Spec*> specs;
    for (const auto& s : CommandRegistry::instance().all()) specs.push_back (&s);
    std::stable_sort (specs.begin(), specs.end(), [] (const Spec* a, const Spec* b) { return a->category < b->category; });
    for (const Spec* sp : specs)
    {
        const Spec& s = *sp;
        if (s.category != lastCat)
        {
            md << "\n## " << s.category << "\n\n| Command | Flags | Parameters | Description |\n|---|---|---|---|\n";
            lastCat = s.category;
        }
        juce::StringArray ps;
        for (const auto& p : s.params)
            ps.add ("`" + p.name + "`" + (p.required ? "*" : ""));
        juce::String id = "`" + s.id + "`";
        if (!s.aliases.isEmpty()) id << " (alias " << s.aliases.joinIntoString (", ") << ")";
        md << "| " << id << " | " << flagNames (s.flags).joinIntoString (", ") << " | "
           << ps.joinIntoString (", ") << " | " << s.title << " |\n";
    }
    md << "\n`*` = required. Full schemas: `app.describe {\"withSchemas\":true}`.\n";
    return md;
}

int runJsonCli (const juce::StringArray& args)
{
    juce::String inPath, outPath;
    bool userSettings = false, listCommands = false, markdown = false;
    for (int i = 0; i < args.size(); ++i)
    {
        const auto& a = args[i];
        if (a == "--json")
        {
            if (i + 1 >= args.size()) { std::cerr << "usage: --json <requests.ndjson|-> [--json-out <file>]\n"; return 2; }
            inPath = args[++i];
        }
        else if (a == "--json-out")
        {
            if (i + 1 >= args.size()) { std::cerr << "--json-out needs a file\n"; return 2; }
            outPath = args[++i];
        }
        else if (a == "--use-user-settings") userSettings = true;
        else if (a == "--list-commands")     listCommands = true;
        else if (a == "--markdown")          markdown = true;
        else if (a.startsWith ("--"))
        {
            std::cerr << "unknown flag " << a << " in --json mode\n";
            return 2;
        }
    }

    if (listCommands)
    {
        const juce::String text = markdown ? commandsMarkdown()
                                           : juce::JSON::toString (CommandRegistry::instance().describe ({}, true, true));
        std::fwrite (text.toRawUTF8(), 1, text.getNumBytesAsUTF8(), stdout);
        std::fputc ('\n', stdout);
        std::fflush (stdout);
        return 0;
    }

    // Headless: alerts are logged, never shown; no audio device is opened.
    sc::alert::setHeadless (true);
    setEnv ("SC_NO_AUDIO_DEVICE", "1");

    juce::File store;
    if (!userSettings)
        store = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("SpectroCraft_json_" + juce::String (processId()) + ".recent");

    std::ifstream fileIn;
    std::istream* in = &std::cin;
    if (inPath != "-")
    {
        const juce::File f = juce::File::isAbsolutePath (inPath) ? juce::File (inPath)
                                 : juce::File::getCurrentWorkingDirectory().getChildFile (inPath);
#if JUCE_WINDOWS
        fileIn.open (f.getFullPathName().toWideCharPointer(), std::ios::binary);
#else
        fileIn.open (f.getFullPathName().toRawUTF8(), std::ios::binary);
#endif
        if (!fileIn.is_open())
        {
            std::cerr << "cannot read " << f.getFullPathName() << "\n";
            return 2;
        }
        in = &fileIn;
    }

    std::unique_ptr<juce::FileOutputStream> out;
    if (outPath.isNotEmpty())
    {
        const juce::File f = juce::File::isAbsolutePath (outPath) ? juce::File (outPath)
                                 : juce::File::getCurrentWorkingDirectory().getChildFile (outPath);
        f.deleteFile();
        out = std::make_unique<juce::FileOutputStream> (f);
        if (!out->openedOk())
        {
            std::cerr << "cannot write " << f.getFullPathName() << "\n";
            return 2;
        }
    }

    int rc = 0;
    {
        Application app (store);
        rc = runJsonSession (app,
            [in] (std::string& line) { return (bool) std::getline (*in, line); },
            [&out] (const juce::String& line) {
                if (out != nullptr)
                {
                    out->writeText (line + "\n", false, false, "\n");
                    out->flush();
                }
                else
                {
                    std::fwrite (line.toRawUTF8(), 1, line.getNumBytesAsUTF8(), stdout);
                    std::fputc ('\n', stdout);
                    std::fflush (stdout);
                }
            });
    }
    if (store != juce::File())
        store.deleteFile();
    return rc;
}

} // namespace sc::control
