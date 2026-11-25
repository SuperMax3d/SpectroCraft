package com.spectrocraft.mobile;

import android.content.Context;

import io.sentry.Sentry;
import io.sentry.android.core.SentryAndroid;

/** Crash reports for SpectroCraft Mobile, driven from C++ (mobile/src/Telemetry_android.cpp).
    The manifest turns Sentry's automatic start off: it starts here only when the
    build has a DSN and the user has not turned reports off. */
public final class Telemetry
{
    private Telemetry() {}

    public static void start (Context context, String dsn, String release, String environment)
    {
        if (dsn == null || dsn.isEmpty() || Sentry.isEnabled())
            return;

        SentryAndroid.init (context, options ->
        {
            options.setDsn (dsn);
            options.setRelease (release);
            options.setEnvironment (environment);
            options.setSendDefaultPii (false);
            options.setAttachScreenshot (false);
            options.setAttachViewHierarchy (false);
            options.setEnableAutoSessionTracking (true);
            // Every trace while developing, one in ten in production.
            options.setTracesSampleRate ("development".equals (environment) ? 1.0 : 0.1);
            options.getLogs().setEnabled (true);
            // No Session Replay: JUCE draws all text itself (file names included),
            // which the SDK's masking cannot see.
        });
    }

    public static void stop()
    {
        if (Sentry.isEnabled())
            Sentry.close();
    }

    public static void message (String text)
    {
        if (Sentry.isEnabled())
            Sentry.captureMessage (text);
    }
}
