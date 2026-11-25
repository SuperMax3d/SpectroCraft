#include "SignalSource.h"
#include <cmath>

namespace scm
{
// ------------------------------------------------------------------ settings

double GeneratorSettings::effectiveSlope() const noexcept
{
    switch (noise)
    {
        case Noise::White:  return 0.0;
        case Noise::Pink:   return -3.0;
        case Noise::Brown:  return -6.0;
        case Noise::Custom: return slopeDbPerOctave;
    }
    return 0.0;
}

juce::var GeneratorSettings::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("kind", (int) kind);
    o->setProperty ("levelDb", levelDb);
    o->setProperty ("toneHz", toneHz);
    o->setProperty ("sweepStartHz", sweepStartHz);
    o->setProperty ("sweepEndHz", sweepEndHz);
    o->setProperty ("sweepSeconds", sweepSeconds);
    o->setProperty ("sweepLog", sweepLog);
    o->setProperty ("sweepRepeat", sweepRepeat);
    o->setProperty ("noise", (int) noise);
    o->setProperty ("slope", slopeDbPerOctave);
    o->setProperty ("lowHz", lowHz);
    o->setProperty ("highHz", highHz);
    return juce::var (o);
}

GeneratorSettings GeneratorSettings::fromVar (const juce::var& v)
{
    GeneratorSettings s;
    if (! v.isObject()) return s;
    s.kind = (Kind) juce::jlimit (0, 2, (int) v.getProperty ("kind", (int) s.kind));
    s.levelDb = juce::jlimit (-100.0, 0.0, (double) v.getProperty ("levelDb", s.levelDb));
    s.toneHz = juce::jlimit (1.0, 96000.0, (double) v.getProperty ("toneHz", s.toneHz));
    s.sweepStartHz = juce::jlimit (1.0, 96000.0, (double) v.getProperty ("sweepStartHz", s.sweepStartHz));
    s.sweepEndHz = juce::jlimit (1.0, 96000.0, (double) v.getProperty ("sweepEndHz", s.sweepEndHz));
    s.sweepSeconds = juce::jlimit (0.1, 3600.0, (double) v.getProperty ("sweepSeconds", s.sweepSeconds));
    s.sweepLog = (bool) v.getProperty ("sweepLog", s.sweepLog);
    s.sweepRepeat = (bool) v.getProperty ("sweepRepeat", s.sweepRepeat);
    s.noise = (Noise) juce::jlimit (0, 3, (int) v.getProperty ("noise", (int) s.noise));
    s.slopeDbPerOctave = juce::jlimit (-12.0, 12.0, (double) v.getProperty ("slope", s.slopeDbPerOctave));
    s.lowHz = juce::jlimit (1.0, 96000.0, (double) v.getProperty ("lowHz", s.lowHz));
    s.highHz = juce::jlimit (1.0, 96000.0, (double) v.getProperty ("highHz", s.highHz));
    return s;
}

juce::String GeneratorSettings::describe() const
{
    const auto lvl = juce::String (levelDb, 1) + " dBFS";
    switch (kind)
    {
        case Kind::Tone:
            return "Tone " + juce::String (toneHz, toneHz < 100.0 ? 2 : 1) + " Hz, " + lvl;
        case Kind::Sweep:
            return juce::String (sweepLog ? "Log" : "Linear") + " sweep "
                 + juce::String (sweepStartHz, 0) + "-" + juce::String (sweepEndHz, 0) + " Hz in "
                 + juce::String (sweepSeconds, 1) + " s, " + lvl;
        case Kind::Noise:
        {
            static const char* names[] = { "White", "Pink", "Brown", "Custom" };
            juce::String s = juce::String (names[(int) noise]) + " noise";
            if (noise == Noise::Custom)
                s << " " << juce::String (slopeDbPerOctave, 1) << " dB/oct";
            return s + ", " + juce::String (lowHz, 0) + "-" + juce::String (highHz, 0) + " Hz, " + lvl;
        }
    }
    return {};
}

// ------------------------------------------------------------------ source

SignalSource::SignalSource()
{
    window.resize ((size_t) kFftSize);
    for (int i = 0; i < kFftSize; ++i)   // periodic sqrt-Hann: w^2 sums to 1 at 50 % overlap
        window[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / kFftSize));
    work.assign ((size_t) kFftSize * 2, 0.0f);
    ola.assign ((size_t) kFftSize / 2, 0.0f);
    ready.assign ((size_t) kFftSize / 2, 0.0f);
    readyPos = kFftSize / 2;   // empty: synthesise on first use
    scratch.resize (4096);
}

SignalSource::~SignalSource() = default;

void SignalSource::setSettings (const GeneratorSettings& s)
{
    const juce::SpinLock::ScopedLockType lock (pendingLock);
    pending = s;
    pendingDirty.store (true);
}

GeneratorSettings SignalSource::settings() const
{
    const juce::SpinLock::ScopedLockType lock (const_cast<juce::SpinLock&> (pendingLock));
    return pending;
}

void SignalSource::prepare (double sampleRate)
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    pendingDirty.store (true);
    phase = 0.0;
    sweepT = 0.0;
    sweepDone.store (false);
    // Force a new noise shape for this rate.
    shape.clear();
    applyPending();
}

void SignalSource::applyPending() noexcept
{
    if (! pendingDirty.load()) return;
    const juce::SpinLock::ScopedTryLockType lock (pendingLock);
    if (! lock.isLocked()) return;   // the message thread is writing: next block
    const auto prev = cur;
    cur = pending;
    pendingDirty.store (false);

    gainLin = std::pow (10.0, cur.levelDb / 20.0);   // RMS amplitude

    const bool kindChanged = prev.kind != cur.kind;
    if (kindChanged)
    {
        phase = 0.0;
        sweepT = 0.0;
        sweepDone.store (false);
    }
    if (cur.kind == GeneratorSettings::Kind::Sweep
        && (prev.sweepStartHz != cur.sweepStartHz || prev.sweepEndHz != cur.sweepEndHz
            || prev.sweepSeconds != cur.sweepSeconds || prev.sweepLog != cur.sweepLog
            || prev.sweepRepeat != cur.sweepRepeat))
    {
        sweepT = 0.0;
        sweepDone.store (false);
    }
    if (cur.kind == GeneratorSettings::Kind::Noise
        && (kindChanged || shape.empty() || prev.noise != cur.noise
            || prev.slopeDbPerOctave != cur.slopeDbPerOctave
            || prev.lowHz != cur.lowHz || prev.highHz != cur.highHz))
        buildNoiseShape();
}

void SignalSource::addTo (float* const* outputs, int numOutputs, int numSamples) noexcept
{
    if (! enabled.load() || numOutputs <= 0 || numSamples <= 0) return;
    applyPending();
    int done = 0;
    while (done < numSamples)
    {
        const int n = std::min (numSamples - done, (int) scratch.size());
        renderBlock (scratch.data(), n);
        for (int ch = 0; ch < numOutputs; ++ch)
            if (auto* o = outputs[ch])
                for (int i = 0; i < n; ++i)
                    o[done + i] = juce::jlimit (-1.0f, 1.0f, o[done + i] + scratch[(size_t) i]);
        done += n;
    }
}

void SignalSource::render (float* out, int numSamples) noexcept
{
    applyPending();
    renderBlock (out, numSamples);
}

void SignalSource::renderBlock (float* out, int n) noexcept
{
    switch (cur.kind)
    {
        case GeneratorSettings::Kind::Tone:  renderTone (out, n); break;
        case GeneratorSettings::Kind::Sweep: renderSweep (out, n); break;
        case GeneratorSettings::Kind::Noise: renderNoise (out, n); break;
    }
}

void SignalSource::renderTone (float* out, int n) noexcept
{
    const double amp = gainLin * juce::MathConstants<double>::sqrt2;   // RMS -> peak
    const double f = juce::jlimit (0.0, rate * 0.5, cur.toneHz);
    const double inc = juce::MathConstants<double>::twoPi * f / rate;
    for (int i = 0; i < n; ++i)
    {
        out[i] = (float) (amp * std::sin (phase));
        phase += inc;
        if (phase >= juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
    }
}

void SignalSource::renderSweep (float* out, int n) noexcept
{
    const double amp = gainLin * juce::MathConstants<double>::sqrt2;
    const double f0 = juce::jlimit (1.0, rate * 0.5, cur.sweepStartHz);
    const double f1 = juce::jlimit (1.0, rate * 0.5, cur.sweepEndHz);
    const double T = std::max (0.01, cur.sweepSeconds);
    const double dt = 1.0 / rate;
    for (int i = 0; i < n; ++i)
    {
        if (sweepDone.load (std::memory_order_relaxed))
        {
            out[i] = 0.0f;
            continue;
        }
        const double x = sweepT / T;
        const double f = cur.sweepLog ? f0 * std::pow (f1 / f0, x) : f0 + (f1 - f0) * x;
        out[i] = (float) (amp * std::sin (phase));
        phase += juce::MathConstants<double>::twoPi * f * dt;
        if (phase >= juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
        sweepT += dt;
        if (sweepT >= T)
        {
            if (cur.sweepRepeat) sweepT -= T;
            else { sweepT = T; sweepDone.store (true); }
        }
    }
    sweepPos.store (sweepT / T);
}

void SignalSource::buildNoiseShape()
{
    const int half = kFftSize / 2;
    shape.assign ((size_t) half + 1, 0.0f);
    const double binHz = rate / kFftSize;
    const double lo = std::max (binHz, std::min (cur.lowHz, cur.highHz));
    const double hi = std::min (rate * 0.5 - binHz, std::max (cur.lowHz, cur.highHz));
    const double expo = cur.effectiveSlope() / (20.0 * std::log10 (2.0));   // dB/oct -> power of f
    double sumSq = 0.0;
    for (int k = 1; k < half; ++k)
    {
        const double f = k * binHz;
        if (f < lo || f > hi) continue;
        const double h = std::pow (f / 1000.0, expo);
        shape[(size_t) k] = (float) h;
        sumSq += h * h;
    }
    // Unit RMS: with JUCE's inverse transform x[n] = (1/N) sum X[k] e^(...),
    // a conjugate-symmetric spectrum of magnitudes A[k] (k = 1..N/2-1) gives a
    // mean square of 2 sum A[k]^2 / N^2, so A = H * N / sqrt (2 sum H^2).
    if (sumSq > 0.0)
    {
        const double c = (double) kFftSize / std::sqrt (2.0 * sumSq);
        for (auto& v : shape) v = (float) (v * c);
    }
    std::fill (ola.begin(), ola.end(), 0.0f);
    readyPos = half;
    // Warm up so the first output block already has the full overlap.
    synthesiseNoiseBlock();
    readyPos = half;
    synthesiseNoiseBlock();
}

void SignalSource::synthesiseNoiseBlock() noexcept
{
    const int N = kFftSize, half = N / 2;
    // Interleaved complex spectrum for performRealOnlyInverseTransform.
    std::fill (work.begin(), work.end(), 0.0f);
    for (int k = 1; k < half; ++k)
    {
        const float a = shape[(size_t) k];
        if (a == 0.0f) continue;
        const double ph = random.nextDouble() * juce::MathConstants<double>::twoPi;
        work[(size_t) (2 * k)] = (float) (a * std::cos (ph));
        work[(size_t) (2 * k + 1)] = (float) (a * std::sin (ph));
    }
    fft.performRealOnlyInverseTransform (work.data());
    // Overlap-add: the first half joins the previous tail, the second half is
    // the new tail. The windowed block has unit-variance-per-sample only after
    // the two overlapping windows are summed (w0^2 + w1^2 = 1).
    for (int i = 0; i < half; ++i)
    {
        ready[(size_t) i] = ola[(size_t) i] + work[(size_t) i] * window[(size_t) i];
        ola[(size_t) i] = work[(size_t) (i + half)] * window[(size_t) (i + half)];
    }
    readyPos = 0;
}

void SignalSource::renderNoise (float* out, int n) noexcept
{
    if (shape.empty())
    {
        std::fill (out, out + n, 0.0f);
        return;
    }
    const int half = kFftSize / 2;
    const float g = (float) gainLin;
    for (int i = 0; i < n; ++i)
    {
        if (readyPos >= half) synthesiseNoiseBlock();
        out[i] = g * ready[(size_t) readyPos++];
    }
}
} // namespace scm
