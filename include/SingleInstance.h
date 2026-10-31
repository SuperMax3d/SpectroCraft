#pragma once
// SpectroCraft - single-instance coordination (Window > "Allow multiple instances")
//
// Every running instance hosts a small named-pipe server that accepts file
// lists from other instances. When the persisted setting is OFF, a second
// launch detects the first one (named mutex on Windows), sends its
// command-line files over the pipe and quits; the first instance opens them.

#include <JuceHeader.h>
#include <functional>

namespace sc { namespace singleinstance {

/** Name of the file-forwarding named pipe. */
inline const char* kPipeName = "SpectroCraft.FileForward";

/** The persisted setting key inside the SpectroCraft.recent properties file. */
inline const char* kSettingKey = "multi_instance";

/** Default location of the settings store (same file Application uses). */
juce::File defaultStorePath ();

/** Reads the "allow multiple instances" setting directly from disk so it can
    be consulted before an Application object exists. @storePath overrides the
    usual location (used by unit tests); empty = default. Checked by default:
    a missing key or unreadable file means "multiple allowed". */
bool allowMultipleInstances (const juce::File& storePath = {});

/** Sends @paths to a running instance over the forwarding pipe and waits for
    its acknowledgement. @returns true when another instance received them -
    the caller should quit. Returns false quickly when no instance is
    listening, so callers can fall back to opening the files themselves.
    @pipeName overrides the usual pipe name (unit tests only). */
bool forwardFiles (const juce::StringArray& paths,
                   const juce::String& pipeName = kPipeName);

/** Startup decision point: call once with the command-line file list (may be
    empty). @returns true only when single-instance mode is ON and another
    instance received the files - the caller must quit without starting up. */
bool handleStartup (const juce::StringArray& cliFiles);

/** Hosts the file-forwarding server that every running instance must run so a
    later single-instance launch can hand its command-line files over to it.
    The callback is invoked on the message thread with each received list. */
class Coordinator
{
public:
    /** @pipeName overrides the usual pipe name (unit tests only). */
    Coordinator (std::function<void (const juce::StringArray&)> onFilesReceived,
                 const juce::String& pipeName = kPipeName);
    ~Coordinator();

    /** Starts the server thread (no-op when already running). */
    void startServer ();

    /** Signals the server to stop and waits briefly for it to finish. */
    void stopServer ();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}}   // namespace sc::singleinstance
