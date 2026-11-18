# The Scale form

One dialog for every axis of a file. It replaces the waveform/spectrogram
context-menu **Scale** submenu and the **Alternative Scale** submenu, and the
the old Files panel's **Set Unit and Scale** dialog (that panel is gone; the
document list is now **File > Loaded Files**).

Opened from:

- right-click on the waveform or the spectrogram → **Scale...**
- **File > Loaded Files > Scale...**
- right-click on the Editor's tab or its header menu → **Scale...**

The window is a **normal window** (its own title bar with a close button) that
is **modeless and stays above the other windows**: the tracks keep reacting
while you move the playhead or click a reference, which is how the
alternative-scale flow was used. Every change applies immediately (live
preview).

## The layout

Three columns — **Time | Amplitude | Frequency** — each holding that kind's
axes as a vertical stack:

```
| Time           | Amplitude      | Frequency      |
|----------------|----------------|----------------|
| Name1      ⊙ × | Name1      ⊙ × | Name1      ⊙ × |
|   Units        |   Units        |   Units        |
|   Offset       |   Scale  SAME  |   Scale        |
|   Scale        |   Type         |   Type         |
|   Type         |                |                |
| New            | New            | New            |
| Name2      ⊙ × |                |                |
|   Units        |                |                |
|   ...          |                |                |
| New            |                |                |
```

- The **first block of a column** is that kind's main axis: it drives the grid,
  the ruler and the level readout. The blocks under it add extra strips (or,
  with **Footer Only**, readouts).
- **SAME** (on every block that is not the column's main axis) copies the main
  axis's units, offset and scale onto this one.
- A **crosshair button** next to a numeric field takes that value from the
  track instead of from the keyboard:

  | button | what it asks | what it computes |
  |---|---|---|
  | Offset (time) | the time the playhead should show | `offset` so `displayed(playhead)` is that time |
  | Scale (time) | the times the selection's two ends should show | `scale` and `offset` that match both ends |
  | Scale (amplitude) | click a point in the waveform, then its value | `scale` that shows that value there |
  | Scale (frequency) | click a line in the spectrogram, then its value | `scale` that shows that value there |

  The form itself only asks (`onPickRequest`); `MainWindow::handleScalePick`
  prompts and arms the click, because it owns the views and the dialogs. The
  arithmetic lives in `sc::axes::offsetForDisplayedValue`,
  `sc::axes::solveTimePoints` and `sc::axes::scaleForReference` — the exact
  inverses of `displayValueTime`, `displayValueFrequency` and `formatAmplitude`
  (a dB reference inverts the dB law, a percent reference uses the Nyquist
  span). A reference at zero, or a selection whose two ends show the same value,
  gives no scale and says so in the status bar instead of changing anything.
  Sample units have no scale factor, so only the offset follows.
- The eye and the remove button are **the same size and sit together** at the
  right edge of the name row, ending at the same margin as the text boxes below
  them (`kRightPad`); a field with a button stops where that button starts, so
  nothing of a block hangs over its column. The eye's tooltip says what clicking
  it would do: **Hide Axis** / **Show Axis**.
- A block's name row carries the **drag grip** (`⊙`, a real widget with its own
  mouse handling, not a decoration), the **visibility eye** — the same icon the
  tables use for showing a table or a parameter — and the **x** remove button.
  The **x only appears when the column holds more than one axis**: a column's
  single axis has nothing to remove, so it has no button. Each control acts on
  its own block: the form keeps the entry index it was built for, and
  `refreshFromApp()` re-reads the document whenever something else changed the
  axes (an undo, another session) — it is a no-op when nothing changed, so a
  field you are typing in is never thrown away. **Order is changed by dragging
  the name** up or down inside its column — there are no arrow buttons. A press
  still puts the caret in the text; a drag only becomes a reorder when it
  clearly goes vertically, so selecting text keeps working. A green line shows
  where the block will land.
- **New** at the bottom of a column adds an axis of that kind.
- Only time axes have an **Offset**; amplitude and frequency axes are
  multipliers.

## The model

`include/AxisScale.h` (`sc::axes`) defines one entry per axis:

| field | meaning |
|---|---|
| `kind` | Time, Amplitude or Frequency |
| `name` | the label used by the readouts, the Go to Time fields and the export columns |
| `units` | index into the kind's vocabulary (see below) |
| `offset` | time axes: `displayed = (t + offset) × scale` |
| `scale` | amplitude / frequency multiplier (and time, for the plain-seconds unit) |
| `chanScale` | amplitude axes: one multiplier per channel (empty = `scale`) |
| `visible` | drawn at all |
| `type` | **Axis and Footer** (a strip on the tracks and a footer readout; called "Add Axis" before issue #54) or **Footer Only** (a readout only — the old alternative scale) |

Unit vocabularies:

- **Time**: `hh:mm:ss.xxxx`, `ss.xxxx`, `samples`
- **Amplitude**: `dB`, `None`, `Volt`, `Pascal`, `g` (a physical unit calibrates the file, as the old "Set Unit and Scale" did)
- **Frequency**: `Hz`, `%` (of Nyquist), `None`

The **first visible Axis and Footer entry of each kind is the main axis**: it
drives the grid, the ruler, the level readouts and the existing session state
(`timeAxisMode`, `timeOffsetSecs`, `waveVScaleMode`, the file's
`ScaleCalibration`, `specFreqScaleMode`/`specFreqFactor`). The remaining Axis
and Footer entries draw extra strips in their own units — they never move the
grid that is actually drawn. A frequency axis is always shared by all
channel rows: the engine renders one frequency window for the whole spectrogram,
so a per-row frequency scale would not be a rendering, only a label.

## Storage, migration, undo

Entries live in the document's `MetaBundle` (`axes`), i.e. in the embedded SCMD
RIFF chunk of a WAV or in the `.meta.json` sidecar. `Application::activate()`
applies them to the session; `pushActiveDocMeta()` writes them back.

A file whose metadata carries the older state is migrated automatically
(`sc::axes::migrateLegacy`): a calibration becomes an amplitude axis with that
unit and scale, a custom frequency factor becomes a scaled frequency axis, and
each alternative reference becomes a **Footer Only** axis with its ratio. The
legacy fields are kept in step so older versions still read the file.

`Application::setAxisEntries (list, description)` is the only way to change
them. It applies the entries, marks the metadata dirty and records **one
undoable step** (History shows "Scale"): undo/redo restores the axis list. An
empty list is rejected — every kind keeps at least its main axis.

## Shared geometry and one look for every track

- `Application::axisStripWidth()` — every track reserves the same horizontal
  space for its vertical axes: the widest of the three label sets wins, the
  others get empty space. `WaveformView::axisW()`, `SpectralView::axisW()`,
  `TablesTrackView::axisW()` all read it. The main strip is
  `sc::axes::primaryStripWidth()` (60 px, as always); **each additional axis
  adds `extraStripWidth()` (38 px)**, not another 60 — a second amplitude or
  frequency axis costs 98 px in total instead of 120, and the strips are laid
  out from the plot outward so they always fill exactly what was reserved.
- `Application::rulerStackHeight()` — the normal 24 px ruler plus one 18 px
  strip per extra visible time axis, with the main ruler always the one closest
  to the data (see "Which strip is which"). The spectrogram fills the
  **stack itself** with the axis-strip colour (it used to fill
  `plotTop - stackHeight`, which painted a dark band over the top of the
  spectrum and left the ruler area unpainted when the ruler is moved to the
  bottom with View > Time Ruler). Ruler labels are drawn below the ticks in
  that case instead of over them.
- `sc::axes::axisLabelFont/axisLabelColour/axisMinorLabelColour/axisGridColour`
  — one size, one colour set for the waveform, the spectrogram and the dedicated
  tables track. The waveform's amplitude grid is drawn **before** the waveforms,
  so it no longer needs the alpha it used to carry to avoid hiding a quiet
  signal on the zero line.
- The History panel's table now uses the shared LookAndFeel table-header colours
  like every other table, on the same panel background.
- **The bars follow the data.** `MainWindow::resized()` insets the horizontal
  time bar by `app.axisStripWidth()` on whichever side the axis is, and
  `vScrollInsetsForView` shortens a view's vertical bar by
  `app.rulerStackHeight()`. Both used the fixed 60 px / 24 px constants, so the
  bars were right for one axis and wrong for more: the time bar ran under the
  extra strips and the vertical bar went past the rows. The tables strip below a
  top ruler uses the same stack height.

## Ticks, strips and the footer

**Every frequency axis ticks at nice values of its own unit** (displayed =
k × Hz), and draws each tick at the exact frequency it stands for, so a label
reads exactly what its line points at (issue #54). The main axis' grid lines
follow its own unit (a scaled main axis gets nice scaled values); an extra axis
draws a short tick in its strip for each of its labels. Labels carry every
digit of the tick value (`sc::axes::formatTickNumber`): the main axis used to
round to one decimal of kHz and an extra axis labelled the main axis' positions
in its own unit, rounded, so a line at 1605 Hz read "1.6 kHz". Extra strips
spell the unit out (`sc::axes::formatFrequencyTick`: `1.25 kHz`, `12.5 %`).
The time rulers already tick at nice displayed values, and the amplitude axes
label fixed dB levels with their exact converted value.

A Footer Only axis adds no strip.

### Which strip is which

**The first row of a column is the axis closest to the track**, everywhere:

| axis | placement | order of the strips |
|---|---|---|
| amplitude, frequency | left of the plot | main strip hugs the plot, extras further out |
| amplitude, frequency | right of the plot | main strip hugs the plot, extras further out |
| time | above the plot | main strip hugs the top edge of the plot, extras above it |
| time | below the plot | main strip hugs the bottom edge of the plot, extras below it |

`sc::axes::verticalStrip()` and `sc::axes::timeStrip()` are the only places
that decide this, and both take the index in the list — index 0 is always the
one nearest the data. A ruler stack that hangs *below* the plot has to be placed
below it: the caller's plot rect already excludes the stack, so putting the main
ruler at `plotBottom - primary` draws it over the track and leaves the reserved
band empty (a black bar).

**Every axis keeps to its own strip.** `sc::axes::verticalLabelRect(index,
count, ...)` gives the label box of one strip; the main labels used to span the
whole shared width, which is why a second frequency axis was drawn on top of the
first on the left side (and why only one set of amplitude units was readable).

**The channel label** (`L` / `R`, or `1 2 3 4`) sits at the *outer* edge of the
axis area — left of the units when the axis is on the left, right of them when
it is on the right — in 13 pt. `sc::axes::channelGutter()` (24 px) is the room
the outermost strip keeps for it on the left, so the two never overlap. That
room belongs to the **outermost** strip: with a second amplitude axis the main
strip gives its 24 px up to the extra one (`verticalStrip(index, count, ...,
gutter)`), so the total width is unchanged and the extra labels keep their
whole 34 px (they used to get 10 px and read `...`). Frequency labels pass a
gutter of 0: they already skip the row centre where the channel label sits, so
they use their strips' full width and, on a left axis, are right-aligned
against the plot (`10.0 kHz` was cut to `10.0 k`). An extra frequency axis
labels the minor ticks too, not only the decades, so it shows as many values
as the main one (issue #27).

**Secondary axes read as secondary**: their labels use the same 8.5 pt type as
the extra time axes, and their strips are narrower (38 px against the main 60).

Every **secondary** axis of a kind (`sc::axes::secondary`: the Axis and Footer
extras and the Footer Only entries) appears, in its own unit:

- in the cursor readouts of the spectrogram and the Frequency Analysis panel
  (`FFTAnalysisView::altFreqSuffix`). They used the derived references, a bare
  factor, so a `%` axis read `f × scale` in the footer and
  `f × scale / Nyquist × 100` on its strip;
- as an extra field in **Go to Time** (parsed in its own units);
- as an extra column in the Frequency Analysis panel's Copy and in **Tables >
  Table from Spectral Selection**, which also gets one level column per
  secondary amplitude axis.

`Application::applyEntries` derives the legacy `altTimeRefs`/`altFreqRefs` from
them, so the code that consumed those lists keeps working.

The Frequency Analysis panel's right-click **Unit > Frequency / Amplitude**
chooses which axis labels its graphs: Hz or any frequency axis (ticks at nice
values of that unit, as above; the 1/3-octave mode keeps its band centres), and
the file's calibration or any amplitude axis.

## The window

**Close** and **Cancel** sit at the bottom right. Every change applies live, so
Close only closes; Cancel puts back the axes the file had when the form was
opened (one undoable step) and closes. Typing in a name, offset or scale field
applies live without rebuilding the form, so the field keeps the keyboard focus
(it was rebuilt after every character). The window opens 690 px wide (it was
980), is resizable down to 600 px, and can be minimised and maximised.

## Tests

`tests/regression/axes.inc` (registered as `regression: axes`) covers: the
default one-axis-per-kind state, the shared strip width and ruler stack growing
with an added axis and shrinking again on undo, strip/ruler geometry, footer
readouts and Go to Time parsing, the main axis' offset and scale, migration of
the legacy state, and the metadata round trip.

## Not done yet

The Files panel has been removed from the docking registry. Its document list,
activation and close live in **File > Loaded Files**, and **Open Containing
Folder** is also in the waveform / spectrogram context menus and in the
Editor's panel header menu. The `FileListPanel` class itself is still built
(the tests exercise it directly) but no window registers it. Its document list, Activate, Close
and Open Containing Folder). Its scale entry point now opens this form. Removing
the panel from the docking registry means updating the docking/layout tests that
use `"files"` as their sample panel — a separate change.
