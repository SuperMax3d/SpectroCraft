// SpectroCraft - Tables panel data model: CSV import, downsampling, value
// transforms, cross-correlation auto alignment and metadata persistence.
#include "Tables.h"

namespace sc::tables
{

const juce::Colour kPalette[12] = {
    juce::Colour::fromRGB (255, 80, 80),     // red
    juce::Colour::fromRGB (255, 170, 60),    // orange
    juce::Colour::fromRGB (240, 230, 90),    // yellow
    juce::Colour::fromRGB (110, 220, 110),   // green
    juce::Colour::fromRGB (80, 220, 200),    // teal
    juce::Colour::fromRGB (90, 160, 255),    // blue
    juce::Colour::fromRGB (170, 120, 255),   // violet
    juce::Colour::fromRGB (255, 120, 200),   // pink
    juce::Colour::fromRGB (255, 210, 160),   // sand
    juce::Colour::fromRGB (180, 200, 220),   // steel
    juce::Colour::fromRGB (230, 230, 230),   // light gray
    juce::Colour::fromRGB (140, 110, 90)     // brown
};

juce::Colour tableColourAt (int colorIndex, const std::vector<juce::Colour>& customs)
{
    if (colorIndex >= 0 && colorIndex < 12) return kPalette[colorIndex];
    const int ci = colorIndex - 12;
    if (ci >= 0 && ci < (int) customs.size()) return customs[(size_t) ci];
    // Stale index (customs were removed): wrap into the built-in palette.
    return kPalette[std::max (0, colorIndex % 12)];
}

juce::String formatCustomColours (const std::vector<juce::Colour>& customs)
{
    juce::String s;
    for (const auto& c : customs)
    {
        if (!s.isEmpty ()) s += ";";
        s += juce::String ((int) c.getRed()) + ","
           + juce::String ((int) c.getGreen()) + ","
           + juce::String ((int) c.getBlue());
    }
    return s;
}

namespace
{
// Validates a 0..255 byte field: digits only (optional leading minus), then
// clamps. Returns false for anything else.
bool parseByteField (const juce::String& s, int* out)
{
    if (s.isEmpty () || s.length () > 4) return false;
    for (int i = 0; i < s.length (); ++i)
    {
        const auto ch = s[i];
        if (!(juce::CharacterFunctions::isDigit (ch)
              || (i == 0 && ch == L'-'))) return false;
    }
    *out = std::clamp (s.getIntValue (), 0, 255);
    return true;
}
} // namespace

void parseCustomColours (const juce::String& text, std::vector<juce::Colour>& out)
{
    out.clear ();
    if (text.isEmpty ()) return;
    juce::StringArray entries;
    entries.addTokens (text, ";", "");
    for (const auto& e : entries)
    {
        juce::StringArray rgb;
        rgb.addTokens (e.trim (), ",", "");
        if (rgb.size () != 3) continue;
        int r = 0, g = 0, b = 0;
        if (!parseByteField (rgb[0].trim (), &r)) continue;
        if (!parseByteField (rgb[1].trim (), &g)) continue;
        if (!parseByteField (rgb[2].trim (), &b)) continue;
        out.push_back (juce::Colour::fromRGB ((juce::uint8) r, (juce::uint8) g,
                                              (juce::uint8) b));
    }
}

// ------------------------------------------------------------------ Import --

// NOTE: this JUCE build has no String::operator+=(char16_t) - on MSVC a
// char16_t would silently bind to the int overload and append the character's
// numeric code. Accumulate code points instead and convert once per field.
std::vector<std::vector<juce::String>> parseCsvGrid (const juce::String& text,
                                                     const std::function<bool (double)>& progress)
{
    std::vector<std::vector<juce::String>> grid;
    std::vector<juce::String> row;
    // One pass over the UTF-8 bytes (issue #14): juce::String's operator[]
    // and length() walk the text from its start, which made this loop
    // quadratic (a 67 MB CSV never finished). Every byte that matters here
    // (quote, comma, tab, CR, LF) is ASCII, and UTF-8 never uses ASCII bytes
    // inside a multi-byte character, so fields split at the same places as
    // the code points did and keep their characters whole.
    const char* const s = text.toRawUTF8();
    const size_t len = text.getNumBytesAsUTF8();
    std::string fieldBuf;
    bool inQuotes = false;

    auto pushField = [&] ()
    {
        if (!fieldBuf.empty ())
            row.push_back (juce::String::fromUTF8 (fieldBuf.data(), (int) fieldBuf.size()).trim());
        else
            row.push_back (juce::String());
        fieldBuf.clear();
    };
    // NOTE: a blank line yields one empty cell and is kept as a data gap
    // (importTable interpolates across it). Ragged rows are tolerated by the
    // importer, which treats missing cells as empty.
    auto pushRow   = [&] ()
    {
        if (!row.empty ())
        {
            const size_t width = row.size();
            grid.push_back (std::move (row));
            row.clear();
            row.reserve (width);   // the next row most likely has as many cells
        }
    };

    constexpr size_t kProgressStep = (size_t) 1 << 20;   // report every MiB
    size_t nextReport = kProgressStep;
    for (size_t i = 0; i < len; ++i)
    {
        if (progress && i >= nextReport)
        {
            nextReport = i + kProgressStep;
            if (!progress ((double) i / (double) len))
                return {};   // cancelled
        }
        const char c = s[i];
        if (inQuotes)
        {
            if (c == '"')
            {
                // "" inside a quoted field is an escaped quote.
                if (i + 1 < len && s[i + 1] == '"')
                    ++i;   // keep one literal quote, skip its twin
                else
                {
                    inQuotes = false;
                    continue;   // closing quote: not part of the field
                }
            }
            fieldBuf.push_back (c);
        }
        else if (c == '"')
        {
            inQuotes = true;
        }
        else if (c == ',' || c == '\t')   // comma- and tab-separated alike
        {
            pushField();
        }
        else if (c == '\n' || c == '\r')
        {
            if (c == '\r' && i + 1 < len && s[i + 1] == '\n')
                ++i;
            pushField();
            pushRow();
        }
        else
        {
            fieldBuf.push_back (c);
        }
    }
    if (!fieldBuf.empty () || !row.empty())
    {
        pushField();
        pushRow();
    }
    return grid;
}

bool cellIsData (const juce::String& s, double* out)
{
    // No allocation for the usual already-trimmed cell (trim() then shares
    // the text): this runs once per cell, millions of times for a big CSV.
    const auto t2 = s.trim();
    if (t2.equalsIgnoreCase ("true"))  { if (out) *out = 1.0; return true; }
    if (t2.equalsIgnoreCase ("false")) { if (out) *out = 0.0; return true; }
    // This JUCE build has no createFloatFromString(): validate the characters
    // first, then parse with getDoubleValue().
    double v = 0.0;
    bool ok = false;
    if (!t2.isEmpty())
    {
        bool haveDigit = false;
        for (char16_t c : t2)
        {
            if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')
            {
                haveDigit = haveDigit || (c >= '0' && c <= '9');
                continue;
            }
            break;
        }
        ok = haveDigit && std::isfinite ((float) (v = t2.getDoubleValue ()));
    }
    if (!ok) return false;
    if (out) *out = v;
    return true;
}

static void interpolateColumn (std::vector<double>& vals, const std::vector<bool>& valid)
{
    // Fill each invalid slot from the nearest valid neighbours on both sides
    // (linear); one-sided when a side has no valid value.
    int n = (int) vals.size();
    for (int i = 0; i < n; ++i)
    {
        if (valid[(size_t) i]) continue;
        int lo = i - 1, hi = i + 1;
        while (lo >= 0 && !valid[(size_t) lo]) --lo;
        while (hi < n && !valid[(size_t) hi]) ++hi;
        if (lo >= 0 && hi < n)
            vals[(size_t) i] = vals[(size_t) lo]
                + (vals[(size_t) hi] - vals[(size_t) lo]) * ((double) (i - lo) / (hi - lo));
        else if (lo >= 0)
            vals[(size_t) i] = vals[(size_t) lo];
        else if (hi < n)
            vals[(size_t) i] = vals[(size_t) hi];
    }
}

bool importTable (const juce::String& name, const juce::String& csvText,
                  TableData& out, std::string* errOut, double docDurationSecs,
                  const std::function<bool (double)>& progress)
{
    // Progress: reading the text is the first half, the columns the second.
    bool cancelled = false;
    std::function<bool (double)> parseProgress;
    if (progress)
        parseProgress = [&] (double f) { cancelled = !progress (0.5 * f); return !cancelled; };
    auto grid = parseCsvGrid (csvText, parseProgress);
    if (cancelled)
    {
        if (errOut) *errOut = "cancelled";
        return false;
    }
    if (grid.empty ())
    {
        if (errOut) *errOut = "the CSV text is empty";
        return false;
    }

    // If the first column contains only text, transpose before importing.
    const int R0 = (int) grid.size(), C0 = (int) grid[0].size();
    bool firstColAllText = true;
    for (const auto& row : grid)
        if (!row.empty() && cellIsData (row[0])) { firstColAllText = false; break; }
    if (firstColAllText && R0 > 1 && C0 > 1)
    {
        std::vector<std::vector<juce::String>> tr ((size_t) C0, std::vector<juce::String> ((size_t) R0));
        for (int r = 0; r < R0; ++r)
            for (int c = 0; c < C0 && c < (int) grid[(size_t) r].size(); ++c)
                tr[(size_t) c][(size_t) r] = grid[(size_t) r][(size_t) c];
        grid = std::move (tr);
    }

    const int R = (int) grid.size(), C = (int) grid[0].size();
    if (R < 2 || C < 1)
    {
        if (errOut) *errOut = "the CSV needs a header row and at least one data row";
        return false;
    }

    // A leading row with any non-numeric cell is the header.
    bool hasHeader = false;
    for (int c = 0; c < C; ++c)
        if (!cellIsData (grid[0][(size_t) c])) { hasHeader = true; break; }

    const int dataRows = hasHeader ? R - 1 : R;
    out = TableData {};
    out.name = name;

    // Time-column auto-detection: a first column whose header contains
    // "time" and whose values are strictly increasing positions the rows on
    // the document timeline instead of becoming a parameter. The uniform-
    // grid model is fitted with sampleRate = (rows-1)/span; when the start
    // lies beyond the audio duration, the span is shifted to begin at 0.
    int firstDataCol = 0;
    if (hasHeader && C >= 2 && dataRows >= 2)
    {
        const auto hdr = grid[0][(size_t) 0].trim().toLowerCase();
        if (hdr.contains ("time"))
        {
            std::vector<double> ts ((size_t) dataRows);
            bool allNum = true;
            for (int r = 0; r < dataRows && allNum; ++r)
            {
                const auto& rowRef = grid[(size_t) (r + 1)];
                if (rowRef.empty () || !cellIsData (rowRef[(size_t) 0], &ts[(size_t) r]))
                    allNum = false;
            }
            bool increasing = true;
            for (int r = 1; r < dataRows && increasing; ++r)
                if (!(ts[(size_t) r] > ts[(size_t) (r - 1)])) increasing = false;

            const double tStart = ts[(size_t) 0], tEnd = ts[(size_t) (dataRows - 1)];
            if (allNum && increasing && tEnd > tStart)
            {
                const double shift = (docDurationSecs >= 0.0 && tStart > docDurationSecs)
                                     ? tStart : 0.0;
                out.sampleRate = (double) (dataRows - 1) / (tEnd - tStart);
                out.offsetSecs = tStart - shift;
                firstDataCol = 1;   // the time column is not a parameter
            }
        }
    }

    // Cells -> numbers, row by row: a row's cells sit together in memory,
    // where walking every row once per column jumped across the whole grid
    // for each cell (issue #14: 52k rows x 135 columns). Each row is freed
    // once read, so the text grid and the numbers are not both held whole.
    const int nCols = C - firstDataCol;
    std::vector<std::vector<double>> colVals ((size_t) nCols, std::vector<double> ((size_t) dataRows, 0.0));
    std::vector<std::vector<bool>> colValid ((size_t) nCols, std::vector<bool> ((size_t) dataRows, false));
    std::vector<int> nValid ((size_t) nCols, 0);
    for (int r = 0; r < dataRows; ++r)
    {
        if (progress && (r & 1023) == 0
            && !progress (0.5 + 0.5 * (double) r / (double) dataRows))
        {
            if (errOut) *errOut = "cancelled";
            return false;
        }
        auto& rowRef = grid[(size_t) (hasHeader ? r + 1 : r)];
        // Ragged rows (fewer cells than the first row): missing cells stay
        // empty (invalid) instead of being read out of range.
        const int n = std::min ((int) rowRef.size(), C);
        for (int c = firstDataCol; c < n; ++c)
        {
            double v = 0.0;
            if (cellIsData (rowRef[(size_t) c], &v))
            {
                const auto k = (size_t) (c - firstDataCol);
                colVals[k][(size_t) r] = v;
                colValid[k][(size_t) r] = true;
                ++nValid[k];
            }
        }
        std::vector<juce::String>().swap (rowRef);   // done with it (the header row is never freed: it names the columns)
    }

    for (int c = firstDataCol; c < C; ++c)
    {
        const auto k = (size_t) (c - firstDataCol);
        if (nValid[k] == 0) continue;   // a column with only text is ignored

        interpolateColumn (colVals[k], colValid[k]);

        ParamData p;
        const auto title = hasHeader ? grid[0][(size_t) c].trim() : juce::String();
        p.name = title.isEmpty () ? "Param" + juce::String (c + 1) : title;
        p.values = std::move (colVals[k]);
        // Every parameter gets its own colour within the table; the host
        // (Application::addTableFromCsv) offsets these so that parameters of
        // different tables stay distinct as well.
        p.colorIndex = ((int) out.params.size()) % 12;
        out.params.push_back (std::move (p));
    }

    if (out.params.empty())
    {
        if (errOut) *errOut = "no numeric columns found in the CSV";
        return false;
    }
    return true;
}

void downsampleTable (TableData& t, double newRateHz)
{
    if (!(newRateHz > 0.0)) return;
    const double oldRate = t.sampleRate;

    // Remove-only decimation: keep every Nth row so the resulting density is
    // at most newRateHz rows/second. No interpolation, no added data.
    const int N = (int) std::ceil (oldRate / newRateHz);
    if (N <= 1) return;   // already at or below the requested rate

    for (auto& p : t.params)
    {
        const int nOld = (int) p.values.size();
        if (nOld < 2) continue;
        std::vector<double> out;
        out.reserve ((size_t) ((nOld - 1) / N + 1));
        for (int i = 0; i < nOld; i += N)
            out.push_back (p.values[(size_t) i]);
        p.values = std::move (out);
    }
    t.sampleRate = oldRate / (double) N;   // exact density of the kept rows
}

// ------------------------------------------------- Timeline anchoring --

namespace
{
    // Linear interpolation of a parameter's raw values at table time ttSecs.
    double interpParamValues (const std::vector<double>& v, double sr, double ttSecs)
    {
        if (v.empty ()) return 0.0;
        const double x = std::clamp (ttSecs * sr, 0.0, (double) (v.size() - 1));
        const int i0 = (int) std::floor (x);
        if (i0 >= (int) v.size() - 1) return v.back ();
        const double f = x - i0;
        return v[(size_t) i0] * (1.0 - f) + v[(size_t) (i0 + 1)] * f;
    }

    // Generic warp of one parameter: W maps old doc time -> new doc time,
    // invW the other way (for inserted gaps it returns the bridging old
    // time). The row density in doc seconds is preserved, so the new row
    // count follows the warped domain length.
    template <typename ForwardFn, typename InverseFn>
    void warpParamGeneric (TableData& t, ParamData& p,
                           ForwardFn&& W, InverseFn&& invW)
    {
        if (p.values.size () < 2) return;
        const double sr = t.sampleRate;
        const auto [off0, sc0] = t.effectiveTimeMap (p);
        const double s = off0;
        const double e = off0 + sc0 * p.duration (sr);

        const double ns = W (s), ne = W (e);
        if (!(ne > ns))
        {
            // The whole domain collapsed onto one point: keep two identical
            // rows so the parameter stays drawable.
            const double v = interpParamValues (p.values, sr,
                                                std::clamp ((ns - off0) / sc0, 0.0,
                                                            p.duration (sr)));
            p.values.assign (2, v);
            p.offsetSecs += ns - s;
            return;
        }

        const double spacing = sc0 / sr;   // doc seconds per row
        int nNew = std::max (2, (int) std::lround ((ne - ns) / spacing) + 1);
        std::vector<double> nv;
        nv.reserve ((size_t) nNew);
        for (int j = 0; j < nNew; ++j)
        {
            const double u = ns + spacing * j;          // new doc time of row j
            const double oldT = invW (u);               // old doc time there now
            nv.push_back (interpParamValues (p.values, sr,
                                             std::clamp ((oldT - off0) / sc0, 0.0,
                                                         p.duration (sr))));
        }
        p.values.swap (nv);
        p.offsetSecs += ns - s;   // effective domain now starts at W(s)
    }
}

void warpTableForDeletion (TableData& t, double a, double b)
{
    if (!(b > a)) return;
    for (auto& p : t.params)
        warpParamGeneric (t, p,
            [a, b] (double x) { return x < a ? x : (x >= b ? x - (b - a) : a); },
            [a, b] (double u)  { return u < a ? u : u + (b - a); });
}

void warpTableForInsertion (TableData& t, double a, double lenSecs)
{
    if (!(lenSecs > 0.0)) return;
    for (auto& p : t.params)
        warpParamGeneric (t, p,
            [a, lenSecs] (double x) { return x < a ? x : x + lenSecs; },
            [a, lenSecs] (double u)  { return u < a ? u
                                                   : (u >= a + lenSecs ? u - lenSecs : a); });
}

void warpTableForStretch   (TableData& t, double a, double bOld, double bNew)
{
    if (!(bOld > a)) return;
    const double k = (bNew - a) / (bOld - a);
    for (auto& p : t.params)
        warpParamGeneric (t, p,
            [a, bOld, bNew, k] (double x)
            {
                if (x <= a) return x;
                if (x >= bOld) return x + (bNew - bOld);
                return a + (x - a) * k;
            },
            [a, bOld, bNew, k] (double u)
            {
                if (u <= a) return u;
                if (u < bNew) return a + (u - a) / std::max (1e-30, k);
                return u - (bNew - bOld);
            });
}

// ------------------------------------------------------------- Value maps --

double transformValue (double v, const ParamData& p, double minV, double maxV,
                       int target)
{
    if (p.normalized && maxV > minV)
        v = (v - minV) / (maxV - minV);
    else if (p.normalized)
        v = 0.0;
    if (p.inverted) v = -v;
    if (p.absolute) v = std::abs (v);
    const int t = std::clamp (target, 0, 2);
    return p.valueScale[t] * v + p.valueOffset[t];
}

double fittedOverlayValue (double v, const ParamData& p, double minV, double maxV)
{
    // Fit the raw range into [-1,+1] so every parameter is visible on the
    // waveform/spectrogram regardless of its units; then apply the user's
    // transforms. The per-target scale/offset are NOT applied here.
    if (maxV > minV) v = 2.0 * (v - minV) / (maxV - minV) - 1.0;
    else             v = 0.0;
    if (p.inverted) v = -v;
    if (p.absolute) v = std::abs (v);
    return v;
}

double overlayValue (double v, const ParamData& p, double minV, double maxV,
                     int target)
{
    const int t = std::clamp (target, 0, 2);
    return p.valueScale[t] * fittedOverlayValue (v, p, minV, maxV) + p.valueOffset[t];
}

double dedicatedValue (double v, const ParamData& p)
{
    // The dedicated track plots raw values on its own auto-scaled axis; the
    // user's per-mode vertical map is a plain linear transform of them.
    return p.valueScale[2] * v + p.valueOffset[2];
}

bool solveValueMap (double f1, double f2, double v1, double v2,
                    double& scale, double& offset)
{
    // Two fitted values f1/f2 must land on doc-side values v1/v2:
    //   v1 = scale*f1 + offset
    //   v2 = scale*f2 + offset
    if (std::abs (f1 - f2) <= 1e-9)
        return false;   // degenerate: no unique scale exists
    const double s = (v1 - v2) / (f1 - f2);
    const double o = v1 - s * f1;
    if (!std::isfinite ((float) s) || !std::isfinite ((float) o))
        return false;
    scale  = s;
    offset = o;
    return true;
}

double paramValueAt (const TableData& t, const ParamData& p, double tTableSecs)
{
    const int n = (int) p.values.size();
    if (n == 0 || !(t.sampleRate > 0.0)) return 0.0;
    const double dur = p.duration (t.sampleRate);
    if (tTableSecs < 0.0 || tTableSecs > dur) return 0.0;
    const double pos = tTableSecs * t.sampleRate;
    const int lo = std::min (n - 2, (int) pos);
    const double frac = pos - lo;
    return p.values[(size_t) lo] * (1.0 - frac) + p.values[(size_t) (lo + 1)] * frac;
}

// ------------------------------------------------- Selection-range edits --

bool finiteMinMax (const std::vector<double>& v, double& lo, double& hi)
{
    bool have = false;
    for (double x : v)
        if (std::isfinite (x))
        {
            if (!have) { lo = hi = x; have = true; }
            else       { lo = std::min (lo, x); hi = std::max (hi, x); }
        }
    return have;
}

void overwriteParamRange (TableData& t, int paramIdx, double t0DocSecs,
                          double t1DocSecs, double startVal, double stopVal)
{
    if (paramIdx < 0 || paramIdx >= (int) t.params.size()) return;
    auto& p = t.params[(size_t) paramIdx];
    const double rate = t.sampleRate;
    if (!(rate > 0.0)) return;

    const double a = std::min (t0DocSecs, t1DocSecs);
    const double b = std::max (t0DocSecs, t1DocSecs);
    if (!(b > a)) return;

    const auto [off, sc] = t.effectiveTimeMap (p);
    if (!(sc > 1e-30)) return;

    // Table-time span of the selection under p's effective map.
    const double ta = (a - off) / sc;   // < 0 when the range starts before p
    const double tb = (b - off) / sc;   // beyond the grid end when it overruns

    int N = 0;
    for (const auto& q : t.params)
        N = std::max (N, (int) q.values.size());

    // Rows to insert at the front so the selection start is reachable. After
    // inserting K rows every parameter's data shifts right by K and its
    // offset moves left by sc*K/rate, so all curves keep their doc times.
    // -1e-9 keeps an exact integer span from rounding up to one extra row.
    const int K = ta < 0.0 ? (int) std::ceil (-ta * rate - 1e-9) : 0;

    // The grid must reach the selection end: last row index >= tb + K/rate.
    const double needEnd = tb + (double) K / rate;
    int Nnew = std::max (N + K, (int) std::floor (needEnd * rate) + 1);
    if (Nnew < 2) Nnew = 2;

    // Shift every parameter's data right by K and pad the new cells with NaN.
    for (auto& q : t.params)
    {
        const auto [qoff, qsc] = t.effectiveTimeMap (q);
        std::vector<double> nv ((size_t) Nnew, std::nan (""));
        const int M = (int) q.values.size();
        for (int i = 0; i < M && K + i < Nnew; ++i)
            nv[(size_t) (K + i)] = q.values[(size_t) i];
        q.values.swap (nv);
        if (K > 0)
            q.offsetSecs -= qsc * ((double) K / rate);
    }

    // Fill p's ramp over [a,b] in document time.
    const double offNew = t.effectiveTimeMap (p).first;   // after the shift
    for (int j = 0; j < Nnew; ++j)
    {
        const double tDoc = offNew + sc * ((double) j / rate);
        if (tDoc >= a && tDoc <= b)
        {
            const double f = std::clamp ((tDoc - a) / (b - a), 0.0, 1.0);
            p.values[(size_t) j] = startVal + (stopVal - startVal) * f;
        }
    }
}

void deleteParamRange (TableData& t, int paramIdx, double t0DocSecs, double t1DocSecs)
{
    if (paramIdx < 0 || paramIdx >= (int) t.params.size()) return;
    auto& p = t.params[(size_t) paramIdx];
    const double rate = t.sampleRate;
    if (!(rate > 0.0)) return;

    const double a = std::min (t0DocSecs, t1DocSecs);
    const double b = std::max (t0DocSecs, t1DocSecs);
    if (!(b > a)) return;

    const auto [off, sc] = t.effectiveTimeMap (p);
    if (!(sc > 1e-30)) return;

    for (int j = 0; j < (int) p.values.size(); ++j)
        if (off + sc * ((double) j / rate) >= a && off + sc * ((double) j / rate) <= b)
            p.values[(size_t) j] = std::nan ("");

    auto rowEmpty = [&t] (int j)
    {
        for (const auto& q : t.params)
            if ((size_t) j < q.values.size() && std::isfinite (q.values[(size_t) j]))
                return false;
        return true;
    };

    // Trim leading rows that are entirely empty for every parameter.
    int maxRows = 0;
    for (const auto& q : t.params)
        maxRows = std::max (maxRows, (int) q.values.size());
    int front = 0;
    while (front < maxRows && rowEmpty (front)) ++front;

    if (front > 0)
        for (auto& q : t.params)
        {
            const auto [qoff, qsc] = t.effectiveTimeMap (q);
            const int drop = std::min (front, (int) q.values.size());
            q.values.erase (q.values.begin (), q.values.begin () + drop);
            if (drop > 0)
                q.offsetSecs += qsc * ((double) drop / rate);   // keep doc times
        }

    // Trim trailing rows that are entirely empty for every parameter.
    while (true)
    {
        bool anyRows = false;
        for (const auto& q : t.params)
            if (!q.values.empty()) { anyRows = true; break; }
        if (!anyRows) break;

        bool allEmpty = true;
        for (const auto& q : t.params)
            if (!q.values.empty() && std::isfinite (q.values.back()))
                { allEmpty = false; break; }
        if (!allEmpty) break;

        for (auto& q : t.params)
            if (!q.values.empty()) q.values.pop_back ();   // indices unchanged
    }
}

bool firstValidValue (const TableData& t, const ParamData& p, double t0Secs,
                      double t1Secs, double& out)
{
    const auto [off, sc] = t.effectiveTimeMap (p);
    if (!(sc > 1e-30)) return false;
    const int N = (int) p.values.size();
    if (N == 0) return false;

    const double a = std::min (t0Secs, t1Secs), b = std::max (t0Secs, t1Secs);
    const double rate = t.sampleRate;
    // First row whose doc time is >= a / last one <= b.
    int jLo = (int) std::ceil (((a - off) / sc) * rate - 1e-9);
    int jHi = (int) std::floor (((b - off) / sc) * rate + 1e-9);

    for (int j = std::max (0, jLo); j <= std::min (N - 1, jHi); ++j)
        if (std::isfinite (p.values[(size_t) j])) { out = p.values[(size_t) j]; return true; }
    // Outward: the nearest finite value before the range.
    for (int j = std::max (-1, std::min (N - 1, jLo) - 1); j >= 0; --j)
        if (std::isfinite (p.values[(size_t) j])) { out = p.values[(size_t) j]; return true; }
    return false;
}

bool lastValidValue (const TableData& t, const ParamData& p, double t0Secs,
                    double t1Secs, double& out)
{
    const auto [off, sc] = t.effectiveTimeMap (p);
    if (!(sc > 1e-30)) return false;
    const int N = (int) p.values.size();
    if (N == 0) return false;

    const double a = std::min (t0Secs, t1Secs), b = std::max (t0Secs, t1Secs);
    const double rate = t.sampleRate;
    int jLo = (int) std::ceil (((a - off) / sc) * rate - 1e-9);
    int jHi = (int) std::floor (((b - off) / sc) * rate + 1e-9);

    for (int j = std::min (N - 1, jHi); j >= std::max (0, jLo); --j)
        if (std::isfinite (p.values[(size_t) j])) { out = p.values[(size_t) j]; return true; }
    // Outward: the nearest finite value after the range.
    for (int j = std::min (N - 1, std::max (0, jHi) + 1); j < N; ++j)
        if (std::isfinite (p.values[(size_t) j])) { out = p.values[(size_t) j]; return true; }
    return false;
}

// ------------------------------------------------------------- Auto align --

namespace {

// Resamples a parameter onto a uniform doc-time grid spanning
// [tMinSecs, tMinSecs + spanSecs], honouring its effective time map
// (docTime = off + sc*tTable). Points outside the parameter's own duration
// become NaN so they never enter the correlation sums (a flat zero tail
// would bias the peak).
std::vector<double> nccResampleDoc (const TableData& t, const ParamData& p,
                                    double offSecs, double scaleTime,
                                    double tMinSecs, double spanSecs, int nPts)
{
    std::vector<double> v ((size_t) nPts);
    const double dur = p.duration (t.sampleRate);
    for (int i = 0; i < nPts; ++i)
    {
        const double tt = tMinSecs + spanSecs * (double) i / (double) (nPts - 1);
        const double tTab = (tt - offSecs) / scaleTime;
        if (tTab < 0.0 || tTab > dur)
        {
            v[(size_t) i] = std::nan ("");
            continue;
        }
        const double val = paramValueAt (t, p, tTab);
        v[(size_t) i] = std::isfinite (val) ? val : std::nan ("");
    }
    return v;
}

// Pearson correlation between A and B shifted by L samples (positive L means
// B's content sits later than A's), computed over the overlap only. Returns
// NaN when the overlap is too small or one side is constant there.
double nccAtLag (const std::vector<double>& A, const std::vector<double>& B,
                 int L, int minOverlap)
{
    const int M = (int) A.size();
    const int i0 = std::max (0, -L);
    const int i1 = std::min (M, M - L);   // exclusive upper bound
    double sa = 0.0, sb = 0.0, sxy = 0.0, sxx = 0.0, syy = 0.0;
    int n = 0;
    for (int i = i0; i < i1; ++i)
    {
        const double a = A[(size_t) i];
        const double b = B[(size_t) (i + L)];
        if (!std::isfinite (a) || !std::isfinite (b)) continue;
        sa += a; sb += b; sxy += a * b; sxx += a * a; syy += b * b; ++n;
    }
    if (n < minOverlap) return std::nan ("");

    const double cov = n * sxy - sa * sb;
    const double den = std::sqrt ((n * sxx - sa * sa) * (n * syy - sb * sb));
    if (!(den > 1e-30)) return std::nan ("");   // constant signal: no timing info

    return cov / den;
}

// The lag whose |Pearson r| is maximal over the full search range.
int nccBestLag (const std::vector<double>& A, const std::vector<double>& B,
                int minOverlap)
{
    const int M = (int) A.size();
    double bestAbs = -1.0, bestSigned = 0.0;
    int bestLag = 0;
    for (int L = -(M - 1); L <= M - 1; ++L)
    {
        const double r = nccAtLag (A, B, L, minOverlap);
        if (!std::isfinite (r)) continue;
        const double absR = std::abs (r);
        // Prefer the stronger |r|; break exact ties toward positive r.
        if (absR > bestAbs + 1e-9 || (std::abs (absR - bestAbs) <= 1e-9 && r > bestSigned))
        {
            bestAbs = absR;
            bestSigned = r;
            bestLag = L;
        }
    }
    return bestLag;   // 0 when no lag had enough overlap
}

} // namespace

double autoAlignLag (const TableData& refTable, const ParamData& refParam,
                     const TableData& aliTable, const ParamData& aliParam)
{
    // Normalized cross-correlation in DOCUMENT time: both parameters are
    // resampled through their effective maps (docTime = off + sc*tTable) onto
    // a common grid spanning the union of their doc-time ranges. Correlating
    // raw table times instead would ignore existing offsets/stretches, so a
    // second alignment pass would rediscover the same lag and shift again.
    // For every candidate shift the Pearson correlation is computed over the
    // overlap of both parameters only, so amplitude differences and unequal
    // durations cannot bias the peak. Two stages: a coarse pass over the full
    // lag range on a grid spanning the union of both ranges, then an 8x finer
    // refinement around the best coarse lag. Result in doc seconds; adding it
    // to B's effective offset shifts its content onto A's (a second run then
    // finds ~0 and leaves everything untouched).
    const double durA = refParam.duration (refTable.sampleRate);
    const double durB = aliParam.duration (aliTable.sampleRate);
    if (!(std::max (durA, durB) > 0.0)) return 0.0;

    const auto [offA, scA] = refTable.effectiveTimeMap (refParam);
    const auto [offB, scB] = aliTable.effectiveTimeMap (aliParam);
    if (!(scA > 1e-9) || !(scB > 1e-9)) return 0.0;

    const double tMin = std::min (offA, offB);
    const double span = std::max (offA + scA * durA, offB + scB * durB) - tMin;
    if (!(span > 0.0)) return 0.0;

    const int M1 = std::clamp ((int) std::lround (span * 256.0), 256, 8192);
    auto A1 = nccResampleDoc (refTable, refParam, offA, scA, tMin, span, M1);
    auto B1 = nccResampleDoc (aliTable, aliParam, offB, scB, tMin, span, M1);

    const int lagCoarse = nccBestLag (A1, B1, std::max (8, M1 / 20));

    // Refine on an 8x finer grid around the coarse winner.
    const int R = 8;
    const int M2 = std::min (65536, M1 * R);
    auto A2 = nccResampleDoc (refTable, refParam, offA, scA, tMin, span, M2);
    auto B2 = nccResampleDoc (aliTable, aliParam, offB, scB, tMin, span, M2);

    const double finePerCoarse = (double) (M2 - 1) / (double) (M1 - 1);
    const int centre = (int) std::lround (lagCoarse * finePerCoarse);
    const int minOverlap2 = std::max (8, M2 / 20);

    double bestAbs = -1.0, bestSigned = 0.0;
    int bestFineLag = centre;   // fallback: the coarse estimate itself
    for (int L = centre - R; L <= centre + R; ++L)
    {
        const double r = nccAtLag (A2, B2, L, minOverlap2);
        if (!std::isfinite (r)) continue;
        const double absR = std::abs (r);
        if (absR > bestAbs + 1e-9 || (std::abs (absR - bestAbs) <= 1e-9 && r > bestSigned))
        {
            bestAbs = absR;
            bestSigned = r;
            bestFineLag = L;
        }
    }

    // A positive lag means B's content sits later than A's, so shifting B
    // earlier by that many seconds aligns it onto A.
    return -((double) bestFineLag * span / (double) (M2 - 1));
}

// ------------------------------------------------------------- Persistence --

juce::var serialiseTables (const std::vector<TableData>& tables, int mode)
{
    auto root = new juce::DynamicObject();
    juce::Array<juce::var> arr;
    for (const auto& t : tables)
    {
        auto to = new juce::DynamicObject();
        to->setProperty ("name", t.name);
        if (!t.visible)      to->setProperty ("hidden", 1);
        // Tables carry no colour of their own - only the parameters do.
        to->setProperty ("rate", t.sampleRate);
        to->setProperty ("offset", t.offsetSecs);
        to->setProperty ("scale", t.scaleTime);

        juce::Array<juce::var> parr;
        for (const auto& p : t.params)
        {
            auto po = new juce::DynamicObject();
            po->setProperty ("name", p.name);
            if (!p.visible)      po->setProperty ("hidden", 1);
            po->setProperty ("colour", p.colorIndex % 12);
            po->setProperty ("target", p.alignTarget);
            po->setProperty ("offset", p.offsetSecs);
            po->setProperty ("scale", p.scaleTime);
            // Per-display-mode vertical maps: 0=Waveform, 1=Spectrogram,
            // 2=Dedicated track.
            for (int i = 0; i < 3; ++i)
            {
                po->setProperty ("voff" + juce::String (i), p.valueOffset[i]);
                po->setProperty ("vscl" + juce::String (i), p.valueScale[i]);
            }
            if (p.normalized)    po->setProperty ("norm", 1);
            if (p.inverted)      po->setProperty ("inv", 1);
            if (p.absolute)      po->setProperty ("abs", 1);

            juce::Array<juce::var> vals;
            for (double v : p.values)
                // null marks an empty cell (NaN); plain numbers are data.
                vals.add (std::isfinite (v) ? juce::var (v) : juce::var());
            po->setProperty ("values", vals);
            parr.add (po);
        }
        to->setProperty ("params", parr);
        arr.add (to);
    }
    root->setProperty ("tables", arr);
    root->setProperty ("mode", mode);
    return root;
}

bool parseTables (const juce::var& v, std::vector<TableData>& out, int* mode)
{
    const auto* obj = v.getDynamicObject();
    if (obj == nullptr || !obj->getProperties().contains ("tables"))
        return false;

    const juce::var tv = obj->getProperty ("tables");
    const auto* tarr = tv.isArray() ? tv.getArray() : nullptr;
    if (tarr == nullptr) return false;

    std::vector<TableData> parsed;
    for (const auto& item : *tarr)
    {
        const auto* to = item.getDynamicObject();
        if (to == nullptr || !to->getProperties().contains ("params")) continue;

        TableData t;
        const auto& tp = to->getProperties();
        // This JUCE build's var has no getIntValue/getDoubleValue: use the
        // implicit conversion operators instead.
        t.name       = tp.getWithDefault ("name", "").toString();
        t.visible    = static_cast<int> (tp.getWithDefault ("hidden", 0)) == 0;
        // Older metadata may carry a table-level "colour": it is ignored now.
        t.sampleRate = std::max (1e-9, static_cast<double> (tp.getWithDefault ("rate", 1000.0)));
        t.offsetSecs = static_cast<double> (tp.getWithDefault ("offset", 0.0));
        t.scaleTime  = std::max (1e-9, static_cast<double> (tp.getWithDefault ("scale", 1.0)));

        const juce::var paramsVar = tp.getWithDefault ("params", juce::var());
        const auto* parr = paramsVar.isArray() ? paramsVar.getArray() : nullptr;
        if (parr == nullptr) continue;

        for (const auto& pv : *parr)
        {
            const auto* po = pv.getDynamicObject();
            if (po == nullptr || !po->getProperties().contains ("values")) continue;

            ParamData p;
            const auto& pp = po->getProperties();
            p.name       = pp.getWithDefault ("name", "").toString();
            p.visible    = static_cast<int> (pp.getWithDefault ("hidden", 0)) == 0;
            p.colorIndex = std::max (0, static_cast<int> (pp.getWithDefault ("colour", 0))) % 12;
            p.alignTarget = std::clamp (static_cast<int> (pp.getWithDefault ("target", 0)), 0, 2);
            p.offsetSecs  = static_cast<double> (pp.getWithDefault ("offset", 0.0));
            p.scaleTime   = std::max (1e-9, static_cast<double> (pp.getWithDefault ("scale", 1.0)));
            // Per-display-mode vertical maps; older metadata carries a single
            // shared pair under "voff"/"vscale" - apply it to every mode.
            const bool havePerTarget = pp.contains ("voff0") || pp.contains ("vscl0");
            for (int i = 0; i < 3; ++i)
            {
                if (havePerTarget)
                {
                    p.valueOffset[i] = static_cast<double> (pp.getWithDefault ("voff" + juce::String (i), 0.0));
                    p.valueScale[i]  = std::max (1e-9, static_cast<double> (pp.getWithDefault ("vscl" + juce::String (i), 1.0)));
                }
                else
                {
                    p.valueOffset[i] = static_cast<double> (pp.getWithDefault ("voff", 0.0));
                    p.valueScale[i]  = std::max (1e-9, static_cast<double> (pp.getWithDefault ("vscale", 1.0)));
                }
            }
            p.normalized  = static_cast<int> (pp.getWithDefault ("norm", 0)) != 0;
            p.inverted    = static_cast<int> (pp.getWithDefault ("inv", 0)) != 0;
            p.absolute    = static_cast<int> (pp.getWithDefault ("abs", 0)) != 0;

            const juce::var vv = pp.getWithDefault ("values", juce::var());
            const auto* vals = vv.isArray() ? vv.getArray() : nullptr;
            if (vals == nullptr || vals->isEmpty()) continue;
            p.values.reserve ((size_t) vals->size());
            for (const auto& vv : *vals)
                // null cells are empty values (NaN), never zero.
                p.values.push_back (vv.isVoid () ? std::nan ("")
                                                 : static_cast<double> (vv));
            t.params.push_back (std::move (p));
        }
        if (!t.params.empty())
            parsed.push_back (std::move (t));
    }

    out = std::move (parsed);
    if (mode != nullptr)
        *mode = std::clamp (static_cast<int> (obj->getProperties().getWithDefault ("mode", 0)), 0, 2);
    return true;
}

} // namespace sc::tables
