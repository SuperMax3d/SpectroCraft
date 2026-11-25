// iOS crash reports: Sentry's SentryObjC framework (see Telemetry.h). Built
// only when mobile/CMakeLists.txt fetched the framework (a DSN was given);
// otherwise the calls do nothing.
#include "Telemetry.h"

#if SC_HAVE_SENTRY_COCOA
 #import "SentryObjC.h"
#endif

namespace scm::telemetry::platform
{
void startCrashReports (const juce::String& dsn, const juce::String& release, const juce::String& environment)
{
   #if SC_HAVE_SENTRY_COCOA
    if (SentryObjCSDK.isEnabled) return;
    NSString* d = [NSString stringWithUTF8String: dsn.toRawUTF8()];
    NSString* r = [NSString stringWithUTF8String: release.toRawUTF8()];
    NSString* e = [NSString stringWithUTF8String: environment.toRawUTF8()];
    [SentryObjCSDK startWithConfigureOptions: ^(SentryObjCOptions* options)
    {
        options.dsn = d;
        options.releaseName = r;
        options.environment = e;
        options.sendDefaultPii = NO;
        options.enableAutoSessionTracking = YES;
    }];
   #else
    juce::ignoreUnused (dsn, release, environment);
   #endif
}

void stopCrashReports()
{
   #if SC_HAVE_SENTRY_COCOA
    if (SentryObjCSDK.isEnabled) [SentryObjCSDK close];
   #endif
}

void crashReportMessage (const juce::String& text)
{
   #if SC_HAVE_SENTRY_COCOA
    if (SentryObjCSDK.isEnabled)
        [SentryObjCSDK captureMessage: [NSString stringWithUTF8String: text.toRawUTF8()]];
   #else
    juce::ignoreUnused (text);
   #endif
}
} // namespace scm::telemetry::platform
