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

// ------------------------------------------------------------------ Import --

// NOTE: this JUCE build has no String::operator+=(char16_t) - on MSVC a
// char16_t would silently bind to the int overload and append the character's
// numeric code. Accumulate raw UTF-16 units instead and convert once per field.
std::vector<std::vector<juce::String>> parseCsvGrid (const juce::String& text)
{
    std::vector<std::vector<juce::String>> grid;
    std::vector<juce::String> row;
    // CharPointer_UTF16's CharType is wchar_t on Windows (distinct from
    // uint16/char16_t), so accumulate in wchar_t units.
    std::vector<wchar_t> fieldBuf;
    bool inQuotes = false;

    auto pushField = [&] ()
    {
        if (!fieldBuf.empty ())
            row.push_back (juce::String (juce::CharPointer_UTF16 (fieldBuf.data()),
                                         (size_t) fieldBuf.size()).trim());
        else
            row.push_back (juce::String());
        fieldBuf.clear();
    };
    // NOTE: a blank line yields one empty cell and is kept as a data gap
    // (importTable interpolates across it). Ragged rows are tolerated by the
    // importer, which treats missing cells as empty.
    auto pushRow   = [&] () { if (!row.empty ()) grid.push_back (std::move (row)); row.clear(); };

    for (size_t i = 0; i < text.length(); ++i)
    {
        const char16_t c = text[(int) i];
        if (inQuotes)
        {
            if (c == '"')
            {
                // "" inside a quoted field is an escaped quote.
                if (i + 1 < text.length() && text[(int) i + 1] == '"')
                    ++i;   // keep one literal quote, skip its twin
                else
                {
                    inQuotes = false;
                    continue;   // closing quote: not part of the field
                }
            }
            fieldBuf.push_back ((wchar_t) c);
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
            if (c == '\r' && i + 1 < text.length() && text[(int) i + 1] == '\n')
                ++i;
            pushField();
            pushRow();
        }
        else
        {
            fieldBuf.push_back ((wchar_t) c);
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
    const auto t = s.trim().toLowerCase();
    if (t == "true")  { if (out) *out = 1.0; return true; }
    if (t == "false") { if (out) *out = 0.0; return true; }
    // This JUCE build has no createFloatFromString(): validate the characters
    // first, then parse with getDoubleValue().
    const auto t2 = s.trim();
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
                  TableData& out, std::string* errOut, double docDurationSecs)
{
    auto grid = parseCsvGrid (csvText);
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

    for (int c = firstDataCol; c < C; ++c)
    {
        std::vector<double> vals ((size_t) dataRows);
        std::vector<bool> valid ((size_t) dataRows, false);
        int nValid = 0;
        for (int r = 0; r < dataRows; ++r)
        {
            const auto& rowRef = grid[(size_t) (hasHeader ? r + 1 : r)];
            // Ragged rows (fewer cells than the first row): treat missing
            // cells as empty instead of reading out of range.
            if ((int) rowRef.size() <= c)
                continue;
            const auto& cell = rowRef[(size_t) c];
            double v = 0.0;
            if (cellIsData (cell, &v))
            {
                vals[(size_t) r] = v;
                valid[(size_t) r] = true;
                ++nValid;
            }
        }
        if (nValid == 0) continue;   // a column with only text is ignored

        interpolateColumn (vals, valid);

        ParamData p;
        const auto title = hasHeader ? grid[0][(size_t) c].trim() : juce::String();
        p.name = title.isEmpty () ? "Param" + juce::String (c + 1) : title;
        p.values = std::move (vals);
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

double transformValue (double v, const ParamData& p, double minV, double maxV)
{
    if (p.normalized && maxV > minV)
        v = (v - minV) / (maxV - minV);
    else if (p.normalized)
        v = 0.0;
    if (p.inverted) v = -v;
    if (p.absolute) v = std::abs (v);
    return p.valueScale * v + p.valueOffset;
}

double overlayValue (double v, const ParamData& p, double minV, double maxV)
{
    // Fit the raw range into [-1,+1] so every parameter is visible on the
    // waveform/spectrogram regardless of its units; then apply the user's
    // adjustments on top of the fitted curve.
    if (maxV > minV) v = 2.0 * (v - minV) / (maxV - minV) - 1.0;
    else             v = 0.0;
    if (p.inverted) v = -v;
    if (p.absolute) v = std::abs (v);
    return p.valueScale * v + p.valueOffset;
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

// ------------------------------------------------------------- Auto align --

static std::vector<double> resampleToGrid (const TableData& t, const ParamData& p,
                                           double durSecs, int nPts)
{
    std::vector<double> out ((size_t) nPts);
    for (int i = 0; i < nPts; ++i)
        out[(size_t) i] = paramValueAt (t, p, (double) i * durSecs / (nPts - 1));
    return out;
}

double autoAlignLag (const TableData& refTable, const ParamData& refParam,
                     const TableData& aliTable, const ParamData& aliParam)
{
    const double durA = refParam.duration (refTable.sampleRate);
    const double durB = aliParam.duration (aliTable.sampleRate);
    const double dur  = std::min (durA, durB);
    if (!(dur > 0.0)) return 0.0;

    // Common grid: at most 4096 points so the O(M^2) correlation stays cheap.
    const int M = std::max (16, std::min (4096, (int) std::lround (dur * 512.0)));
    auto A = resampleToGrid (refTable, refParam, dur, M);
    auto B = resampleToGrid (aliTable, aliParam, dur, M);

    // Mean-subtract both so the correlation is level-independent.
    double mA = 0.0, mB = 0.0;
    for (auto v : A) mA += v;
    for (auto v : B) mB += v;
    mA /= M; mB /= M;
    for (auto& v : A) v -= mA;
    for (auto& v : B) v -= mB;

    const int maxLag = M / 2;
    double bestScore = -1e300, bestLag = 0.0;
    for (int L = -maxLag; L <= maxLag; ++L)
    {
        double s = 0.0;
        if (L >= 0)
            for (int i = 0; i + L < M; ++i) s += A[(size_t) i] * B[(size_t) (i + L)];
        else
            for (int i = -L; i < M; ++i)     s += A[(size_t) i] * B[(size_t) (i + L)];
        if (s > bestScore) { bestScore = s; bestLag = (double) L; }
    }

    // c(L) peaks at the lag where B's content sits L samples AFTER A's, so
    // shifting B earlier by that many seconds aligns it onto A.
    return -bestLag * dur / (M - 1);
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
            po->setProperty ("voff", p.valueOffset);
            po->setProperty ("vscale", p.valueScale);
            if (p.normalized)    po->setProperty ("norm", 1);
            if (p.inverted)      po->setProperty ("inv", 1);
            if (p.absolute)      po->setProperty ("abs", 1);

            juce::Array<juce::var> vals;
            for (double v : p.values) vals.add (v);
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
            p.valueOffset = static_cast<double> (pp.getWithDefault ("voff", 0.0));
            p.valueScale  = static_cast<double> (pp.getWithDefault ("vscale", 1.0));
            p.normalized  = static_cast<int> (pp.getWithDefault ("norm", 0)) != 0;
            p.inverted    = static_cast<int> (pp.getWithDefault ("inv", 0)) != 0;
            p.absolute    = static_cast<int> (pp.getWithDefault ("abs", 0)) != 0;

            const juce::var vv = pp.getWithDefault ("values", juce::var());
            const auto* vals = vv.isArray() ? vv.getArray() : nullptr;
            if (vals == nullptr || vals->isEmpty()) continue;
            p.values.reserve ((size_t) vals->size());
            for (const auto& vv : *vals)
                p.values.push_back (static_cast<double> (vv));
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
