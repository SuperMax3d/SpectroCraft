# Lot 6 — splitting out the commands and migrating the menus

Status: **partial, solid** (what is delivered is tested; the exact list of what remains is at the bottom).

## Done

### Typed commands (`src/control/commands/Edit.cpp`)

- The **20 step types** have a typed command whose alias is the old type name:
  `effect.silence/fadeIn/fadeOut/normalize/reverse/invert/changeGain {db}/timeStretch {pct}/
  catTimeScale/filterEq {points}/envelope {points}/resample {rate, weights}`, `gen.silence/noise/tone`,
  `edit.delete/trim`, plus `ai.noiseSuppress`/`ai.transcribe` (lot 3); `python` → `python.run` in lot 7.
  Each command has its `ParamSpec`s (published schema, `invalid_params` with `path`) + `select`/`channels`
  and goes through the **same** executor as `step.run` (jobs, action recording, journal).
- `edit.cut/copy/paste/pasteMix/pasteToNewChannels/copyToNewFile {select}`, `edit.splitSpectralSel`.
- `doc.new {channels, sampleRate, durationSec}` (`doc.activate` has existed since lot 0).
- `selection.tool`, `selection.clearSpectral`, `selection.brush`.
- `settings.get {keys}` / `settings.set {values}`: key table (same names as `SpectroCraft.recent`)
  with **the same bounds** as `loadSettings` (`fft_size` 99999 → 65536, `clamped:["fft_size"]`),
  full validation before applying, persistence (`saveSettings` + `saveAiSettings`). **Security**:
  `control_enabled`/`control_roots` cannot be modified through the API (a client cannot widen
  its own access); unknown key → `invalid_params`.
- `recent.list/remove/clear`, `meta.setMode {embedded|external}`, `meta.save`,
  `record.start/stop/setMode`, `actions.list`, `actions.run {name|index, select, channels}` (one history
  entry per step), `tables.list`, `tables.importCsv`.
- `docs/commands.md` **generated** (`SpectroCraft --list-commands --markdown`, sorted by domain):
  100 commands.

### Migrated menus (go through `CommandRegistry::invoke`, source `menu`)

`MainWindow::runMenuCommand (cmd, params, effectId, quiet)`: same code, same refresh
(`UiBridge`), same action recording, same journal and same patch provenance (`opId` =
command) as the API; a streamed file goes through the registry's detour (job + progress
dialog, error shown at the end of the job), a synchronous error through `showEditError`.

| MenuID | Command |
|---|---|
| `EditUndo`, `EditRedo` | `history.undo`, `history.redo` |
| `EditCut`, `EditCopy`, `EditPaste`, `EditPasteMix`, `EditPasteToNewChannels`, `EditCopyToNewFile` | `edit.*` |
| `EditDelete`, `EditTrimSelection` | `edit.delete`, `edit.trim` |
| `EditSelectAll` | `selection.all` |
| `ToolsSilence`, `ToolsFadeIn`, `ToolsFadeOut`, `ToolsNormalize`, `ToolsReverse`, `ToolsInvert` | `effect.*` (the "repeat last effect" id is kept) |

That is **17 MenuIDs** and **8 of the 27 `recordActionStep` sites** (those of the immediate effects and of
Delete/Trim): action recording now happens in the registry, after success.

- `continueReplay`: new callback `MainWindow::onReplayFinished (ok, message)` that replaces the
  "finished / stopped" modal boxes when it is set (the `actions.run` API does not use
  `continueReplay` anyway).

## Tests (`tests/regression/menu_commands.inc`, "regression: menu_commands")

- **menu == command** for the 8 effect/edit menus: the menu (`menuItemSelected` on a real
  `MainWindow`) and the API on the same file and the same range give **identical samples**;
  the menu's patch carries `opId` = the command and `source:"menu"`; `getLastEffectId` kept.
- Cut/paste/undo/select all through the menu go through the registry (`edit.cut` credited).
- Each step type has its typed command; `effect.changeGain` == `step.run changeGain`; bounds
  validated; aliases; `gen.tone`; clipboard; `edit.copyToNewFile`; `doc.new`;
  `selection.tool/brush`; `settings.set` (bound, `control_*` refusal, unknown key) / `settings.get`;
  `recent.list`; `meta.setMode/save`; `actions.run` (2 history entries) / `actions.list`;
  `tables.list`; `record.stop/setMode`.

Results (macOS arm64, Debug + AI): "menu_commands + control" 1111/1111; full suite
**6410/6410**, 10/10.

## What remains (precisely)

1. **Facades**: `applyActionStep`, `validateStepParams`, `isKnownActionType` remain the
   original implementations (the registry calls them) instead of becoming 5-line facades over
   `invoke`: replay of recorded actions and batch (`runActionBatch`, `--run-action`, `--batch`)
   run on isolated `Application`s on workers, where `invoke` (message thread) does not
   apply as is. To do with Max: decide whether batch goes through `--json`.
2. **Effect dialogs** (19 remaining `recordActionStep` sites): `showChangeGainDialog`,
   `TimeStretch`, `CatTimeScale`, `FilterEq`, `Envelope`, `Resample`, `GenSilence/Noise/Tone`,
   `NoiseSuppress`, `Transcribe`, Python: the dialog remains the form and applies the edit itself; its
   "Apply" button should call `runMenuCommand ("effect.changeGain", {db})` etc. The
   edits they produce are still journaled and attributed (`opId` = step type,
   `source:"menu"`, lot 5).
3. **Non-migrated MenuIDs** (they have an API command but the menu keeps its own path): `FileOpen/Save/
   SaveAs/Close/CloseAll/New/Quit` (FileChooser / confirmations), `View*` (zoom, views, grids:
   `settings` toggles), `Transport*` (the menu keeps `playWithAudioInit` and its dialog),
   `Selection*`, `Rec*`, `Meta*`, `Exp*`, `WindowAllowMultiple`, `Recent*`, `ActionsRunSelected/Batch`,
   `ToolsSplitSpectralSel*`.
4. **Without a command**: `export.snapshot` (`FileSnapshot`, NeedsUi), `drawing.*`, `clone.*`,
   `selection.saved.*`, `actions.batch`, `export.spectrogram` with overlays (lot 4), `tables.*` other
   than list/importCsv.
5. "menu == command" test extended to the commands above once they are migrated.
