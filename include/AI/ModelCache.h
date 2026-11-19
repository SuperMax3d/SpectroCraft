#pragma once
// SpectroCraft - a cache that keeps one loaded AI model at a time.
//
// Loading a model takes seconds (whisper weights) to tens of seconds (an
// OpenVINO compile), so a model stays loaded after a run: re-applying the
// effect starts at once. But a loaded model is large (whisper large holds
// about 3 GB), so the cache keeps a single model, and lets it go once it has
// not been used for a while (kIdleMs). Loading another model replaces it.
//
// acquire() returns a shared_ptr: the caller keeps the model alive for the
// whole run, even if the cache drops it meanwhile (another model loaded, idle
// release). A model in use is never released for being idle.
//
// The idle release runs on the message thread (a juce timer); without a
// message manager (command-line runs) the model simply stays loaded until
// exit, as before. Nothing is released at exit: callers that must not destroy
// their models then (ggml / OpenVINO static state) never destroy the cache.

#include <JuceHeader.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>

namespace sc { namespace ai {

template <class T>
class ModelCache
{
public:
    /** How long an unused model stays loaded. */
    static constexpr int64_t kIdleMs = 5 * 60 * 1000;

    using Loader = std::function<std::unique_ptr<T> (std::string* errOut)>;

    /** The model for key: the cached one, or a new one made by load (outside
        the lock: loading is slow). Null when load fails. A different model
        that is not in use is released before loading, so two large models
        are not held together. */
    std::shared_ptr<T> acquire (const std::string& key, const Loader& load, std::string* errOut)
    {
        std::shared_ptr<T> previous;   // released after the lock
        {
            std::lock_guard<std::mutex> l (lock);
            if (model != nullptr && modelKey == key)
            {
                lastUseMs = nowMs();
                scheduleCheckLocked (idleMs);
                return model;
            }
            if (model != nullptr && model.use_count() == 1)
            {
                previous = std::move (model);
                modelKey.clear();
            }
        }
        previous.reset();

        std::shared_ptr<T> loaded (load (errOut));
        if (loaded == nullptr)
            return nullptr;

        std::lock_guard<std::mutex> l (lock);
        if (model != nullptr && modelKey == key)
            return model;   // another thread loaded it meanwhile (and may be using it): keep that one
        previous = std::move (model);   // in use elsewhere: freed when that run ends
        model = loaded;
        modelKey = key;
        lastUseMs = nowMs();
        scheduleCheckLocked (idleMs);
        return loaded;
    }

    /** Releases the model when nothing uses it and it has been idle for
        idleMs. Returns true when one was released. Called by the timer;
        tests call it directly with their own clock. */
    bool releaseIfIdle (int64_t atMs)
    {
        std::shared_ptr<T> released;
        {
            std::lock_guard<std::mutex> l (lock);
            checkPending = false;
            if (model == nullptr)
                return false;
            if (model.use_count() > 1)
            {   // a run is using it: count the idle time from now on
                lastUseMs = atMs;
                scheduleCheckLocked (kBusyRecheckMs);
                return false;
            }
            const int64_t idleFor = atMs - lastUseMs;
            if (idleFor < idleMs)
            {
                scheduleCheckLocked (idleMs - idleFor);
                return false;
            }
            released = std::move (model);
            modelKey.clear();
        }
        released.reset();   // outside the lock: freeing a large model takes a moment
        return true;
    }

    /** Releases the model now unless it is in use (tests, low memory). */
    bool releaseNow() { return releaseIfIdle (std::numeric_limits<int64_t>::max() / 2); }

    bool holds (const std::string& key) const
    {
        std::lock_guard<std::mutex> l (lock);
        return model != nullptr && modelKey == key;
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> l (lock);
        return model == nullptr;
    }

    /** Tests: a shorter idle delay, and no timer (releaseIfIdle by hand). */
    void setIdleMsForTests (int64_t ms) { std::lock_guard<std::mutex> l (lock); idleMs = ms; timerEnabled = false; }

private:
    static constexpr int kBusyRecheckMs = 30 * 1000;

    mutable std::mutex lock;
    std::shared_ptr<T> model;
    std::string modelKey;
    int64_t lastUseMs = 0;
    int64_t idleMs = kIdleMs;
    bool checkPending = false;
    bool timerEnabled = true;

    static int64_t nowMs() { return juce::Time::currentTimeMillis(); }

    // One pending check at a time. The cache outlives every timer it starts
    // (it is never destroyed, or destroyed at exit after the message loop).
    void scheduleCheckLocked (int64_t delayMs)
    {
        if (checkPending || ! timerEnabled || juce::MessageManager::getInstanceWithoutCreating() == nullptr)
            return;
        checkPending = true;
        const int delay = (int) std::min<int64_t> (std::max<int64_t> (delayMs, 1000), kIdleMs) + 500;
        juce::MessageManager::callAsync ([this, delay]
        {
            juce::Timer::callAfterDelay (delay, [this] { releaseIfIdle (nowMs()); });
        });
    }
};

}} // namespace sc::ai
