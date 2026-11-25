// Desktop development builds: no crash reporter (see Telemetry.h).
#include "Telemetry.h"

namespace scm::telemetry::platform
{
void startCrashReports (const juce::String&, const juce::String&, const juce::String&) {}
void stopCrashReports() {}
void crashReportMessage (const juce::String&) {}
} // namespace scm::telemetry::platform
