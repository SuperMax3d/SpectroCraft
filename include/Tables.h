#pragma once
// SpectroCraft - Tables panel: imported CSV tables with per-parameter
// alignment, overlays on the waveform/spectrogram and a dedicated track.
#include <JuceHeader.h>
#include <vector>

namespace sc::tables
{

/** One column of an imported table (a "parameter"). */
struct ParamData
{
    juce::String name;
    std::vector<double> values;   // one entry per sample row, in file order

    bool visible = true;          // eye icon on the panel row
    int colorIndex = 0;           // index into kPalette (colour square)

    // Alignment configuration (per parameter, independent of its table).
    int alignTarget = 0;         // 0=Waveform 1=Spectrogram 2=Dedicated
    double offsetSecs = 0.0;     // time alignment: docTime = off + scale*tTable
    double scaleTime = 1.0;      //   (composed with the table's own values)
    double valueOffset = 0.0;    // value mapping applied after the transforms
    double valueScale = 1.0;
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
    its own time alignment and the parsed parameter columns. */
struct TableData
{
    juce::String name;           // file name or "Clipboard"
    bool visible = true;         // master eye icon for the whole table
    int colorIndex = 0;

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

/** The 12 colour-square colours (index == colorIndex). */
extern const juce::Colour kPalette[12];

// ------------------------------------------------------------------ Import --

/** Splits CSV text into a grid of fields. Handles quoted fields (with
    embedded commas), CRLF/LF line endings and trims surrounding spaces. */
std::vector<std::vector<juce::String>> parseCsvGrid (const juce::String& text);

/** True when the cell parses as a number or true/false (i.e. is data). */
bool cellIsData (const juce::String& s, double* out = nullptr);

/** Imports CSV text into `out`. Rules:
    - if the first column contains only text, the grid is transposed first;
    - a leading row with any non-numeric cell becomes the header (column
      titles become parameter names; empty titles get "ParamN");
    - true/false cells become 1.0/0.0;
    - scattered non-data cells inside an otherwise numeric column are
      linearly interpolated from their nearest valid neighbours;
    - a column with no data at all is ignored. */
bool importTable (const juce::String& name, const juce::String& csvText,
                  TableData& out, std::string* errOut = nullptr);

/** Resamples every parameter to `newRateHz` rows/second (linear). The new
    length is max(2, round(N * newRate / oldRate)); endpoints are preserved. */
void downsampleTable (TableData& t, double newRateHz);

// ------------------------------------------------------------- Value maps --

/** Applies the parameter's value transforms in order: normalized -> inverted
    -> absolute, then valueScale*v + valueOffset. `minV`/`maxV` are the raw
    range of the whole parameter (for normalization). */
double transformValue (double v, const ParamData& p, double minV, double maxV);

/** Raw (unscaled) value of a parameter at table time tSecs by linear
    interpolation; 0 outside [0, duration]. */
double paramValueAt (const TableData& t, const ParamData& p, double tTableSecs);

// ------------------------------------------------------------- Auto align --

/** Cross-correlates two parameters to find the best time lag for aligning
    `alignTo` onto `reference`. Both are resampled to a common grid first.
    Returns the lag in seconds: adding it to alignTo.offsetSecs shifts its
    curve so that its content matches reference's (positive = later). */
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
