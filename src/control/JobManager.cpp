// SpectroCraft - AI control: long-running commands as jobs (see include/control/JobManager.h).
#include "control/JobManager.h"
#include "AlertGate.h"

namespace sc::control {

enum class JobState { Running, Done, Failed, Cancelled };

struct JobRecord
{
    juce::String id, cmd, source;
    bool mutating = false;
    int docId = -1;
    juce::var params;
    juce::int64 contentVersionBefore = -1;
    std::atomic<JobState> state { JobState::Running };
    std::atomic<bool> cancel { false };
    std::atomic<double> progress { 0.0 };
    double startMs = 0.0;
    std::atomic<double> endMs { 0.0 };
    std::atomic<double> lastProgressPostMs { 0.0 };
    mutable std::mutex m;
    juce::String status;
    Outcome outcome;
    std::thread th;
    std::function<void (const juce::String&, const juce::var&)>* progressSink = nullptr;
    std::function<void (double, const juce::String&)> uiProgress;   // message thread
    std::function<void()> uiDone;                                   // message thread
    const std::atomic<bool>* externalCancel = nullptr;

    /** Folds the dialog's Cancel button into the job's own flag. */
    void pollExternalCancel()
    {
        if (externalCancel != nullptr && externalCancel->load()) cancel.store (true);
    }
};

static const char* stateName (const JobRecord& r)
{
    switch (r.state.load())
    {
        case JobState::Running:   return r.cancel.load() ? "cancelling" : "running";
        case JobState::Done:      return "done";
        case JobState::Failed:    return "failed";
        case JobState::Cancelled: return "cancelled";
    }
    return "running";
}

static juce::var infoOf (const JobRecord& r)
{
    auto v = obj ({ { "job", r.id }, { "cmd", r.cmd }, { "state", juce::String (stateName (r)) },
                    { "progress", r.progress.load() }, { "mutating", r.mutating },
                    { "source", r.source } });
    if (r.docId >= 0) set (v, "doc", r.docId);
    if (r.params.isObject()) set (v, "params", r.params);
    if (r.contentVersionBefore >= 0) set (v, "contentVersionBefore", r.contentVersionBefore);
    const double end = r.endMs.load();
    const double now = juce::Time::getMillisecondCounterHiRes();
    set (v, "elapsedMs", (int) ((end > 0.0 ? end : now) - r.startMs));
    std::lock_guard<std::mutex> l (r.m);
    if (r.status.isNotEmpty()) set (v, "status", r.status);
    if (r.state.load() != JobState::Running)
    {
        set (v, "ok", r.outcome.ok);
        if (r.outcome.ok) set (v, "result", r.outcome.result.isVoid() ? obj() : r.outcome.result);
        else              set (v, "error", errorVar (r.outcome.error));
    }
    return v;
}

// ------------------------------------------------------------------ JobControl

bool JobControl::cancelled() const
{
    rec->pollExternalCancel();
    return rec->cancel.load() || stopping->load();
}

juce::String JobControl::id() const { return rec->id; }

const std::atomic<bool>* JobControl::cancelFlag() const { return &rec->cancel; }

void JobControl::progress (double p, const juce::String& status)
{
    rec->pollExternalCancel();
    rec->progress.store (p);
    if (status.isNotEmpty())
    {
        std::lock_guard<std::mutex> l (rec->m);
        rec->status = status;
    }
    // Throttle the event to ~10 per second (the final value is in job.done).
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - rec->lastProgressPostMs.load() < 100.0)
        return;
    rec->lastProgressPostMs.store (now);
    auto r = rec;
    auto stop = stopping;
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr)
        return;
    juce::MessageManager::callAsync ([r, stop] {
        if (stop->load() || r->state.load() != JobState::Running) return;
        juce::var data = obj ({ { "job", r->id }, { "progress", r->progress.load() } });
        juce::String st;
        {
            std::lock_guard<std::mutex> l (r->m);
            st = r->status;
        }
        if (st.isNotEmpty()) set (data, "status", st);
        if (r->uiProgress) r->uiProgress (r->progress.load(), st);
        if (r->progressSink != nullptr && *r->progressSink)
            (*r->progressSink) ("job.progress", data);
    });
}

namespace {
struct CommitCall
{
    std::function<void()> fn;
    juce::WaitableEvent done;
    std::atomic<bool> abandoned { false };
};
}

bool JobControl::onMessageThread (std::function<void()> fn)
{
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr || mm->isThisTheMessageThread())
    {
        fn();
        return true;
    }
    if (stopping->load())
        return false;

    auto call = std::make_shared<CommitCall>();
    call->fn = std::move (fn);
    if (!juce::MessageManager::callAsync ([call] {
            if (call->abandoned.load()) return;
            {
                sc::alert::ScopedCapture capture;   // a job's commit never shows a message box
                call->fn();
            }
            call->done.signal();
        }))
        return false;

    // Never an unbounded wait: JobManager::shutdown() runs on the message
    // thread, so while it waits for this worker the posted call cannot be
    // running - abandoning it here is race-free.
    while (!call->done.wait (50))
        if (stopping->load())
        {
            call->abandoned.store (true);
            return call->done.wait (0);
        }
    return true;
}

// ------------------------------------------------------------------ JobManager

JobManager::JobManager()
    : stopping (std::make_shared<std::atomic<bool>> (false)),
      self (std::make_shared<JobManager*> (this))
{
}

JobManager::~JobManager()
{
    shutdown();
}

juce::String JobManager::start (const StartInfo& info, Work work)
{
    reapFinishedThreads();

    auto rec = std::make_shared<JobRecord>();
    {
        std::lock_guard<std::mutex> l (lock);
        rec->id = "j-" + juce::String (nextId++);
        jobs[rec->id] = rec;
        order.push_back (rec->id);
        // Keep the 200 most recent finished jobs.
        while (order.size() > 200)
        {
            auto it = jobs.find (order.front());
            if (it != jobs.end() && it->second->state.load() == JobState::Running)
                break;
            if (it != jobs.end())
            {
                if (it->second->th.joinable()) it->second->th.join();
                jobs.erase (it);
            }
            order.erase (order.begin());
        }
    }
    rec->cmd = info.cmd;
    rec->mutating = info.mutating;
    rec->docId = info.docId;
    rec->source = info.source;
    rec->params = info.params;
    rec->contentVersionBefore = info.contentVersionBefore;
    rec->startMs = juce::Time::getMillisecondCounterHiRes();
    rec->progressSink = &onEvent;
    rec->uiProgress = info.uiProgress;
    rec->uiDone = info.uiDone;
    rec->externalCancel = info.externalCancel;

    auto stop = stopping;
    auto me = self;
    rec->th = std::thread ([rec, stop, me, work] {
        JobControl ctl (rec, stop);
        Outcome o;
        try
        {
            o = work (ctl);
        }
        catch (const std::exception& e)
        {
            o = Outcome::failure (err::failed, juce::String ("exception: ") + e.what());
        }
        catch (...)
        {
            o = Outcome::failure (err::failed, "unexpected exception");
        }
        const bool wasCancelled = rec->cancel.load() || stop->load();
        if (!o.ok && wasCancelled && o.error.code == err::failed)
            o = Outcome::failure (err::cancelled, "cancelled by client");
        {
            std::lock_guard<std::mutex> l (rec->m);
            rec->outcome = o;
        }
        rec->progress.store (o.ok ? 1.0 : rec->progress.load());
        rec->endMs.store (juce::Time::getMillisecondCounterHiRes());
        rec->state.store (o.ok ? JobState::Done
                               : (o.error.code == err::cancelled ? JobState::Cancelled : JobState::Failed));

        if (stop->load() || juce::MessageManager::getInstanceWithoutCreating() == nullptr)
            return;
        juce::MessageManager::callAsync ([rec, stop, me] {
            if (stop->load() || *me == nullptr) return;
            JobManager& jm = **me;
            if (rec->uiDone) { auto done = std::move (rec->uiDone); rec->uiDone = nullptr; done(); }
            const auto inf = infoOf (*rec);
            if (jm.onEvent) jm.onEvent ("job.done", inf);
            if (jm.onFinished) jm.onFinished (rec->id, inf);
        });
    });
    return rec->id;
}

juce::var JobManager::info (const juce::String& id) const
{
    std::lock_guard<std::mutex> l (lock);
    auto it = jobs.find (id);
    return it == jobs.end() ? juce::var() : infoOf (*it->second);
}

juce::var JobManager::list (bool includeFinished) const
{
    juce::Array<juce::var> a;
    std::lock_guard<std::mutex> l (lock);
    for (const auto& id : order)
    {
        auto it = jobs.find (id);
        if (it == jobs.end()) continue;
        if (!includeFinished && it->second->state.load() != JobState::Running) continue;
        a.add (infoOf (*it->second));
    }
    return a;
}

bool JobManager::cancel (const juce::String& id)
{
    std::lock_guard<std::mutex> l (lock);
    auto it = jobs.find (id);
    if (it == jobs.end() || it->second->state.load() != JobState::Running)
        return false;
    it->second->cancel.store (true);
    return true;
}

bool JobManager::exists (const juce::String& id) const
{
    std::lock_guard<std::mutex> l (lock);
    return jobs.count (id) > 0;
}

bool JobManager::isFinished (const juce::String& id) const
{
    std::lock_guard<std::mutex> l (lock);
    auto it = jobs.find (id);
    return it == jobs.end() || it->second->state.load() != JobState::Running;
}

std::optional<Outcome> JobManager::outcome (const juce::String& id) const
{
    std::lock_guard<std::mutex> l (lock);
    auto it = jobs.find (id);
    if (it == jobs.end() || it->second->state.load() == JobState::Running)
        return std::nullopt;
    std::lock_guard<std::mutex> l2 (it->second->m);
    return it->second->outcome;
}

juce::String JobManager::sourceOf (const juce::String& id) const
{
    std::lock_guard<std::mutex> l (lock);
    auto it = jobs.find (id);
    return it == jobs.end() ? juce::String() : it->second->source;
}

juce::String JobManager::runningMutating (juce::String* cmdOut) const
{
    std::lock_guard<std::mutex> l (lock);
    for (const auto& kv : jobs)
        if (kv.second->mutating && kv.second->state.load() == JobState::Running)
        {
            if (cmdOut != nullptr) *cmdOut = kv.second->cmd;
            return kv.first;
        }
    return {};
}

int JobManager::numRunning() const
{
    std::lock_guard<std::mutex> l (lock);
    int n = 0;
    for (const auto& kv : jobs)
        if (kv.second->state.load() == JobState::Running) ++n;
    return n;
}

void JobManager::reapFinishedThreads()
{
    std::lock_guard<std::mutex> l (lock);
    for (auto& kv : jobs)
        if (kv.second->state.load() != JobState::Running && kv.second->th.joinable())
            kv.second->th.join();   // already returned (or about to): short
}

void JobManager::shutdown()
{
    if (stopping->exchange (true))
        return;
    *self = nullptr;
    std::vector<std::shared_ptr<JobRecord>> all;
    {
        std::lock_guard<std::mutex> l (lock);
        for (auto& kv : jobs)
        {
            kv.second->cancel.store (true);
            kv.second->progressSink = nullptr;
            all.push_back (kv.second);
        }
    }
    for (auto& r : all)
        if (r->th.joinable())
            r->th.join();
}

} // namespace sc::control
