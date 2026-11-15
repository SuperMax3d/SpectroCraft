// SpectroCraft - AI control: diagnostic log commands (diag.summary / diag.tail).
// The log itself is opt-in (Help > Diagnostic Log); these
// commands only read it back, so an assistant connected to the open app can
// look at what went wrong. Schema: docs/diagnostics.md.
#include "control/CommandRegistry.h"
#include "Diagnostics.h"

namespace sc::control {

namespace {

constexpr const char* kOffHint = "turn it on with Help > Diagnostic Log, reproduce the problem, "
                                 "then call diag.summary again";

Outcome summaryCmd (Context& ctx)
{
    const int maxBytes = ctx.integer ("maxBytes", 30 * 1024);
    auto r = sc::diag::status();
    set (r, "markdown", sc::diag::summaryMarkdown (maxBytes));
    if (!sc::diag::enabled())
        set (r, "hint", kOffHint);
    return Outcome::success (r);
}

Outcome tailCmd (Context& ctx)
{
    juce::StringArray kinds;
    if (auto* a = ctx.get ("kinds").getArray())
        for (const auto& k : *a)
            kinds.add (k.toString());
    auto r = sc::diag::tail (ctx.num ("since", -1.0), kinds, ctx.integer ("limit", 200));
    if (!sc::diag::enabled())
        set (r, "hint", kOffHint);
    return Outcome::success (r);
}

} // namespace

void registerDiagCommands (CommandRegistry& r)
{
    r.add ({ "diag.summary", "Markdown summary of the diagnostic log (errors, freezes, slow operations, last events)",
             "diag",
             { P ("maxBytes", PType::Integer).range (2048, 512 * 1024).def (30 * 1024)
                   .describe ("Size cap of the markdown, bytes") },
             {}, Query, {}, summaryCmd });
    r.add ({ "diag.tail", "Events of the diagnostic log since a session time, optionally filtered by kind",
             "diag",
             { P ("since", PType::Number).def (-1).describe ("Session time in seconds: events with t > since"),
               P ("kinds", PType::Array).items (PType::String)
                   .describe ("Event names or domains (\"audio\" matches audio.*); empty = all"),
               P ("limit", PType::Integer).range (1, 2000).def (200).describe ("At most this many (the most recent)") },
             {}, Query, {}, tailCmd });
}

} // namespace sc::control
