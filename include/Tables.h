#pragma once
// SpectroCraft - Tables panel: imported CSV tables with per-parameter
// alignment, overlays on the waveform/spectrogram and a dedicated track.
#include <JuceHeader.h>
#include <vector>
#include <functional>

namespace sc::tables
{

/** One column of an imported table (a "parameter"). */
struct ParamData
{
    juce::String name;
    std::vector<double> values;   // one entry per sample row, in file order

    bool visible = true;          // eye icon on the panel row
    int colorIndex = 0;           // index into kPalette + custom colours (tableColourAt)

    // Alignment configuration (per parameter, independent of its table).
    int alignTarget = 0;         // 0=Waveform 1=Spectrogram 2=Dedicated
    double offsetSecs = 0.0;     // time alignment: docTime = off + scale*tTable
    double scaleTime = 1.0;      //   (composed with the table's own values)
    // Vertical value mapping, independent per display mode:
    // [0]=Waveform overlay, [1]=Spectrogram overlay, [2]=Dedicated track.
    // Horizontal (time) alignment stays shared across all three modes.
    double valueOffset [3] = { 0.0, 0.0, 0.0 };
    double valueScale  [3] = { 1.0, 1.0, 1.0 };
    bool normalized = false;     // map to [0,1] over the parameter's range
    bool inverted = false;       // negate
    bool absolute = false;       // absolute value

    /** Duration of this parameter in table time (seconds). */
    double duration (double sampleRate) const
    {
        return values.empty () ? 0.0 : (double) (values.size() - 1) / sampleRate;
    }
};

/** One imported CSV file: a name, an implicit sample rate (rows per second),
    its own time alignment and the parsed parameter columns. Tables carry no
    colour of their own - only their parameters do. */
struct TableData
{
    juce::String name;           // file name or "Clipboard"
    bool visible = true;         // master eye icon for the whole table

    double sampleRate = 1000.0;  // rows per second (changed by Downsample)
    double offsetSecs = 0.0;     // table-level time alignment
    double scaleTime = 1.0;

    std::vector<ParamData> params;

    /** Duration of the longest parameter in table time (seconds). */
    double duration() const
    {
        double d = 0.0;
        for (const auto& p : params)
            d = std::max (d, p.duration (sampleRate));
        return d;
    }

    /** Effective time mapping of a parameter: docTime = offset + scale*tTable.
        The table's and the parameter's own configurations compose
        additively in offset and multiplicatively in scale. */
    std::pair<double, double> effectiveTimeMap (const ParamData& p) const
    {
        return { offsetSecs + p.offsetSecs, scaleTime * p.scaleTime };
    }

    /** Document time of sample i of parameter p under the effective map. */
    double docTimeOfSample (const ParamData& p, int i) const
    {
        const auto [off, sc] = effectiveTimeMap (p);
        return off + sc * ((double) i / sampleRate);
    }
};

/** The 12 built-in colour-square colours (indices 0..11 of the palette). */
extern const juce::Colour kPalette[12];

/** Resolves a parameter colour index against the full palette: indices 0..11
    are the built-in kPalette entries, higher indices address user-defined
    custom colours (in order). Out-of-range values fall back into the built-
    in palette so stale metadata never breaks drawing. */
juce::Colour tableColourAt (int colorIndex, const std::vector<juce::Colour>& customs);

/** Serialises a custom colour list as "r,g,b;r,g,b;..." for persistence. */
juce::String formatCustomColours (const std::vector<juce::Colour>& customs);

/** Parses the format produced by formatCustomColours into `out` (replaces it).
    Malformed entries are skipped silently. */
void parseCustomColours (const juce::String& text, std::vector<juce::Colour>& out);

// ------------------------------------------------------------------ Import --

/** Splits CSV/TSV text into a grid of fields. Handles quoted fields (with
    embedded separators), comma AND tab column separators, CRLF/LF line
    endings and trims surrounding spaces. One pass over the UTF-8 bytes.
    `progress`, when set, gets the fraction of the text read (about once per
    MiB) and cancels the parse by returning false: the grid comes back empty. */
std::vector<std::vector<juce::String>> parseCsvGrid (const juce::String& text,
                                                     const std::function<bool (double)>& progress = {});

/** True when the cell parses as a number or true/false (i.e. is data). */
bool cellIsData (const juce::String& s, double* out = nullptr);

/** Imports CSV/TSV text into `out`. Rules:
    - if the first column contains only text, the grid is transposed first;
    - a leading row with any non-numeric cell becomes the header (column
      titles become parameter names; empty titles get "ParamN");
    - true/false cells become 1.0/0.0;
    - scattered non-data cells inside an otherwise numeric column are
      linearly interpolated from their nearest valid neighbours;
    - a column with no data at all is ignored.

    Time-column auto-detection: when the first column's header contains
    "time" (case-insensitive) and its values are strictly increasing, that
    column positions the rows on the document timeline instead of becoming a
    parameter. The table is fitted to the uniform-grid model with
    sampleRate = (rows-1)/span and offsetSecs = start; when `docDurationSecs`
    is >= 0 and the start lies beyond it, the whole span is shifted so that
    the table starts at 0.

    `progress` (any thread: the import touches no shared state) gets the
    fraction done, 0..1, and cancels by returning false: the import then
    fails with "cancelled". */
bool importTable (const juce::String& name, const juce::String& csvText,
                  TableData& out, std::string* errOut = nullptr,
                  double docDurationSecs = -1.0,
                  const std::function<bool (double)>& progress = {});

/** Decimates every parameter so the table holds at most `newRateHz` rows per
    second: keeps every ceil(oldRate/newRate)th row - pure removal, no
    interpolation and no added data. The table's sample rate becomes exactly
    oldRate/N (the density of the kept rows). Does nothing when the current
    rate is already at or below `newRateHz`. */
void downsampleTable (TableData& t, double newRateHz);

// ------------------------------------------------- Timeline anchoring --

/** Resamples every parameter of `t` so its curves follow the audio timeline
    after a warp: each parameter's effective domain [s, e] moves to
    [W(s), W(e)], its own time offset is shifted by W(s)-s (its scale is
    untouched) and its values are re-interpolated through the inverse warp.
    Content inside deleted ranges disappears; inserted gaps are bridged with
    a constant value. Called after destructive audio edits, exactly like the
    marker/drawing/SCTT remaps. */
void warpTableForDeletion (TableData& t, double a, double b);
void warpTableForInsertion (TableData& t, double a, double lenSecs);
void warpTableForStretch   (TableData& t, double a, double bOld, double bNew);

// ------------------------------------------------------------- Value maps --

/** Applies the parameter's value transforms in order: normalized -> inverted
    -> absolute, then valueScale[target]*v + valueOffset[target]. `minV`/
    `maxV` are the raw range of the whole parameter (for normalization).
    target: 0=Waveform overlay, 1=Spectrogram overlay, 2=Dedicated track. */
double transformValue (double v, const ParamData& p, double minV, double maxV,
                       int target);

/** Value used by the waveform/spectrogram overlays: the raw value is first
    fitted into [-1,+1] over its own range (so a curve is visible regardless
    of the data's units), then inverted/absolute are applied and finally the
    per-target valueScale[target]/valueOffset[target]. The normalized flag is
    ignored - fitting already happened. */
double overlayValue (double v, const ParamData& p, double minV, double maxV,
                     int target);

/** The fitted value before the user's per-target scale/offset: what
    overlayValue() returns with scale=1 and offset=0 for `target`. Used by
    two-point alignment to solve the vertical mapping. */
double fittedOverlayValue (double v, const ParamData& p, double minV, double maxV);

/** Value plotted on the dedicated track: the raw value through that mode's
    own linear map (valueScale[2]*v + valueOffset[2]). No fitting - the track
    has its own auto-scaled axis. */
double dedicatedValue (double v, const ParamData& p);

/** Solves scale/offset so that fitted values f1/f2 land on doc-side values
    v1/v2:  v = scale*f + offset. Returns false when the two fitted values are
    indistinguishable (then only an offset could be solved). */
bool solveValueMap (double f1, double f2, double v1, double v2,
                    double& scale, double& offset);

/** Raw (unscaled) value of a parameter at table time tSecs by linear
    interpolation; 0 outside [0, duration]. An empty cell (NaN) interpolates
    to NaN - emptiness never turns into zero. */
double paramValueAt (const TableData& t, const ParamData& p, double tTableSecs);

// ------------------------------------------------- Selection-range edits --
// The dedicated track's "Edit" menu operates on document-time ranges.
// Empty cells are NaN in values[]: they are not drawn, do not affect the
// auto-scaled axes and survive warping/downsampling as gaps (they are never
// treated as zero).

/** Min/max over the finite entries of v; returns false when there are none. */
bool finiteMinMax (const std::vector<double>& v, double& lo, double& hi);

/** Overwrites p's values inside [t0,t1] (document time) with a linear ramp
    from startVal to stopVal. When the range extends beyond the table's row
    grid, every parameter is extended: new cells are empty (NaN), and for a
    front extension all parameters' offsets shift so their existing curves
    keep their document times. Rows outside [t0,t1] keep their values. */
void overwriteParamRange (TableData& t, int paramIdx, double t0DocSecs,
                          double t1DocSecs, double startVal, double stopVal);

/** Marks p's cells inside [t0,t1] (document time) as empty (NaN). Rows that
    become entirely empty at the very beginning or end of the table are
    trimmed away (a front trim shifts every parameter's offset so the
    remaining curves keep their document times); interior gaps stay. */
void deleteParamRange (TableData& t, int paramIdx, double t0DocSecs, double t1DocSecs);

/** First/last finite value of p whose document time lies in [t0,t1]; when no
    such row exists the search continues outward from the range edge. Returns
    false only if the parameter has no finite values at all. */
bool firstValidValue (const TableData& t, const ParamData& p, double t0Secs,
                      double t1Secs, double& out);
bool lastValidValue  (const TableData& t, const ParamData& p, double t0Secs,
                     double t1Secs, double& out);

// ------------------------------------------------------------- Auto align --

/** Cross-correlates two parameters in DOCUMENT time (through their effective
    offset/scale maps) to find the best lag for aligning `aliParam` onto
    `refParam`. Both are resampled onto a common grid spanning the union of
    their doc-time ranges. Returns the lag in seconds: adding it to ali's
    effective offset shifts its curve so that its content matches ref's.
    Re-running after applying the result yields ~0 (idempotent). */
double autoAlignLag (const TableData& refTable, const ParamData& refParam,
                     const TableData& aliTable, const ParamData& aliParam);

// ------------------------------------------------------------- Persistence --

/** Serialises the tables + display mode to a JSON array/object pair. The
    returned var is an object { "tables": [...], "mode": N }. */
juce::var serialiseTables (const std::vector<TableData>& tables, int mode);

/** Parses what serialiseTables produced; missing keys leave `out`/`mode`
    untouched. Returns false on malformed input. */
bool parseTables (const juce::var& v, std::vector<TableData>& out, int* mode = nullptr);

} // namespace sc::tables
