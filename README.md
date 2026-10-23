# SpectroCraft

A high-performance waveform audio editor in the style of classic DAW waveform
editors. No multitrack/mixer — one *active* file at a time (any number of
files can be open and switched between), with a tiled, multithreaded FFT
spectral view that stays interactive on multi-hour, 96 kHz, 32-bit
multichannel files without ever loading the whole file into RAM.

Built with **C++20**, **JUCE 9** (vendored in `extern/JUCE`) and **CMake**.

---

## Features

- **Formats**: WAV / AIFF / FLAC / MP3 / Ogg via JUCE AudioFormatManager
  (PCM 8/16/24/32-bit int, 32-bit float), up to 32+ channels.
- **Streaming engine**: files larger than ~256 MB of samples stay on disk and are
  read through the `AudioFormatReader` under a mutex; small files are materialized
  in memory for fast editing. Edits require the in-memory buffer.
- **Waveform view**: mipmap pyramid overview, own time window + horizontal zoom,
  own vertical amplitude axis with dB gridlines and its own vertical zoom level,
  selection with optional zero-crossing snapping, persistent playhead (click to
  move it), loop playback. When zoomed in to ~4 samples per pixel or fewer the
  view switches from min/max envelope columns to a **per-sample polyline** —
  every individual sample is drawn as a connected line, so at high zoom you see
  the actual sample values (flat tops and vertical jumps on square waves,
  fine detail on anything else) instead of blocky columns. The time ruler adapts
  to every zoom level: classic 1-2-5 second steps below one minute (down to µs
  at deep zoom). Once the visible range exceeds a minute, labels switch to
  clock style (`m:ss`), and once it exceeds an hour they gain the hours field
  (`h:mm:ss`, e.g. `30:00` next to `1:00:00`); each major tick is subdivided
  into five unlabeled minor ticks. The right-click **Scale** menu (see below)
  can switch the time axis between seconds, an offset clock and sample counts,
  and relabel each view's vertical axis.
- **Synchronized horizontal view**: zooming or panning in either view moves the
  shared time range of *both* views (they always show exactly the same slice of
  the file), while each keeps its own vertical axis/zoom — waveform amplitude
  and spectral frequency ranges are independent. Keyboard zoom shortcuts apply
  to both at once.
- **Scrollbars**: a horizontal time scrollbar along the bottom shows the current
  view range relative to the whole file; each view has a vertical scrollbar on
  its right edge (amplitude for the waveform, frequency for the spectrogram).
  Dragging the thumb centre pans, dragging an edge zooms in/out in that
  direction, clicking outside jumps.
- **Spectral view**:
  - Tiled FFT: fixed 256 frames/tile, multi-resolution levels (hop doubles per
    level), computed in parallel on a `juce::ThreadPool`, LRU tile cache.
  - Own frequency axis (independent of the waveform view): log or linear scale,
    configurable min/max frequency, "Reset Freq" button, own vertical zoom via
    wheel over the left frequency ruler.
  - Per-channel rows: when several channels are displayed each gets its own row
    with **its own frequency axis** on the left; thin dark gaps separate the
    rows. Channel labels ("L"/"R" for stereo, channel numbers otherwise) are
    drawn in the left axis area of every row.
  - The waveform's dB amplitude axis is symmetric: negative dB values are shown
    on both sides of the centre line.
  - Bilinear smoothing between tiles/frames/bins — no blockiness.
  - Colour maps: Classic default (black → dark purple/magenta → red/orange →
    yellow, the traditional DAW spectrogram ramp) plus Viridis, Plasma, Inferno,
    Magma, Hot, Cool and Grayscale.
- **Markers**: point & region markers with editable **Title and Comments**
  (multi-line box starting on the label row), editable Begin/End times plus an
  editable **Length** field that stays in sync with End (editing Length moves
  End; clearing it converts a region back to a point marker). Sidecar
  persistence (`<file>.markers.json`); when no sidecar exists, markers are
  recovered from the WAV file itself by parsing its RIFF `cue` +
  LIST/`adtl` chunks (the classic DAW marker layout — files saved with that
  layout load their markers back automatically). Sorted access, find-near. "Add Marker" (button or `M`) creates a **region
  marker spanning the current selection** if there is one, otherwise a point
  marker at the cursor. Every file has its own marker list — switching files
  shows that file's markers, and a sidecar next to a file is re-read when it
  loads (if present) — and **double-clicking a
  marker** — in the Markers tab or on its line/region in either view — sets the
  time selection to that marker's range and makes sure both views actually show
  it: if the marker is not fully inside the current time window, the views pan
  (keeping their zoom) or zoom out when the marker is wider than the window.
  Right-clicking a row in the marker table shows a context menu with **Copy All
  Markers** (tab-separated `Label\tBegin\tEnd\tComment` lines, CRLF between rows,
  `-` for point-marker ends), **Paste Markers from Clipboard**, which parses that
  TSV format back into markers (times clamped to the file duration),
  **Export Markers as CSV...** / **Import Markers from CSV...** — a
  `Title,Begin,End,Comment` table with times in seconds (`-` for point-marker
  ends; fields containing commas/quotes are quoted per RFC 4180; import appends
  the parsed markers and clamps them to the file duration) — and **Remove All
  Markers**, which clears the list after a confirmation.
  Markers are drawn identically in both views (vertical line for points,
  bracket + label for regions) and stay in sync with the playhead, selection
  and zoom.
- **Markers inside WAV files**: saving as `.wav` embeds the markers into the file
  itself using standard RIFF chunks — a `cue` chunk with one entry per marker at
  its start sample, plus a `LIST`/`adtl` sub-chunk containing one `labl`
  (title) and one `ltxt` (comment) entry per marker. The audio data is written
  as IEEE float32 exactly as before; other formats keep the plain JUCE writer.
- **Classic DAW waveform-editor layout**:
  - Top-left tab group: **Files** (lists every loaded file with its sample
    rate, channel count and duration — plus its scale calibration when set;
    double-click a row to make it active, right-click for Activate / Close /
    Set Unit and Scale... / Open Containing Folder), **History**, and
    **Frequency Analysis** (visible by default): live FFT with dB/Hz axes and
    a cursor readout.
    A single click in either view re-analyses immediately: with no selection
    it shows one full-size window centred on the cursor (and follows the
    playhead while playing); with a selection it analyses the whole selection
    at the selected FFT size — when the selection is wider than one window,
    several evenly spaced windows are computed and their magnitudes averaged
    (the classic "average over the selection" behaviour). Its FFT size is
    **independent of the spectrogram's**, plus a window-type selector
    (Rectangular/Hann/Hamming) and a frequency-axis dropdown with six modes:
    **FFT Linear** (linear Hz axis), **FFT Log** (logarithmic axis, the default)
    and **1/3 Octave** (IEC 61260-1 band plot from 50 Hz to 16 kHz — one curve
    per visible channel, each band drawn as a flat-topped step across its
    frequency edges with vertical connectors at the boundaries — the classic
    1/3-octave band-plot look; every click replaces the curve instead of
    adding another), plus three curve-analysis modes:
    **Freq vs Time** (the tracked dominant frequency plotted against time —
    the cursor readout also shows each channel's level), **Amp vs Time**
    (level over time) and **Amp vs Freq** (a per-bin spectrum of the analysed
    region, using the FFT size + window type like the classic modes). The curve modes
    analyse the selection frame by frame (fixed 2048-point Hann frames in the
    two time-based modes, capped at 2048 points per curve) and honour a
    spectral-selection mask — bins outside it do not count. Their extra row of
    options offers **Ref Time** (Relative = ticks counted from the selection
    start, Absolute = file time), **Smooth** (None / 3 / 5-frame centred moving
    average) and **Track** (Max = strongest component per frame/bin with its
    frequency; Total = summed level with amplitude-weighted centroid
    frequency). In the time-based modes the bottom axis is fixed to the
    analysed region: left-drag/wheel/right-drag box zoom only affect the
    vertical range, and **Zoom Out Full** resets just that range. The 8
    snapshot buttons keep working in these modes but store a *separate* set of
    curve snapshots per mode (a slot saved in Freq vs Time is empty when you
    switch to Amp vs Time); stored curves follow the current Track/Smooth
    options, exactly like classic snapshots follow the current FFT size.
    8 numbered snapshot buttons, each with its own classic DAW hold-button
    colour: red, orange, yellow, light green, dark green, teal, blue and
    magenta. Each button's background matches the colour of the curve it
    reproduces; a toggled-on button shows the full colour with a thick white
    border, an off-but-stored one is dimmed, and an empty slot is a dark tint.
    **Plain click** on an empty slot stores the current live region (selection
    or playhead) and shows it; plain click on a stored slot only toggles
    visibility — it does *not* update the value. **Ctrl+click** re-stores/
    updates from the current selection/cursor position and shows the curve.
    **Shift+click** clears the slot. Stored curves are drawn in their button's
    full colour — just a little thinner than the live curves — beneath them,
    at their own stored FFT size, in the classic axis modes (the curve modes
    keep their separate per-mode snapshot store described above). Snapshots are **shared across
    all open files**: each one stores its source file and channel list, so a
    curve captured from another file keeps working (and is recomputed from
    that file's engine when the FFT size or window type changes). The tooltip
    shows the stored time segment plus the source file name. **Right-clicking
    a snapshot button** offers **Convert to marker** (creates a region marker
    spanning the snapshot's time range in its source file) and **Clear**
    (same as Shift+click).

    The analysis view has **display ranges fully decoupled from the FFT
    settings**: magnitudes are always normalised against a fixed −160 dB
    floor, while what you *see* is controlled by two independent windows —
    a frequency window (default 20 Hz–20 kHz in log mode) and a level window
    (default 0…−80 dB). Interactions:

    - **Mouse wheel** over the plot zooms the frequency window around the
      cursor; **Shift+wheel** (or wheel over the right level axis) zooms the
      level window instead.
    - **Right-drag a box** (a rubber-band rectangle is shown while dragging):
      horizontal span sets the frequency window, vertical span sets the level
      window (both at once if you drag in both directions). A plain right-click
      shows a context menu with **Zoom Out Full**, **Copy Spectrum to
      Clipboard**, and the harmonic-reference tools below; right-clicking the
      *level* axis resets only the level window, the *frequency* axis only the
      frequency window.
    - **Set Harmonic References** (right-click): a popup asks which harmonic
      number the frequency under the cursor is (default 1 = fundamental). The
      panel then draws delicate vertical lines for harmonics 1..N wherever they
      are inside the visible frequency window, each labelled with its number.
      The lines are dynamic — they re-anchor to the frequency under the mouse as
      you move it. **Copy Harmonics** copies an Excel-friendly table (tab-
      separated, CRLF line endings) with columns *Harmonic*, *Frequency* and
      *Amplitude* (sampled from the first visible channel at each harmonic's
      nearest FFT bin; empty above Nyquist). **Reset Harmonic** removes the
      reference and its lines.
    - **Left-drag on the bottom Hz strip** pans the frequency window;
      **left-drag on the right dB strip** pans the level window (log-aware in
      log mode).
    - The frequency axis uses adaptive ticks — every digit per decade at mid
      zooms (with nice linear subdivisions filling wide gaps such as 1k–2k),
      a nice linear step inside sub-decade ranges, and even divisions when a
      deep 1/3-octave zoom lands between band centres — so the axis keeps its
      labels no matter how far in you zoom. The spectrogram's frequency ruler
      uses the same scheme.
    - The cursor readout shows the frequency under the mouse plus each
      visible channel's level at that bin; when the mouse is within a few
      pixels of a curve it **snaps to the nearest one**, showing the exact
      bin-centre frequency and that channel's value with a marker dot.
  - **History**: lists every stored undo state of the active file — row 0 is
    "Loaded <file name>", then one row per edit with a human-readable action
    name ("Changed gain by +6.00 dB", "Deleted samples", "Silenced selection",
    "Faded in/out", "Normalized", "Reversed selection", "Pasted samples"). The
    current state is highlighted; states you can still redo are dimmed below it.
    **Clicking any row jumps straight to that state**, undoing or redoing as
    many steps as needed in one batched operation (a single mipmap rebuild for
    the whole run instead of one per step). The header shows "N of M applied".
  - **Python**: a script editor next to History. The dropdown lists every
    `*.py` file in your personal script folder (`%APPDATA%/SpectroCraft/python`
    on Windows, created automatically with six examples: *Dynamic
    processing*, *Threshold*, *Modulation*, *Generate White Noise*,
    *Generate Tones* and *Add Echo*). **Add** creates a new script
    from a template, **Remove** deletes the selected one, and **Run** executes
    it against the current selection — or against the whole file when nothing
    is selected. The run is undoable (it appears in History as "Python script
    '<name>'").

    A script is called as `python <script.py> <input.wav> <output.wav>
    <meta.json>`: `input.wav` is a 32-bit float WAV of the processed range with
    all channels, and `meta.json` carries `sampleRate`, `numChannels`,
    `fileDurationSec`, `selectionStartSec`, `selectionEndSec`, `wholeFile`,
    `fileName` and every marker (`name`, `comment`, `start`, `end`; `-1` means
    a point marker). The script must write `output.wav` with the **same**
    sample rate, channel count and length; anything else is rejected. Temp
    files are cleaned up after each run. Note that Python's stdlib `wave`
    module only handles PCM — since SpectroCraft exchanges IEEE float32 WAVs,
    the example scripts (and the Add template) embed a small RIFF/WAVE
    reader/writer you can reuse in your own scripts.

    Only a minimal CPython installation is needed — the example scripts use
    nothing but the standard library (no pip packages). If no interpreter is
    found on the PATH, Run shows installation instructions: download Python 3.x
    from <https://www.python.org/downloads/>, install with default options
    (on Windows keep "Add python.exe to PATH" checked), verify with
    `python --version` in a terminal, then press Run again. Note that pure-Python
    processing is slow on long selections — select shorter ranges for big jobs.
  - Bottom-left tab group: **Spectral Controls** (colour map + preview, dB
    range, Log/Linear toggle, Reset Freq, FFT size — from **512**, the finest
    frequency resolution, up to 65536). Above the colour-map
    gradient the panel shows the absolute level of each end of the scale in
    the active file's unit — plain `dB` when uncalibrated, or calibrated values
    after **Set Scale**: `14.0 dB-SPL … 94.0 dB-SPL` for a 1 Pa full scale,
    while Volts/g files show plain linear values (`0.0001 V … 1 V` for a 1 V
    full scale). The labels follow both the dB-range slider and calibration
    changes.
    **Markers** (table with
    a Comment column; select a row to edit its Title, multi-line Comments,
    Begin, End and Length in the detail editors below the table — the comments
    box starts beside its label and extends to the bottom of the panel;
    Add Marker, delete; right-click a row for Copy All Markers / Paste Markers
    from Clipboard, Export/Import CSV, Remove All Markers or Find (Ctrl+F
    searches titles and comments, F3 / Ctrl+F3 jump to the next / previous
    match)).
  - **Draggable panel tabs**: drag any tab to reorder the panels within its
    group, or drop it onto a tab of the other group to move it across — e.g.
    put Markers next to Frequency Analysis. The arrangement (plus the column
    width and the top/bottom split) is remembered between sessions.
  - **View > Left Panels** hides all left panels at once so you can focus on
    the tracks; toggle it again to bring them back.
  - Individual panels can be closed with a right-click **Close Panel** menu
    item (on any panel, including its list rows where applicable). The
    **Window** menu lists every left-column panel with a checkmark and opens
    or closes each one; a group whose last panel is closed gives up its space
    so the other tab bar fills the whole column. Closed panels stay closed
    between sessions (reopening inserts them back at their default position).
  - Per-channel display selection lives in the **View menu**: one checkable item
    per channel ("Channel 1", "Channel 2", …) that toggles each channel
    independently for any number of channels and applies to both views.
    **Ctrl/Shift+click** on a channel item shows *only* that channel (solo).
  - The View menu also has **Waveform View / Spectrogram View** visibility
    toggles; at least one view is always kept visible — hiding the last one is
    refused and the remaining view takes the whole main area.
  - Main area: waveform view on top, spectral view below, separated by a
    **draggable divider** — drag it up/down to resize the two views. A time
    scrollbar runs along the bottom; each view has its own vertical scrollbar.
  - Bottom status bar: left side shows the **name of the file currently in
    the views** (updated on open, when switching between loaded files and
    after closing; empty while no file is loaded) followed by a live cursor
    probe (time + amplitude on the waveform, time + frequency + level on the
    spectrogram; in the file's physical unit when a scale calibration is set —
    linear Pa/V/g on the waveform, dB-SPL for Pascal files and plain V/g values
    for Volts/g files) while hovering over either view; right side shows the
    **selection start, stop and length as three editable fields**
    (`Sel: [start] - [stop] ([len])`) whenever a selection exists — updated
    continuously while dragging it in either view. Click into any field to type
    or paste a new value (plain seconds like `12.5` or clock style like
    `1:02:35.127`); parseable changes apply live, Enter normalises the field,
    and losing focus reverts invalid text. Times of a minute or more display in
    clock style (`m:ss`, `h:mm:ss[.dddd]`). The current **playhead position**
    (`Pos: t`) uses the same formatting and tracks playback at 30 Hz.
  - The File menu offers **Save Spectrogram As...** (Ctrl+Shift+E), which writes
    the current spectrogram content (per-channel rows included, no axes or
    overlays) to a PNG file.
- **New files**: File → **New...** (Ctrl+Shift+N) creates an empty document with
  the chosen channel count and sample rate (defaults: 2 channels / 44100 Hz,
  or the active file's values). New documents are named *Untitled*,
  *Untitled 2*, ... in creation order, have no path until you Save As, and can
  be pasted into immediately — a paste at position 0 grows an empty file to the
  clipboard length. The Edit menu's **Copy to New File** copies the current
  selection (or the whole file when nothing is selected) into a fresh document
  that becomes active; it keeps exactly the *visible* channels, and if a
  spectral selection is active only its (time, frequency) content is copied.
  **Paste Mix** (Ctrl+Shift+V) adds the clipboard to the audio at the selection
  start instead of replacing it — the overlap region is summed in place, and if
  the clipboard extends past end-of-file the remainder is appended. Both are
  single undoable edits.
- **Large-file loading**: files bigger than ~200 MB are opened on a background
  thread with a modal progress dialog showing an animated bar (read + mipmap
  rebuild progress); smaller files load synchronously as before. The UI stays
  responsive throughout, and the dialog closes itself when loading finishes.
- **Save / Save As / Export Spectral** also run on background threads behind a
  modal dialog with an indeterminate spinning bar, so multi-second writes never
  freeze the window (and no stale wait cursor is left behind afterwards).
- **Application icon**: `icon.ico` (project root) is embedded in
  `include/IconData.h` and applied to the main window at startup via
  `CreateIconFromResourceEx` + `WM_SETICON`, so it appears in the title bar,
  taskbar and Alt-Tab. It is set at runtime rather than through a `.rc`
  resource file because this toolchain's rc.exe emits ICON statements as
  RT_CURSOR entries, which Windows ignores for window icons.
- **Multi-file support**: any number of files can be open at once. The **Files**
  tab lists every loaded file with its properties (sample rate, channel count,
  duration); **double-click a row to make that file active** — it is loaded
  into the tracks/views and its markers appear in the marker list. Each file
  remembers its own playhead position, selection range, visible channels,
  markers and undo history; switching files restores all of them.
  **Ctrl+Tab / Ctrl+Shift+Tab** cycles through the loaded files (forward /
  backwards). Closing a file (right-click → Close, File → Close, or
  Ctrl+W / Ctrl+F4 for the active one) removes it from the list. The live frequency analysis always
  follows the playhead of the *active* file, and **the eight stored snapshots
  are shared across all open files** — each snapshot remembers which file it
  was stored from (shown in its tooltip), so curves from different files can be
  compared side by side. Closing a file clears only the snapshots that came
  from it.
- **Scale calibration (Set Unit and Scale)**: right-clicking a file in the Files list
  offers **Set Unit and Scale...**, which asks for physical units (**Volts, Pascal, g or
  None**) and the value at full scale — e.g. "2 Pascal" means a full-scale sample
  (|1.0|) corresponds to 2 Pa of pressure. The calibration is stored per file
  (for the session) and displayed under the corresponding entry in the Files
  list. When set, readouts are converted from relative dB into absolute levels:
  the waveform's amplitude axis shows physical values in that unit (Pa/V/g),
  and the frequency/spectral readouts — the spectrogram hover probe, the
  Spectral Controls level-scale labels, and the frequency analysis view (FFT
  curves as well as 1/3-octave bands, including its axis labels, cursor readout
  and clipboard export) — show **dB-SPL** for Pascal files but plain linear
  values in **V** or **g** for Volts/g files. "None" restores plain relative dB.
- **Open Recent**: File → Open Recent lists up to the ten most recently opened
  files (most recent first) and re-opens them with one click. The list is
  persisted in a `PropertiesFile` (`SpectroCraft.recent` under the user's
  application-data folder), survives restarts, deduplicates repeated opens,
  and self-heals: entries whose file was deleted or moved are dropped when
  clicked. "Clear Recent Files" empties the list.
- **Drag & drop**: drop one or more WAV/AIFF/FLAC/MP3/Ogg files onto the
  window; every valid dropped file becomes its own document. Dropping a
  `.selections.json` file onto the **Selection** panel loads it into that
  file's saved-selection list instead.
- **Edits** with 50-level undo/redo (patch-based: old/new sample data + length
  delta — memory efficient): silence, fade in/out, normalize, reverse, gain,
  cut/copy/paste, delete, insert. The destructive effects (silence/fade/
  normalize/reverse/gain) apply **only to the currently selected (visible)
  channels** — e.g. with only Channel 2 visible, a fade never touches the
  other tracks; undo restores every channel exactly (patches capture the old
  data *before* the in-place modification).
- **Deleting a time range adjusts related state**: everything that references
  file time follows the cut inside the same undo step. Markers shift left,
  trim, or drop depending on where they sit relative to the deleted range;
  named saved spectral selections (Selection panel) follow the same rules —
  rectangles trim/shift/drop, while polygons and strokes that straddle the
  cut are split into two contiguous pieces; stored FFT snapshots of that file
  shift/trim/clear accordingly. Undo and redo restore all of it together with
  the audio patch (snapshot curves are recomputed from the restored audio), so
  a single Ctrl+Z brings back both the samples and every marker, selection and
  snapshot exactly as before.
- **Resample**: Effects → Resample… converts the whole file to any sample rate
  using SoXR's very-high-quality (`SOXR_VHQ`) resampler. Whole-file operation,
  requires the file fully in memory, not undoable (the old data is discarded);
  the footer shows the current sample rate.
- **AI features** (built by default; see [AI features](#ai-features) below):
  - *Noise Suppression* — Effects → Noise Suppression… applies a neural
    speech-enhancement model (DeepFilterNet 2/3 or DenseUNet) to the current
    selection via OpenVINO.
  - *Transcription* — Effects → Transcribe… runs Whisper over the whole file;
    segments appear in the **Transcription** tab (double-click a row to jump,
    Ctrl+F / F3 search for matches). Results are stored inside the WAV as
    a custom `SCTT` RIFF chunk, so they travel with the file.
  - *Batch transcription* — Effects → Transcribe Folder… transcribes every
    `.wav` in a folder tree headlessly (files never open in the editor) and
    writes each file's `SCTT` chunk back in place; optional skip-existing mode.
- **Cancelling long operations**: every progress dialog — loading large files,
  resampling, noise suppression, transcription (single and batch), spectrogram
  export — has a Cancel button, and ESC works as well. The operation stops at
  the next safe checkpoint and nothing is committed on cancel; for Whisper
  inference a cancel takes effect after the current phase finishes.
- **Offline tools**: `--test-spectral` renders a spectrogram PNG headlessly;
  `--test-load` times a headless load and prints mipmap stats (useful for
  multi-hour files); `--generate-test-files` creates the test WAVs;
  `--run-unit-tests` runs the unit suite from either executable.

---

## Building (Windows / MSVC)

Requirements: Visual Studio 2019+ Build Tools (MSVC x64), CMake ≥ 3.22.
JUCE is vendored — no network fetch needed at configure time.

```bat
:: from the project root, inside a "x64 Native Tools Command Prompt" or after vcvars64.bat
cmake -S . -B build -G "Ninja"            :: or any CMake generator you have
cmake --build build
```

Outputs:

- Debug:   `build/src/SpectroCraft_artefacts/Debug/SpectroCraft.exe`,
            `build/tests/SpectroCraftTests_artefacts/Debug/SpectroCraftTests.exe`
- Release: `cmake --build build --config Release` → same paths under `Release/`

Notes:

- The project enables C and C++ (JUCE 9 requires both) and sets C++20.
- `SpectroCraft` defines `JUCE_MODAL_LOOPS_PERMITTED=1` so the file dialogs work.
- On other platforms, point CMake at your JUCE (the vendored copy works anywhere;
  you may also use `FetchContent` instead).

### Versioning

The version number is **auto-incremented on every build**: a custom target runs the
`cmake/bump_version.cmake` script before compilation, which bumps the patch number in
`VERSION.txt` (first build on a fresh checkout uses `1.0.0`, then `1.0.1`, `1.0.2`, …)
and writes `generated/Version.h` — defining `SC_VERSION_STRING`, shown in **Help > About**.

A second custom command (`cmake/patch_generated.cmake`) then rewrites the freshly bumped
version into JUCE's generated files *after* juceaide has written them but *before* they are
compiled:

- `SpectroCraft_resources.rc` — the `VS_VERSION_INFO` resource embedded in the exe, so
  Windows **file properties** (File version / Product version) always carry the new version;
- `JuceHeader.h` — keeps `ProjectInfo::versionString` / `versionNumber` in sync.

(In-place patching is used because this environment's `rc.exe` ignores `/D`
preprocessor definitions, so JUCE's `JUCE_USER_DEFINED_RC_FILE` mechanism cannot be relied on.)

### CPU erratum & SIMD dispatch

The AVX2 FFT path (`src/FFTAvx.cpp`) is guarded by a one-time hardware self-test:
some CPUs (observed on an AMD Ryzen 9 3900X / Zen 2) have an erratum where a SIMD
store inside a loop causes the CPU to commit one spurious extra iteration beyond
the loop bounds, corrupting adjacent memory. The machine code is correct; the
detection runs the exact load/add/store loop pattern over a sentinel-padded buffer
and disables the AVX2 path for the life of the process if corruption is found.
On affected machines the scalar JUCE FFT fallback is used instead (verified to be
bit-identical in output).

In addition, the AVX2 path is disabled outright in Debug builds: the erratum
only manifests under concurrent multithreaded load, which a one-shot
single-threaded self-test cannot reliably detect. (A separate, real bug that
looked like this instability — a data race in the window-function cache that
returned a dangling pointer when two threads first requested the same window —
has been fixed; see `SpectralEngine::getWindow`.) The unit test suite and all
offline renders now pass deterministically in **both** Debug and Release on the
affected machine.

### Rendering backend (no D3D11/Direct2D at all)

JUCE 9 uses **Direct2D/D3D11** on Windows in three separate places. On some
machines (observed with Intel GPU drivers) `D3D11CreateDevice` throws an
unhandled C++ exception inside `d3d11.dll`/the driver, crashing the whole
process (`0xE06D7363`). SpectroCraft therefore patches the vendored JUCE to
remove **all** D3D11 device creation:

- `extern/JUCE/modules/juce_gui_basics/native/juce_Windowing_windows.cpp` —
  `Component::createNewPeer()` now passes engine index `0` (GDI "Software
  Renderer") for every top-level window (main window, file chooser dialogs,
  alerts) instead of the default Direct2D.
- Same file — `HWNDComponentPeer::getCurrentRenderingEngine()` now compares the
  backend names as strings. The original code compared `const char*` pointers,
  which only coincidentally matched in Release builds (string-literal
  deduplication) and returned `-1` in Debug builds.
- `extern/JUCE/modules/juce_graphics/native/juce_Direct2DImage_windows.cpp` —
  `NativeImageType::create()` now always returns software pixel data. JUCE 9's
  default on Windows is to back **every** `juce::Image` with a Direct2D/D3D11
  surface, so even with GDI window rendering the app still created D3D11
  devices (this was the remaining crash source after the first patch).
- `extern/JUCE/modules/juce_graphics/native/juce_DirectX_windows.cpp` —
  `DxgiAdapter::create()` no longer calls `D3D11CreateDevice`/`d2dFactory->
  CreateDevice`. JUCE creates one D3D11 device per GPU adapter as soon as any
  window exists (via the VBlank dispatcher, even in GDI mode). The DXGI
  adapter/output handles are kept because the VBlank threads need them to
  synchronise GDI repaints with vertical blank.

All four changes are marked with a `SPECTROCRAFT PATCH` comment; re-apply if
JUCE is updated. The result: SpectroCraft never creates a D3D11 or Direct2D
device, so it runs identically on machines whose GPU drivers cannot handle
them. The GDI backend is slower than Direct2D, but SpectroCraft's rendering
cost is dominated by CPU-side work (FFT tiles, bilinear pixel mapping into
image buffers), so the difference is not noticeable in practice. A unit test
(`rendering backend`) verifies that top-level windows really get the GDI
backend.

### Debug builds: OutputDebugString quirk (this VM only)

In this specific VM environment, calling the Win32 `OutputDebugString()` API
causes a misbehaving debug-event listener to raise fail-fast exceptions
(`0x4001000A` / `0x40010006`) on the *calling* thread. In Debug builds every
failed JUCE assertion goes through `logAssertion()` → `Logger::
outputDebugString()`, so a single tripped `jassert` used to kill the process
at startup (exit code 3). Two patches make this impossible:

- `extern/JUCE/modules/juce_core/native/juce_SystemStats_windows.cpp` —
  `Logger::outputDebugString()` now writes assertion/debug messages to stderr
  instead of calling Win32 `OutputDebugString()`. Assertion failures are still
  visible (on stderr) but can no longer trigger the quirk.
- All narrow string literals in the app source are ASCII-only. JUCE's
  `juce::String(const char*)` treats bytes as Latin-1 and trips a `jassert`
  for any byte > 127, so non-ASCII characters (e.g. "·") must not appear in
  narrow literals — use `juce::CharPointer_UTF8(...)` or escape sequences if
  you need them.

`include/CrashTrace.h` additionally passes the fail-fast codes through as a
safety net, so even an unexpected trigger cannot terminate the process.

### Tooltips: stale-tip fix in vendored JUCE

JUCE 9.0.4's `TooltipWindow::timerCallback()` refuses to update a visible
tip when the cursor sits exactly on the pixel where a mouse button was last
pressed (`mouseSource.getLastMouseDownPosition() != lastMousePos`). That
position is stored in a ring buffer that is never cleared after release, so
after clicking any control, hovering back over that exact pixel left the
*previous* tooltip visible forever (e.g. click snapshot button 2, move to
button 1 and back: button 2 kept showing "Snapshot 1 ..." instead of its own
tip). The guard is redundant — while a button is actually held down,
`getTipFor()` already returns an empty tip and the window hides itself — so
the patch removes it:

- `extern/JUCE/modules/juce_gui_basics/windows/juce_TooltipWindow.cpp` — the
  `showTip()` lambda now always calls `displayTipInternal()`. Marked with a
  `SPECTROCRAFT PATCH` comment; re-apply if JUCE is updated.

### Double-clicks: missing `WM_*BUTTONDBLCLK` dispatch in vendored JUCE

On Windows the OS delivers the second press of a fast double-click as
`WM_LBUTTONDBLCLK` (not another `WM_LBUTTONDOWN`). The original JUCE 9.0.4
window-proc switch only handled the plain button-down messages, so that
second press was never registered in JUCE's click ring buffer and
`Component::mouseDoubleClick` **could never fire** — double-clicking a marker
(or anywhere else) did nothing. The patch routes `WM_LBUTTONDBLCLK`,
`WM_MBUTTONDBLCLK` and `WM_RBUTTONDBLCLK` through `doMouseDown()` like their
plain counterparts, so JUCE's own click counter sees both presses:

- `extern/JUCE/modules/juce_gui_basics/native/juce_Windowing_windows.cpp`
  — marked with a `SPECTROCRAFT PATCH` comment; re-apply if JUCE is updated.

With that in place, double-clicking the waveform or spectrogram selects the
whole file (and fits it to view); double-clicking inside a marker region still
selects just that marker's range. Both right-click context menus also gained a
**Select All** item (same action as Ctrl+A).

### Snapshot overlays: dangling-else fix in `FFTAnalysisView::paint`

Stored snapshot curves were silently not drawn in **FFT Linear** and
**FFT Log** axis modes (they only appeared in 1/3 Octave mode). The paint
loop had an unbraced nested `if`:

```cpp
if (axisMode_ == 2)
    if (!snapshots[i].bands.empty())
        drawBandCurve (...);
else
    drawCurve (...);   // C++ binds this else to the *inner* if!
```

By the dangling-else rule the `else` paired with the inner
`if (!bands.empty())`, so in linear/log modes (where `bands` is empty) the
entire statement — including the intended `drawCurve` fallback — was skipped.
The fix adds braces around the then-branch so the `else` binds to the outer
`if (axisMode_ == 2)` as intended. Regression test:
`testSnapshotOverlayPaint()` renders a stored snapshot plus a live curve at a
different frequency headlessly and counts the dim cyan overlay pixels in both
linear and log modes.

---

## AI features

The AI module is built by default (`-DSPECTRO_ENABLE_AI=ON`; disable with
`-DSPECTRO_ENABLE_AI=OFF` for a lean build). It provides neural **noise
suppression** (OpenVINO) and Whisper **transcription** (whisper.cpp b5454
prebuilt binaries, CPU-only). Noise suppression runs on CPU by default; when
the OpenVINO runtime exposes a GPU or NPU device it is used automatically
(*Auto*), and can be overridden per-run in the dialog's *Device* dropdown.
Transcription always runs on CPU — ggml auto-selects an AVX2/AVX-512 backend
for your microarchitecture at load time.

### How it works

- **Noise Suppression** (`Effects → Noise Suppression…`) processes the current
  selection on the visible channels. Models: **DeepFilterNet 2** (recommended),
  **DeepFilterNet 3** (experimental) and **DenseUNet** (legacy). The
  *attenuation limit* slider (dB, default 100 = no limit) controls how hard
  the DeepFilterNet models suppress noise: 6–12 for light reduction,
  18–24 for medium. The result is committed as an undoable edit.
- **Transcription** (`Effects → Transcribe…`) runs Whisper over the whole file
  (visible channels mixed to mono, resampled to 16 kHz with SoXR). A VAD pass
  first detects speech regions and skips long silences entirely — files with
  pauses transcribe several times faster than their length would suggest.
  *Balanced* uses beam search; *Fast* uses greedy decoding (~1.4× quicker,
  slightly lower accuracy on difficult audio). Segments are
  shown in the **Transcription** tab: double-click a row to jump + select its
  range; press Ctrl+F (or right-click > Find) to search, then F3 / Ctrl+F3 to
  cycle through matches.
  Results are embedded in the WAV as a custom top-level `SCTT` RIFF chunk
  (UTF-8 JSON, times in seconds) — written on save, parsed on load, never shown
  in the Markers panel. *Clear* removes them from the document without touching
  the audio.
- **Transcribe Folder…** (`Effects → Transcribe Folder…`) scans a folder tree
  for `.wav` files and transcribes each one headlessly — nothing opens in the
  editor. Each file's `SCTT` chunk is replaced surgically (all other RIFF chunks
  are preserved byte-for-byte). The progress dialog has a Cancel button that
  stops after the current file; a summary lists done/skipped/failed per run.
  *Skip files with SCTT* re-runs only transcribe files without an existing
  transcription.
- **Device selection** — the noise-suppression dialog offers a *Device*
  dropdown: *Auto* (default) picks GPU over NPU over CPU when the OpenVINO
  runtime exposes it, otherwise CPU; picking *CPU*, *GPU* or *NPU* explicitly
  overrides that. A device that is not available falls back to CPU and the
  progress dialog says so. The choice persists between sessions. Transcription
  has no device option: whisper.cpp's prebuilt runtime is CPU-only.
- **Model preferences persist** between sessions (last model, language, mode
  and the auto-download toggle) in the same `SpectroCraft.recent` store as the
  recent-files list. With *auto-download off*, missing models are never
  fetched; the failure dialog then shows exactly where to put them manually.

### Model sources & licenses

| Feature | Model(s) | Source | Size |
|---|---|---|---|
| Noise suppression | DeepFilterNet 2 (recommended) | [Intel/deepfilternet-openvino](https://huggingface.co/Intel/deepfilternet-openvino) (`deepfilternet2/{enc,erb_dec,df_dec}.{xml,bin}`) | ~9 MB |
| Noise suppression | DeepFilterNet 3 (experimental) | same repo, `deepfilternet3/…` | ~10 MB |
| Noise suppression | DenseUNet (legacy) | [Intel Open Model Zoo 2023.0](https://storage.openvinotoolkit.org/repositories/open_model_zoo/2023.0/models_bin/1/noise-suppression-denseunet-ll-0001/FP16) FP16 | ~9 MB |
| Transcription | Whisper tiny / base / small / medium / large-v1 / v2 / v3 (+ q5_1 quantized variants, large-v3-turbo + q8_0) | plain ggml `.bin` from [ggerganov/whisper.cpp](https://huggingface.co/ggerganov/whisper.cpp) | 32 MB – 3.1 GB |

Weights are **never committed to this repository** — they are downloaded on
first use (SHA-256 verified against the registry in `src/AI/ModelManager.cpp`)
or installed manually, and cached under `%APPDATA%/SpectroCraft/models/<id>/`.
The model weights remain under their upstream licenses (Intel's repositories /
OpenAI MIT for Whisper); SpectroCraft itself is AGPL-3.0, which is compatible
with the GPL-licensed DeepFilterNet code this port derives from.

### Manual installation (no auto-download)

If a download fails — or you turn *auto-download* off in the dialog — put the
files yourself into `%APPDATA%/SpectroCraft/models/<model-id>/`:

```
deepfilternet2:\  enc.xml, enc.bin, erb_dec.xml, erb_dec.bin, df_dec.xml, df_dec.bin
denseunet:    \  noise-suppression-denseunet-ll-0001.xml, …-0001.bin
whisper-base: \  ggml-base.bin   (i.e. a single ggml-<name>.bin for every model)
```

The failure dialog always shows the exact destination folder and the download
URLs in a copyable text box. The **OpenVINO runtime DLLs** (`openvino.dll`,
`openvino_intel_cpu_plugin.dll`, `openvino_ir_frontend.dll`, `tbb12.dll`) must sit
next to `SpectroCraft.exe` for noise suppression — they are copied there by the
CMake build (from `extern/OpenVINO/bin/`). A prebuilt exe without them fails
gracefully; even a *partial* set (e.g. only `openvino.dll`) is detected and the
dialog lists all four required files plus where to get them. Transcription needs
the **whisper runtime DLLs** (`whisper.dll` plus 12 ggml/llama DLLs — see
`extern/whisper-b5454/bin/`) next to `SpectroCraft.exe`; the CMake build copies
them there too, and a missing or incompatible set is reported with a clear error.

To use a **GPU or NPU**, install its plugin DLL from the same OpenVINO release
(`runtime\Windows\bin`): `openvino_intel_gpu_plugin.dll` for GPU,
`openvino_npu_plugin.dll` for NPU — next to `SpectroCraft.exe`. Until then the
runtime simply doesn't list that device, *Auto* stays on CPU, and selecting it
explicitly falls back to CPU with a note in the progress dialog.

### Performance notes (CPU)

- Noise suppression runs in near-real-time on modern CPUs for typical selection
  lengths; DeepFilterNet models are ~9–10 MB and fast, DenseUNet is heavier.
- Whisper speed scales with model size: `tiny`/`base` transcribe a minute of
  audio in seconds on CPU; `medium`/`large-v3` take minutes. Use the smallest
  model that meets your accuracy needs for batch runs.
- Transcription runs on CPU via ggml's per-microarchitecture SIMD backends
  (AVX2/AVX-512 auto-selected at load time). `large-v3-turbo` is a good
  accuracy/speed compromise for longer files; the q5_1/q8_0 quantized variants
  cut memory use with minimal quality loss.
- VAD silence skipping + *Fast* (greedy) mode stack: on a 60 s file with two
  20 s speech regions, `whisper-base` drops from ~53 s to ~19 s (beam) or
  ~15 s (fast) on a modern desktop CPU.

---

## Running & CLI flags

```
SpectroCraft.exe                          :: launch GUI
SpectroCraft.exe --run-unit-tests         :: run the unit test suite, exit code 0 on success
SpectroCraft.exe --generate-test-files [dir]   :: write tests/data/*.wav (default: tests/data)
SpectroCraft.exe --test-spectral <in.wav> <out.png> [width height t0 t1]
                                           :: offline spectrogram render, e.g.
SpectroCraft.exe --test-spectral tests/data/tone_10s_stereo.wav out.png 512 256 0.0 10.0
SpectroCraft.exe --test-load <in.wav>
                                           :: headless load timing + mipmap stats,
                                              e.g. for multi-hour files
```

AI builds additionally accept (see [AI features](#ai-features)):

```
SpectroCraft.exe --test-noise-suppression <in.wav> <out.wav> [modelId]
                                           :: headless noise suppression, e.g.
SpectroCraft.exe --test-noise-suppression speech.wav out.wav deepfilternet2
SpectroCraft.exe --test-transcribe <in.wav> [modelId] [language]
                                           :: headless Whisper transcription,
                                              prints the segments
SpectroCraft.exe --test-batch-transcribe <folder> [modelId] [language]
                                           :: transcribes every .wav in a folder
                                              tree and writes each SCTT chunk
SpectroCraft.exe --test-ns-failure-alert <in.wav> [modelId] ["fake error"]
                                           :: shows the AI failure dialog (with a
                                              copyable details box)
```

The test executable accepts the same flags (useful for CI without a display).

---

## Keyboard shortcuts

| Action | Shortcut |
|---|---|
| Open file | Ctrl+O |
| Close active file | Ctrl+W or Ctrl+F4 |
| Cycle through loaded files (forward / backwards) | Ctrl+Tab / Ctrl+Shift+Tab |
| Save / Save As | Ctrl+S / Ctrl+Shift+S |
| Undo / Redo | Ctrl+Z / Ctrl+Y |
| New file (empty document, choose channels + rate) | Ctrl+Shift+N |
| Cut / Copy / Paste | Ctrl+X / Ctrl+C / Ctrl+V |
| Paste Mix (add the clipboard at the selection start instead of replacing; appends past end-of-file if it extends that far) | Ctrl+Shift+V |
| Select all | Ctrl+A |
| Delete time selection (or **zero the spectral selection** when one is active) | Del / Backspace |
| Play / Pause toggle | Space |
| Move playhead in 10 ms steps left / right | ← / → |
| Move playhead one sample left / right | Ctrl+← / Ctrl+→ |
| Select while moving the playhead (anchor = position at first Shift press; released when Shift is let go) | Shift+← / Shift+→ |
| Fine selection, one sample per step | Ctrl+Shift+← / Ctrl+Shift+→ |
| Move playhead and pan both views to the start / end (keeps the current zoom span) | Home / End |
| Loop (selection, or whole file if nothing selected) on/off | Ctrl+L |
| Find in the active list panel (Markers / Transcription tab; right-click > Find also works) | Ctrl+F |
| Find next match (opens the dialog first when no query is set yet) | F3 |
| Find previous match | Ctrl+F3 |
| Add marker (region if selected, else point at cursor) | F2 / F8 |
| Selection tool: Time Range / Marquee / Lasso / Paintbrush | S / M / L / P |
| Brush radius smaller / larger (Paintbrush only) | [ / ] |
| Paintbrush: straight line from the last stroke's end point to the click position (straight in linear-frequency space) | Alt+click |
| Clear the active spectral selection (time selection untouched) | Esc |
| Zoom in / out (horizontal, both views) | Ctrl+= / Ctrl+- |
| Zoom to fit (both views) | Ctrl+0 |
| Vertical zoom in / out (waveform amplitude + spectral frequency) | Ctrl+Shift+= / Ctrl+Shift+- |
| Toggle a channel's visibility (like the View menu items); show ONLY that channel with Shift held | Ctrl+1..9 / Ctrl+Shift+1..9 |
| Cycle view layout: both → waveform only → spectrogram only (reverse direction with Shift) | F9 / Shift+F9 |

Keyboard focus follows the mouse: clicking either view puts keyboard focus on
that view, so ←/→ and Shift+←/→ work immediately after any click inside a plot.
Clicking neutral areas (splitters, scrollbars, empty space) lands focus on the
main window itself — arrow keys keep working there too. Interactive widgets
(combo boxes, text fields, the marker table) take focus when clicked and keep
the standard Windows behaviour for arrow keys while focused; click back into a
view to return playhead control.

Menus: **File, Edit, Selection, View, Transport, Effects, Window, Help** (the
Window menu opens/closes the individual left-column panels). The Edit menu has a
checkable **Snap to Zero Crossings** toggle (**off by default**): when on,
dragging out a new time selection snaps both edges to the nearest zero
crossing of the first visible channel (within ±50 ms), so cuts and effects
start/end where the waveform crosses zero instead of clipping mid-cycle.
Programmatic selections (markers, keyboard steps) are never snapped. The Edit menu's
Delete item is labelled **Zero Spectral Selection** while a spectral selection is
active (it then zeros the selected time/frequency content in place instead of
cutting samples). Copy/Cut respect an active spectral selection too: only the
selected (time, frequency) content goes onto the app-level clipboard, which is
shared across all open files — you can copy from one file and paste into
another. **Copy to New File** and **Paste Mix** are described above. Effects offers Silence,
Fade In/Out, Normalize, Reverse and **Change Gain...** (a dB dialog whose
field opens focused with its value pre-selected so a new one can be typed in
right away; it also has a -60..+24 dB slider kept two-way synced with the box —
dragging rewrites the field, typing moves the slider clamped to its range,
and OK applies whatever is in the field; it multiplies the selection by
10^(dB/20)) — all applied only to
the currently visible channels. The View menu has **Zoom to Selection** / **Zoom Out Full**,
a **Colour Map** submenu
(Classic default), checkable **Waveform View / Spectrogram View** items (at
least one always stays visible) and one checkable item per channel; plain click
toggles that channel, **Ctrl/Shift+click shows only it**.

**Remembered between sessions**: the spectrogram's Log/Linear state and FFT
size, the Frequency Analysis axis mode (FFT Linear / FFT Log / 1/3 Octave /
Freq vs Time / Amp vs Time / Amp vs Freq), its independent FFT size, and the
curve-mode options (Ref Time, Smooth, Track) are saved to the same properties
file as the recent-files list and restored on startup.

Mouse (waveform *and* spectral views):

| Gesture | Action |
|---|---|
| Click | Move the playhead to that position in **both** views (selection drags start from it) |
| Left-drag | Select a time range |
| Left-drag on the left axis | **Vertical pan**: drag up/down to shift the amplitude window (waveform) or frequency window (spectral, log-aware). The content follows the cursor at half speed; panning against a range limit pins that edge and keeps the span. Does not move the playhead |
| Left-drag on the top time ruler | Pan the shared time window at half cursor speed (same as middle-drag); does not create a selection |
| Middle-drag | Pan the shared time window (both views stay in sync). The content follows the cursor at half speed; panning against a file edge pins that edge, and reversing direction responds immediately (no dead zone) |
| Right-drag a region | **Box zoom**: dragging on the track zooms **both** axes at once — time from the box's horizontal span, amplitude (waveform) or frequency (spectral) from its vertical span. Dragging that starts on an axis zooms only that axis: left axis → vertical range, top time ruler (waveform) → time range. A rubber-band rectangle is shown while dragging |
| Right-click | Context menu: Zoom to Selection, Zoom Out Full, Select All (Ctrl+A), Cut / Copy / Paste / Delete, plus a **Scale** submenu (time axis Time/Time Offset.../Sample; vertical axis Wav Units/dB/Normalized in the waveform, Hz/Custom... in the spectrogram; checkable Alternative Scale time/frequency overlays there). On the **left axis**, "Zoom Out Full" resets that view's *vertical* range (amplitude/frequency) instead of the time window. In the spectral view, right-clicking **on the current spectral selection** additionally offers **Move Selection** and **Scale Selection** |
| Mouse wheel over the plot / time ruler | Horizontal zoom around the cursor (both views) |
| Mouse wheel over the left axis (or Shift + wheel anywhere) | Vertical amplitude zoom (waveform) / frequency-range zoom around the cursor (spectral) — independent per view |
| Drag the divider between the views | Resize waveform vs spectral area |
| Double-click a marker (line/region) | Set the selection to that marker's range (both views) |
| Double-click elsewhere | **Select the whole file** and fit it in both views |

Selecting a time range in *either* view applies the selection to **both**
views at once.

### Axis scales (right-click → Scale)

Both views share a right-click **Scale** menu that changes how axis values are
*displayed*. The underlying data, zoom windows and selections never change;
the settings apply session-wide (rulers, footer fields, playhead readout,
cursor probes and the marker panel) until changed again and are not persisted
across restarts.

- **Time Axis** (both views):
  - **Time** (default): seconds, with clock-style labels past a minute.
  - **Time Offset...**: a dialog asks for the time that should appear at the
    playhead position (`hh:mm:ss.xxxx`); from then on every time readout shows
    *time + offset*, and typing a clock value in any footer/marker field applies
    it with the offset removed. Ruler ticks land on round clock values.
  - **Sample**: times are shown as sample counts at the active file's rate;
    ruler ticks land on nice 1-2-5×10^k sample steps, and footer/marker fields
    accept integer or decimal sample counts (the length field is a duration in
    samples).
- **Vertical Axis** (waveform): how the left amplitude axis is labelled —
  **Wav Units** (physical value when calibrated, relative dB otherwise),
  **dB** (plain 0/−6/−12…) or **Normalized** (signed linear amplitude: +a above
  and −a below the centre line). Grid-line positions are unchanged.
- The spectrogram's Scale menu instead offers a **Vertical Axis** for its
  frequency axis: **Hz** (default) or **Custom...**. Custom arms a reference
  click — click any point in the plot, then enter the value that should appear
  at its frequency; the whole axis is relabelled by the resulting factor while
  tick positions stay put.
- **Alternative Scale** (spectrogram only, checkable): a second linear scale
  anchored at zero, shown alongside the normal readout in the status-bar probe
  as `| alt t=…` / `| alt f=…`. Enabling one arms the same reference-click flow:
  click a point and enter its value; the overlay rescales time or frequency by
  *value/ref*. Disabling just hides the overlay.
- While both views are visible, the spectrogram hides its own (identical) time
  ruler to save plot space; it reappears when only the spectrogram is shown.

### Spectral selections (Selection menu)

Besides plain time ranges, you can select arbitrary regions of the
time/frequency plane and apply edits to exactly that content. The
**Selection** menu switches the drawing tool:

| Tool | Key | How to draw |
|---|---|---|
| Time Range (default) | S | Left-drag a time range, as before |
| Marquee | M | Left-drag a rectangle in either view's plot area |
| Lasso | L | Hold and drag through the vertices of the region; releasing closes the polygon (≥3 points) |
| Paintbrush | P | Drag a freehand stroke; its width is the brush radius (Hz) |

Modifier keys while dragging: **plain = replace** the current selection,
**Shift = add** to it, **Ctrl = subtract** from it (subtractive shapes are
drawn with dashed edges). With the paintbrush, **Alt+click** draws a straight
line from the last stroke's end point to the click position; the line is
straight in *linear-frequency* space, so on a logarithmic frequency axis it
appears slightly curved (switching the view to a linear scale shows it as a
perfectly straight line). Shift/Ctrl combine with Alt for add/subtract lines. The active tool and brush radius are shown in the
status bar; while holding **Shift** or **Ctrl**, it additionally shows "Add Selection" / "Subtract Selection", so you can always see how the next drag will edit the selection. `[` / `]` change the brush radius (5–5000 Hz), and **Esc**
clears the spectral selection without touching the time selection.

**Moving / scaling an existing selection**: in the spectral view, right-click
on the current spectral selection and choose **Move Selection** or **Scale
Selection**. The first click sets a reference point (shown with the same round
orange indicator as Pick Points); moving the mouse — button held or not —
updates the selection live: Move shifts it by the cursor's offset from the
reference, Scale stretches it about the reference (moving one full span of the
selection doubles that axis). Holding **Shift** locks to X-only or Y-only based
on which direction the mouse has moved more. A second click applies the change;
Esc or a right-click cancels and restores the original selection.

With a spectral selection active, destructive edits operate on its content
over its own time extent instead of the time selection: **Del/Backspace**
zeros it, Silence / Fade In / Fade Out / Normalize / Reverse / Change Gain
apply to the selected (time, frequency) region only — e.g. you can remove a
hum or a tone from one channel's band while leaving everything else
bit-exact. The waveform and spectral views both refresh immediately after
the edit, and undo/redo works as usual. **Play Spectral Selection**
(Transport menu) plays back with the selection masked out in real time,
without modifying the file.

### Selection panel (named selections + clone & multiply)

The **Selection** tab in the left panel manages *named* spectral selections
for the active file and provides **Clone & Multiply**, a tool for picking out
harmonics of whatever is currently selected.

- **Store / Recall / Delete**: Store saves the current spectral selection under
  a name (storing again with the same name updates that entry); Recall replaces
  the current selection with a saved one (double-clicking a row does the same)
  and makes sure both views show its time range — panning or zooming out when
  it is not fully inside the current window. The list belongs to each file, and
  when **Save to disk** is on it is kept in
  sync with `<file>.selections.json` next to the audio file so it survives
  restarts.
- **Load from disk...**: right-click the panel (or drop a `.selections.json`
  file onto it) to re-read saved selections from disk — the sidecar next to
  the audio file is used when present, otherwise you pick any JSON file. The
  in-memory list is replaced by what the file contains, and loading the
  canonical sidecar also turns **Save to disk** on. A sidecar next to a wav is
  already loaded automatically whenever that wav opens.
- **Clone & Multiply**: enter frequency multipliers such as `0.5, 2, 5` and
  press Apply — additive clones of every shape in the current selection are
  appended at those multiples (the original stays in place), with each clone's
  thickness stretched by the same factor. This is exactly what you need to
  select harmonics: pick a fundamental band, then multiply by 2, 3, 5...
- **Pick points...** switches to interactive mode: click a reference point in
  the spectrogram (shown as an orange circle), then click each target
  frequency — every click appends clones scaled by `clicked / reference` (e.g.
  reference 100 Hz plus clicks at 50/200/500 Hz gives multipliers 0.5, 2 and
  5). Picked points are drawn as circles on every channel row; clicking near
  an already-picked point removes it (and the clones its pick added), so a
  stray click can be undone without Esc. **Esc** or the button ends the session
  without touching the selection.

---

## Architecture

Clean separation into independent components (see `include/` + `src/`):

| Component | Responsibility |
|---|---|
| `AudioEngine` (`juce::AudioSource`) | File I/O via AudioFormatManager, streaming vs in-memory decision, sample access API, mipmap pyramid for the waveform overview, playback transport (play/pause/loop), edit primitives (`applyPatch`, silence/fade/normalize/reverse/cut/copy/paste/delete) plus `initNew` (empty or pre-filled documents with no file path yet), `readRange` and `pasteMixAt` (sum-in-place paste that appends past end-of-file). All disk reads are mutex-protected; the UI thread never blocks on I/O. |
| `SpectralEngine` | Tiled multithreaded FFT: level selection from zoom, tile scheduling on a `juce::ThreadPool`, LRU cache keyed by (tile, channel, level, fftSize, hop, window). Tiles store **raw dB magnitudes**; gain/bias/dB-range/colour-map scaling is applied per pixel at render time, so changing those settings never requires recomputing tiles. Bilinear sampling into RGBA pixels, colour-map LUTs, headless PNG rendering. Never materializes more than one tile per channel at a time — multi-GB files stream through the reader tile by tile. |
| `WaveformView` / `SpectralView` (`juce::Component`) | Pure drawing + mouse interaction; hold references to their engines and repaint on change messages. Each view holds its own time-window state and its **vertical axis/zoom level** (amplitude for the waveform, frequency range for the spectral view); at application level all horizontal changes flow through `MainWindow::applyTimeWindow`, which keeps both views on the same time range while vertical zooms stay independent. The spectral view requests tiles for its visible range only. Selection, markers and playhead are drawn as overlays *on top of* the audio in both views, so they behave identically wherever you look. |
| `ScrollBars` (`TimeScrollBar`, `ValueScrollBar`) | Custom scrollbar components: thumb shows the displayed portion relative to the whole track; centre-drag pans, edge-drag zooms one side, click outside jumps. One horizontal time bar plus one vertical bar per view. |
| `Panels.cpp`: `FileListPanel`, `FFTAnalysisView`, `SpectralControlsPanel`, `MarkerListPanel`, `SelectionPanel`, `PythonPanel` | Classic DAW-style left column: the **Files** list (every loaded document with its properties; double-click activates, right-click Activate/Close/Set Unit and Scale/Open Containing Folder), live FFT analysis with cursor readout (drawn in frequency space — never indexes the magnitude vector by pixel x), a selectable axis mode (FFT Linear / FFT Log / 1/3 Octave band plot, plus Freq vs Time / Amp vs Time / Amp-vs-Freq curve modes with Ref Time / Smooth / Track options and their own per-mode snapshot store) and harmonic reference lines (right-click **Set Harmonic References** / **Copy Harmonics** / **Reset Harmonic**), spectral settings (colour map/dB/Log-Linear toggle/Reset Freq/FFT size), the marker table, named spectral selections with clone & multiply (`SavedSelections` JSON sidecar — auto-loaded on open, re-loadable via right-click "Load from disk..." or by dropping a `.selections.json` onto the panel), and the **Python** script editor (dropdown + Add/Remove/Run over a multi-line editor). Panels mutate shared state through `Application` and notify `MainWindow` via `app.onStateChange`. |
| `PythonScripts` | Python panel support: per-user script folder management (`%APPDATA%/SpectroCraft/python`, auto-created with three example scripts), interpreter probing on the PATH, metadata JSON building, float32 WAV temp-file I/O and cross-platform child-process launching (CreateProcess/fork) with cancellation. `Application::runPythonScript` exchanges data with the script through temp files and commits the result via the normal undoable edit path. |
| `MarkerManager` | Point/region markers with title + comment, sorting, find-near, JSON sidecar persistence next to the audio file. |
| WAV marker writer (`AudioEngine::saveWavWithMarkers`) | Custom RIFF writer for `.wav` saves: `fmt` (IEEE float32), `cue` (one entry per marker at its start sample), `LIST`/`adtl` with `labl` (title) + `ltxt` (comment) entries, then the raw interleaved `data`. All chunk sizes are computed up front; streaming files are written in 1 MB blocks. |
| `UndoManager` / `UndoPatch` | 50-level undo/redo stacks of compact patches (old/new sample data + length delta) instead of full snapshots. |
| `FileDoc` | One per loaded file: its own `AudioEngine`, `MarkerManager` and `UndoManager`, plus the remembered playhead position, selection range, visible-channel list and scale calibration (`ScaleCalibration`). Created by `Application::openFile` (from disk) or `Application::newFile` / `copyToNewFile` (path-less documents); destroyed on close (the audio player is detached from its engine *before* destruction). |
| `Application` | Owns the document list (`std::vector<std::unique_ptr<FileDoc>>`) and the active-document index; exposes the active doc's engine/markers/undo through accessors (empty fallback objects while no file is loaded); handles open/activate/close/cycle, keeps the mirrored UI state (selection/playhead/channels) in sync with the active document, wires edit actions to undo. Also owns the app-level clipboard (shared across files), and implements `newFile`, `copyToNewFile` (visible-channel-limited, spectral-selection-masked) and `pasteMix`. |
| `MainWindow` (`DocumentWindow` + `MenuBarModel` + `Timer` + `FileDragAndDropTarget`) | Classic DAW-style layout (left tab groups + waveform/spectral main area + scrollbars), menus, keyboard shortcuts (incl. Space play/pause, Home/End, F8 marker), drag & drop, playhead animation timer and view-range state sync between views/panels. |

Threading model: the UI thread only draws and dispatches; FFT tiles are computed
on a pool sized to hardware threads; file reads happen on the reader under a
mutex with bounded copies. Tile results are published through an LRU cache guarded
by a single mutex (short critical sections, no lock held during FFT work).

---

## Tests (`tests/`)

- `UnitTests.cpp` — lightweight CHECK-based suite (~1800 checks), run via
  `SpectroCraftTests.exe` or `SpectroCraft.exe --run-unit-tests`:
  - **FFT**: peak bin of a known sine matches the expected frequency; buffer
    layout and normalization verified.
  - **Markers**: ids, sorting, rename/move/remove, find-near, JSON sidecar
    round-trip; region markers spanning a selection vs point markers at cursor;
    per-channel visibility toggling (arbitrary subsets).
  - **Undo/redo**: silence → undo restores samples → redo re-silences; stack
    depth; fade-in undo regression (per-sample edits must capture old data
    before the in-place modification, or undo is a no-op).
  - **FFT follow-playhead window**: with a very narrow selection and
    follow-playhead mode on, the panel still analyses a full-size window
    centred on the cursor — a pure tone peaks at its true frequency instead of
    smearing into a wide low-frequency hump.
  - **Click updates + selection averaging**: in a two-tone file (440 Hz then
    880 Hz), clicking in each half moves the analysis with the cursor, and a
    selection spanning both halves is averaged over many windows so that both
    tones appear while far bins stay quiet.
  - **1/3-octave step drawing**: a single tone renders as a flat-topped step
    across its whole band cell (topmost curve pixel at the same height in
    every column of the cell), not a smooth curve.
  - **Change Gain**: +6 dB scales the selected samples by exactly 10^(dB/20),
    leaves everything outside the selection untouched, and undo restores the
    original level.
  - **Change Gain dialog**: on open the dB field already has keyboard focus
    with its initial "0.0" fully selected (so typing replaces it immediately)
    while Enter/Escape still trigger OK/Cancel; the slider below the box is
    verified two-way synced — dragging rewrites the field without a feedback
    loop, typing moves the slider clamped to -60..+24 dB, and half-typed text
    like "-" leaves it alone.
  - **Selection / zero-crossing snapping** and duration clipping.
  - **Spectral engine end-to-end**: renders a real PNG from a generated WAV,
    verifies non-trivial output, cache population/clearing.
  - **Headless view-paint regressions**: spectral view (tone lands on the
    expected log-scale row), waveform view (min/max envelope above *and* below
    centre — catches gradient-style rendering bugs), Frequency Analysis panel
    (paint with tiny selections, with a 65536-point FFT — regression for an
    out-of-bounds magnitude read that crashed the tab — and in all three axis
    modes: linear peak position, log peak position, and 1/3-octave band values
    peaking in the correct IEC band).
  - **Independent view components**: driven directly, each view keeps its own
    time window without cross-talk and has an independent vertical zoom
    (amplitude / frequency axis); at app level `MainWindow` syncs the horizontal
    range of both views.
  - **Scrollbars**: thumb geometry, centre-drag pan, edge-drag zoom, click-to-
    jump and clamping — all headless.
  - **WAV marker chunks**: saves a WAV with markers (title + comment) and
    parses the RIFF back — exact chunk order (`fmt` → `data` → `cue` →
    `LIST`), 24-byte `cue` entry layout, `adtl` sub-chunk walk for ltxt/labl/
    note texts, data-chunk size, plus a JUCE round-trip reload of the saved
    file. A byte-exact test re-saves a classic DAW reference WAV with identical
    markers and compares every output byte against the original (except the
    RIFF total-size field, which covers exporter-specific extra chunks).
  - **Effect channel scoping**: stereo ramp/DC fixture; silence/normalize/reverse
    restricted to one visible channel leave the other bit-exact and undo
    restores everything.
  - **Multi-file support**: two files open at once — per-file playhead/
    selection restoration on switch, Ctrl+Tab cycling (with wrap-around),
    independent marker lists and undo histories per file, closing a non-active
    doc leaves the active one untouched, closing the last doc resets to the
    empty state, re-opening an already-loaded file activates it without
    duplicating. Shared snapshots: one stored from each file survives switching,
    "Convert to marker" lands in the snapshot's source file (even when another
    file is active), and closing a file clears only that file's snapshots.
  - **Open Recent**: adding entries deduplicates and keeps most-recent-first,
    caps at ten, persists across app instances via the `PropertiesFile`, clear
    empties the list, stale paths are skipped on load, and the menu handler
    re-opens a live entry while silently dropping a deleted one.
  - **History tab**: rows carry human-readable action names ("Changed gain by
    +6.00 dB", "Silenced selection", ...), undone actions are marked as future
    entries, and jumping to any row applies the right batch of undo/redo steps —
    verified against captured original data at every jump target.
  - **Batched undo/redo**: `undoSteps`/`redoSteps` move several patches in one
    call (single mipmap rebuild), clamp when more steps are requested than
    available, and restore/re-apply the exact sample data.
  - **Deletion side-state**: deleting a time range shifts/trims/drops markers,
    adjusts named saved spectral selections (rectangles trim/shift/drop;
    polygons and strokes straddling the cut split into two contiguous pieces),
    and shifts/trims/clears stored FFT snapshots *and* curve-mode snapshot
    slots of that file — all verified end-to-end through `del()` with undo and
    redo restoring every piece.
  - **Settings persistence**: spectrogram log/linear + FFT size, frequency-
    analysis axis mode (all six modes) + its independent FFT size, the
    curve-mode options (Ref Time / Smooth / Track), and the zero-crossing snap
    toggle all survive an app restart via the properties file; stored
    non-power-of-two sizes round up to the next power of two on load.
  - **Axis scale modes** (right-click Scale menu): time-axis Time / Time
    Offset... / Sample and vertical-axis Wav Units / dB / Normalized (waveform)
    or Hz / Custom... (spectrogram), plus checkable Alternative-Scale overlays —
    verified headlessly through the pure conversion helpers, per-mode ruler
    rendering diffs, probe text in Sample/TimeOffset modes, frequency-factor
    relabelling, StatusBar field/playhead round-trips with typed input, and the
    marker panel's time cells + detail editors.
  - **Zero-crossing snap selection**: with the toggle off (the default) drag
    edges are untouched; with it on both edges land on tone zero crossings;
    zero-length and programmatic selections are never snapped. The analysis
    view's combos write their axis mode / FFT size back to the shared settings.
- `tests/data/` — generated fixtures (regenerate with `--generate-test-files`):
  - `tone_1s_mono.wav`, `tone_10s_stereo.wav` (ch0 = 400 Hz, ch1 = 600 Hz),
    `sweep_10s_mono.wav` (100→5100 Hz), `noise_10s_8ch.wav`,
    `tone_1h_32ch.wav` (header + sparse stub for streaming-path tests).

### Visual verification

The offline renderer makes visual comparison reproducible:

```bat
SpectroCraft.exe --test-spectral tests/data/tone_10s_stereo.wav out_tones.png 512 256 0.0 10.0
SpectroCraft.exe --test-spectral tests/data/sweep_10s_mono.wav   out_sweep.png 512 256 0.0 10.0
```

Verified results (log frequency axis, 20 Hz–20 kHz, Classic colour map):

- **Tones**: the 400 Hz and 600 Hz tones appear as bright horizontal lines at
  exactly their log-scale positions (rows ≈ 145 and 130 in a 256 px image —
  computed from `row = h·(1 − ln(f/20)/ln(20000/20))`). The surrounding glow is
  genuine Hann-window spectral leakage of off-bin tones (confirmed against an
  independent Python DFT reference: per-bin magnitudes match to 3 decimals).
- **Sweep**: a clean diagonal from bottom-left (~100 Hz) to upper-right
  (~5100 Hz), matching the log axis.
- The C++ FFT pipeline was cross-checked sample-exact against a pure-Python DFT
  of the same windowed frames (identical dB-normalized magnitudes at all tested
  bins).

For human verification: open `out_tones.png` / `out_sweep.png` next to a classic
DAW's spectral view of the same files — line positions, colour ramp and overall shape
should agree.

---

## Project layout

```
icon.ico                application icon (embedded via include/IconData.h)
CMakeLists.txt            root build (JUCE vendored in extern/)
include/                  component headers
src/                      app sources + Main.cpp (CLI flags)
tests/                    UnitTests.cpp, CMakeLists.txt, data/ fixtures
tools/                    auxiliary diagnostics (address→symbol resolvers,
                          GDI stress test) used while debugging this project
extern/JUCE               vendored JUCE 9.0.4
requirements/             original task requirements & reference screenshots
```
