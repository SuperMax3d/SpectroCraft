// SpectroCraft - time, ruler and amplitude-scale text helpers (see TimeFormat.h).
#include "TimeFormat.h"
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------- time axis

double niceTimeStep (double pxPerSec)
{
    if (!(pxPerSec > 0.0)) return 1.0;
    const double target = 80.0 / pxPerSec;   // seconds per ~80 px of ruler

    // Below one minute: classic 1-2-5 metric steps in seconds, from whole
    // seconds down to microseconds at deep zooms.
    if (target < 60.0)
    {
        double p = std::pow (10.0, std::floor (std::log10 (target)));
        for (int d = 0; d < 3 && p < 60.0; ++d)
        {
            for (double m : { 1.0, 2.0, 5.0 })
                if (m * p >= target - 1e-12) return m * p;
            p *= 10.0;
        }
    }

    // One minute and above: time-unit steps that always yield clean labels
    // ("1m", "2m", "5m", "10m", "15m", "30m", "1h", ...).
    static const double timeSteps [] = { 60.0, 120.0, 300.0, 600.0, 900.0, 1800.0,
                                         3600.0, 7200.0, 18000.0, 36000.0, 54000.0,
                                         108000.0, 216000.0, 432000.0, 864000.0 };
    for (double s : timeSteps)
        if (s >= target - 1e-9) return s;

    // Multi-day files: fall back to metric hours/days.
    double p = std::pow (10.0, std::floor (std::log10 (target)));
    for (int d = 0; d < 4; ++d)
    {
        for (double m : { 1.0, 2.0, 5.0 })
            if (m * p >= target - 1e-9) return m * p;
        p *= 10.0;
    }

    return 864000.0;   // unreachable for sane inputs
}

juce::String formatClockTime (double t, int maxDecimals)
{
    if (!std::isfinite (t)) t = 0.0;
    const bool neg = t < 0.0;   // negative displayed times (negative offsets)
    t = std::abs (t);

    // Round the fractional part to `maxDecimals` digits first so a carry can
    // bump the whole-second count (e.g. 155.99996 @4dp -> 2:36). With no
    // fraction requested, round the whole value instead.
    double wholeD = (maxDecimals > 0) ? std::floor (t) : std::llround (t);
    double frac = t - wholeD;
    if (maxDecimals > 0 && frac > 0.0)
    {
        const double scale = std::pow (10.0, maxDecimals);
        frac = std::round (frac * scale) / scale;
        if (frac >= 1.0 - 1e-12) { wholeD += 1.0; frac = 0.0; }
    }

    const int64_t whole = (int64_t) std::llround (wholeD);
    const int h = (int) (whole / 3600), m = (int) ((whole % 3600) / 60), sec = (int) (whole % 60);

    // Hours appear only once the value reaches one hour, so a 30-minute tick
    // in a two-hour ruler reads "30:00" next to "1:00:00".
    juce::String s;
    if (h > 0)
        s << h << ":" << juce::String (m).paddedLeft ('0', 2) << ":"
          << juce::String (sec).paddedLeft ('0', 2);
    else
        s << m << ":" << juce::String (sec).paddedLeft ('0', 2);

    if (maxDecimals > 0 && frac > 0.0)
    {
        // "0.1270" -> "1270" -> trim trailing zeros -> "127"
        juce::String fs = juce::String (frac, maxDecimals).substring (2);
        while (fs.endsWith ("0")) fs = fs.dropLastCharacters (1);
        if (!fs.isEmpty()) s << "." << fs;
    }
    return neg ? "-" + s : s;
}

juce::String formatFooterTime (double t)
{
    if (!std::isfinite (t) || t < 0.0) t = 0.0;
    if (t >= 60.0) return formatClockTime (t, 4);
    return juce::String (t, 3);
}

bool parseTimeText (const juce::String& sIn, double& out)
{
    const auto s = sIn.trim();
    if (s.isEmpty()) { out = 0.0; return false; }

    // Split on ':' - at most two separators are allowed.
    juce::StringArray parts;
    int start = 0;
    for (int i = 0; ; ++i)
    {
        const bool end = (i >= s.length());
        if (!end && s[i] != ':') continue;

        // Note: this build's substring takes (startIndex, endIndex).
        const auto part = s.substring (start, i);
        if (part.isEmpty()) { out = 0.0; return false; }
        // Digits with at most one decimal point.
        int dots = 0;
        for (int j = 0; j < part.length(); ++j)
        {
            const juce_wchar c = part[j];
            if (c == L'.') { if (++dots > 1) { out = 0.0; return false; } }
            else if (c < L'0' || c > L'9') { out = 0.0; return false; }
        }
        parts.add (part);
        if (end) break;
        start = i + 1;
    }
    if (parts.size() > 3) { out = 0.0; return false; }

    double v = 0.0;
    try
    {
        for (const auto& p : parts)
            v = v * 60.0 + std::stod (p.toRawUTF8());
    }
    catch (...) { out = 0.0; return false; }

    if (!std::isfinite (v) || v < 0.0) { out = 0.0; return false; }
    out = v;
    return true;
}

juce::String formatOffsetTime (double t)
{
    // Always "hh:mm:ss.xxxx" with exactly four fractional digits: the Time
    // Offset prompt and its readouts use this fixed shape.
    if (!std::isfinite (t)) t = 0.0;
    const bool neg = t < 0.0;
    t = std::abs (t);
    const int64_t whole = (int64_t) std::llround (std::floor (t));
    const double frac = t - std::floor (t);
    const int h = (int) (whole / 3600), m = (int) ((whole % 3600) / 60), s = (int) (whole % 60);
    juce::String fs = juce::String (frac, 4).substring (2);   // "1270"
    while (fs.length() < 4) fs = "0" + fs;
    juce::String out;
    if (neg) out << "-";
    if (h > 0)
        out << h << ":" << juce::String (m).paddedLeft ('0', 2) << ":"
            << juce::String (s).paddedLeft ('0', 2);
    else
        out << m << ":" << juce::String (s).paddedLeft ('0', 2);
    return out << "." << fs;
}

bool parseSignedClock (const juce::String& sIn, double& outSecs)
{
    auto s = sIn.trim();
    bool neg = false;
    if (!s.isEmpty() && (s[0] == L'-' || s[0] == L'+'))
    {
        neg = (s[0] == L'-');
        s = s.substring (1);
    }
    double v = 0.0;
    if (!parseTimeText (s, v)) return false;
    outSecs = neg ? -v : v;
    return true;
}

/* Displayed value of a real time, for the axis of a given mode: every mode
   carries the entry's offset and (optionally) its scale multiplier. */
double timeToDisplayValue (double tSecs, TimeAxisMode m, double offsetSecs, double rate,
                           double scale)
{
    const double t = tSecs + offsetSecs;
    const double sc = (scale != 0.0 ? scale : 1.0);
    switch (m)
    {
        case TimeAxisMode::Sample:
            return t * std::max (1.0, rate);
        default:
            return t * sc;
    }
}

juce::String formatDisplayedTime (double tSecs, TimeAxisMode m, double offsetSecs, double rate,
                                  double scale)
{
    const double d = timeToDisplayValue (tSecs, m, offsetSecs, rate, scale);
    switch (m)
    {
        case TimeAxisMode::Sample:
            return juce::String ((int64_t) std::llround (d));
        case TimeAxisMode::Seconds:
            return juce::String (d, 4);
        case TimeAxisMode::TimeOffset:
            return formatOffsetTime (d);
        default:
            return formatFooterTime (d);
    }
}

bool parseDisplayedTime (const juce::String& s, TimeAxisMode m, double offsetSecs,
                         double rate, double& outSecs, double scale)
{
    double v = 0.0;
    switch (m)
    {
        case TimeAxisMode::Sample:
        {
            // Integer or decimal sample count (sign allowed).
            auto t = s.trim();
            if (t.isEmpty()) { outSecs = 0.0; return false; }
            bool neg = false;
            if (t[0] == L'-' || t[0] == L'+')
            {
                neg = (t[0] == L'-');
                t = t.substring (1);
            }
            int dots = 0, digits = 0;
            for (int i = 0; i < t.length(); ++i)
            {
                const juce_wchar c = t[i];
                if (c == L'.') { if (++dots > 1) { outSecs = 0.0; return false; } }
                else if (c < L'0' || c > L'9') { outSecs = 0.0; return false; }
                else ++digits;
            }
            if (dots == 0 && digits == 0) { outSecs = 0.0; return false; }
            double raw = t.getDoubleValue();
            if (!std::isfinite (raw)) { outSecs = 0.0; return false; }
            v = (neg ? -raw : raw) * (scale != 0.0 ? scale : 1.0);
            outSecs = v / std::max (1.0, rate) - offsetSecs;
            return true;
        }
        case TimeAxisMode::Seconds:
        {
            // Plain seconds, with or without the decimal point.
            if (!parseSignedClock (s, v)) { outSecs = 0.0; return false; }
            break;
        }
        default:
        {
            if (!parseSignedClock (s, v)) { outSecs = 0.0; return false; }
            break;
        }
    }

    outSecs = v / (scale != 0.0 ? scale : 1.0) - offsetSecs;
    return true;
}

double niceSampleStep (double samplesPerPixel)
{
    // Same 1-2-5 metric idea as niceTimeStep(), but in the sample domain and
    // never below one sample.
    const double target = std::max (1e-9, samplesPerPixel * 80.0);
    const int e = (int) std::floor (std::log10 (target));
    for (int k = -2; k < 5; ++k)
        for (double m : { 1.0, 2.0, 5.0 })
        {
            const double step = m * std::pow (10.0, e + k);
            if (step >= target) return std::max (1.0, step);
        }
    return std::max (1.0, std::pow (10.0, e + 5));
}

juce::String formatWaveVScaleLabel (double gridDb, int mode, const ScaleCalibration& sc,
                                    bool positiveSide, int ch)
{
    switch (mode)
    {
        case 1:   // dB: plain relative level.
            return juce::String ((int) std::lround (gridDb));
        case 2:
        {
            // Normalized: signed linear amplitude at this grid line
            // (+a above the centre line, -a below it).
            const double a = std::pow (10.0, gridDb / 20.0);
            juce::String s = juce::String (std::abs (a), 3);
            while (s.length() > 2 && s.endsWith ("0")) s = s.dropLastCharacters (1);
            if (s.endsWith (".")) s = s.dropLastCharacters (1);
            return (positiveSide ? "+" : "-") + s;
        }
        default:
            // Wav Units: physical value when calibrated, relative dB otherwise.
            return ScaleCalibration::formatWaveformAxisValue (gridDb, sc, ch);
    }
}

juce::String waveVScaleTitle (int mode, const ScaleCalibration& sc)
{
    switch (mode)
    {
        case 1: return "dB";
        case 2: return "norm";
        default: return sc.unitTitle();
    }
}

juce::String formatTimeTickLabel (double t, double step, double span)
{
    // Zoomed-out rulers use clock-style labels based on the visible range:
    // m:ss above one minute, h:mm:ss above an hour. Fractional seconds only
    // appear when the step is sub-second (defensive; the nice-step sequence
    // keeps steps >= 1 s at these zooms for any realistic plot width).
    if (span > 60.0)
    {
        int maxDec = 0;
        if (step < 1.0)
            maxDec = std::min (4, (int) std::ceil (-std::log10 (step) - 1e-9));
        return formatClockTime (t, maxDec);
    }

    // A tick at the origin, or one a rounding error away from it.
    if (t == 0.0 || (step > 0.0 && std::abs (t) < step * 1e-6)) return "0";
    if (step >= 1.0)   return juce::String ((int) std::llround (t));
    if (step >= 0.01)  return juce::String (t, step >= 0.1 ? 1 : 2);

    // Sub-10 ms steps inside the first second: milliseconds, with as many
    // decimals as a sub-millisecond step needs ("380ms", "380.5ms",
    // "380.005ms"). No microseconds (#36): counts like "380000us" were hard
    // to read, and the escaped micro sign was not UTF-8 in MSVC builds, so
    // Windows drew a box in its place.
    if (std::abs (t) < 1.0)
    {
        const double stepMs = step * 1000.0;
        if (stepMs >= 1.0 - 1e-9) return juce::String ((int) std::llround (t * 1000.0)) + "ms";
        const int decimals = juce::jlimit (1, 6, (int) std::ceil (-std::log10 (stepMs) - 1e-9));
        return juce::String (t * 1000.0, decimals) + "ms";
    }

    // ...otherwise decimal seconds with as many digits as the step needs.
    int digits = (int) std::ceil (-std::log10 (step) - 1e-9);
    if (digits < 3) digits = 3;
    return juce::String (t, digits);
}
