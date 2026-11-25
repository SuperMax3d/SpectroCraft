#include "Telemetry.h"
#include <atomic>
#include <deque>

#ifndef SC_SENTRY_DSN
 #define SC_SENTRY_DSN ""
#endif
#ifndef SC_POSTHOG_KEY
 #define SC_POSTHOG_KEY ""
#endif
#ifndef SC_POSTHOG_HOST
 #define SC_POSTHOG_HOST "https://us.i.posthog.com"
#endif

namespace scm::telemetry
{
namespace
{
const juce::String sentryDsn (SC_SENTRY_DSN);
const juce::String posthogKey (SC_POSTHOG_KEY);
const juce::String posthogHost (SC_POSTHOG_HOST);

juce::String platformName()
{
   #if JUCE_IOS
    return "iOS";
   #elif JUCE_ANDROID
    return "Android";
   #elif JUCE_MAC
    return "macOS";
   #elif JUCE_WINDOWS
    return "Windows";
   #else
    return "Linux";
   #endif
}

juce::String releaseName()
{
    return juce::String ("spectrocraft-mobile@") + JUCE_APPLICATION_VERSION_STRING;
}

/** Sends batches on a background thread; keeps at most 500 waiting events. */
class PostHogSender : public juce::Thread
{
public:
    PostHogSender() : juce::Thread ("posthog") { startThread (juce::Thread::Priority::background); }
    ~PostHogSender() override { stopThread (3000); }

    void push (juce::var event)
    {
        bool full = false;
        {
            const juce::ScopedLock sl (lock);
            queue.push_back (std::move (event));
            while (queue.size() > 500) queue.pop_front();
            full = queue.size() >= 20;
        }
        if (full) notify();
    }

    void clear()
    {
        const juce::ScopedLock sl (lock);
        queue.clear();
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            wait (30000);   // every 30 s, or sooner when 20 events wait / flush() is called
            sendWaiting();
        }
    }

private:
    void sendWaiting()
    {
        juce::Array<juce::var> batch;
        {
            const juce::ScopedLock sl (lock);
            while (! queue.empty() && batch.size() < 100)
            {
                batch.add (queue.front());
                queue.pop_front();
            }
        }
        if (batch.isEmpty() || posthogKey.isEmpty()) return;
        if (! postBatch (posthogHost, posthogKey, batch))
        {
            // Offline or refused: put them back for the next attempt.
            const juce::ScopedLock sl (lock);
            for (int i = batch.size(); --i >= 0;) queue.push_front (batch[i]);
            while (queue.size() > 500) queue.pop_front();
        }
    }

    juce::CriticalSection lock;
    std::deque<juce::var> queue;
};

struct State
{
    juce::PropertiesFile* props = nullptr;
    std::atomic<bool> on { false };
    juce::String distinctId;
    std::unique_ptr<PostHogSender> sender;
};

State& state()
{
    static State s;
    return s;
}

void start()
{
    auto& s = state();
    if (sentryDsn.isNotEmpty())
    {
       #if JUCE_DEBUG
        const juce::String environment ("development");
       #else
        const juce::String environment ("production");
       #endif
        platform::startCrashReports (sentryDsn, releaseName(), environment);
    }
    if (posthogKey.isNotEmpty() && s.sender == nullptr)
        s.sender = std::make_unique<PostHogSender>();
}

void stop()
{
    auto& s = state();
    if (sentryDsn.isNotEmpty()) platform::stopCrashReports();
    if (s.sender != nullptr) s.sender->clear();
    s.sender.reset();
}
} // namespace

bool available()
{
    return sentryDsn.isNotEmpty() || posthogKey.isNotEmpty();
}

void initialise (juce::PropertiesFile& props)
{
    auto& s = state();
    s.props = &props;
    s.distinctId = props.getValue ("telemetryId");
    if (s.distinctId.isEmpty())
    {
        s.distinctId = juce::Uuid().toDashedString();
        props.setValue ("telemetryId", s.distinctId);
    }
    s.on.store (available() && props.getBoolValue ("telemetry", true));
    if (s.on.load()) start();
}

void shutdown()
{
    flush();
    auto& s = state();
    s.sender.reset();
    s.props = nullptr;
}

bool enabled() { return state().on.load(); }

void setEnabled (bool on)
{
    auto& s = state();
    on = on && available();
    if (s.props != nullptr) s.props->setValue ("telemetry", on);
    if (on == s.on.load()) return;
    s.on.store (on);
    if (on) start();
    else stop();
}

bool takeFirstLaunchNotice()
{
    auto& s = state();
    if (! available() || s.props == nullptr || s.props->getBoolValue ("telemetryNoticeShown", false))
        return false;
    s.props->setValue ("telemetryNoticeShown", true);
    return true;
}

juce::var makeEvent (const juce::String& event, const juce::var& props, const juce::String& distinctId,
                     juce::Time when)
{
    auto* p = new juce::DynamicObject();
    if (auto* given = props.getDynamicObject())
        for (const auto& kv : given->getProperties())
            p->setProperty (kv.name, kv.value);
    p->setProperty ("distinct_id", distinctId);
    p->setProperty ("$process_person_profile", false);   // anonymous events, no person profiles
    p->setProperty ("$lib", "spectrocraft-mobile");
    p->setProperty ("$lib_version", JUCE_APPLICATION_VERSION_STRING);
    p->setProperty ("$app_version", JUCE_APPLICATION_VERSION_STRING);
    p->setProperty ("$os", platformName());
    p->setProperty ("$os_version", juce::SystemStats::getOperatingSystemName());
    p->setProperty ("$device_model", juce::SystemStats::getDeviceDescription());
    auto* e = new juce::DynamicObject();
    e->setProperty ("event", event);
    e->setProperty ("properties", juce::var (p));
    e->setProperty ("timestamp", when.toISO8601 (true));
    return juce::var (e);
}

bool postBatch (const juce::String& host, const juce::String& apiKey, const juce::Array<juce::var>& events)
{
    const auto url = juce::URL (host + "/batch/").withPOSTData (buildBatchJson (apiKey, events));
    int status = 0;
    auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                                             .withExtraHeaders ("Content-Type: application/json")
                                             .withConnectionTimeoutMs (10000)
                                             .withStatusCode (&status));
    if (stream != nullptr) stream->readEntireStreamAsString();
    return stream != nullptr && status >= 200 && status < 300;
}

juce::String buildBatchJson (const juce::String& apiKey, const juce::Array<juce::var>& events)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("api_key", apiKey);
    o->setProperty ("batch", juce::var (events));
    return juce::JSON::toString (juce::var (o), true);
}

void capture (const juce::String& event, const juce::StringPairArray& props)
{
    auto& s = state();
    if (! s.on.load() || s.sender == nullptr) return;
    auto* o = new juce::DynamicObject();
    for (const auto& k : props.getAllKeys())
        o->setProperty (k, props[k]);
    s.sender->push (makeEvent (event, juce::var (o), s.distinctId, juce::Time::getCurrentTime()));
}

void capture (const juce::String& event, std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto& s = state();
    if (! s.on.load() || s.sender == nullptr) return;
    auto* o = new juce::DynamicObject();
    for (const auto& kv : props) o->setProperty (kv.first, kv.second);
    s.sender->push (makeEvent (event, juce::var (o), s.distinctId, juce::Time::getCurrentTime()));
}

void reportError (const juce::String& what)
{
    if (state().on.load() && sentryDsn.isNotEmpty())
        platform::crashReportMessage (what);
}

void flush()
{
    if (auto& s = state(); s.sender != nullptr) s.sender->notify();
}
} // namespace scm::telemetry
