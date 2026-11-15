# Diagnostic log

SpectroCraft can keep a small local log of what went wrong in a session: errors and warnings the
user saw, crashes, interface freezes, audio dropouts, slow operations, the startup phase timings,
and short breadcrumbs of what the user did just before. Support reads it to find performance
problems and bugs, directly or through an AI tool (Claude Code reads it through the MCP adapter,
see *Reading it back*). Code: `include/Diagnostics.h`,
`src/Diagnostics.cpp`. Tests: `tests/regression/diagnostics.inc`.

## Turning it on

The log is **off by default**. Any of these turns it on:

| How | Scope |
|---|---|
| *Help > Diagnostic Log* | Saved (`diag_log_enabled`), applied at once. A dialog first says what is recorded, with a link that opens the log folder. |
| `SpectroCraft --diag-log` | This session only. |
| `SC_DIAG_LOG=1` | This session only. |
| `SC_DIAG_LOG_DIR=<folder>` | This session only, files in `<folder>`. |

While it runs, the status bar shows a green dot and *Diagnostic log*.

## Files

Folder: `<application data>/SpectroCraft/diagnostics` (macOS `~/Library/SpectroCraft/diagnostics`,
Linux `~/.config/SpectroCraft/diagnostics`, Windows `%APPDATA%\SpectroCraft\diagnostics`).
*Help > Open Diagnostic Log Folder* opens it.

| File | Content |
|---|---|
| `diag-<UTC start>-<sid>-<part>.jsonl` | The log, JSON Lines. Every part starts with the `session.start` header. |
| `running-<sid>.json` | Exists while the session runs; touched every minute. At the next start, a marker whose process is gone, or that nobody touched for 10 minutes (a reused pid), means the session ended abnormally. |
| `crash-<sid>.txt` | Written by the crash handlers. Folded into the next session's `session.unclean` event, then deleted. |

Budget: a file rotates at 2 MB, and the folder keeps the 5 most recent files (10 MB at most).
Normal use writes about 60–100 KB per hour.

## Policy

- **Local only.** Nothing is ever sent. The user chooses what to share: *Copy Diagnostic Summary*
  or *Create Support Bundle...*.
- **Never written:** audio, transcribed text, table values, scripts, the AI control token.
  Fields named `token`, `text`, `script`, `scriptText`, `code`, `content`, `values`, `samples`,
  `prompt`, `transcript` or `rows` are dropped at any depth. Registered secrets (the control
  token) are replaced by `[redacted]`. Strings are cut at 500 characters.
- **File paths are written in clear.** The log is meant for internal support. A document's path
  appears once, in `doc.open`; later events use the document id (`doc`, the same `docId` as the
  AI control API).
- No user name, machine name or home folder in the header.

## Format

One JSON object per line. Common keys:

| Key | Meaning |
|---|---|
| `ev` | Event name, `domain.action`. |
| `lvl` | `warn`, `error` or `debug`. Absent = `info`. |
| `t` | Seconds since the session started (monotonic clock, millisecond resolution). |
| `durMs`, `sizeMb`, `rssMb`... | Units are in the key names. |
| `repeat` | This line stands for that many more identical events (see below); `lastT` is the time of the last one. |
| `fr` | `1` on lines that come from the flight recorder. |

The first line of every file is the header:

```json
{"v":1,"ev":"session.start","ts":"2026-11-09T21:04:12.381Z","t":0,"sid":"7f3c9a","pid":4312,"reason":"setting","app":"1.0.104","build":"Release","os":"Mac OSX 26.6","arch":"64-bit","cpu":"Apple M2","cores":8,"ramGb":16,"screens":[{"w":1512,"h":982,"scale":2,"main":true}],"budgetMb":10,"docs":0,"fft":2048,"aiControl":false,"ws":"Default","floating":0,"aiBuild":true}
```

`reason` says why the log runs: `setting`, `flag`, `env` or `tests`. Later parts repeat the header
with `"part": n`.

### Writing by exception

- **Collapsing.** Identical events (same name, level and text fields) within 10 s are written once.
  When the 10 s are over, one more line carries the last occurrence with `"repeat": n`, `lastT`
  (its time) and `maxDurMs` when the events had a duration; its `t` is when it was written.
- **Rate limit.** At most 60 lines per event name per minute (120 for errors). The rest is counted
  and reported in `diag.dropped` (`counts` per event name).
- **Flight recorder.** `debug` events are not written. The last 500 stay in memory and are written
  right after the next `error`, preceded by `{"ev":"flight","n":…,"cause":"<error event>"}`.
- **Never on the audio thread.** The audio callback is not instrumented. Its load and dropouts are
  read once a second from the message thread.

## Events

### Session

| Event | Level | Fields |
|---|---|---|
| `session.start` | info | Header (above). |
| `session.end` | info | `durS`, `errors`, `warnings`, `counts` per event, `worst`: the 10 slowest events. Only written on a clean exit or when the log is turned off. |
| `session.unclean` | error | `prevSid`, `prevStarted`, `prevFile`, `lastEvent` (the last line the previous session wrote), and with a crash report `cause` (its first line) and `crash` (its lines). |
| `startup` | info | Posted once the main window has been shown (+150 ms): `ms` from process start, `files` given on the command line, `phases`: `{phase: ms since process start}` in order (`before-audio-device-create`, `after-audio-device-create`, `before-properties-file`, `after-properties-file`, `before-setup-views`, `after-setup-views`, `end-of-MainWindow-ctor`, `window-shown(+150ms)`). Only when the log is on at launch: turn it on, then restart. Replaces the former `--measure_load_time` option and its `SpectroCraft_load_time.log`. |
| `startup.phase` | info | A phase marked after `startup` (`phase`, `ms`): the audio device opened in the background (`audio-device-ready(async)`, or `audio-device-disabled(SC_NO_AUDIO_DEVICE)`). |
| `diag.dropped` | warn | `counts` dropped by the rate limit, `queueFull`. |
| `diag.bundle` | info | A support bundle was written (`path`, `sizeKb`). |

### Errors and bugs

| Event | Level | Fields |
|---|---|---|
| `alert` | error for warning boxes, info otherwise | `title`, `msg`, `icon`, `hint`. Every message box raised through `sc::alert` (`include/AlertGate.h`), also in tests. |
| `exception` | error | `what`: an exception reached `std::terminate`. |
| `thread.killed` | error | `msg`: JUCE had to kill a thread by force (`Thread::stopThread` time-out). |
| `assert` | warn | `msg`: a JUCE assertion reported through `juce::Logger` (`JUCE_LOG_ASSERTIONS` builds). |
| `log` | debug | Other `juce::Logger` messages. |
| `file.load` | error (info if cancelled) | A file could not be opened: `path`, `exists`, `sizeMb`, `cancelled`, `durMs`. |
| `file.save` | info, warn above 5 s, error if failed | `path`, `fmt`, `ok`, `ch`, `sizeMb`, `durMs`. |
| `job.end` | info, error if failed | `job`, `cmd`, `src`, `ok`, `durMs`, `err`, `exitCode`, `timedOut`, `deviceUsed`, `modelId`. |

Crashes: on macOS and Linux, `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE` and `SIGABRT` handlers write
`crash-<sid>.txt` with async-signal-safe calls only: the signal, the address, the stack
(`backtrace_symbols_fd`) and the last 64 lines written. Then they re-raise the signal, so the OS
crash report is unchanged. On Windows, `include/CrashTrace.h` calls the same report after its
own stderr trace; the stack is written as `module + offset` (symbolize it with the PDB of that
build). `std::terminate` adds a `terminate: <what>` line first.

### Performance

| Event | Level | Fields |
|---|---|---|
| `ui.stall` | warn, error from 2 s | `durMs` the message thread did not respond, `during` (the command or menu item it was running), `last` (the last action within 30 s). |
| `ui.hang` | error | Written while the freeze is still going on (after 2 s, up to 10 min), in case the app is force-quit. Same fields. |
| `audio.device` | info | `dev`, `type`, `sr`, `block`, `out` channels, `latencyMs`, `xruns` (`reported` or `not reported` by the driver). |
| `audio.xrun` | warn | `n` new dropouts, `total`, `block`, `sr`, `load`, `playing`. |
| `audio.load` | info, warn if p95 > 0.8 | Per minute while playing: `n` samples, `p50`, `p95`, `max` of the audio callback load (0–1). |
| `spectral.tiles` | info, warn if p95 > 200 ms | Per minute with activity: `n` tiles computed, `batches`, `hit` cache hit ratio, `p50Ms`, `p95Ms`, `maxMs` per batch, `fft`. |
| `spectral.batch` | debug | One batch slower than 50 ms. |
| `paint.slow` | warn | `view` (`waveform`, `spectral`, `fft`), `durMs` (> 50), `w`, `h`. |
| `doc.open` | info, warn above 5 s | `doc`, `path`, `fmt`, `ch`, `sr`, `durS`, `sizeMb`, `mode` (`memory` or `stream`), `meta`, `video`, `durMs` (load time). |
| `work` | info from 0.5 s, warn from 30 s, debug below | A progress dialog: `title`, `durMs` it stayed open, `cancelled`. |
| `cmd.slow` | warn | A command that took 1 s or more (same fields as `cmd`). |
| `mem` | info, warn below 10 % free | At start, then every minute when the resident size moved by 200 MB: `rssMb`, `availMb`, `totalMb`. |

The freeze watchdog is a thread that expects a heartbeat from the message thread every 50 ms.
It ignores gaps while the application is not in the foreground (macOS App Nap throttles
background timers) and when the whole machine slept, and it stops when the application starts
shutting down (the message loop has ended by then).

### Breadcrumbs

| Event | Level | Fields |
|---|---|---|
| `cmd` | info, warn if refused | Every command of the registry (menus that go through it, AI control, tests) except successful pure reads: `cmd`, `src` (`menu`, `api:<client>`, `cli`, `test`), `ok`, `durMs`, `doc`, `err`, `job`, `params` (when short). |
| `step` | info (debug inside a command) | A recorded edit step: `what`, `doc`, `src`, `params` (when short). |
| `ui.menu` | info | `item`: `"Effects > Normalize..."`. |
| `ui.layout` | info | The panel layout changed: `ws` (workspace), `floating` windows. |
| `doc.close` | info | `doc`, `unsaved`. |
| `journal` | debug | Other AI control journal entries (history, metadata). |

### Hints

`alert` events carry a `hint` when the message matches a known cause:

| Hint | Matched text (case-insensitive) |
|---|---|
| `ai.runtime` | openvino, whisper |
| `ai.model` | not installed, model is missing |
| `network` | download |
| `fs.write` | disk full, not writable, could not save, could not be written |
| `fs.permission` | permission |
| `file.format` | unsupported format, damaged |
| `file.open` | could not open |
| `audio.device` | audio device, output device |
| `python` | python |
| `video.decoder` | ffmpeg |
| `memory` | out of memory, not enough memory |
| `timeout` | timed out |

## Reading it back

- *Help > Copy Diagnostic Summary*: Markdown, 30 KB at most, ready to paste into a chat. It has
  the session, counts, errors and warnings, the slowest operations, the documents opened, the
  recent aggregates and the last events.
- *Help > Create Support Bundle...*: lists the files, then saves a zip with the log files, the
  crash reports, `summary.md` and `README.txt`.
- AI control commands (also MCP tools `sc_diag_summary` and `sc_diag_tail`):
  - `diag.summary {maxBytes}` returns the same Markdown plus `enabled`, `sid`, `file`, `folder`
    and `files`.
  - `diag.tail {since, kinds, limit}` returns `{enabled, sid, file, events, next}`. It lists
    events with `t > since` (default −1: from the start), filtered by name or domain (`"audio"`
    matches `audio.*`), the last `limit` ones (default 200). Pass `next` as `since` to follow
    the log. The flight recorder lines written after a new error come with it, although their
    `t` is older.
  - When the log is off, both read the latest session on disk and add a `hint` to turn it on.

Suggested reading order for an assistant:

1. `session.unclean` (a crash or a force quit last time).
2. `error` lines and their flight recorder.
3. `ui.stall` / `ui.hang` with `during` and `last`.
4. `audio.xrun` and `audio.load`.
5. The slowest `durMs` (`session.end.worst` or the summary).

## Limits

- `jassert` is compiled out of Release builds. Turning on `JUCE_LOG_ASSERTIONS` would evaluate
  every assertion, including those on the audio thread. Release logs therefore have no `assert`
  events.
- `DBG()` output bypasses `juce::Logger` and is not captured.
- Exceptions thrown inside JUCE callbacks are not caught by JUCE in this build
  (`JUCE_CATCH_UNHANDLED_EXCEPTIONS` is off). They reach `std::terminate`, so they appear as
  `exception` and in the crash report.
- The signal handlers run on an alternate stack only on the message thread. A stack overflow on
  another thread may end the process without a report.
- On Windows the `std::terminate` handler is per thread (MSVC runtime) and is installed on the
  message thread. An exception escaping another thread still reaches `CrashTrace.h`, so it
  appears in the crash report as exception code `0xE06D7363`, without its message.
- `audio.xrun` depends on the driver: `audio.device` says whether it reports dropouts.

## Tests and CI

The unit suite starts its own log when `SC_DIAG_LOG_DIR` is set (no freeze watchdog: the tests
block the message thread on purpose). `tests/CMakeLists.txt` gives each ctest shard a folder
under `build/Testing/Temporary/diag/`. CI already uploads that folder with the other test logs
when a shard fails, so a crash or a killed thread in CI leaves its report there.
