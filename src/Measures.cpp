// SpectroCraft - headless measurement module (see include/Measures.h).
//
// The spectrum / 1/3-octave / curve computations below were moved verbatim
// out of src/Panels.cpp (FFTAnalysisView) so the panel and the measure.*
// commands share one implementation; only the window type became an argument
// and the long loops gained a progress/cancel callback. Keep the arithmetic
// byte-for-byte identical: tests/regression/measures.inc checks the panel's
// caches against this module with exact equality.
#include "Measures.h"
#include "FFTHelpers.h"
#include "SpectralSelection.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace sc::measures
{
// ------------------------------------------------------------- band tables

// Standard IEC 61260-1 1/3-octave centre frequencies, 50 Hz .. 16 kHz.
static const double kThirdOctCenters[] = {
    50.0, 63.0, 80.0, 100.0, 125.0, 160.0, 200.0, 250.0, 315.0, 400.0,
    500.0, 630.0, 800.0, 1000.0, 1250.0, 1600.0, 2000.0, 2500.0, 3150.0,
    4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0, 16000.0
};
static constexpr int kNumThirdOctBands = (int) (sizeof (kThirdOctCenters) / sizeof (kThirdOctCenters[0]));

// Decade-based 1/3-octave band centres of the dB-SPL pipeline:
// fc(i) = 1000 * 10^((i-30)/10), i = 17..43 (50.12 Hz .. 19952.6 Hz).
static const double kThirdOptCentersRef[] = {
    50.1187,   63.0957,   79.4328,  100.0,     125.8925,  158.4893,
    199.5262,  251.1886,  316.2278,  398.1072,  501.1872,  630.9573,
    794.3282,  1000.0,    1258.9254, 1584.8932, 1995.2623, 2511.8864,
    3162.2777, 3981.0717, 5011.8723, 6309.5734, 7943.2823, 10000.0,
    12589.2541, 15848.9319, 19952.6231
};
static const int kNumThirdOptBandsRef = 27;

const double* iecThirdOctCentres() noexcept { return kThirdOctCenters; }
int numIecThirdOctBands() noexcept { return kNumThirdOctBands; }
const double* refThirdOctCentres() noexcept { return kThirdOptCentersRef; }
int numRefThirdOctBands() noexcept { return kNumThirdOptBandsRef; }

// ------------------------------------------------------------- small helpers

const char* windowName (int windowType) noexcept
{
    switch (windowType)
    {
        case Rectangular: return "rectangular";
        case Hamming:     return "hamming";
        default:          return "hann";
    }
}

int windowTypeFromName (const juce::String& name)
{
    const auto s = name.trim().toLowerCase();
    if (s == "rectangular" || s == "rect" || s == "none" || s == "boxcar") return Rectangular;
    if (s == "hann" || s == "hanning") return Hann;
    if (s == "hamming") return Hamming;
    return -1;
}

static bool wantsCancel (const ProgressFn& progress, double fraction)
{
    return progress && !progress (std::clamp (fraction, 0.0, 1.0));
}

static bool validFftSize (int n) noexcept
{
    return n >= 2 && n <= 65536 && (n & (n - 1)) == 0;
}

// Analysis window of the classic spectrum path (0 = rectangular, 1 = Hann,
// 2 = Hamming), shared by the live spectrum, snapshots and the band ENBW.
std::vector<float> makeAnalysisWindow (int wt, int n)
{
    std::vector<float> win ((size_t) n, 1.0f);
    if (wt == 1 || wt == 2)
        for (int i = 0; i < n; ++i)
            win[(size_t) i] = (wt == 1)
                ? 0.5f * (1.0f - (float) std::cos (2.0 * juce::MathConstants<double>::pi * i / n))
                : 0.54f - 0.46f * (float) std::cos (2.0 * juce::MathConstants<double>::pi * i / (n - 1));
    return win;
}

// Equivalent noise bandwidth of a window, in bins: n * sum(w^2) / sum(w)^2
// (1.0 rectangular, 1.5 Hann, ~1.36 Hamming).
double windowEnbwBins (const std::vector<float>& win)
{
    double s1 = 0.0, s2 = 0.0;
    for (float w : win) { s1 += w; s2 += (double) w * w; }
    return s1 > 0.0 ? (double) win.size() * s2 / (s1 * s1) : 1.0;
}

double enbwBins (int windowType, int n)
{
    return windowEnbwBins (makeAnalysisWindow (windowType, n));
}

// Convert normalized [0..1] band values to the view's relative-dB space.
std::vector<float> bandsNormalizedToDb (const std::vector<float>& norm)
{
    std::vector<float> out (norm.size());
    for (size_t i = 0; i < norm.size(); ++i)
        out[i] = (float) (-kNormFloorDb * (1.0 - (double) norm[i]));
    return out;
}

// Sum an FFT magnitude curve into 1/3-octave band levels.
// mag holds dB-normalized RMS amplitudes in [0,1] (see regionSpectrumNorm) for
// an n-point FFT at the given rate; returns one normalized value per band
// whose centre is <= Nyquist. The band level is the SUM of the bin powers
// inside the band (a bin straddling an edge contributes its overlapping
// fraction, so narrow low bands are never empty), divided by the window's
// ENBW so that both a tone and broadband noise read their true band power,
// independent of the FFT size. Band edges come from the exact base-10 centre
// behind each nominal label (fc * 10^(+-1/20)), so adjacent bands neither
// overlap nor leave gaps.
std::vector<float> toThirdOctBands (const std::vector<float>& mag, int n,
                                    double rate, double enbwBins)
{
    const double nyq = rate * 0.5;
    // Same fixed normalization floor as the live curve (see kNormFloorDb).
    const double dBMin = -160.0;
    const double binHz = rate / (double) n;
    const int lastBin = std::min (n / 2, (int) mag.size() - 1);
    std::vector<float> out;
    for (int b = 0; b < kNumThirdOctBands && kThirdOctCenters[b] <= nyq; ++b)
    {
        const double idx = std::round (10.0 * std::log10 (kThirdOctCenters[b] / 1000.0));
        const double fc = 1000.0 * std::pow (10.0, idx / 10.0);
        const double lo = fc * std::pow (10.0, -0.05);
        const double hi = fc * std::pow (10.0,  0.05);
        const int kLo = std::max (1, (int) std::floor (lo / binHz + 0.5));
        const int kHi = std::min (lastBin, (int) std::floor (hi / binHz + 0.5));

        double sumPow = 0.0;
        for (int k = kLo; k <= kHi; ++k)
        {
            const double overlap = std::min ((k + 0.5) * binHz, hi)
                                 - std::max ((k - 0.5) * binHz, lo);
            if (overlap <= 0.0) continue;
            const double dB = dBMin * (1.0 - (double) mag[(size_t) k]);   // undo normalization
            sumPow += std::pow (10.0, dB / 10.0) * (overlap / binHz);
        }
        const double p = sumPow / std::max (1e-9, enbwBins);
        const double dBb = 10.0 * std::log10 (std::max (1e-24, p));
        out.push_back ((float) std::clamp ((dBb - dBMin) / (-dBMin), 0.0, 1.0));
    }
    return out;
}

// Frame placement shared by both curve computations: frames of `fftN`
// samples covering [s0, s1] with a hop that yields at most kMaxCurveFrames
// points. A region shorter than one window gets a single frame centred on it
// (clamped inside the file).
std::vector<int64_t> curveFrameStarts (double totalSamples, int64_t s0, int64_t s1, int fftN)
{
    std::vector<int64_t> starts;
    const int64_t avail = s1 - s0;
    if (avail <= fftN)
    {
        const int64_t mid = (int64_t) ((s0 + s1) * 0.5);
        starts.push_back (std::clamp<int64_t> (mid - fftN / 2, 0,
                                                std::max<int64_t> (0, totalSamples - fftN)));
        return starts;
    }
    const int64_t hop = std::max<int64_t> (fftN / 4,
                                           (avail + kMaxCurveFrames - 1) / kMaxCurveFrames);
    for (int64_t s = s0; s + fftN <= s1 && (int) starts.size() < kMaxCurveFrames; s += hop)
        starts.push_back (s);
    if (starts.empty())
        starts.push_back (std::clamp<int64_t> (s0, 0,
                                               std::max<int64_t> (0, totalSamples - fftN)));
    return starts;
}

// Where to place the FFT windows of a region spectrum (sample offsets).
std::vector<int64_t> spectrumWindowStarts (const AudioEngine& eng, double t0, double t1,
                                           int n, double overlap, bool* capped)
{
    const double rate = eng.sampleRate();
    const double dur = eng.duration();
    if (capped != nullptr) *capped = false;

    std::vector<int64_t> winStarts;
    const double regionLenS = std::max (0.0, (t1 - t0) * rate);   // in samples
    if (regionLenS <= (double) n)
    {
        int64_t s = (int64_t) ((t0 + t1) * 0.5 * rate) - n / 2;   // centre on the region
        const int64_t maxStart = std::max<int64_t> (0, (int64_t) (dur * rate) - n);
        winStarts.push_back (std::clamp<int64_t> (s, 0, maxStart));
        return winStarts;
    }

    if (overlap < 0.0)
    {
        // The panel's placement. Cap the work: at most kMaxAvgWindows windows
        // and ~4M samples.
        const int64_t uncapped = (int64_t) std::ceil (regionLenS / n);
        const int numW = (int) std::min<int64_t> ({ kMaxAvgWindows,
                                                    uncapped,
                                                    std::max<int64_t> (1, 4000000 / n) });
        if (capped != nullptr) *capped = numW < uncapped;
        for (int i = 0; i < numW; ++i)
        {
            const double s = t0 * rate + ((double) i * (regionLenS - n) / (numW - 1));
            winStarts.push_back ((int64_t) std::clamp (s, 0.0, dur * rate - n));
        }
        return winStarts;
    }

    // Fixed hop n*(1-overlap) from t0; windows fully inside the region.
    overlap = std::min (overlap, 0.95);
    const int64_t hop = std::max<int64_t> (1, (int64_t) std::llround ((double) n * (1.0 - overlap)));
    const int64_t s0 = (int64_t) (t0 * rate);
    const int64_t count = 1 + (int64_t) ((regionLenS - n) / (double) hop);
    const int64_t maxStart = std::max<int64_t> (0, eng.numSamples() - n);
    if (count <= kMaxAvgWindows)
        for (int64_t i = 0; i < count; ++i)
            winStarts.push_back (std::clamp<int64_t> (s0 + i * hop, 0, maxStart));
    else
    {
        if (capped != nullptr) *capped = true;
        for (int64_t i = 0; i < kMaxAvgWindows; ++i)
            winStarts.push_back (std::clamp<int64_t>
                (s0 + (int64_t) ((double) i * (double) (count - 1) / (double) (kMaxAvgWindows - 1)) * hop,
                 0, maxStart));
    }
    return winStarts;
}

// ------------------------------------------------------------ region spectrum

// Core averaging computation shared by the live spectrum and every stored
// snapshot: analyse region [t0, t1] with windows of exactly n samples at the
// given window type. A region wider than one window is covered by several
// evenly spaced windows whose bin powers are averaged (the classic "average
// over the selection" behaviour); a point or short region gets a single
// window centred on it. Each bin is scaled to an RMS amplitude: corrected for
// the window's coherent gain (sum of w) and single-sided (x sqrt(2) except DC
// and Nyquist), so a sine of peak A reads A/sqrt(2) whatever the window - the
// same convention as the dB-SPL 1/3-octave pipeline.
bool regionSpectrumNorm (const AudioEngine& eng, const std::vector<int>& chans,
                         double t0, double t1, int n, int windowType,
                         std::vector<std::vector<float>>& outMags,
                         double overlap, const ProgressFn& progress,
                         int* windowsUsed, bool* windowsCapped)
{
    // Where to place the FFT windows (sample offsets).
    const std::vector<int64_t> winStarts = spectrumWindowStarts (eng, t0, t1, n, overlap, windowsCapped);
    if (windowsUsed != nullptr) *windowsUsed = (int) winStarts.size();

    outMags.clear();
    const int order = (int) std::log2 ((double) n);

    // Window function, computed once and shared by all channels.
    const std::vector<float> win = makeAnalysisWindow (windowType, n);
    double sumW = 0.0;
    for (float w : win) sumW += w;
    sumW = std::max (1e-12, sumW);

    // Fixed normalization floor: the stored magnitudes do not depend on the
    // visible level window, so zooming out never changes the curve data.
    const double dBMin = -kNormFloorDb;
    const double totalWork = std::max<double> (1.0, (double) chans.size() * (double) winStarts.size());
    double done = 0.0;
    for (int ch : chans)
    {
        if (ch < 0 || ch >= eng.numChannels()) { done += (double) winStarts.size(); continue; }

        // Average the bin powers of all windows in this channel.
        std::vector<double> acc ((size_t) (n / 2 + 1), 0.0);
        std::vector<float> samples ((size_t) n, 0.0f);   // zero-filled: partial reads at the file end stay silent
        std::vector<float> buf ((size_t) n * 2, 0.0f);
        for (int64_t s : winStarts)
        {
            if (wantsCancel (progress, done / totalWork)) { outMags.clear(); return false; }
            done += 1.0;
            std::fill (samples.begin(), samples.end(), 0.0f);
            if (!eng.readSamples (ch, s, n, samples.data())) continue;
            for (int i = 0; i < n; ++i)
                buf[i] = samples[i] * win[(size_t) i];
            scfft::realForward (buf.data(), order);      // AVX2 when available via scfft
            for (int k = 0; k <= n / 2; ++k)
            {
                const float re = buf[k * 2], im = buf[k * 2 + 1];
                acc[(size_t) k] += (double) re * re + (double) im * im;
            }
        }

        std::vector<float> mag ((size_t) (n / 2 + 1));
        for (int k = 0; k <= n / 2; ++k)
        {
            const double oneSided = (k == 0 || k == n / 2) ? 1.0 : std::sqrt (2.0);
            const double m = std::sqrt (acc[(size_t) k] / (double) winStarts.size())
                             * oneSided / sumW;
            const double dB = 20.0 * std::log10 (std::max (1e-12, m));
            mag[k] = (float) std::clamp ((dB - dBMin) / (-dBMin), 0.0, 1.0);
        }
        outMags.push_back (std::move (mag));
    }
    if (progress) progress (1.0);
    return true;
}

// ------------------------------------------------------------- calibration

ChannelCalibration channelCalibration (const ScaleCalibration* cal, int ch)
{
    ChannelCalibration c;
    if (cal == nullptr || !cal->isCalibratedFor (ch)) return c;
    c.calibrated = true;
    c.fullScale = cal->fullScaleFor (ch);
    c.levelOffsetDb = cal->levelOffsetDb (ch);
    return c;
}

std::string absUnitFor (const ScaleCalibration* cal)
{
    if (cal == nullptr || !cal->isCalibrated()) return {};
    return cal->levelUnitLabel().toStdString();
}

std::string physicalUnitFor (const ScaleCalibration* cal)
{
    if (cal == nullptr || !cal->isCalibrated()) return {};
    return cal->unitTitle().toStdString();
}

// ------------------------------------------------------------------ spectrum

SpectrumResult spectrum (const AudioEngine& eng, const SpectrumParams& p,
                         const ScaleCalibration* cal, const ProgressFn& progress)
{
    SpectrumResult r;
    r.sampleRate = eng.sampleRate();
    r.fftSize = p.fftSize;
    r.windowType = std::clamp (p.windowType, 0, 2);
    r.absUnit = absUnitFor (cal);
    const double dur = eng.duration();
    r.t0 = std::clamp (p.t0, 0.0, std::max (0.0, dur));
    r.t1 = std::clamp (p.t1, r.t0, std::max (r.t0, dur));
    if (!eng.isFileLoaded() || !(r.sampleRate > 0.0) || !validFftSize (p.fftSize))
        return r;
    r.binHz = r.sampleRate / (double) p.fftSize;
    r.enbwBins = enbwBins (r.windowType, p.fftSize);

    std::vector<int> chans;
    for (int ch : p.channels)
        if (ch >= 0 && ch < eng.numChannels()) chans.push_back (ch);

    std::vector<std::vector<float>> mags;
    if (!regionSpectrumNorm (eng, chans, r.t0, r.t1, p.fftSize, r.windowType, mags,
                             p.overlap, progress, &r.framesAveraged, &r.framesCapped))
    {
        r.cancelled = true;
        return r;
    }
    for (size_t i = 0; i < chans.size() && i < mags.size(); ++i)
    {
        ChannelSpectrum cs;
        cs.ch = chans[i];
        cs.norm = std::move (mags[i]);
        cs.relDb.resize (cs.norm.size());
        for (size_t k = 0; k < cs.norm.size(); ++k)
            cs.relDb[k] = normToDb (cs.norm[k]);
        cs.cal = channelCalibration (cal, cs.ch);
        r.channels.push_back (std::move (cs));
    }
    return r;
}

// -------------------------------------------------------------- third octave

std::vector<float> thirdOctaveRefBands (const AudioEngine& eng, int ch,
                                        double t0, double t1, int order,
                                        const ProgressFn& progress, int* framesUsed)
{
    // Reference-analyzer 1/3-octave pipeline: fixed 65536-point Hanning frames
    // at 50% overlap, frame-averaged power spectrum (no coherent-gain
    // correction), Butterworth bandpass weighting of order 2*`order`, and the
    // classic +98.5 dB offset. The result is expressed in the panel's
    // relative-dB space so that formatSpectralLevel() renders it as dB-SPL
    // once a Pascal full-scale value is set, reproducing the reference
    // analyzer's numbers.
    if (framesUsed != nullptr) *framesUsed = 0;
    const int N = 65536;
    const double rate = eng.sampleRate();
    if (!(rate > 0.0) || ch < 0 || ch >= eng.numChannels()) return {};
    const double dur = eng.duration();
    t0 = std::clamp (t0, 0.0, dur);
    t1 = std::clamp (t1, t0, dur);

    // Frame starts: the reference frames the analysed slice from its start with
    // a hop of N/2; long regions are capped and spread evenly so interactive
    // use stays responsive. A region shorter than one frame gets a single
    // centred (zero-padded) frame.
    std::vector<int64_t> starts;
    const int64_t s0 = (int64_t) (t0 * rate);
    const int64_t s1 = (int64_t) (t1 * rate);
    const int64_t total = eng.numSamples();
    if (s1 - s0 <= N)
        starts.push_back (std::clamp<int64_t> ((s0 + s1) / 2 - N / 2, 0,
                                               std::max<int64_t> (0, total - N)));
    else
    {
        const int64_t hop = N / 2;
        const int64_t full = (s1 - s0) / hop;   // reference frame count for the slice
        const int maxFrames = kRefThirdOctMaxFrames;   // cap for interactive use
        if (full <= maxFrames)
            for (int64_t i = 0; i < full; ++i)
                starts.push_back (s0 + i * hop);
        else
            for (int i = 0; i < maxFrames; ++i)
                starts.push_back (std::clamp<int64_t>
                                  (s0 + (int64_t) ((double) i * (full - 1) / (maxFrames - 1)) * hop,
                                   s0, std::min<int64_t> (s1, total - N)));
    }

    // Symmetric Hanning window exactly as the reference uses it.
    std::vector<float> win ((size_t) N);
    for (int i = 0; i < N; ++i)
        win[i] = 0.5f - 0.5f * std::cos (2.0 * juce::MathConstants<double>::pi
                                          * (double) (i + 1) / (N + 1));

    const int orderLog = 16;   // log2(65536)
    std::vector<float> buf ((size_t) N * 2);
    std::vector<double> psd ((size_t) N / 2, 0.0);
    size_t frameIndex = 0;
    for (int64_t s : starts)
    {
        if (wantsCancel (progress, (double) frameIndex / (double) starts.size())) return {};
        ++frameIndex;
        // Zero-fill first: reads near the file end are clamped and leave
        // the tail untouched, which must stay silent.
        std::fill (buf.begin(), buf.end(), 0.0f);
        if (!eng.readSamples (ch, s, N, buf.data())) continue;
        for (int i = 0; i < N; ++i) buf[i] *= win[i];
        scfft::realForward (buf.data(), orderLog);
        for (int j = 0; j < N / 2; ++j)
            psd[(size_t) j] += (double) buf[j * 2] * buf[j * 2]
                             + (double) buf[j * 2 + 1] * buf[j * 2 + 1];
    }
    // Reference PSD normalisation: divide by frame count and (N+1).
    const double invN = 1.0 / ((double) starts.size() * (N + 1));
    for (auto& v : psd) v *= invN;

    // Butterworth bandpass weighting per the reference's PSD2OneThirdOctave.
    const int nOrd = 2 * order;
    const double lowerF = std::pow (2.0, -1.0 / 6.0), upperF = std::pow (2.0, 1.0 / 6.0);
    const double Qr = 1.0 / (upperF - lowerF);
    const double piC = juce::MathConstants<double>::pi;
    const double Qd = piC * Qr / ((double) nOrd * std::sin (piC / (double) nOrd));
    const double binHz = rate / N;

    std::vector<float> out;
    for (int i = 17; i <= 43; ++i)
    {
        const double fc = 1000.0 * std::pow (10.0, (i - 30) / 10.0);
        double sum = 0.0;
        // Upper scan: from the middle bin upward while the weighting is > 1e-4.
        for (int j = (int) std::floor (fc / binHz); j < N / 2; ++j)
        {
            const double ratio = (double) j * binHz / fc;
            const double gain = 1.0 / (1.0 + std::pow (Qd * (ratio - 1.0 / ratio), nOrd));
            if (gain <= 1e-4) break;
            sum += psd[(size_t) j] * gain;
        }
        // Lower scan: from the bin below the middle downward while > 1e-6.
        for (int j = (int) std::floor (fc / binHz) - 1; j >= 0 && j < N / 2; --j)
        {
            const double ratio = (double) j * binHz / fc;
            const double gain = 1.0 / (1.0 + std::pow (Qd * (ratio - 1.0 / ratio), nOrd));
            if (gain <= 1e-6) break;
            sum += psd[(size_t) j] * gain;
        }
        // Reference dB conversion: 10*log10(sum/(N/2)) + 98.5; empty bands
        // read as -999 (the reference's "no data" sentinel).
        const double L = (sum > 0.0) ? 10.0 * std::log10 (sum / (N / 2)) + 98.5 : -999.0;
        // Express in the panel's relative-dB space: formatSpectralLevel() adds
        // levelOffsetDb(ch) = 20*log10(F/2e-5), so subtracting the dB-SPL
        // reference (20*log10(2e-5)) leaves exactly the reference value.
        out.push_back ((float) L + (float) (20.0 * std::log10 (2e-5)));
    }
    if (framesUsed != nullptr) *framesUsed = (int) starts.size();
    return out;
}

ThirdOctaveResult thirdOctave (const AudioEngine& eng, const ThirdOctaveParams& p,
                               const ScaleCalibration* cal, const ProgressFn& progress)
{
    ThirdOctaveResult r;
    r.mode = p.mode;
    r.order = p.order;
    r.sampleRate = eng.sampleRate();
    r.absUnit = absUnitFor (cal);
    if (!eng.isFileLoaded() || !(r.sampleRate > 0.0)) return r;

    std::vector<int> chans;
    for (int ch : p.channels)
        if (ch >= 0 && ch < eng.numChannels()) chans.push_back (ch);

    if (p.mode == ThirdOctaveParams::Mode::Reference)
    {
        r.fftSize = kRefThirdOctFFT;
        r.windowType = Hann;
        r.framesCap = kRefThirdOctMaxFrames;
        r.centresHz.assign (kThirdOptCentersRef, kThirdOptCentersRef + kNumThirdOptBandsRef);
        if (p.order < 1) return r;
        const float emptyValue = (float) kEmptyBandValue + (float) (20.0 * std::log10 (2e-5));
        for (size_t i = 0; i < chans.size(); ++i)
        {
            const double base = (double) i / (double) chans.size();
            const double span = 1.0 / (double) chans.size();
            ProgressFn sub;
            if (progress)
                sub = [&progress, base, span] (double f) { return progress (base + f * span); };
            int used = 0;
            auto bands = thirdOctaveRefBands (eng, chans[i], p.t0, p.t1, p.order, sub, &used);
            if (bands.empty()) { r.cancelled = true; r.channels.clear(); return r; }
            r.framesUsed = used;
            ThirdOctaveChannel c;
            c.ch = chans[i];
            c.noData.resize (bands.size());
            for (size_t b = 0; b < bands.size(); ++b)
                c.noData[b] = (bands[b] == emptyValue);
            c.relDb = std::move (bands);
            c.cal = channelCalibration (cal, c.ch);
            r.channels.push_back (std::move (c));
        }
        if (progress) progress (1.0);
        return r;
    }

    // IEC mode: bands summed from the averaged spectrum (the panel's
    // non-Pascal 1/3-octave pipeline).
    r.fftSize = p.fftSize;
    r.windowType = std::clamp (p.windowType, 0, 2);
    r.framesCap = (int) kMaxAvgWindows;
    if (!validFftSize (p.fftSize)) return r;
    std::vector<std::vector<float>> mags;
    bool capped = false;
    if (!regionSpectrumNorm (eng, chans, p.t0, p.t1, p.fftSize, r.windowType, mags,
                             -1.0, progress, &r.framesUsed, &capped))
    {
        r.cancelled = true;
        return r;
    }
    const double enbw = enbwBins (r.windowType, p.fftSize);
    for (size_t i = 0; i < chans.size() && i < mags.size(); ++i)
    {
        ThirdOctaveChannel c;
        c.ch = chans[i];
        c.relDb = bandsNormalizedToDb (toThirdOctBands (mags[i], p.fftSize, r.sampleRate, enbw));
        c.noData.assign (c.relDb.size(), false);
        c.cal = channelCalibration (cal, c.ch);
        if (r.centresHz.empty())
            r.centresHz.assign (kThirdOctCenters, kThirdOctCenters + c.relDb.size());
        r.channels.push_back (std::move (c));
    }
    return r;
}

// -------------------------------------------------------------------- curves

bool timeCurve (const AudioEngine& eng, const std::vector<int>& chans,
                double t0, double t1, int track, const SpectralSelection* mask,
                std::vector<CurveData>& out, const ProgressFn& progress)
{
    const double rate = eng.sampleRate();
    const double durS = eng.duration();
    const int64_t totalSamples = (int64_t) (durS * rate);
    const int64_t s0 = (int64_t) std::clamp (t0 * rate, 0.0, (double) totalSamples);
    const int64_t s1 = (int64_t) std::clamp (t1 * rate, (double) s0, (double) totalSamples);
    const int fftN = kTimeModeFFT;

    out.clear();
    for (size_t ci = 0; ci < chans.size(); ++ci)
    {
        const int ch = chans[ci];
        if (ch < 0 || ch >= eng.numChannels()) continue;

        CurveData cd;
        std::vector<int64_t> starts = curveFrameStarts (totalSamples, s0, s1, fftN);

        const int order = (int) std::log2 ((double) fftN);
        // Fixed Hann window for the time-based modes: the Win type combo is
        // hidden there, and a Hann keeps peak tracking free of leakage.
        std::vector<float> win ((size_t) fftN);
        for (int i = 0; i < fftN; ++i)
            win[(size_t) i] = 0.5f * (1.0f - std::cos (2.0 * juce::MathConstants<double>::pi
                                                       * (double) i / (double) fftN));
        double sumW = 0.0;
        for (float w : win) sumW += w;
        const double ampScale = std::sqrt (2.0) / std::max (1e-12, sumW);

        std::vector<float> samples ((size_t) fftN, 0.0f);   // zero-filled: partial reads at the file end stay silent
        std::vector<float> buf ((size_t) fftN * 2, 0.0f);

        size_t fi = 0;
        for (int64_t s : starts)
        {
            if (wantsCancel (progress, ((double) ci + (double) fi / (double) starts.size())
                                           / (double) chans.size()))
            { out.clear(); return false; }
            ++fi;
            const double tMid = ((double) s + fftN / 2.0) / rate;   // frame centre time
            std::fill (samples.begin(), samples.end(), 0.0f);
            if (!eng.readSamples (ch, s, fftN, samples.data())) continue;
            for (int i = 0; i < fftN; ++i)
                buf[i] = samples[i] * win[(size_t) i];
            scfft::realForward (buf.data(), order);

            double maxMag = 0.0, maxF = 0.0, sumLin = 0.0, wsum = 0.0;
            for (int k = 1; k <= fftN / 2; ++k)
            {
                const float re = buf[k * 2], im = buf[k * 2 + 1];
                const double m = std::sqrt ((double) re * re + (double) im * im);
                if (mask != nullptr
                    && !mask->contains (tMid, (double) k * rate / (double) fftN))
                    continue;   // outside the spectral selection: this bin does not count
                const double f = (double) k * rate / (double) fftN;
                if (m > maxMag) { maxMag = m; maxF = f; }
                sumLin += m;
                wsum += m * f;
            }

            // Max: strongest bin's level + frequency. Total / Weighted:
            // summed level + amplitude-weighted average (centroid) frequency.
            // Normalise exactly like the classic spectrum path (coherent gain
            // of the window, single-sided RMS), so a tone of peak A reads
            // A/sqrt(2).
            const double lin = (track == 0 ? maxMag : sumLin) * ampScale;
            cd.x.push_back (tMid);
            cd.ampDb.push_back ((float) std::max (-kNormFloorDb,
                                                  20.0 * std::log10 (std::max (1e-12, lin))));
            cd.freqHz.push_back ((float) ((track == 0) ? maxF
                                                      : (sumLin > 1e-30 ? wsum / sumLin : 0.0)));
        }

        out.push_back (std::move (cd));
    }
    if (progress) progress (1.0);
    return true;
}

bool spectrumCurve (const AudioEngine& eng, const std::vector<int>& chans,
                    double t0, double t1, int n, int windowType, int track,
                    const SpectralSelection* mask, std::vector<CurveData>& out,
                    const ProgressFn& progress)
{
    const double rate = eng.sampleRate();
    const double durS = eng.duration();
    const int64_t totalSamples = (int64_t) (durS * rate);
    const int64_t s0 = (int64_t) std::clamp (t0 * rate, 0.0, (double) totalSamples);
    const int64_t s1 = (int64_t) std::clamp (t1 * rate, (double) s0, (double) totalSamples);

    out.clear();
    for (size_t ci = 0; ci < chans.size(); ++ci)
    {
        const int ch = chans[ci];
        if (ch < 0 || ch >= eng.numChannels()) continue;

        CurveData cd;
        const int numBins = n / 2 + 1;
        std::vector<double> accMax ((size_t) numBins, 0.0);   // per-bin max linear magnitude
        std::vector<double> accSum ((size_t) numBins, 0.0);   // per-bin summed magnitudes

        const int order = (int) std::log2 ((double) n);
        std::vector<float> win ((size_t) n, 1.0f);
        if (windowType == 1 || windowType == 2)
            for (int i = 0; i < n; ++i)
                win[(size_t) i] = (windowType == 1)
                    ? 0.5f * (1.0f - std::cos (2.0 * juce::MathConstants<double>::pi
                                               * (double) i / (double) n))
                    : 0.54f - 0.46f * std::cos (2.0 * juce::MathConstants<double>::pi
                                               * (double) i / ((double) n - 1.0));

        double sumW = 0.0;
        for (float w : win) sumW += w;
        const double ampScale = std::sqrt (2.0) / std::max (1e-12, sumW);

        std::vector<float> samples ((size_t) n, 0.0f);
        std::vector<float> buf ((size_t) n * 2, 0.0f);
        const std::vector<int64_t> starts = curveFrameStarts (totalSamples, s0, s1, n);
        size_t fi = 0;
        for (int64_t s : starts)
        {
            if (wantsCancel (progress, ((double) ci + (double) fi / (double) starts.size())
                                           / (double) chans.size()))
            { out.clear(); return false; }
            ++fi;
            const double tMid = ((double) s + n / 2.0) / rate;
            std::fill (samples.begin(), samples.end(), 0.0f);
            if (!eng.readSamples (ch, s, n, samples.data())) continue;
            for (int i = 0; i < n; ++i)
                buf[i] = samples[i] * win[(size_t) i];
            scfft::realForward (buf.data(), order);
            for (int k = 1; k <= n / 2; ++k)
            {
                if (mask != nullptr
                    && !mask->contains (tMid, (double) k * rate / (double) n))
                    continue;
                const float re = buf[k * 2], im = buf[k * 2 + 1];
                const double m = std::sqrt ((double) re * re + (double) im * im);
                if (m > accMax[(size_t) k]) accMax[(size_t) k] = m;
                accSum[(size_t) k] += m;
            }
        }

        // Same normalisation as the classic spectrum path: window coherent
        // gain and single-sided RMS (and the frame count for Total) so levels
        // are comparable to the averaged spectrum of a single window.
        const double numFrames = std::max<int64_t> (1, starts.size());
        for (int k = 1; k <= n / 2; ++k)
        {
            const double f = (double) k * rate / (double) n;
            const double lin = (track == 0) ? accMax[(size_t) k] * ampScale
                                            : accSum[(size_t) k] * ampScale / (double) numFrames;
            cd.x.push_back (f);
            cd.ampDb.push_back ((float) std::max (-kNormFloorDb,
                                                  20.0 * std::log10 (std::max (1e-12, lin))));
            cd.freqHz.push_back ((float) f);   // the "frequency curve" is the axis itself
        }

        out.push_back (std::move (cd));
    }
    if (progress) progress (1.0);
    return true;
}

void smoothSeries (std::vector<float>& v, int window)
{
    if (window < 2 || v.size() <= (size_t) window) return;
    const size_t half = (size_t) (window - 1) / 2;   // windows are odd: 3 or 5
    std::vector<float> tmp (v);
    for (size_t i = 0; i < v.size(); ++i)
    {
        double acc = 0.0;
        size_t cnt = 0;
        const size_t lo = (i > half) ? i - half : 0;
        const size_t hi = std::min (v.size() - 1, i + half);
        for (size_t j = lo; j <= hi; ++j)
        { acc += tmp[j]; ++cnt; }
        v[i] = (float) (acc / (double) cnt);
    }
}

// -------------------------------------------------------------------- levels

static double linToDb (double v) noexcept
{
    return v > 0.0 ? 20.0 * std::log10 (v) : -300.0;
}

LevelsResult levels (const AudioEngine& eng, const LevelsParams& p,
                     const ScaleCalibration* cal, const ProgressFn& progress)
{
    LevelsResult r;
    r.sampleRate = eng.sampleRate();
    r.absUnit = absUnitFor (cal);
    r.physicalUnit = physicalUnitFor (cal);
    if (!eng.isFileLoaded() || !(r.sampleRate > 0.0)) return r;

    const double dur = eng.duration();
    const int64_t total = eng.numSamples();
    r.t0 = std::clamp (p.t0, 0.0, dur);
    r.t1 = (p.t1 > p.t0) ? std::clamp (p.t1, r.t0, dur) : dur;
    r.s0 = std::clamp<int64_t> ((int64_t) (r.t0 * r.sampleRate), 0, total);
    r.s1 = std::clamp<int64_t> ((int64_t) (r.t1 * r.sampleRate), r.s0, total);
    if (p.t1 <= p.t0) r.s1 = total;

    std::vector<int> chans;
    if (p.channels.empty())
        for (int ch = 0; ch < eng.numChannels(); ++ch) chans.push_back (ch);
    else
        for (int ch : p.channels)
            if (ch >= 0 && ch < eng.numChannels()) chans.push_back (ch);

    constexpr int64_t kBlock = 65536;
    std::vector<float> buf ((size_t) kBlock, 0.0f);
    const double span = (double) std::max<int64_t> (1, r.s1 - r.s0);
    for (size_t ci = 0; ci < chans.size(); ++ci)
    {
        ChannelLevels L;
        L.ch = chans[ci];
        double sum = 0.0, sumSq = 0.0, peak = 0.0;
        int64_t peakPos = r.s0, count = 0, clipped = 0;
        float mn = std::numeric_limits<float>::max(), mx = std::numeric_limits<float>::lowest();
        for (int64_t pos = r.s0; pos < r.s1; pos += kBlock)
        {
            if (wantsCancel (progress, ((double) ci + (double) (pos - r.s0) / span) / (double) chans.size()))
            {
                r.cancelled = true;
                r.channels.clear();
                return r;
            }
            const int64_t cnt = std::min<int64_t> (kBlock, r.s1 - pos);
            std::fill (buf.begin(), buf.begin() + (size_t) cnt, 0.0f);
            if (!eng.readSamples (L.ch, pos, cnt, buf.data())) continue;
            for (int64_t i = 0; i < cnt; ++i)
            {
                const float x = buf[(size_t) i];
                const double a = std::abs ((double) x);
                sum += x;
                sumSq += (double) x * x;
                if (a > peak) { peak = a; peakPos = pos + i; }
                if (x < mn) mn = x;
                if (x > mx) mx = x;
                if (a >= (double) p.clipThreshold) ++clipped;
            }
            count += cnt;
        }
        L.samples = count;
        if (count > 0)
        {
            L.rms = std::sqrt (sumSq / (double) count);
            L.dcOffset = sum / (double) count;
            L.minSample = mn;
            L.maxSample = mx;
        }
        L.peak = peak;
        L.peakAtSec = (double) peakPos / r.sampleRate;
        L.rmsDb = linToDb (L.rms);
        L.peakDb = linToDb (L.peak);
        L.crestDb = (L.rms > 0.0 && L.peak > 0.0) ? L.peakDb - L.rmsDb : 0.0;
        L.clipped = clipped;
        L.cal = channelCalibration (cal, L.ch);
        if (L.cal.calibrated)
        {
            L.rmsAbsDb = L.rms > 0.0 ? L.rmsDb + L.cal.levelOffsetDb : -300.0;
            L.peakAbsDb = L.peak > 0.0 ? L.peakDb + L.cal.levelOffsetDb : -300.0;
            L.rmsPhysical = L.rms * L.cal.fullScale;
            L.peakPhysical = L.peak * L.cal.fullScale;
        }
        r.channels.push_back (L);
    }
    if (progress) progress (1.0);
    return r;
}

LevelCurve levelCurve (const AudioEngine& eng, int ch, double t0, double t1,
                       double windowSec, double hopSec, LevelKind kind,
                       const ProgressFn& progress)
{
    LevelCurve c;
    const double rate = eng.sampleRate();
    if (!eng.isFileLoaded() || !(rate > 0.0) || ch < 0 || ch >= eng.numChannels()
        || !(windowSec > 0.0) || !(hopSec > 0.0))
        return c;

    const double dur = eng.duration();
    const int64_t total = eng.numSamples();
    t0 = std::clamp (t0, 0.0, dur);
    t1 = std::clamp (t1, t0, dur);
    const int64_t s0 = std::clamp<int64_t> ((int64_t) (t0 * rate), 0, total);
    const int64_t s1 = std::clamp<int64_t> ((int64_t) (t1 * rate), s0, total);
    int64_t win = std::max<int64_t> (1, (int64_t) std::llround (windowSec * rate));
    const int64_t hop = std::max<int64_t> (1, (int64_t) std::llround (hopSec * rate));
    if (s1 - s0 < win) win = std::max<int64_t> (1, s1 - s0);   // one value over a short region
    c.windowSec = (double) win / rate;
    c.hopSec = (double) hop / rate;
    c.t0 = ((double) s0 + (double) win * 0.5) / rate;

    std::vector<float> buf ((size_t) win, 0.0f);
    const double span = (double) std::max<int64_t> (1, s1 - s0);
    for (int64_t s = s0; s + win <= std::max (s1, s0 + win); s += hop)
    {
        if (wantsCancel (progress, (double) (s - s0) / span))
        {
            c.cancelled = true;
            c.valuesDb.clear();
            return c;
        }
        std::fill (buf.begin(), buf.end(), 0.0f);
        double v = 0.0;
        if (eng.readSamples (ch, s, win, buf.data()))
        {
            double acc = 0.0;
            for (float x : buf)
                acc = (kind == LevelKind::Rms) ? acc + (double) x * x
                                               : std::max (acc, std::abs ((double) x));
            v = (kind == LevelKind::Rms) ? std::sqrt (acc / (double) win) : acc;
        }
        c.valuesDb.push_back (linToDb (v));
        if (s1 - s0 <= win) break;
    }
    if (progress) progress (1.0);
    return c;
}

// --------------------------------------------------------------------- peaks

// Parabola through (k-1, a), (k, b), (k+1, c): vertex offset in [-0.5, 0.5]
// and the level there.
static void parabolicPeak (double a, double b, double c, double& delta, double& level)
{
    const double denom = a - 2.0 * b + c;
    delta = (denom < 0.0) ? std::clamp (0.5 * (a - c) / denom, -0.5, 0.5) : 0.0;
    level = b - 0.25 * (a - c) * delta;
}

std::vector<Peak> findPeaks (const std::vector<double>& x, double rate, int fftSize,
                             const PeakParams& p)
{
    std::vector<Peak> out;
    const int m = std::min ((int) x.size(), fftSize / 2 + 1);
    if (m < 3 || !(rate > 0.0) || fftSize < 2) return out;
    const double binHz = rate / (double) fftSize;

    // Previous / next strictly greater sample (monotonic stacks): the
    // prominence bases lie between them (scipy.signal.peak_prominences).
    std::vector<int> prevG ((size_t) m, -1), nextG ((size_t) m, m);
    {
        std::vector<int> st;
        st.reserve ((size_t) m);
        for (int i = 0; i < m; ++i)
        {
            while (!st.empty() && x[(size_t) st.back()] <= x[(size_t) i]) st.pop_back();
            prevG[(size_t) i] = st.empty() ? -1 : st.back();
            st.push_back (i);
        }
        st.clear();
        for (int i = m - 1; i >= 0; --i)
        {
            while (!st.empty() && x[(size_t) st.back()] <= x[(size_t) i]) st.pop_back();
            nextG[(size_t) i] = st.empty() ? m : st.back();
            st.push_back (i);
        }
    }
    // Sparse table for range minimum queries.
    int levelsN = 1;
    while ((1 << levelsN) <= m) ++levelsN;
    std::vector<std::vector<double>> rmq ((size_t) levelsN);
    rmq[0].assign (x.begin(), x.begin() + m);
    for (int j = 1; j < levelsN; ++j)
    {
        const int len = m - (1 << j) + 1;
        rmq[(size_t) j].resize ((size_t) std::max (0, len));
        for (int i = 0; i < len; ++i)
            rmq[(size_t) j][(size_t) i] = std::min (rmq[(size_t) j - 1][(size_t) i],
                                                    rmq[(size_t) j - 1][(size_t) (i + (1 << (j - 1)))]);
    }
    auto rangeMin = [&rmq] (int lo, int hi)   // inclusive, lo <= hi
    {
        int j = 0;
        while ((1 << (j + 1)) <= hi - lo + 1) ++j;
        return std::min (rmq[(size_t) j][(size_t) lo], rmq[(size_t) j][(size_t) (hi - (1 << j) + 1)]);
    };

    const int kLo = std::max (1, p.minFreqHz > 0.0 ? (int) std::ceil (p.minFreqHz / binHz) : 1);
    const int kHi = std::min (m - 2, p.maxFreqHz > 0.0 ? (int) std::floor (p.maxFreqHz / binHz) : m - 2);
    for (int k = kLo; k <= kHi; ++k)
    {
        const double v = x[(size_t) k];
        if (!(v > x[(size_t) k - 1])) continue;
        // Strict maximum, or the first bin of a plateau that falls afterwards.
        int e = k;
        while (e + 1 < m && x[(size_t) e + 1] == v) ++e;
        if (e + 1 >= m || !(x[(size_t) e + 1] < v)) { k = e; continue; }
        if (v < p.minLevelDb) { k = e; continue; }

        const double leftMin = rangeMin (prevG[(size_t) k] + 1, k);
        const double rightMin = rangeMin (k, nextG[(size_t) k] - 1);
        const double prom = v - std::max (leftMin, rightMin);
        if (prom >= p.minProminenceDb)
        {
            Peak pk;
            pk.bin = k;
            pk.binRelDb = v;
            pk.prominenceDb = prom;
            double delta = 0.0, level = v;
            if (e == k)
                parabolicPeak (x[(size_t) k - 1], v, x[(size_t) k + 1], delta, level);
            pk.freqHz = ((double) k + delta) * binHz;
            pk.relDb = level;
            pk.absDb = level;
            out.push_back (pk);
        }
        k = e;
    }

    std::sort (out.begin(), out.end(), [] (const Peak& a, const Peak& b)
    {
        if (a.prominenceDb != b.prominenceDb) return a.prominenceDb > b.prominenceDb;
        return a.freqHz < b.freqHz;
    });
    if (p.maxCount > 0 && (int) out.size() > p.maxCount)
        out.resize ((size_t) p.maxCount);
    return out;
}

std::vector<ChannelPeaks> peaks (const SpectrumResult& spec, const PeakParams& p)
{
    std::vector<ChannelPeaks> out;
    for (const auto& cs : spec.channels)
    {
        ChannelPeaks cp;
        cp.ch = cs.ch;
        cp.peaks = findPeaks (cs.relDb, spec.sampleRate, spec.fftSize, p);
        for (auto& pk : cp.peaks)
            pk.absDb = pk.relDb + cs.cal.levelOffsetDb;
        out.push_back (std::move (cp));
    }
    return out;
}

// ----------------------------------------------------------------- harmonics

static double harmonicTolerance (const HarmonicsParams& p, double f0, double binHz)
{
    double tol = p.toleranceHz > 0.0 ? p.toleranceHz : std::max (2.0 * binHz, 0.03 * f0);
    return std::min (tol, 0.45 * f0);
}

double detectFundamental (const std::vector<double>& relDb, double rate, int fftSize,
                          const HarmonicsParams& p)
{
    const auto pk = findPeaks (relDb, rate, fftSize, p.detect);
    if (pk.empty() || !(rate > 0.0)) return 0.0;
    const double binHz = rate / (double) fftSize;
    double strongest = -1e300;
    for (const auto& q : pk) strongest = std::max (strongest, q.relDb);

    const int maxK = std::max (1, p.maxK);
    double bestF = 0.0, bestScore = -1.0;
    for (const auto& cand : pk)
    {
        if (cand.relDb < strongest - 40.0 || !(cand.freqHz > 0.0)) continue;
        const double fc = cand.freqHz;
        const double tol = harmonicTolerance (p, fc, binHz);
        double score = 0.0;
        for (const auto& q : pk)
        {
            const double k = std::round (q.freqHz / fc);
            if (k < 1.0 || k > (double) maxK) continue;
            if (std::abs (q.freqHz - k * fc) <= tol)
                score += std::pow (10.0, q.relDb / 10.0);
        }
        // Higher score wins; a tie keeps the lower frequency.
        if (score > bestScore * (1.0 + 1e-9)
            || (std::abs (score - bestScore) <= bestScore * 1e-9 && fc < bestF))
        {
            bestScore = score;
            bestF = fc;
        }
    }
    return bestF;
}

HarmonicsResult harmonics (const std::vector<double>& x, double rate, int fftSize,
                           const HarmonicsParams& p)
{
    HarmonicsResult r;
    if (!(rate > 0.0) || fftSize < 2 || x.empty()) return r;

    double f0 = p.fundamentalHz;
    if (!(f0 > 0.0))
    {
        if (p.anchorHz > 0.0 && p.anchorN >= 1)
            f0 = p.anchorHz / (double) p.anchorN;
        else
        {
            f0 = detectFundamental (x, rate, fftSize, p);
            r.autoDetected = true;
        }
    }
    if (!(f0 > 0.0) || !std::isfinite (f0)) return r;
    r.fundamentalHz = f0;

    const double binHz = rate / (double) fftSize;
    const int maxBin = std::min ((int) (fftSize / 2), (int) x.size() - 1);
    r.toleranceHz = (p.search == HarmonicsParams::Search::NearestBin) ? 0.0
                                                                      : harmonicTolerance (p, f0, binHz);
    auto isLocalMax = [&x, maxBin] (int b)
    {
        return b > 0 && b < maxBin && x[(size_t) b] > x[(size_t) b - 1] && x[(size_t) b] >= x[(size_t) b + 1];
    };

    for (int k = 1; k <= std::max (1, p.maxK); ++k)
    {
        Harmonic h;
        h.k = k;
        const double f = (double) k * f0;
        h.expectedHz = f;
        if (maxBin < 1 || f > rate * 0.5)
        {
            r.harmonics.push_back (h);   // no FFT data above Nyquist
            continue;
        }
        h.valid = true;
        const int nearest = (int) std::clamp ((double) std::lround (f * fftSize / rate),
                                              0.0, (double) maxBin);
        if (p.search == HarmonicsParams::Search::NearestBin)
        {
            h.bin = nearest;
            h.freqHz = f;
            h.relDb = x[(size_t) nearest];
        }
        else
        {
            const int lo = std::max (1, (int) std::ceil ((f - r.toleranceHz) / binHz));
            const int hi = std::min (maxBin, (int) std::floor ((f + r.toleranceHz) / binHz));
            int best = nearest;
            if (lo <= hi)
            {
                best = lo;
                for (int b = lo + 1; b <= hi; ++b)
                    if (x[(size_t) b] > x[(size_t) best]) best = b;
            }
            h.bin = best;
            h.freqHz = (double) best * binHz;
            h.relDb = x[(size_t) best];
            if (isLocalMax (best))
            {
                double delta = 0.0, level = h.relDb;
                parabolicPeak (x[(size_t) best - 1], x[(size_t) best], x[(size_t) best + 1], delta, level);
                h.freqHz = ((double) best + delta) * binHz;
                h.relDb = level;
            }
        }
        h.isLocalMax = isLocalMax (h.bin);
        h.absDb = h.relDb;
        r.harmonics.push_back (h);
    }

    if (!r.harmonics.empty() && r.harmonics.front().valid)
    {
        const double l1 = r.harmonics.front().relDb;
        double sumPow = 0.0;
        for (auto& h : r.harmonics)
        {
            if (!h.valid) continue;
            h.relToFundamentalDb = h.relDb - l1;
            if (h.k >= 2) sumPow += std::pow (10.0, h.relDb / 10.0);
        }
        r.thdPercent = 100.0 * std::sqrt (sumPow) / std::pow (10.0, l1 / 20.0);
    }
    return r;
}

HarmonicsResult harmonics (const SpectrumResult& spec, int chIndex, const HarmonicsParams& p)
{
    if (chIndex < 0 || chIndex >= (int) spec.channels.size()) return {};
    const auto& cs = spec.channels[(size_t) chIndex];
    HarmonicsResult r = harmonics (cs.relDb, spec.sampleRate, spec.fftSize, p);
    for (auto& h : r.harmonics)
        if (h.valid) h.absDb = h.relDb + cs.cal.levelOffsetDb;
    return r;
}
} // namespace sc::measures
