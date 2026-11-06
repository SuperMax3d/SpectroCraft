#pragma once
// SpectroCraft - headless measurement module (lot 4 of the AI-control plan).
//
// Pure functions over an AudioEngine: no Component, no geometry, no
// app.selection. The FFT Analysis panel (FFTAnalysisView) and the future
// `measure.*` commands share this ONE implementation, so the numbers an API
// client reads are the numbers drawn on screen.
//
// Threading: every function only reads samples through
// AudioEngine::readSamples (thread-safe) and uses scfft::realForward (per-
// thread FFT cache), so it may run on a worker thread. The caller must keep
// the engine alive for the duration of the call (do not close the document
// while a job runs) and pass calibration by value (copy app.scale() on the
// message thread first). Long computations take a ProgressFn: it receives a
// fraction in [0, 1] and returns false to cancel; a cancelled call returns
// with `cancelled = true` (or an empty result for the low-level helpers).
//
// Level conventions (identical to the panel, see the 2026-11-03 calibration
// fixes):
//   - dBFS: 0 dB = |1.0| sample PEAK. A full-scale sine has peak 0 dBFS and
//     RMS -3.01 dBFS.
//   - Spectra are single-sided RMS amplitudes corrected for the window's
//     coherent gain: a sine of peak A reads 20*log10(A/sqrt 2) on its bin
//     whatever the window, so a full-scale sine reads -3.01 dB.
//   - Absolute levels add ScaleCalibration::levelOffsetDb(ch) =
//     20*log10(fullScale(ch)/ref) (ref 20 uPa for Pascal, 1 V, 1 g): a
//     full-scale sine at 1 Pa reads 90.97 dB-SPL.

#include <JuceHeader.h>
#include "AudioEngine.h"
#include "ScaleCalibration.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class SpectralSelection;

namespace sc::measures
{
    /** Progress / cancellation callback: fraction in [0, 1]; return false to
        cancel. An empty std::function means "never cancel". */
    using ProgressFn = std::function<bool (double fraction)>;

    // ------------------------------------------------------------ constants

    /** Fixed normalisation floor of the panel's stored magnitudes (dB below
        full scale): normalised value v in [0, 1] <-> dB = -160 * (1 - v). */
    inline constexpr double kNormFloorDb = 160.0;
    /** Cap on the number of averaged FFT windows of a region spectrum. */
    inline constexpr int64_t kMaxAvgWindows = 2048;
    /** Max points of one time curve (Freq/Amp vs Time). */
    inline constexpr int kMaxCurveFrames = 2048;
    /** Fixed FFT size of the curve modes (Freq vs Time, Amp vs Time, Amp vs Freq). */
    inline constexpr int kTimeModeFFT = 2048;
    /** FFT size of the reference (dB-SPL) 1/3-octave pipeline. */
    inline constexpr int kRefThirdOctFFT = 65536;
    /** Max frames averaged by the reference 1/3-octave pipeline. */
    inline constexpr int kRefThirdOctMaxFrames = 2048;
    /** The reference pipeline's "no data" band value (reference dB). */
    inline constexpr double kEmptyBandValue = -999.0;

    /** Window types of the classic spectrum path (the panel's "Win" combo). */
    enum WindowType { Rectangular = 0, Hann = 1, Hamming = 2 };
    /** "rectangular" / "hann" / "hamming" (JSON names). */
    const char* windowName (int windowType) noexcept;
    /** Inverse of windowName (case-insensitive; "hanning" accepted); -1 if unknown. */
    int windowTypeFromName (const juce::String& name);

    // ------------------------------------------------------- shared helpers

    /** Analysis window of the classic spectrum path (0 = rectangular,
        1 = periodic Hann, 2 = symmetric Hamming). */
    std::vector<float> makeAnalysisWindow (int windowType, int n);
    /** Equivalent noise bandwidth of a window, in bins: n*sum(w^2)/sum(w)^2
        (1.0 rectangular, 1.5 Hann, ~1.36 Hamming). */
    double windowEnbwBins (const std::vector<float>& win);
    /** ENBW (bins) of window type `windowType` at size n. */
    double enbwBins (int windowType, int n);

    /** Normalised [0..1] magnitude <-> dB relative to full scale (floor -160). */
    inline double normToDb (float norm) noexcept { return -kNormFloorDb * (1.0 - (double) norm); }
    /** Normalised band values -> the panel's relative-dB space (float). */
    std::vector<float> bandsNormalizedToDb (const std::vector<float>& norm);

    /** IEC 61260-1 nominal 1/3-octave centres (50 Hz .. 16 kHz, 26 bands). */
    const double* iecThirdOctCentres() noexcept;
    int numIecThirdOctBands() noexcept;
    /** Decade-based centres of the dB-SPL pipeline: 1000*10^((i-30)/10),
        i = 17..43 (50.12 Hz .. 19952.6 Hz, 27 bands). */
    const double* refThirdOctCentres() noexcept;
    int numRefThirdOctBands() noexcept;

    /** Sums an FFT magnitude curve (normalised as regionSpectrumNorm returns
        it, n-point FFT at `rate`) into IEC 1/3-octave bands: bin powers
        summed per band (edge bins by overlapping fraction) and divided by
        the window ENBW. One normalised value per band whose centre is
        <= Nyquist. */
    std::vector<float> toThirdOctBands (const std::vector<float>& mag, int n,
                                        double rate, double enbwBins);

    /** Frame placement of the curve computations: frames of fftN samples
        covering [s0, s1] with a hop yielding at most kMaxCurveFrames points;
        a region shorter than one frame gets one centred frame. */
    std::vector<int64_t> curveFrameStarts (double totalSamples, int64_t s0, int64_t s1, int fftN);

    /** Window placement of the region spectrum. overlap < 0: the panel's
        placement (one centred window when the region is <= n samples,
        otherwise min(2048, ceil(len/n), 4M/n) windows spread evenly).
        overlap in [0, 0.95]: fixed hop n*(1-overlap) from t0 (windows fully
        inside the region), spread evenly when more than kMaxAvgWindows.
        `capped` (optional) is set when the cap reduced the window count. */
    std::vector<int64_t> spectrumWindowStarts (const AudioEngine& eng, double t0, double t1,
                                               int n, double overlap = -1.0,
                                               bool* capped = nullptr);

    // ------------------------------------------- region spectrum (low level)

    /** The panel's core averaging computation (was
        FFTAnalysisView::computeRegionFrom): bin powers of the windows placed by
        spectrumWindowStarts averaged, scaled to single-sided RMS amplitude
        (coherent gain), stored normalised [0..1] vs kNormFloorDb. One vector of
        n/2+1 values per valid channel of `chans` (invalid channels skipped).
        Returns false (outMags cleared) when cancelled. */
    bool regionSpectrumNorm (const AudioEngine& eng, const std::vector<int>& chans,
                             double t0, double t1, int n, int windowType,
                             std::vector<std::vector<float>>& outMags,
                             double overlap = -1.0, const ProgressFn& progress = {},
                             int* windowsUsed = nullptr, bool* windowsCapped = nullptr);

    // ------------------------------------------------- calibration snapshot

    /** Per-channel calibration facts attached to every absolute value. */
    struct ChannelCalibration
    {
        bool calibrated = false;    // ScaleCalibration::isCalibratedFor(ch)
        double fullScale = 0.0;     // physical value at |1.0| (0 = none)
        double levelOffsetDb = 0.0; // added to dBFS / relative dB for absolute levels
    };
    ChannelCalibration channelCalibration (const ScaleCalibration* cal, int ch);
    /** "dB-SPL" / "dBV" / "dB/g", or "" when uncalibrated. */
    std::string absUnitFor (const ScaleCalibration* cal);
    /** "Pa" / "V" / "g", or "" when uncalibrated. */
    std::string physicalUnitFor (const ScaleCalibration* cal);

    // ------------------------------------------------------------ spectrum

    struct SpectrumParams
    {
        double t0 = 0.0, t1 = 0.0;     // seconds; t0 == t1 = one window centred on t0
        std::vector<int> channels;     // invalid entries are skipped
        int fftSize = 2048;            // power of two, 2..65536
        int windowType = Hann;
        double overlap = -1.0;         // < 0 = the panel's placement (see spectrumWindowStarts)
    };

    struct ChannelSpectrum
    {
        int ch = -1;
        std::vector<float> norm;       // n/2+1 normalised values (the panel's cache)
        std::vector<double> relDb;     // n/2+1 values, dB re full scale (floor -160)
        ChannelCalibration cal;        // absDb = relDb + cal.levelOffsetDb
    };

    struct SpectrumResult
    {
        double t0 = 0.0, t1 = 0.0;
        double sampleRate = 0.0;
        int fftSize = 0;
        int windowType = Hann;
        double binHz = 0.0;
        double enbwBins = 1.0;
        int framesAveraged = 0;
        bool framesCapped = false;
        std::string unit = "dBFS";
        std::string absUnit;           // "" when uncalibrated
        std::string levelConvention = "RMS single-sided, dB re full scale";
        std::vector<ChannelSpectrum> channels;
        bool cancelled = false;
    };

    /** Averaged spectrum of a region: what the panel shows in FFT Linear/Log
        mode. `cal` may be nullptr (uncalibrated). */
    SpectrumResult spectrum (const AudioEngine& eng, const SpectrumParams& p,
                             const ScaleCalibration* cal = nullptr,
                             const ProgressFn& progress = {});

    // -------------------------------------------------------- third octave

    /** Reference-analyzer (dB-SPL) 1/3-octave bands (was the body of
        FFTAnalysisView::thirdOctBandsRef, unchanged): fixed 65536-point
        Hanning frames at 50% overlap, frame-averaged power spectrum,
        Butterworth weighting of order 2*`order`, +98.5 dB. 27 values in the
        panel's relative-dB space (abs = value + levelOffsetDb(ch)); empty
        bands hold -999 + 20*log10(2e-5). Empty vector on bad arguments or
        cancellation. `framesUsed` (optional) receives the averaged frames. */
    std::vector<float> thirdOctaveRefBands (const AudioEngine& eng, int ch,
                                            double t0, double t1, int order,
                                            const ProgressFn& progress = {},
                                            int* framesUsed = nullptr);

    struct ThirdOctaveParams
    {
        enum class Mode { Reference, Iec };
        double t0 = 0.0, t1 = 0.0;
        std::vector<int> channels;
        Mode mode = Mode::Reference;
        int order = 40;                // Reference: one of 1,2,3,4,5,10,20,30,40
        int fftSize = 2048;            // Iec only
        int windowType = Hann;         // Iec only
    };

    struct ThirdOctaveChannel
    {
        int ch = -1;
        std::vector<float> relDb;      // one per band, the panel's relative-dB space
        std::vector<bool> noData;      // Reference mode: band had no data (-999)
        ChannelCalibration cal;
    };

    struct ThirdOctaveResult
    {
        ThirdOctaveParams::Mode mode = ThirdOctaveParams::Mode::Reference;
        int order = 40;
        int fftSize = 0;               // 65536 in Reference mode
        int windowType = Hann;         // Hann in Reference mode (symmetric "hanning")
        double sampleRate = 0.0;
        int framesUsed = 0;
        int framesCap = 0;
        std::vector<double> centresHz; // per band (Iec: only centres <= Nyquist)
        std::string unit = "dB";       // relative dB re full scale
        std::string absUnit;
        double emptyBandValue = kEmptyBandValue;
        std::vector<ThirdOctaveChannel> channels;
        bool cancelled = false;
    };

    /** 1/3-octave bands of a region. Reference = the panel's dB-SPL pipeline
        (used by the panel when the file is Pascal-calibrated); Iec = the
        panel's non-Pascal pipeline (bands summed from the averaged spectrum). */
    ThirdOctaveResult thirdOctave (const AudioEngine& eng, const ThirdOctaveParams& p,
                                   const ScaleCalibration* cal = nullptr,
                                   const ProgressFn& progress = {});

    // ---------------------------------------------------------------- curves

    /** One curve (the panel's FFTAnalysisView::CurveData). */
    struct CurveData
    {
        std::vector<double> x;      // time (s) or frequency (Hz), per point
        std::vector<float> ampDb;   // amplitude, dB rel. full scale (floor -160)
        std::vector<float> freqHz;  // tracked frequency per point (time modes);
                                    // equals x in Amp vs Freq mode
    };

    /** Track option of the curve modes: 0 = Max (strongest bin), 1 = Total
        (summed level + centroid), 2 = Weighted (level like Total, centroid). */
    enum Track { TrackMax = 0, TrackTotal = 1, TrackWeighted = 2 };

    /** Freq vs Time / Amp vs Time (was FFTAnalysisView::computeTimeCurveFrom):
        fixed 2048-point Hann frames, one point per frame (curveFrameStarts).
        `mask` = spectral selection (nullptr = every bin counts). Returns false
        (out cleared) when cancelled. */
    bool timeCurve (const AudioEngine& eng, const std::vector<int>& chans,
                    double t0, double t1, int track, const SpectralSelection* mask,
                    std::vector<CurveData>& out, const ProgressFn& progress = {});

    /** Amp vs Freq (was FFTAnalysisView::computeSpectrumCurveFrom): per-bin
        Max or mean (Total) magnitude over the frames under [t0, t1]. */
    bool spectrumCurve (const AudioEngine& eng, const std::vector<int>& chans,
                        double t0, double t1, int n, int windowType, int track,
                        const SpectralSelection* mask, std::vector<CurveData>& out,
                        const ProgressFn& progress = {});

    /** Centred moving average (window in points; no-op for window < 2 or a
        series not longer than the window). Same as FFTAnalysisView::smoothSeries. */
    void smoothSeries (std::vector<float>& v, int window);

    // ---------------------------------------------------------------- levels

    struct LevelsParams
    {
        double t0 = 0.0, t1 = 0.0;     // seconds; t1 <= t0 = whole file from t0
        std::vector<int> channels;     // empty = every channel
        /** |sample| >= clipThreshold counts as clipped (default: one 16-bit
            LSB below full scale, so a clipped 16-bit file is counted). */
        float clipThreshold = 1.0f - 1.5f / 32768.0f;
    };

    struct ChannelLevels
    {
        int ch = -1;
        int64_t samples = 0;
        double rms = 0.0, peak = 0.0;  // linear (full scale = 1.0)
        double rmsDb = -300.0;         // dBFS (floor -300 for digital silence)
        double peakDb = -300.0;        // dBFS
        double peakAtSec = 0.0;        // absolute file time of the first max |x|
        double crestDb = 0.0;          // peakDb - rmsDb (0 for silence)
        double dcOffset = 0.0;         // mean sample value
        double minSample = 0.0, maxSample = 0.0;
        int64_t clipped = 0;           // samples with |x| >= clipThreshold
        ChannelCalibration cal;
        double rmsAbsDb = -300.0, peakAbsDb = -300.0;   // dBFS + levelOffsetDb (when calibrated)
        double rmsPhysical = 0.0, peakPhysical = 0.0;   // rms/peak * fullScale (when calibrated)
    };

    struct LevelsResult
    {
        double t0 = 0.0, t1 = 0.0;     // clamped region actually measured
        int64_t s0 = 0, s1 = 0;        // sample range [s0, s1)
        double sampleRate = 0.0;
        std::string unit = "dBFS";
        std::string absUnit;           // "" when uncalibrated
        std::string physicalUnit;      // "Pa" / "V" / "g" or ""
        std::vector<ChannelLevels> channels;
        bool cancelled = false;
    };

    /** RMS, peak, crest factor, DC offset and clip count per channel. */
    LevelsResult levels (const AudioEngine& eng, const LevelsParams& p,
                         const ScaleCalibration* cal = nullptr,
                         const ProgressFn& progress = {});

    /** RMS or peak level vs time (`measure.curve` kind "rms" / "peak"):
        windows of windowSec every hopSec from t0 (window centred times
        t0 + windowSec/2 + i*hopSec); values in dBFS (floor -300). */
    struct LevelCurve
    {
        double t0 = 0.0;               // centre time of the first value
        double hopSec = 0.0, windowSec = 0.0;
        std::vector<double> valuesDb;
        bool cancelled = false;
    };
    enum class LevelKind { Rms, Peak };
    LevelCurve levelCurve (const AudioEngine& eng, int ch, double t0, double t1,
                           double windowSec, double hopSec, LevelKind kind,
                           const ProgressFn& progress = {});

    // ----------------------------------------------------------------- peaks

    struct PeakParams
    {
        int maxCount = 10;             // top-N by prominence (<= 0 = all)
        double minProminenceDb = 6.0;
        double minLevelDb = -150.0;    // relative dB
        double minFreqHz = 0.0;        // 0 = from bin 1
        double maxFreqHz = 0.0;        // 0 = up to Nyquist
    };

    struct Peak
    {
        int bin = 0;                   // local-maximum bin
        double freqHz = 0.0;           // parabolic interpolation (dB domain)
        double relDb = 0.0;            // interpolated level, dB re full scale
        double binRelDb = 0.0;         // level of the bin itself (the panel's value)
        double prominenceDb = 0.0;     // topographic prominence
        double absDb = 0.0;            // relDb + levelOffsetDb (filled by the SpectrumResult overload)
    };

    /** Spectral peaks of one dB spectrum (n/2+1 values of an n-point FFT at
        `rate`): strict local maxima (plateaus take their first bin), excluding
        DC and Nyquist; prominence as in scipy.signal.peak_prominences; sorted by
        prominence (descending, then frequency); frequency/level refined by a
        parabola through the three bins around the maximum. */
    std::vector<Peak> findPeaks (const std::vector<double>& relDb, double rate, int fftSize,
                                 const PeakParams& p = {});

    struct ChannelPeaks { int ch = -1; std::vector<Peak> peaks; };
    /** findPeaks on every channel of a spectrum, absDb filled from calibration. */
    std::vector<ChannelPeaks> peaks (const SpectrumResult& spec, const PeakParams& p = {});

    // ------------------------------------------------------------- harmonics

    struct HarmonicsParams
    {
        /** Fundamental in Hz. <= 0: anchorHz / anchorN when anchorHz > 0
            (the panel's "Set Harmonic References": the anchor is harmonic #N),
            otherwise auto-detected from the spectral peaks. */
        double fundamentalHz = 0.0;
        double anchorHz = 0.0;
        int anchorN = 1;
        int maxK = 10;                 // harmonics 1..maxK
        enum class Search { PeakInTolerance, NearestBin };
        /** PeakInTolerance: strongest bin within +-tolerance of k*f0, refined by
            parabolic interpolation. NearestBin: the bin nearest to k*f0 (the
            panel's "Copy Harmonics" table). */
        Search search = Search::PeakInTolerance;
        /** Half-width of the search window (Hz). <= 0 = max(2 bins, 3% of f0),
            always capped below f0/2. */
        double toleranceHz = 0.0;
        /** Peak settings of the auto-detection (top 32, prominence >= 6 dB). */
        PeakParams detect { 32, 6.0, -150.0, 0.0, 0.0 };
    };

    struct Harmonic
    {
        int k = 0;
        double expectedHz = 0.0;       // k * f0
        bool valid = false;            // false above Nyquist (no data)
        int bin = 0;
        double freqHz = 0.0;           // measured frequency (expectedHz for NearestBin)
        double relDb = -300.0;         // dB re full scale
        double absDb = -300.0;         // relDb + levelOffsetDb (SpectrumResult overload)
        double relToFundamentalDb = 0.0;
        bool isLocalMax = false;       // the measured bin is a local maximum
    };

    struct HarmonicsResult
    {
        double fundamentalHz = 0.0;    // 0 = none found
        bool autoDetected = false;
        double toleranceHz = 0.0;
        std::vector<Harmonic> harmonics;
        /** THD (harmonics 2..maxK vs the fundamental, power ratio) in percent;
            < 0 when the fundamental is not valid. */
        double thdPercent = -1.0;
    };

    /** Harmonic levels of one dB spectrum (n/2+1 values, see findPeaks). */
    HarmonicsResult harmonics (const std::vector<double>& relDb, double rate, int fftSize,
                               const HarmonicsParams& p);

    /** Auto-detection only: the fundamental best explaining the spectral peaks
        (candidate peaks within 40 dB of the strongest, scored by the summed
        power of the peaks found at its multiples); 0 when there is no peak. */
    double detectFundamental (const std::vector<double>& relDb, double rate, int fftSize,
                              const HarmonicsParams& p);

    /** harmonics() on one channel of a spectrum (chIndex = index into
        spec.channels), absDb filled from calibration. */
    HarmonicsResult harmonics (const SpectrumResult& spec, int chIndex, const HarmonicsParams& p);
}
