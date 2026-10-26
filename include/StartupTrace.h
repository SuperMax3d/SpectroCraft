// SpectroCraft - optional startup timing measurement (--measure_load_time).
//
// scStart::begin() is called from a static initializer in Main.cpp, i.e. as soon
// as the process image and its imported DLLs are loaded (T0). Individual phases
// call scStart::mark(); nothing is emitted until enable() is called with the
// --measure_load_time command-line option, so the normal startup path pays only
// a single boolean check per phase.

#pragma once

#include <cstdint>
#include <cstdio>
#include <chrono>
#include <fstream>
#include "JuceHeader.h"

namespace scStart {

    // Shared measurement state (C++17 inline variable: one instance across all
    // translation units).
    struct State
    {
        std::chrono::steady_clock::time_point t0;
        bool armed = false;
        bool enabled = false;
        juce::File logFile;

        void emit (const juce::String& line) const
        {
            std::fprintf (stderr, "%s\n", line.toRawUTF8());
            std::fflush (stderr);
            if (enabled && logFile != juce::File())
            {
                // append mode: creates the file on first use
                std::ofstream out (logFile.getFullPathName().toStdString(), std::ios::app);
                if (out)
                    out << line.toStdString() << "\n";
            }
        }
    };

    inline State state;

    // Arm (or re-arm) the T0 clock. Safe to call more than once; tests use it
    // to start a fresh measurement session.
    inline void begin()
    {
        state.t0 = std::chrono::steady_clock::now();
        state.armed = true;
    }

    inline bool isEnabled() noexcept
    {
        return state.enabled;
    }

    inline void disable()
    {
        state.enabled = false;
    }

    // Record a phase and append "[startup] <phase>: T+<ms> ms" to stderr and
    // the log file (when enabled). Returns elapsed milliseconds since T0, or -1
    // when measurement is off.
    inline int64 mark (const char* phase)
    {
        if (!state.enabled || !state.armed)
            return -1;

        const int64 ms = std::chrono::duration_cast<std::chrono::milliseconds>
                            (std::chrono::steady_clock::now() - state.t0).count();
        state.emit (juce::String ("[startup] ") + phase + ": T+" + juce::String (ms) + " ms");
        return ms;
    }

    // Append an arbitrary line to the log file (and stderr) when enabled.
    inline void note (const juce::String& text)
    {
        state.emit (text);
    }

    // Turn on measurement and write a per-run header block to the log file.
    // The preferred path is used when writable, otherwise the app-data folder
    // is used instead. Returns the actual log file in use.
    inline juce::File enable (const juce::File& preferredLog)
    {
        if (!state.armed)
            begin();

        juce::File log = preferredLog;
        {
            std::ofstream probe (log.getFullPathName().toStdString(), std::ios::app);
            if (!probe)
                log = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                          .getChildFile ("SpectroCraft_load_time.log");
        }

        state.enabled = true;
        state.logFile = log;

        const juce::String exePath = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                         .getFullPathName();
        note ("==== SpectroCraft load-time measurement ====");
        note ("time: " + juce::Time::getCurrentTime().toString (true, true));
        note ("version: " + juce::String (ProjectInfo::versionString));
        note ("exe: " + exePath);
        note ("log: " + log.getFullPathName());
        note ("note: T0 = static init complete (a few ms after process start)");
        return log;
    }

}   // namespace scStart
