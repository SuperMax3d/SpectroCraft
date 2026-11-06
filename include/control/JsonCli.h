#pragma once
// SpectroCraft - AI control: headless JSON mode.
//
//   SpectroCraft --json <requests.ndjson | -> [--json-out <responses.ndjson>]
//                [--use-user-settings]
//   SpectroCraft --list-commands [--markdown]
//
// One request per line, one response per line (same envelope as the control
// socket, RAPPORT §6), read and answered line by line: each response is
// flushed before the next line is read, so a parent process can hold a
// dialogue over pipes. Jobs are waited for (message loop pumped) and inlined;
// a line's timeoutMs (default 30 s, 0 = unlimited) cancels them.
// Exit code: 0 all ok, 1 at least one error, 2 usage / unreadable JSON /
// line > 1 MiB / unknown flag.
#include <JuceHeader.h>
#include <functional>

class Application;

namespace sc::control {

constexpr int kMaxLineBytes = 1024 * 1024;

struct JsonSessionOptions
{
    bool continueOnError = false;
    juce::String source = "cli";
    int defaultTimeoutMs = 30000;
};

/** Runs a request/response session over abstract line I/O (message thread).
    readLine returns false at end of input. Returns the exit code. */
int runJsonSession (Application& app,
                    const std::function<bool (std::string&)>& readLine,
                    const std::function<void (const juce::String&)>& writeLine,
                    JsonSessionOptions opts = {});

/** The --json / --list-commands entry point (args = the tokenised command
    line). Creates its own headless Application. Returns the exit code. */
int runJsonCli (const juce::StringArray& args);

/** Markdown reference of every command (docs/commands.md, --list-commands --markdown). */
juce::String commandsMarkdown();

} // namespace sc::control
