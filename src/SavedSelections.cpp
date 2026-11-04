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
        s += "\n { \"name\": \"" + jsonEscape (sel.name) + "\""
           + (sel.comment.isEmpty () ? juce::String() : juce::String(", \"comment\": \"")
                                      + jsonEscape (sel.comment) + "\"")
           + ", \"shapes\": [";

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
// Parsing. juce::JSON runs in linear time and decodes every escape; the former
// hand-rolled scanner indexed juce::String with operator[], which is O(index)
// on this build's UTF-8 strings (quadratic on files with long strokes).

bool deserialiseSavedSelections (const juce::String& json,
                                 std::vector<SavedSpectralSelection>& out)
{
    out.clear();
    if (json.isEmpty())
        return false;

    const juce::var root = juce::JSON::parse (json);
    const auto* rootObj = root.getDynamicObject();
    if (rootObj == nullptr)
        return false;
    const juce::var selVar = rootObj->getProperties().getWithDefault ("selections", juce::var());
    const auto* sels = selVar.isArray() ? selVar.getArray() : nullptr;
    if (sels == nullptr)
        return false;

    auto isNum = [] (const juce::var& v) { return v.isDouble() || v.isInt() || v.isInt64() || v.isBool(); };
    auto num = [&isNum] (const juce::NamedValueSet& p, const char* key, double& v) -> bool
    {
        const juce::var x = p.getWithDefault (key, juce::var());
        if (!isNum (x)) return false;
        v = x.isBool() ? (static_cast<bool> (x) ? 1.0 : 0.0) : static_cast<double> (x);
        return true;
    };
    // "pts": flat [t0, f0, t1, f1, ...] (this format) or [[t, f], ...] pairs
    // (the unified .meta.json layout).
    auto points = [&isNum] (const juce::NamedValueSet& p, std::vector<std::pair<double, double>>& pts) -> bool
    {
        const juce::var x = p.getWithDefault ("pts", juce::var());
        const auto* a = x.isArray() ? x.getArray() : nullptr;
        if (a == nullptr) return false;
        if (a->size() > 0 && (*a)[0].isArray())
        {
            for (int i = 0; i < a->size(); ++i)
            {
                const auto* pr = (*a)[i].isArray() ? (*a)[i].getArray() : nullptr;
                if (pr == nullptr || pr->size() != 2 || !isNum ((*pr)[0]) || !isNum ((*pr)[1]))
                    return false;
                pts.emplace_back (static_cast<double> ((*pr)[0]), static_cast<double> ((*pr)[1]));
            }
            return true;
        }
        if (a->size() % 2 != 0) return false;
        for (int i = 0; i + 1 < a->size(); i += 2)
        {
            if (!isNum ((*a)[i]) || !isNum ((*a)[i + 1])) return false;   // not a number -> malformed
            pts.emplace_back (static_cast<double> ((*a)[i]), static_cast<double> ((*a)[i + 1]));
        }
        return true;
    };

    std::vector<SavedSpectralSelection> list;
    for (int i = 0; i < sels->size(); ++i)
    {
        const auto* o = (*sels)[i].getDynamicObject();
        if (o == nullptr) continue;
        const auto& p = o->getProperties();

        const juce::var nameVar = p.getWithDefault ("name", juce::var());
        if (!nameVar.isString())
            return false;
        SavedSpectralSelection sel;
        sel.name = nameVar.toString();

        // Optional free-form note (older files have no comment key).
        const juce::var commentVar = p.getWithDefault ("comment", juce::var());
        if (commentVar.isString())
            sel.comment = commentVar.toString();

        const juce::var shapesVar = p.getWithDefault ("shapes", juce::var());
        const auto* shapes = shapesVar.isArray() ? shapesVar.getArray() : nullptr;
        if (shapes == nullptr)
            return false;

        for (int k = 0; k < shapes->size(); ++k)
        {
            const auto* so = (*shapes)[k].getDynamicObject();
            if (so == nullptr) continue;
            const auto& sp = so->getProperties();

            const juce::var kindVar = sp.getWithDefault ("kind", juce::var());
            if (!kindVar.isString()) return false;
            const juce::String kind = kindVar.toString();
            double subV = 0.0;
            num (sp, "sub", subV);
            const bool subtract = subV != 0.0;

            if (kind == "rect")
            {
                double t0 = 0, t1 = 0, f0 = 0, f1 = 0;
                if (!num (sp, "t0", t0) || !num (sp, "t1", t1)
                    || !num (sp, "f0", f0) || !num (sp, "f1", f1))
                    return false;
                sel.data.addRect (t0, t1, f0, f1, subtract, false);
            }
            else if (kind == "poly")
            {
                std::vector<std::pair<double, double>> pts;
                if (!points (sp, pts) || pts.size() < 3)
                    return false;
                sel.data.addPolygon (pts, subtract, false);
            }
            else if (kind == "stroke")
            {
                double radius = 0.0;
                std::vector<std::pair<double, double>> pts;
                if (!num (sp, "radius", radius) || !points (sp, pts) || pts.empty())
                    return false;
                sel.data.addStroke (pts, radius, subtract, false);
            }
            else
                return false;   // unknown shape kind
        }

        list.push_back (std::move (sel));
    }

    out = std::move (list);
    return true;
}
