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
        // Scoped capture (control commands): while captureDepth > 0 every
        // message is recorded in `captured` and never shown, headless or not.
        std::atomic<int> captureDepth { 0 };
        juce::StringArray captured;
    };

    inline State& state()
    {
        static State s;
        return s;
    }

    /** Sees every message box raised through this gate, shown or swallowed:
        the diagnostic log (src/Diagnostics.cpp) records it. Any thread. */
    using Observer = void (*) (int iconType, const juce::String& title, const juce::String& message);
    inline std::atomic<Observer>& observer()
    {
        static std::atomic<Observer> o { nullptr };
        return o;
    }

    inline void notifyObserver (juce::MessageBoxIconType icon, const juce::String& title,
                                const juce::String& message)
    {
        if (auto o = observer().load())
            o ((int) icon, title, message);
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

    /** Records the message and returns true when headless or inside a
        ScopedCapture (nothing is shown). */
    inline bool swallow (const juce::String& title, const juce::String& message)
    {
        const bool capturing = state().captureDepth.load() > 0;
        if (!isHeadless() && !capturing)
            return false;
        const juce::String line = title + ": " + message.replace ("\n", " ");
        {
            std::lock_guard<std::mutex> l (state().lock);
            if (isHeadless()) state().suppressed.add (line);
            if (capturing)    state().captured.add (line);
        }
        if (isHeadless())
            std::cerr << "[alert] " << line << "\n";
        return true;
    }

    /** Captures (instead of showing) every alert raised through this gate
        while alive - a control command reports them in its response
        ("alerts": [...]) instead of blocking on a message box. Never toggles
        the global headless flag. Nestable; message thread. */
    class ScopedCapture
    {
    public:
        ScopedCapture()
        {
            std::lock_guard<std::mutex> l (state().lock);
            start = state().captured.size();
            state().captureDepth.fetch_add (1);
        }
        ~ScopedCapture()
        {
            std::lock_guard<std::mutex> l (state().lock);
            if (state().captureDepth.fetch_sub (1) == 1)
                state().captured.clear();
        }
        /** Alerts captured since this scope began. */
        juce::StringArray take() const
        {
            std::lock_guard<std::mutex> l (state().lock);
            juce::StringArray out;
            for (int i = start; i < state().captured.size(); ++i)
                out.add (state().captured[i]);
            return out;
        }
    private:
        int start = 0;
        JUCE_DECLARE_NON_COPYABLE (ScopedCapture)
    };

    inline void showMessageBoxAsync (juce::MessageBoxIconType icon,
                                     const juce::String& title, const juce::String& message,
                                     const juce::String& buttonText = {},
                                     juce::Component* associatedComponent = nullptr,
                                     juce::ModalComponentManager::Callback* callback = nullptr)
    {
        notifyObserver (icon, title, message);
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
        notifyObserver (icon, title, message);
        if (swallow (title, message))
            return;
        juce::AlertWindow::showMessageBox (icon, title, message, buttonText, associatedComponent);
    }
   #endif
}
