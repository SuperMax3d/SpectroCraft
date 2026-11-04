#pragma once
// SpectroCraft - single entry point for the application's message boxes.
//
// With JUCE_MODAL_LOOPS_PERMITTED=1 even AlertWindow::showMessageBoxAsync()
// without a callback runs a modal loop and waits for a click. Unit tests run
// the real application code, so every warning it raised popped up on the
// developer's desktop and blocked the suite until someone pressed OK.
//
// In headless mode (set by the unit-test runner) the message is written to
// stderr as "[alert] <title>: <message>" and recorded instead of shown; tests
// can inspect what would have been displayed. Outside tests these are thin
// wrappers around the JUCE calls with identical signatures.
#include <JuceHeader.h>
#include <atomic>
#include <iostream>
#include <mutex>

namespace sc::alert
{
    struct State
    {
        std::atomic<bool> headless { false };
        std::mutex lock;
        juce::StringArray suppressed;   // "<title>: <message>", oldest first
    };

    inline State& state()
    {
        static State s;
        return s;
    }

    inline void setHeadless (bool on)   { state().headless = on; }
    inline bool isHeadless()            { return state().headless.load(); }

    /** Messages suppressed in headless mode since the last clear. */
    inline juce::StringArray suppressedMessages()
    {
        std::lock_guard<std::mutex> l (state().lock);
        return state().suppressed;
    }

    inline void clearSuppressedMessages()
    {
        std::lock_guard<std::mutex> l (state().lock);
        state().suppressed.clear();
    }

    /** Records the message and returns true when headless (nothing is shown). */
    inline bool swallow (const juce::String& title, const juce::String& message)
    {
        if (!isHeadless())
            return false;
        const juce::String line = title + ": " + message.replace ("\n", " ");
        {
            std::lock_guard<std::mutex> l (state().lock);
            state().suppressed.add (line);
        }
        std::cerr << "[alert] " << line << "\n";
        return true;
    }

    inline void showMessageBoxAsync (juce::MessageBoxIconType icon,
                                     const juce::String& title, const juce::String& message,
                                     const juce::String& buttonText = {},
                                     juce::Component* associatedComponent = nullptr,
                                     juce::ModalComponentManager::Callback* callback = nullptr)
    {
        if (swallow (title, message))
        {
            if (callback != nullptr)
            {
                callback->modalStateFinished (0);
                delete callback;
            }
            return;
        }
        juce::AlertWindow::showMessageBoxAsync (icon, title, message, buttonText,
                                                associatedComponent, callback);
    }

   #if JUCE_MODAL_LOOPS_PERMITTED
    inline void showMessageBox (juce::MessageBoxIconType icon,
                                const juce::String& title, const juce::String& message,
                                const juce::String& buttonText = {},
                                juce::Component* associatedComponent = nullptr)
    {
        if (swallow (title, message))
            return;
        juce::AlertWindow::showMessageBox (icon, title, message, buttonText, associatedComponent);
    }
   #endif
}
