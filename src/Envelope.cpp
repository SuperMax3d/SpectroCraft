// SpectroCraft - Envelope (graphical amplitude-over-time effect).
#include "Envelope.h"

#include <cmath>
#include <algorithm>

namespace sc { namespace env {

namespace {

juce::String formatNumber (double v)
{
    juce::String s = juce::String (v, 6).trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
    if (s.isEmpty ()) s = "0";
    return s;
}

/** Strict plain-number parse: optional sign, digits, at most one dot. */
bool parseNumber (const juce::String& text, double& out)
{
    // A named String: binding a StringRef to the trim() temporary would
    // dangle once the statement ends (undefined behaviour).
    const juce::String t = text.trim ();
    if (t.isEmpty ()) return false;
    bool digits = false, dot = false, sign = false;
    for (int i = 0; i < t.length (); ++i)
    {
        const auto ch = t[i];
        if (!sign && (ch == '+' || ch == '-')) { sign = true; continue; }
        if (ch >= '0' && ch <= '9')          { digits = true;  continue; }
        if (ch == '.' && !dot)               { dot = true;     continue; }
        return false;
    }
    if (!digits) return false;
    out = t.getDoubleValue ();
    return std::isfinite (out);
}

/** dB gain of the curve at `timeSec`, linear in time. */
double gainAt (const std::vector<EnvPoint>& pts, double t)
{
    if (pts.empty ()) return 0.0;
    if (t <= pts.front ().tSec) return pts.front ().gainDb;
    if (t >= pts.back ().tSec) return pts.back ().gainDb;

    // Binary search for the bracketing pair.
    int lo = 0, hi = (int) pts.size () - 1;
    while (hi - lo > 1)
    {
        const int mid = (lo + hi) / 2;
        if (pts[(size_t) mid].tSec <= t) lo = mid; else hi = mid;
    }
    const EnvPoint& a = pts[(size_t) lo];
    const EnvPoint& b = pts[(size_t) hi];
    const double span = b.tSec - a.tSec;
    if (span <= 0.0) return a.gainDb;
    return a.gainDb + (b.gainDb - a.gainDb) * ((t - a.tSec) / span);
}

} // namespace

double curveGainDb (const std::vector<EnvPoint>& pts, double timeSec)
{
    if (!std::isfinite (timeSec)) return 0.0;
    return gainAt (pts, timeSec);
}

bool isFlatCurve (const std::vector<EnvPoint>& pts)
{
    for (const auto& p : pts)
        if (!(std::abs (p.gainDb) < 1e-6))
            return false;
    return true;
}

void applyEnvelope (float* data, int len, double rate, const std::vector<EnvPoint>& pts)
{
    if (data == nullptr || len <= 0 || rate <= 0.0) return;
    for (int i = 0; i < len; ++i)
    {
        const double g = gainAt (pts, (double) i / rate);
        data[(size_t) i] *= (float) std::pow (10.0, g * 0.05);
    }
}

juce::String serializePoints (const std::vector<EnvPoint>& pts)
{
    juce::StringArray parts;
    for (const auto& p : pts)
        if (std::isfinite (p.tSec) && p.tSec >= 0.0 && std::isfinite (p.gainDb))
            parts.add (formatNumber (p.tSec) + "," + formatNumber (p.gainDb));
    return parts.joinIntoString (";");
}

std::vector<EnvPoint> parsePoints (const juce::String& text)
{
    std::vector<EnvPoint> out;
    juce::StringArray pairs;
    // 3-arg form: the 2-arg call would bind to the bool overload and split on
    // whitespace instead of ';'.
    pairs.addTokens (text, ";", "");
    for (const auto& pair : pairs)
    {
        juce::StringArray tokens;
        tokens.addTokens (pair, ",", "");
        if (tokens.size () != 2) continue;
        double t = 0.0, g = 0.0;
        if (!parseNumber (tokens[0], t) || !parseNumber (tokens[1], g)) continue;
        if (!(t >= 0.0)) continue;
        out.push_back ({ t, g });
    }
    std::sort (out.begin (), out.end (),
               [] (const EnvPoint& a, const EnvPoint& b) { return a.tSec < b.tSec; });
    return out;
}

juce::String serializeUserPresets (const std::vector<EnvPreset>& presets)
{
    juce::StringArray lines;
    for (const auto& p : presets)
    {
        // newLineCharsRemoved() is not in this build: strip manually. Newlines
        // and the '|' separator become spaces so the preset survives, matching
        // the Add-preset dialog's sanitisation.
        juce::String name = p.name.replace ("\r\n", " ").replace ("\n", " ")
                                   .replace ("\r", " ").replace ("|", " ")
                                   .trim ();
        if (name.isEmpty ()) continue;
        lines.add (name + "|" + serializePoints (p.points));
    }
    return lines.joinIntoString ("\n");
}

std::vector<EnvPreset> parseUserPresets (const juce::String& text)
{
    std::vector<EnvPreset> out;
    for (const auto& line : juce::StringArray::fromLines (text))
    {
        const int bar = line.indexOf ("|");
        if (bar <= 0 || bar >= line.length () - 1) continue;
        EnvPreset p;
        p.name = line.substring (0, bar).trim ();
        p.points = parsePoints (line.substring (bar + 1));
        if (!p.name.isEmpty ())
            out.push_back (std::move (p));
    }
    return out;
}

std::vector<EnvPoint> scaleToDuration (const std::vector<EnvPoint>& pts, double targetSecs)
{
    auto out = pts;
    if (!std::isfinite (targetSecs) || targetSecs <= 0.0 || out.size () < 2)
        return out;   // a single point holds its gain everywhere: nothing to stretch

    const double maxT = out.back ().tSec;
    if (maxT - out.front ().tSec <= 1e-9 || maxT <= 1e-9)
        return out;   // zero-span curve: nothing to stretch

    const double scale = targetSecs / maxT;
    for (auto& p : out)
        p.tSec *= scale;
    std::sort (out.begin (), out.end (),
               [] (const EnvPoint& a, const EnvPoint& b) { return a.tSec < b.tSec; });
    return out;
}

const std::vector<EnvPreset>& presets()
{
    static const std::vector<EnvPreset> list = {
        { "Flat", {
            { 0.0, 0.0 },
        }},
        // Times are relative: the preset is stretched to the target duration
        // when it is loaded (see scaleToDuration).
        { "Fade In", {
            { 0.0, -30.0 }, { 1.0, 0.0 },
        }},
        { "Fade Out", {
            { 0.0, 0.0 }, { 1.0, -30.0 },
        }},
        { "Dip in Middle", {
            { 0.0, 0.0 }, { 0.5, -30.0 }, { 1.0, 0.0 },
        }},
    };
    return list;
}

}} // namespace sc::env
