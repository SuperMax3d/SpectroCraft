#include "StoredPlots.h"
#include <algorithm>
#include <cmath>

namespace scm
{
juce::String analysisModeName (AnalysisMode m)
{
    switch (m)
    {
        case AnalysisMode::FftLinear:   return "FFT Linear";
        case AnalysisMode::FftLog:      return "FFT Log";
        case AnalysisMode::ThirdOctave: return "1/3 Octave";
    }
    return {};
}

namespace
{
juce::var numbers (const std::vector<double>& v, int decimals)
{
    juce::Array<juce::var> a;
    a.ensureStorageAllocated ((int) v.size());
    const double k = std::pow (10.0, decimals);
    for (double x : v)
        a.add (std::isfinite (x) ? std::round (x * k) / k : -999.0);
    return juce::var (a);
}

std::vector<double> readNumbers (const juce::var& v)
{
    std::vector<double> out;
    if (const auto* a = v.getArray())
    {
        out.reserve ((size_t) a->size());
        for (const auto& x : *a) out.push_back ((double) x);
    }
    return out;
}
} // namespace

juce::var StoredPlot::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("name", name);
    o->setProperty ("mode", (int) mode);
    o->setProperty ("t0", t0);
    o->setProperty ("t1", t1);
    o->setProperty ("channel", channel);
    if (fftSize > 0) o->setProperty ("fftSize", fftSize);
    o->setProperty ("unit", unit);
    o->setProperty ("colour", colourIndex);
    if (! visible) o->setProperty ("hidden", true);
    o->setProperty ("freqs", numbers (freqs, 3));
    o->setProperty ("db", numbers (db, 2));
    return juce::var (o);
}

bool StoredPlot::fromVar (const juce::var& v, StoredPlot& p)
{
    if (! v.isObject()) return false;
    p.name = v.getProperty ("name", "Plot").toString();
    p.mode = (AnalysisMode) juce::jlimit (0, 2, (int) v.getProperty ("mode", 1));
    p.t0 = (double) v.getProperty ("t0", 0.0);
    p.t1 = (double) v.getProperty ("t1", 0.0);
    p.channel = std::max (0, (int) v.getProperty ("channel", 0));
    p.fftSize = std::max (0, (int) v.getProperty ("fftSize", 0));
    p.unit = v.getProperty ("unit", "dBFS").toString();
    p.colourIndex = (int) v.getProperty ("colour", 0);
    p.visible = ! (bool) v.getProperty ("hidden", false);
    p.freqs = readNumbers (v.getProperty ("freqs", {}));
    p.db = readNumbers (v.getProperty ("db", {}));
    if (p.freqs.size() != p.db.size() || p.freqs.empty()) return false;
    return true;
}

int StoredPlots::nextColour() const
{
    for (int c = 0; c < kMaxPlots; ++c)
        if (std::none_of (list.begin(), list.end(), [c] (const StoredPlot& p) { return p.colourIndex == c; }))
            return c;
    return (int) list.size() % kMaxPlots;
}

bool StoredPlots::add (StoredPlot p)
{
    if (full() || p.freqs.empty() || p.freqs.size() != p.db.size()) return false;
    reduce (p.freqs, p.db, kMaxPoints);
    p.colourIndex = nextColour();
    if (p.name.isEmpty()) p.name = "Plot " + juce::String ((int) list.size() + 1);
    list.push_back (std::move (p));
    return true;
}

void StoredPlots::remove (int index)
{
    if (juce::isPositiveAndBelow (index, (int) list.size()))
        list.erase (list.begin() + index);
}

void StoredPlots::setVisible (int index, bool v)
{
    if (juce::isPositiveAndBelow (index, (int) list.size())) list[(size_t) index].visible = v;
}

void StoredPlots::rename (int index, const juce::String& name)
{
    if (juce::isPositiveAndBelow (index, (int) list.size()) && name.trim().isNotEmpty())
        list[(size_t) index].name = name.trim();
}

juce::var StoredPlots::toVar() const
{
    if (list.empty()) return {};
    juce::Array<juce::var> a;
    for (const auto& p : list) a.add (p.toVar());
    return juce::var (a);
}

void StoredPlots::fromVar (const juce::var& v)
{
    list.clear();
    if (const auto* a = v.getArray())
        for (const auto& item : *a)
        {
            StoredPlot p;
            if (StoredPlot::fromVar (item, p) && ! full())
                list.push_back (std::move (p));
        }
}

void StoredPlots::reduce (std::vector<double>& freqs, std::vector<double>& db, int maxPoints)
{
    if ((int) freqs.size() <= maxPoints || maxPoints < 2) return;
    // Log-spaced edges between the first positive frequency and the last one.
    size_t first = 0;
    while (first < freqs.size() && freqs[first] <= 0.0) ++first;
    if (first + 2 >= freqs.size()) return;
    const double lo = freqs[first], hi = freqs.back();
    std::vector<double> nf, nd;
    nf.reserve ((size_t) maxPoints);
    nd.reserve ((size_t) maxPoints);
    const int buckets = maxPoints;
    size_t i = first;
    for (int b = 0; b < buckets && i < freqs.size(); ++b)
    {
        const double edge = lo * std::pow (hi / lo, (double) (b + 1) / buckets);
        double best = -1e300, bestF = freqs[i];
        bool any = false;
        while (i < freqs.size() && (freqs[i] <= edge || b == buckets - 1))
        {
            if (db[i] > best) { best = db[i]; bestF = freqs[i]; }
            any = true;
            ++i;
        }
        if (any) { nf.push_back (bestF); nd.push_back (best); }
    }
    freqs = std::move (nf);
    db = std::move (nd);
}
} // namespace scm
