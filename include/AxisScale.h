// SpectroCraft - axis scale entries: the model behind the Scale form.
//
// One entry describes one axis of a track: its presentation name, the unit it
// is labelled in, its offset/scale, whether it is drawn as an extra axis strip
// or only reported in the status bar ("Footer Only", the old alternative
// scale), and whether it is visible. Entries are ordered; the FIRST visible
// "Add Axis" entry of a kind drives the track's main axis exactly like the old
// single-mode settings did, the following ones add extra strips next to it.
//
// The entries are the source of truth for the Scale form and for the file
// metadata (MetaBundle::axes). They are also translated into the session's
// existing fields (timeAxisMode / timeOffsetSecs / waveVScaleMode /
// specFreq* / scale calibration / alt refs) so the views keep working from the
// state they always used - see applyEntries().
#pragma once
#include <JuceHeader.h>
#include "ScaleCalibration.h"   // Legacy::scale

class Application;   // applyEntries()

namespace sc::axes
{

enum class Kind : int { Time = 0, Amplitude = 1, Frequency = 2 };

/** How an entry is shown: an extra axis strip in the track, or only its value
    in the status bar (the behaviour of the old "Alternative Scale"). */
enum class Type : int { AddAxis = 0, FooterOnly = 1 };

struct Entry
{
    Kind kind = Kind::Time;
    juce::String name;
    int units = 0;                    // index into unitNames (kind)
    double offset = 0.0;              // Time only: displayed = (t + offset) * scale
    double scale = 1.0;               // Amplitude / Frequency multiplier
    std::vector<double> chanScale;    // Amplitude: one multiplier per channel (empty = scale)
    bool visible = true;
    Type type = Type::AddAxis;

    double scaleFor (int ch) const
    {
        if (ch >= 0 && ch < (int) chanScale.size() && chanScale[(size_t) ch] != 0.0)
            return chanScale[(size_t) ch];
        return scale;
    }
    bool hasPerChannelScale() const { return !chanScale.empty(); }
    bool isAddAxis() const { return type == Type::AddAxis; }

    /* Two entries are the same when every displayed field matches: the Scale
       form uses this to avoid recording an undo step for a no-op edit. */
    bool operator== (const Entry& o) const noexcept
    {
        return kind == o.kind && name == o.name && units == o.units
            && offset == o.offset && scale == o.scale && chanScale == o.chanScale
            && visible == o.visible && type == o.type;
    }
    bool operator!= (const Entry& o) const noexcept { return ! (*this == o); }
};

using List = std::vector<Entry>;

// ------------------------------------------------------------------ vocabulary

const char* kindName (Kind k);              // "Time" / "Amplitude" / "Frequency"
const char* typeName (Type t);              // "Axis and Footer" / "Footer Only"

/** The unit choices of a kind, in combo order.
    Time:      "hh:mm:ss.xxxx", "ss.xxxx", "samples"
    Amplitude: "dB", "None", "Volt", "Pascal", "g"
    Frequency: "Hz", "%", "None"
    Amplitude "None" is the uncalibrated plain -1..1 axis; a physical unit
    calibrates the file (the old Files panel "Set Unit and Scale"). */
const juce::StringArray& unitNames (Kind k);
int defaultUnits (Kind k);

/** Default entries for a document without stored axis state: one axis per kind
    with the built-in names ("Time hh:mm:ss.xxxx", "Amplitude dB",
    "Frequency Hz"). */
List defaults();

// ------------------------------------------------- migration and application

/** Builds entries from the state a document may already carry (the old
    calibration + alternative references + custom frequency units + time-axis
    mode), so existing files keep their calibration and their alternative
    scales. `hadLegacy` is true when any of those was present, in which case
    the caller keeps these entries instead of the built-in defaults. */
struct Legacy
{
    ScaleCalibration scale;
    int timeAxisMode = 0;         // TimeAxisMode
    double timeOffsetSecs = 0.0;
    int waveVScaleMode = 0;
    int specFreqScaleMode = 0;
    double specFreqFactor = 1.0;
    std::vector<std::pair<juce::String, std::pair<double, double>>> altTime, altFreq;
        // name -> {refMain, altValue}
};
List migrateLegacy (const Legacy& in, bool& hadLegacy);

/** Amplitude units index (unitNames (Kind::Amplitude)) of a calibration's
    physical unit: Volt 2, Pascal 3, g 4, otherwise 1 ("None"). */
int unitsFromCalibration (const ScaleCalibration& sc);
/** The calibration unit an Amplitude units index stands for (None for dB / None). */
ScaleCalibration::Unit calibrationUnit (int units);

/** Writes the entries into the session's existing display state: the primary
    entry of each kind drives timeAxisMode / timeOffsetSecs / waveVScaleMode /
    the file's ScaleCalibration / specFreqScaleMode + factor, and every
    "Footer Only" entry becomes an alternative-scale reference (status bar
    readouts, Go to Time fields, export columns). */
void applyEntries (Application& app, const List& entries);

// ------------------------------------------------------------- selection

const Entry* primary (const List& entries, Kind k);   // first visible Add Axis
int countAddAxis (const List& entries, Kind k);       // visible Add Axis entries
int countKind (const List& entries, Kind k);          // entries of that kind at all
List extras (const List& entries, Kind k);            // visible Add Axis after the primary
List footerOnly (const List& entries, Kind k);        // visible "Footer Only" entries
/** Every visible entry of a kind except its main axis, in list order: the
    "Axis and Footer" extras and the "Footer Only" entries. These are the
    footer readouts, the Go to Time fields and the extra table columns. */
List secondary (const List& entries, Kind k);
/** True when entries[index] is one of secondary (entries, its kind). */
bool isSecondary (const List& entries, int index);

/** Suggested name for a new axis of that kind ("Frequency Hz 2"). */
juce::String defaultName (Kind k, int unitsIndex);

// ------------------------------------------------------------- formatting

/** Time / amplitude / frequency value of an entry, formatted for its unit.
    `rate` is the sample rate (samples unit), `nyq` the frequency the 100 %
    point of the "%" unit stands for. */
juce::String formatTime (const Entry& e, double tSecs, double rate);
bool parseTime (const Entry& e, const juce::String& text, double rate, double& outSecs);
juce::String formatAmplitude (const Entry& e, double normalised, int ch);
juce::String formatFrequency (const Entry& e, double fHz, double nyq);
/** The same value as an AXIS LABEL: with the unit written out, the way the
    main frequency axis labels itself ("500 Hz", "1.5 kHz", "12 %"). Extra
    frequency axes use this so they read like the axis they belong to. */
juce::String formatFrequencyAxis (const Entry& e, double fHz, double nyq);

/** A tick value with every digit it carries, up to maxDecimals, without
    trailing zeros: 1550 -> "1550", 1.25 -> "1.25", 12.5 -> "12.5". Axis ticks
    are nice values, so this is exact; rounding them to one decimal put
    "1.6 kHz" next to a line at 1550 Hz (issue #54). */
juce::String formatTickNumber (double v, int maxDecimals = 3);

/** An extra frequency axis' tick label for a DISPLAYED value of its unit
    (not Hz): "1.25 kHz" / "500 Hz", "12.5 %", or the plain number. */
juce::String formatFrequencyTick (const Entry& e, double displayed);

/* --------------------------------------------------------------------------
   The inverse of the formatters: what an entry's offset / scale must be for a
   given point to show a given value. The Scale form's crosshair buttons use
   these (playhead, selection ends, a clicked reference), and they are the
   counterpart of displayValueTime / displayValueFrequency / formatAmplitude.
   ------------------------------------------------------------------------ */

/** Offset that makes `tSecs` display as `displayed`, keeping the scale. */
double offsetForDisplayedValue (const Entry& e, double tSecs, double displayed, double rate);

/** Offset and scale that make t0 -> v0 and t1 -> v1. Sample units have no
    scale factor, so only the offset is set (returns false to say so). */
bool solveTimePoints (Entry& e, double t0, double v0, double t1, double v1, double rate);

/** Scale that makes `mainValue` (the axis's own units: normalized amplitude, or
    Hz) display as `typedValue`. NaN when the reference cannot give one. */
double scaleForReference (const Entry& e, double mainValue, double typedValue, double nyq);

/** The value an entry maps a raw axis position to (used by the extra strips
    and the status bar). */
double displayValueTime (const Entry& e, double tSecs, double rate);
double displayValueFrequency (const Entry& e, double fHz, double nyq);

// ------------------------------------------------------------------- JSON

juce::var toJson (const List& entries);
/** Parses the "axes" array. Returns false and leaves `out` untouched when the
    key was absent or unreadable (`hadKey` reports the key either way). */
bool fromJson (const juce::var& v, List& out, bool& hadKey);

/** Shared geometry of the tracks: every track reserves the same horizontal
    space for its vertical axes (the widest one wins, the others get empty
    space) and the same height for its time rulers. */
/** Width of the MAIN vertical axis strip (the historical 60 px). */
int primaryStripWidth();
/** Width of an EXTRA strip: narrower than the main one, so adding a second
    amplitude or frequency axis does not double the space the plot loses. */
int extraStripWidth();
/** Room the channel label ("L", "R", "1 2 3 4") keeps at the OUTER edge of
    the axis area, so the unit labels of the outermost strip never sit under
    it. */
int channelGutter();

/** Where the tick labels of one vertical axis strip are drawn: inside that
    strip, inset at the outer edge of the whole axis area by the channel
    label's room. `index` / `count` are the axis position and total, so only
    the outermost strip is inset. `gutter` is that room: channelGutter() for
    amplitude labels (the zero line's sits at the row centre, beside the
    channel label), 0 for frequency labels, which skip the row centre. */
juce::Rectangle<int> verticalLabelRect (int index, int count, int axisW,
                                        int plotX, int plotRight, int top, int bottom,
                                        bool axisOnRight, int gutter);
/** Total horizontal space the vertical axes reserve: the main strip plus one
    extra per additional visible axis. */
int axisStripWidth (const Application& app);
int primaryRulerHeight();                 // unchanged 24 px ruler
int extraRulerHeight();                  // a narrower secondary time axis
int rulerStackHeight (const Application& app);   // primary + every extra ruler

/** Rectangle of the `index`-th of `count` vertical axis strips (0 = the one
    next to the plot, higher indexes further out), inside the shared `axisW`
    space. On a left axis the outermost strip also holds `gutter`, the channel
    label's room: the main strip gives it up when it is not the outermost one,
    so an extra strip keeps its whole width for its labels and the total stays
    primaryStripWidth() + extras (issue #27). Pass the same gutter as to
    verticalLabelRect (0 = labels that never meet the channel label). */
juce::Rectangle<int> verticalStrip (int index, int count, int axisW, int plotX, int plotRight,
                                    int top, int bottom, bool axisOnRight, int gutter);
/** Rectangle of the `index`-th time ruler (0 = the normal one). The primary
    ruler sits at the outer edge of the stack, the extras between it and the
    plot, so the main axis always looks the same. */
juce::Rectangle<int> timeStrip (int index, int x, int width, int plotTop,
                                int plotBottom, int stackHeight, bool rulerAtBottom);

/** Axis ticks collected while the main axis is drawn: a pixel position and the
    MAIN-axis value at it. Extra axes label the very same positions in their
    own units, so all the axes agree with the grid that is actually drawn. */
using Ticks = std::vector<std::pair<float, double>>;

/** The one look every track's vertical axis uses: the same label size and
    colour, the same grid line, the same axis-strip background. */
juce::Font axisLabelFont();
juce::Font axisMinorLabelFont();
juce::Colour axisLabelColour();
juce::Colour axisMinorLabelColour();
juce::Colour axisGridColour();
juce::Colour axisStripFill();

/** Labels one extra axis strip for the collected ticks. `mainIsTime` selects
    the value conversion (seconds / normalised amplitude / Hz). */
void drawExtraVerticalAxis (juce::Graphics& g, const Entry& e,
                            juce::Rectangle<int> strip, const Ticks& ticks,
                            int channelForLabels, double rate, double nyq);
void drawExtraTimeAxis (juce::Graphics& g, const Entry& e,
                        juce::Rectangle<int> strip, const Ticks& ticks,
                        double rate);

} // namespace sc::axes
