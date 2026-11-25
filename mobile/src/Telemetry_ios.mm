// iOS crash reports: Sentry's SentryObjC framework (see Telemetry.h). Built
// only when mobile/CMakeLists.txt fetched the framework (a DSN was given);
// otherwise the calls do nothing.
#define DONT_SET_USING_JUCE_NAMESPACE 1   // see AudioRoute_ios.mm
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
        // Every trace while developing, one in ten in production.
        options.tracesSampleRate = [e isEqualToString: @"development"] ? @1.0 : @0.1;
        options.enableLogs = YES;
        // No Session Replay: JUCE draws all text itself (file names included),
        // which the SDK's masking cannot see.
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
