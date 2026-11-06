# Phase 10 — execution of the "AI control" audit plan

Started on 2026-11-05. Branch `ai-control` of the fork `nicolasmarcotteTaintin/SpectroCraft`, created from SuperMax3d's
`main` (`8c527e3`). Pushed to the fork only; no PR to SuperMax3d without agreement.

- Reference report: `docs/ai-control/REPORT.md` (§5 architecture, §6 protocol,
  §7 MCP, §8 lots).
- Build: local macOS (`SpectroCraft-build/macos`, outside the repository); cloud agent: Linux (see CI `linux-ai`).
- One handoff note per lot: `docs/ai-control/progress/lot-N.md` (done, deviations, debts, instructions for what follows).

## Decisions taken (report recommendations, §9)

| Question | Choice |
|---|---|
| Q1 order of lots 3/4 | report order (AI then measurements) |
| Q2 opt-in | disabled by default; no toast |
| Q3 discovery file | `<userApplicationDataDirectory>/SpectroCraft/control/<pid>.json` |
| Q4 adapter | Python 3.11+, `uv` or `pip install -e` |
| Q5 MCP tools | typed tools + dynamic tools |
| Q6 persistent journal | yes (lot 5) |
| Q7 editing after undo | accepted, flagged by `redoDiscarded` |
| Q8 streamed file | materialized in a job; old flags kept |
| Q9 menu migration | done (lot 6) |
| Q11 protocol | `protocol: 1`, experimental |
| Q12 fixes | separate commit at the start of lot 0 |
| Q14 CMake option | `SC_ENABLE_CONTROL` ON by default |
| Q15 `--control` × `multi_instance=0` | second instance allowed |
| Q16 language | everything in English: identifiers, messages and docs |
| Q18 path scope | home folder + `scriptFolder()` + temporary |
| Q10, Q13, Q17 | left to Nicolas/Max (not blocking for the code) |
