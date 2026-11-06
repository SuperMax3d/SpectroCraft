#pragma once
// SpectroCraft - AI control: shared runner of action steps (step.run,
// steps.run, ai.noiseSuppress, ai.transcribe...). Applies "select" /
// "channels" from ctx.params, runs AI and Python steps as a job, attributes
// edits to ctx.source. See src/control/commands/Steps.cpp.
#include "control/CommandRegistry.h"
#include "Actions.h"
#include <vector>

namespace sc::control {

/** single = one step (result is that step's result) vs a sequence. */
Outcome runStepsCommand (Context& ctx, std::vector<sc::ActionStep> steps, bool stopOnError, bool single);

/** The "select" / "channels" parameters every step-like command accepts. */
std::vector<ParamSpec> stepScopeParams();

} // namespace sc::control
