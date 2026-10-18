#pragma once
// SpectroCraft - per-file scale calibration (physical units at full scale)
#include <JuceHeader.h>
#include <cmath>

/** Physical-unit calibration for one loaded file.
    When the user sets e.g. "2 Pascal", a full-scale sample (|1.0|) corresponds
    to 2 Pa, so relative-dB readouts can be converted into absolute levels:
      - Pascal -> dB-SPL   (reference 20 uPa)
      - Volts  -> dBV      (reference 1 V)
      - g      -> dB/g     (reference 1 g)
    "None" leaves every readout as plain relative dB. */
struct ScaleCalibration
{
    enum class Unit { None, Volts, Pascal, Grams };

    Unit unit = Unit::None;
    double fullScaleValue = 0.0;   // physical value at |1.0| (in the chosen unit)

    bool isCalibrated() const { return unit != Unit::None && fullScaleValue > 0.0; }

    /** dB offset added to relative-dB readouts: 20*log10(fullScaleValue/ref). */
    double levelOffsetDb() const
    {
        if (!isCalibrated()) return 0.0;
        const double ref = (unit == Unit::Pascal) ? 2e-5 : 1.0;
        return 20.0 * std::log10 (fullScaleValue / ref);
    }

    /** Label for absolute level readouts: "dB-SPL" / "dBV" / "dB/g" / "dB". */
    juce::String levelUnitLabel() const
    {
        switch (unit)
        {
            case Unit::Pascal: return "dB-SPL";
            case Unit::Volts:  return "dBV";
            case Unit::Grams:  return "dB/g";
            default:           return "dB";
        }
    }

    /** Axis title for linear time-domain values: "Pa" / "V" / "g" (or "dB"). */
    juce::String unitTitle() const
    {
        switch (unit)
        {
            case Unit::Pascal: return "Pa";
            case Unit::Volts:  return "V";
            case Unit::Grams:  return "g";
            default:           return "dB";
        }
    }

    /** Suffix for linear time-domain values: " Pa" / " V" / " g". */
    juce::String unitSuffix() const
    {
        switch (unit)
        {
            case Unit::Pascal: return " Pa";
            case Unit::Volts:  return " V";
            case Unit::Grams:  return " g";
            default:           return "";
        }
    }

    /** Short description for the file list, e.g. "2 Pa" (empty when unset). */
    juce::String shortDescription() const
    {
        if (!isCalibrated()) return {};
        return formatPhysicalValue (fullScaleValue) + unitSuffix();
    }

    static const char* unitName (Unit u)
    {
        switch (u)
        {
            case Unit::Volts:  return "Volts";
            case Unit::Pascal: return "Pascal";
            case Unit::Grams:  return "g";
            default:           return "None";
        }
    }

    /** Trimmed physical value, e.g. 2 -> "2", 0.5 -> "0.5", 0.126 -> "0.126". */
    static juce::String formatPhysicalValue (double v)
    {
        const double a = std::abs (v);
        if (a == 0.0) return "0";
        int dp;
        if      (a >= 100.0) dp = 0;
        else if (a >= 10.0)  dp = 1;
        else if (a >= 1.0)   dp = 2;
        else                 dp = 3;
        // JUCE's String(double, 0) falls back to the default 6-significant-digit
        // format, so round explicitly for whole-number values.
        juce::String s (dp == 0 ? juce::String ((int) std::lround (v)) : juce::String (v, dp));
        while (s.endsWith ("0") && s.contains ("."))
            s = s.substring (0, s.length() - 1);
        if (s.endsWith ("."))
            s = s.substring (0, s.length() - 1);
        return s;
    }

    /** Absolute level readout for a relative-dB value: "-18.1 dB-SPL" / "-96.1 dB". */
    static juce::String formatLevelDb (double relativeDb, const ScaleCalibration& sc)
    {
        const double v = (relativeDb > -290.0 ? relativeDb : -300.0) + sc.levelOffsetDb();
        return juce::String (v, 1) + " " + sc.levelUnitLabel();
    }

    /** Waveform axis label for a grid line `gridDb` dB below full scale. */
    static juce::String formatWaveformAxisValue (double gridDb, const ScaleCalibration& sc)
    {
        if (!sc.isCalibrated()) return juce::String ((int) gridDb);
        return formatPhysicalValue (sc.fullScaleValue * std::pow (10.0, gridDb / 20.0));
    }

    /** Linear time-domain value under the cursor: "0.42" or "1.68 Pa". */
    static juce::String formatLinearValue (double normalized, const ScaleCalibration& sc)
    {
        if (!sc.isCalibrated()) return juce::String (normalized, 2);
        return formatPhysicalValue (normalized * sc.fullScaleValue) + sc.unitSuffix();
    }

    /** True when frequency/spectral level readouts should be plain physical values
        ("V" / "g") instead of dB: volts and grams are linear units. */
    bool usesLinearLevels() const { return unit == Unit::Volts || unit == Unit::Grams; }

    /** Unit label for spectral readouts/exports: "V"/"g" when linear, else the dB label. */
    juce::String spectralUnitLabel() const
    {
        return usesLinearLevels() ? unitTitle() : levelUnitLabel();
    }

    /** Linear level value with ~`sig` significant digits: "1", "0.251", "3.16e-5". */
    static juce::String formatLevelValue (double v, int sig = 3)
    {
        if (!std::isfinite (v) || v == 0.0) return "0";
        const double a = std::abs (v);
        const int e = (int) std::floor (std::log10 (a));
        if (e >= sig - 1 && e < 9)
        {
            // Round to the appropriate power of ten, no decimals.
            const double p = std::pow (10.0, e - (sig - 1));
            return juce::String ((int) std::lround (v / p) * (int) p);
        }
        if (e >= -(sig + 2))
        {
            const int dp = juce::jmax (0, sig - 1 - e);
            if (dp == 0) return juce::String ((int64_t) std::llround (v));
            juce::String s (v, dp);
            while (s.endsWith ("0") && s.contains ("."))
                s = s.substring (0, s.length() - 1);
            if (s.endsWith ("."))
                s = s.substring (0, s.length() - 1);
            return s;
        }
        // Scientific notation for very small values: "3.16e-5".
        const double m = v / std::pow (10.0, e);
        juce::String s (m, sig - 1);
        while (s.endsWith ("0") && s.contains ("."))
            s = s.substring (0, s.length() - 1);
        if (s.endsWith ("."))
            s = s.substring (0, s.length() - 1);
        return s + "e" + juce::String (e);
    }

    /** Level readout for frequency/spectral displays. Volts/Grams show the linear
        physical value ("0.42 V"); Pascal shows dB-SPL; uncalibrated shows plain dB. */
    static juce::String formatSpectralLevel (double relativeDb, const ScaleCalibration& sc)
    {
        if (!sc.isCalibrated()) return juce::String (relativeDb, 1) + " dB";
        if (sc.usesLinearLevels())
            return formatLevelValue (sc.fullScaleValue * std::pow (10.0, relativeDb / 20.0))
                   + sc.unitSuffix();
        return formatLevelDb (relativeDb, sc);
    }

    /** FFT-view axis label for a grid line `gridDb` dB below full scale.
        Volts/Grams show the linear value with unit ("0.251 V"); Pascal shows the
        absolute level; uncalibrated keeps plain relative dB. */
    static juce::String formatAxisLabel (int gridDb, const ScaleCalibration& sc)
    {
        if (!sc.isCalibrated()) return juce::String (gridDb);
        if (sc.usesLinearLevels())
            return formatLevelValue (sc.fullScaleValue * std::pow (10.0, gridDb / 20.0))
                   + sc.unitSuffix();
        return juce::String ((int) std::lround ((double) gridDb + sc.levelOffsetDb()));
    }
};
