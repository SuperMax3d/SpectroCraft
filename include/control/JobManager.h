#pragma once
// SpectroCraft - AI control: long-running commands as jobs.
//
// One std::thread per job (never detached: joined on shutdown). At most one
// MUTATING job at a time. A job never edits a document from its worker: it
// prepares on the message thread (the command handler), computes on the
// worker, and commits through JobControl::onMessageThread(), which posts to
// the message thread and waits - never forever: shutdown abandons the wait.
#include <JuceHeader.h>
#include "control/CommandRegistry.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace sc::control {

class JobManager;
struct JobRecord;

/** Handed to a job's worker function. */
class JobControl
{
public:
    /** True once job.cancel was requested (or the app is shutting down). */
    bool cancelled() const;
    /** 0..1 (negative = indeterminate) + optional status text. Throttled. */
    void progress (double p, const juce::String& status = {});
    /** Runs fn on the message thread and waits for it. Returns false (fn not
        run) when the app is shutting down. Runs in place on the message
        thread / without a MessageManager. */
    bool onMessageThread (std::function<void()> fn);
    juce::String id() const;
    /** The job's own cancel flag, for APIs taking const std::atomic<bool>*
        (set by job.cancel and by shutdown). */
    const std::atomic<bool>* cancelFlag() const;

private:
    friend class JobManager;
    JobControl (std::shared_ptr<JobRecord> r, std::shared_ptr<std::atomic<bool>> stop)
        : rec (std::move (r)), stopping (std::move (stop)) {}
    std::shared_ptr<JobRecord> rec;
    std::shared_ptr<std::atomic<bool>> stopping;
};

class JobManager
{
public:
    using Work = std::function<Outcome (JobControl&)>;

    JobManager();
    ~JobManager();

    struct StartInfo
    {
        juce::String cmd;
        bool mutating = false;
        int docId = -1;
        juce::String source;
        juce::var params;            // journal / job.get (metadata only)
        /** Optional UI hooks (message thread): progress dialog updates, and
            its teardown right before job.done. */
        std::function<void (double, const juce::String&)> uiProgress;
        std::function<void()> uiDone;
        /** Optional extra cancel flag (the dialog's Cancel button). Must stay
            valid until uiDone ran. */
        const std::atomic<bool>* externalCancel = nullptr;
        /** Content version of the target document at start (journal: the
            job's entry carries the new undo state only if content changed). */
        juce::int64 contentVersionBefore = -1;
    };

    /** Starts a job (message thread). Returns its id ("j-<n>"). */
    juce::String start (const StartInfo& info, Work work);

    /** {job, cmd, state, progress, status, source, doc, result|error, elapsedMs};
        void var when unknown. state: running | cancelling | done | failed | cancelled */
    juce::var info (const juce::String& id) const;
    juce::var list (bool includeFinished = true) const;

    /** Requests cancellation. False when unknown or already finished. */
    bool cancel (const juce::String& id);
    bool exists (const juce::String& id) const;
    bool isFinished (const juce::String& id) const;
    std::optional<Outcome> outcome (const juce::String& id) const;
    juce::String sourceOf (const juce::String& id) const;

    /** Id of the running mutating job (empty when none). */
    juce::String runningMutating (juce::String* cmdOut = nullptr) const;
    int numRunning() const;

    /** Cancels everything and joins every worker (message thread). After
        this, commits are refused and finished-callbacks are not delivered. */
    void shutdown();

    /** Message thread: "job.progress" / "job.done" with their data. */
    std::function<void (const juce::String& event, const juce::var& data)> onEvent;
    /** Message thread: a job finished (journal entry). */
    std::function<void (const juce::String& id, const juce::var& infoVar)> onFinished;

private:
    void reapFinishedThreads();

    mutable std::mutex lock;
    std::map<juce::String, std::shared_ptr<JobRecord>> jobs;
    std::vector<juce::String> order;       // start order, for list() and pruning
    int nextId = 1;
    std::shared_ptr<std::atomic<bool>> stopping;
    std::shared_ptr<JobManager*> self;     // cleared on shutdown (callbacks check it)
};

} // namespace sc::control
