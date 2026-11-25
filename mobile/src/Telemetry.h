// SpectroCraft Mobile - crash reports (Sentry) and usage statistics (PostHog).
//
// Both are compiled in only when the build gets keys (CMake SC_SENTRY_DSN,
// SC_POSTHOG_KEY; CI passes them from the repository's variables), so a local
// or pull-request build sends nothing. When compiled in they are on by default;
// the user can turn them off (Files > Privacy), and a first-launch notice says
// so. Nothing identifying is sent: a random install id, the app version, the OS
// and device model, and event counters - never audio, file names or paths.
//
// Sentry: the native SDKs catch crashes, including C++ ones (iOS: SentryObjC
// framework, Telemetry_ios.mm; Android: sentry-android with its NDK part,
// Telemetry_android.cpp + com.spectrocraft.mobile.Telemetry). PostHog: events
// go to its /batch/ endpoint from a background thread (no SDK needed).
#pragma once
#include <JuceHeader.h>

namespace scm::telemetry
{
/** True when this build has a Sentry DSN or a PostHog key. */
bool available();

/** Reads the consent and the install id from the app settings, then starts
    Sentry and the PostHog sender when allowed. Message thread, once. */
void initialise (juce::PropertiesFile& props);
void shutdown();

bool enabled();
/** Turns crash reports and statistics on or off (saved in the settings). */
void setEnabled (bool on);

/** True until the first-launch notice was shown (then marks it shown). */
bool takeFirstLaunchNotice();

/** A PostHog event (no-op when off). Property values: numbers, bools, short strings. */
void capture (const juce::String& event, const juce::StringPairArray& props = {});
void capture (const juce::String& event, std::initializer_list<std::pair<const char*, juce::var>> props);

/** A non-fatal problem worth seeing in Sentry (no-op when off). */
void reportError (const juce::String& what);

/** Sends what is queued (app going to the background). */
void flush();

// ---- internals shared with the tests
/** POSTs the events to <host>/batch/; true on a 2xx answer. Blocking. */
bool postBatch (const juce::String& host, const juce::String& apiKey, const juce::Array<juce::var>& events);
/** The PostHog /batch/ request body for these events. */
juce::String buildBatchJson (const juce::String& apiKey, const juce::Array<juce::var>& events);
/** One PostHog event object: distinct_id, timestamp and the common properties added. */
juce::var makeEvent (const juce::String& event, const juce::var& props, const juce::String& distinctId,
                     juce::Time when);
} // namespace scm::telemetry

/** Platform crash reporter (Telemetry_ios.mm, Telemetry_android.cpp, Telemetry_none.cpp). */
namespace scm::telemetry::platform
{
void startCrashReports (const juce::String& dsn, const juce::String& release, const juce::String& environment);
void stopCrashReports();
void crashReportMessage (const juce::String& text);
} // namespace scm::telemetry::platform
