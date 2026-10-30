// SpectroCraft - Filter EQ (graphical curve equalizer).
#include "FilterEQ.h"

#include <cmath>
#include <algorithm>

namespace sc { namespace eq {

namespace {

// Audacity's classic Filter Curve constants (Equalization.cpp): 16384-point
// FFT windows and a 4001-sample Blackman-windowed impulse response.
constexpr int kFftSize = 16384;      // must be a power of two
constexpr int kFilterLen = 4001;     // odd, < kFftSize
constexpr double kLoFreqHz = 20.0;   // curve axis starts at 20 Hz

/** dB gain of the curve at `freqHz`, linear in log10(f). */
double gainAt (const std::vector<EqPoint>& pts, double fHi, double freqHz)
{
    if (pts.empty ()) return 0.0;
    if (freqHz <= pts.front ().freqHz) return pts.front ().gainDb;
    if (freqHz >= pts.back ().freqHz) return pts.back ().gainDb;

    // Binary search for the bracketing pair.
    int lo = 0, hi = (int) pts.size () - 1;
    while (hi - lo > 1)
    {
        const int mid = (lo + hi) / 2;
        if (pts[(size_t) mid].freqHz <= freqHz) lo = mid; else hi = mid;
    }
    const EqPoint& a = pts[(size_t) lo];
    const EqPoint& b = pts[(size_t) hi];
    const double la = std::log10 (std::max (1e-9, a.freqHz));
    const double lb = std::log10 (std::max (1e-9, b.freqHz));
    if (!(lb > la)) return a.gainDb;
    const double t = (std::log10 (std::max (1e-9, freqHz)) - la) / (lb - la);
    return a.gainDb + t * (b.gainDb - a.gainDb);
}

/** Builds the windowed complex filter function H(re,im) for bins 0..N/2.
    Mirrors Audacity's EffectEqualization::CalcFilter(): per-bin linear gain ->
    IFFT to time domain -> Blackman window of length kFilterLen centred on the
    impulse -> causal shift by (kFilterLen-1)/2 -> FFT back. */
void buildFilterFunction (const std::vector<EqPoint>& pts, double rate,
                          juce::dsp::FFT& fft,
                          std::vector<float>& re, std::vector<float>& im)
{
    const int N = kFftSize;
    const double fHi = rate * 0.5;

    // 1) Per-bin linear magnitude (mirrored for the negative frequencies).
    std::vector<float> mag ((size_t) N, 0.0f);
    for (int i = 0; i <= N / 2; ++i)
    {
        const double freq = (double) i * rate / (double) N;
        const double db = gainAt (pts, fHi, freq);
        mag[(size_t) i] = (float) std::pow (10.0, db / 20.0);
    }
    for (int i = 1; i < N / 2; ++i)
        mag[(size_t) (N - i)] = mag[(size_t) i];

    // 2) IFFT the real filter function into the time domain. The packed
    // layout is (re_k, im_k) at [2k, 2k+1] for k = 0..N/2; the imaginary part
    // of a real spectrum is zero.
    std::vector<float> spec ((size_t) 2 * N, 0.0f);
    for (int i = 0; i <= N / 2; ++i)
        spec[(size_t) 2 * i] = mag[(size_t) i];
    fft.performRealOnlyInverseTransform (spec.data());   // time domain in [0..N)

    // 3) Blackman window of length kFilterLen centred on the impulse, then
    // zero-pad everything outside it.
    const int half = (kFilterLen - 1) / 2;
    for (int i = 0; i <= half; ++i)
    {
        const double x = 2.0 * juce::MathConstants<double>::pi
                        * (double) (i + half) / (double) (kFilterLen - 1);
        const float mult = (float) (0.42
                                   - 0.5 * std::cos (x)
                                   + 0.08 * std::cos (2.0 * x));
        spec[(size_t) i] *= mult;
        if (i != 0)
            spec[(size_t) (N - i)] *= mult;
    }
    for (int i = half + 1; i <= N / 2; ++i)
    {
        spec[(size_t) i] = 0.0f;
        spec[(size_t) (N - i)] = 0.0f;
    }

    // 4) Shift right by `half` so the impulse response is causal with its peak
    // at sample `half` (the overlap-add loop compensates for that delay).
    // The time domain occupies spec[0..N): samples [N-half..N-1] are the left
    // side of the centred impulse, wrapped around to the front.
    std::vector<float> shifted ((size_t) N, 0.0f);
    for (int i = 0; i < half; ++i)
    {
        shifted[(size_t) (half + i)] = spec[(size_t) i];
        shifted[(size_t) i] = spec[(size_t) (N - half + i)];
    }
    shifted[(size_t) (2 * half)] = spec[(size_t) half];   // last sample of the window

    // 5) FFT back to the complex frequency domain. The real-only forward
    // transform writes a full N-bin complex spectrum, so this needs 2N floats.
    std::vector<float> packed ((size_t) 2 * N, 0.0f);
    std::copy (shifted.begin (), shifted.end (), packed.begin ());
    fft.performRealOnlyForwardTransform (packed.data (), true);

    re.resize ((size_t) N / 2 + 1);
    im.resize ((size_t) N / 2 + 1);
    for (int i = 0; i <= N / 2; ++i)
    {
        re[(size_t) i] = packed[(size_t) 2 * i];
        im[(size_t) i] = packed[(size_t) 2 * i + 1];
    }
}

} // anonymous namespace

double curveGainDb (const std::vector<EqPoint>& pts, double fLo, double fHi,
                    double freqHz)
{
    jassert (fHi > fLo);
    return gainAt (pts, fHi, freqHz);
}

bool isFlatCurve (const std::vector<EqPoint>& pts)
{
    for (const auto& p : pts)
        if (std::abs (p.gainDb) > 1e-6)
            return false;
    return true;
}

void applyEq (float* data, int len, double rate, const std::vector<EqPoint>& pts)
{
    if (data == nullptr || len <= 0 || !(rate > 0.0))
        return;
    if (isFlatCurve (pts))
        return;   // flat curve is the identity: stay bit-exact

    const int N = kFftSize;
    juce::dsp::FFT fft ((int) std::log2 ((double) N));
    std::vector<float> hRe, hIm;
    buildFilterFunction (pts, rate, fft, hRe, hIm);

    // Overlap-add: hop L = N - (kFilterLen-1); each window's filtered output is
    // added over its full N-sample span, so consecutive windows overlap by
    // exactly kFilterLen-1 samples. The causal shift of the impulse response
    // delays every window by half = (kFilterLen-1)/2 samples; reading from
    // `half` removes it (zero-phase result).
    const int L = N - (kFilterLen - 1);
    const int half = (kFilterLen - 1) / 2;

    std::vector<float> out ((size_t) len + (size_t) N, 0.0f);
    std::vector<float> win ((size_t) 2 * N, 0.0f);

    for (int64 start = 0; start < len; start += L)
    {
        const int wcopy = std::min (L, len - (int) start);
        std::copy (data + start, data + start + wcopy, win.begin ());
        std::fill (win.begin () + wcopy, win.begin () + N, 0.0f);
        std::fill (win.begin () + N, win.end (), 0.0f);

        fft.performRealOnlyForwardTransform (win.data (), true);
        for (int i = 0; i <= N / 2; ++i)
        {
            const float re = win[(size_t) 2 * i];
            const float im = win[(size_t) 2 * i + 1];
            win[(size_t) 2 * i]     = re * hRe[(size_t) i] - im * hIm[(size_t) i];
            win[(size_t) 2 * i + 1] = re * hIm[(size_t) i] + im * hRe[(size_t) i];
        }
        fft.performRealOnlyInverseTransform (win.data());

        const int64 limit = std::min<int64> ((int64) N, (int64) out.size () - start);
        for (int j = 0; j < limit; ++j)
            out[(size_t) (start + j)] += win[(size_t) j];
    }

    std::copy (out.begin () + half, out.begin () + half + len, data);
}

const std::vector<EqPreset>& presets()
{
    static const std::vector<EqPreset> list = {
        { "Flat", {
            { 1000.0, 0.0 },
        }},
        // Audacity's built-in default curves (presets/EQDefaultCurves.xml).
        { "AM Radio", {
            { 20, -63.67 }, { 31, -33.219 }, { 50, -3.01 }, { 63, -0.106 },
            { 100, 0 }, { 2500, 0 }, { 4000, -0.614 }, { 5000, -8.059 },
            { 8000, -39.981 }, { 20000, -103.651 }, { 48000, -164.485 },
        }},
        { "Telephone", {
            { 20, -94.087 }, { 200, -14.254 }, { 250, -7.243 }, { 315, -2.245 },
            { 400, -0.414 }, { 500, 0 }, { 2500, 0 }, { 3150, -0.874 },
            { 4000, -3.992 }, { 5000, -9.993 }, { 48000, -88.117 },
        }},
        { "Walkie-talkie", {
            { 100, -120 }, { 101, 0 }, { 2000, 0 }, { 2001, -120 },
        }},
        { "100Hz Rumble", {
            { 20, -80 }, { 49.2373, -33.1077 }, { 54.196, -29.5538 },
            { 88.0336, -6.92308 }, { 95.8719, -4.52308 }, { 108.957, -1.93846 },
            { 123.828, -0.738462 }, { 149.228, -0.092308 },
        }},
        { "RIAA", {
            { 20, 19.274 }, { 25, 18.954 }, { 31, 18.516 }, { 40, 17.792 },
            { 50, 16.946 }, { 63, 15.852 }, { 80, 14.506 }, { 100, 13.088 },
            { 125, 11.563 }, { 160, 9.809 }, { 200, 8.219 }, { 250, 6.677 },
            { 315, 5.179 }, { 400, 3.784 }, { 500, 2.648 }, { 630, 1.642 },
            { 800, 0.751 }, { 1000, 0 }, { 1250, -0.744 }, { 1600, -1.643 },
            { 2000, -2.589 }, { 2500, -3.7 }, { 3150, -5.038 }, { 4000, -6.605 },
            { 5000, -8.21 }, { 6300, -9.98 }, { 8000, -11.894 },
            { 10000, -13.734 }, { 12500, -15.609 }, { 16000, -17.708 },
            { 20000, -19.62 }, { 25000, -21.542 }, { 48000, -27.187 },
        }},
        { "Bass Boost", {
            { 100, 9 }, { 500, 0 },
        }},
        { "Bass Cut", {
            { 150, -50 }, { 300, 0 },
        }},
        { "Treble Boost", {
            { 4000, 0 }, { 5000, 9 },
        }},
        { "Treble Cut", {
            { 6000, 0 }, { 10000, -110 },
        }},
        { "Low rolloff for speech", {
            { 50, -120 }, { 60, -50 }, { 65, -24 }, { 70, -12 },
            { 80, -4 }, { 90, -1 }, { 100, 0 },
        }},
    };
    return list;
}

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
    const StringRef t = text.trim ();
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
    const String s = t;   // StringRef has no getDoubleValue in this build
    out = s.getDoubleValue ();
    return std::isfinite (out);
}
} // namespace

juce::String serializePoints (const std::vector<EqPoint>& pts)
{
    juce::StringArray parts;
    for (const auto& p : pts)
        if (std::isfinite (p.freqHz) && p.freqHz > 0.0 && std::isfinite (p.gainDb))
            parts.add (formatNumber (p.freqHz) + "," + formatNumber (p.gainDb));
    return parts.joinIntoString (";");
}

std::vector<EqPoint> parsePoints (const juce::String& text)
{
    std::vector<EqPoint> out;
    juce::StringArray pairs;
    // 3-arg form: the 2-arg call would bind to the bool overload and split on
    // whitespace instead of ';'.
    pairs.addTokens (text, ";", "");
    for (const auto& pair : pairs)
    {
        juce::StringArray tokens;
        tokens.addTokens (pair, ",", "");
        if (tokens.size () != 2) continue;
        double f = 0.0, g = 0.0;
        if (!parseNumber (tokens[0], f) || !parseNumber (tokens[1], g)) continue;
        if (!(f > 0.0)) continue;
        out.push_back ({ f, g });
    }
    std::sort (out.begin (), out.end (),
               [] (const EqPoint& a, const EqPoint& b) { return a.freqHz < b.freqHz; });
    return out;
}

juce::String serializeUserPresets (const std::vector<EqPreset>& presets)
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

std::vector<EqPreset> parseUserPresets (const juce::String& text)
{
    std::vector<EqPreset> out;
    for (const auto& line : juce::StringArray::fromLines (text))
    {
        const int bar = line.indexOf ("|");
        if (bar <= 0) continue;
        const juce::String name = line.substring (0, bar).trim ();
        if (name.isEmpty ()) continue;
        auto pts = parsePoints (line.substring (bar + 1));
        if (pts.empty ()) continue;
        bool dup = false;
        for (const auto& p : out)
            if (p.name == name) { dup = true; break; }
        if (!dup) out.push_back ({ name, std::move (pts) });
    }
    return out;
}

}} // namespace sc::eq
