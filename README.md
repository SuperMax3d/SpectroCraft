# SpectroCraft

A high-performance, single-file waveform audio editor in the style of classic
DAW waveform editors. No multitrack/mixer — one file at a time, with a tiled,
multithreaded FFT spectral view that stays interactive on multi-hour, 96 kHz,
32-bit multichannel files without ever loading the whole file into RAM.

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
  fine detail on anything else) instead of blocky columns.
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
  marker at the cursor. The list is cleared when a new file is loaded (a
  sidecar next to the file is re-read if present), and **double-clicking a
  marker** — in the Markers tab or on its line/region in either view — sets the
  time selection to that marker's range.
  Right-clicking a row in the marker table shows a context menu with **Copy All
  Markers** (tab-separated `Label\tBegin\tEnd\tComment` lines, CRLF between rows,
  `-` for point-marker ends) and **Paste Markers from Clipboard**, which parses
  that TSV format back into markers (times clamped to the file duration).
  Markers are drawn identically in both views (vertical line for points,
  bracket + label for regions) and stay in sync with the playhead, selection
  and zoom.
- **Markers inside WAV files**: saving as `.wav` embeds the markers into the file
  itself using standard RIFF chunks — a `cue` chunk with one entry per marker at
  its start sample, plus a `LIST`/`adtl` sub-chunk containing one `labl`
  (title) and one `ltxt` (comment) entry per marker. The audio data is written
  as IEEE float32 exactly as before; other formats keep the plain JUCE writer.
- **Classic DAW waveform-editor layout**:
  - Top-left tab group: **Files** (open/close + file info) and
    **Frequency Analysis** (visible by default): live FFT with dB/Hz axes and
    a cursor readout.
    A single click in either view re-analyses immediately: with no selection
    it shows one full-size window centred on the cursor (and follows the
    playhead while playing); with a selection it analyses the whole selection
    at the selected FFT size — when the selection is wider than one window,
    several evenly spaced windows are computed and their magnitudes averaged
    (the classic "average over the selection" behaviour). Its FFT size is
    **independent of the spectrogram's**, plus a window-type selector
    (Rectangular/Hann/Hamming) and a frequency-axis dropdown with three modes:
    **FFT Linear** (linear Hz axis), **FFT Log** (logarithmic axis, the default)
    and **1/3 Octave** (IEC 61260-1 band plot from 50 Hz to 16 kHz — one curve
    per visible channel, each band drawn as a flat-topped step across its
    frequency edges with vertical connectors at the boundaries — the classic
    1/3-octave band-plot look; every click replaces the curve instead of
    adding another).
    10 numbered snapshot buttons. Each button's background matches its curve
    colour (slot *i* uses `fftColours[i % 4]`); a toggled-on button shows the
    full colour with a thick white border, an off-but-stored one is dimmed,
    and an empty slot is a dark tint. **Plain click** on an empty slot stores
    the current live region (selection or playhead) and shows it; plain click
    on a stored slot only toggles visibility — it does *not* update the value.
    **Ctrl+click** re-stores/updates from the current selection/cursor position
    and shows the curve. **Shift+click** clears the slot. Stored curves are
    drawn as dim (alpha 90) overlays beneath the live curves, at their own
    stored FFT size, in all three axis modes. A right-click menu copies the
    spectrum data to the clipboard.
  - Bottom-left tab group: **Spectral Controls** (colour map + preview, dB
    range, Log/Linear toggle, Reset Freq, FFT size) and **Markers** (table with
    a Comment column; select a row to edit its Title, multi-line Comments,
    Begin, End and Length in the detail editors below the table — the comments
    box starts beside its label and extends to the bottom of the panel;
    Add Marker, delete; right-click a row for Copy All Markers / Paste Markers
    from Clipboard).
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
  - Bottom status bar: left side shows a live cursor probe (time + amplitude)
    while hovering over either view; right side shows the **selection start
    and stop times** (`Sel: t0 - t1 s`) whenever a selection exists — updated
    continuously while dragging it in either view — plus the current
    **playhead position** (`Pos: t s`), which tracks playback at 30 Hz.
  - The File menu offers **Save Spectrogram As...** (Ctrl+Shift+E), which writes
    the current spectrogram content (per-channel rows included, no axes or
    overlays) to a PNG file.
- **Application icon**: `icon.ico` (project root) is embedded in
  `include/IconData.h` and applied to the main window at startup via
  `CreateIconFromResourceEx` + `WM_SETICON`, so it appears in the title bar,
  taskbar and Alt-Tab. It is set at runtime rather than through a `.rc`
  resource file because this toolchain's rc.exe emits ICON statements as
  RT_CURSOR entries, which Windows ignores for window icons.
- **Drag & drop**: drop a WAV/AIFF/FLAC/MP3/Ogg file onto the window to load it.
- **Edits** with 50-level undo/redo (patch-based: old/new sample data + length
  delta — memory efficient): silence, fade in/out, normalize, reverse, gain,
  cut/copy/paste, delete, insert. The destructive effects (silence/fade/
  normalize/reverse/gain) apply **only to the currently selected (visible)
  channels** — e.g. with only Channel 2 visible, a fade never touches the
  other tracks; undo restores every channel exactly (patches capture the old
  data *before* the in-place modification).
- **Offline tools**: `--test-spectral` renders a spectrogram PNG headlessly;
  `--generate-test-files` creates the test WAVs; `--run-unit-tests` runs the
  unit suite from either executable.

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

## Running & CLI flags

```
SpectroCraft.exe                          :: launch GUI
SpectroCraft.exe --run-unit-tests         :: run the unit test suite, exit code 0 on success
SpectroCraft.exe --generate-test-files [dir]   :: write tests/data/*.wav (default: tests/data)
SpectroCraft.exe --test-spectral <in.wav> <out.png> [width height t0 t1]
                                           :: offline spectrogram render, e.g.
SpectroCraft.exe --test-spectral tests/data/tone_10s_stereo.wav out.png 512 256 0.0 10.0
```

The test executable accepts the same flags (useful for CI without a display).

---

## Keyboard shortcuts

| Action | Shortcut |
|---|---|
| Open / Close file | Ctrl+O / Ctrl+W |
| Save / Save As | Ctrl+S / Ctrl+Shift+S |
| Undo / Redo | Ctrl+Z / Ctrl+Y |
| Cut / Copy / Paste | Ctrl+X / Ctrl+C / Ctrl+V |
| Select all | Ctrl+A |
| Delete selection | Del / Backspace |
| Play / Pause toggle | Space |
| Move playhead one frame left / right | ← / → |
| Select while moving the playhead (anchor = position at first Shift press; released when Shift is let go) | Shift+← / Shift+→ |
| Jump to start / end of file | Home / End |
| Loop (selection, or whole file if nothing selected) on/off | L |
| Add marker (region if selected, else point at cursor) | M or F8 |
| Zoom in / out (horizontal, both views) | Ctrl+= / Ctrl+- |
| Zoom to fit (both views) | Ctrl+0 |
| Vertical zoom in / out (waveform amplitude + spectral frequency) | Ctrl+Shift+= / Ctrl+Shift+- |

Keyboard focus follows the mouse: clicking either view puts keyboard focus on
that view, so ←/→ and Shift+←/→ work immediately after any click inside a plot.
Clicking neutral areas (splitters, scrollbars, empty space) lands focus on the
main window itself — arrow keys keep working there too. Interactive widgets
(combo boxes, text fields, the marker table) take focus when clicked and keep
the standard Windows behaviour for arrow keys while focused; click back into a
view to return playhead control.

Menus: **File, Edit, View, Transport, Tools, Help**. Tools offers Silence,
Fade In/Out, Normalize, Reverse and **Change Gain...** (a dB dialog that
multiplies the selection by 10^(dB/20)) — all applied only to the currently
visible channels. The View menu has **Zoom to Selection** / **Zoom Out Full**,
a **Colour Map** submenu
(Classic default), checkable **Waveform View / Spectrogram View** items (at
least one always stays visible) and one checkable item per channel; plain click
toggles that channel, **Ctrl/Shift+click shows only it**.

Mouse (waveform *and* spectral views):

| Gesture | Action |
|---|---|
| Click | Move the playhead to that position in **both** views (selection drags start from it) |
| Left-drag | Select a time range |
| Middle-drag | Pan the shared time window (both views stay in sync) |
| Right-click | Context menu: Zoom to Selection, Zoom Out Full, Select All (Ctrl+A), Cut / Copy / Paste / Delete |
| Mouse wheel over the plot / time ruler | Horizontal zoom around the cursor (both views) |
| Mouse wheel over the left axis (or Shift + wheel anywhere) | Vertical amplitude zoom (waveform) / frequency-range zoom around the cursor (spectral) — independent per view |
| Drag the divider between the views | Resize waveform vs spectral area |
| Double-click a marker (line/region) | Set the selection to that marker's range (both views) |
| Double-click elsewhere | **Select the whole file** and fit it in both views |

Selecting a time range in *either* view applies the selection to **both**
views at once.

---

## Architecture

Clean separation into independent components (see `include/` + `src/`):

| Component | Responsibility |
|---|---|
| `AudioEngine` (`juce::AudioSource`) | File I/O via AudioFormatManager, streaming vs in-memory decision, sample access API, mipmap pyramid for the waveform overview, playback transport (play/pause/loop), edit primitives (`applyPatch`, silence/fade/normalize/reverse/cut/copy/paste/delete). All disk reads are mutex-protected; the UI thread never blocks on I/O. |
| `SpectralEngine` | Tiled multithreaded FFT: level selection from zoom, tile scheduling on a `juce::ThreadPool`, LRU cache keyed by (tile, channel, level, fftSize, hop, window). Tiles store **raw dB magnitudes**; gain/bias/dB-range/colour-map scaling is applied per pixel at render time, so changing those settings never requires recomputing tiles. Bilinear sampling into RGBA pixels, colour-map LUTs, headless PNG rendering. Never materializes more than one tile per channel at a time — multi-GB files stream through the reader tile by tile. |
| `WaveformView` / `SpectralView` (`juce::Component`) | Pure drawing + mouse interaction; hold references to their engines and repaint on change messages. Each view holds its own time-window state and its **vertical axis/zoom level** (amplitude for the waveform, frequency range for the spectral view); at application level all horizontal changes flow through `MainWindow::applyTimeWindow`, which keeps both views on the same time range while vertical zooms stay independent. The spectral view requests tiles for its visible range only. Selection, markers and playhead are drawn as overlays *on top of* the audio in both views, so they behave identically wherever you look. |
| `ScrollBars` (`TimeScrollBar`, `ValueScrollBar`) | Custom scrollbar components: thumb shows the displayed portion relative to the whole track; centre-drag pans, edge-drag zooms one side, click outside jumps. One horizontal time bar plus one vertical bar per view. |
| `Panels.cpp`: `FileListPanel`, `FFTAnalysisView`, `SpectralControlsPanel`, `MarkerListPanel` | Classic DAW-style left column: file open/close + info, live FFT analysis with cursor readout (drawn in frequency space — never indexes the magnitude vector by pixel x) and a selectable axis mode (FFT Linear / FFT Log / 1/3 Octave band plot), spectral settings (colour map/dB/Log-Linear toggle/Reset Freq/FFT size) and the marker table. Panels mutate shared state through `Application` and notify `MainWindow` via `app.onStateChange`. |
| `MarkerManager` | Point/region markers with title + comment, sorting, find-near, JSON sidecar persistence next to the audio file. |
| WAV marker writer (`AudioEngine::saveWavWithMarkers`) | Custom RIFF writer for `.wav` saves: `fmt` (IEEE float32), `cue` (one entry per marker at its start sample), `LIST`/`adtl` with `labl` (title) + `ltxt` (comment) entries, then the raw interleaved `data`. All chunk sizes are computed up front; streaming files are written in 1 MB blocks. |
| `UndoManager` / `UndoPatch` | 50-level undo/redo stacks of compact patches (old/new sample data + length delta) instead of full snapshots. |
| `Application` | Owns engine instances, settings and colour map; wires edit actions to undo. |
| `MainWindow` (`DocumentWindow` + `MenuBarModel` + `Timer` + `FileDragAndDropTarget`) | Classic DAW-style layout (left tab groups + waveform/spectral main area + scrollbars), menus, keyboard shortcuts (incl. Space play/pause, Home/End, F8 marker), drag & drop, playhead animation timer and view-range state sync between views/panels. |

Threading model: the UI thread only draws and dispatches; FFT tiles are computed
on a pool sized to hardware threads; file reads happen on the reader under a
mutex with bounded copies. Tile results are published through an LRU cache guarded
by a single mutex (short critical sections, no lock held during FFT work).

---

## Tests (`tests/`)

- `UnitTests.cpp` — lightweight CHECK-based suite (164 checks), run via
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
