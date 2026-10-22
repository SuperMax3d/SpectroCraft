// SpectroCraft - named spectral selections: JSON sidecar persistence.
#include "SavedSelections.h"
#include <cmath>

juce::File savedSelectionsSidecarPath (const juce::File& audioFile)
{
    return juce::File (audioFile.getFullPathName() + ".selections.json");
}

static juce::String jsonEscape (const juce::String& s)
{
    juce::String out;
    for (int i = 0; i < s.length(); ++i)
    {
        const char16_t c = s[i];
        switch (c)
        {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n";  break;
            case '\r': out << "\\r";  break;
            case '\t': out << "\\t";  break;
            // Cast to juce_wchar: on MSVC char16_t is a distinct type and the
            // unqualified `out << c` would pick the numeric overload.
            default:   out << static_cast<juce_wchar> (c); break;
        }
    }
    return out;
}

static juce::String num (double v)
{
    // Fixed precision keeps the files compact and stable across platforms.
    if (!std::isfinite (v)) return "0";
    return juce::String (v, 9);
}

juce::String serialiseSavedSelections (const std::vector<SavedSpectralSelection>& list)
{
    juce::String s = "{ \"version\": 1, \"selections\": [";
    for (size_t i = 0; i < list.size(); ++i)
    {
        const auto& sel = list[i];
        if (i > 0) s += ",";
        s += "\n { \"name\": \"" + jsonEscape (sel.name) + "\", \"shapes\": [";

        bool firstShape = true;
        for (const auto& sh : sel.data.shapes())
        {
            if (!firstShape) s += ",";
            firstShape = false;
            s += "\n  { \"kind\": \"";
            switch (sh.kind)
            {
                case SpectralShape::Kind::Rect:    s += "rect";   break;
                case SpectralShape::Kind::Polygon: s += "poly";   break;
                default:                           s += "stroke"; break;
            }
            s += "\", \"sub\": " + juce::String (sh.subtract ? 1 : 0);

            switch (sh.kind)
            {
                case SpectralShape::Kind::Rect:
                    s += ", \"t0\": " + num (sh.t0)
                       + ", \"t1\": " + num (sh.t1)
                       + ", \"f0\": " + num (sh.f0)
                       + ", \"f1\": " + num (sh.f1);
                    break;
                case SpectralShape::Kind::Polygon:
                    s += ", \"pts\": [";
                    for (size_t p = 0; p < sh.pts.size(); ++p)
                    {
                        if (p > 0) s += ",";
                        s += num (sh.pts[p].first);
                        s += ",";
                        s += num (sh.pts[p].second);
                    }
                    s += "]";
                    break;
                default:   // Stroke: re-stamped from pts at load time.
                    s += ", \"radius\": " + num (std::max (1.0, sh.strokeRadiusHz));
                    s += ", \"pts\": [";
                    for (size_t p = 0; p < sh.pts.size(); ++p)
                    {
                        if (p > 0) s += ",";
                        s += num (sh.pts[p].first);
                        s += ",";
                        s += num (sh.pts[p].second);
                    }
                    s += "]";
                    break;
            }
            s += " }";
        }
        s += "\n ]}";
    }
    if (!list.empty())
        s += "\n";
    s += "]}\n";
    return s;
}

// ---------------------------------------------------------------------------
// Parsing helpers (same hand-rolled style as the marker sidecar parser).

/** Index of the bracket/brace matching `openIdx`, skipping quoted strings.
    Returns -1 when unbalanced. */
static int findMatching (const juce::String& s, size_t openIdx, char open, char close)
{
    int depth = 0;
    bool inStr = false;
    for (size_t i = openIdx; i < (size_t) s.length(); ++i)
    {
        const char c = s[(int) i];
        if (inStr)
        {
            if (c == '\\') { ++i; continue; }
            if (c == '"') inStr = false;
            continue;
        }
        if (c == '"')      { inStr = true;  continue; }
        if (c == open)     ++depth;
        else if (c == close)
        {
            --depth;
            if (depth == 0) return (int) i;
        }
    }
    return -1;
}

/** Extracts the number following `key` inside [objStart, objEnd). */
static bool findKeyNum (const juce::String& s, size_t objStart, size_t objEnd,
                        const juce::String& key, double& value)
{
    const auto at = s.indexOf ((int) objStart, key);
    if (at < 0 || (size_t) at >= objEnd)
        return false;
    size_t pos = (size_t) at + (size_t) key.length();
    while (pos < objEnd && (s[(int) pos] == ':' || s[(int) pos] == ' '))
        ++pos;
    const auto start = pos;
    while (pos < objEnd && (s[(int) pos] == '-' || s[(int) pos] == '.'
                            || s[(int) pos] == 'e' || s[(int) pos] == 'E'
                            || (s[(int) pos] >= '0' && s[(int) pos] <= '9')))
        ++pos;
    value = s.substring ((int) start, (int) pos).getDoubleValue();
    return true;
}

/** Reverses jsonEscape() on an extracted string body. */
static juce::String jsonUnescape (const juce::String& s)
{
    juce::String out;
    for (int i = 0; i < s.length(); ++i)
    {
        const char16_t c = s[i];
        if (c == '\\' && i + 1 < s.length())
        {
            ++i;
            switch (s[i])
            {
                case '"':  out << (juce_wchar) '"';  break;
                case '\\': out << (juce_wchar) '\\'; break;
                case 'n':  out << (juce_wchar) '\n'; break;
                case 'r':  out << (juce_wchar) '\r'; break;
                case 't':  out << (juce_wchar) '\t'; break;
                default:   out << static_cast<juce_wchar> (s[i]); break;
            }
        }
        else
            out << static_cast<juce_wchar> (c);
    }
    return out;
}

/** Extracts the string following `key` inside [objStart, objEnd).
    Escaped quotes inside the value are handled and unescaped. */
static bool findKeyStr (const juce::String& s, size_t objStart, size_t objEnd,
                        const juce::String& key, juce::String& value)
{
    const auto at = s.indexOf ((int) objStart, key);
    if (at < 0 || (size_t) at >= objEnd)
        return false;
    size_t q = (size_t) at + (size_t) key.length();
    while (q < objEnd && s[(int) q] != '"') ++q;
    if (q >= objEnd) return false;
    ++q;   // opening quote
    int qEnd = -1;
    for (size_t i = q; i < objEnd; ++i)
    {
        if (s[(int) i] == '\\') { ++i; continue; }
        if (s[(int) i] == '"')  { qEnd = (int) i; break; }
    }
    if (qEnd < 0 || (size_t) qEnd >= objEnd)
        return false;
    // substring() takes an END index in this JUCE build.
    const juce::String raw = s.substring ((int) q, qEnd);
    value = jsonUnescape (raw);
    return true;
}

/** Parses a flat JSON number array "[a,b,c,...]" starting at `openIdx`. */
static bool parseNumArray (const juce::String& s, size_t openIdx, std::vector<double>& out)
{
    const int close = findMatching (s, openIdx, '[', ']');
    if (close < 0 || (size_t) close > (size_t) s.length()) return false;
    const juce::String inner = s.substring ((int) openIdx + 1, close);
    out.clear();
    size_t pos = 0;
    while (pos < (size_t) inner.length())
    {
        // skip separators/whitespace
        while (pos < inner.length() && (inner[(int) pos] == ',' || inner[(int) pos] == ' ')) ++pos;
        if (pos >= inner.length()) break;
        const auto start = pos;
        while (pos < inner.length() && (inner[(int) pos] == '-' || inner[(int) pos] == '.'
                                        || inner[(int) pos] == 'e' || inner[(int) pos] == 'E'
                                        || (inner[(int) pos] >= '0' && inner[(int) pos] <= '9')))
            ++pos;
        if (pos == start) return false;   // not a number -> malformed
        out.push_back (inner.substring ((int) start, (int) pos).getDoubleValue());
    }
    return true;
}

bool deserialiseSavedSelections (const juce::String& json,
                                 std::vector<SavedSpectralSelection>& out)
{
    out.clear();
    if (json.isEmpty())
        return false;

    // Locate the "selections" array.
    const auto selKey = json.indexOf ("\"selections\"");
    if (selKey < 0)
        return false;
    size_t arrOpen = (size_t) selKey + 14;
    while (arrOpen < (size_t) json.length() && json[(int) arrOpen] != '[') ++arrOpen;
    const int arrClose = findMatching (json, arrOpen, '[', ']');
    if (arrClose < 0)
        return false;

    // Iterate the selection objects inside it.
    size_t pos = arrOpen + 1;
    while (pos < (size_t) arrClose)
    {
        const auto open = json.indexOfChar ((int) pos, '{');
        if (open < 0 || (size_t) open >= (size_t) arrClose) break;
        const int close = findMatching (json, open, '{', '}');
        if (close < 0) return false;
        const size_t objStart = (size_t) open, objEnd = (size_t) close;

        SavedSpectralSelection sel;
        if (!findKeyStr (json, objStart, objEnd, "\"name\"", sel.name))
            return false;

        // Locate the "shapes" array inside this selection object.
        const auto shpKey = json.indexOf ((int) objStart, "\"shapes\"");
        size_t shpOpen = (size_t) shpKey + 10;
        while (shpOpen < objEnd && json[(int) shpOpen] != '[') ++shpOpen;
        const int shpClose = findMatching (json, shpOpen, '[', ']');
        if (shpKey < 0 || shpClose < 0 || (size_t) shpClose > objEnd)
            return false;

        // Iterate the shape objects.
        size_t sp = (size_t) shpOpen + 1;
        while (sp < (size_t) shpClose)
        {
            const auto so = json.indexOfChar ((int) sp, '{');
            if (so < 0 || (size_t) so >= (size_t) shpClose) break;
            const int sc = findMatching (json, so, '{', '}');
            if (sc < 0) return false;
            const size_t sStart = (size_t) so, sEnd = (size_t) sc;

            juce::String kind;
            double subV = 0.0;
            if (!findKeyStr (json, sStart, sEnd, "\"kind\"", kind)) return false;
            findKeyNum (json, sStart, sEnd, "\"sub\"", subV);
            const bool subtract = subV != 0.0;

            if (kind == "rect")
            {
                double t0 = 0, t1 = 0, f0 = 0, f1 = 0;
                if (!findKeyNum (json, sStart, sEnd, "\"t0\"", t0)
                    || !findKeyNum (json, sStart, sEnd, "\"t1\"", t1)
                    || !findKeyNum (json, sStart, sEnd, "\"f0\"", f0)
                    || !findKeyNum (json, sStart, sEnd, "\"f1\"", f1))
                    return false;
                sel.data.addRect (t0, t1, f0, f1, subtract, false);
            }
            else if (kind == "poly")
            {
                const auto ptsKey = json.indexOf ((int) sStart, "\"pts\"");
                if (ptsKey < 0 || (size_t) ptsKey >= sEnd) return false;
                size_t po = (size_t) ptsKey + 7;
                while (po < sEnd && json[(int) po] != '[') ++po;
                std::vector<double> flat;
                if (!parseNumArray (json, po, flat) || flat.size() < 6 || flat.size() % 2 != 0)
                    return false;
                std::vector<std::pair<double, double>> pts;
                for (size_t i = 0; i + 1 < flat.size(); i += 2)
                    pts.emplace_back (flat[i], flat[i + 1]);
                sel.data.addPolygon (pts, subtract, false);
            }
            else if (kind == "stroke")
            {
                double radius = 0.0;
                const auto ptsKey = json.indexOf ((int) sStart, "\"pts\"");
                if (ptsKey < 0 || (size_t) ptsKey >= sEnd) return false;
                size_t po = (size_t) ptsKey + 7;
                while (po < sEnd && json[(int) po] != '[') ++po;
                std::vector<double> flat;
                if (!findKeyNum (json, sStart, sEnd, "\"radius\"", radius)
                    || !parseNumArray (json, po, flat) || flat.size() < 2 || flat.size() % 2 != 0)
                    return false;
                std::vector<std::pair<double, double>> pts;
                for (size_t i = 0; i + 1 < flat.size(); i += 2)
                    pts.emplace_back (flat[i], flat[i + 1]);
                sel.data.addStroke (pts, radius, subtract, false);
            }
            else
                return false;   // unknown shape kind

            sp = sc + 1;
        }

        out.push_back (std::move (sel));
        pos = close + 1;
    }

    return true;
}
