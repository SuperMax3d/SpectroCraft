// SpectroCraft - axis scale entries (see include/AxisScale.h).
#include "AxisScale.h"
#include "TimeFormat.h"   // formatClockTime / parseTimeText

#include <cmath>

namespace sc::axes
{

namespace
{
/** Strict plain-number parser (the same rules as the app's gain prompt): an
    optional sign, digits and at most one decimal point. */
bool parseNumber (const juce::String& sIn, double& out)
{
    const auto t = sIn.trim();
    if (t.isEmpty()) return false;

    int i = 0;
    if (t[i] == '+' || t[i] == '-') ++i;

    bool digitSeen = false, dotSeen = false;
    for (; i < t.length(); ++i)
    {
        const char c = t[i];
        if (c >= '0' && c <= '9') { digitSeen = true; continue; }
        if (c == '.' && ! dotSeen) { dotSeen = true;  continue; }
        return false;
    }
    if (! digitSeen) return false;
    out = t.getDoubleValue();
    return true;
}
} // namespace

// ------------------------------------------------------------------ vocabulary

const char* kindName (Kind k)
{
    switch (k)
    {
        case Kind::Amplitude: return "Amplitude";
        case Kind::Frequency: return "Frequency";
        default:              return "Time";
    }
}

const char* typeName (Type t)
{
    // "Axis and Footer": an extra axis strip AND a footer readout (issue
    // #54; it was called "Add Axis" and showed no readout).
    return t == Type::FooterOnly ? "Footer Only" : "Axis and Footer";
}

const juce::StringArray& unitNames (Kind k)
{
    static const juce::StringArray timeUnits  { "hh:mm:ss.xxxx", "ss.xxxx", "samples" };
    static const juce::StringArray ampUnits   { "dB", "None", "Volt", "Pascal", "g" };
    static const juce::StringArray freqUnits  { "Hz", "%", "None" };
    switch (k)
    {
        case Kind::Amplitude: return ampUnits;
        case Kind::Frequency: return freqUnits;
        default:              return timeUnits;
    }
}

int defaultUnits (Kind k)
{
    switch (k)
    {
        case Kind::Amplitude: return 0;   // dB
        case Kind::Frequency: return 0;   // Hz
        default:              return 0;   // hh:mm:ss.xxxx
    }
}

List defaults()
{
    List l;
    for (auto k : { Kind::Time, Kind::Amplitude, Kind::Frequency })
    {
        Entry e;
        e.kind   = k;
        e.units  = defaultUnits (k);
        e.name   = juce::String (kindName (k)) + " " + unitNames (k)[e.units];
        e.scale  = 1.0;
        e.visible = true;
        e.type   = Type::AddAxis;
        l.push_back (e);
    }
    return l;
}

// ------------------------------------------------------------- selection

const Entry* primary (const List& entries, Kind k)
{
    for (const auto& e : entries)
        if (e.kind == k && e.visible && e.isAddAxis()) return &e;
    return nullptr;
}

int countAddAxis (const List& entries, Kind k)
{
    int n = 0;
    for (const auto& e : entries)
        if (e.kind == k && e.visible && e.isAddAxis()) ++n;
    return n;
}

List extras (const List& entries, Kind k)
{
    List out;
    bool past = false;
    for (const auto& e : entries)
    {
        if (e.kind != k || !e.visible || !e.isAddAxis()) continue;
        if (!past) { past = true; continue; }   // skip the primary one
        out.push_back (e);
    }
    return out;
}

int countKind (const List& entries, Kind k)
{
    int n = 0;
    for (const auto& e : entries)
        if (e.kind == k) ++n;
    return n;
}

List footerOnly (const List& entries, Kind k)
{
    List out;
    for (const auto& e : entries)
        if (e.kind == k && e.visible && e.type == Type::FooterOnly)
            out.push_back (e);
    return out;
}

List secondary (const List& entries, Kind k)
{
    List out;
    const Entry* main = primary (entries, k);
    for (const auto& e : entries)
        if (e.kind == k && e.visible && &e != main)
            out.push_back (e);
    return out;
}

bool isSecondary (const List& entries, int index)
{
    if (index < 0 || index >= (int) entries.size()) return false;
    const Entry& e = entries[(size_t) index];
    return e.visible && &e != primary (entries, e.kind);
}

juce::String defaultName (Kind k, int unitsIndex)
{
    const auto& names = unitNames (k);
    const juce::String unit = (unitsIndex >= 0 && unitsIndex < names.size())
                            ? names[unitsIndex] : juce::String();
    return juce::String (kindName (k)) + " " + unit;
}

// -------------------------------------------------------------- formatting

double displayValueTime (const Entry& e, double tSecs, double rate)
{
    const double t = tSecs + e.offset;
    if (e.units == 2) return t * std::max (1.0, rate);   // samples
    return t * (e.scale != 0.0 ? e.scale : 1.0);
}

juce::String formatTime (const Entry& e, double tSecs, double rate)
{
    const double v = displayValueTime (e, tSecs, rate);
    if (e.units == 2) return juce::String ((int64_t) std::llround (v));
    if (e.units == 1) return juce::String (v, 4);        // plain seconds
    return formatClockTime (v, 4);
}

bool parseTime (const Entry& e, const juce::String& textIn, double rate, double& outSecs)
{
    const auto text = textIn.trim();
    if (text.isEmpty()) return false;

    double v = 0.0;
    if (e.units == 2)
    {
        // Sample count: integer or decimal, sign allowed.
        if (! parseNumber (text, v)) return false;
        outSecs = v / std::max (1.0, rate);
        return true;
    }
    if (e.units == 1)
    {
        if (! parseNumber (text, v)) return false;       // plain seconds
    }
    else if (!parseSignedClock (text, v)) return false;  // h:m:s.xxxx

    const double sc = (e.scale != 0.0 ? e.scale : 1.0);
    outSecs = v / sc - e.offset;
    return true;
}

double displayValueFrequency (const Entry& e, double fHz, double nyq)
{
    const double sc = (e.scale != 0.0 ? e.scale : 1.0);
    if (e.units == 1) return fHz * sc / std::max (1.0, nyq) * 100.0;   // % of Nyquist
    return fHz * sc;
}

juce::String formatFrequency (const Entry& e, double fHz, double nyq)
{
    const double v = displayValueFrequency (e, fHz, nyq);
    if (e.units == 1) return juce::String (v, 2) + "%";
    if (e.units == 2) return juce::String (v, 1);
    // Hz: the compact form the frequency axis uses today.
    if (std::abs (v) >= 1000.0)
    {
        juce::String k (v / 1000.0, 2);
        while (k.endsWith ("0") && k.contains (".")) k = k.substring (0, k.length() - 1);
        if (k.endsWith (".")) k = k.substring (0, k.length() - 1);
        return k + "k";
    }
    return juce::String (v, 1);
}

juce::String formatTickNumber (double v, int maxDecimals)
{
    juce::String s (v, juce::jmax (0, maxDecimals));
    if (s.containsChar ('.'))
    {
        while (s.endsWithChar ('0')) s = s.dropLastCharacters (1);
        if (s.endsWithChar ('.')) s = s.dropLastCharacters (1);
    }
    if (s == "-0") s = "0";
    return s;
}

juce::String formatFrequencyTick (const Entry& e, double displayed)
{
    if (e.units == 1) return formatTickNumber (displayed) + " %";
    if (e.units == 2) return formatTickNumber (displayed);
    if (std::abs (displayed) >= 1000.0) return formatTickNumber (displayed / 1000.0) + " kHz";
    return formatTickNumber (displayed) + " Hz";
}

juce::String formatFrequencyAxis (const Entry& e, double fHz, double nyq)
{
    const double v = displayValueFrequency (e, fHz, nyq);
    if (e.units == 1) return juce::String (v, 2) + " %";
    if (e.units == 2) return juce::String (v, 1);

    // Hz, spelled out like the main frequency axis does.
    if (std::abs (v) >= 1000.0)
    {
        juce::String k (v / 1000.0, 1);
        if (k.endsWith (".0")) k = k.substring (0, k.length() - 2);
        return k + " kHz";
    }
    return juce::String (v, 0) + " Hz";
}

juce::String formatAmplitude (const Entry& e, double normalised, int ch)
{
    const double sc = e.scaleFor (ch);
    const double v  = normalised * sc;

    if (e.units == 0)   // dB, relative to the entry's full scale
        return (std::abs (v) > 1e-9) ? juce::String (20.0 * std::log10 (std::abs (v)), 1)
                                     : juce::String ("-inf");

    if (e.units == 1)   // plain -1..1
        return juce::String (v, 3);

    // Physical units: the value at |1.0| is the entered scale.
    const char* suffix = (e.units == 3) ? " Pa" : (e.units == 4) ? " g" : " V";
    return ScaleCalibration::formatPhysicalValue (v) + suffix;
}

double offsetForDisplayedValue (const Entry& e, double tSecs, double displayed, double rate)
{
    // displayed(t) = (t + offset) * scale, or (t + offset) * rate for samples.
    const double unit = (e.units == 2) ? std::max (1.0, rate)
                                       : (e.scale != 0.0 ? e.scale : 1.0);
    return displayed / unit - tSecs;
}

bool solveTimePoints (Entry& e, double t0, double v0, double t1, double v1, double rate)
{
    const double dt = t1 - t0;
    if (std::abs (dt) < 1e-12) return false;

    if (e.units == 2)
    {
        // Sample counts carry no scale factor: only the offset can follow.
        e.offset = v0 / std::max (1.0, rate) - t0;
        return false;
    }
    const double sc = (v1 - v0) / dt;
    if (std::abs (sc) < 1e-12) return false;
    e.scale  = sc;
    e.offset = v0 / sc - t0;
    return true;
}

double scaleForReference (const Entry& e, double mainValue, double typedValue, double nyq)
{
    if (std::abs (mainValue) < 1e-9) return std::nan ("");

    switch (e.kind)
    {
        case Kind::Frequency:
            // % of Nyquist: displayed = f * scale / nyq * 100.
            if (e.units == 1) return typedValue * std::max (1.0, nyq) / (mainValue * 100.0);
            return typedValue / mainValue;

        case Kind::Amplitude:
        {
            // dB labels 20*log10(|normalised * scale|); the rest is linear.
            const double target = (e.units == 0) ? std::pow (10.0, typedValue / 20.0)
                                                 : typedValue;
            return target / mainValue;
        }

        default:
            return typedValue / (e.scale != 0.0 ? e.scale : 1.0);
    }
}

// ------------------------------------------------- migration and application

int unitsFromCalibration (const ScaleCalibration& sc)
{
    switch (sc.unit)
    {
        case ScaleCalibration::Unit::Volts:  return 2;
        case ScaleCalibration::Unit::Pascal: return 3;
        case ScaleCalibration::Unit::Grams:  return 4;
        default:                             return 1;   // None
    }
}
ScaleCalibration::Unit calibrationUnit (int units)
{
    switch (units)
    {
        case 2: return ScaleCalibration::Unit::Volts;
        case 3: return ScaleCalibration::Unit::Pascal;
        case 4: return ScaleCalibration::Unit::Grams;
        default: return ScaleCalibration::Unit::None;
    }
}

List migrateLegacy (const Legacy& in, bool& hadLegacy)
{
    List l;
    const ScaleCalibration& sc = in.scale;
    const bool anyAlt = ! in.altTime.empty() || ! in.altFreq.empty();
    const bool customFreq = in.specFreqScaleMode != 0
                         || std::abs (in.specFreqFactor - 1.0) > 1e-12;
    const bool timeMoved = in.timeAxisMode != 0 || std::abs (in.timeOffsetSecs) > 1e-12;
    hadLegacy = sc.isCalibrated() || anyAlt || customFreq || timeMoved
             || in.waveVScaleMode != 0;

    // --- Time: the main axis first, then one entry per alternative reference.
    {
        Entry e;
        e.kind = Kind::Time;
        e.type = Type::AddAxis;
        e.visible = true;
        e.scale = 1.0;
        if (in.timeAxisMode == 2)      e.units = 2;      // samples
        else if (in.timeAxisMode == 3) e.units = 1;      // plain seconds
        else e.units = 0;
        e.offset = in.timeOffsetSecs;
        e.name = std::abs (e.offset) > 1e-12
                   ? juce::String ("Time ") + formatOffsetTime (e.offset)
                   : juce::String ("Time ") + unitNames (Kind::Time)[e.units];
        l.push_back (e);
    }
    for (const auto& a : in.altTime)
    {
        const double refMain = a.second.first, altValue = a.second.second;
        if (std::abs (refMain) < 1e-30) continue;
        Entry e;
        e.kind  = Kind::Time;
        e.type  = Type::FooterOnly;
        e.units = 1;   // plain numbers
        e.scale = altValue / refMain;
        e.name  = a.first;
        e.visible = true;
        l.push_back (e);
    }

    // --- Amplitude: one entry, carrying the file's calibration.
    {
        Entry e;
        e.kind = Kind::Amplitude;
        e.type = Type::AddAxis;
        e.visible = true;
        e.units = sc.isCalibrated() ? unitsFromCalibration (sc)
                                    : (in.waveVScaleMode == 1 ? 0 : 1);
        e.scale = sc.fullScaleValue > 0.0 ? sc.fullScaleValue : 1.0;
        if (sc.hasPerChannelValues())
            for (const double v : sc.channelFullScale)
                e.chanScale.push_back (v > 0.0 ? v : 1.0);
        e.name = (e.units == 0) ? "Amplitude dB"
                                : (e.units == 1) ? "Amplitude"
                                : juce::String ("Amplitude ") + unitNames (Kind::Amplitude)[e.units];
        l.push_back (e);
    }

    // --- Frequency: the main axis (with the custom-unit factor), then the
    // alternative references.
    {
        Entry e;
        e.kind = Kind::Frequency;
        e.type = Type::AddAxis;
        e.visible = true;
        e.units = 0;
        e.scale = customFreq ? in.specFreqFactor : 1.0;
        e.name  = customFreq ? "Frequency Custom"
                             : juce::String ("Frequency ") + unitNames (Kind::Frequency)[0];
        l.push_back (e);
    }
    for (const auto& a : in.altFreq)
    {
        const double refMain = a.second.first, altValue = a.second.second;
        if (std::abs (refMain) < 1e-30) continue;
        Entry e;
        e.kind  = Kind::Frequency;
        e.type  = Type::FooterOnly;
        e.units = 2;   // bare numbers
        e.scale = altValue / refMain;
        e.name  = a.first;
        e.visible = true;
        l.push_back (e);
    }
    return l;
}

// ------------------------------------------------------------------- JSON

juce::var toJson (const List& entries)
{
    juce::Array<juce::var> arr;
    for (const auto& e : entries)
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("kind",   (int) e.kind);
        o->setProperty ("name",   e.name);
        o->setProperty ("units",  e.units);
        o->setProperty ("offset", e.offset);
        o->setProperty ("scale",  e.scale);
        o->setProperty ("visible", static_cast<int> (e.visible));
        o->setProperty ("type",   (int) e.type);
        if (! e.chanScale.empty())
        {
            juce::Array<juce::var> cs;
            for (const double v : e.chanScale) cs.add (v);
            o->setProperty ("chanScale", cs);
        }
        arr.add (o.get());
    }
    return arr;
}

bool fromJson (const juce::var& v, List& out, bool& hadKey)
{
    hadKey = false;
    if (! v.isArray()) return false;
    hadKey = true;

    const auto* arr = v.getArray();
    if (arr == nullptr) return false;

    List parsed;
    for (const auto& item : *arr)
    {
        if (! item.isObject()) continue;
        auto* o = item.getDynamicObject();
        if (o == nullptr) continue;

        Entry e;
        auto& props = o->getProperties();
        e.kind    = (Kind) (int) props.getWithDefault ("kind", 0);
        e.name    = props.getWithDefault ("name", juce::String());
        e.units   = (int) props.getWithDefault ("units", 0);
        e.offset  = props.getWithDefault ("offset", 0.0);
        e.scale   = props.getWithDefault ("scale", 1.0);
        e.visible = (int) props.getWithDefault ("visible", 1) != 0;
        e.type    = (Type) (int) props.getWithDefault ("type", 0);
        if (e.kind != Kind::Time && e.kind != Kind::Amplitude && e.kind != Kind::Frequency)
            continue;
        const int maxUnits = unitNames (e.kind).size();
        if (e.units < 0 || e.units >= maxUnits) e.units = defaultUnits (e.kind);
        if (! std::isfinite (e.scale) || e.scale == 0.0) e.scale = 1.0;
        if (! std::isfinite (e.offset)) e.offset = 0.0;

        const juce::var cs = props.getWithDefault ("chanScale", juce::var());
        if (cs.isArray())
            if (const auto* cv = cs.getArray())
                for (const auto& x : *cv)
                    if (x.isInt() || x.isDouble())
                        e.chanScale.push_back ((double) x);
        parsed.push_back (e);
    }
    if (parsed.empty()) return false;
    out = parsed;
    return true;
}

// --------------------------------------------------------------- geometry

// WaveformView::kRulerH (checked by a static_assert in AxisScaleApp.cpp).
int primaryRulerHeight() { return 24; }
int extraRulerHeight()   { return 18; }

int primaryStripWidth() { return kLeftAxisWidth; }   // 60 px, as always
int extraStripWidth()   { return 38; }               // a second axis costs 38, not 60

// Enough room at the outer edge for "L", "R" or a two-digit channel number.
int channelGutter() { return 24; }

/* The label box of one vertical axis strip: inside that strip, and pushed in
   at the outer edge of the whole axis area by the room the channel label
   needs - only for the outermost strip, which is where it sits. */
juce::Rectangle<int> verticalLabelRect (int index, int count, int axisW,
                                       int plotX, int plotRight, int top, int bottom,
                                       bool axisOnRight, int gutter)
{
    const auto strip = verticalStrip (index, count, axisW, plotX, plotRight, top, bottom,
                                      axisOnRight, gutter);
    // Only the outermost strip on the left gives up the gutter; with no
    // gutter it keeps the usual 2 px margin like the others.
    const bool inset = (index == count - 1) && gutter > 0;
    if (axisOnRight)
        return { strip.getX() + 2, top, strip.getWidth() - 4, bottom - top };
    return { strip.getX() + (inset ? gutter : 2), top,
             strip.getWidth() - 4 - (inset ? gutter : 0), bottom - top };
}

// ---------------------------------------------------------------- axis style

/* One look for the vertical axes of every track (waveform, spectrogram,
   dedicated tables track): the same label size and colour, the same grid line.
   The waveform's grid is drawn BEHIND its waveform, so it no longer needs the
   alpha it used to carry to avoid hiding a quiet signal on the zero line. */
juce::Font axisLabelFont()          { return juce::Font { juce::FontOptions (10.0f) }; }
juce::Font axisMinorLabelFont()     { return juce::Font { juce::FontOptions (9.0f) }; }
juce::Colour axisLabelColour()      { return juce::Colour::fromRGB (200, 200, 200); }
juce::Colour axisMinorLabelColour() { return juce::Colour::fromRGB (130, 130, 138); }
juce::Colour axisGridColour()       { return juce::Colour::fromRGB (60, 60, 68); }
juce::Colour axisStripFill()        { return juce::Colour::fromRGB (32, 32, 36); }

juce::Rectangle<int> verticalStrip (int index, int count, int axisW, int plotX, int plotRight,
                                    int top, int bottom, bool axisOnRight, int gutter)
{
    /* Walk out from the plot: the main axis strip (60 px) hugs the plot, each
       extra one (38 px) sits further out. `axisW` is only a sanity check - the
       strips are laid out from the plot edge, so they always fill exactly the
       space Application::axisStripWidth() reserved. */
    (void) axisW;
    const auto h = std::max (1, bottom - top);
    const int outer = std::max (1, count) - 1;
    auto widthOf = [&] (int i)
    {
        if (axisOnRight)
            return i == 0 ? primaryStripWidth() : extraStripWidth();
        /* On the left the channel label sits at the outer edge, and
           verticalLabelRect() insets the outermost strip by its room. The
           main strip's 60 px include that room: when an extra axis is the
           outermost one, the room moves out with it - otherwise the extra
           strip kept 10 px for its labels (they read "...") and the main
           labels floated in the middle of the axis area. */
        const int g = std::max (0, gutter);
        const int base = i == 0 ? primaryStripWidth() - g : extraStripWidth();
        return base + (i == outer ? g : 0);
    };

    if (axisOnRight)
    {
        int x = plotRight;
        for (int i = 0; i <= index; ++i)
        {
            const int w = widthOf (i);
            if (i == index) return { x, top, w, h };
            x += w;
        }
        return { plotRight, top, primaryStripWidth(), h };
    }

    int x = plotX;
    for (int i = 0; i <= index; ++i)
    {
        const int w = widthOf (i);
        x -= w;
        if (i == index) return { x, top, w, h };
    }
    return { plotX - widthOf (0), top, widthOf (0), h };
}

juce::Rectangle<int> timeStrip (int index, int x, int width, int plotTop,
                                int plotBottom, int stackHeight, bool rulerAtBottom)
{
    const int primary = primaryRulerHeight(), extra = extraRulerHeight();
    const int w = std::max (1, width);

    if (rulerAtBottom)
    {
        /* The caller's plot rect already excludes the stack, so the stack hangs
           *below* the plot: the primary ruler hugs the bottom edge of the
           component and the extras sit between it and the plot. Placing the
           primary at plotBottom - primary put it inside the plot instead, which
           left the reserved band empty (a black bar) and drew the ruler over
           the track. */
        if (index == 0)
            return { x, plotBottom, w, primary };
        const int y = plotBottom + primary + extra * (index - 1);
        return { x, y, w, extra };
    }

    /* Above the plot the stack hangs *above* it, and the order is the same
       rule as everywhere else: the first axis in the Scale form is the one
       closest to the track. So the primary ruler hugs the top edge of the
       plot and the extras stack above it. */
    if (index == 0)
        return { x, plotTop - primary, w, primary };
    return { x, plotTop - primary - extra * index, w, extra };
}

void drawExtraVerticalAxis (juce::Graphics& g, const Entry& e,
                            juce::Rectangle<int> strip, const Ticks& ticks,
                            int channelForLabels, double rate, double nyq)
{
    if (strip.getWidth() < 8 || ticks.empty()) return;

    const bool right = (strip.getX() > 0);   // filled below from the caller's side
    // A secondary axis reads as secondary: the same smaller type the extra
    // time axes use.
    g.setFont (juce::Font { juce::FontOptions (8.5f) });
    g.setColour (axisMinorLabelColour());
    for (const auto& tk : ticks)
    {
        const double main = tk.second;
        juce::String text;
        switch (e.kind)
        {
            case Kind::Amplitude: text = formatAmplitude (e, main, channelForLabels); break;
            case Kind::Frequency: text = formatFrequency (e, main, nyq); break;
            default:              text = formatTime (e, main, rate); break;
        }
        const int y = (int) std::lround (tk.first) - 7;
        g.drawText (text, juce::Rectangle<int> (strip.getX() + 3, y,
                                                strip.getWidth() - 6, 14),
                    right ? juce::Justification::left : juce::Justification::right,
                    true);
    }
}

void drawExtraTimeAxis (juce::Graphics& g, const Entry& e,
                        juce::Rectangle<int> strip, const Ticks& ticks, double rate)
{
    if (strip.getHeight() <= 0 || ticks.empty()) return;

    // A narrower, darker strip: the secondary time axis must read as a
    // secondary one next to the normal ruler.
    g.setColour (juce::Colour::fromRGB (26, 26, 28));
    g.fillRect (strip);
    g.setColour (juce::Colour::fromRGB (55, 55, 60));
    g.drawLine ((float) strip.getX(), (float) strip.getY(),
                (float) strip.getRight(), (float) strip.getY(), 1.0f);

    g.setFont (juce::Font { juce::FontOptions (8.5f) });
    g.setColour (axisMinorLabelColour());
    for (const auto& tk : ticks)
    {
        const int x = (int) std::lround (tk.first);
        if (x < strip.getX() || x >= strip.getRight()) continue;
        g.drawLine ((float) x + 0.5f, (float) strip.getY(),
                    (float) x + 0.5f, (float) strip.getY() + 3.0f, 1.0f);
        g.drawText (formatTime (e, tk.second, rate),
                    juce::Rectangle<int> (x + 3, strip.getY() + 3,
                                         std::max (1, strip.getRight() - x - 6),
                                         strip.getHeight() - 5),
                    juce::Justification::left, true);
    }
}

} // namespace sc::axes
