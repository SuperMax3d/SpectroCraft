package com.spectrocraft.mobile;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.view.View;

/** The activity hosting the JUCE window.

    JUCE's own com.rmsl.juce.JuceActivity calls the native appOnResume() /
    appNewIntent(), which JUCE registers only for apps with push notifications
    or in-app purchases (JUCE_PUSH_NOTIFICATIONS_ACTIVITY): without them it
    crashes with an UnsatisfiedLinkError on the first onResume. JUCE needs no
    particular activity class otherwise (it follows activities through the
    application's lifecycle callbacks), so this one only draws edge to edge,
    like JuceActivity, which JUCE's safe-area insets expect. */
public class MainActivity extends Activity
{
    @Override
    protected void onCreate (Bundle savedInstanceState)
    {
        drawEdgeToEdge();
        super.onCreate (savedInstanceState);
    }

    @SuppressWarnings ("deprecation")
    private void drawEdgeToEdge()
    {
        if (Build.VERSION.SDK_INT < 35)
        {
            final View decor = getWindow().getDecorView();
            final int flags = Build.VERSION.SDK_INT < 30
                ? (View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                   | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                   | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN)
                : 0;
            decor.setSystemUiVisibility (decor.getSystemUiVisibility() | flags);
        }

        if (Build.VERSION.SDK_INT >= 30)
            getWindow().setDecorFitsSystemWindows (false);

        if (Build.VERSION.SDK_INT < 35)
            getWindow().setStatusBarContrastEnforced (false);
        getWindow().setNavigationBarContrastEnforced (false);
    }
}
