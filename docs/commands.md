# Commandes du protocole de contrôle SpectroCraft

Généré par `SpectroCraft --list-commands --markdown` (protocole 1, expérimental). Identifiants et messages en anglais ; voir docs/ai-control/RAPPORT.md §6.


## actions

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `actions.list` | Query |  | Recorded actions (Actions panel) |
| `actions.run` | Mutates, NeedsDoc | `name`, `index`, `stopOnError`, `select`, `channels` | Replay a recorded action on a range (one history entry per step) |

## ai

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `ai.models.list` | AiBuildOnly, Query |  | AI models of every engine: installed, size, checksum, backends, installable |
| `ai.models.install` | LongRunning, AiBuildOnly | `modelId`* | Download + verify (SHA-256) a model, as a job |
| `ai.models.verify` | LongRunning, AiBuildOnly, Query | `modelId`* | Re-hash a model's files (SHA-256), as a job |
| `ai.models.remove` | Destructive, AiBuildOnly | `modelId`*, `confirm` | Delete a model's files (confirm required) |
| `ai.devices` | LongRunning, AiBuildOnly, Query | `init` | Devices each AI engine can really use (init:true loads OpenVINO, as a job) |
| `ai.setDevice` | LongRunning, AiBuildOnly | `engine`, `device`*, `persist` | Choose the device of an engine (validated against the real list) |
| `ai.noiseSuppress` (alias noiseSuppress) | Mutates, NeedsDoc, LongRunning, AiBuildOnly | `modelId`*, `device`, `attenLimDb`, `df3PostFilter`, `copyNoise`, `autoDownload`, `select`, `channels` | AI noise suppression of a range, as a job (one undo step) |
| `ai.transcribe` (alias transcribe) | Mutates, NeedsDoc, LongRunning, AiBuildOnly | `modelId`*, `language`, `translate`, `fast`, `autoDownload`, `channels` | Whisper transcription of the document (channels mixed), stored in its metadata; returns the segments |
| `ai.batchTranscribe` | LongRunning, AiBuildOnly | `folder`*, `modelId`, `language`, `translate`, `fast`, `skipExisting`, `channel`, `autoDownload`, `metaMode`, `confirm` | Transcribe every .wav of a folder tree in place (SCTT chunk / sidecar), as a job |

## app

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `app.describe` | Query | `category`, `withSchemas`, `includeInternal` | Describe every command (JSON Schemas of params) |
| `app.state` | Query |  | Application state: documents, selection, view, transport, AI, jobs |
| `app.quit` | NeedsUi, Destructive | `confirm` | Quit the application (confirm when documents are unsaved) |

## debug

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `debug.sleep` | LongRunning, Internal | `ms`, `mutating` | Test helper: a cancellable job that sleeps |

## doc

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `doc.list` | Query |  | Open documents (stable ids) |
| `doc.open` | LongRunning | `path`*, `activate` | Open an audio file (or activate it when already open) |
| `doc.close` | NeedsDoc, Destructive | `confirm` | Close a document (confirm when it has unsaved changes) |
| `doc.closeAll` | Destructive | `confirm` | Close every document (confirm when some are unsaved) |
| `doc.activate` | NeedsDoc |  | Make a document the active one (the one the window shows) |
| `doc.meta` | NeedsDoc, Query | `sections` | Document metadata bundle (markers, selections, calibration, transcription...) |
| `doc.saveAs` | NeedsDoc, LongRunning, Destructive | `path`*, `meta`, `confirm` | Save the document to a new path (audio + metadata) |
| `doc.save` | NeedsDoc, LongRunning, Destructive | `meta`, `confirm` | Save the document in place (overwrites its file) |
| `doc.new` |  | `channels`, `sampleRate`, `durationSec` | New empty document |

## edit

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `edit.delete` (alias delete) | Mutates, NeedsDoc | `select`, `channels` | Delete the range |
| `edit.trim` (alias trim) | Mutates, NeedsDoc | `select`, `channels` | Keep only the range |
| `edit.cut` (alias cut) | Mutates, NeedsDoc | `select` | Cut the range to the app clipboard |
| `edit.copy` (alias copy) | NeedsDoc | `select` | Copy the range to the app clipboard |
| `edit.paste` (alias paste) | Mutates, NeedsDoc | `select` | Insert the clipboard at the selection start / playhead |
| `edit.pasteMix` (alias pasteMix) | Mutates, NeedsDoc | `select` | Mix the clipboard into the file |
| `edit.pasteToNewChannels` (alias pasteToNewChannels) | Mutates, NeedsDoc | `select` | Paste the clipboard into new channels |
| `edit.copyToNewFile` (alias copyToNewFile) | NeedsDoc | `select` | Copy the range (visible channels) into a new document |
| `edit.splitSpectralSel` | Mutates, NeedsDoc |  | Run the configured Split Spectral Selection |

## effect

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `effect.silence` (alias silence) | Mutates, NeedsDoc | `select`, `channels` | Silence the range |
| `effect.fadeIn` (alias fadeIn) | Mutates, NeedsDoc | `select`, `channels` | Fade in over the range |
| `effect.fadeOut` (alias fadeOut) | Mutates, NeedsDoc | `select`, `channels` | Fade out over the range |
| `effect.normalize` (alias normalize) | Mutates, NeedsDoc | `select`, `channels` | Normalize the range |
| `effect.reverse` (alias reverse) | Mutates, NeedsDoc | `select`, `channels` | Reverse the range |
| `effect.invert` (alias invert) | Mutates, NeedsDoc | `select`, `channels` | Invert the polarity of the range |
| `effect.changeGain` (alias changeGain) | Mutates, NeedsDoc | `db`*, `select`, `channels` | Change gain (dB) |
| `effect.timeStretch` (alias timeStretch) | Mutates, NeedsDoc | `pct`*, `select`, `channels` | Pitch-preserving tempo change |
| `effect.catTimeScale` (alias catTimeScale) | Mutates, NeedsDoc | `reduceGaps`, `percentile`, `speedFactor`, `select`, `channels` | Content-aware time scale |
| `effect.filterEq` (alias filterEq) | Mutates, NeedsDoc | `points`*, `select`, `channels` | Zero-phase curve EQ |
| `effect.envelope` (alias envelope) | Mutates, NeedsDoc | `points`*, `select`, `channels` | Amplitude envelope over time |
| `effect.resample` (alias resample) | Mutates, NeedsDoc | `rate`*, `weights`, `select`, `channels` | Resample the file (not undoable: clears the history) |

## export

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `export.spectrogram` | NeedsDoc, LongRunning | `path`*, `start`, `end`, `width`, `height`, `overlays`, `confirm` | Render the spectrogram of a region to a PNG (no overlays) |

## gen

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `gen.silence` (alias genSilence) | Mutates, NeedsDoc | `dur`*, `select`, `channels` | Generate silence (replaces the range, or inserts at the playhead) |
| `gen.noise` (alias genNoise) | Mutates, NeedsDoc | `type`, `intensity`, `dur`*, `select`, `channels` | Generate noise |
| `gen.tone` (alias genTone) | Mutates, NeedsDoc | `tones`*, `shape`, `dur`*, `select`, `channels` | Generate tones |

## history

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `history.list` | NeedsDoc, Query | `includeJournal`, `since` | Undo history of a document (+ the session journal) |
| `history.undo` (alias undo) | Mutates, NeedsDoc | `steps` | Undo the last edit(s) |
| `history.redo` (alias redo) | Mutates, NeedsDoc | `steps` | Redo the next edit(s) |
| `history.jumpTo` | Mutates, NeedsDoc | `state`* | Undo/redo to a history state (0 = as loaded); never discards the redo stack |

## job

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `job.get` | Query | `job`* | State, progress and result of a job |
| `job.cancel` |  | `job`*, `force` | Cancel a running job (only the client that started it, unless force) |
| `job.list` | Query | `all` | Jobs of this session |

## journal

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `journal.read` | Query | `sinceSeq`, `kinds`, `limit`, `docId` | Read the session journal (commands, steps, undo/redo, jobs) |

## markers

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `markers.list` | NeedsDoc, Query |  | Markers of a document |
| `markers.add` | NeedsDoc | `name`, `start`*, `end` | Add a point marker, or a region when end > start |
| `markers.exportCsv` | NeedsDoc | `path`*, `confirm` | Write the markers to a CSV file |
| `markers.importCsv` | NeedsDoc | `path`*, `confirm` | Add markers from a CSV file (same columns as exportCsv) |

## measure

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `measure.levels` | NeedsDoc, LongRunning, Query | `start`, `end`, `channels`, `clipThreshold` | RMS / peak / crest / DC / clipping per channel, dBFS and calibrated units |
| `measure.spectrum` | NeedsDoc, LongRunning, Query | `start`, `end`, `channels`, `fftSize`, `window`, `overlap`, `values`, `peaks` | Averaged spectrum of a region (the FFT panel's numbers), optional peak list |
| `measure.thirdOctave` | NeedsDoc, LongRunning, Query | `start`, `end`, `channels`, `mode`, `order`, `fftSize`, `window` | 1/3-octave bands: reference dB-SPL analyzer or IEC bands |
| `measure.harmonics` | NeedsDoc, LongRunning, Query | `start`, `end`, `channel`, `fftSize`, `window`, `fundamentalHz`, `anchorHz`, `n`, `maxK`, `search`, `toleranceHz` | Harmonic levels (given, anchored or auto-detected fundamental) and THD |
| `measure.curve` | NeedsDoc, LongRunning, Query | `start`, `end`, `channel`, `kind`, `windowMs`, `hopMs`, `track`, `smooth`, `useSpectralSelection`, `fftSize`, `window` | Level vs time (rms|peak) or the panel's curves (ampVsTime|freqVsTime|ampVsFreq) |
| `measure.probe` | NeedsDoc, Query | `t`*, `f`*, `channel` | Spectrogram magnitude at one (time, frequency) point |
| `measure.spectrogramData` | NeedsDoc, LongRunning, Query | `start`, `end`, `channel`, `fftSize`, `window`, `hop`, `fMax`, `format` | Spectrogram matrix (frames x bins, dBFS), inline or as a float32/CSV file |

## meta

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `meta.setMode` | NeedsDoc, ActiveDocOnly | `mode`* | Metadata storage: embedded RIFF chunks or .meta.json sidecar (migrates on disk) |
| `meta.save` | NeedsDoc, ActiveDocOnly |  | Write the metadata only (no audio) |

## recent

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `recent.list` | Query |  | Open Recent list |
| `recent.remove` |  | `path`* | Remove a path from Open Recent |
| `recent.clear` |  |  | Clear Open Recent |

## record

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `record.start` |  |  | Start recording (device chosen in the Transport menu) |
| `record.stop` |  |  | Stop recording |
| `record.setMode` |  | `mode`*, `seconds` | Recording mode |

## selection

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `selection.set` (alias select) | NeedsDoc | `start`*, `end`*, `channels` | Set the time selection (seconds) and optionally the channels edits apply to |
| `selection.all` (alias selectAll) | NeedsDoc |  | Select the whole file |
| `selection.tool` |  | `tool`* | Spectral selection tool |
| `selection.clearSpectral` | NeedsDoc |  | Clear the spectral selection |
| `selection.brush` |  | `radiusHz`, `larger` | Paintbrush size |

## settings

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `settings.get` | Query | `keys` | Read settings (same clamps and names as the settings store) |
| `settings.set` |  | `values`* | Change settings (clamped like the app, persisted); control_* refused |

## step

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `step.run` | Mutates, NeedsDoc | `step`*, `select`, `channels` | Apply one action step (any of the 20 recorded types) on an explicit range |
| `steps.run` | Mutates, NeedsDoc | `steps`*, `select`, `channels`, `stopOnError` | Apply several action steps in order |
| `step.validate` | Query | `step`* | Check a step without touching any document |

## tables

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `tables.list` | NeedsDoc, Query |  | Parameter tables of the document |
| `tables.importCsv` | NeedsDoc, ActiveDocOnly | `path`*, `confirm` | Import a CSV table onto the document |

## transcription

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `transcription.get` | NeedsDoc, Query |  | Transcription segments of a document |

## transport

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `transport.play` | NeedsDoc, ActiveDocOnly | `from`, `loop` | Start playback (from a position, optionally looping) |
| `transport.stop` | ActiveDocOnly |  | Stop playback |
| `transport.pause` | ActiveDocOnly |  | Pause playback |
| `transport.loop` | NeedsDoc, ActiveDocOnly | `on` | Loop the selection (or the whole file) |
| `transport.playSpectralSel` | NeedsDoc, ActiveDocOnly | `on` | Play only the spectral selection |

## ui

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `ui.panels` | NeedsUi, Query |  | Left-column panels (Window menu) and whether they are open |
| `ui.panel.set` | NeedsUi | `name`, `index`, `visible` | Open or close a panel (Window menu) |

## view

| Commande | Drapeaux | Paramètres | Description |
|---|---|---|---|
| `view.set` | NeedsDoc, ActiveDocOnly | `t0`*, `t1`* | Set the visible time window (seconds) |
| `view.zoom` | NeedsDoc, ActiveDocOnly | `factor`, `to`, `vertical` | Zoom: factor (>1 zooms in), to selection|fit|all, or vertical factor (window only) |
| `view.channels` | NeedsDoc, ActiveDocOnly | `visible`* | Visible tracks (also the channels edits apply to) |
| `view.show` | NeedsUi | `waveform`, `spectral`, `leftPanels` | Show/hide the waveform, the spectrogram or the left panels |

`*` = obligatoire. Schémas complets : `app.describe {"withSchemas":true}`.

