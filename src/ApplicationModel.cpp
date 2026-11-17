/*
 * ApplicationModel.cpp - the Model panel's actions on the active file
 * (issue #40): parameters from the Tables panel, the audio and the stored
 * selections handed to the sound model (include/SoundModel.h), analysis,
 * Render Selection, Compare, Render CSV, Export / Import, and the picking
 * on the spectrogram. Behaviour: docs/model-panel.md.
 */
#include "Application.h"
#include "FFTHelpers.h"
#include "Tables.h"

#include <algorithm>
#include <cmath>

namespace
{
    namespace md = sc::model;

    /** Value of parameter p of table t at document time tDoc; the end
        values are held before and after the table. */
    double tableParamAt (const sc::tables::TableData& t, const sc::tables::ParamData& p, double tDoc)
    {
        if (p.values.empty()) return 0.0;
        if (p.values.size() == 1 || ! (t.sampleRate > 0.0))
            return std::isfinite (p.values.front()) ? p.values.front() : 0.0;
        const auto [off, sc] = t.effectiveTimeMap (p);
        double tTab = (tDoc - off) / (std::abs (sc) > 1e-12 ? sc : 1.0);
        tTab = juce::jlimit (0.0, (double) (p.values.size() - 1) / t.sampleRate, tTab);
        const double v = sc::tables::paramValueAt (t, p, tTab);
        return std::isfinite (v) ? v : 0.0;
    }

    /** Finds a parameter by "Table.Param", by its name alone, or by the
        part after the last dot (a CSV whose file name differs). */
    bool findParam (const std::vector<sc::tables::TableData>& tables, const juce::String& name,
                    const sc::tables::TableData*& tOut, const sc::tables::ParamData*& pOut)
    {
        for (const auto& t : tables)
            for (const auto& p : t.params)
                if (t.name + "." + p.name == name) { tOut = &t; pOut = &p; return true; }
        for (const auto& t : tables)
            for (const auto& p : t.params)
                if (p.name == name) { tOut = &t; pOut = &p; return true; }
        if (name.containsChar ('.'))
        {
            const auto tail = name.fromLastOccurrenceOf (".", false, false);
            for (const auto& t : tables)
                for (const auto& p : t.params)
                    if (p.name == tail) { tOut = &t; pOut = &p; return true; }
        }
        return false;
    }

    /** Reads `tables` where they are (no copy): the function must not
        outlive them (every caller uses it at once, on the message thread). */
    md::ParamFn paramFnOver (const std::vector<sc::tables::TableData>* tables)
    {
        // The last name looked up is remembered: renders and the picking
        // guide ask for the same parameter thousands of times in a row.
        struct Cache
        {
            juce::String name;
            bool ok = false;
            const sc::tables::TableData* t = nullptr;
            const sc::tables::ParamData* p = nullptr;
        };
        auto cache = std::make_shared<Cache>();
        return [tables, cache] (const juce::String& name, double t, bool* found) -> double
        {
            if (name != cache->name || cache->name.isEmpty())
            {
                cache->name = name;
                cache->ok = findParam (*tables, name, cache->t, cache->p);
            }
            if (found) *found = cache->ok;
            return cache->ok ? tableParamAt (*cache->t, *cache->p, t) : 0.0;
        };
    }

    /** Merged, sorted copy of a list of ranges. */
    std::vector<md::TimeRange> mergeRanges (std::vector<md::TimeRange> r)
    {
        std::sort (r.begin(), r.end(), [] (const md::TimeRange& a, const md::TimeRange& b) { return a.t0 < b.t0; });
        std::vector<md::TimeRange> out;
        for (const auto& x : r)
        {
            if (! (x.t1 > x.t0)) continue;
            if (! out.empty() && x.t0 <= out.back().t1) out.back().t1 = std::max (out.back().t1, x.t1);
            else out.push_back (x);
        }
        return out;
    }

    bool anyRenderable (const md::Model& m)
    {
        for (const auto& c : m.components)
            if (c.enabled && c.analysed) return true;
        return false;
    }
}

sc::model::Model* Application::soundModel()
{
    auto* d = doc();
    return d != nullptr ? &d->soundModel : nullptr;
}

void Application::soundModelChanged()
{
    if (auto* d = doc()) d->metaDirty = true;
    if (onSoundModelChanged) onSoundModelChanged();
}

juce::StringArray Application::modelParameterNames() const
{
    juce::StringArray out;
    if (const auto* d = doc())
        for (const auto& t : d->tables)
            for (const auto& p : t.params)
                out.add (t.name + "." + p.name);
    return out;
}

sc::model::ParamFn Application::modelParamFn (const FileDoc& d) const
{
    return paramFnOver (&d.tables);
}

sc::model::ParamFn Application::modelParamFnFor (const std::vector<sc::tables::TableData>& tables)
{
    auto copy = std::make_shared<const std::vector<sc::tables::TableData>> (tables);
    auto fn = paramFnOver (copy.get());
    return [copy, fn] (const juce::String& name, double t, bool* found) { return fn (name, t, found); };
}

sc::model::Audio Application::modelAudio (const FileDoc& d) const
{
    md::Audio a;
    const auto& eng = d.engine;
    a.sampleRate = eng.sampleRate();
    a.numChannels = eng.numChannels();
    a.numSamples = eng.numSamples();
    a.read = [&eng] (int ch, int64_t start, int n, float* out)
    {
        std::fill (out, out + n, 0.0f);
        const int64_t s0 = std::max<int64_t> (0, start);
        const int64_t s1 = std::min<int64_t> (eng.numSamples(), start + n);
        if (s1 > s0) eng.readSamples (ch, s0, s1 - s0, out + (s0 - start));
    };
    return a;
}

sc::model::SelectionFn Application::modelSelectionFn (const FileDoc& d) const
{
    const auto* sels = &d.savedSelections;   // read in place, like the parameters
    return [sels] (const juce::String& name, md::SelectionShape& out) -> bool
    {
        for (const auto& s : *sels)
        {
            if (s.name != name) continue;
            const auto bb = s.data.boundingBox();
            if (bb.isEmpty()) return false;
            out.t0 = bb.getX();
            out.t1 = bb.getRight();
            const SpectralSelection sel = s.data;
            const double f0 = bb.getY(), f1 = bb.getBottom();
            // The band the selection covers at time t, scanned in 256 steps
            // over its frequency span (lassos and strokes are not rectangles).
            out.bandAt = [sel, f0, f1] (double t, double& lo, double& hi)
            {
                bool any = false;
                for (int i = 0; i <= 256; ++i)
                {
                    const double f = f0 + (f1 - f0) * i / 256.0;
                    if (! sel.contains (t, f)) continue;
                    if (! any) lo = f;
                    hi = f;
                    any = true;
                }
                return any;
            };
            return true;
        }
        return false;
    };
}

bool Application::analyseModel (int index, juce::String* err)
{
    auto* d = doc();
    if (d == nullptr) { if (err) *err = "Open a file first."; return false; }
    auto& m = d->soundModel;
    const auto audio = modelAudio (*d);
    const auto param = modelParamFn (*d);
    const auto sel = modelSelectionFn (*d);
    const bool ok = index < 0 ? md::analyseAll (m, audio, param, sel, err)
                              : md::analyse (m, index, audio, param, sel, err);
    soundModelChanged();
    return ok;
}

std::vector<sc::model::TimeRange> Application::modelCompareRanges() const
{
    std::vector<md::TimeRange> all;
    if (const auto* d = doc())
    {
        const auto sel = modelSelectionFn (*d);
        for (const auto& c : d->soundModel.components)
        {
            if (! c.enabled || ! c.analysed) continue;
            for (const auto& r : c.ranges) all.push_back (r);
            for (const auto& l : c.lines)
                for (const auto& name : l.selections)
                {
                    md::SelectionShape s;
                    if (sel (name, s)) all.push_back ({ s.t0, s.t1 });
                }
        }
        const double dur = d->engine.duration();
        for (auto& r : all) { r.t0 = juce::jlimit (0.0, dur, r.t0); r.t1 = juce::jlimit (0.0, dur, r.t1); }
    }
    return mergeRanges (all);
}

bool Application::renderModelSelection (juce::String* err)
{
    auto* d = doc();
    if (d == nullptr) { if (err) *err = "Open a file first."; return false; }
    if (! anyRenderable (d->soundModel)) { if (err) *err = "Analyse the model first."; return false; }
    if (selection.isEmpty()) { if (err) *err = "Select a time range first."; return false; }
    auto& eng = d->engine;
    const double rate = eng.sampleRate();
    const int64_t s0 = std::max<int64_t> (0, (int64_t) std::llround (selection.getStart() * rate));
    const int64_t s1 = std::min<int64_t> (eng.numSamples(), (int64_t) std::llround (selection.getEnd() * rate));
    if (s1 <= s0) { if (err) *err = "Select a time range first."; return false; }

    std::vector<std::vector<float>> data (1, std::vector<float> ((size_t) (s1 - s0), 0.0f));
    md::render (d->soundModel, (double) s0 / rate, s1 - s0, rate, modelParamFn (*d), data[0].data());
    const int oldN = eng.numChannels();
    std::string e;
    if (! eng.writeDataToNewChannels (s0, data, "Render Model Selection", &e))
    {
        if (err) *err = juce::String (e);
        return false;
    }
    for (int ch = oldN; ch < eng.numChannels(); ++ch)
        if (! isChannelVisible (ch)) visibleChannels.push_back (ch);
    std::sort (visibleChannels.begin(), visibleChannels.end());
    syncActiveDoc();
    return true;
}

bool Application::compareModel (juce::String* err)
{
    auto* src = doc();
    if (src == nullptr) { if (err) *err = "Open a file first."; return false; }
    if (! anyRenderable (src->soundModel)) { if (err) *err = "Analyse the model first."; return false; }
    const auto ranges = modelCompareRanges();
    if (ranges.empty()) { if (err) *err = "The model has no time ranges to compare."; return false; }

    const auto& eng = src->engine;
    const double rate = eng.sampleRate();
    int ch = 0;
    for (const auto& c : src->soundModel.components)
        if (c.enabled && c.analysed) { ch = juce::jlimit (0, eng.numChannels() - 1, c.channel); break; }

    sc::MergeResult res;
    res.rate = rate;
    res.channels = 2;
    res.data.assign (2, {});
    const auto param = modelParamFn (*src);
    for (const auto& r : ranges)
    {
        const int64_t s0 = (int64_t) std::llround (r.t0 * rate);
        const int64_t s1 = std::min<int64_t> (eng.numSamples(), (int64_t) std::llround (r.t1 * rate));
        if (s1 <= s0) continue;
        const size_t at = res.data[0].size(), n = (size_t) (s1 - s0);
        res.data[0].resize (at + n, 0.0f);
        res.data[1].resize (at + n, 0.0f);
        md::render (src->soundModel, (double) s0 / rate, s1 - s0, rate, param, res.data[0].data() + at);
        eng.readSamples (ch, s0, s1 - s0, res.data[1].data() + at);
    }
    if (res.data[0].empty()) { if (err) *err = "The model's time ranges are empty."; return false; }
    res.markers = src->markers.markers();

    // The markers and the parameters follow: keep the ranges, close the gaps
    // (from the last one back, so earlier positions do not move).
    auto tables = src->tables;
    const double dur = eng.duration();
    std::vector<md::TimeRange> gaps;
    double t = 0.0;
    for (const auto& r : ranges) { if (r.t0 > t) gaps.push_back ({ t, r.t0 }); t = r.t1; }
    if (dur > t) gaps.push_back ({ t, dur });

    std::string e;
    if (! createDocFromMerged (res, &e, src->name + " - model compare"))
    {
        if (err) *err = juce::String (e);
        return false;
    }
    auto* nd = doc();
    nd->tables = std::move (tables);
    nd->tablesMode = src->tablesMode;
    for (auto it = gaps.rbegin(); it != gaps.rend(); ++it)
        nd->remapForDeletion (it->t0, it->t1);
    nd->metaDirty = true;
    if (onTablesChanged) onTablesChanged();
    if (onStateChange) onStateChange();
    return true;
}

bool Application::renderModelCsv (const juce::File& csv, juce::String* err)
{
    auto* src = doc();
    if (src == nullptr) { if (err) *err = "Open the file whose model to render first."; return false; }
    if (! anyRenderable (src->soundModel)) { if (err) *err = "Analyse the model first."; return false; }
    const juce::String text = csv.loadFileAsString();
    if (text.isEmpty()) { if (err) *err = "Could not read " + csv.getFileName() + "."; return false; }
    sc::tables::TableData table;
    std::string e;
    if (! sc::tables::importTable (csv.getFileNameWithoutExtension(), text, table, &e))
    {
        if (err) *err = juce::String (e);
        return false;
    }
    if (table.params.empty() || table.params.front().values.size() < 2 || ! (table.sampleRate > 0.0))
    {
        if (err) *err = "The CSV holds no parameter over time.";
        return false;
    }

    // Every component to render needs its parameter in the CSV.
    std::vector<sc::tables::TableData> tables { table };
    const auto param = modelParamFnFor (tables);
    for (const auto& c : src->soundModel.components)
    {
        if (! c.enabled || ! c.analysed || c.parameter.isEmpty()) continue;
        bool found = false;
        param (c.parameter, 0.0, &found);
        if (! found)
        {
            if (err) *err = "The CSV has no column for \"" + c.parameter + "\" (" + c.name + ").";
            return false;
        }
    }

    const auto [off, sc] = table.effectiveTimeMap (table.params.front());
    const double tEnd = off + sc * (double) (table.params.front().values.size() - 1) / table.sampleRate;
    if (! (tEnd > 0.0)) { if (err) *err = "The CSV's time column ends before 0 s."; return false; }
    const double rate = src->engine.sampleRate() > 0.0 ? src->engine.sampleRate() : 48000.0;
    const int64_t n = (int64_t) std::ceil (tEnd * rate);

    sc::MergeResult res;
    res.rate = rate;
    res.channels = 1;
    res.data.assign (1, std::vector<float> ((size_t) n, 0.0f));
    md::render (src->soundModel, 0.0, n, rate, param, res.data[0].data());
    if (! createDocFromMerged (res, &e, csv.getFileNameWithoutExtension() + " - model"))
    {
        if (err) *err = juce::String (e);
        return false;
    }
    std::string te;
    addImportedTable (doc()->id, std::move (table), &te);
    return true;
}

bool Application::exportModel (const juce::File& target, juce::String* err)
{
    auto* m = soundModel();
    if (m == nullptr || m->empty()) { if (err) *err = "The model is empty."; return false; }
    if (! target.replaceWithText (m->toJson()))
    {
        if (err) *err = "Could not write " + target.getFullPathName() + ".";
        return false;
    }
    return true;
}

bool Application::importModel (const juce::File& source, juce::String* err)
{
    auto* m = soundModel();
    if (m == nullptr) { if (err) *err = "Open a file first."; return false; }
    md::Model loaded;
    if (! md::Model::fromJson (source.loadFileAsString(), loaded, err)) return false;
    *m = std::move (loaded);
    soundModelChanged();
    return true;
}

// ------------------------------------------------------------------ picking

void Application::startModelPick (ModelPick mode, const juce::String& componentId)
{
    modelPick = mode;
    modelPickId = componentId;
    if (onModelPickChanged) onModelPickChanged();
}

void Application::cancelModelPick()
{
    if (modelPick == ModelPick::none) return;
    modelPick = ModelPick::none;
    modelPickId.clear();
    if (onModelPickChanged) onModelPickChanged();
}

int Application::modelPickOrderAt (double t, double fHz) const
{
    const auto* d = doc();
    if (d == nullptr) return 0;
    const int i = d->soundModel.indexOf (modelPickId);
    if (i < 0) return 0;
    const auto& c = d->soundModel.components[(size_t) i];
    const double f0 = c.fundamentalAt (modelParamFn (*d) (c.parameter, t, nullptr));
    if (! (f0 > 0.0) || ! (fHz > 0.0)) return 0;
    return std::max (1, (int) std::lround (fHz / f0));
}

bool Application::modelPickClick (double t, double fHz)
{
    auto* d = doc();
    if (d == nullptr || modelPick == ModelPick::none) return false;
    auto* c = d->soundModel.find (modelPickId);
    if (c == nullptr) { cancelModelPick(); return false; }

    if (modelPick == ModelPick::fundamental)
    {
        // Snap to the strongest bin within 6 % of the click.
        constexpr int order = 13, n = 1 << order;
        std::vector<float> buf ((size_t) n * 2, 0.0f);
        const auto& eng = d->engine;
        const double rate = eng.sampleRate();
        const int ch = juce::jlimit (0, std::max (0, eng.numChannels() - 1), c->channel);
        const int64_t start = (int64_t) std::llround (t * rate) - n / 2;
        modelAudio (*d).read (ch, start, n, buf.data());
        for (int i = 0; i < n; ++i)
            buf[(size_t) i] *= (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / n));
        scfft::realForward (buf.data(), order);
        const double binHz = rate / n;
        const int k0 = std::max (1, (int) std::floor (fHz * 0.94 / binHz));
        const int k1 = std::min (n / 2 - 2, (int) std::ceil (fHz * 1.06 / binHz));
        auto mag = [&] (int k) { return std::hypot (buf[(size_t) k * 2], buf[(size_t) k * 2 + 1]); };
        double f = fHz;
        if (k1 >= k0)
        {
            int m = k0;
            for (int k = k0 + 1; k <= k1; ++k) if (mag (k) > mag (m)) m = k;
            const double a = std::log (mag (m - 1) + 1e-20), b = std::log (mag (m) + 1e-20),
                         cc = std::log (mag (m + 1) + 1e-20);
            const double den = a - 2.0 * b + cc;
            const double delta = std::abs (den) > 1e-12 ? juce::jlimit (-0.5, 0.5, 0.5 * (a - cc) / den) : 0.0;
            f = (m + delta) * binHz;
        }
        const double p = modelParamFn (*d) (c->parameter, t, nullptr);
        if (! c->parameter.isEmpty() && std::abs (p) < 1e-9) return false;   // nothing to scale from
        c->f0Hz = f;
        c->f0AtParam = c->parameter.isEmpty() ? 100.0 : p;
        c->analysed = false;
        modelPick = ModelPick::none;
        modelPickId.clear();
        soundModelChanged();
        if (onModelPickChanged) onModelPickChanged();
        return true;
    }

    // Harmonics: add or remove the order nearest the click.
    const int k = modelPickOrderAt (t, fHz);
    if (k <= 0) return false;
    auto& lines = c->lines;
    const auto it = std::find_if (lines.begin(), lines.end(), [k] (const md::HarmonicLine& l)
                                  { return ! l.isTrack() && std::abs (l.order - k) < 1e-9; });
    if (it != lines.end()) lines.erase (it);
    else
    {
        md::HarmonicLine l;
        l.order = k;
        // Keep the order lines sorted (tracks stay at the end).
        auto pos = std::find_if (lines.begin(), lines.end(), [k] (const md::HarmonicLine& o)
                                 { return o.isTrack() || o.order > k; });
        lines.insert (pos, l);
    }
    c->analysed = false;
    soundModelChanged();
    return true;
}
