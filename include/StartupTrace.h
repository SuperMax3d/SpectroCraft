// SpectroCraft - startup phase timings, kept in memory and written to the
// diagnostic log ("startup" event, docs/diagnostics.md) when it is on.
//
// scStart::begin() runs from a static initializer in Main.cpp (T0: the process
// image and its DLLs are loaded). Phases call scStart::mark(): a clock read and
// an append to a short list, no I/O. Once the main window has been shown,
// scStart::report() posts one "startup" event with every phase so far; a phase
// marked after that (the audio device, opened in the background) posts its own
// "startup.phase" event.

#pragma once

#include "Diagnostics.h"

#include <chrono>
#include <mutex>
#include <vector>

namespace scStart {

    struct Phase
    {
        const char* name;   // a string literal
        juce::int64 ms;     // since T0
    };

    struct State
    {
        std::chrono::steady_clock::time_point t0;
        bool armed = false;
        std::vector<Phase> phases;   // in the order marked, at most kMaxPhases
        bool reported = false;       // report() ran
        int files = 0;               // files given on the command line
    };

    constexpr size_t kMaxPhases = 64;

    // C++17 inline variables: one instance across all translation units.
    inline State state;
    inline std::mutex lock;          // phases may be marked from a worker thread

    /** Starts (or restarts, in tests) the T0 clock with no phase recorded. */
    inline void begin()
    {
        const std::lock_guard<std::mutex> l (lock);
        state.t0 = std::chrono::steady_clock::now();
        state.armed = true;
        state.phases.clear();
        state.reported = false;
    }

    /** Milliseconds since T0, -1 before begin(). */
    inline juce::int64 elapsedMs()
    {
        if (! state.armed)
            return -1;
        return std::chrono::duration_cast<std::chrono::milliseconds> (
                   std::chrono::steady_clock::now() - state.t0).count();
    }

    /** Records a phase (any thread). Returns its time since T0, -1 before
        begin(). After report(), also posts a "startup.phase" event. */
    inline juce::int64 mark (const char* phase)
    {
        if (! state.armed)
            return -1;
        const auto ms = elapsedMs();
        bool late = false;
        {
            const std::lock_guard<std::mutex> l (lock);
            if (state.phases.size() < kMaxPhases)
                state.phases.push_back ({ phase, ms });
            late = state.reported;
        }
        if (late && sc::diag::enabled())
            sc::diag::event ("startup.phase", sc::diag::Level::info,
                             { { "phase", juce::String (phase) }, { "ms", ms } });
        return ms;
    }

    /** {"<phase>": ms, ...} in the order marked; a phase marked again (a
        second window, in tests) keeps its first time. */
    inline juce::var phases()
    {
        auto* o = new juce::DynamicObject();
        const std::lock_guard<std::mutex> l (lock);
        for (const auto& p : state.phases)
            if (! o->hasProperty (p.name))
                o->setProperty (p.name, p.ms);
        return juce::var (o);
    }

    /** The main window has been shown: posts the "startup" event once, when
        the diagnostic log is on (launch to now in ms, the phases, the number
        of command-line files). */
    inline void report()
    {
        {
            const std::lock_guard<std::mutex> l (lock);
            if (state.reported)
                return;
            state.reported = true;
        }
        if (! sc::diag::enabled())
            return;
        auto* o = new juce::DynamicObject();
        o->setProperty ("ms", elapsedMs());
        o->setProperty ("files", state.files);
        o->setProperty ("phases", phases());
        sc::diag::eventObject ("startup", sc::diag::Level::info, juce::var (o));
    }

}   // namespace scStart
