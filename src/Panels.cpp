/**
 * SpectroCraft - Left-side panels implementation
 */

#include "Panels.h"
#include "FFTHelpers.h"
#include <algorithm>
#include <cmath>

juce::String formatTimeCode (double secs)
{
    if (secs < 0) secs = 0;
    const int m = (int) (secs / 60.0);
    const double s = secs - m * 60.0;
    return juce::String (m) + ":" + juce::String (s, 3).paddedLeft ('0', 6);
}

static double parseTimeCode (const juce::String& text)
{
    // Accepts "12", "12.5", "0:04.536", "1:02"
    const int colon = text.indexOfChar (':');
    if (colon >= 0)
    {
        const double m = text.substring (0, colon).getDoubleValue();
        const double s = text.substring (colon + 1).getDoubleValue();
        return m * 60.0 + s;
    }
    return text.getDoubleValue();
}

// Rectangle helper (JUCE 9 has no withXOffset)
static juce::Rectangle<int> xoffset (const juce::Rectangle<int>& r, int dx)
{
    return juce::Rectangle<int> (r.getX() + dx, r.getY(), std::max (0, r.getWidth() - dx),
                                 r.getHeight());
}

// ================================================================= FileListPanel

FileListPanel::FileListPanel (Application& appIn)
    : app (appIn), openBtn ("Open..."), closeBtn ("Close")
{
    openBtn.onClick = [this] { if (onOpenRequested) onOpenRequested(); };
    closeBtn.onClick = [this] {
        app.closeFile();
        refresh();
        if (app.onStateChange) app.onStateChange();
    };

    openBtn.setTooltip ("Open an audio file (WAV, AIFF, FLAC, MP3 or Ogg). "
                        "You can also drag & drop a file onto the window.");
    closeBtn.setTooltip ("Close the currently loaded file and clear all views.");
    addAndMakeVisible (&openBtn);
    addAndMakeVisible (&closeBtn);
}

void FileListPanel::refresh()
{
    fileInfoLines.clear();
    if (app.audioEngine.isFileLoaded())
    {
        const auto& f = app.audioEngine.getFile();
        fileInfoLines.push_back (f.getFileName());
        fileInfoLines.push_back (juce::String ((int) app.audioEngine.sampleRate()) + " Hz | "
            + juce::String (app.audioEngine.numChannels()) + " ch | "
            + formatTimeCode (app.audioEngine.duration()));
    }
    else
        fileInfoLines.push_back ("(no file loaded)");

    openBtn.setEnabled (true);
    closeBtn.setEnabled (app.audioEngine.isFileLoaded());
    repaint();
}

void FileListPanel::resized()
{
    auto b = getLocalBounds().reduced (4, 4);
    openBtn.setBounds (b.removeFromTop (26).removeFromLeft (90));
    closeBtn.setBounds (openBtn.getRight() + 8, openBtn.getY(), 70, 26);
}

void FileListPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));
    const auto b = getLocalBounds().reduced (8, 30);
    g.setColour (juce::Colours::white);
    g.setFont (12.0f);
    for (size_t i = 0; i < fileInfoLines.size(); ++i)
        g.drawText (fileInfoLines[i], b.getX(), b.getY() + (int) i * 18, b.getWidth(), 16,
                    juce::Justification::left, false);
}

// ================================================================= FFTAnalysisView

// Standard IEC 61260-1 1/3-octave centre frequencies, 50 Hz .. 16 kHz.
static const double kThirdOctCenters[] = {
    50.0, 63.0, 80.0, 100.0, 125.0, 160.0, 200.0, 250.0, 315.0, 400.0,
    500.0, 630.0, 800.0, 1000.0, 1250.0, 1600.0, 2000.0, 2500.0, 3150.0,
    4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0, 16000.0
};
static constexpr int kNumThirdOctBands = (int) (sizeof (kThirdOctCenters) / sizeof (kThirdOctCenters[0]));

const double* FFTAnalysisView::thirdOctCentres() noexcept { return kThirdOctCenters; }
int FFTAnalysisView::numThirdOctBands() noexcept { return kNumThirdOctBands; }

// Only a subset of band centres gets an axis label (every ~3rd band), so the
// 26 grid lines do not crowd the bottom axis.
static bool isThirdOctLabelFreq (double f)
{
    for (double c : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        if (std::abs (f - c) < 1e-6)
            return true;
    return false;
}

// Average an FFT magnitude curve into 1/3-octave bands.
// mag holds dB-normalized values in [0,1] for an n-point FFT at the given rate;
// returns one normalized value per band whose centre is <= Nyquist. Power is
// averaged over all bins inside each band (band edges = centre * 2^(+-1/6)).
// Cap on the number of averaged FFT windows per channel for wide selections.
static constexpr int64_t kMaxAvgWindows = 2048;

static std::vector<float> toThirdOctBands (const std::vector<float>& mag, int n,
                                           double rate, float dBRange)
{
    const double nyq = rate * 0.5;
    const double dBMin = -(double) dBRange;
    std::vector<float> out;
    for (int b = 0; b < kNumThirdOctBands && kThirdOctCenters[b] <= nyq; ++b)
    {
        const double fc = kThirdOctCenters[b];
        const double lo = fc * std::pow (2.0, -1.0 / 6.0);
        const double hi = fc * std::pow (2.0,  1.0 / 6.0);
        int kLo = (int) std::ceil (lo * n / rate);
        int kHi = (int) std::floor (hi * n / rate);
        kLo = std::max (1, kLo);
        kHi = std::min (n / 2, kHi);

        double sumPow = 0.0;
        int cnt = 0;
        for (int k = kLo; k <= kHi && k < (int) mag.size(); ++k)
        {
            const double dB = dBMin * (1.0 - (double) mag[k]);   // undo normalization
            const double m = std::pow (10.0, dB / 20.0);
            sumPow += m * m;
            ++cnt;
        }
        const double bm = cnt > 0 ? std::sqrt (sumPow / (double) cnt) : 0.0;
        const double dBb = 20.0 * std::log10 (std::max (1e-12, bm));
        out.push_back ((float) std::clamp ((dBb - dBMin) / (-dBMin), 0.0, 1.0));
    }
    return out;
}

// Curve colours for the live spectrum and the snapshot overlays. Button i+1
// reproduces curve fftColours[(i + 1) % 4] and wears that colour as its
// background.
static const juce::Colour fftColours[] = {
    juce::Colour::fromRGB (255, 48, 144),   // pink
    juce::Colour::fromRGB (48, 192, 255),   // cyan
    juce::Colour::fromRGB (120, 255, 120),  // green
    juce::Colour::fromRGB (255, 220, 60)    // yellow
};

SnapshotButton::SnapshotButton() : juce::Button (juce::String())
{
}

void SnapshotButton::paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted,
                                  bool /*shouldDrawButtonAsDown*/)
{
    auto b = getLocalBounds().reduced (1);
    if (b.isEmpty()) return;

    const bool on = getToggleState();

    // The background always shows the colour of the curve this button
    // reproduces; toggled ON is the most obvious state.
    juce::Colour fill;
    if (on)            fill = col;
    else if (stored)   fill = col.darker (0.45f);
    else               fill = col.darker (0.72f);
    if (shouldDrawButtonAsHighlighted && !on)
        fill = fill.brighter (0.18f);

    g.setColour (fill);
    g.fillRect (b);

    g.setColour (on ? juce::Colours::white
                    : (stored ? col.brighter (0.35f)
                             : juce::Colour::fromRGB (90, 90, 100)));
    g.drawRect (b, on ? 2 : 1);

    g.setColour (on ? fill.contrasting()
                   : (stored ? juce::Colour::fromRGB (215, 215, 225)
                            : juce::Colour::fromRGB (160, 160, 170)));
    g.setFont (11.0f);
    g.drawText (getButtonText(), b, juce::Justification::centred, false);
}

FFTAnalysisView::FFTAnalysisView (Application& appIn)
    : app (appIn)
{
    for (int n = 1024; n <= 65536; n *= 2)
        fftSizeCombo.addItem (juce::String (n), n);
    localN = std::clamp (app.settings.fftSize, 1024, 65536);
    fftSizeCombo.setSelectedId (localN, juce::dontSendNotification);
    fftSizeCombo.onChange = [this] {
        // Independent from the spectrogram's FFT size.
        localN = fftSizeCombo.getSelectedId();
        settingsGen++;               // stored snapshots must be recomputed too
        mags.clear(); cachedN = 0;   // invalidate cached spectrum
        recomputeSnapshots();
        repaint();
    };

    // JUCE ComboBox item IDs must be non-zero.
    windowCombo.addItem ("Rectangular", 1);
    windowCombo.addItem ("Hann", 2);
    windowCombo.addItem ("Hamming", 3);
    windowCombo.setSelectedId (std::clamp (app.settings.windowType, 0, 2) + 1,
                               juce::dontSendNotification);
    windowCombo.onChange = [this] {
        settingsGen++;               // stored snapshots must be recomputed too
        mags.clear(); cachedN = 0;
        recomputeSnapshots();
        repaint();
    };

    axisCombo.addItem ("FFT Linear", 1);
    axisCombo.addItem ("FFT Log", 2);
    axisCombo.addItem ("1/3 Octave", 3);
    axisCombo.setSelectedId (axisMode_ + 1, juce::dontSendNotification);
    axisCombo.onChange = [this] {
        axisMode_ = axisCombo.getSelectedId() - 1;
        thirdOctMags.clear();   // band cache is only valid for mode 2
        refreshSnapshotBands(); // re-derive stored snapshots' bands for the new mode
        repaint();
    };

    for (int i = 0; i < 10; ++i)
    {
        // The button's background always shows the colour of the curve it
        // reproduces (same mapping as the overlay drawing in paint()).
        snapBtns[i].setCurveColour (fftColours[(i + 1) % 4]);
        snapBtns[i].setButtonText (juce::String (i + 1));
        snapBtns[i].setClickingTogglesState (true);
        snapBtns[i].onClick = [this, i] { handleSnapshotClick (i); };
        updateSnapshotTooltip (i);
    }

    fftSizeCombo.setTooltip ("FFT size of this analysis (independent of the spectrogram's). "
                             "Stored snapshots are recomputed at the new size.");
    windowCombo.setTooltip ("Window function applied to each analysed frame: Rectangular, Hann or Hamming. "
                            "Stored snapshots are recomputed with the new window.");
    axisCombo.setTooltip ("Frequency axis mode: FFT Linear (linear Hz), FFT Log (logarithmic) or "
                          "1/3 Octave (IEC 61260-1 band plot from 50 Hz to 16 kHz).");

    addAndMakeVisible (&fftSizeCombo);
    addAndMakeVisible (&windowCombo);
    addAndMakeVisible (&axisCombo);
    for (auto& b : snapBtns) addAndMakeVisible (&b);
}

// Frequency-axis limits for the current mode.
static void fftAxisLimits (int mode, double rate, double& fLo, double& fHi)
{
    if (mode == 0) { fLo = 0.0; fHi = rate * 0.5; }
    else if (mode == 2 && rate * 0.5 > 50.0)
        { fLo = 50.0; fHi = std::min (rate * 0.5, 16000.0); }   // 1/3-octave range
    else          { fLo = 20.0; fHi = std::min (rate * 0.5, 20000.0); }
}

// Core averaging computation shared by the live spectrum and every stored
// snapshot: analyse region [t0, t1] with windows of exactly n samples at the
// current window type. A region wider than one window is covered by several
// evenly spaced windows whose linear magnitudes are averaged (the classic
// "average over the selection" behaviour); a point or short region gets a
// single window centred on it.
void FFTAnalysisView::computeRegion (double t0, double t1, int n,
                                     std::vector<std::vector<float>>& outMags)
{
    const double rate = app.audioEngine.sampleRate();
    const double dur = app.audioEngine.duration();

    // Where to place the FFT windows (sample offsets).
    std::vector<int64_t> winStarts;
    {
        const double regionLenS = std::max (0.0, (t1 - t0) * rate);   // in samples
        if (regionLenS <= (double) n)
        {
            int64_t s = (int64_t) ((t0 + t1) * 0.5 * rate) - n / 2;   // centre on the region
            const int64_t maxStart = std::max<int64_t> (0, (int64_t) (dur * rate) - n);
            winStarts.push_back (std::clamp<int64_t> (s, 0, maxStart));
        }
        else
        {
            // Cap the work: at most kMaxAvgWindows windows and ~4M samples.
            const int numW = (int) std::min<int64_t> ({ kMaxAvgWindows,
                                                        (int64_t) std::ceil (regionLenS / n),
                                                        std::max<int64_t> (1, 4000000 / n) });
            for (int i = 0; i < numW; ++i)
            {
                const double s = t0 * rate + ((double) i * (regionLenS - n) / (numW - 1));
                winStarts.push_back ((int64_t) std::clamp (s, 0.0, dur * rate - n));
            }
        }
    }

    outMags.clear();
    const int order = (int) std::log2 ((double) n);

    // Window function, computed once and shared by all channels.
    std::vector<float> win ((size_t) n, 1.0f);
    const int wt = windowCombo.getSelectedId() - 1;
    if (wt == 1 || wt == 2)
    {
        for (int i = 0; i < n; ++i)
            win[(size_t) i] = (wt == 1)
                ? 0.5f * (1.0f - std::cos (2.0 * juce::MathConstants<double>::pi * i / n))
                : 0.54f - 0.46f * std::cos (2.0 * juce::MathConstants<double>::pi * i / (n - 1));
    }

    const double dBMin = -(double) app.settings.dBRange;
    for (int ch : app.visibleChannels)
    {
        if (ch < 0 || ch >= app.audioEngine.numChannels()) continue;

        // Average the linear magnitudes of all windows in this channel.
        std::vector<double> acc ((size_t) (n / 2 + 1), 0.0);
        std::vector<float> samples ((size_t) n, 0.0f);   // zero-filled: partial reads at the file end stay silent
        std::vector<float> buf ((size_t) n * 2, 0.0f);
        for (int64_t s : winStarts)
        {
            std::fill (samples.begin(), samples.end(), 0.0f);
            if (!app.audioEngine.readSamples (ch, s, n, samples.data())) continue;
            for (int i = 0; i < n; ++i)
                buf[i] = samples[i] * win[(size_t) i];
            scfft::realForward (buf.data(), order);      // AVX2 when available via scfft
            for (int k = 0; k <= n / 2; ++k)
            {
                const float re = buf[k * 2], im = buf[k * 2 + 1];
                acc[(size_t) k] += std::sqrt ((double) re * re + (double) im * im);
            }
        }

        std::vector<float> mag ((size_t) (n / 2 + 1));
        for (int k = 0; k <= n / 2; ++k)
        {
            const double m = acc[(size_t) k] / ((double) winStarts.size() * n);
            const double dB = 20.0 * std::log10 (std::max (1e-12, m));
            mag[k] = (float) std::clamp ((dB - dBMin) / (-dBMin), 0.0, 1.0);
        }
        outMags.push_back (std::move (mag));
    }
}

void FFTAnalysisView::recomputeIfNeeded()
{
    const int w = getWidth(), h = getHeight();
    if (w <= 0 || !app.audioEngine.isFileLoaded()) return;

    // Region to analyse: the selection when non-empty, otherwise the playhead
    // position.
    double t0, t1;
    if (!app.selection.isEmpty()) { t0 = app.selection.getStart(); t1 = app.selection.getEnd(); }
    else                          { t0 = app.playhead;          t1 = app.playhead; }

    const int n = localN;
    const auto file = app.audioEngine.getFile();

    // A new file invalidates every stored snapshot: their time segments belong
    // to the previous file.
    if (!mags.empty() && cachedFile != file)
        for (int i = 0; i < 10; ++i)
        {
            snapshots[i] = Snapshot{};
            snapBtns[i].setToggleState (false, juce::dontSendNotification);
            snapBtns[i].setStored (false);
            updateSnapshotTooltip (i);
        }

    if (mags.size() == app.visibleChannels.size() && cachedN == n && cachedT0 == t0
        && cachedT1 == t1 && cachedFile == file
        && std::abs (cachedDB - (float) app.settings.dBRange) < 0.5f
        && (axisMode_ != 2 || thirdOctMags.size() == mags.size()))
        return;

    // Throttle only repeated expensive multi-window averaging (e.g. while
    // dragging a wide selection). Single-window updates are cheap enough to
    // run on every click or playhead move, and the first compute of a new
    // region always runs so clicks stay responsive.
    const int64_t now = juce::Time::getMillisecondCounter();
    const double regionLenS = std::max (0.0, (t1 - t0) * app.audioEngine.sampleRate());
    if (!mags.empty() && regionLenS > (double) n && lastComputeWasExpensive
        && now - lastComputeMs < 60)
        return;

    computeRegion (t0, t1, n, mags);

    // 1/3-octave band values for mode 2 (power-averaged per band).
    // Replace the cache on every recompute — never accumulate, or stale
    // curves from earlier clicks would be drawn alongside the live one.
    thirdOctMags.clear();
    if (axisMode_ == 2)
        for (auto& m : mags)
            thirdOctMags.push_back (toThirdOctBands (m, n, app.audioEngine.sampleRate(),
                                                     app.settings.dBRange));

    cachedN = n; cachedT0 = t0; cachedT1 = t1;
    cachedDB = (float) app.settings.dBRange;
    cachedFile = file;
    lastComputeMs = now;
    lastComputeWasExpensive = (regionLenS > (double) n);
}

void FFTAnalysisView::recomputeSnapshots()
{
    if (!app.audioEngine.isFileLoaded()) return;
    for (auto& s : snapshots)
        if (s.n > 0 && s.gen != settingsGen)
        {
            // Recompute this snapshot from its own stored time segment using
            // the current FFT size and window type.
            computeRegion (s.t0, s.t1, localN, s.mags);
            s.n = localN;
            s.gen = settingsGen;
        }
    refreshSnapshotBands();
}

void FFTAnalysisView::refreshSnapshotBands()
{
    if (!app.audioEngine.isFileLoaded()) return;
    const double rate = app.audioEngine.sampleRate();
    for (auto& s : snapshots)
        if (s.n > 0 && !s.mags.empty())
        {
            s.bands.clear();
            for (auto& m : s.mags)
                s.bands.push_back (toThirdOctBands (m, s.n, rate, app.settings.dBRange));
        }
}

juce::Rectangle<int> FFTAnalysisView::plotBounds() const
{
    const auto b = getLocalBounds();
    const int topH = 52;      // two control rows
    const int axisR = 34, axisB = 18, readoutH = 16;
    return { 2, topH + 2,
             std::max (1, b.getWidth() - axisR - 4),
             std::max (1, b.getHeight() - topH - axisB - readoutH - 4) };
}

float FFTAnalysisView::xToFreq (int x) const
{
    const auto plot = plotBounds();
    if (plot.getWidth() <= 0) return 0.0f;
    const double rate = app.audioEngine.sampleRate();
    double fLo, fHi; fftAxisLimits (axisMode_, rate, fLo, fHi);
    const double t = std::clamp ((double) (x - plot.getX()) / (double) plot.getWidth(), 0.0, 1.0);
    if (axisMode_ == 0)
        return (float) (fLo + t * (fHi - fLo));
    return (float) std::exp (std::log (fLo) + t * (std::log (fHi) - std::log (fLo)));
}

double FFTAnalysisView::freqToX (double f) const
{
    const auto plot = plotBounds();
    if (plot.getWidth() <= 0) return (float) plot.getX();
    const double rate = app.audioEngine.sampleRate();
    double fLo, fHi; fftAxisLimits (axisMode_, rate, fLo, fHi);
    double t;
    if (axisMode_ == 0)
        t = (f - fLo) / std::max (1e-9, fHi - fLo);
    else
        t = (std::log (std::max (1.0, f)) - std::log (fLo)) / (std::log (fHi) - std::log (fLo));
    return plot.getX() + t * plot.getWidth();
}

void FFTAnalysisView::resized()
{
    auto b = getLocalBounds();
    // Row 1: FFT size combo + window combo. Row 2: linear toggle + snapshots.
    auto row1 = b.removeFromTop (26);
    fftSizeCombo.setBounds (30, row1.getY(), 85, 22);
    windowCombo.setBounds (148, row1.getY(), 90, 22);

    auto row2 = b.removeFromTop (26);
    axisCombo.setBounds (4, row2.getY() + 2, 90, 22);
    for (int i = 0; i < 10; ++i)
        snapBtns[i].setBounds (98 + i * 24, row2.getY() + 2, 23, 20);

    repaint();
}

void FFTAnalysisView::paint (juce::Graphics& g)
{
    const auto fullBounds = getLocalBounds();
    g.fillAll (juce::Colours::black);

    // Top control rows background + labels
    const int topH = 52;
    const auto topRow = juce::Rectangle<int> (fullBounds).removeFromTop (topH);
    g.setColour (juce::Colour::fromRGB (45, 45, 48));
    g.fillRect (topRow);
    g.setColour (juce::Colours::white);
    g.setFont (11.0f);
    g.drawText ("FFT", 6, topRow.getY() + 5, 22, 16, juce::Justification::centredLeft, false);
    g.drawText ("Win", 124, topRow.getY() + 5, 22, 16, juce::Justification::centredLeft, false);

    const int axisR = 34;   // right dB axis width
    const int axisB = 18;   // bottom Hz axis height
    const int readoutH = 16; // cursor info strip at the very bottom
    const auto plot = plotBounds();

    if (!app.audioEngine.isFileLoaded())
    {
        g.setColour (juce::Colours::grey);
        g.setFont (12.0f);
        g.drawText ("Load a file to see the frequency analysis", plot, juce::Justification::centred);
        return;
    }

    recomputeIfNeeded();

    const double rate = app.audioEngine.sampleRate();
    double fLo, fHi; fftAxisLimits (axisMode_, rate, fLo, fHi);
    const int n = cachedN;
    const bool haveSpectrum = (n > 1) && !mags.empty()
                              && (axisMode_ != 2 || !thirdOctMags.empty());

    // Frequency grid ticks for the current axis mode
    std::vector<double> freqTicks;
    if (axisMode_ == 2)
    {
        // One grid line per 1/3-octave band centre.
        for (int b = 0; b < kNumThirdOctBands && kThirdOctCenters[b] <= fHi + 1e-6; ++b)
            freqTicks.push_back (kThirdOctCenters[b]);
    }
    else if (axisMode_ == 1)
    {
        for (double p = std::ceil (std::log10 (fLo)); p <= std::floor (std::log10 (fHi)); ++p)
            freqTicks.push_back (std::pow (10.0, p));
    }
    else
    {
        const double span = fHi - fLo;
        for (double step : { 100.0, 250.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
            if (span / step >= 3.0 && span / step <= 9.0)
            { for (double f = step; f < fHi - 1e-6; f += step) freqTicks.push_back (f); break; }
        if (freqTicks.empty())
            for (int i = 1; i <= 4; ++i) freqTicks.push_back (span * (double) i / 5.0);
    }

    // Grid: frequency ticks + dB lines
    g.setColour (juce::Colour::fromRGB (40, 40, 46));
    for (double f : freqTicks)
        g.drawVerticalLine ((float) freqToX (f), plot.getY(), plot.getBottom());
    for (int db = 0; db >= -(int) app.settings.dBRange; db -= 12)
    {
        const float y = plot.getY() + (float) ((double) -db / (double) app.settings.dBRange) * plot.getHeight();
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
    }

    // Curves
    if (!haveSpectrum)
    {
        g.setColour (juce::Colours::grey);
        g.setFont (11.0f);
        g.drawText ("No audio data", plot, juce::Justification::centred);
    }
    else
    {
        // Draw in frequency space: bin k corresponds to f = k * rate / n.
        // (The magnitude vector only has n/2+1 entries — never index it by x!)
        // Each curve is mapped with the FFT size it was computed at, so stored
        // snapshots do not shift when the live analysis uses a different size.
        auto drawCurve = [&](const std::vector<float>& mag, int curveN, const juce::Colour& col)
        {
            g.setColour (col);
            juce::Path path;
            bool started = false;
            for (int k = 1; k <= curveN / 2 && k < (int) mag.size(); ++k)
            {
                const double f = (double) k * rate / (double) curveN;
                if (f < fLo || f > fHi) continue;
                const float x = (float) freqToX (f);
                const float y = plot.getY() + (float) ((1.0f - mag[k]) * plot.getHeight());
                if (!started) { path.startNewSubPath (x, y); started = true; }
                else          path.lineTo (x, y);
            }
            g.strokePath (path, juce::PathStrokeType (1.2f));
        };

        // In 1/3-octave mode each band is drawn as a flat step spanning its
        // frequency edges with vertical connectors at the boundaries — the
        // classic 1/3-octave band-plot look, not a smooth curve.
        auto drawBandCurve = [&](const std::vector<float>& bands, const juce::Colour& col)
        {
            if (bands.empty()) return;
            g.setColour (col);
            juce::Path path;
            bool started = false;
            for (int b = 0; b < kNumThirdOctBands && b < (int) bands.size(); ++b)
            {
                const double fc = kThirdOctCenters[b];
                if (fc > fHi + 1e-6) break;
                const double lo = std::max (fLo, fc * std::pow (2.0, -1.0 / 6.0));
                const double hi = std::min (fHi, fc * std::pow (2.0,  1.0 / 6.0));
                if (hi <= lo) continue;
                const float xLo = (float) freqToX (lo);
                const float xHi = (float) freqToX (hi);
                const float y   = plot.getY() + (float) ((1.0f - bands[(size_t) b]) * plot.getHeight());
                if (!started) { path.startNewSubPath (xLo, y); started = true; }
                else          path.lineTo (xLo, y);    // vertical connector at the shared boundary
                path.lineTo (xHi, y);                  // flat top of this band
            }
            g.strokePath (path, juce::PathStrokeType (1.2f));
        };

        // Stored snapshot overlays first (dimmer), live curves on top.
        for (int i = 0; i < 10; ++i)
            if (snapshots[i].shown && snapshots[i].n > 1 && !snapshots[i].mags.empty())
                for (size_t c = 0; c < snapshots[i].mags.size() && c < 4; ++c)
                    if (axisMode_ == 2)
                    {
                        // Braces are required here: without them the else below
                        // would bind to the inner if and snapshot curves would
                        // never be drawn in linear/log axis modes.
                        if (!snapshots[i].bands.empty())
                            drawBandCurve (snapshots[i].bands[c], fftColours[(i + 1) % 4].withAlpha (90 / 255.0f));
                    }
                    else
                        drawCurve (snapshots[i].mags[c], snapshots[i].n, fftColours[(i + 1) % 4].withAlpha (90 / 255.0f));

        if (axisMode_ == 2)
            for (size_t c = 0; c < mags.size() && c < 4; ++c)
                drawBandCurve (thirdOctMags[c], fftColours[c]);
        else
            for (size_t c = 0; c < mags.size() && c < 4; ++c)
                drawCurve (mags[c], n, fftColours[c]);
    }

    // Axis labels
    g.setColour (juce::Colours::lightgrey);
    g.setFont (9.0f);
    for (double f : freqTicks)
    {
        if (axisMode_ == 2 && !isThirdOctLabelFreq (f)) continue;   // avoid crowding
        const float x = (float) freqToX (f);
        juce::String label;
        if (f >= 1000.0)
            label = juce::String (f / 1000.0, std::abs (f - std::round (f)) < 1e-6 ? 0 : 1) + "k";
        else
            label = juce::String ((int) std::lround (f));
        g.drawText (label, (int) x - 12, plot.getBottom() + 2, 26, axisB - 4, juce::Justification::centred, false);
    }
    for (int db = 0; db >= -(int) app.settings.dBRange; db -= 12)
    {
        const float y = plot.getY() + (float) ((double) -db / (double) app.settings.dBRange) * plot.getHeight();
        g.drawText (juce::String (db), plot.getRight() + 4, (int) y - 7, axisR - 6, 14, juce::Justification::centredLeft, false);
    }

    // Cursor readout
    if (haveSpectrum && cursorX >= plot.getX() && cursorX <= plot.getRight()
        && cursorY >= plot.getY() && cursorY <= plot.getBottom())
    {
        const double f = xToFreq (cursorX);
        juce::String info;
        if (axisMode_ == 2 && !thirdOctMags.empty())
        {
            // Report the band containing the cursor frequency (or nearest centre).
            int b = -1;
            double bestD = 1e30;
            for (int i = 0; i < kNumThirdOctBands && i < (int) thirdOctMags[0].size(); ++i)
            {
                const double fc = kThirdOctCenters[i];
                const double lo = fc * std::pow (2.0, -1.0 / 6.0);
                const double hi = fc * std::pow (2.0,  1.0 / 6.0);
                if (f >= lo && f <= hi) { b = i; break; }
                const double d = std::abs (f - fc);
                if (d < bestD) { bestD = d; b = i; }
            }
            if (b >= 0)
            {
                info = "Cursor:  ~" + juce::String ((int) std::lround (kThirdOctCenters[b])) + " Hz band";
                for (size_t c = 0; c < thirdOctMags.size() && c < 4; ++c)
                    if (b < (int) thirdOctMags[c].size())
                        info += ",  Ch" + juce::String ((int) c + 1) + "="
                              + juce::String (-app.settings.dBRange + thirdOctMags[c][(size_t) b] * app.settings.dBRange, 2) + " dB";
            }
        }
        else
        {
            info = "Cursor:  " + juce::String ((int) std::lround (f)) + " Hz";
            for (size_t c = 0; c < mags.size() && c < 4; ++c)
            {
                if (mags[c].empty()) continue;
                const int maxBin = std::min ((int) (n / 2), (int) mags[c].size() - 1);
                const int bin = (int) std::clamp ((double) (int) std::lround (f * rate / n),
                                                  0.0, (double) maxBin);
                info += ",  Ch" + juce::String ((int) c + 1) + "=" + juce::String (-app.settings.dBRange + mags[c][bin] * app.settings.dBRange, 2) + " dB";
            }
        }
        g.setColour (juce::Colours::white);
        g.setFont (10.0f);
        g.drawText (info, juce::Rectangle<int> (4, fullBounds.getHeight() - readoutH,
                                                fullBounds.getWidth() - 8, readoutH),
                    juce::Justification::left);

        // Cursor crosshair
        g.setColour (juce::Colour::fromRGBA (255, 255, 255, 90));
        g.drawVerticalLine ((float) cursorX, plot.getY(), plot.getBottom());
    }
}

void FFTAnalysisView::mouseMove (const juce::MouseEvent& e)
{
    cursorX = e.x; cursorY = e.y;
    repaint();
}

void FFTAnalysisView::mouseExit (const juce::MouseEvent&)
{
    cursorX = -1; cursorY = -1;
    repaint();
}

void FFTAnalysisView::setFollowPlayhead (bool f)
{
    if (f != followPlayhead_) { followPlayhead_ = f; cachedT0 = -1.0; } // force recompute
}

// Snapshot buttons: click stores the current curve in the slot and toggles its
// overlay visibility; Ctrl/Shift+click clears the slot entirely.
void FFTAnalysisView::handleSnapshotClick (int i, juce::ModifierKeys mods)
{
    auto& s = snapshots[i];
    const bool stored = (s.n > 0);

    if (mods.isShiftDown())
    {
        // Shift+click clears the slot.
        s = Snapshot{};
        snapBtns[i].setToggleState (false, juce::dontSendNotification);
        snapBtns[i].setStored (false);
        updateSnapshotTooltip (i);
        repaint();
        return;
    }

    if (!stored || mods.isCommandDown())
    {
        // Empty slot: store it. Stored slot + Ctrl+click: re-store the time
        // segment currently being analysed. The live cache is exactly this
        // region at the current settings, so a copy suffices; later changes
        // of FFT size / window type recompute it from t0..t1.
        if (!(cachedN > 1) || mags.empty())
        {
            snapBtns[i].setToggleState (false, juce::dontSendNotification); // nothing to store yet
            return;
        }

        s.t0 = cachedT0;
        s.t1 = cachedT1;
        s.n = cachedN;
        s.gen = settingsGen;
        s.mags = mags;
        refreshSnapshotBands();
        s.shown = true;   // newly captured data is shown
    }
    else
    {
        // Plain click on a stored slot: only toggle the overlay, never update.
        s.shown = !s.shown;
    }

    snapBtns[i].setToggleState (s.shown, juce::dontSendNotification);
    snapBtns[i].setStored (true);
    updateSnapshotTooltip (i);
    repaint();
}

void FFTAnalysisView::updateSnapshotTooltip (int i)
{
    auto& s = snapshots[i];
    const juce::String num = juce::String (i + 1);
    if (s.n > 0)
        snapBtns[i].setTooltip ("Snapshot " + num + ": stores " + formatTimeCode (s.t0)
                                + " - " + formatTimeCode (s.t1)
                                + ". Click shows/hides the stored curve. Ctrl+Click re-stores it from the current selection/cursor. Shift+Click clears it.");
    else
        snapBtns[i].setTooltip ("Snapshot " + num + " is empty. Click stores the currently analysed time segment here and shows its curve.");
}

void FFTAnalysisView::mouseDown (const juce::MouseEvent& e)
{
    if (!e.mods.isRightButtonDown()) return;
    const bool haveData = cachedN > 1 && !mags.empty();
    if (!haveData) return;

    juce::PopupMenu m;
    m.addItem (1, "Copy Spectrum to Clipboard", true, false);
    if (m.showMenu (juce::PopupMenu::Options().withMousePosition()) == 1)
        copySpectrumToClipboard();
}

void FFTAnalysisView::copySpectrumToClipboard()
{
    const double rate = app.audioEngine.sampleRate();
    double fLo, fHi; fftAxisLimits (axisMode_, rate, fLo, fHi);

    juce::StringArray lines;
    juce::String header = "freq_Hz";
    for (size_t c = 0; c < mags.size() && c < 4; ++c)
        header += ",ch" + juce::String ((int) c + 1) + "_dB";
    lines.add (header);

    if (axisMode_ == 2 && !thirdOctMags.empty())
    {
        // One line per 1/3-octave band centre.
        for (size_t b = 0; b < thirdOctMags[0].size(); ++b)
        {
            juce::String line = juce::String (kThirdOctCenters[b], 1);
            for (size_t c = 0; c < thirdOctMags.size() && c < 4; ++c)
                if (b < thirdOctMags[c].size())
                    line += "," + juce::String (-app.settings.dBRange + thirdOctMags[c][b] * app.settings.dBRange, 2);
            lines.add (line);
        }
        juce::SystemClipboard::copyTextToClipboard (lines.joinIntoString ("\n"));
        return;
    }

    for (int k = 1; k <= cachedN / 2; ++k)
    {
        const double f = (double) k * rate / (double) cachedN;
        if (f < fLo || f > fHi) continue;
        juce::String line = juce::String (f, 3);
        for (size_t c = 0; c < mags.size() && c < 4; ++c)
        {
            const int bin = std::min (k, (int) mags[c].size() - 1);
            line += "," + juce::String (-app.settings.dBRange + mags[c][(size_t) bin] * app.settings.dBRange, 2);
        }
        lines.add (line);
    }

    juce::SystemClipboard::copyTextToClipboard (lines.joinIntoString ("\n"));
}

// ============================================================ SpectralControlsPanel

SpectralControlsPanel::SpectralControlsPanel (Application& appIn)
    : app (appIn), preview (appIn)
{
    const ColourMap maps[] = { ColourMap::Classic, ColourMap::Viridis, ColourMap::Plasma,
                               ColourMap::Inferno, ColourMap::Magma, ColourMap::Hot,
                               ColourMap::Cool, ColourMap::Grayscale };
    const char* names[] = { "Classic (Default)", "Viridis", "Plasma", "Inferno",
                            "Magma", "Hot", "Cool", "Grayscale" };
    for (int i = 0; i < 8; ++i)
        colourMapCombo.addItem (names[i], (int) maps[i] + 1);

    colourMapCombo.onChange = [this] {
        app.colourMap = (ColourMap) (colourMapCombo.getSelectedId() - 1);
        app.spectralEngine.setColourMap (app.colourMap);
        preview.repaint();
        if (app.onStateChange) app.onStateChange();
    };

    for (int n = 1024; n <= 65536; n *= 2)
        fftSizeCombo.addItem (juce::String (n), n);
    fftSizeCombo.onChange = [this] {
        app.settings.fftSize = fftSizeCombo.getSelectedId();
        if (app.onStateChange) app.onStateChange();
    };

    dBRangeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    dBRangeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 40, 20);
    dBRangeSlider.setRange (20.0, 120.0, 1.0);
    dBRangeSlider.setValue (app.settings.dBRange, juce::dontSendNotification);
    dBRangeSlider.onValueChange = [this] {
        app.settings.dBRange = (float) dBRangeSlider.getValue();
        if (app.onStateChange) app.onStateChange();
    };

    logToggle.setClickingTogglesState (true);
    logToggle.setButtonText (app.settings.logFrequency ? "Logarithmic" : "Linear");
    logToggle.setToggleState (app.settings.logFrequency, juce::dontSendNotification);
    logToggle.onClick = [this] {
        app.settings.logFrequency = logToggle.getToggleState();
        logToggle.setButtonText (app.settings.logFrequency ? "Logarithmic" : "Linear");
        if (app.onStateChange) app.onStateChange();
    };

    resetFreqBtn.setButtonText ("Reset Freq");
    resetFreqBtn.onClick = [this] {
        // Reset the spectral view's local frequency axis to the defaults.
        app.settings.minFrequency = 20.0f;
        app.settings.maxFrequency = 20000.0f;
        if (onResetFreq) onResetFreq();
        if (app.onStateChange) app.onStateChange();
    };

    colourMapCombo.setTooltip ("Colour map used by the spectrogram. 'Classic (Default)' is the "
                                "traditional DAW spectral palette.");
    dBRangeSlider.setTooltip ("Spectral dynamic range in dB: how far below full scale the "
                               "spectrogram displays. Higher values reveal quieter detail.");
    logToggle.setTooltip ("Toggle the spectrogram's frequency axis between logarithmic and linear scaling.");
    resetFreqBtn.setTooltip ("Reset the spectrogram's vertical (frequency) zoom to its default range.");
    fftSizeCombo.setTooltip ("FFT size used by the spectrogram tiles. Larger values give finer "
                             "frequency resolution but render more slowly.");

    colourMapLab.setText ("Colour Map:", juce::dontSendNotification);
    dbRangeLab.setText ("dB Range:", juce::dontSendNotification);
    for (auto* lab : { &colourMapLab, &dbRangeLab })
        lab->setFont (12.0f);

    addAndMakeVisible (&colourMapLab);
    addAndMakeVisible (&colourMapCombo);
    addAndMakeVisible (&preview);
    addAndMakeVisible (&dbRangeLab);
    addAndMakeVisible (&dBRangeSlider);
    addAndMakeVisible (&logToggle);
    addAndMakeVisible (&resetFreqBtn);
    addAndMakeVisible (&fftSizeCombo);
}

void SpectralControlsPanel::refresh()
{
    colourMapCombo.setSelectedId ((int) app.colourMap + 1, juce::dontSendNotification);
    fftSizeCombo.setSelectedId (app.settings.fftSize, juce::dontSendNotification);
    dBRangeSlider.setValue (app.settings.dBRange, juce::dontSendNotification);
    logToggle.setToggleState (app.settings.logFrequency, juce::dontSendNotification);
    logToggle.setButtonText (app.settings.logFrequency ? "Logarithmic" : "Linear");

    preview.repaint();
}

void SpectralControlsPanel::resized()
{
    auto b = getLocalBounds().reduced (6, 4);

    {   // Colour map row
        auto row = b.removeFromTop (28);
        colourMapLab.setBounds (row.removeFromLeft (76));
        colourMapCombo.setBounds (row.reduced (0, 0));
    }

    preview.setBounds (b.removeFromTop (56));
    b.reduce (0, 4);

    {   // dB range row
        auto row = b.removeFromTop (28);
        dbRangeLab.setBounds (row.removeFromLeft (76));
        dBRangeSlider.setBounds (row.reduced (0, 0));
    }

    {   // Log + Reset Freq + FFT size row
        auto row = b.removeFromTop (28);
        logToggle.setBounds (row.removeFromLeft (104));
        resetFreqBtn.setBounds (row.removeFromRight (86));
        fftSizeCombo.setBounds (row.reduced (6, 0));
    }
}

void SpectralControlsPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));
}

// ================================================================= MarkerListPanel

MarkerListPanel::Model::Model (MarkerListPanel* ownerIn) : owner (ownerIn) {}

int MarkerListPanel::Model::getNumRows() { return (int) owner->app.markers.count(); }

void MarkerListPanel::Model::paintRowBackground (juce::Graphics& g, int, int width, int height,
                                                 bool selected)
{
    if (selected)
    {
        g.setColour (juce::Colour::fromRGB (60, 80, 120));
        g.fillRect (0, 0, width, height);
    }
}

void MarkerListPanel::Model::paintCell (juce::Graphics& g, int rowNumber, int columnId,
                                        int width, int height, bool)
{
    if (rowNumber < 0 || rowNumber >= getNumRows()) return;
    const auto& m = owner->app.markers.markers()[(size_t) rowNumber];

    g.setFont (11.0f);
    if (columnId == 1)   // Label
    {
        g.setColour (juce::Colours::white);
        g.drawText (m.name, 4, 0, width - 8, height, juce::Justification::centredLeft, false);
    }
    else
    {
        g.setColour (juce::Colours::lightgrey);
        const juce::String text = (columnId == 2) ? formatTimeCode (m.start)
                                                  : (columnId == 3 ? (m.end < 0 ? "-" : formatTimeCode (m.end))
                                                                   : m.comment);
        g.drawText (text, 4, 0, width - 8, height, juce::Justification::centredLeft, false);
    }
}

void MarkerListPanel::Model::cellDoubleClicked (int rowNumber, int, const juce::MouseEvent&)
{
    if (rowNumber >= 0 && rowNumber < getNumRows())
    {
        owner->selectedRow = rowNumber;
        owner->updateDetails(); // Title/Comment are edited in the detail editors below

        // Double-clicking a marker sets the time selection to its range.
        const Marker& m = owner->app.markers.markers()[(size_t) rowNumber];
        const double end = (m.end > m.start) ? m.end : m.start;
        owner->app.setSelection ({ m.start, end });
        if (owner->app.onStateChange)
            owner->app.onStateChange(); // push the new selection to both views
    }
}

void MarkerListPanel::Model::selectedRowsChanged (int row)
{
    if (row >= 0 && row < getNumRows())
    {
        owner->selectedRow = row;
        owner->updateDetails();
    }
}

void MarkerListPanel::Model::deleteKeyPressed (int row)
{
    if (row >= 0 && row < getNumRows())
    {
        const int id = owner->app.markers.markers()[(size_t) row].id;
        owner->app.markers.removeMarker (id);
        owner->app.markers.saveSidecar (owner->app.audioEngine.getFile());
        owner->selectedRow = -1;
        owner->refresh();
    }
}

MarkerListPanel::MarkerListPanel (Application& appIn)
    : app (appIn), addBtn ("Add Marker"), deleteBtn ("Delete")
{
    auto& header = table.getHeader();
    // JUCE TableHeaderComponent column IDs must be non-zero.
    header.addColumn ("Label", 1, 120);
    header.addColumn ("Begin", 2, 75);
    header.addColumn ("End", 3, 75);
    header.addColumn ("Comment", 4, 120);

    addAndMakeVisible (&table);
    addAndMakeVisible (&addBtn);
    addAndMakeVisible (&deleteBtn);
    addAndMakeVisible (&beginEdit);
    addAndMakeVisible (&endEdit);
    addAndMakeVisible (&lengthEdit);
    addAndMakeVisible (&titleEdit);
    addAndMakeVisible (&commentEdit);

    // Names let unit tests (and future code) find the editors without exposing
    // the private members.
    beginEdit.setName  ("marker-begin-edit");
    endEdit.setName    ("marker-end-edit");
    lengthEdit.setName ("marker-length-edit");
    titleEdit.setName  ("marker-title-edit");
    commentEdit.setName("marker-comment-edit");

    beginEdit.setMultiLine (false);
    endEdit.setMultiLine (false);
    lengthEdit.setMultiLine (false);
    titleEdit.setMultiLine (false);
    // The comment editor is multi-line and stretches to the bottom of the panel;
    // Return inserts a line break instead of committing.
    commentEdit.setMultiLine (true);
    commentEdit.setReturnKeyStartsNewLine (true);

    addBtn.onClick = [this] {
        // Region marker spanning the current selection if there is one,
        // otherwise a point marker at the cursor.
        app.addMarkerAtCursor();
        app.markers.saveSidecar (app.audioEngine.getFile());
        refresh();
        if (app.onStateChange) app.onStateChange();
    };

    deleteBtn.onClick = [this] {
        if (selectedRow >= 0 && selectedRow < (int) app.markers.count())
        {
            const int id = app.markers.markers()[(size_t) selectedRow].id;
            app.markers.removeMarker (id);
            app.markers.saveSidecar (app.audioEngine.getFile());
            selectedRow = -1;
            refresh();
            if (app.onStateChange) app.onStateChange(); // refresh the spectral view's marker overlay
        }
    };

    beginEdit.onTextChange   = [this] { onBeginEndChanged (true); };
    endEdit.onTextChange     = [this] { onBeginEndChanged (false); };
    lengthEdit.onTextChange  = [this] { onLengthChanged(); };
    titleEdit.onTextChange   = [this] { onTitleCommentChanged (true); };
    commentEdit.onTextChange = [this] { onTitleCommentChanged (false); };

    // Right-clicking a row in the table shows the marker context menu. Deep
    // registration is required: clicks on rows are delivered to the row's own
    // component, not to the TableListBox itself.
    table.addMouseListener (this, true);

    addBtn.setTooltip ("Add a marker at the playhead. With an active selection it creates a "
                       "region marker spanning the selection; otherwise a point marker.");
    deleteBtn.setTooltip ("Delete the selected marker from the list and from both views.");
    table.setTooltip ("Double-click a row to edit its details in the fields below. "
                      "Right-click for copy/paste of markers. Press Delete to remove the selected marker.");
    beginEdit.setTooltip ("Begin time of the selected marker, in seconds.");
    endEdit.setTooltip ("End time of the selected marker, in seconds (leave empty for a point marker).");
    lengthEdit.setTooltip ("Length of the selected region marker. Editing it adjusts the End time "
                           "(and vice versa); leave empty to make it a point marker.");
    titleEdit.setTooltip ("Title shown on the marker line and in the Label column.");
    commentEdit.setTooltip ("Free-text comment stored with the marker (saved into WAV cue chunks).");;

    refresh();
}

// Title / Comment editors below the table edit the selected marker in place.
void MarkerListPanel::onTitleCommentChanged (bool isTitle)
{
    if (editing || selectedRow < 0 || selectedRow >= (int) app.markers.count()) return;
    const auto& m = app.markers.markers()[(size_t) selectedRow];
    const juce::String text = (isTitle ? titleEdit : commentEdit).getText().trim();

    if (isTitle)
        app.markers.renameMarker (m.id, text.isEmpty() ? m.name : text);
    else
        app.markers.setComment (m.id, text);

    editing = true;
    updateDetails();
    editing = false;
    table.repaint();
    app.markers.saveSidecar (app.audioEngine.getFile());
    if (app.onStateChange) app.onStateChange(); // refresh the spectral view's marker overlay
}

void MarkerListPanel::onBeginEndChanged (bool isBegin)
{
    if (editing || selectedRow < 0 || selectedRow >= (int) app.markers.count()) return;
    const auto& m = app.markers.markers()[(size_t) selectedRow];
    double begin = parseTimeCode (beginEdit.getText());
    double end = parseTimeCode (endEdit.getText());

    const double dur = app.audioEngine.duration();

    if (m.end < 0) // point marker: begin moves it; typing an end turns it into a region
    {
        if (isBegin && begin >= 0.0 && begin <= dur)
            app.markers.moveMarker (m.id, begin);
        else if (!isBegin && begin >= 0.0 && end > begin + 1e-9)
            app.markers.setRegion (m.id, std::min (begin, dur), std::min (end, dur));
    }
    else
    {
        if (begin >= 0.0 && end > begin && end <= dur + 1e-6)
            app.markers.setRegion (m.id, begin, std::min (end, dur));
    }

    editing = true;
    updateDetails();
    editing = false;
    table.repaint();
    app.markers.saveSidecar (app.audioEngine.getFile());
    if (app.onStateChange) app.onStateChange(); // refresh the spectral view's marker overlay
}

// Editing the Length field adjusts the End time; clearing it turns a region
// marker back into a point marker. (Editing End updates this box via updateDetails.)
void MarkerListPanel::onLengthChanged()
{
    if (editing || selectedRow < 0 || selectedRow >= (int) app.markers.count()) return;
    const auto& m = app.markers.markers()[(size_t) selectedRow];

    double len = parseTimeCode (lengthEdit.getText()); // empty text parses to 0.0
    if (!(len > 0.0))
    {
        if (m.end >= 0)
            app.markers.setPoint (m.id);
    }
    else
    {
        double end = m.start + len;
        const double dur = app.audioEngine.duration();
        if (end > dur) end = dur;
        if (end <= m.start + 1e-9)
            app.markers.setPoint (m.id); // too short to be a region
        else
            app.markers.setRegion (m.id, m.start, end);
    }

    editing = true;
    updateDetails();
    editing = false;
    table.repaint();
    app.markers.saveSidecar (app.audioEngine.getFile());
    if (app.onStateChange) app.onStateChange(); // refresh the spectral view's marker overlay
}

void MarkerListPanel::updateDetails()
{
    if (selectedRow < 0 || selectedRow >= (int) app.markers.count())
    {
        beginEdit.setText ("", juce::dontSendNotification);
        endEdit.setText ("", juce::dontSendNotification);
        lengthEdit.setText ("", juce::dontSendNotification);
        titleEdit.setText ("", juce::dontSendNotification);
        commentEdit.setText ("", juce::dontSendNotification);
        return;
    }

    const auto& m = app.markers.markers()[(size_t) selectedRow];
    editing = true;
    beginEdit.setText (formatTimeCode (m.start), juce::dontSendNotification);
    endEdit.setText (m.end < 0 ? "" : formatTimeCode (m.end), juce::dontSendNotification);
    lengthEdit.setText (m.end < 0 ? "" : formatTimeCode (m.end - m.start),
                        juce::dontSendNotification);
    titleEdit.setText (m.name, juce::dontSendNotification);
    commentEdit.setText (m.comment, juce::dontSendNotification);
    editing = false;
}

void MarkerListPanel::refresh()
{
    table.updateContent();
    table.repaint();
    updateDetails();
}

void MarkerListPanel::selectRow (int row)
{
    selectedRow = (row >= 0 && row < (int) app.markers.count()) ? row : -1;
    updateDetails();
}

// Right-click on the marker table: select the row under the cursor and show
// the copy/paste context menu.
void MarkerListPanel::mouseDown (const juce::MouseEvent& e)
{
    if (!e.mods.isRightButtonDown() || e.mods.isLeftButtonDown())
        return;

    // The event position is relative to the component that was actually hit
    // (a row child of the table); walk up the parent chain to get it in
    // table coordinates.
    float px = e.getPosition().x;
    float py = e.getPosition().y;
    for (auto* c = e.eventComponent; c != nullptr && c != &table;
         c = c->getParentComponent())
    {
        px += static_cast<float> (c->getX());
        py += static_cast<float> (c->getY());
    }
    const auto pInTable = juce::Point<float> (px, py);

    // JUCE 9's TableListBox has no getRowNumberAtPosition; find the row by
    // testing each cell position (marker lists are small).
    int row = -1;
    for (int r = 0; r < table.getNumRows(); ++r)
        if (table.getCellPosition (1, r, true).contains ((int) pInTable.x, (int) pInTable.y))
        {
            row = r;
            break;
        }

    if (row >= 0 && row != selectedRow)
    {
        juce::SparseSet<int> sel;
        sel.addRange ({ row, row + 1 });
        table.setSelectedRows (sel, juce::sendNotificationSync); // -> selectedRowsChanged
    }

    showMarkerContextMenu();
}

void MarkerListPanel::showMarkerContextMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "Copy All Markers", app.markers.count() > 0);
    m.addItem (2, "Paste Markers from Clipboard",
               !juce::SystemClipboard::getTextFromClipboard().trim().isEmpty());

    const int id = m.showMenu (juce::PopupMenu::Options().withMousePosition());
    if (id == 1)
        copyAllMarkers();
    else if (id == 2)
        pasteMarkersFromClipboard();
}

// Copies every marker as tab-separated lines (one per line, CRLF between rows):
//   Label <tab> Begin <tab> End <tab> Comment
// so the text can be pasted straight into a spreadsheet. Tabs/newlines inside
// fields are flattened to spaces to keep each marker on one row.
void MarkerListPanel::copyAllMarkers()
{
    auto flatten = [] (const juce::String& s) {
        return s.replace ("\r\n", " ").replace ("\n", " ")
                .replace ("\r", " ").replace ("\t", " ");
    };

    juce::String out;
    for (const auto& m : app.markers.markers())
    {
        if (!out.isEmpty())
            out << "\r\n";
        out << flatten (m.name) << '\t'
            << formatTimeCode (m.start) << '\t'
            << (m.end < 0 ? juce::String ("-") : formatTimeCode (m.end)) << '\t'
            << flatten (m.comment);
    }

    if (!out.isEmpty())
        juce::SystemClipboard::copyTextToClipboard (out);
}

// Pastes tab-separated marker lines from the clipboard. Each line is
//   Label <tab> Begin <tab> End <tab> Comment
// with an empty or "-" End creating a point marker.
int MarkerListPanel::pasteMarkersFromClipboard()
{
    if (!app.audioEngine.isFileLoaded())
        return 0;

    const juce::String text = juce::SystemClipboard::getTextFromClipboard();
    if (text.trim().isEmpty())
        return 0;

    juce::StringArray lines;
    lines.addLines (text);

    const double dur = app.audioEngine.duration();
    int added = 0;

    for (const auto& line : lines)
    {
        if (line.trim().isEmpty())
            continue;

        juce::StringArray f;
        f.addTokens (line, "\t", "");
        if (f.size() < 2) // need at least Label + Begin
            continue;

        const juce::String name = f[0].trim();
        double begin = parseTimeCode (f[1]);
        if (!(begin >= 0.0))
            continue;
        begin = std::min (begin, dur);

        double end = -1.0;
        if (f.size() > 2)
        {
            const juce::String eTok = f[2].trim();
            if (!eTok.isEmpty() && eTok != "-")
            {
                const double e = parseTimeCode (eTok);
                if (e > begin + 1e-9)
                    end = std::min (e, dur);
            }
        }

        juce::String comment;
        if (f.size() > 3)
            comment = f[3].trim();

        const juce::String label = name.isEmpty() ? "Marker" : name;
        const int id = (end < 0.0) ? app.markers.addPointMarker (begin, label)
                                   : app.markers.addRegionMarker (begin, end, label);
        if (!comment.isEmpty())
            app.markers.setComment (id, comment);
        ++added;
    }

    if (added > 0)
    {
        app.markers.saveSidecar (app.audioEngine.getFile());
        refresh();
        if (app.onStateChange) app.onStateChange(); // refresh the spectral view's marker overlay
    }

    return added;
}

void MarkerListPanel::resized()
{
    auto b = getLocalBounds().reduced (4, 4);
    table.setBounds (b.removeFromTop (std::max (80, (int) (b.getHeight() * 0.42))));

    addBtn.setBounds (b.removeFromTop (26));
    deleteBtn.setTopLeftPosition (addBtn.getRight() + 8, b.getY());
    deleteBtn.setBounds (deleteBtn.getX(), b.getY(), 70, 26);
    b.reduce (0, 4);

    // Detail grid: one label + editor per row. The Comments editor is
    // multi-line, starts on the same row as its label and extends to the
    // bottom of the panel.
    auto row1 = b.removeFromTop (24);
    beginEdit.setBounds (xoffset (row1, 60));

    auto row2 = b.removeFromTop (24);
    endEdit.setBounds (xoffset (row2, 60));

    auto row3 = b.removeFromTop (24);
    lengthEdit.setBounds (xoffset (row3, 60));

    auto row4 = b.removeFromTop (24);
    titleEdit.setBounds (xoffset (row4, 60));

    commentEdit.setBounds (xoffset (b, 60));
}

void MarkerListPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));
    g.setColour (juce::Colours::white);
    g.setFont (12.0f);

    auto b = getLocalBounds().reduced (4, 4);
    const int tableH = std::max (80, (int) (b.getHeight() * 0.42));
    b.removeFromTop (tableH + 30); // skip table + buttons rows
    g.drawText ("Begin:", 6, b.getY(), 52, 20, juce::Justification::centredLeft, false);
    auto row2 = b.reduced (0, 24);
    g.drawText ("End:", 6, row2.getY(), 52, 20, juce::Justification::centredLeft, false);
    auto row3 = b.reduced (0, 48);
    g.drawText ("Length:", 6, row3.getY(), 52, 20, juce::Justification::centredLeft, false);
    auto row4 = b.reduced (0, 72);
    g.drawText ("Title:", 6, row4.getY(), 52, 20, juce::Justification::centredLeft, false);
    auto row5 = b.reduced (0, 96);
    // "Comments:" is wider than the other labels; give it room so it is not
    // clipped (the text still ends before the editor column starts).
    g.drawText ("Comments:", 6, row5.getY(), 72, 20, juce::Justification::centredLeft, false);
}

// ---------------------------------------------------------------- Status bar

void StatusBar::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds();
    g.fillAll (juce::Colour::fromRGB (45, 45, 48));
    g.setColour (juce::Colour::fromRGB (90, 90, 100));
    g.drawLine (0.0f, 0.0f, (float) b.getWidth(), 0.0f);

    g.setFont (12.0f);
    if (!mouseText.isEmpty())
    {
        g.setColour (juce::Colour::fromRGB (205, 205, 215));
        g.drawText (mouseText, 6, 0, std::max (10, b.getWidth() / 2), b.getHeight(),
                    juce::Justification::left, false);
    }

    const juce::String right = selPart.isEmpty() ? playPart
                              : (playPart.isEmpty() ? selPart
                                                   : selPart + "     " + playPart);
    if (!right.isEmpty())
    {
        g.setColour (juce::Colour::fromRGB (160, 190, 235));
        juce::Font f (12.0f);
        const int w = std::min (b.getWidth() - 8,
                                (int) juce::GlyphArrangement::getStringWidth (f, right) + 16);
        g.drawText (right, b.getWidth() - w, 0, w, b.getHeight(),
                    juce::Justification::right, false);
    }
}

void StatusBar::setSelection (double t0, double t1)
{
    const juce::String s = (t1 > t0)
        ? "Sel: " + juce::String (t0, 3) + " - " + juce::String (t1, 3) + " s"
        : juce::String();
    if (s != selPart) { selPart = s; repaint(); }
}

void StatusBar::setPlayhead (double t)
{
    const juce::String s = "Pos: " + juce::String (t, 3) + " s";
    if (s != playPart) { playPart = s; repaint(); }
}
