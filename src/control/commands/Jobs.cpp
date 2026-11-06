// SpectroCraft - AI control: job commands (job.get / job.cancel / job.list).
#include "control/CommandRegistry.h"
#include "control/ControlState.h"
#include "Application.h"

namespace sc::control {

namespace {

Outcome notFound (const juce::String& id)
{
    return Outcome::failure (err::jobNotFound, "no job " + id, obj ({ { "job", id } }));
}

Outcome getCmd (Context& ctx)
{
    const auto id = ctx.str ("job");
    const auto inf = ctx.state.jobs.info (id);
    if (!inf.isObject()) return notFound (id);
    return Outcome::success (inf);
}

Outcome cancelCmd (Context& ctx)
{
    const auto id = ctx.str ("job");
    auto& jobs = ctx.state.jobs;
    if (!jobs.exists (id)) return notFound (id);
    const auto owner = jobs.sourceOf (id);
    if (owner != ctx.source && !ctx.flag ("force", false))
        return Outcome::failure (err::confirmRequired, "job " + id + " was started by " + owner,
                                 obj ({ { "owner", owner }, { "hint", "pass force:true to cancel another client's job" } }));
    if (!jobs.cancel (id))
        return Outcome::success (obj ({ { "job", id }, { "state", jobs.info (id).getProperty ("state", {}) } }));
    return Outcome::success (obj ({ { "job", id }, { "state", "cancelling" } }));
}

Outcome listCmd (Context& ctx)
{
    return Outcome::success (obj ({ { "jobs", ctx.state.jobs.list (ctx.flag ("all", true)) } }));
}

} // namespace

void registerJobCommands (CommandRegistry& r)
{
    r.add ({ "job.get", "State, progress and result of a job", "job",
             { P ("job", PType::String).req().describe ("Job id (\"j-<n>\")") },
             {}, Query, {}, getCmd });
    r.add ({ "job.cancel", "Cancel a running job (only the client that started it, unless force)", "job",
             { P ("job", PType::String).req(), P ("force", PType::Boolean).def (false) },
             {}, None, {}, cancelCmd });
    r.add ({ "job.list", "Jobs of this session", "job",
             { P ("all", PType::Boolean).def (true).describe ("Include finished jobs") },
             {}, Query, {}, listCmd });
}

} // namespace sc::control
