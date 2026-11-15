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
  (PCM 8/16/24/32-bit int, 32-bit float), up to 32+ channels — plus the audio
  track of video containers (.mp4/.m4a, .mkv, .avi, .webm, .mov, .wmv, .flv,
  .ts, .wma, .ac3, ...) through a vendored minimal FFmpeg build on Windows,
  and .mp4/.mov/.m4v/.3gp through AVFoundation on macOS (see **Video files**
  below).
- **Video window** (Windows, macOS): the picture of a video file in its own
  window, in step with the spectrogram — it follows playback, the playhead and,
  optionally, the mouse hovering over the spectrogram or waveform.
- **Recording** (Windows): capture from any recorder plugin — WASAPI capture and
  loopback devices ship out of the box, and third-party DLLs (ASIO, network,
  ...) can be dropped into `recorders/` without recompiling. The waveform and
  spectrogram update live while recording; see **Recording** below.
- **Streaming engine**: files larger than ~256 MB of samples stay on disk and are
  read through the `AudioFormatReader` under a mutex; small files are materialized
  in memory for fast editing. Edits require the in-memory buffer.
- **Waveform view**: mipmap pyramid overview, own time window + horizontal zoom,
  own vertical amplitude axis with dB gridlines and its own vertical zoom level,
  selection with optional zero-crossing snapping, persistent playhead — a thin
  red line that continues into the time axis strip (click to move it) — and
  loop playback. When several channels are displayed each gets its
  own row separated by a thin dark gap, and every row shows its channel label
  ("L"/"R" for stereo, numbers otherwise) — even when only one channel is
  visible. When zoomed in to ~4 samples per pixel or fewer the
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
  Each vertical bar spans only the track data area — when that view draws its
  own time ruler strip (top or bottom) the bar stops at the actual data height
  instead of running into the axis. Dragging the thumb centre pans, dragging an
  edge zooms in/out in that direction, clicking outside jumps.
- **Spectral view**:
  - Tiled FFT: fixed 256 frames/tile, multi-resolution levels (hop doubles per
    level), computed in parallel on a `juce::ThreadPool`, LRU tile cache.
  - Own frequency axis (independent of the waveform view): log or linear scale,
    configurable min/max frequency, "Reset Freq" button, own vertical zoom via
    wheel over the frequency axis strip (left or right).
  - Per-channel rows: when several channels are displayed each gets its own row
    with **its own frequency axis**; a 5 px dark gap separates the rows (the
    same shared layout as the waveform view). Channel labels ("L"/"R" for
    stereo, channel numbers otherwise) are drawn in the axis strip of every
    row — even when only one channel is visible — at its right edge when the
    axis sits on the right so they never collide with the tick labels.
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
  persistence is automatic via [per-file metadata](#per-file-metadata):
  markers live either inside the WAV's RIFF `cue` + LIST/`adtl` chunks (the
  classic DAW marker layout) or in a `.meta.json` sidecar, depending on the
  file's storage mode — files saved with that layout load their markers back
  automatically. Sorted access, find-near. "Add Marker" (button or `M`) creates a **region
  marker spanning the current selection** if there is one, otherwise a point
  marker at the cursor. Every file has its own marker list — switching files
  shows that file's markers, which are restored from the file's metadata store
  on load — and **double-clicking a
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
  the parsed markers and clamps them to the file duration).
  Marker overlays depend on which views are visible: the **waveform always**
  draws the full overlay (vertical line for points, top bracket line + two
  vertical lines + yellow title for regions); while it is stacked above,
  the **spectrogram shows only the vertical delimiter lines**; a lone
  spectrogram draws the full overlay itself. The dedicated tables track also
  shows the vertical delimiter lines only. All overlays stay in sync with
  the playhead, selection and zoom.
- **Markers inside WAV files**: saving as `.wav` embeds the markers into the file
  itself using standard RIFF chunks — a `cue` chunk with one entry per marker at
  its start sample, plus a `LIST`/`adtl` sub-chunk containing one `labl`
  (title) and one `ltxt` (comment) entry per marker. The audio data is written
  as IEEE float32 exactly as before; other formats keep the plain JUCE writer.
- **Drawing panel**: annotate the spectrogram with **text, arrows, rectangles
  and freehand strokes** in an adjustable colour/width/text size (the last-used
  values persist between sessions). Every drawing is stored in data coordinates
  so it stays anchored to the spectrum under zooming and panning; the Drawing
  tab lists each item with a Type + Comment column, a per-item visibility eye
  icon (toggling it never moves the viewed data section) plus a master Show/Hide
  switch, Excel-style multi-selection that fits all selected drawings into view
  (10% margin on every side, and only when they are not already fully visible),
  right-click **Edit...** and **Delete Selected**. New items are created from the
  panel's context menu or with **T / A / R / D** by clicking (text) or dragging
  (the rest) in the spectrogram — arrows point toward where you started dragging,
  so you place the arrowhead first and pull the shaft back; **Esc** cancels a
  pending creation. Drawings persist with the file
  via [per-file metadata](#per-file-metadata), exactly like selections.
- **Classic DAW waveform-editor layout** (the *Default* workspace; every
  panel can be moved, docked anywhere or undocked — see *Docking workspace*
  below):
  - Top-left tab group: **Files** (lists every loaded file with its sample
    rate, channel count and duration — plus its scale calibration when set;
    double-click a row to make it active, right-click for Activate / Close /
    Scale… (opens the Scale form) / Open Containing Folder), **History**, and
    **Frequency Analysis** (visible by default): live FFT with dB/Hz axes and
    a cursor readout.
    A single click in either view re-analyses immediately: with no selection
    it shows one full-size window centred on the cursor (and follows the
    playhead while playing); with a selection it analyses the whole selection
    at the selected FFT size — when the selection is wider than one window,
    several evenly spaced windows are computed and their magnitudes averaged
    (the classic "average over the selection" behaviour). Its FFT size is
    **independent of the spectrogram's**, plus a window-type selector
    (Rectangular/Hann/Hamming) and a frequency-axis dropdown with six modes
    (the dropdown widens to show its full item text whenever the panel is wide
    enough; the eight snapshot buttons always keep their slots):
    **FFT Linear** (linear Hz axis), **FFT Log** (logarithmic axis, the default)
    and **1/3 Octave** (IEC 61260-1 band plot from 50 Hz to 16 kHz — one curve
    per visible channel, each band drawn as a flat-topped step across its
    frequency edges with vertical connectors at the boundaries — the classic
    1/3-octave band-plot look; every click replaces the curve instead of
    adding another. When the file is calibrated in **Pascal**, this mode
    switches to a dedicated dB-SPL pipeline that reproduces the numbers of
    classic reference analysers: fixed 65536-point symmetric-Hann frames at
    50% overlap, frame-averaged power spectrum, Butterworth band weighting and
    the classic +98.5 dB offset — levels then read in **dB-SPL** up to ~20 kHz,
    and an **Order** dropdown (1/2/3/4/5/10/20/30/40, default 40) appears to
    the right of the FFT size / window controls since the frame size is fixed),
    plus three
    curve-analysis modes:
    **Freq vs Time** (the tracked dominant frequency plotted against time —
    the cursor readout also shows each channel's level), **Amp vs Time**
    (level over time) and **Amp vs Freq** (a per-bin spectrum of the analysed
    region; it uses fixed 2048-point Hann frames, so the FFT size / window type
    controls are hidden in that mode). The curve modes analyse the selection
    frame by frame with fixed 2048-point Hann frames (capped at 2048 points per
    curve) and honour a spectral-selection mask — bins outside it do not count.
    Their options row offers **Smooth** (None / 3 / 5 / 7 / 9 / 11 / 21 / 51-
    frame centred moving average) and **Track** (Max = strongest component per frame/bin with
    its frequency; Total = summed level with amplitude-weighted centroid
    frequency; Weighted = same as Total for the level, using the weighted mean
    frequency). The two time-based modes add a **Ref Time** option (Relative =
    ticks counted from the selection start, Absolute = file time) and have a
    horizontal view window over the analysed region: the mouse wheel zooms
    only the axis under the cursor — the time window when over the plot or
    the bottom time ruler, the frequency/level window when over the vertical
    axis strip (or with Shift held), like the other graphs; a right-drag box
    sets both windows (the box's x span becomes the time window and its y span
    the frequency/level window — in Freq vs Time the vertical axis is
    frequency), dragging that starts on an axis zooms only that axis,
    left-dragging the bottom axis pans the time
    window, **middle-dragging anywhere in the plot pans both axes at once** —
    the horizontal range (time in the curve modes, frequency in FFT Linear / FFT
    Log / 1/3 Octave / Amp vs Freq) and the vertical one (level, or frequency in
    Freq vs Time) — at half cursor speed with the same incremental scheme as the
    waveform and spectrogram: the step is measured from the previous cursor
    position, an edge clamp never leaves a dead zone and reversing responds at
    once. In the log frequency axis the pan works in log space so the zoom level
    is preserved. The right-click menu's checkable **Zoom Fit** fits both windows
    to every displayed curve (live + shown stored curves) — while checked,
    each new time selection zoom-fits automatically; any manual zoom (pan,
    box or wheel) turns auto-fit off, and switching axis modes resets it to on.
    The 8 snapshot buttons keep working in these modes but store a *separate* set of
    curve snapshots per mode (a slot saved in Freq vs Time is empty when you
    switch to Amp vs Time); stored curves follow the current Track/Smooth
    options, exactly like classic snapshots follow the current FFT size.
    In **Freq vs Time** only, the view's right-click menu offers **Check
    Harmonic Ratio**: every displayed curve (the live one plus stored curves
    1-8) is divided by the curve with the lowest average frequency - the
    fundamental - so a signal carrying exactly the k-th harmonic of another
    plots as a flat line at "kx". While active, the vertical axis switches
    from Hz to ratio units (gridlines labelled "1x", "2x", ...), entering and
    leaving the mode zoom-fits the window, the cursor readout shows ratios,
    wheel-zoom/pan operate in ratio space, and Copy exports `chN_x` columns.
    The snapshot buttons' right-click menu adds **Clear All**, which wipes all
    eight stored curves of the active store type (classic snapshots in modes
    0-2, curve snapshots in modes 3-5) at once.
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
    spanning the snapshot's time range in its source file), **Clear** (same as
    Shift+click) and **Clear All** (wipes every stored curve of the active
    store type at once).

    The analysis view has **display ranges fully decoupled from the FFT
    settings**: magnitudes are always normalised against a fixed −160 dB
    floor, while what you *see* is controlled by two independent windows —
    a frequency window (default 20 Hz–20 kHz in log mode) and a level window
    (default 0…−120 dB). Interactions:

    - **Mouse wheel** over the plot zooms the frequency window around the
      cursor — same direction as the tracks: wheel up zooms in, wheel down
      zooms out; **Shift+wheel** (or wheel over the right level axis) zooms
      only the level window instead, anchored on the cursor.
    - **Right-drag a box** (a rubber-band rectangle is shown while dragging):
      horizontal span sets the frequency window, vertical span sets the level
      window (both at once if you drag in both directions). A plain right-click
      shows a context menu with checkable **Zoom Fit** (fits both windows to
      every displayed curve and toggles auto-fit on new selections), **Copy**
      (in the curve modes
      it copies the currently displayed points of the live curve — only what is
      on screen), and the harmonic-reference tools below; right-clicking the
      *level* axis resets only the level window, the *frequency* axis only the
      frequency window.
    - **Activate Harmonics** (right-click): a popup with two labelled
      fields — *Harmonic number of the mouse:* (which harmonic number the
      frequency under the cursor is, default 1 = fundamental) and *Number of
      harmonics to show:* (default 10). The reference frequency itself has no
      field: it starts at the frequency under the cursor. The panel then draws
      delicate vertical lines for harmonics
      1..max(N, maximum) wherever they are inside the visible frequency window,
      each labelled with its number — so setting harmonic #3 still shows the
      higher harmonics 4, 5, 6, ... up to the configured maximum. White dots
      mark where each line crosses the channel curves. The lines are dynamic —
      they re-anchor to the frequency under the mouse as you move it.
      **Copy Harmonics** copies an Excel-friendly table (tab-separated, CRLF
      line endings) with columns *Harmonic*, *Frequency* and *Amplitude*
      (harmonics 1..N, sampled from the first visible channel at each harmonic's
      nearest FFT bin; empty above Nyquist). **Clear Harmonics** removes the
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
    the whole run instead of one per step). Styled exactly like the Actions
    table: dark blue-green list background under a light grey header with
    black bold **Step** and **Action** columns; the header's right side still
    shows "N of M applied".
  - **Python**: a script editor next to History. The dropdown lists every
    `*.py` file in your personal script folder (`%APPDATA%/SpectroCraft/python`
    on Windows, created automatically with six examples: *Dynamic
    processing*, *Threshold*, *Modulation*, *Add Echo*, *Sync And Merge*
    and *Make Loop* (the last two are described below). The first script in
    the list is selected by default, and whichever script you load last is
    remembered and restored when SpectroCraft is reopened. Right-clicking the
    panel offers **Reset to original** for the built-in example scripts —
    after confirming "Are you sure you want to reset this script to the
    default value?" the file's text is replaced with its factory version.
    **Add** creates a new script from a template, **Remove** deletes the
    selected one, and **Run** executes
    it against the current selection — or against the whole file when nothing
    is selected. The run is undoable (it appears in History as "Python script
    '<name>'").

    A script is called as `python <script.py> <input.wav> <output.wav>
    <meta.json>`: `input.wav` is a 32-bit float WAV of the processed range with
    all channels, and `meta.json` carries `sampleRate`, `numChannels`,
    `fileDurationSec`, `selectionStartSec`, `selectionEndSec`, `wholeFile`,
    `fileName` and every marker (`name`, `comment`, `start`, `end`; `-1` means
    a point marker). The script must write `output.wav` with the **same**
    sample rate and channel count; the length may differ — a shorter or longer
    result resizes the file accordingly (markers are adjusted when the file
    gets shorter, exactly like a trim). Anything else is rejected. Temp files
    are cleaned up after each run. Note that Python's stdlib `wave`
    module only handles PCM — since SpectroCraft exchanges IEEE float32 WAVs,
    the example scripts (and the Add template) embed a small RIFF/WAVE
    reader/writer you can reuse in your own scripts.

    *Sync And Merge* aligns and stacks repeated instances of the same signal:
    set `N_REPETITIONS` to how many copies the recording contains (the first
    copy is the reference), cross-correlation finds where every other copy
    starts, all copies are aligned sample-accurately and averaged into one —
    uncorrelated noise drops by about 10·log10(√N) dB (~3 dB for four copies).
    The output is a single aligned copy, so the file becomes N times shorter.

    *Make Loop* makes the file loop seamlessly: it takes the last `PCT`
    percent of the duration (default 20), overlaps it with the first `PCT`
    percent and crossfades them together (equal-power by default). The result
    is shorter by exactly that overlap, and when played on loop there is no
    hard cut — the ending flows straight into the beginning.

    Only a minimal CPython installation is needed — the example scripts use
    nothing but the standard library (no pip packages). If no interpreter is
    found on the PATH, Run shows installation instructions: download Python 3.x
    from <https://www.python.org/downloads/>, install with default options
    (on Windows keep "Add python.exe to PATH" checked), verify with
    `python --version` in a terminal, then press Run again. Note that pure-Python
    processing is slow on long selections — select shorter ranges for big jobs.
  - **Tables**: imported CSV data tables shown as an indented list of table
    rows and their parameter rows: every row has a visibility eye icon, and
    each parameter additionally carries its own unique palette colour square
    (click cycles it, double-click opens a preset picker). Tables are created
    by dragging CSV files onto the
    window or via right-click **Paste to new table** — either way the new table
    is selected in the list and its plot is drawn at once, no click needed;
    right-click also offers
    **Remove Selected**, **Copy** (CSV) and **Downsample...** (rows/second).
    The top dropdown chooses where visible parameters are drawn: overlay on
    the waveform, overlay on the spectrogram (both auto-fit each parameter's
    range so curves stay visible; switching the choice re-targets every
    parameter to the new view, so imported tables show up without a manual
    alignment first), or a dedicated resizable track with its own
    vertical zoom/pan scrollbar plus the blue selection band and red playhead.
    The panel's own preview plot carries the same blue selection band.
    Tables and individual
    parameters can be time-aligned manually, by picking one/two points on the
    panel's preview plot plus the waveform/spectrogram (two points also
    stretch), or automatically via cross-correlation when two parameters from
    different tables are selected (**Auto Align**). Parameter alignment adds
    value offset/scale and Normalized/Inverted/Absolute transforms. Import
    rules: `true`/`false` → 1/0, first-column-text grids are transposed,
    scattered gaps are interpolated, all-text columns are ignored. Everything
    persists with the file via per-file metadata.
  - Bottom-left tab group: **Spectral Controls** (colour map + preview, dB
    range, Log/Linear toggle, Reset, FFT size — from **256**, the finest
    frequency resolution, up to 65536; an **Auto** option at the end of that
    combo recomputes the size on every render from the current zoom levels:
    about "Target time slices:" (default 80) FFT frames across the visible time
    range sets a ceiling and "Bins per pixel:" (default 1.25) spectral bins per
    pixel of the frequency axis sets a floor, snapped to a power of two in
    256..16384 — both fields appear below the combo while Auto is selected).
    **Multi-resolution**, the last entry, draws each frequency with the window
    that suits it: three sizes N, 4N and 16N (512 / 2048 / 8192 at 48 kHz; N
    is the power of two nearest 10.7 ms of audio) are blended by frequency, so
    low notes get the long window's fine frequency detail and transients in
    the upper range keep the short window's sharpness. Each frequency uses the
    shortest window whose bins are at most 5% of it apart (a window of M
    samples takes over above 20 × rate / M: 469 Hz and 1875 Hz at 48 kHz),
    blended over one octave around each crossover. The level readout under the
    mouse uses the same blend. Tones read the same level in every band; broad
    noise reads about 6 dB lower per band towards the bass, since a 4x longer
    window spreads it over 4x more bins. Each size only draws the rows where
    it has a share, and a view zoomed entirely above the upper crossover only
    computes the short window. Measured on an Apple M2 (Debug build, one
    1600 × 300 row of a 48 kHz file, log 20 Hz–20 kHz): a repaint takes about
    1.5x the time of a single size, a new view (empty tile cache) 2–2.5x that
    of 2048 or Auto and 1.3–1.5x that of 8192, and the tiles take the memory
    of all three sizes (about 5x that of 2048, 1.3x that of 8192). Run the
    tests with `SC_BENCH_MULTIRES=1` to print the table. The choice is
    remembered (`fft_multires`).
    **Auto is on by default**, and its combo entry shows the size currently
    resolved for the zoom levels (e.g. `Auto (4096)`); picking a fixed size
    switches auto mode off, and **Alt+Shift+= / Alt+Shift+-** step the FFT size
    up/down the 256..65536 ladder (from Auto, one step away in manual mode).
    Right-clicking the spectrogram's vertical frequency axis offers these same
    actions in an **FFT Size** submenu: **Increase**, **Reduce** and
    **Auto (N)** — N being the size Auto mode would resolve to at the current
    zoom levels, selecting it switches auto mode on — and **Multi-resolution**,
    followed by a checkable
    **Linear / Logarithmic** pair that toggles the axis scale.
    The full-width **Reset** button restores every Spectral Controls setting to
    its defaults at once — Classic colour map, -120..0 dB window, log axis on,
    harmonic markers off (with 10 harmonics and N=1 configured, no numbers),
    Auto FFT size
    on with the default parameters (80 time slices / 1.25 bins per pixel),
    frequency range 20 Hz..20 kHz — and resets the spectral view's frequency
    axis to match. The full-width **Refresh F5** button below it forces a
    complete spectrogram recomputation by discarding every cached tile and
    re-rendering all tracks; the same action is bound to global shortcut
    **F5**, which works from any focus).
    Two persisted toggle buttons plus two persisted fields control
    harmonic markers in the spectrogram: **"Show harmonics CTRL+H"**
    (default off; also bound to global shortcut **Ctrl+H**, which works from any
    focus) and **"Show harmonic numbers"** (default off — adds a small white
    number next to each circle), then **"Number of harmonics to display:"**
    (default 10) and **"Harmonic number under the mouse:"** (default 1). While
    the mouse is over a channel row, small white circles — half the size of
    the orange selection-scaling circle, no lines — mark the harmonics of the
    frequency under the cursor: fundamental = f_mouse / N, drawn for every
    harmonic k in [1, max] that falls inside the current frequency window.
    Move the mouse over a tone's higher harmonic and set N accordingly so the
    circles land on that tone's whole series. All values are remembered
    between sessions. Above the colour-map
    gradient the panel shows the absolute level of each end of the scale in
    the active file's unit — plain `dB` when uncalibrated, or calibrated values
    after **Set Scale**: `14.0 dB-SPL … 94.0 dB-SPL` for a 1 Pa full scale,
    while Volts/g files show plain linear values (`0.0001 V … 1 V` for a 1 V
    full scale). The labels follow both the dB-range slider and calibration
    changes.
    **Markers** (table with
    a Comment column; select rows Excel-style — plain click selects one row,
    Ctrl/Cmd+click toggles additional rows, Shift+click range-selects from the
    anchor — any change of the selected set makes their union the time
    selection and fits both views to it with the usual 10% margin); edit Title,
    multi-line Comments, Begin, End and Length of the first selected marker in
    the detail editors below the table (the comments box starts beside its label
    and extends to the bottom of the panel); Add Marker; Delete Selected removes
    every selected row at once — DEL instead deletes the audio data of the active
    selection (zeros a spectral selection or trims out a time range, undoable);
    right-click a row for Copy All Markers / Paste Markers from Clipboard,
    Export/Import CSV, Delete Selected,
    Fit View to Selected or Find (Ctrl+F searches titles and comments, F3 /
    Ctrl+F3 jump to the next / previous match). A right-click only shows that
    menu — it never collapses an active multi-selection down to the clicked row
    (with nothing selected it selects exactly that row first).
  - **Drawing**: a table of user annotations drawn over the spectrogram — text,
    arrows, rectangles and freehand strokes with per-item comments and
    visibility; see [Drawing panel](#drawing-panel) below.
  - **Docking workspace (Adobe-style)**: every panel — the **Editor**
    (waveform + spectrogram) included — sits in a *panel group* with text
    tabs, and the groups can be arranged freely:
    - The Editor has **one tab per open file**, titled with the file's name
      (*Editor* while no file is open; a new, never saved file shows its
      display name; *Save As* renames the tab). Each file opened joins the
      Editor's group as a new tab; clicking a tab makes that file the active
      one, and switching files any other way (File > Loaded Files, Ctrl+Tab) brings
      its tab to the front. All the tabs share one waveform/spectrogram,
      which moves into the active file's tab: a file tab dragged to another
      group or window shows the file's name there until it is clicked. Files
      are closed as before (File > Close, File > Loaded Files > Close Active
      Document). The old **Files** panel is gone: its document list lives in
      **File > Loaded Files** (activate a document, **Scale...**, **Open
      Containing Folder**, **Close Active Document**).
    - **Drag a tab** onto another group: a drop overlay splits that group into
      a centre rectangle (*add as a tab*) and four trapezoids (*dock left /
      right / top / bottom* of it); the zone under the cursor turns violet and
      the dragged panel, shown as a translucent thumbnail, snaps onto it like
      a magnet. Drop on a **tab strip** to insert the tab at the white caret,
      or on the thin band along an **edge of the window** to dock the panel
      along that whole edge. Dragging the **empty part of a tab strip** moves
      the whole group. While a panel is dragged (or held down for a moment)
      it turns half-transparent; **Esc** cancels the drag.
    - **Undock**: drop a panel outside the window (or hold **Ctrl** — **Cmd**
      on macOS — while dropping), or choose **Undock Panel** from the tab's
      right-click menu or from the **≡** panel menu next to the active tab. It
      becomes a floating window, moved by its top strip (it snaps to screen
      edges and to the other windows) and resized by its borders (an 8 px
      zone along each edge); other panels can be docked into it the same
      way. The strip's buttons **minimise** the window to the strip itself
      (the same button brings the panels back), **maximise** it to the work
      area of its screen (double-click the strip does the same; dragging a
      maximised window restores its size) and close it. **Dock Panel**
      returns a floating panel to where it was docked. Closing a floating
      window closes its panels (the Editor docks back instead); floating
      windows come back above the main window when it is activated and hide
      while it is minimised.
    - The tab menu (right-click a tab, or **≡**) also has **Close Panel**,
      — for a floating window — **Always on Top** to keep it above every
      other window, **Close Other Panels in Group** and *Panel Group
      Settings* (**Close / Undock / Maximize Panel Group**). **Double-click a tab** or press **`**
      (backquote, shown as *Backquote* in the Window menu) to maximise the group under the mouse, again to restore it.
      When the tabs do not fit, the **>>** button lists the hidden ones.
    - Drag the **gaps** between groups to resize them: the 4 px gap and 4 px
      into each neighbour grab it, and the resize arrow shows exactly where a
      press resizes. Double-click a gap to share the space equally between
      its two neighbours.
    - **Workspaces**: *Default* (the classic left column beside the Editor),
      *Editing*, *Analysis* and *Video* are listed at the right end of the
      menu bar and in **Window > Workspaces**. Each workspace keeps its own
      modifications; **Reset to Saved Layout** goes back to its saved
      arrangement, **Save Changes to this Workspace** updates that, and
      **Save as New Workspace...**, **Rename Workspace...** and **Delete
      Workspace** manage your own (right-click a name in the bar for the same
      menu). The whole arrangement, floating windows included, is restored at
      the next launch; the first launch after the update keeps the
      arrangement of the former two-group left column.
    - **View > Panels** (Ctrl+Shift+Enter) maximises the Editor's group to
      hide every other panel, and brings them back.
    - Individual panels can be closed from their tab menu (**≡**) or the
      **Window** menu, which lists every panel with a checkmark and opens or
      closes each one. Closed panels stay
      closed between sessions; a reopened panel goes back to its former
      group, in its former tab order, or — when that group is gone — to a new
      group on the side of the one it sat next to.
    - The panel chrome (tab strips, menu bar, workspace bar, popup menus)
      uses **Source Sans 3**, Adobe's open-source UI typeface (embedded,
      SIL OFL 1.1); the whole interface uses neutral dark greys, dark menus
      and dark table headers. Panel contents keep the system font.
  - Per-channel display selection lives in the **View menu**: one checkable item
    per channel ("Channel 1", "Channel 2", …, with a **Ctrl+(Shift)+N** shortcut
    hint on items 1–9) that toggles each channel independently for any number of
    channels and applies to both views.
    **Ctrl/Shift+click** on a channel item shows *only* that channel (solo);
    repeating the same solo click restores every channel, and solo-clicking a
    different channel switches the solo to it. The keyboard shortcuts
    Ctrl+Shift+1..9 behave the same way.
  - **Channel numbers** in the tracks (the "1", "2"… or "L"/"R" labels in the
    axis strip) mute playback: a click **mutes/unmutes** that channel — its
    number turns red and its waveform/spectrogram is drawn darker, and its
    output is silent while playing. **Ctrl+click** (Cmd+click on macOS, where
    Ctrl+click opens the context menu) plays **only** that channel (every
    other one muted); doing it again on the lone playing channel unmutes them
    all. Hidden channels count like the others. The mutes belong to the
    document for the session (not saved with the file).
  - The **status bar** shows every channel number of the file next to its
    sample rate: dimmed while the channel is hidden, red while it is muted.
    A click shows/hides that channel (like *View > Channel N*); **Ctrl/Shift+click**
    (Cmd+click on macOS) shows **only** that channel — the same solo rule as the
    channel list and the track numbers — and repeating it on the lone visible
    channel shows them all again.
  - The View menu also has **Waveform View / Spectrogram View** visibility
    toggles; at least one view is always kept visible — hiding the last one is
    refused and the remaining view takes the whole main area.
  - Track axis placement (both views at once, remembered between sessions):
    **Vertical Axis on Right** moves the amplitude/frequency axis strip from
    the left edge to the right edge of both track views (the data stops at its
    left edge and the channel labels sit at the strip's right side), and
    **Time Ruler at Bottom** moves the time ruler strip from the top to the
    bottom. When both views are stacked, exactly one of them draws the shared
    time ruler: the waveform when it is at the top, the spectrogram (the lower
    view) when it is at the bottom — the other hides its strip and expands.
    The same section offers **Show Amplitude Grid**, **Show Frequency
    Grid** and **Show 1/3 Octave Grid**. The first two hide/show the
    horizontal grey grid lines inside the data tracks - the dB amplitude
    levels in each waveform track (plus a line at amplitude 0, labelled
    **-inf** in dB mode, **0.000** in Normalized and **0** in Wav Units) and
    the frequency tick lines in each spectrogram track. The waveform grid lines
    are semi-transparent, so a quiet signal drawn along the zero line is not
    hidden behind them. The third overlays
    thin delicate white dashed lines at the borders of the IEC 61260-1 1/3
    octave bands (centres 1, 1.25, 1.6, 2, 2.5, 3.15, 4, 5, 6.3, 8 × 10ⁿ Hz)
    with each band's centre value written in white between its borders,
    just inside the spectrogram on the same side as the vertical frequency
    axis (labels are skipped when a band is too thin on screen or would hit
    a channel label). Nothing is drawn below the 40 Hz band: **40** is the
    lowest label and its lower border (~35.5 Hz) the lowest grid line. The first two default to on; **Show 1/3 Octave Grid**
    defaults to off. All three are remembered between sessions and leave the
    axis labels untouched when off.
  - Main area: waveform view on top, spectral view below, separated by a
    **draggable divider** — drag it up/down to resize the two views. A time
    scrollbar runs along the bottom, aligned with the data region (inset by
    the axis strip on whichever side it is placed); each view has its own
    vertical scrollbar.
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
    is editable too: click into the `Pos:` field to type/paste where the cursor
    should go (clamped to the file) — it tracks playback at 30 Hz whenever you
    are not editing it.
  - The File menu's **Spectrogram** submenu offers:
    - **Save Spectrogram As...** — writes the current spectrogram
      content (per-channel rows included) to a PNG file.
    - **Take Snapshot** — saves a PNG right next to the WAV file with
      a timestamped name (`<name>_snapshot_<yyyy-mm-dd>_<hh-mm-ss>.png`, numeric
      suffix on collision); falls back to a save dialog when no file is loaded.
    - Four checkable options, persisted across restarts, that add overlays to
      the exported image (all off by default):
      **Save Axes** (frequency + time axes around the plot), **Save Markers**
      (marker lines and titles), **Save Selections** (time selection band plus
      spectral selection shapes) and **Save Drawing** (your committed drawings —
      text, arrows, rectangles, freehand).
- **New files**: File → **New...** creates an empty document with
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
  single undoable edits. **Paste to New Channel(s)** (Edit menu) appends one new
  file channel per clipboard channel and writes each into it at the selection
  start (or playhead): the existing channels stay byte-for-byte identical, the
  file length is unchanged (a paste running past end-of-file is truncated),
  the added channels become visible, and the whole operation is a single
  undoable edit.
- **Large-file loading**: files bigger than ~200 MB are opened on a background
  thread with a modal progress dialog showing an animated bar (read + mipmap
  rebuild progress); smaller files load synchronously as before. The UI stays
  responsive throughout, and the dialog closes itself when loading finishes.
- **Save / Save As / Export Spectral** also run on background threads behind a
  modal dialog with an indeterminate spinning bar, so multi-second writes never
  freeze the window (and no stale wait cursor is left behind afterwards).
- **Application icon**: `src/Icons/icon4.ico` (the candidates `icon1.ico` …
  `icon4.ico` all live in `src/Icons/`; `icon1.ico` is the previous design) is
  embedded into the exe as
  a proper PE resource (`RT_ICON` + `RT_GROUP_ICON`) — an `ICON` statement is
  appended to JUCE's generated `.rc` at build time by
  `cmake/patch_generated.cmake` — so Windows Explorer and the taskbar show it.
  The same icon is additionally applied to the main window at startup via
  `CreateIconFromResourceEx` + `WM_SETICON` (from `include/IconData.h`) as a
  fallback for the title bar and Alt-Tab. `include/IconData.h` is the byte
  array of that same file: after changing the icon, run
  `python3 tools/make_icon_data.py` so the resource and the runtime icon stay
  the same picture.
- **Multi-file support**: any number of files can be open at once. The **Files**
  tab lists every loaded file with its properties (sample rate, channel count,
  duration); **double-click a row to make that file active** — it is loaded
  into the tracks/views and its markers appear in the marker list. Each file
  remembers its own playhead position, selection range, visible channels,
  time zoom (the waveform/spectrogram window), markers and undo history;
  switching files restores all of them.
  **Ctrl+Tab / Ctrl+Shift+Tab** cycles through the loaded files (forward /
  backwards). Closing a file (right-click → Close, File → Close, or
  Ctrl+W / Ctrl+F4 for the active one) removes it from the list. The live frequency analysis always
  follows the playhead of the *active* file, and **the eight stored snapshots
  are shared across all open files** — each snapshot remembers which file it
  was stored from (shown in its tooltip), so curves from different files can be
  compared side by side. Closing a file clears only the snapshots that came
  from it.
- **Single-instance mode**: the Window menu has an **Allow multiple instances**
  toggle (**checked by default**, remembered across restarts). When unchecked,
  launching SpectroCraft again — e.g. double-clicking a WAV file in Explorer —
  forwards that file to the already-running instance and quits the second
  process; the running instance opens it like any other File > Open (large
  files get their async load with progress dialog). Every running instance
  hosts a small named-pipe forwarding server, so this works no matter how the
  first instance was started — even one that launched while multiple instances
  were still allowed.
- **Scale calibration (Scale form)**: right-clicking a file in the Files list
  offers **Scale…**, which opens the Scale form for that document; there the
  amplitude axis' unit (**Volt, Pascal, g** or plain **None**) and its per-channel
  full-scale values calibrate the file, exactly as the old "Set Unit and Scale"
  dialog did.
  The unit is shared by all channels, with **one full-scale value per
  channel** — e.g. "2 Pascal" means a full-scale sample (|1.0|) corresponds to
  2 Pa of pressure on that channel. Equal values are stored as a single shared
  number; differing ones are kept per channel and shown joined in the Files list
  (e.g. `2/3 Pa`). The calibration is stored per file (for the session) and
  displayed under the corresponding entry in the Files list. When set, readouts
  are converted from relative dB into absolute levels: the waveform's amplitude
  axis shows physical values in that unit (Pa/V/g), with each channel row using
  its own full-scale value when they differ, and the frequency/spectral readouts
  — the spectrogram hover probe, the Spectral Controls level-scale labels, and
  the frequency analysis view (FFT curves as well as 1/3-octave bands,
  including its axis labels, cursor readout and clipboard export) — show
  **dB-SPL** for Pascal files but plain linear values in **V** or **g** for
  Volts/g files (in the 1/3 Octave view, Pascal calibration additionally
  switches the whole band computation to the reference-analyser pipeline
  described above, so its readings match those instruments). "None" restores
  plain relative dB.
- **Automatic calibration from `bkdk` WAV chunks**: some legacy measurement WAV
  files carry a custom top-level RIFF chunk named `bkdk` with per-channel
  *Calibration* gains (sample × Calibration = pascals). Loading such a file
  automatically sets the unit to **Pascal** and takes each channel's full-scale
  value from its Calibration field — a single defined group applies to every
  channel, and missing/unparseable values default to no scaling. This overrides
  any previously stored scale for that file. The raw `bkdk` chunk is preserved
  byte-for-byte when the file is saved as WAV again (and never added to files
  that don't have one).
- **Open Recent**: File → Open Recent lists up to the ten most recently opened
  files (most recent first) and re-opens them with one click. The list is
  persisted in a `PropertiesFile` (`SpectroCraft.recent` under the user's
  application-data folder), survives restarts, deduplicates repeated opens,
  and self-heals: entries whose file was deleted or moved are dropped when
  clicked. "Clear Recent Files" empties the list.
- **Drag & drop**: drop one or more audio (or video) files onto the window;
  every valid dropped file becomes its own document.
- **Merge (Shift + drag & drop)**: dropping files while holding Shift shows a
  menu at the cursor with four merge options (plus plain *Open*):
  - *Open in sequence in current file / …in a new file*: concatenate the files
    one after another (the current document first when merging into it). The
    result uses the highest sample rate involved — every source is resampled
    to it with SoXR — and the maximum channel count, so shorter/narrower
    sources are padded with silence. Markers from the dropped files travel
    along, shifted to their new positions, and each dropped file gains a region
    marker spanning its segment.
  - *Open in parallel in current file / …in a new file*: stack the files on top
    of each other — every source keeps all of its channels as fresh output
    channels (the current document's channels first), length = the longest
    input, shorter inputs padded with silence at their end.
  - Merging into the current file at an unchanged rate in sequence mode is a
    pure append and **undoable** like any other edit; parallel merges and rate
    changes replace the whole file and are not undoable (like Resample).
  - The work runs on a worker thread behind a progress dialog with cancel;
    multi-GB files stream through in chunks and never load fully into RAM.
- **Edits** with 50-level undo/redo (patch-based: old/new sample data + length
  delta — memory efficient): silence, fade in/out, normalize, reverse, gain,
  cut/copy/paste, delete, insert. The destructive effects (silence/fade/
  normalize/reverse/gain) apply **only to the currently selected (visible)
  channels** — e.g. with only Channel 2 visible, a fade never touches the
  other tracks; undo restores every channel exactly (patches capture the old
  data *before* the in-place modification).
- **Time-anchored state follows every length-changing edit**: everything that
  references file time is remapped inside the same undo step as the audio —
  markers, named saved spectral selections (Selection panel), the active
  spectral selection, drawings (Drawing panel) and SCTT transcription segments,
  plus the live playhead and time selection. The rules per operation:
    - **Delete / Trim**: anchors inside the removed range drop; anchors that
      straddle an edge trim to it; everything after shifts left by the removed
      length.
    - **Paste / Generate (Insert at Playhead) / Python scripts that extend a
      file**: anchors at or after the insertion point shift right by the
      inserted length; shapes straddling the point split into two pieces
      (rectangles and polygons; strokes crop their column grid).
    - **Time Stretch / Content Aware Time Scale**: anchors inside the
      edited range scale with it, everything after shifts by the length
      delta.
    - **Stem Separation / Super Resolution in Overwrite mode** (the result
      replaces an extracted window): equivalent to deleting everything before
      and after the new content — anchors keep their position relative to the
      replaced window, anything outside it is dropped or clamped to the new
      end.
  Stored FFT snapshots of that file shift/trim/clear accordingly (snapshots
  straddling an insertion point are truncated at it). Undo and redo restore
  all of it together with the audio patch (snapshot curves are recomputed from
  the restored audio), so a single Ctrl+Z brings back both the samples and
  every marker, selection, drawing, transcription segment and snapshot exactly
  as before.
- **Resample**: Edit → Resample… converts the whole file to any sample rate
  using SoXR's very-high-quality (`SOXR_VHQ`) resampler — and can change the
  channel count in the same operation. The dialog has captioned *Sample Rate*
  and *Number of Channels* fields (1–512) plus a remix weight grid: one row
  per output channel, one column per input channel; each row is normalised to
  sum to 1 (empty cells are 0), so `{2, 1}` means "two-thirds of L +
  one-third of R". When the count changes, the default weights mix every
  input equally into each output. A row of five **fill buttons** above the
  grid presets the whole matrix: *Default* (identity for an unchanged count,
  equal split otherwise), *Identity* (1 on the diagonal, 0 elsewhere),
  *Merge* (every input averaged onto every output), *Ones* and *Zeros*. All
  fields and buttons carry explanatory tooltips. Whole-file operation,
  requires the file fully in memory, not undoable (the old data is discarded);
  the footer shows the current sample rate and channel count.
- **AI features** (built by default; see [AI features](#ai-features) below):
  - *Noise Suppression* — Effects → Noise Suppression… applies a neural
    speech-enhancement model (DeepFilterNet 2/3 or DenseUNet) to the current
    selection via OpenVINO. The optional **Copy noise on new track** setting
    appends one extra track per processed channel containing only the
    estimated noise (original minus enhanced).
  - *Transcription* — the **Transcribe...** button in the **Transcription** tab
    runs Whisper over the whole file; segments appear in that tab (double-click
    a row to jump, Ctrl+F / F3 search for matches). Results are stored with the
    file as a custom `SCTT` RIFF chunk (embedded mode) or inside `.meta.json`
    (external mode — see [Per-file metadata](#per-file-metadata)), so they
    travel with the file.
  - *Batch transcription* — the **Transcribe Folder...** button in the same tab
    transcribes every
    `.wav` in a folder tree headlessly (files never open in the editor) and
    writes each file's `SCTT` chunk back in place; choose which channel to
    analyse (default: channel 1, enter 0 or less to mix all channels),
    optional skip-existing mode.
- **Stem separation** (`Effects → Stem Separation…`, see [below](#stem-separation)):
  describes an audio element in plain text and the tool separates it from the
  current selection (or whole file) into a *target* plus *residual*; advanced
  mode runs several descriptions in one pass with optional include/exclude time
  anchors. Runs an external prebuilt CLI — binary and model weights are
  downloaded on demand or installed manually.
- **Super resolution** (`Effects → Super Resolution…`, see [below](#super-resolution)):
  enhances a time range into a single mono 48 kHz track using the same external
  `audiocpp_cli.exe` tool — either the FlashSR utility (small, fast) or an
  AudioSR GGUF model. The result can open as a new file, replace the current
  file (one undoable step, including any sample-rate change), or be appended as
  a new track aligned to the enhanced range.
- **Split spectral selection** (`Effects → Split Spectral Selection Configuration`, Shift+F12,
  and `Effects → Split Spectral Selection Execute`, F12): moves the selected band content out of
  chosen source channels into destination channels you pick per channel. The configuration window has one checkbox row per channel plus a destination-channel field for each; an existing destination mixes in place, while a number above the current count creates that channel on first use. Presets: **All Tracks** (every channel extracts into its own new track), **Last channel into new**, and **Second last into last**. Closing the window saves the configuration without performing any split — Execute then applies it as one undoable step; the first time per file open it opens the configuration, and closing that window then executes the split on the current selection. The blend is trapezoidal (25% linear ramp at each band edge, full weight across the middle), and where energy is removed the original channels are refilled with synthetic noise at the per-frame estimated floor level — so *original + destination* reconstructs the pre-edit signal with no hard spectral edge; at both time ends of the selection the extracted content fades in/out smoothly and the originals crossfade back, so there is no spike or hard cut either. One undoable step (content change plus channel-count change); undo/redo keeps track visibility in sync with the file's channel count, so reverting a mono→stereo split returns to a single visible track instead of leaving a phantom silent one.
- **Generate Silence / Noise / Tone** (`Effects → Generate …`): inserts or
  replaces generated audio, each as one undoable step. The target depends on
  the active selection state: with a *spectral* selection only the content
  inside that region is replaced (the rest of the signal is untouched); with a
  *time* selection the selected range is replaced; with neither, the generated
  audio is inserted at the playhead position and everything after it shifts
  right. The dialogs:
  - *Generate Silence…* — fills the target with digital silence. When a time
    or spectral selection already exists there is nothing to configure, so no
    dialog appears at all and the edit runs immediately.
  - *Generate Noise…* — white, pink (Vos six-stage filter) or brown (random
    walk) noise; the intensity slider sets the peak amplitude from 0 to 1. The
    **Average Level** button next to it measures the RMS of the selection (or
    of ±0.5 s around the playhead when nothing is selected) and sets the
    intensity so the generated noise matches that level.
  - *Generate Tone…* — any number of frequency + amplitude pairs added
    together, rendered as sine, square, sawtooth or triangle. Phase is
    continuous with absolute file time, so generated tones line up with
    surrounding audio instead of clicking at the boundary.
- **Invert / Time Stretch / Content Aware Time Scale / Filter EQ /
  Envelope** (`Effects → Invert`, `Time Stretch…`,
  `Content Aware Time Scale…`, `Filter EQ…`, `Envelope…`): five classic
  waveform effects added after *Reverse*:
  - *Invert* — negates every sample in the target (spectral selection if
    active, else the time selection, else the whole file). One undoable step.
  - *Time Stretch…* — SoundTouch-based tempo change that preserves pitch.
    The dialog has two linked fields: **Time Duration** (the new length of
    the stretched audio) and **Speed Factor** (how much faster or slower it
    plays); editing either rewrites the other (duration d ↔ factor ref/d,
    where ref is the selection length, or the whole file's duration when
    nothing is selected). A positive stretch makes the region shorter and
    faster. It changes the file's length, so **all channels** are stretched
    (the engine holds one
    fixed-length buffer) and markers, saved spectral selections and FFT
    snapshot/curve slots are warped through the same time-warp function —
    nothing is dropped, everything stays in sync. One undoable step that
    restores both the old content and the old length; a spectral selection,
    if present, is ignored (a stretch is a time-domain operation).
  - *Content Aware Time Scale…* — a content-aware alternative to Time
    Stretch with two modes in a drop-down:
    - **Time Stretch** — the same linked duration/speed-factor fields as
      Time Stretch, but instead of SoundTouch it removes (to shorten) or
      duplicates (to lengthen) short time-domain segments chosen by spectral
      stability. The analysis computes STFT magnitude differences between
      consecutive frames (Hann window, size ≈ rate/48 clamped to 1024–4096,
      hop N/4), normalises them over the range and derives a per-gap
      stability `exp(-3·normDiff)`. Onsets and offsets — local maxima of the
      frame-to-frame change reaching 30% of the maximum — protect themselves
      plus their neighbouring gaps, so transients are almost never cut or
      duplicated. Stable runs (≥3 consecutive qualifying gaps) become regions
      at a P40 stability threshold, falling back to P25/P10 when the selection
      has less stable material than requested; segments are then picked with
      an exponential preference for the most stable region plus a light length
      bonus until the requested amount is reached. Every edit joins its sides
      with a 10 ms equal-power (cosine/sine) crossfade; each removal's end
      point is additionally moved to where the audio resembles its start
      (normalised cross-correlation over one hop, searched within two hops of
      the planned end), so the fade blends similar material and no clicks
      appear at the seams.
    - **Reduce Gaps** — a percentile field (0–100): every region whose
      frame-to-frame change sits at or below that percentile of all gaps is
      deleted. The resulting duration is shown live in the dialog before you
      apply, so you can dial the amount in. A **Mark only** checkbox adds a
      region marker at every removable gap instead of deleting audio (no undo
      step, like manual markers).
    Both modes work on the time selection (or the whole file when nothing is
    selected), change the file length like Time Stretch — markers, saved
    selections and FFT snapshots are warped through the same time-warp — and
    record one undoable step. A factor of ~1 or a 0% percentile is a bit-exact
    no-op that records nothing.
  - *Filter EQ…* — Audacity-style graphical curve equalizer: an interactive
    log-frequency/dB editor (click empty space to add a point, drag to move
    it, **double-click** a point to type its exact frequency/gain coordinates,
    right-click to remove it) with the ten built-in Audacity presets plus
    Flat. The preset row also has **Add…** and **Delete** buttons for
    user-defined presets: Add saves the current curve under a name you type
    (replacing an existing preset of the same name), Delete removes the
    selected user preset (built-ins are protected). Both the last used curve
    and your custom presets persist across restarts — reopening the dialog
    restores the previous curve. The drawn curve holds its end values flat
    before the first point and after the last one, matching how the filter
    itself behaves. The curve is applied as a zero-phase FFT filter (16384-point windows,
    4001-sample Blackman-windowed impulse response, overlap-add), so phase
    is preserved and no pre-/post-ringing appears. Works on the spectral
    selection if active, else the time selection, else the whole file; a flat
    curve is a bit-exact no-op that records nothing.
  - *Envelope…* — Audacity-style amplitude-over-time envelope: an interactive
    time/dB graph (click empty space to add a point, drag to move it,
    **double-click** a point to type its exact time/gain coordinates,
    right-click to remove it) with the built-in presets Flat, Fade In, Fade
    Out and Dip in Middle. The preset row also has **Add…** and **Delete**
    buttons for user-defined presets (built-ins are protected); both the last
    used curve and your custom presets persist across restarts — reopening
    the dialog restores the previous curve. Presets and the remembered curve
    are proportionally rescaled to fit the current target length, so a Fade In
    always fades over the whole selection regardless of its duration. The
    drawn curve holds its end values flat before the first point and after the
    last one; gains are applied per sample as 10^(dB/20). Works on the spectral
    selection if active (through the same STFT masking path as the other
    effects), else the time selection, else the whole file; a flat curve is a
    bit-exact no-op that records nothing. One undoable step.
- **Actions panel** (Photoshop-style record/replay/batch): record a sequence of
  edits as an *action*, replay it on the current document, or batch-run it over
  a whole folder tree. Recorded steps include gain, time stretch, content-aware
  time scale, filter EQ, envelope, generators (silence/noise/tones), resample,
  transcribe, noise suppression, Python scripts and the immediate effects
  (silence/fade/normalize/reverse/invert/delete/trim). Steps can be marked
  *prompt* so replay re-opens their dialog for fresh parameters. Actions persist
  across restarts; see **Actions panel** below.
- **Cancelling long operations**: every progress dialog — loading large files,
  resampling, noise suppression, transcription (single and batch), spectrogram
  export — has a Cancel button, and ESC works as well. The operation stops at
  the next safe checkpoint and nothing is committed on cancel; for Whisper
  inference a cancel takes effect after the current phase finishes.
- **Offline tools**: `--test-spectral` renders a spectrogram PNG headlessly;
  `--test-load` times a headless load and prints mipmap stats (useful for
  multi-hour files); `--generate-test-files` creates the test WAVs;
  `--run-unit-tests` runs the unit suite from either executable. Startup phase
  timings go to the diagnostic log (its `startup` event).

---

## Video files

SpectroCraft opens video files as their audio: the first audio stream is
decoded and treated as a normal audio file (spectrogram, edits, saving), and
the picture is shown in the **Video window** (below).

### Video window

Opening a video file opens a separate, resizable **Video** window (Windows,
macOS). It always shows the frame at the document time:

- **while playing**: the playback position (the sound is the document audio,
  played by SpectroCraft itself — the picture is silent and follows it);
- **when stopped**: the playhead (click in the spectrogram or waveform,
  Home/End, arrows, the status-bar position field).

*Window > Video* hides or shows it; closing it keeps it closed for that file
until reopened from the menu. Switching to a document without a video shows
"No video for this document". After an edit that changes the duration
(delete, time stretch), a notice says that audio and video lengths differ:
the picture keeps following the document time. The document remembers its
video file, so the picture still follows after *Save As* to WAV. The frame
time is shown in the top-left corner; phone videos are displayed with their
rotation. The window has the usual minimise / maximise / close buttons.

Zooming and panning work like in the spectrogram:

- **mouse wheel** over the picture zooms in/out about the cursor position;
  double-click (or *Zoom Fit* from the right-click menu) fits it again;
- **right-drag** draws a selection box that is then zoomed to fill the view,
  like the spectrogram's region zoom; a plain right-click shows the context
  menu (*Zoom In / Zoom Out / Zoom Fit*, *Rotate 90° CW / CCW* — the rotation
  is remembered per file), followed by the actions of the track currently
  shown in the picture (see below);
- **zoom bars** on the right and bottom edges of the picture show the
  visible part, one per axis: drag their edges to zoom, drag the thumb (or
  click in the track) to pan;
- left-drag or **middle-drag** pans while zoomed: the picture follows the
  pointer one to one, measured from the previous cursor position, so hitting a
  picture edge never leaves a dead zone to unwind and reversing responds at once;
- **Upscaling** (right-click menu): how the picture is resampled to the
  screen — *Lanczos* (default, windowed sinc with 6 taps per axis, the kernel
  of FFmpeg's swscale `SWS_LANCZOS`), *Bicubic* (cubic convolution, A = −0.6,
  swscale's `SWS_BICUBIC` default) or *Nearest* (the raw pixels, blocky when
  enlarged). The choice is remembered between sessions. Only the part of the
  picture on screen is resampled, straight to physical screen pixels, on
  several threads (`src/videoscale/`): a few milliseconds per frame even full
  screen on a Retina display.

*Docks as a panel* like Tables or Markers: **Window > Video** (listed with
the other panels, after *Drawing*) opens the **Video** panel of the docking
workspace (a tab of the top-left group by default) with the picture **and
the tracks panel below it**, and unchecking it hides the picture wherever it
is. Like any panel, dragging its tab out (or *Undock Panel* from the tab
menu) turns it into a floating panel window (*Dock Panel* brings it back).
Closing the Video panel from its tab brings the picture back to its own
Video window. A floating Video
panel can be kept above every other window with *Always on Top* from the
tab menu. The picture keeps its zoom, pan and rotation either way.

Between the picture and the tracks panel sits a thin **draggable separator**:
drag it to change their ratio (resizing the window keeps it). It is the **same
bar** as the one between the waveform and the spectrogram and the one between
the table list and its preview plot: 6 px, three short lines in the middle
(`include/SplitterStyle.h`, used by `ViewSplitter`, `TrackSplitter` and
`TablesSplitter`). The little
double-chevron button in the corner where the scrollbars meet (the workspace
bar's chevron, rotated 90°) collapses the tracks area away — the picture then
fills the whole window — and expands it again.

#### Video tracks

A document can hold several video files at once. Below the picture there is a
small **tracks panel**: a time ruler with a green band showing the length of
the main audio (which defines the timeline), and one coloured band per video,
drawn from its offset to its end.

- **dragging** a band left/right changes that video's time offset relative to
  the audio; hold **Shift while dragging** and the offset snaps to 0 s, to the
  end of the main audio, or to the start/end of another video track (a yellow
  marker shows the anchor); the **mouse wheel** over the panel zooms in/out
  about the cursor for fine positioning;
- **dragging** a band **up or down** moves the track to another row (a white
  line shows where it lands): the order is saved with the document, and the
  top row renders above the others where tracks overlap in time. The first
  few pixels of a drag decide its direction, so reordering never nudges an
  offset (a lone track's band moves its offset from the first pixel);
- a red **playhead line** runs from the ruler to the bottom of the panel and
  follows the main playhead while playing and on every seek; clicking the
  time ruler moves the main playhead to that time, and **dragging on the ruler**
  selects that time range (the same 4-pixel threshold the waveform's ruler and
  the dedicated track's axis use), which appears everywhere the selection is
  drawn;
- the **current time selection** is shown across the ruler and every row as the
  same translucent blue band with light-blue edges used by the waveform,
  spectral and Tables views, so the same selection is visible wherever you are
  working;
- **clicking** a band selects it: that video renders above while it covers the
current time. Clicking outside any band clears the selection and shows the
topmost available video at the current time; when the selected video ends,
the topmost covering one takes over automatically;
- right-click on a **band** (or the ruler) for *Zoom In / Zoom Out / Zoom Fit*
  — plus, on a band: *Sync based on Audio*, *Align…*,
  *Copy Audio to New Channel(s)*, *Add Video Track…*, *Remove Track* and
  *Adjust Image*; right-clicking the picture offers those same actions for
  the track currently shown in it;
- when the tracks or the timeline outgrow the panel, **scrollbars** appear
  at its right and bottom edges (the ruler stays pinned to the top);
- **adding** a track works from that menu or by dragging a video file onto the
  window/panel; it joins at offset 0 (drag its band afterwards);
- *Align…* asks for a manual time offset in seconds (negative = the video
  starts before the audio);
- *Sync based on Audio* runs a background job that cross-correlates the
  video's embedded audio with the main document audio and sets the offset to
  the best match;
- *Copy Audio to New Channel(s)* decodes the video's own audio into new
document channels at the track's offset (an undoable edit, like a paste);
- *Adjust Image* opens a dialog with live **contrast** and **brightness**
sliders plus *Apply to All*, *Reset* and *Close*. Each track keeps its own
zoom, pan, rotation, contrast and brightness.

All of this — the video file references, their offsets and per-track picture
adjustments — is stored in the document's metadata (the `scmd` sidecar / WAV
chunk), so a saved document reopens with the same tracks.

Pictures are decoded with the platform's own media framework, so there is no
extra dependency (`src/VideoFrames*.{cpp,mm}`): AVFoundation (`AVAssetReader`)
on macOS, Media Foundation (`IMFSourceReader`) on Windows. A worker thread
decodes the frame for the latest requested time only, so fast scrubbing never
queues up; moving forward by less than a second decodes onward from the
current frame (playback), anything else seeks to the previous key frame. On
Windows, HEVC/VP9/AV1 need the codec extensions from the Microsoft Store, and
Windows "N" editions need the Media Feature Pack (the Media Foundation DLLs are
delay-loaded, so the app still starts without them). Linux has no Video
window.

### Audio extraction

On macOS, `.mp4`, `.mov`, `.m4v`, `.3gp` and `.3g2` are read through
AVFoundation (`src/VideoAudio_mac.mm`, any codec the system decodes: AAC, ALAC,
PCM, ...). On Windows, SpectroCraft can open video containers directly through
FFmpeg. Supported extensions include `.mp4`, `.m4a`, `.mov`,
`.mkv`, `.webm`, `.avi`, `.wmv`, `.asf`, `.flv`, `.ts`/`.m2ts`, `.wma`,
`.ac3`, `.ape`, `.tta` and more; any audio codec the bundled FFmpeg build
knows (AAC, MP3, Vorbis, Opus, AC-3, PCM, ...) works inside those containers.

The extraction is done in-process by a small `juce::AudioFormatReader`
wrapper (`src/VideoAudio.cpp`) around libavformat/libavcodec/libswresample —
no external converter executable. Because it plugs into the normal
`AudioFormatManager`, everything else just works: large video files stream
from disk tile by tile like any other file, and **saving always produces a
plain audio file** (WAV/FLAC/AIFF) with the decoded audio.

The FFmpeg runtime is vendored in `extern/ffmpeg/` (headers + import
libraries) with its four prebuilt Windows x64 DLLs (~2.6 MB total:
`avformat-61`, `avcodec-61`, `swresample-5`, `avutil-59`) in
`extern/ffmpeg/bin`, copied into the `SpectroCraft` subfolder next to the
executable at build time.
The four DLLs are delay-loaded (`/DELAYLOAD`) so the app starts even when they
are missing; on first use of video extraction the runtime is probed and, if
absent, opening video files simply fails with a clear message instead of
crashing. It is a
minimal **audio-only** build (demuxers + audio decoders, no video decoding,
no encoders/filters/network) under **LGPL-2.1-or-later**, so it can be
dynamically linked without affecting SpectroCraft's AGPL-3.0 licence — see
`third_party/licenses/NOTICE.md` and `extern/ffmpeg/README.md`. Disable with
`-DSPECTRO_ENABLE_FFMPEG=OFF`; on Linux the feature is simply absent (the
built-in JUCE formats still cover WAV/AIFF/FLAC/MP3/Ogg).

---

## Recording

SpectroCraft can record audio into a new document while you watch it come in:
the waveform and spectrogram update live, and when your view range is at the end
of the file it follows the recording edge (keeping your current zoom width — it
never auto-zooms out to show everything). If you pan away from the end during a
recording the view stays exactly where you left it; once you come back within
~1.5 s of the end, following resumes automatically. While recording, the status
bar shows a red dot followed by `REC <seconds> s` counting up in real time.

### Using it

1. **Transport > Recording Device** — pick a plugin and device. The submenu is
   built from every loaded recorder plugin (see below); each device opens a small
   dialog where you choose channel count, sample rate and bit depth from what
   the device offers.
   - For devices with a fixed channel width the **Channels** combo lists
     `All (N)` plus one entry per source channel (`Channel 1` … `Channel N`).
     Picking a single channel records only that input into a mono document —
     e.g. keep just the left mic of a stereo pair. The mapping happens in the
     host, so any plugin works with it.
   - Devices whose width varies (e.g. loopback endpoints) show a plain count
     list instead.
2. **Transport > Record** (or press **Shift+F10**) — starts recording into a new
   document named `rec_YYYYMMDD_HHMMSS.wav` (made unique against other open
documents). The menu item becomes **Stop Recording**; stopping drains whatever is
still buffered in the plugin and rebuilds the mipmap pyramid so the overview
shows the whole take. Pressing Shift+F10 again stops it.
3. Save the document as usual (File > Save) to keep it on disk — until then it
   lives only in memory like any other unsaved file.

### Recording options (Unlimited / Loop Mode / Timed)

**Transport > Recording Options** selects how a take is bounded:

- **Unlimited** (default, ticked) — record until you stop; the document grows
  as long as needed. This is the classic behaviour.
- **Loop Mode** — clicking it asks for a loop duration (plain seconds or
  `m:ss`, e.g. `10` or `1:30`). The menu then shows the configured value, e.g.
  *"Loop Mode (10s)"*. Recording fills a buffer of exactly that length and,
  once full, keeps capturing by progressively overwriting from the start —
  like a tape loop. The document never grows past the duration; at any moment
  it holds one contiguous sliding window of the captured stream (the newest
  `duration` seconds), so you can stop whenever you want and keep whatever was
  on the "loop" at that instant.
- **Timed** — clicking it asks for a recording duration and updates the menu to
  e.g. *"Timed (10s)"*. Recording starts as usual and stops automatically when
  the requested amount has been captured; the finished document stays open.

The mode and its duration are remembered between sessions (like the device
selection). If a mode is selected without a usable duration it falls back to
Unlimited behaviour for that take.

### Recorder plugins

Recording is extensible through native DLLs implementing a small standard C ABI
(`include/RecorderPlugin.h`, API version 1). The host loads every `*.dll` found in:

- `<executable directory>/recorders/`
- `%APPDATA%/SpectroCraft/recorders/`

and lists them under **Transport > Recording Device**. New DLLs dropped into
those folders are picked up the next time the menu is opened — no restart needed.

A plugin exports these functions (all return 0 on success; `sc_rec_last_error()`
gives a human-readable message):

| Export | Purpose |
|---|---|
| `sc_rec_get_api_version` | must return `SC_RECORDER_API_VERSION` (1) |
| `sc_rec_get_plugin_info` | id / display name / version string |
| `sc_rec_count_devices`, `sc_rec_get_device` | enumerate devices with their capabilities: min/max channels, supported sample rates and bit depths (16/24/32) — arbitrary channel counts are fine (e.g. a 16-channel ASIO card) |
| `sc_rec_configure(deviceId, wantChans, wantRate, wantDepth, actual)` | select device + desired format; the plugin clamps to what the hardware supports and reports the real values back in `actual` |
| `sc_rec_start` / `sc_rec_stop` / `sc_rec_is_running` | begin/end capture (stop joins the capture thread) |
| `sc_rec_available_frames`, `sc_rec_read(dest, channels, maxFrames)` | **pull model**: the host polls these at 25 Hz; data is always interleaved float32 in [-1, 1] in device channel order. Reads are *consuming* — each frame is delivered exactly once (the plugin's ring buffer advances on read). The plugin must keep a bounded internal buffer (drop-oldest when full) so a stalled host never blocks the capture thread |
| `sc_rec_last_error` | last error message (`""` if none) |

Strings are UTF-8 NUL-terminated; pointers returned by the plugin stay valid only
until the next call into that plugin, so the host copies them immediately.

### The WASAPI recorder (shipped)

`plugins/wasapi/` builds `wasapi_recorder.dll`, deployed to `<exe>/recorders/`
automatically. It enumerates:

- **Loopback devices** — every render endpoint, listed first with a
  `[Loopback]` suffix (Audacity-style "record what you hear"), captured via the
  `AUDCLNT_STREAMFLAGS_LOOPBACK` flag;
- **Capture devices** — every capture endpoint.

It opens endpoints in shared mode only and negotiates both format **and**
stream flags: for loopback it tries, in order, (1) `EVENTCALLBACK | LOOPBACK`
with the alternate SDK bit values (`0x40000 | 0x20000`, as defined by
`AudioSessionTypes.h` on some stacks — this is what Audacity's PortAudio build
uses there), registering an event handle before `Start`; (2) the standard
`AUDCLNT_STREAMFLAGS_LOOPBACK` (`0x4`); then auto-conversion fallbacks (native,
then PCM16). Stock Windows rejects the alternate flags as invalid and falls
down to the standard path, so both stack families work. Exclusive-mode attempts
are deliberately avoided — a few drivers crash inside an exclusive `Initialize`
even though shared mode works fine on the same endpoint (shared is what
Audacity uses). All WASAPI calls run on one dedicated worker thread inside the
DLL.

Notes:
- Loopback capture requires Windows 8+ and works best when something is actually
  playing through that endpoint (silence otherwise).
- On some virtualized/VM audio stacks a loopback session opened with the
  standard flag values streams frames in real time but never delivers captured
  content (all zeros). The alternate-flag attempt above fixes this: on such
  systems loopback now captures real data exactly like Audacity does, and when
  nothing is playing the plugin's silent keep-alive render stream keeps a
  continuous (silent) capture flowing so the host watchdog never starves.
- **Failures are visible.** A zero-data watchdog in the host stops a session that
  has delivered nothing for ~3 s and shows a message box with the plugin's last
  error (`sc_rec_last_error`); stopping such a session manually does the same
  instead of leaving an empty document behind. The WASAPI plugin reports its own
  failures: `configure` explains unsupported channel counts / sample rates (with
  what the endpoint actually offers), and a hard capture failure (e.g.
  `AUDCLNT_E_DEVICE_INVALIDATED`) is recorded as "capture stopped: GetBuffer
  failed (hr=0x…)" so the host can surface it.
- Device labels are composed Audacity-style as `"name (manufacturer)"`:
  first from `IMMDevice::GetProperty(PKEY_FriendlyName)` on standard Windows,
  otherwise from the endpoint property store. Some reduced/virtualized stacks
  expose `IPropertyStore` with a non-standard vtable layout and no working
  `GetProperty`; the plugin detects that at runtime with crash-free ABI probes
  (no SEH — SpectroCraft's first-chance vectored exception handler would
  terminate the process on any deliberate access violation) and reads the
  name/manufacturer keys it finds there, and as a last resort scans every
  string property in the store for the first value that looks like a human-
  readable name (short, no GUID/path syntax). Fallbacks: "Loopback device",
  "Capture device".
- The silent keep-alive render stream (a shared-mode session on the same
  endpoint that pumps silence) is started before capture begins: WASAPI
  loopback only delivers packets while the endpoint's audio engine is actively
  rendering, so without it an idle endpoint yields no frames at all.

### How the host drives it

`src/Recorder.cpp` (`sc::rec::RecorderHost`) loads/unloads plugins, caches each
plugin's device list, and forwards configure/start/stop. While recording,
a 25 Hz `juce::Timer` in `Application` pulls up to one second of audio per tick
through `AudioEngine::appendSamples()` (which resizes the in-memory buffer under
a race guard against the playback thread) and steps the follow-end window
(`stepRecordingWindow`). In **Loop Mode** the pull switches to
`AudioEngine::overwriteSamples()` once the document reaches its configured
length — writing in place at a wrapping position without ever growing the
buffer. Mipmaps are rebuilt throttled during recording (only while the file is
small enough that it matters) and once more on stop. The spectrogram also
re-renders every tick even when the view window does not move: its paint path
invalidates the cached image whenever the engine's content version changes —
appended *and* loop-mode overwritten samples both bump it — so a fixed-window
view shows new data as it arrives (only the newly recorded tiles are computed;
the tiled LRU cache keeps that cheap).

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

Every exe is self-contained after a build: all runtime DLLs (FFmpeg, OpenVINO,
whisper) are copied into a **`SpectroCraft` subfolder next to the executable** by
CMake. The app discovers that folder at startup from its own path and adds it to
its DLL search path — no installation, no environment variables, fully portable:

```
SpectroCraft.exe          <- the application
SpectroCraft\             <- runtime DLLs (OpenVINO, whisper.cpp/ggml, FFmpeg)
recorders\                <- recorder plugins (wasapi_recorder.dll)
```

Older flat layouts with the DLLs directly beside `SpectroCraft.exe` keep working:
Windows always searches the exe directory first, so both layouts are supported and
the exe-directory copy wins when both are present. A local build outputs this
layout to `bin/Release` (untracked — kept locally for testing); official builds
are published as GitHub Releases by the CI workflow.

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

On **GitHub Actions** builds the patch number comes from the workflow run number
(`GITHUB_RUN_NUMBER`) instead, so every CI build and release gets a unique,
monotonically increasing version (`1.0.<run>`, with major/minor taken from `VERSION.txt`
when present) without needing to push `VERSION.txt` back; nothing is written in that mode.

A second custom command (`cmake/patch_generated.cmake`) then rewrites the freshly bumped
version into JUCE's generated files *after* juceaide has written them but *before* they are
compiled:

- `SpectroCraft_resources.rc` — the `VS_VERSION_INFO` resource embedded in the exe, so
  Windows **file properties** (File version / Product version) always carry the new version;

`JuceHeader.h` is deliberately **not** patched: it is included by almost every
translation unit, so rewriting it on every build forced a full rebuild.
`ProjectInfo::versionString` therefore keeps JUCE's default; code that needs the
bumped version (About box, diagnostic log) uses `SC_VERSION_STRING`
from `generated/Version.h`.

(In-place patching is used because this environment's `rc.exe` ignores `/D`
preprocessor definitions, so JUCE's `JUCE_USER_DEFINED_RC_FILE` mechanism cannot be relied on.)

### SIMD dispatch

`src/FFTAvx.cpp` contains an AVX2 radix-2 FFT that is compiled only when the
translation unit is built with AVX2 enabled (`__AVX2__`); the default build
does not enable it, so `scfft::realForward` uses JUCE's FFT (one cached
`juce::dsp::FFT` per order and per thread). When compiled in, the AVX2 path is
used only if CPUID reports AVX2 with OS YMM support and a one-time numerical
self-test against a double-precision DFT passes. (An earlier "CPU erratum"
note was a bug in the vector loop - 8-float vectors stepped by 4 - fixed in
the 2026-11 audit.)

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

### macOS and Linux builds

On macOS (arm64 or x86_64) and Linux (Ubuntu 24.04 x86_64) the same option
fetches the runtimes at configure time instead of using the vendored Windows
x64 DLLs in `extern/OpenVINO/bin`, `extern/whisper-b5454/bin` and
`extern/ffmpeg/bin` (`cmake/ScAiRuntimes.cmake`; nothing binary is committed for
these platforms):

- **OpenVINO 2024.6.0** — Intel's official archive for the platform from
  `storage.openvinotoolkit.org`, checked against its published SHA-256
  (pinned in the CMake file). `-DSC_OPENVINO_DIR=<install>` uses an existing
  OpenVINO 2024.6 installation instead (other distributions / architectures).
- **whisper.cpp b5454** — source archive of the tagged commit (SHA-256
  pinned), built as shared libraries in Release; its `whisper.h` / `ggml.h`
  must be identical to the vendored ones in `extern/whisper-b5454/include`.
  On Linux x86_64 it is built with one ggml CPU backend per micro-architecture,
  like the Windows binaries.
- The libraries the app needs (`libopenvino`, the CPU plugin, the IR
  frontend, `libtbb`, `libwhisper`, `libggml*`) are copied into
  `SpectroCraft.app/Contents/Frameworks` (macOS) or next to the `SpectroCraft`
  executable (Linux, RPATH `$ORIGIN`); ship that folder with the binary.
- `-DSC_AI_DOWNLOAD_DIR=<dir>` keeps the downloaded archives outside the build
  tree (shared between build trees, CI cache).
- Linux needs `libcurl4-openssl-dev` (models are downloaded with JUCE's libcurl
  backend) in addition to JUCE's usual packages; macOS needs nothing more.
- Models are stored in `~/Library/SpectroCraft/models` (macOS) or
  `~/.config/SpectroCraft/models` (Linux). The unit tests use
  `SC_MODELS_DIR` instead (ctest: `<build>/test-models`, a run of
  `--run-unit-tests` by hand: the temp folder), so they never fill the real
  profile.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSPECTRO_ENABLE_AI=ON
cmake --build build --target SpectroCraft
```

### How it works

- **Noise Suppression** (`Effects → Noise Suppression…`) processes the current
  selection on the visible channels. Models: **DeepFilterNet 2** (recommended),
  **DeepFilterNet 3** (experimental) and **DenseUNet** (legacy). The
  *attenuation limit* slider (dB, default 100 = no limit) controls how hard
  the DeepFilterNet models suppress noise: 6–12 for light reduction,
  18–24 for medium. The result is committed as an undoable edit.
  With **Copy noise on new track** enabled, one extra channel is appended per
  processed (visible) channel holding only the estimated noise — original minus
  enhanced — over the selection range; the enhanced signal stays in the existing
  channels and the new tracks are made visible. The whole change (content plus
  channel count) is a single undoable step.
- **Transcription** (the **Transcribe...** button in the Transcription tab) runs Whisper over the whole file
  (visible channels mixed to mono, resampled to 16 kHz with SoXR). A VAD pass
  first detects speech regions and skips long silences entirely — files with
  pauses transcribe several times faster than their length would suggest.
  *Balanced* uses beam search; *Fast* uses greedy decoding (~1.4× quicker,
  slightly lower accuracy on difficult audio). Segments are
  shown in the **Transcription** tab: rows support the same Excel-style
  multi-selection as the other list panels (plain click selects one row,
  Ctrl/Cmd+click toggles a row, Shift+click range-selects from the anchor), and
  right-clicking never disturbs an active multi-selection. Double-click a row to
  jump + select its range and pan/zoom both views until the whole selection is
  visible with ~10% breathing room on each side (same behaviour as marker and
  stored-selection double-clicks); press Ctrl+F (or right-click > Find) to
  search, then F3 / Ctrl+F3 to cycle through matches. Right-clicking a row offers **Edit...**
  (start/end/text), **Add as marker(s)** — one marker per selected segment,
  or just the clicked row when nothing is selected —, **Find...**, **Add entry** /
  **Remove selected**, and **Copy Selected** / **Copy All** / **Paste**;
  right-clicking the list's empty area also offers **Add entry**, the same
  copy/paste items and **Find...**. Copy writes Excel-friendly tab-separated text with a
  Start / End / Text header row (times as M:SS.mmm); Paste accepts that layout —
  the header row is optional, plain seconds are accepted too, and rows are
  inserted in sorted order and selected. New entries are inserted at the selection start (or the
  playhead when nothing is selected) with empty text, so segments can be added
  by hand to files that have no transcription.
  Results are stored as a custom top-level `SCTT` RIFF chunk (UTF-8 JSON,
  times in seconds) when the file uses embedded metadata, or inside the
  `.meta.json` sidecar otherwise — written on save, parsed on load, never shown
  in the Markers panel. Selecting rows and using *Remove selected* clears them
  from the document without touching the audio.
- **Transcribe Folder...** (the **Transcribe Folder...** button in the
  Transcription tab) scans a folder tree
  for `.wav` files and transcribes each one headlessly — nothing opens in the
  editor. Each file's `SCTT` chunk is replaced surgically (all other RIFF chunks
  are preserved byte-for-byte). The progress dialog has a Cancel button that
  stops after the current file; a summary lists done/skipped/failed per run.
  *Skip files with SCTT* re-runs only transcribe files without an existing
  transcription. The **Channel to analyse** field takes a 1-based channel
  number (default: 1); enter `0` or less to mix all channels together before
  transcribing. A file whose channel count is below the requested number fails
  that file only, with the reason listed in the summary.
- **Device selection** — the noise-suppression dialog offers a *Device*
  dropdown: *Auto* (default) picks GPU over NPU over CPU when the OpenVINO
  runtime exposes it, otherwise CPU; picking *CPU*, *GPU* or *NPU* explicitly
  overrides that. A device that is not available falls back to CPU and the
  progress dialog says so. The choice persists between sessions. Transcription
  has no device option: whisper.cpp's prebuilt runtime is CPU-only.
- **Model preferences persist** between sessions (last model, language, mode,
  batch channel and the auto-download toggle) in the same `SpectroCraft.recent`
  store as the recent-files list. With *auto-download off*, missing models are never
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
in the `SpectroCraft` folder next to `SpectroCraft.exe` for noise suppression — they
are vendored in `extern/OpenVINO/bin` and copied there by the CMake build. A
prebuilt exe without them fails gracefully; even a *partial* set (e.g. only
`openvino.dll`) is detected and the dialog lists all four required files plus where
to get them. Transcription needs the **whisper runtime DLLs** (`whisper.dll` plus
12 ggml/llama DLLs — all vendored in `extern/whisper-b5454/bin`, see
`extern/whisper-b5454/README.md`) in that same folder; the CMake build copies them
there too, and a missing or incompatible set is reported with a clear error.

To use a **GPU or NPU**, install its plugin DLL from the same OpenVINO release
(`runtime\Windows\bin`): `openvino_intel_gpu_plugin.dll` for GPU,
`openvino_npu_plugin.dll` for NPU — into the same `SpectroCraft` folder. Until then the
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

## Stem separation

**Effects → Stem Separation…** separates an audio element you describe in plain
language ("birds", "a cello", "the bass drum") from the current selection — or
from the whole file when nothing is selected. It runs the prebuilt
`audiocpp_cli` tool (`audiocpp_cli.exe` on Windows) as a child process; no part
of it is compiled into SpectroCraft. It works on Windows x64, macOS (Apple
silicon and Intel, with the Metal GPU backend) and Linux x64.

The **Mode** dropdown selects the separation family:

- **Sam Audio** (default) — text-prompted extraction with anchors, as described below.
- **htdemucs_6stems** / **htdemucs_ft** — plain source separation with no
description or anchors: the tool writes one named `.wav` per stem into its output
folder and every file becomes a track. These families require a 44.1 kHz input,
so the extracted range is resampled to 44.1 kHz first when the source rate differs
(the stems are resampled back afterwards). In these modes the description area of
the dialog is hidden entirely.
- **htdemucs-whine-hiss_v1** / **htdemucs-whine-hiss-click-bump_v1** — plain
separation with a single `.gguf` model from the Super3dMax HuggingFace repos,
aimed at whine + hiss (and click + bump) artifacts. Like the other htdemucs
families they need 44.1 kHz stereo input: a mono range is duplicated to L=R and
every stem comes back downmixed to mono; a file with more than two channels is
processed channel by channel, each source channel as its own duplicated-stereo run
and every stem coming back as one mono track named `ch<i>_<stem>`. The model for
these modes can be fetched from the dialog's **Download…** button. In these modes
the description area of the dialog is hidden entirely.

### Getting the binary and models

The dialog's **Download…** button fetches what is missing (with progress and
cancel):

- **Binary** — one variant of this platform's build from the
  [0xShug0/audio.cpp](https://github.com/0xShug0/audio.cpp) v0.9.2-drama3-base-small
  release, checked against its published SHA-256:
  - Windows x64: *CPU portable* (~29 MB), *CUDA 12.4* (~465 MB), *CUDA 13.3*
    (~276 MB) or *Vulkan portable* (~63 MB) — a zip, of which only
    `audiocpp_cli.exe` is extracted (so the CUDA variants do not start yet: they
    also need the zip's `ggml*.dll`);
  - macOS: *Metal* for Apple silicon (~33 MB) or for Intel Macs (~35 MB) — a
    self-contained `audiocpp_cli` with the `cpu` and `metal` backends;
  - Linux x64: *CPU portable* (~61 MB) or *Vulkan portable* (~75 MB) —
    `audiocpp_cli` plus the `libggml*.so` libraries it loads from its own folder
    (it also needs the system's `libgomp.so.1`, and the Vulkan loader for the
    Vulkan variant).

  The archive is extracted into the models folder, the executable checked
  against its own published SHA-256, and a variant marker is written so the
  dialog knows what you have.
- **Models** — for Sam Audio mode, any number of `sam-audio-*.gguf` weights from
  [audio-cpp/SAM-Audio-GGUF](https://huggingface.co/audio-cpp/SAM-Audio-GGUF/tree/main)
  (small/base/large in q8_0/bf16/f32). The model dropdown lists every `.gguf`
  found, so you can install more manually and they appear automatically.
- **htdemucs models** — `htdemucs_6stems` uses any `.gguf` file placed in
  `<models>/audiocpp/HTDemucs-6stems-GGUF/`, and `htdemucs_ft` uses the folder
  `<models>/audiocpp/htdemucs_ft/`, which must contain at least one `.gguf`
  file. These two are not offered by the downloader — install them manually (e.g.
  `htdemucs-6stems-q8_0.gguf`, 42 MB, from
  [audio-cpp/audio.cpp-gguf](https://huggingface.co/audio-cpp/audio.cpp-gguf/tree/main/HTDemucs-6stems-GGUF)).
  The whine-hiss modes are offered by the downloader: **htdemucs-whine-hiss_v1**
  takes any `.gguf` from `<models>/audiocpp/HTDemucs-WhineHiss-v1-GGUF/` (fetched
  from [Super3dMax/htdemucs-whine-hiss_v1](https://huggingface.co/Super3dMax/htdemucs-whine-hiss_v1),
  ~84 MB f16 or ~99 MB q8_0) and **htdemucs-whine-hiss-click-bump_v1** takes any
  `.gguf` from `<models>/audiocpp/HTDemucs-WhineHiss-ClickBump-v1-GGUF/` (fetched
  from [Super3dMax/htdemucs-whine-hiss-click-bump_v1](https://huggingface.co/Super3dMax/htdemucs-whine-hiss-click-bump_v1),
  ~62 MB q8_0 or ~84 MB f16). The model dropdown lists every `.gguf` found in the
  folder, so you can install more manually and they appear automatically.

Everything lives in `<models>/audiocpp/`, where `<models>` is
`%APPDATA%\SpectroCraft\models` (Windows), `~/Library/SpectroCraft/models` (macOS)
or `~/.config/SpectroCraft/models` (Linux) — put the CLI (with the libraries
next to it in its release archive) and one or more `sam-audio-*.gguf` files
there by hand if you prefer not to use the downloader. The *Backend* dropdown
(*Cpu* / *Cuda* / *Vulkan*, plus *Metal* on macOS) should match the installed
binary variant; the status line warns when it does not (the macOS build accepts
both *Cpu* and *Metal*).

### How a run works

1. The time range comes from the dialog's editable **Start**/**End** fields
   (seeded once from the current selection when the window opens; **Select All**
   resets them to the full duration), with the resulting length shown in the
   **Duration** label next to them. That range is extracted to a temporary WAV.
2. For each description, `audiocpp_cli.exe` runs with
   `--task s2s --family sam_audio`, the chosen model and backend, the text,
   optional anchor ranges and an output directory; its log lines stream into
   the progress dialog (cancellable). In htdemucs modes a
   `--task sep --family htdemucs[_6stems]` run is made instead — no text or
   anchors: one per source channel when the file has more than two channels,
   and mono input is duplicated to L=R first (the stems are downmixed back).
3. The tool's `target.wav` (the described element) and `residual.wav`
   (everything else) are read back, resampled to the source rate if needed and
   padded to equal length. With several descriptions only the LAST residual is
   kept — an intermediate one would just repeat what the next step already
   removes.
4. The result is applied according to the **Output** dropdown at the bottom of
   the dialog (persisted between sessions):
   - **Copy stems to new file** (default) — a new document `<name>_stems` opens
     with one track per output channel, ready for further editing or saving as
     WAV/FLAC/AIFF. In Sam Audio mode that is one target per description plus the
     final residual; in htdemucs modes it is the tool's named stems.
   - **Overwrite current file** — the open file's content is replaced by the
     stems (one undoable step). Markers inside the extracted range keep their
     relative position, everything else is dropped with the old timeline, and the
     playhead/selection follow suit.
   - **Append to current file** — every original track stays untouched and each
     stem becomes a new track holding its data over the extracted time range
     (silence elsewhere), so the stems line up in time with what was separated
     from them. One undoable step covers all of it.

   If you switch documents while a run is in flight, overwrite/append falls back
   to creating a new file rather than writing into the wrong document.

When a run finishes successfully the Stem Separation window closes itself;
on failure or cancellation it stays open so you can read the status and retry.

### Advanced mode

**Advanced…** expands the dialog (the single *Description* box is hidden —
each advanced row carries its own editor, with a tooltip explaining it), and
the **Add Stem** button below the last description row appends another row so
you can describe several elements in one run. Each row has:

- a text field,
- **Include Selection** / **Exclude Selection** buttons that add the current
  track selection as an anchor range (times are converted to the extracted
  input's timeline when the run starts). Every added anchor appears in a list
  under its description as `Included: t0 - t1 s` or `Excluded: t0 - t1 s`, and
  each listed entry has its own **Remove** button (the row's **Remove** still
  drops the whole description),

Anchors reach the tool as `--request-option "anchors=[[\"+\",t0,t1],…]]`
(`+` = include, `-` = exclude). The **Extract subsequent stem from original**
checkbox (on by default) is shown in advanced mode once there is more than
one description: checked, every description runs against the original
extraction; unchecked, each description after the first works on the previous
*residual* instead — a simple way to peel several elements off one recording
in sequence.

Subsequent descriptions never work on the whole multichannel extraction: the
first description gets the full extracted range as-is, but every later one
works on a single mono track — either the previous run's *residual* (chained
mode) or a downmix of the original extraction (from-original mode). Mono
sources are unaffected since their input is already one track.

The window stays above the main window while open (global topmost), so making
include/exclude selections in the track view does not push it behind.

The last mode, model (per mode), backend, extract-from-original and output mode
choices persist between sessions in `SpectroCraft.recent`.

### Remembered descriptions

Descriptions you have typed are remembered for the session: closing the
window (or a successful run) stores them, and reopening restores them —
multiple descriptions, or any description with anchors, reopen straight into
advanced mode. Anchor ranges are absolute document times, so on restore an
anchor is kept only if it still fits inside the current file's duration;
anchors that no longer fit are dropped while their text is kept.

---

## Super resolution

**Effects → Super Resolution…** enhances a time range of the current file into
a single *mono 48 kHz* track — both model families downmix and resample to
their native pipeline internally, regardless of the source's rate or channel
count. Like Stem Separation it drives the prebuilt `audiocpp_cli` as a
child process; nothing is compiled into SpectroCraft.

The **Mode** dropdown selects the family:

- **FlashSR** (default) — the small, fast super-resolution utility that ships
  inside audio.cpp itself (`--family builtin_audio_utils` with
  `--load-option utility=flashsr`).
- **AudioSR** — a GGUF model from the [audio-cpp/audio.cpp-gguf](https://huggingface.co/audio-cpp/audio.cpp-gguf)
  repository; any `.gguf` placed in the `AudioSR-GGUF` folder is listed.

### Getting the binary and models

The dialog's **Download…** button fetches what is missing (with progress and
cancel), using the same CLI-binary variants as Stem Separation:

- **FlashSR model** — `flashsr.safetensors` (~0.3 MB) from the audio.cpp
  repository assets, installed to
  `<models>/audiocpp/flashsr/flashsr.safetensors` (`<models>`: see Stem separation).
- **AudioSR models** — e.g. `audiosr-basic-f32.gguf` (~5.8 GB) from the
  Hugging Face repository above, installed to
  `<models>/audiocpp/AudioSR-GGUF/`. Install any other
  `.gguf` there by hand and it appears in the model dropdown automatically.

The *Backend* dropdown (*Cpu* / *Cuda* / *Vulkan*, plus *Metal* on macOS) should
match the installed binary variant (shared with Stem Separation).

### How a run works

1. The time range comes from the dialog's editable **Start**/**End** fields
   (seeded once from the current selection when the window opens; **Select All**
   resets them to the full duration), with the resulting length shown in the
   **Duration** label next to them. That range is extracted to a temporary WAV.
2. `audiocpp_cli` runs with `--task s2s`, the chosen family/model/backend,
   the input WAV and an output path; its log lines stream into the progress
dialog (cancellable). The tool writes one enhanced WAV straight to that path.
3. The result is applied according to the **Output** dropdown at the bottom of
   the dialog (persisted between sessions):
   - **Copy result to new file** (default) — a new document `<name>_superres`
     opens with the enhanced mono track at its native rate (48 kHz).
   - **Overwrite current file** — the open file is replaced by the enhanced
     track, including any sample-rate change (one undoable step; undo restores
     the old content *and* the old rate). Markers inside the extracted range
     keep their relative position, everything else is dropped with the old
     timeline, and the playhead/selection follow suit.
   - **Append to current file** — every original track stays untouched and the
     enhanced track becomes one new channel holding its data over the extracted
     time range (silence elsewhere), so it lines up in time with what was
     enhanced. The result is resampled back to the document's rate first.

   If you switch documents while a run is in flight, overwrite/append falls
   back to creating a new file rather than writing into the wrong document.

When a run finishes successfully the Super Resolution window closes itself;
on failure or cancellation it stays open so you can read the status and retry.
The mode, model (per mode), backend and output-mode choices persist between
sessions in `SpectroCraft.recent`.

---

## Running & CLI flags

```
SpectroCraft.exe                          :: launch GUI
SpectroCraft.exe --run-unit-tests         :: run the unit test suite (isolated settings store),
                                           :: exit code 0 on success, 1 if a check fails
SpectroCraft.exe --generate-test-files [dir]   :: write tests/data/*.wav (default: tests/data)
SpectroCraft.exe --test-spectral <in.wav> <out.png> [width height t0 t1]
                                           :: offline spectrogram render, e.g.
SpectroCraft.exe --test-spectral tests/data/tone_10s_stereo.wav out.png 512 256 0.0 10.0
SpectroCraft.exe --test-load <in.wav>
                                           :: headless load timing + mipmap stats,
                                              e.g. for multi-hour files
SpectroCraft.exe --diag-log                 :: launch the GUI with the diagnostic log on for this
                                           :: session (see "Diagnostic log");
                                           :: SC_DIAG_LOG=1 or SC_DIAG_LOG_DIR=<folder> do the same.
                                           :: Its "startup" event times the launch phases
                                           :: (process start -> audio device creation ->
                                           :: properties file -> views -> window shown; the
                                           :: background WASAPI open follows as startup.phase).
SpectroCraft.exe --run-action <actionName> <inputFile> <outputFile>
                                           :: runs a recorded action (see the Actions panel)
                                              on one file headlessly and saves the
                                              result to outputFile. The action is
                                              looked up in the user's recent store.
SpectroCraft.exe --batch <actionName> <inFolder> <outFolder> [formats] [rename]
      [--subfolders]
                                           :: batch-runs a recorded action over every file
                                              matching `formats` (a ';'-separated list
                                              like "*.wav; *.mp3", default "*.wav") in
                                              the folder tree. Output files are written
                                              to outFolder with the rename pattern
                                              applied (%Filename% = original name without
                                              extension, default "%Filename%.wav");
                                              --subfolders scans recursively and mirrors
                                              the subfolder structure into outFolder.
```

Exit codes: `--run-action` returns 0 on success, 1 when the action, a step,
the input or the output fails, and 2 for a usage error. `--batch` returns 1
only for a setup failure (missing folder, no matching file, unknown action);
per-file failures are listed in the printed summary but do not change the exit
code. `ctest` (after building) runs the unit suite through
`SpectroCraft --run-unit-tests` (registered in `tests/CMakeLists.txt`).

AI builds additionally accept (see [AI features](#ai-features)):

```
SpectroCraft.exe --test-noise-suppression <in.wav> <out.wav> [modelId]
                                           :: headless noise suppression, e.g.
SpectroCraft.exe --test-noise-suppression speech.wav out.wav deepfilternet2
SpectroCraft.exe --test-transcribe <in.wav> [modelId] [language]
                                           :: headless Whisper transcription,
                                              prints the segments
SpectroCraft.exe --test-batch-transcribe <folder> [modelId] [language]
      [skipExisting 0|1] [fast 0|1] [--translate] [--channel N]
      [--no-auto-download] [--meta-mode auto|embedded|external]
                                           :: transcribes every .wav in a folder
                                              tree and writes each SCTT chunk.
                                              Defaults: whisper-base, auto
                                              language, overwrite existing,
                                              balanced speed, channel 1 (a
                                              value of 0 or less mixes all
                                              channels together). --meta-mode
                                              forces where each file's metadata
                                              ends up after the run (auto keeps
                                              what it found): embedded moves
                                              everything into RIFF chunks in
                                              the WAV; external consolidates it
                                              into a .meta.json sidecar.
SpectroCraft.exe --test-ns-failure-alert <in.wav> [modelId] ["fake error"]
                                           :: shows the AI failure dialog (with a
                                              copyable details box)
```

The test executable accepts the same flags (useful for CI without a display).

---

## AI control (Claude Code / MCP)

SpectroCraft can be driven by Claude Code (VS Code extension or terminal) **while it is
open**: state, history, undo/redo, any of the 20 action steps on a time range
and explicit channels, views, playback, markers, long jobs (AI, Python) with progress. The app
starts a control server on the **loopback interface only** (`127.0.0.1`, ephemeral port,
token specific to each instance) when *Window > Allow AI control (localhost)* is checked (disabled by
default) or when it is launched with `--control`. The token and the port are published in
`<application data>/SpectroCraft/control/<pid>.json` (macOS `~/Library/SpectroCraft/control`,
Linux `~/.config/SpectroCraft/control`, Windows `%APPDATA%\SpectroCraft\control`, file 0600 on
macOS/Linux). A small Python MCP adapter (`tools/mcp`) connects Claude Code to this server.

The same commands run without a window:

```
SpectroCraft --json <requests.ndjson | -> [--json-out <responses.ndjson>] [--use-user-settings]
SpectroCraft --list-commands [--markdown]     :: list of commands (JSON Schema of the parameters)
SpectroCraft --control                        :: GUI + control server (never forwarded
                                              :: to an already open instance)
```

One request per line (`{"id":1,"cmd":"step.run","params":{"select":{"start":1.0,"end":2.5},
"step":{"type":"changeGain","params":{"db":-3}}}}`), one response per line; exit code 0 (all
ok), 1 (at least one error), 2 (usage, unreadable JSON, line > 1 MiB, unknown option). Any
`--xxx` option unknown to the executable now gives exit code 2 instead of starting the interface.

Installing the adapter (Python 3.11+):

    cp .mcp.json.example .mcp.json      # set SPECTROCRAFT_EXE to your executable
    # with uv: nothing else (uv run creates tools/mcp/.venv)
    # without uv: pip install -e tools/mcp, then "command": "python3", "args": ["-m", "spectrocraft_mcp"]

Open the folder in VS Code (or run `claude`) then ask, for example, "open recording_b.wav,
select 2–3.5 s, apply a fade-out and show me the history".
`python3 tools/mcp/sc_client.py app.state` checks the connection without the `mcp` package.

Security: loopback only, mandatory token (`session.hello`), opt-in, paths limited to the home
folder, the scripts folder and the temporary folder (`control_roots` setting), `confirm:true`
required for destructive operations (overwriting a file, closing an unsaved document, Python
script outside the scripts folder). CMake option `-DSC_ENABLE_CONTROL=OFF` for a build without
the control server.

Details: [docs/ai-control/mcp.md](docs/ai-control/mcp.md) (macOS/Windows/Linux installation,
troubleshooting); protocol: [docs/ai-control/REPORT.md](docs/ai-control/REPORT.md) §6; progress:
[docs/ai-control/progress/](docs/ai-control/progress/).

---

## Diagnostic log

*Help > Diagnostic Log* (off by default, saved) keeps a small local log that helps find
performance problems and bugs. It records the errors and warnings shown, crashes, interface
freezes, audio dropouts, slow operations (file loads and saves, spectrogram tiles, paints, commands,
long jobs), the startup phase timings, and short breadcrumbs: the commands and menu items used and
the files opened. It never records audio, transcribed text, table values, scripts or the AI control
token, and nothing is sent anywhere. File paths are written in clear (internal use). The dialog
shown when it is turned on links to its folder. The status bar shows *Diagnostic log* while it
runs.

- Files: JSON Lines in `<application data>/SpectroCraft/diagnostics` (macOS
  `~/Library/SpectroCraft/diagnostics`, Linux `~/.config/SpectroCraft/diagnostics`, Windows
  `%APPDATA%\SpectroCraft\diagnostics`), 2 MB per file, 5 files at most.
- *Help > Open Diagnostic Log Folder*, *Help > Copy Diagnostic Summary* (Markdown, 30 KB at most, to
  paste into a support request or a chat) and *Help > Create Support Bundle...* (zip of the logs,
  crash reports and a summary, listed before it is saved).
- Startup: once the main window is shown, a `startup` event gives the launch time and each phase
  (audio device creation, settings file, views, window shown); the audio device, opened in the
  background, follows as a `startup.phase` event. Turn the log on, then restart the app to time a
  launch. (This replaces the former `--measure_load_time` option and its separate log file.)
- With *Allow AI control*, `diag.summary` and `diag.tail` (MCP tools `sc_diag_summary`,
  `sc_diag_tail`) let Claude Code read the log of the open app.
- A crash, a force quit or a killed thread is reported at the next start (`session.unclean`).
- `--diag-log`, `SC_DIAG_LOG=1` or `SC_DIAG_LOG_DIR=<folder>` turn it on for one session.

Schema, events and limits: [docs/diagnostics.md](docs/diagnostics.md).

---

## Keyboard shortcuts

| Action | Shortcut |
|---|---|
| Open file | Ctrl+O |
| Close active file | Ctrl+W or Ctrl+F4 |
| Cycle through loaded files (forward / backwards) | Ctrl+Tab / Ctrl+Shift+Tab |
| Save / Save As | Ctrl+S / Ctrl+Shift+S |
| Undo / Redo | Ctrl+Z / Ctrl+Y |
| New file (empty document, choose channels + rate) | File menu |
| Cut / Copy / Paste | Ctrl+X / Ctrl+C / Ctrl+V |
| Paste Mix (add the clipboard at the selection start instead of replacing; appends past end-of-file if it extends that far) | Ctrl+Shift+V |
| Paste to New Channel(s) (appends one new channel per clipboard channel; existing channels untouched, length unchanged) | Edit menu |
| Select all | Ctrl+A |
| Delete time selection (or **zero the spectral selection** when one is active) | Del / Backspace |
| Trim Selection (keep only the selected range; deletes everything before and after it in one undoable step) | Ctrl+T |
| Play / Pause toggle | Space |
| Move playhead left / right (one percent of the visible time span per press, so zooming in shrinks the step down to exactly one sample at deep zoom) | ← / → |
| Step the playhead by one video frame (1/60 s without a video) | Ctrl+← / Ctrl+→ |
| Select while moving the playhead (anchor = position at first Shift press; released when Shift is let go) | Shift+← / Shift+→ |
| Fine selection, one sample per step | Ctrl+Shift+← / Ctrl+Shift+→ |
| Move playhead and pan both views to the start / end (keeps the current zoom span) | Home / End |
| Loop (selection, or whole file if nothing selected) on/off | Ctrl+L |
| Find in the active list panel (Markers / Transcription tab; right-click > Find also works) | Ctrl+F |
| Find next match (opens the dialog first when no query is set yet) | F3 |
| Find previous match | Ctrl+F3 |
| Go to Time... (jump the playhead by time, sample count or any alternative-scale value) | Ctrl+G |
| Add marker (region if selected, else point at cursor) | F2 / F8 |
| Selection tool: Time Range / Marquee / Lasso / Paintbrush | S / M / L / P |
| Brush size smaller / larger (Paintbrush only; % of cursor frequency) | [ / ] |
| Paintbrush: straight line from the last stroke's end point to the click position (straight in linear-frequency space) | Alt+click |
| New drawing on the spectrogram: Text / Arrow / Rectangle / Free Drawing | T / A / R / D |
| Cancel a pending drawing creation (otherwise clears the active spectral selection) | Esc |
| Zoom in / out (horizontal, both views) | Ctrl+= / Ctrl+- |
| Zoom Out Full — reset the time window and both vertical ranges (Zoom Fit is menu-only now) | Ctrl+0 |
| Vertical zoom in / out (waveform amplitude + spectral frequency) | Ctrl+Shift+= / Ctrl+Shift+- |
| Spectrogram FFT size one step up / down the 256..65536 ladder; from Auto, switches to manual one step away | Alt+Shift+= / Alt+Shift+- |
| Toggle a channel's visibility (like the View menu items); show ONLY that channel with Shift held — pressing again while solo restores every channel | Ctrl+1..9 / Ctrl+Shift+1..9 |
| Cycle view layout: both → waveform only → spectrogram only (reverse direction with Shift) | F9 / Shift+F9 |
| Start/stop recording (Transport > Record; mode via Transport > Recording Options) | Shift+F10 |
| Change Gain... (dB dialog on the selection or whole file) | Ctrl+Shift+G |
| Raise / lower the current selection by 6 dB, shaped as an ease-in / hold / ease-out envelope (works on a spectral selection too; needs a selection, otherwise the key is ignored) | + / - |
| Same, 1 dB steps | Shift + / Shift - |
| Normalize (selection peak to full scale) | Ctrl+Shift+N |
| Fade In / Fade Out (on the time selection, or whole file when none) | Ctrl+Shift+I / Ctrl+Shift+O |
| Filter EQ... (graphical curve equalizer dialog) | Ctrl+Shift+E |
| Time Stretch... (SoundTouch tempo change dialog) | Ctrl+Shift+T |
| Repeat last effect (re-applies the most recent Effects-menu edit; re-opens its dialog for dialog-based effects like Change Gain / Filter EQ / Time Stretch / Content Aware Time Scale / Envelope) | Ctrl+Shift+F |
| Resample... (Edit menu, own section: new sample rate + optional channel remix) | F11 |
| Run selected action (Actions panel; opens the prompt dialogs for *prompt* steps) | Ctrl+Shift+P |
| Split Spectral Selection Configuration — per-channel input → destination setup; closing saves without splitting | Shift+F12 |
| Split Spectral Selection Execute — runs the configured split (needs an active spectral selection); first time per file open opens the configuration instead | F12 |
| Refresh spectrogram (Spectral Controls > Refresh: discard every cached tile and re-render all tracks) | F5 |
| Toggle spectrogram harmonic markers (Spectral Controls > "Show harmonics") | Ctrl+H |
| Show / hide the panels around the Editor (View > Panels: maximises the Editor's group) | Ctrl+Shift+Enter |
| Maximise / restore the panel group under the mouse (also: double-click a tab) | ` (backtick) |
| While dragging a panel: cancel the drag / undock instead of docking | Esc / hold Ctrl (Cmd on macOS) |

Keyboard focus follows the mouse: clicking either view puts keyboard focus on
that view, so ←/→ and Shift+←/→ work immediately after any click inside a plot.
Clicking neutral areas (splitters, scrollbars, empty space) lands focus on the
main window itself — arrow keys keep working there too. Interactive widgets
(combo boxes, text fields, the marker table) take focus when clicked and keep
the standard Windows behaviour for arrow keys while focused; click back into a
view to return playhead control.

Menus: **File, Edit, Selection, Transport, Effects, View, Window, Help** (the
Window menu holds the **Workspaces** submenu and **Maximize Panel Group**,
opens/closes the individual panels — **Video** included — and carries the **Allow multiple
instances** toggle — see above; the **Help** menu holds **Show Tooltips** (on by default,
remembered: off hides every hover tip in all windows; every button, list, slider and text field
of the panels and tool windows has one, as do the panel tabs, the workspace bar and the channel
labels of the views), the **Diagnostic Log** toggle and its
folder, summary and support-bundle actions). The Edit menu has a
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
Fade In/Out, Normalize, Reverse, **Invert**, **Time Stretch…** (SoundTouch
tempo change that preserves pitch; changes the file length and warps markers,
saved selections and FFT snapshots through the same time-warp),
**Content Aware Time Scale…** (content-aware stretch with a Time Stretch /
Reduce Gaps mode drop-down: stable segments are removed or duplicated based
on STFT frame-to-frame differences, transients protected; see above) and
**Filter EQ…** (graphical curve equalizer applied as a zero-phase FFT filter;
see [above](#features)), **Envelope…** (amplitude-over-time envelope drawn on
a time/dB graph, applied per sample; spectral-selection aware) — plus
**Change Gain...** (a dB dialog whose
field opens focused with its value pre-selected so a new one can be typed in
right away; it also has a -60..+24 dB slider kept two-way synced with the box —
dragging rewrites the field, typing moves the slider clamped to its range,
and OK applies whatever is in the field; it multiplies the selection by
10^(dB/20)) — all applied only to
the currently visible channels.

**Quick gain on a selection (+ / -)**: with a time or spectral selection active,
**+** and **-** raise or lower it by 6 dB (**Shift + / Shift -** for 1 dB). The
change is not a flat gain: it is applied as an envelope — a raised-cosine ramp up
to the full gain over the first 25% of the selection, that gain held through the
middle, and a mirror ramp back to 0 dB over the last 25% — so a boost never
starts or ends with an audible step at the selection edges. On a spectral
selection only the masked content moves, as with the other masked effects. Every
press is one undoable step; with no selection the keys do nothing. The View menu has **Zoom to Selection** / **Zoom Out Full**, checkable
**Waveform View / Spectrogram View** items (at least one always stays visible)
and one checkable item per channel with its keyboard shortcut shown as a hint
(**Ctrl+(Shift)+1..9**: Ctrl+N toggles the channel, Ctrl+Shift+N shows only it);
plain click also toggles that channel. The spectrogram colour map is chosen in
the Spectral Controls panel (Classic default) rather than from a View-menu
submenu.

**Remembered between sessions**: the spectrogram's Log/Linear state and FFT
size, the Frequency Analysis axis mode (FFT Linear / FFT Log / 1/3 Octave /
Freq vs Time / Amp vs Time / Amp vs Freq), its independent FFT size, its
1/3-octave order, and the curve-mode options (Ref Time, Smooth, Track) are
saved to the same properties file as the recent-files list and restored on
startup.

Mouse (waveform *and* spectral views):

| Gesture | Action |
|---|---|
| Click | Move the playhead to that position in **both** views (selection drags start from it) |
| **Deep zoom (waveform only, fewer than 60 samples in view)** | Every sample is drawn as a round dot in the curve colour. **Hovering** a dot marks it with a slightly larger hollow ring and prints its value next to it in the same green (in the current vertical scale: Wav Units / dB / Normalized, also in the status-bar probe). **Press and drag a dot** sets that sample's amplitude to the value under the cursor: the dot follows the cursor while dragging and the file is written when you release, so one drag is one undoable step. A press that misses every dot behaves like a normal click (seek / drag-select); releasing a dot without having moved it seeks to that sample and records nothing. The neighbouring samples are never touched |
| Left-drag | Select a time range. With several visible channels the selection rectangle is clipped to the **channel row it started on** — dragging over another channel does not paint over it (Select All, marker double-clicks and typed footer ranges span all rows) |
| Left-drag on the amplitude/frequency axis strip (left or right) | **Vertical pan**: drag up/down to shift the amplitude window (waveform) or frequency window (spectral, log-aware). The content follows the cursor at half speed; panning against a range limit pins that edge and keeps the span. Does not move the playhead |
| Left-drag on the time ruler (top or bottom) | Pan the shared time window at half cursor speed (same as middle-drag); does not create a selection |
| Middle-drag | **Pan both axes at once**: the shared time window horizontally (both views stay in sync) and the amplitude window (waveform) or frequency window (spectral) vertically, from anywhere in the plot. The content follows the cursor at half speed on both axes; panning against a file edge or range limit pins that edge and keeps the span, and reversing direction responds immediately (no dead zone) |
| Right-drag a region | **Box zoom**: dragging on the track zooms **both** axes at once — time from the box's horizontal span, amplitude (waveform) or frequency (spectral) from its vertical span. With several visible channels the box's vertical extent is clipped to the **channel row it started on**, and the vertical zoom factor is measured against that single row's data height (so a multi-channel file zooms exactly like a single channel would). Dragging that starts on an axis zooms only that axis: the amplitude/frequency axis strip (left or right) → vertical range, the time ruler strip (top or bottom of whichever view draws it) → time range. A rubber-band rectangle is shown while dragging |
| Right-click | Context menu: Zoom to Selection, Zoom Out Full, Select All (Ctrl+A), Cut / Copy / Paste / Delete, plus **Scale…** which opens the Scale form for this file (every time/amplitude/frequency axis: units, offset, scale, order, visibility, Add Axis vs Footer Only). On the **amplitude/frequency axis strip** (on either side), "Zoom Out Full" resets that view's *vertical* range (amplitude/frequency) instead of the time window. In the spectral view, right-clicking **on the current spectral selection** additionally offers **Move Selection** and **Scale Selection** |
| Mouse wheel over the plot / time ruler | Horizontal zoom around the cursor (both views) |
| Mouse wheel over the amplitude/frequency axis strip (left or right; or Shift + wheel anywhere) | Vertical amplitude zoom (waveform) / frequency-range zoom around the cursor (spectral) — independent per view |
| Drag the divider between the views | Resize waveform vs spectral area |
| Double-click a marker (line/region) | Set the selection to that marker's range (both views) |
| Double-click elsewhere | **Select the whole file** and fit it in both views |

Selecting a time range in *either* view applies the selection to **both**
views at once. A drag-selection remembers the channel row it started on and
is drawn clipped to that row in both views; selections created elsewhere
(Select All, marker double-clicks, footer fields, keyboard) span every row.

### How the waveform draws at each zoom

The waveform picks between three drawing levels according to how many samples
fall in one pixel column of the plot:

| Zoom | What is drawn |
|---|---|
| A pyramid block still fits inside one column | Filled min/max envelope columns taken from the min/max pyramid built when the file is opened |
| A pyramid block would be **wider** than the column it fills | The same filled columns, but each column's min/max is computed **from the file itself** |
| Up to 4 samples per column | The samples joined as a polyline (the true sample shape) |
| Fewer than 60 samples in the whole view | The polyline **plus one dot per sample** (see the gesture table above) |

The second level is what keeps a deep zoom smooth: a pyramid block coarser than
the column it is drawn into repeats the same min/max across several columns,
which reads as a staircase of block-wide steps. As soon as that would happen the
exact per-column values are read from the file instead — cheap, because the view
is short there. The pyramid is only used where it still resolves the picture.

### Axis scales (right-click → Scale)

Both views share a right-click **Scale** menu that changes how axis values are
*displayed*. The underlying data, zoom windows and selections never change;
the settings apply session-wide (rulers, footer fields, playhead readout,
cursor probes and the marker panel) until changed again and are not persisted
across restarts.

- **Time Axis** (both views):
  - **Time** (default): seconds, with clock-style labels past a minute.
  - **Time Offset...**: a dialog asks for the time that should appear at the
    playhead position — plain seconds (`-123.5`) or clock style
    (`hh:mm:ss.xxxx`, negative and multi-hour values both work); from then on
    every time readout shows *time + offset* (with a minus sign when it goes
    below zero), and typing a clock value in any footer/marker field applies it
    with the offset removed. Ruler ticks land on round clock values.
  - **Sample**: times are shown as sample counts at the active file's rate;
    ruler ticks land on nice 1-2-5×10^k sample steps chosen from the current
    zoom (samples-per-pixel), so numbers stay visible at every magnification,
    and footer/marker fields accept integer or decimal sample counts (the
    length field is a duration in samples).
- **Scale...** (both views): the **Scale form**, one modeless dialog in its own
  window (title bar with a close button) that stays above the main window: three
  columns (Time / Amplitude / Frequency), one block per axis with its settings
  stacked under its name, **New** at the bottom of each column, **SAME** to copy
  the column's main axis, and reordering by **dragging a name** inside its
  column (no arrow buttons)
  that lists every axis of the current file. It replaces the old **Scale**
  submenu (Time Axis / Vertical Axis) and the **Alternative Scale** submenu.
  Each row is one axis: **Name** (with a drag grip), **Units**, **Offset**,
  **Scale**, **Type** (**Add Axis** draws a strip on the tracks, **Footer Only**
  only adds a readout — the old alternative scale), a **visibility eye** (the
  same icon the tables use; its tooltip says **Hide Axis** / **Show Axis**) and
  an **x** to remove the axis; **Add Axis** adds a row. The **x only appears
  when the column holds more than one axis**, and it is the same size as the
  eye, sitting right next to it and ending at the same margin as the text boxes
  below them. Units are
  `hh:mm:ss.xxxx` / `ss.xxxx` / `samples` for time, `dB` / `None` / `Volt` /
  `Pascal` / `g` for amplitude (a physical unit calibrates the file, as the old
  "Set Unit and Scale" did, with one scale value per channel), and `Hz` / `%`
  (of Nyquist) / `None` for frequency.
- **Crosshair buttons take a value from the track instead of the keyboard.**
  Next to a time axis's **Offset**: the time the playhead should show, which
  sets the offset. Next to a time axis's **Scale**: the times the selection's
  two ends should show, which sets the offset *and* the scale (a selection is
  required; sample units have no scale factor, so only the offset follows). Next
  to an amplitude or frequency **Scale**: the app waits for a click in the
  waveform (amplitude) or on a spectrogram line (frequency) and then asks for the
  value that belongs at that point, which sets the scale — the flow the old
  Alternative Scale used. A reference at zero, or a selection whose ends show the
  same value, gives no scale and says so in the status bar.
- The **first visible Add Axis of each kind is the main axis**: it labels the
  grid that is actually drawn and drives the level readouts. The other Add Axis
  entries relabel the *same* tick positions in their own units, in strips placed
  further out — they never move the grid. **The first row of a column is always
  the axis closest to the track**: hugging the plot for an amplitude or frequency
  axis, and hugging the plot's edge for a time ruler whether it is above or
  below the track, with the extra strips on the far side. Each axis labels only
  its own strip (an extra axis used to be drawn on top of the main labels on the
  left side), a secondary axis uses the same smaller type as the extra time
  axes, and its strip is narrower (38 px against the main 60). A frequency axis is always shared by
  every channel row (the engine renders one frequency window per spectrogram).
  The form is modeless and stays on top on purpose: the tracks and the readouts
  update live while you move the playhead or click a reference.
- **Every axis of a track reserves the same space**: all three tracks share one
  axis-strip width (the widest label set wins) and one ruler stack height (the
  normal 24 px ruler plus a narrower 18 px strip per extra time axis, the main
  ruler always the one nearest the data). The horizontal time bar and the
  vertical scrollbars are inset by those same widths, so they span the data
  rather than the whole track, whatever the axes are. The channel label (L / R /
  1 2 3 4) sits at the outer edge of the axis area — left of the units when the
  axis is on the left, right of them when it is on the right — in a larger
  type, with a gutter reserved for it so it never collides with the labels. The three tracks also share one
  label size/colour and one grid colour, and the waveform's amplitude grid is
  drawn *behind* the waveforms instead of over them.
- **Footer Only** axes appear in the cursor probe as `| Name=value` (including
  the Frequency Analysis panel's readout, where each displayed frequency —
  snapped bin, band centre or tracked component — gets its own suffix), as extra
  fields in **Go to Time**, and as extra columns in the exported tables
  (`chN_Name` next to each channel's Hz in Freq/Amp-vs-Time curve modes, a
  shared `Name` column in Amp-vs-Freq and the classic/1/3-octave tables, all
  converted by exactly the same factor as the axis overlay). Every exported
  table uses **tab-separated columns** (Excel-friendly).
- Axis changes are **undoable**: each edit in the form is one step in the History
  tab (shown as "Scale"), and undo/redo restores the axis list. The axes are
  stored with the file (embedded SCMD chunk or `.meta.json` sidecar); a file
  that still carries the older calibration / alternative-reference state is
  migrated automatically when it is opened. Details: `docs/scale-form.md`.
- **Go to Time... (Ctrl+G)**: a small dialog with one field for the main time
  axis plus one field per visible Footer Only time axis. Type into any of them
  and OK jumps the playhead there — each field is parsed in its own units (plain
  seconds, sample counts, clock text, or its scale), and the fields are
  pre-filled with the current values at the playhead.
- While both views are stacked, exactly one of them draws the shared time
  ruler: the waveform by default (ruler at the top), or the spectrogram when
  **View > Time Ruler at Bottom** is set — the other view hides its strip and
  expands into that space. A lone view always draws its own ruler.

### Spectral selections (Selection menu)

Besides plain time ranges, you can select arbitrary regions of the
time/frequency plane and apply edits to exactly that content. The
**Selection** menu switches the drawing tool:

| Tool | Key | How to draw |
|---|---|---|
| Time Range (default) | S | Left-drag a time range, as before |
| Marquee | M | Left-drag a rectangle in either view's plot area |
| Lasso | L | Hold and drag through the vertices of the region; releasing closes the polygon (≥3 points) |
| Paintbrush | P | Drag a freehand stroke; its half-width is a percentage of each point's own frequency, so it tapers along the path and stays fine at low frequencies |

Modifier keys while dragging: **plain = replace** the current selection,
**Shift = add** to it, **Ctrl = subtract** from it (subtractive shapes are
drawn with dashed edges). With the paintbrush, **Alt+click** draws a straight
line from the last stroke's end point to the click position; the line is
straight in *linear-frequency* space, so on a logarithmic frequency axis it
appears slightly curved (switching the view to a linear scale shows it as a
perfectly straight line). Shift/Ctrl combine with Alt for add/subtract lines. The active tool and brush size are shown in the
status bar; while holding **Shift** or **Ctrl**, it additionally shows "Add Selection" / "Subtract Selection", so you can always see how the next drag will edit the selection. `[` / `]` step the brush size through 1/2/4/8/16/32 % of the cursor's own frequency (default 4 %), and **Esc**
clears the spectral selection without touching the time selection.

Releasing the drag commits the shape, mirrors its time extent into the normal
time selection (so time-based effects apply to exactly that stretch) and moves
the **playhead to the start** of the selection — like the time-selection tool,
pressing Space then plays from where the selected region begins. A drag too
small to define a shape only clears the selection and leaves the playhead where
it was.

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

**Split Spectral Selection** (Effects menu) goes one step further: instead of
just zeroing the band, it *moves* the band content from chosen source channels
into destination channels you configure per file open.
**Split Spectral Selection Configuration** (Shift+F12) opens a window with one checkbox row per channel and a destination-channel field for each checked row: an existing destination mixes the extracted content into that channel in place, while a number above the current count creates that channel on first use. The preset buttons fill the grid quickly — **All Tracks** (every current channel extracts into its own new track), **Last channel into new**, and **Second last into last**. Closing the window saves the configuration without performing any split. **Split Spectral Selection Execute** (F12) runs it as one undoable step; the first time after opening a file it opens the configuration window, and closing that window then executes the split on the current selection. Each FFT frame inside
the selection's time extent gets a trapezoidal vertical profile (linear ramps
over the bottom and top 25% of every contiguous selected band, full weight in
the middle); the extracted part is the band content with its amplitude reduced
by the per-frame noise floor (mean magnitude of the bins just outside the
bands), and the removed energy in the original channels is replaced by
synthetic noise at that same floor level with a deterministic pseudo-random
phase. The result: the new track(s) carry the isolated band, the originals keep
everything else plus an uncorrelated noise fill inside the bands, and their sum
reconstructs the pre-edit signal — no hard spectral edge to hear or see.
Time edges are handled the same way: FFT frames straddle both ends of the
selection (each window spans 2048 samples), so the processed extent extends
slightly beyond the drawn rectangle. Unmasked context frames are
overlap-added on both sides, giving every output sample full window
coverage — the extracted track fades in/out with the Hann taper at the
two ends of the extent and the original channels crossfade back to their
pre-edit content over those same regions, so no amplitude spike or hard
cut appears at either end of the selection. Everything from the end of the
extent to EOF stays bit-identical to the pre-edit content (no silent gap is
written after the selection).
The whole operation (content change *and* channel-count change) is a single
undoable step; undo restores the original channels bit-exact. New channels are
made visible automatically, and any undo/redo that changes the channel count
re-syncs track visibility to the file — reverting a mono→stereo split returns
to a single visible track (no phantom silent track), re-applying it shows all
tracks again.

The old one-shot variants are now presets of this configuration: **All Tracks**
reproduces the former *Split Spectral Selection (All Tracks)* behaviour (one new appended channel per source), while **Last channel into new** and **Second last into last** cover the common single-channel cases — including the old *Track 1→2* flow, which is just "second last into last" on a stereo file. Sources that are not ticked in the configuration are left completely untouched, and every configured split is again a single undoable step.

### Selection panel (named selections + harmonic cloning)

The **Selection** tab in the left panel manages *named* spectral selections
for the active file and provides **Clone Harmonics**, a tool for picking out
harmonics of whatever is currently selected.

- The table lists each saved selection with its **Name** and a free-text
  **Comment** (which fills the remaining width). Right-clicking a row offers
  **Edit...** — a dialog to change name and comment at once; comments travel
  with the file via [per-file metadata](#per-file-metadata).
- **Store / Recall / Delete** buttons below the table: Store saves the current
  selection under an auto name — "Selection 01", "Selection 02", ... skipping
  numbers already in use. In Time Range mode it stores the time range (as a
  full-height band) even when a recalled spectral selection is still live, so
  storing several different ranges keeps each one distinct; with a spectral
  tool active it stores the live spectral selection instead — falling back to
  the time range as a full-height band when there is none. Recall replaces the current selection
  with the first selected saved one and makes sure both views show its time
  range — panning or zooming out when it is not fully inside the current window.
  The list belongs to each file and is persisted automatically, so it survives
  restarts without any manual save.
- **Excel-style multi-selection**: plain click selects one row, Ctrl/Cmd+click
  toggles additional rows in or out, Shift+click range-selects from the anchor
  row. Any change of the selected set makes the union of those saved selections
  the *actual* live spectral selection (drawn like any other mask) and fits both
  views to its time range with the usual 10% margin; DEL deletes the audio data
  of that active selection, while Delete Selected removes every selected entry
  from the list.
- Right-clicking a row offers **Recall**, **Edit...**, **Delete Selected**,
  **Clone Harmonics by Numbers...**, **Clone Harmonics by Mouse** /
  **Stop Picking** and **Fit View to Selected**. Double-clicking
  a row recalls it the same way.
- **Clone Harmonics by Numbers...** opens a dialog for frequency multipliers
  such as `0.5, 2, 5` — additive clones of every shape in the current
  selection are appended at those multiples (the original stays in place),
  with each clone's thickness stretched by the same factor. This is exactly
  what you need to select harmonics: pick a fundamental band, then multiply by
  2, 3, 5...
- **Clone Harmonics by Mouse** switches to interactive mode: click a reference
  point in the spectrogram (shown as an orange circle), then click each target
  frequency — every click appends clones scaled by `clicked / reference` (e.g.
  reference 100 Hz plus clicks at 50/200/500 Hz gives multipliers 0.5, 2 and
  5). Picked points are drawn as circles on every channel row; clicking near
  an already-picked point removes it (and the clones its pick added), so a
  stray click can be undone without Esc. **Esc** or choosing *Stop Picking*
  ends the session without touching the selection.

### Drawing panel (annotations over the spectrogram)

The **Drawing** tab in the left panel lists user annotations drawn on top of
the spectrogram: **Text**, **Arrow**, **Rectangle** and **Free Drawing**.

- The table shows each item's type and a free-text **Comment**; right-clicking
  a row offers **Edit...** (text/comment, colour, line width, text size) and
  **Delete Selected**. Double-clicking a row fits it into view the same way as
  selecting it.
- The first column is an eye icon that toggles *that* drawing's visibility;
  the **Show** button at the top of the panel is the master switch for all of
  them. Hidden items stay in the list but are not drawn on the spectrogram.
- Above the table sit the defaults applied to *new* drawings: a **Colour**
  picker, a line-width slider and a text-size slider. The last-used values are
  remembered between sessions.
- **Excel-style multi-selection**: plain click selects one row, Ctrl/Cmd+click
  toggles rows in or out, Shift+click range-selects from the anchor. Any change
  of the selected set fits every selected drawing into view — both time and
  frequency axes get a 10% margin on each side (clamped to the file).
- New items are created from the panel's right-click menu (**New Text / New
  Arrow / New Rectangle / New Free Drawing**) or with **T / A / R / D**: the
  status bar shows "New drawing: click / drag in the spectrogram", a single
  click places text (a dialog asks for its content) and a drag draws an arrow,
  rectangle or freehand stroke. **Esc** cancels a pending creation.
- Drawings are stored in data coordinates (time seconds × frequency Hz), so they
  stay anchored to the spectrum under zooming and panning, and are drawn on every
  visible channel row like spectral selection shapes. They persist with the file
  via [per-file metadata](#per-file-metadata) — embedded in the WAV's `SCMD`
  chunk or in the `.meta.json` sidecar, exactly like selections.

### Tables panel (CSV data tables)

The **Tables** tab in the left panel manages any number of imported CSV data
tables. Each table holds one or more parameters (columns), shown as an indented
list: a table row followed by its parameter rows.

- Every row has an eye icon that toggles *that* table's/parameter's
  visibility. Table rows carry no colour of their own; each **parameter** gets
  a unique palette colour when it is imported (colours stay distinct across
  all tables in the document). A single click on a parameter's colour square
  cycles it through the palette (the cycle is deferred until JUCE's
  double-click window has passed, so a double-click never changes the colour);
  **double-clicking opens the colour picker** — the built-in twelve swatches
  plus any user-defined ones. In the dialog you can add custom colours with
  **New**, edit their RGB values in place, remove one with **Delete**, restore
  just the defaults with **Reset**, and apply or discard everything with
  **OK/Cancel**. Custom colours persist between sessions. The **Show** button
  at the top of the panel is the master switch for everything.
- **Shift+clicking an eye *solos* it**: a parameter's eye shows only that
  parameter (and its parent table), a table's eye shows only that table with
  all of its parameters, and shift-clicking the master **Show** button makes
  everything visible. Shift-clicking again while in any of those states turns
  *all* visibility off.
- Tables are created by **dragging CSV files onto the window** (the table name
  comes from the file name) or via right-click → **Paste to new table** (named
  *Clipboard*; parameter names come from the first row's column titles). A
  CSV of 2 MB or more imports in the background behind a progress dialog
  (**Cancel** or Esc stops it, nothing is added); several dropped files
  import one after the other.
- Right-clicking a selection offers **Remove Selected**, **Copy** (CSV with a
  header row of parameter names), **Rename Table...** / **Rename Parameter...**
  (prompts for the new name of the item under the mouse), **Down sample...**
  (prompts for a new rate in Hz, default 10 — rows are *removed only*, keeping
  every Nth row so the density is at most that rate; no interpolation and no
  added data). Excel-style multi-selection applies: plain
  click, Ctrl/Cmd+click toggle, Shift+click range.
- The dropdown at the top picks where visible parameters are drawn. Switching
  it re-targets every parameter to the newly chosen view, so a table you just
  imported shows up in the spectrogram overlay immediately - no need to open
  each parameter's Align → **Manual…** dialog. (You can still give a parameter
  its own target afterwards from the Align submenu; the dedicated track shows
  every visible parameter whatever the target.)
  - **Overlay on Waveform** — parameters whose alignment target is *Waveform*
    are drawn over the waveform view. Each parameter's raw range is first
    fitted into [-1,+1] (so the curve is visible regardless of the data's
    units), then its Inverted/Absolute/offset/scale settings are applied and
    the result lands in amplitude units. With several channels visible, every
    channel row gets its own copy of the same curves scaled to that row's
    centre and height;
  - **Overlay on Spectrogram** — same for parameters targeted at *Spectrogram*:
    the fitted [-1,+1] value is mapped linearly onto the view's current
    frequency range and drawn through its own frequency axis, once per visible
    channel row (each row maps the curve across its own frequency span);
  - **Dedicated track** — a resizable strip above/below the data views (next
    to the time ruler) shows every visible parameter auto-scaled on the same
    dark background as the waveform/spectral tracks, with its own vertical
    zoom/pan scrollbar (fully two-way: dragging the thumb pans or zooms the
    track's value window, and wheel-zooming inside the track moves the thumb),
    the blue time-selection band and the red playhead over it. Hovering near a curve lists each parameter's *unscaled* value at that
    time. The strip is separated from the data views by a draggable splitter
    (like the waveform/spectrogram one): dragging it up or down resizes the
    track.
    - **Left-drag on the value axis** pans vertically: the content follows the
      cursor at half speed, an edge clamp never leaves a dead zone and
      reversing responds at once (like the waveform/spectrogram axes); left-drag
      in the data area makes a time selection. **Middle-drag in the data area**
      pans *both* axes at once — the shared time window horizontally and the
      value window vertically, both at half cursor speed with the same
      incremental scheme. The **wheel over the value axis**
      zooms vertically around the cursor, while the **wheel anywhere else in
      the strip** zooms *horizontally* around the cursor (the shared time
      window — exactly like the other tracks). Axis labels and hover values
      drop their decimals as the magnitude grows (4 below 1, none at 1000+).
    - The panel's preview plot draws a grid like the Frequency Analysis graph
      and shows a delicate white vertical cursor line while hovering.
    - **Right-drag from the data area** box-zooms both axes at once: a
      translucent blue rubber band is drawn while dragging, and on release the
      dragged span becomes the new time window *and* value range. **Right-drag
      from the value axis** zooms vertically only (the band stays inside the
      strip).
    - A plain **right-click** opens the track's context menu: **Zoom to
      Selection**, **Zoom Out Full** (time + value on the data area, vertical
      only on the axis), **Select All** and **Copy Curve Data** — an
      Excel-friendly tab/CR grid with one column per visible parameter; with a
      time selection active only the samples inside it are copied. With a time
      selection active the menu also has an **Edit** submenu:
      - **Overwrite Values...** prompts for start and stop values (a checked
        *Constant Value* box disables the stop field and mirrors the start one)
        and fills the target parameters' cells inside the selection with a
        linear ramp. The defaults are the first/last existing values of the
        first target parameter inside the selection. If the selection extends
        beyond the table's row grid, every parameter is extended: new cells
        outside the selection stay empty, and a front extension shifts all
        parameters' offsets so their curves keep their document times.
      - **Delete Values** empties the target parameters' cells inside the
        selection; rows that become entirely empty at the very beginning or end
        of the table are trimmed away (a front trim shifts every parameter's
        offset so the remaining curves keep their document times). Interior
        gaps stay.
      - Both actions operate on the parameters selected in the Tables panel
        (a selected table row means all its parameters) and fall back to every
        visible parameter when nothing is selected there; each records one undo
        step.
    - Empty cells are drawn as a gap in the curve, never as zero: they do not
      affect the auto-scaled axes, copy as blank fields, survive warps and
      downsampling as gaps and round-trip through save/reload (stored as JSON
      `null`).
    - **Ctrl+0 / Zoom Out Full** also vertically fits this strip when it is
      shown.
- The panel's preview plot shows hover values too: moving the mouse over it
  lists every visible parameter of the focused table at that table time, in
  each one's colour (unscaled values).
- The preview plot draws the **playhead** as a red line at the matching table
  time (through the table's alignment, and the selected parameter's own when
  a parameter row is selected); it follows playback and every seek, and is
  hidden while the playhead lies outside the table. **Clicking** in the plot
  moves the playhead to that time. A **left-drag** past the usual 4-pixel
  threshold instead **selects that time range** in the document (mapped back
  through the same alignment the curves use), like the waveform and the
  dedicated track; the playhead stays where the press put it. While a 1/2-point
  alignment is waiting for its plot clicks its gestures keep scrubbing as
  before, so the alignment is not disturbed.
- The preview plot can be panned and zoomed independently of the document's
  view: **middle-drag** grabs both axes (table time horizontally, value range
  vertically) at half cursor speed with the same incremental scheme as the
  other views — reversing responds at once — the **wheel** zooms the time axis
  around the cursor and **Shift + wheel** the value axis, and a **double-click**
  puts the whole table back with the auto-fitted value range. The windows never
  grow past the data, so the pan always has something to move.
- The preview plot also draws the **current time selection** as a translucent
  blue band with light-blue edges — the same band the waveform, spectral and
  dedicated-track views show — mapped through the very same alignment as its
  curves and the playhead line.
- A **draggable separator** between the table list and the preview plot
  trades height between them; the plot height is remembered between sessions.
  Same bar as everywhere else in the app (6 px, three-line grip).
- **Align** (parameter row): Waveform / Spectrogram / Dedicated submenus with
  Manual / 1 Point / 2 Points options. *Manual* opens a dialog whose fields are
  described by explanatory labels above them: the time offset/scale of the
  parameter's *effective* map, plus value offset/scale and **Normalized**,
  **Inverted**, **Absolute** transforms applied before them. 1/2 Points arm
  click-picking — first on the panel's preview plot, then on the waveform/
  spectrogram (two points also stretch the table's time axis).
- Time alignment is **shared at the table level**: aligning any parameter of a
  table re-aligns *every* parameter of that table to the same map (the
  per-parameter time fields are zeroed and the table carries the offset/scale).
  Only the value transforms and the *alignment target* stay per-parameter:
  completing an alignment records the chosen view as the parameter's target —
  a Waveform-targeted parameter is drawn by the waveform overlay, a
  Spectrogram-targeted one by the spectrogram overlay, and Dedicated-targeted
  parameters only appear in the dedicated track (which always shows every
  visible parameter). Auto Align adds the correlated lag to the aligned
  parameter's own time offset (both curves share the table map, so only that
  can separate them).
- With two parameters from different tables selected, right-clicking shows
  **Auto Align <table> <param> with <table> <param>**: a normalized
  cross-correlation in *document* time (each curve resampled through its
  effective offset/scale onto the union of both ranges) finds the delay
  between the curves and shifts the parameter under the mouse onto the other
  one. Because the correlation runs in doc time, re-running Auto Align after
  applying it is a no-op instead of shifting again.
- Import rules: both **comma- and tab-separated** data are accepted; `true`/
  `false` cells become `1.0`/`0.0`; if the first column is entirely text the
  grid is transposed before import (rows become parameters); scattered missing
  numeric values are interpolated from their neighbours; columns that contain
  only text are ignored.
- **Time-column auto-detection**: when the first column's header contains
  *time* and its values are strictly increasing numbers, it is treated as a
  timestamp axis instead of a parameter — no parameter is created for it, the
  remaining columns keep their rows at those exact timestamps (the table's row
  rate is fitted to `(rows-1)/span`), and if the first timestamp lies beyond
  the audio duration the whole span is shifted so the table starts at 0.
- Tables **follow timeline edits**: deleting, inserting or stretching audio
  (including Content Aware Time Scale) warps every table's rows — each row
  stays anchored to its document time, values are re-interpolated through the
  inverse warp and the row count adjusts so the rows-per-second density is
  preserved.
- Like drawings and selections, tables persist with the file via
  [per-file metadata](#per-file-metadata) — names, colours, visibility,
  sample rates and every alignment/transform setting round-trip through the
  `SCMD` chunk or `.meta.json` sidecar.

### Actions panel (record / replay / batch)

The **Actions** tab in the left column works like Photoshop's Actions palette:

- **New** creates an empty action; **Record** starts capturing edits into the
  selected action (or prompts for a new name when nothing is selected). While
  recording, every supported edit you perform — menu effects, dialog-based
  effects, generators, resample, transcribe, noise suppression, Python scripts,
  delete/trim and the immediate tools — appends one *step* to the action.
- **Run** replays the selected action on the current document. Steps marked
  *prompt* (right-click a step → Prompt) re-open their normal dialog so you can
  confirm or change parameters before each step runs; un-prompted steps run with
  the recorded values. Ctrl+Shift+P runs the selected action from anywhere.
- **Batch...** opens a batch dialog: pick an input folder, an output folder
  (or tick *Same output folder* to write next to the sources), optionally scan
  subfolders, choose the file formats (`*.wav; *.mp3`) and a rename pattern
  (`%Filename%.wav`). Each matching file is processed headlessly in its own
  isolated session — your open documents and settings are never touched.
- Right-click an action for **Rename**, **Edit** (a JSON text editor with
  validation — the step list is plain data, so you can hand-write actions) and
  **Remove Selected**. Multi-select works; Remove applies to every selection.

Actions persist in the recent store (`actions_json`) across restarts. The same
engine is available headlessly: `--run-action <name> <in.wav> <out.wav>` runs one
file, `--batch <name> <inFolder> <outFolder> [formats] [rename] [--subfolders]`
runs a whole tree (see **Running & CLI flags**).

### Per-file metadata (File → Metadata)

Everything that belongs to a file — **markers**, **user drawings**,
**data tables**, **named spectral selections**,
the **scale calibration** (Set Unit and Scale), the **axis calibrations**
(custom frequency units + alternative scales) and the **Frequency Analysis**
settings (axis mode, FFT size, Ref Time, Smooth, Track) — is stored together
in one bundle that travels with the file in whichever of two formats it already
uses:

- **Embedded into Wav** — inside the WAV as RIFF chunks: `cue` + LIST/`adtl`
  (the classic DAW marker layout), a custom top-level `SCMD` chunk carrying the
  rest of the bundle as compact JSON, and the transcription in its own `SCTT`
  chunk. Non-WAV formats cannot carry these chunks.
- **External Json file** — a sibling `<file>.meta.json` holding the same bundle
  (readable layout) plus the transcription; the WAV itself stays untouched.

Opening a file picks the format automatically: an existing `.meta.json` wins,
otherwise managed RIFF chunks inside the WAV are detected, and files with
neither default to **Embedded** for `.wav` and **External** for everything else.
A file keeps whichever format it already uses — nothing is migrated silently.

**File → Metadata** switches the active file between the two modes on demand:
switching to External writes the sidecar and surgically removes every managed
chunk from the WAV; switching back re-embeds all chunks (audio bytes are
preserved byte-for-byte in both directions) and deletes the sidecar. Legacy
sidecars (`<file>.markers.json`, `<file>.selections.json`) are removed when a
file is switched, since their contents now live in the unified store. Saving a
non-WAV file while it is in Embedded mode automatically falls back to External.

Because saving and loading handle the bundle transparently, there is no manual
"save markers / save selections" step anymore: **File → Save** (or Save As) is
the only persistence point, and reopening a file restores everything —
including the Frequency Analysis settings that were in effect when it was last
saved. A freshly opened file with no stored state inherits the session's
current display settings instead of resetting to defaults.

**File → Metadata → Save Metadata** writes just the metadata to disk in
its current storage mode without touching the audio data: embedded mode
surgically patches only the managed RIFF chunks inside the WAV, external mode
rewrites the `.meta.json` sidecar. Useful after editing markers/selections/
calibrations when you don't want to rewrite a large audio file.

Every document tracks whether its non-audio state differs from what is on disk
(`FileDoc::metaDirty`, set by marker edits, storing/deleting/importing named
selections, transcription writes, scale/axis calibration changes and undo/redo
of such states). The title bar shows `*` for unsaved audio edits *and* dirty
metadata. **Quit** (Ctrl+Q or closing the window) asks "You have unsaved files…
Do you want to quit?" when any document has unsaved work, and **File → Close
All** offers Close all / Close unmodified only / Cancel when some documents are
dirty (immediate close-all otherwise).

---

## Architecture

Clean separation into independent components (see `include/` + `src/`):

| Component | Responsibility |
|---|---|
| `AudioEngine` (`juce::AudioSource`) | File I/O via AudioFormatManager, streaming vs in-memory decision, sample access API, mipmap pyramid for the waveform overview, playback transport (play/pause/loop), edit primitives (`applyPatch`, silence/fade/normalize/reverse/cut/copy/paste/delete) plus `initNew` (empty or pre-filled documents with no file path yet), `readRange`, `pasteMixAt` (sum-in-place paste that appends past end-of-file) and `writeDataToNewChannels` (writes data into new channels at a sample position, extending the document with silence when it runs past the end — used by *Copy Audio to New Channel(s)*), plus a **solo playback overlay** (`setSoloAudio`: while active, the output is replaced by a decoded video track's audio played from its offset) and recording append/overwrite (`appendSamples` / `overwriteSamples`) with a monotonic content version views compare to invalidate cached renders. All disk reads are mutex-protected; the UI thread never blocks on I/O. |
| `SpectralEngine` | Tiled multithreaded FFT: level selection from zoom, tile scheduling on a `juce::ThreadPool`, LRU cache keyed by (tile, channel, level, fftSize, hop, window). Tiles store **raw dB magnitudes**; gain/bias/dB-range/colour-map scaling is applied per pixel at render time, so changing those settings never requires recomputing tiles. Bilinear sampling into RGBA pixels, colour-map LUTs, headless PNG rendering. Never materializes more than one tile per channel at a time — multi-GB files stream through the reader tile by tile. |
| `WaveformView` / `SpectralView` (`juce::Component`) | Pure drawing + mouse interaction; hold references to their engines and repaint on change messages. Each view holds its own time-window state and its **vertical axis/zoom level** (amplitude for the waveform, frequency range for the spectral view); at application level all horizontal changes flow through `MainWindow::applyTimeWindow`, which keeps both views on the same time range while vertical zooms stay independent. The spectral view requests tiles for its visible range only. Selection, markers and playhead are drawn as overlays *on top of* the audio in both views, so they behave identically wherever you look. |
| `ScrollBars` (`TimeScrollBar`, `ValueScrollBar`) | Custom scrollbar components: thumb shows the displayed portion relative to the whole track; centre-drag pans, edge-drag zooms one side, click outside jumps. One horizontal time bar plus one vertical bar per view. |
| `Panels.cpp`: `FFTAnalysisView`, `SpectralControlsPanel`, `MarkerListPanel`, `SelectionPanel`, `DrawingPanel`, `TablesPanel`, `PythonPanel` | Classic DAW-style left column (no document list any more: that is **File > Loaded Files**), live FFT analysis with cursor readout (drawn in frequency space — never indexes the magnitude vector by pixel x), a selectable axis mode (FFT Linear / FFT Log / 1/3 Octave band plot, plus Freq vs Time / Amp vs Time / Amp-vs-Freq curve modes with Ref Time / Smooth / Track options and their own per-mode snapshot store) and harmonic reference lines (right-click **Activate Harmonics** / **Copy Harmonics** / **Clear Harmonics**), spectral settings (colour map/dB/Log-Linear toggle/Reset Freq/FFT size), the marker table, named spectral selections with comments + harmonic cloning, user drawings over the spectrogram (text/arrow/rectangle/freehand with per-item comments, visibility toggles and fit-to-view; Excel-style multi-selection in all three list panels; persisted automatically with per-file metadata), imported CSV **data tables** (`TablesPanel`: indented table/parameter rows with visibility eyes; parameters carry unique palette colours — click cycles, double-click opens the preset picker — plus CSV drag-and-drop / clipboard import with transpose & gap-interpolation rules, downsample, copy, overlay-on-waveform/spectrogram or dedicated-track display modes, and manual/click-picked/correlation-based time alignment per table and per parameter), and the **Python** script editor (dropdown + Add/Remove/Run over a multi-line editor). Panels mutate shared state through `Application` and notify `MainWindow` via `app.onStateChange`. |
| `PythonScripts` | Python panel support: per-user script folder management (`%APPDATA%/SpectroCraft/python`, auto-created with six example scripts), interpreter probing on the PATH, metadata JSON building, float32 WAV temp-file I/O and cross-platform child-process launching (CreateProcess/fork) with cancellation. `Application::runPythonScript` exchanges data with the script through temp files and commits the result via the normal undoable edit path. |
| `MarkerManager` | Point/region markers with title + comment, sorting, find-near; persisted via per-file metadata (embedded RIFF chunks or `.meta.json`). |
| WAV marker writer (`AudioEngine::saveWavWithMarkers`) | Custom RIFF writer for `.wav` saves: `fmt` (IEEE float32), `cue` (one entry per marker at its start sample), `LIST`/`adtl` with `labl` (title) + `ltxt` (comment) entries, then the raw interleaved `data`. All chunk sizes are computed up front; streaming files are written in 1 MB blocks. |
| `UndoManager` / `UndoPatch` | 50-level undo/redo stacks of compact patches (old/new sample data + length delta) instead of full snapshots. |
| `FileDoc` | One per loaded file: its own `AudioEngine`, `MarkerManager` and `UndoManager`, plus the remembered playhead position, selection range, visible-channel list, scale calibration (`ScaleCalibration`, including optional per-channel full-scale values), saved spectral selections, data tables (with their alignment/transform state), axis calibrations (`AltRefState`), Frequency Analysis settings (`FreqAnalysisState`) and the metadata storage mode. Also keeps the raw `bkdk` payload of the source file (when present) so WAV saves preserve it byte-for-byte. Created by `Application::openFile` (from disk) or `Application::newFile` / `copyToNewFile` (path-less documents); destroyed on close (the audio player is detached from its engine *before* destruction). |
| `BkdkChunk` (`src/BkdkChunk.cpp`) | Parser for the custom `bkdk` RIFF chunk found in some legacy measurement WAV files: NUL-separated ASCII fields, one 11-field group per channel whose Calibration field (sample × gain = pascals) drives the automatic Pascal calibration on load. A single defined group applies to every channel; missing/unparseable values fall back to no scaling. |
| `Tables` (`src/Tables.cpp`) | Data-table model + CSV import (comma/tab separated, quoted fields, CRLF, boolean cells, first-column-text transpose, time-column auto-detection with duration-based start shift, scattered-gap interpolation, text-only column drop), remove-only downsample decimation, value transforms (normalized/inverted/absolute + offset/scale), composed table+parameter time maps, document-time normalized cross-correlation auto-align lag (idempotent), and the JSON serialise/parse used by `FileMeta`. |
| `TablesTrackView` (`src/TablesTrack.cpp`) | The dedicated-tables strip: draws every visible table parameter auto-scaled into its own plot area (same width as the data views, same left-axis width), with vertical zoom/pan driven by a `ValueScrollBar`, the blue time-selection band and red playhead over it, vertical yellow marker delimiter lines (no bracket line or titles on this strip), and hover readout of each curve's unscaled value at the cursor time. |
| `FileMeta` (`src/FileMeta.cpp`) | Unified per-file metadata bundle: serialises/parses the JSON that holds markers, saved selections, data tables, scale calibration, axis calibrations and Frequency Analysis settings; scans a WAV for managed RIFF chunks (`cue`, LIST/`adtl`, `SCTT`, `SCMD`). The transcription always travels in its own `SCTT` chunk (see `ScttChunk`). Also provides `migrateFileMetadata` (behind the CLI `--meta-mode` option): moves a WAV's whole metadata bundle between embedded chunks and `.meta.json` without opening it in the editor, reading each category from whichever source currently holds it. |
| `MergeFiles` (`src/MergeFiles.cpp`) | Multi-file merge engine behind Shift + drag & drop: reads every source at its native rate in streaming chunks (+ markers from sidecar/embedded/cue), resamples all sources to the highest rate with SoXR, computes the sequence/parallel layout and builds the merged audio buffer (silence padding on missing channels) plus the merged marker list. `Application::mergeFilesIntoActiveDoc` applies a result to the active document (undoable append for same-rate sequence, whole-file replacement otherwise); `Application::createDocFromMerged` activates a fresh document from a standalone merge result. |
| `VideoAudio` (`src/VideoAudio.cpp`, `src/VideoAudio_mac.mm`) | Video-container audio extraction (Windows + FFmpeg; macOS through an `AVAssetReader` reader that continues sequential reads and restarts at the requested time otherwise): a `juce::AudioFormatReader` over libavformat/libavcodec/libswresample that opens the first audio stream of .mp4/.mkv/.avi/etc., decodes it to float and positions reads sample-accurately via frame timestamps (seek-per-read, zero-fill beyond EOF). Registered after JUCE's basic formats so native files keep their usual readers; video-only containers are rejected. |
| `VideoFrames` (`src/VideoFrames.cpp`, `src/VideoFrames_mac.mm`, `src/VideoFrames_win.cpp`) | Video picture decoding for the Video window: a per-platform frame stream (AVFoundation on macOS, Media Foundation on Windows, none on Linux), `VideoDecoder::frameAt(t)` (decode forward for small forward moves, seek otherwise; skipped frames are never converted) and `VideoFramePump`, a worker thread that serves only the latest requested time and hands frames to the message thread. |
| `VideoWindow` (`src/VideoWindow.cpp`) | The separate Video window: letterboxed picture with the track rotation, frame time, audio/video length notice; zoom/pan (wheel about the cursor, right-drag region box, double-click fit, vertical + horizontal zoom bars, left- or middle-drag pan one to one — the picture follows the pointer from the previous cursor position, so an edge never leaves a dead zone and reversing responds at once), per-file rotation, *Upscaling* (Lanczos / Bicubic / Nearest, see `VideoScaler`). Docks as the Video panel of the docking workspace (`VideoDockHost`, picture on top and tracks panel below, draggable separator between them, corner button collapses the tracks area; floating or in the main window) via **Window > Video** (opens the Video panel) or *Dock/Undock Panel* from the tab menu once docked/floating (*Always on Top* in a floating group's tab menu), and can be sent back to its own window. Below the picture, `VideoTracksPanel` shows one band per video track of the document (green ruler band = main audio length): drag left/right to offset (Shift-drag snaps to 0 s / audio end / other track edges) or up/down to reorder the tracks (axis picked by the first pixels; the top row renders above), wheel zoom about the cursor, right/bottom scrollbars when content overflows, click a band to select which video renders above (auto-switching to the topmost covering track when the selected one ends); clicking the time ruler seeks the main playhead and dragging it selects that time range (a drag past the usual 4-pixel threshold, like the other time axes); the playhead is drawn as a red line from the ruler to the bottom of the panel, with the shared time selection painted over the ruler and rows as the usual blue band; right-click menu on a band — or on the picture, targeting the currently displayed track — for sync/align/copy-audio/adjust-image. Per-track zoom/pan/rotation/contrast/brightness and offsets live in `FileDoc::videoTracks` (`sc::VideoTrackData`, serialised under `videoTracks` in the document metadata). Polls the document time from `MainWindow::videoTimeNow()` (playback position, else playhead) at 60 Hz while visible or docked. |
| `VideoScaler` (`src/VideoScaler.cpp`, `src/videoscale/VideoScalerCore.cpp`) | Picture scaling for the Video window: the visible part of the frame resampled to physical screen pixels with Lanczos (a = 3), Bicubic (A = −0.6) or Nearest — swscale's kernels, stretched when shrinking, weights normalised — separable, weights cached per geometry, rows split across up to 3 worker threads, 4-wide SIMD (SSE2 / NEON), quarter turns folded into the pixel reads. The core is plain C++ in its own library (`sc_video_scaler_core`), compiled optimised in every configuration, Debug included. |
| `SingleInstance` (`src/SingleInstance.cpp`) | Single-instance coordination behind the Window > **Allow multiple instances** toggle: reads/persists the setting straight from disk at startup, detects a running instance via a named mutex (Windows), and forwards command-line files to it over a small named-pipe server that every running instance hosts (length-prefixed UTF-8 paths + acknowledgement byte). The second instance quits after the hand-off; if no instance answers it opens the files itself. |
| `Application` | Owns the document list (`std::vector<std::unique_ptr<FileDoc>>`) and the active-document index; exposes the active doc's engine/markers/undo through accessors (empty fallback objects while no file is loaded); handles open/activate/close/cycle, keeps the mirrored UI state (selection/playhead/channels) in sync with the active document, wires edit actions to undo. Also owns the app-level clipboard (shared across files), and implements `newFile`, `copyToNewFile` (visible-channel-limited, spectral-selection-masked), `pasteMix`, the merge entry points (`mergeFilesIntoActiveDoc`, `createDocFromMerged`) and metadata-only persistence (`saveActiveDocMeta`) plus the unsaved-state queries (`docIsUnsaved`, `unsavedDocNames`) that back the quit / Close All prompts. |
| `Docking` (`src/Docking.cpp`: `DockManager`, `DockArea`, `GroupComponent`, `FloatingWindow`, `Workspaces`, `WorkspaceBar`, `DockLookAndFeel`) | Adobe-style docking workspace, independent of the rest of the app: a tree of horizontal / vertical splits of *panel groups* per dock area (the main window's and one per floating window); panels are registered with an id and a content component they do not own. Drag sessions (translucent ghost window snapping onto the hovered zone, Esc / Ctrl handling), the trapezoid drop overlay, tab strips with the ≡ panel menu and >> overflow, gap resizing, maximise, floating windows with magnetic moves, the memory of where closed / undocked panels go back, the JSON layout (`saveLayout` / `loadLayout`) and named workspaces with saved + current layouts. Components a drop removes are hidden at once and deleted asynchronously, so a group may be retired from inside its own mouse handler. `DockLookAndFeel` (the app's default look and feel) embeds Source Sans 3. |
| `Diagnostics` (`src/Diagnostics.cpp`, `sc::diag`) | Opt-in diagnostic log (docs/diagnostics.md): `event()` posts into a bounded queue (one atomic load when off); a writer thread cleans, collapses, rate-limits and writes JSON Lines with size rotation; a flight recorder of debug events written after an error; a freeze watchdog thread fed by a message-thread heartbeat; per-minute aggregates (spectrogram tiles, audio load, memory); crash reports from signal / terminate handlers (Windows: `CrashTrace.h`); summary, tail and support bundle for *Help* and the `diag.*` commands. Hooks: `sc::alert` observer, the AI control journal, `FileDoc::load`, `AudioEngine::saveFile`, `SpectralEngine::getTiles`, the views' `paint()`, `WorkProgressDialog`. |
| `MainWindow` (`DocumentWindow` + `MenuBarModel` + `Timer` + `FileDragAndDropTarget`) | Docking workspace host (menu bar with the workspace switcher on its right, the main dock area, status bar); the **Editor** panel holds the waveform/spectral views + scrollbars + dedicated tables track (`layoutEditor()`), one tab per open document (`syncEditorTabs()`: the permanent `editor` panel plus an `editor-<docId>` panel per other document, all sharing the views, which move into the active document's tab); registers every panel, the built-in workspaces and the migration of the old two-group column; menus, keyboard shortcuts (incl. Space play/pause, Home/End, F8 marker), drag & drop (plain = open each file; **Shift** = merge menu with sequence/parallel options into the current or a new file), playhead animation timer and view-range state sync between views/panels. |

Threading model: the UI thread only draws and dispatches; FFT tiles are computed
on a pool sized to hardware threads; file reads happen on the reader under a
mutex with bounded copies. Tile results are published through an LRU cache guarded
by a single mutex (short critical sections, no lock held during FFT work).

Audio device initialisation is lazy: creating the `AudioDeviceManager` happens at
startup (cheap), but the expensive WASAPI enumeration/open runs on a worker
thread kicked off right after the window appears (`Application::startAudioInitAsync`).
The first playback request waits for completion (`ensureAudioReady`) and shows a
short "Preparing audio" dialog if it is still running, so a slow audio stack never
delays the window from appearing.

---

## Tests (`tests/`)

- `UnitTests.cpp` — lightweight CHECK-based suite (~4024 checks in Release), run via
  `SpectroCraftTests.exe` or `SpectroCraft.exe --run-unit-tests`:
  - **FFT**: peak bin of a known sine matches the expected frequency; buffer
    layout and normalization verified.
  - **Per-file metadata**: bundle JSON round-trip (markers with colours,
    rect/polygon selections, data tables, scale calibration, axis calibrations,
    Frequency Analysis settings, transcription); RIFF chunk scanner + surgical patching
    (add/replace/remove while preserving every other byte of the audio);
    embedded save/load round-trip through `FileDoc` — including named spectral
    selections surviving an embedded WAV save/reopen; switching between
    Embedded and External modes (sidecar written / chunks removed and back
    again); **Save Metadata** in both modes (embedded: audio bytes
    byte-identical afterwards, marker persisted; external: sidecar updated);
    `migrateFileMetadata` round-trip (chunks → sidecar → chunks with the data
    intact at every step); the `metaDirty` lifecycle that backs the quit /
    Close All prompts (fresh docs clean, edits dirty, saves clean) and the
    unsaved-doc name listing; fresh files inheriting the session's display
    state; non-WAV files defaulting to external mode.
  - **Markers**: ids, sorting, rename/move/remove, find-near; region markers
    spanning a selection vs point markers at cursor; per-channel visibility
    toggling (arbitrary subsets).
  - **Panel multi-selection** (Markers + Selection tabs): plain / Ctrl+click /
    Shift+click row semantics with anchor tracking; any change of the selected
    set becomes the time selection (markers) or the live spectral selection
    (Selection tab, union of the stored shapes — verified to be drawn as a real
    mask, never an amber overlay) and fits both views to it with the usual 10%
    margin; bulk Delete Selected removes every selected row without touching
    audio data; DEL deletes the audio data of the active selection (spectral
    band zeroed / time range trimmed out, marker rows untouched); Edit... dialog
    commit path for name/comment and the "Selection NN" auto-name pre-fill.
  - **Drawing panel**: bundle round-trip for all four drawing kinds (hidden flag
    and point lists included), sidecar load + SCMD payload, settings persistence
    (width/colour/text size), `ensureDrawingVisible` margin/pan/zoom/clamp math,
    headless DrawingPanel row operations (eye toggle, master switch, Excel-style
    selection with fit margins, Delete Selected) and SpectralView pending-creation
    simulation (text click data coordinates, arrow drag commit, sub-3px discard,
    Esc cancel).
  - **Saved selection comments**: optional comment field round-trips through
    both the legacy JSON format and the unified metadata bundle; empty
    comments are not written.
  - **Store in Time Range mode after a recall**: storing several different time
    ranges keeps each one distinct even when clicking/recalling a saved row has
    installed its shapes into the live spectral selection (the old behaviour
    re-saved the first range over and over); with a spectral tool active the
    live selection is still what gets stored.
  - **bkdk calibration**: payload parsing (multi-group, single-group, malformed,
    trailing padding, unparseable Calibration → no scaling); loading a WAV with
    a `bkdk` chunk auto-calibrates to Pascal per channel (single group applies
    to all channels; it overrides any stored scale) and saving preserves the
    raw chunk byte-for-byte while files without one gain none; per-channel
    full-scale storage/collapse in the Set Unit and Scale dialog commit path;
    `ScaleCalibration` per-channel helpers + level labels.
  - **Undo/redo**: silence → undo restores samples → redo re-silences; stack
    depth; fade-in undo regression (per-sample edits must capture old data
    before the in-place modification, or undo is a no-op).
  - **Split spectral selection**: processor-level two-tone file — the extracted
    channel carries the selected band and nothing of the out-of-band tone,
    the residual keeps the out-of-band tone and loses the band, and their sum
    reconstructs the original; with a strong in-window noise floor the weak
    in-band tone is fully swallowed by the floor subtraction while the residual
    gains the synthetic noise fill. App-level: menu enablement (Effects item
    appears only with an active spectral selection), one new channel per visible
    source made visible, and undo/redo restoring the pre-edit file bit-exact.
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
  - **dB-SPL 1/3-octave pipeline (Pascal files)**: a full-scale bin-exact tone
    reads ~-3 dB in the view's relative space, which renders as ~91 dB-SPL —
    matching the reference-analyser computation; neighbouring bands stay far
    below; and switching the octave order from 40 to 1 lowers an edge-of-band
    reading by more than 1.5 dB (the Tustin-corrected Butterworth response).
  - **Change Gain**: +6 dB scales the selected samples by exactly 10^(dB/20),
    leaves everything outside the selection untouched, and undo restores the
    original level.
  - **Quick gain (+ / - keys, `Application::boostSelectionGain`)**: the applied
    gain is 0 dB at both selection edges, reaches the requested value at 25% of
    the range, holds through the middle (measured peak-by-peak on audio), ramps
    with a raised cosine rather than a straight line, and a 1 dB step gains
    exactly 1 dB; outside the selection nothing changes, a spectral selection
    boosts only the masked band (the out-of-band tone is untouched), no selection
    means no edit, and undo restores the original audio.
  - **Sample-level waveform**: which zoom uses the sample polyline, the exact
    per-column read, or the min/max pyramid; per-sample dots appear only below
    60 visible samples; a click hits the sample whose dot it lands on; the drag
    previews the new amplitude while the file is untouched and writes it on
    release (neighbours unchanged); undo restores the sample.
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
  - **Curve-mode options & zooming**: Amp vs Freq hides the FFT size / window
    type row and always uses fixed 2048-point frames (bin count independent of
    the panel's FFT setting); Smooth offers None/3/5/7/9/11/21/51 with a
    persisted value surviving a resync; Track's Weighted option matches Total
    exactly; time-based modes zoom horizontally via wheel, bottom-axis left-drag
    pan, right-drag box and Zoom Out Full (all clamped to the analysed region);
    Copy exports only the points inside the visible window in both time-based
    and Amp vs Freq modes.
  - **Check Harmonic Ratio + Clear All**: with a stereo file whose channels are
    exactly one octave apart, entering the mode picks the lower channel as the
    fundamental (its average frequency), maps it to exactly 1x and the other
    to ~2x, zoom-fits the ratio window around [1, 2], shows "Ch1=1.00x" in the
    cursor readout with no Hz units, exports `chN_x` columns on Copy, refits to
    Hz on exit, and resets when leaving Freq vs Time; Clear All wipes every
    stored curve slot (curve store) and every classic snapshot slot.
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
  - **Resample remixing**: same-rate downmix (equal-and-opposite channels
    average to silence), weight rows normalised by their sum (`{2,1}` →
    two-thirds/one-third), zero-sum rows produce silence while identity rows
    pass through, and a rate change plus downmix in one call keeps the tone's
    Goertzel amplitude exact; invalid weight shapes are rejected.
  - **Stem separation (audio.cpp CLI wrapper)**: anchor-time formatting and the
    `--request-option` anchors JSON (include/exclude signs, escaping); full CLI
    argument construction with and without anchors (`--task s2s --family
    sam_audio`, model/backend/audio/text/out-dir/log in order, `--request-option`
    inserted before `--out-dir` only when anchors exist); binary/model discovery
    against an injectable folder (only `audiocpp_cli.exe` and `sam-audio-*.gguf`
    count).
  - **Paste to New Channel(s)**: existing channels stay byte-for-byte identical
    with unchanged length, the clipboard lands in the new channels at the
    selection start (truncated when it runs past end-of-file), added channels
    become visible, and undo/redo restores the old channel count and content;
    an empty clipboard is a no-op.
  - **Channel row geometry**: one row per visible channel with exactly a 5 px
    gap between rows, the last row absorbing any rounding remainder so rows plus
    gaps fill the area; the spectral view's row layout matches the waveform
    view's.
  - **Multi-file merge (Shift + drag & drop)**: pure layout computation for
    sequence/parallel placement (summed lengths, stacked channels, segment
    offsets), merged audio content with silence padding on missing channels,
    marker shifting plus a region marker per dropped file; `mergeFilesToResult`
    from real WAV files on disk (mixed rates resampled to the highest rate,
    mixed channel counts padded); merging into an active document — same-rate
    sequence append is undoable with exact content/length restoration, rate
    changes and parallel merges replace the whole file and are not undoable;
    `createDocFromMerged` activates a fresh document holding the merged audio
    and markers; and the merge menu's item counts with/without an active doc.
  - **Video audio extraction** (Windows + FFmpeg builds): hand-crafted PCM AVI
    files are read through the FFmpeg reader — channel count/rate/length,
    sample-exact sequential *and* random-access reads, zero-fill beyond EOF;
    a full `AudioEngine` load of an AVI verifies tone amplitudes and a save-as-
    WAV round-trip; a video-only AVI (no audio stream) is rejected gracefully;
    native formats keep priority on `.wav`. Without FFmpeg the registration is
    verified to be a harmless no-op.
  - **Video window** (`tests/regression/video.inc`, fixture
    `tests/data/video_frames_25fps.mp4`: 50 H.264 frames with B-frames, each
    showing its index as binary bars, plus a 1 kHz AAC tone): every frame in
    order without extra seeks, backward / long forward scrubbing, exact frame
    boundaries, before-start / past-end, random scrubbing; the frame pump
    serves the latest of a burst of requests; movie audio level, frequency and
    random-access reads equal to the sequential decode; letterboxing and
    rotation; the MainWindow wiring (document video file, playhead and hover
    following, the Window menu toggles, length notice, closed-by-user state);
    multi-track: the tracks panel's fit/hit-testing/drag/wheel/menu ids,
    selecting a track so it renders above with its offset applied, automatic
    switch to the topmost covering track when the selected one ends, and
    per-track zoom persistence into the document metadata.
  - **Video track metadata & playback** (pure unit tests): `videoTracks`
    serialisation round-trip through `sc::MetaBundle`/`FileDoc` (offsets,
    zoom/pan/rotation, contrast/brightness, solo flag; absent key preserves
    the current list); the image adjustment pixel math (neutral no-op,
    brightness lift, contrast around mid-grey, clamping); the AudioEngine
    solo overlay (a video track's audio replaces the output at its offset,
    silent before/after, main audio returns on removal) and
    `writeDataToNewChannels` (clip at the offset in a new channel, existing
    channels untouched, one undo step, extension past the end).
  - **Alternative scale in the Frequency Analysis readout**: every cursor
    readout (classic snapped/plain, 1/3-octave band centre, Freq-vs-Time
    tracked frequency, Amp-vs-Freq cursor frequency) appends `| Name=value`
    for each enabled alternative-frequency reference; disabled references add
    nothing.
  - **Alternative scales in copied curves**: Copy Spectrum to Clipboard appends
    one column per enabled alternative-frequency reference next to every
    exported frequency (per-channel `chN_Name` after each channel's Hz in the
    curve modes, a shared `Name` column after `freq_Hz` in Amp-vs-Freq and the
    classic/1/3-octave tables); every row's scaled value matches the axis
    conversion; disabled references add nothing and with no reference set the
    output is unchanged.
  - **Transcription list editing**: right-click row menu (Edit... / Add as
    marker / Find... / Add entry / Remove selected); segment edits validate and
    normalise reversed times, reject negative/NaN values and out-of-range rows;
    Add entry inserts at the selection start or playhead in sorted order with an
    empty text; Remove selected deletes every selected row (fallback to the
    clicked row); edited segments persist through a metadata-mode switch and a
    full reload.
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
  - **Settings persistence**: spectrogram log/linear + FFT size (+ auto mode
    with its target time slices / bins-per-pixel parameters), frequency-
    analysis axis mode (all six modes) + its independent FFT size, the
    curve-mode options (Ref Time / Smooth / Track), and the zero-crossing snap
    toggle all survive an app restart via the properties file; stored
    non-power-of-two sizes round up to the next power of two on load.
  - **Axis scale modes** (the Scale form): time-axis `hh:mm:ss.xxxx` /
    `ss.xxxx` / samples and amplitude-axis dB / None / Volt / Pascal / g
    (per-channel scale) / frequency-axis Hz / % / None, with offset, scale,
    drawing order, visibility and Add Axis vs Footer Only per axis —
    verified in `tests/regression/axes.inc`: the default one-axis-per-kind
    state, the shared strip width and ruler stack growing with an added axis and
    shrinking again on undo, strip/ruler geometry, footer readouts, the main
    axis' offset+scale, legacy migration and the metadata round trip; the ruler
    moved to the bottom filling the band below the plot instead of overlapping
    the track (with a second time axis stacked on the far side), the unit-bearing
    labels of an extra frequency axis (`500 Hz`, `1.5 kHz`, `50.00 %`); the
    crosshair arithmetic inverted through the formatters (an offset that makes the
    playhead show a typed time, respecting an existing scale and sample units; a
    selection's two ends giving scale+offset, with the degenerate and sample cases
    reported instead of applied; a clicked reference giving an amplitude scale —
    including the dB law — or a frequency scale, Hz and % of Nyquist); and the
    Scale form's own layout: one block per axis in its column, drag reordering
    inside a column only, the remove button present only where a column has more
    than one axis, the eye and the remove button the same size and side by side
    within the fields' right margin, the crosshairs inside their own rows, and the
    eye's tooltip following what clicking it would do. Probe text
    in sample/offset modes, frequency-factor relabelling, StatusBar field/
    playhead round-trips with typed input, and the marker panel's time cells +
    detail editors are covered as before.
  - **Footer-only axes + Go to Time**: footer entries naming / value-inversion
    helpers; legacy single-reference metadata falling back to one
    "Alt 1" entry plus array-form round-trip of names/enabled flags; sample-mode
    tick density at two zoom levels (samples-per-pixel, not seconds); negative
    clock formatting with carry rounding; Go-to-Time field layout per axis and
    resolution priority (main field first, then footer fields parsed in their own
    units, sample counts and clock-through-offset parsing); dialog structure
    (pre-filled main/alt/reference editors, visibility toggle, OK/Cancel/Delete)
    and the apply logic for edit/cancel/delete with invalid-text rejection;
    waveform probe listing every enabled reference by name, signed readout in
    negative offset mode, and sample-mode ruler labels actually rendering.
  - **Zero-crossing snap selection**: with the toggle off (the default) drag
    edges are untouched; with it on both edges land on tone zero crossings;
    zero-length and programmatic selections are never snapped. The analysis
    view's combos write their axis mode / FFT size back to the shared settings.
  - **Recording** (plugin ABI + host): a deterministic fake recorder plugin
    (`tests/plugins/fake_rec/`) drives the full session — configure/start,
    pulling interleaved float32 through `AudioEngine::appendSamples` until stop,
    phase-recovered sine content checks on the recorded document, re-engagement
    after a stop; the follow-end window state machine is tested statically
    (follows at the end, freezes when panned away, resumes within ~1.5 s of the
    end, never narrows below 5 s); per-channel selection is covered end-to-end
    (a stereo fake device recording only its second channel yields a mono
    document with exactly that channel's content; stale saved modes fall back
    to `All`), and the Transport menu structure itself is verified (Record +
    Recording Device present with correct item counts). The thread-safe
    recorder ring buffer (`sc::rec::FloatRing`) is tested directly: consuming
    read order, partial reads, wrap-around with drop-oldest when full, and
    stride reads into wider channel layouts.
  - **Content Aware Time Scale**: on a silence/tone/silence buffer the onset
    and offset show up as large frame-to-frame changes and their gaps are
    protected, while stable regions score stability ≈1; reduce-gaps edits
    avoid protected gaps, match the previewed sample amount exactly and
    shorten the buffer by exactly that amount (percentile 0 removes nothing);
    time-scale planning only removes when shortening / only duplicates when
    lengthening with the total within one hop of the requested delta, the
    output length matches the plan exactly, no NaN/Inf appears and the prefix
    before the first edit's crossfade is bit-exact; end-to-end through
    `Application::contentAwareTimeScale` — reduce-gaps on a selection shortens
    the file with the edited range staying selected in place, undo restores the
    original length *and* content exactly, and a factor-2 stretch halves the
    whole file (hop-quantised); the dialog's row visibility switches with the
    mode drop-down; end-point refinement snaps a steady tone's cut end back
    onto its phase grid while leaving silence and noise untouched; mark-only
    mode adds hop-aligned "CAT remove N" region markers without touching the
    audio or the undo stack (selection-scoped marking stays inside the
    selection); head/tail coverage shows leading silence tagged from t=0 and
    trailing content removable up to one FFT window before EOF.
  - **Single-instance mode**: the persisted setting round-trips through a real
    properties file (checked by default; stored 0/1 read back); a full wire-
    protocol round trip over a named pipe delivers both paths intact (including
    one with spaces), an empty list is acknowledged, and forwarding fails
    cleanly when no server listens; the Window menu item exists, is checked by
    default, and toggling it persists to disk immediately.
  - **Waveform zero line**: the per-mode zero-line labels ("0" / "-inf" /
    "0.000") and a headless paint check that with the amplitude grid on the row
    centre carries the semi-transparent grey zero line (measured as a blend with
    the (60,60,60) divider it covers, not an opaque colour) while with it off
    only the divider shows - drawn at amplitude zero so it follows the vertical
    pan/zoom instead of staying fixed at the row middle.
  - **Track v-pan behaviour**: a left-drag on the waveform's amplitude axis
    follows the cursor in both directions (drag down then back up restores the
    offset exactly; reversing responds immediately, no ratcheting), and a
    left-drag on the dedicated track's value axis moves content with the
    cursor like the other tracks' axes do.
  - **1/3 octave grid**: the IEC preferred centre series inside a range is
    exact (100…1600 for [100 Hz, 2 kHz]), and a headless paint check renders
    the spectrogram with the overlay off vs on — the grid-on image must show
    visible new pixels (the dashed band borders + centre labels).
  - **Menu bar order**: View sits directly between Effects and Window in the
    menu bar (File Edit Selection Transport Effects View Window Help), and
    the View menu's new "Show 1/3 Octave Grid" item (listed directly under
    "Show Frequency Grid") toggles its setting when clicked.
  - **FFT axis wheel zoom**: in Amp vs Time / Freq vs Time a wheel over the
    plot or bottom time ruler shrinks only the time window, one over the right
    vertical axis strip (or Shift+wheel) shrinks only the level/frequency
    window — each leaving the other axis untouched.
  - **Selection channel clip**: with two visible channels a left-drag selection
    started on row 0 and dragged into row 1 stays clipped to row 0 in both
    views (headless paint probe: blue fill inside the start row, none in the
    other), Select All resets it to all rows; right-drag box-zoom from row 0
    into row 1 clips the box to one data area and scales the amplitude axis by
    the single-row factor (not the whole-plot factor); an axis drag behaves the
    same while leaving the time window alone, a ruler drag stays time-only;
    the spectral view clips its selection rectangle and maps a clipped box-zoom
    through the drag-start row's frequency scale.
  - **Split execute first run**: pressing F12 on an unconfigured file opens the
    configuration dialog; closing it saves the default preset and then executes
    the split, creating the destination track with the extracted tone while the
    source channel loses its in-band content.
  - **Stem separation advanced layout**: adding stems up to five rows no longer
    crashes (the old forward-iteration child delete double-freed the last row);
    the description rows stack downward below the hint line, nothing at window
    level overlaps, and the window expands downward with each new stem.
  - **Diagnostic log** (`tests/regression/diagnostics.inc`): off by default and
    silent while off; the versioned session header without the home folder;
    levels and the flight recorder written after an error; secrets redacted,
    content keys dropped (also nested), long strings cut; identical events
    collapsed with a repeat count; the 60/min rate limit with `diag.dropped`;
    the size budget (rotation, oldest files deleted, a header in every part);
    a crash report and an abandoned session reported at the next start; killed
    threads through `juce::Logger`; breadcrumbs from commands, documents, saves,
    failed loads and message boxes (with hints); slow paints, spectrogram tile
    and audio load aggregates; `diag.tail` / `diag.summary` and the support
    bundle; the Window / Help items and the status bar indicator; a 2.3 s
    freeze reported as `ui.hang` then `ui.stall`; the cost while off and on.
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
bin/Release             local build output (untracked): SpectroCraft.exe plus the
                        SpectroCraft/ subfolder with every external DLL. Official
                        builds ship as GitHub Releases from CI instead.
extern/ffmpeg           vendored FFmpeg headers, import libs + prebuilt x64 DLLs (bin/)
extern/OpenVINO         vendored OpenVINO headers, import lib + runtime DLLs (bin/)
extern/whisper-b5454    vendored whisper.cpp b5454 header + runtime DLLs (bin/)
src/Icons/icon1..4.ico    application icon candidates; src/Icons/icon4.ico is the active one
                        (PE resource + runtime WM_SETICON; include/IconData.h is
                        generated from it by tools/make_icon_data.py)
CMakeLists.txt            root build (JUCE vendored in extern/)
include/                  component headers (incl. RecorderPlugin.h plugin ABI)
plugins/wasapi/           WASAPI recorder DLL source (deployed to <exe>/recorders/)
src/                      app sources + Main.cpp (CLI flags)
tests/                    UnitTests.cpp, CMakeLists.txt, data/ fixtures,
                          plugins/fake_rec/ deterministic test recorder plugin
tools/                    auxiliary diagnostics (address→symbol resolvers,
                          GDI stress test) used while debugging this project
extern/JUCE               vendored JUCE 9.0.4
requirements/             original task requirements & reference screenshots
```
