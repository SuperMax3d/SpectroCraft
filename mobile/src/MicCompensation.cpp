#include "MicCompensation.h"
#include "AudioEngine.h"
#include "Measures.h"
#include "MobileDocument.h"
#include <algorithm>
#include <cmath>

namespace scm::miccomp
{
namespace
{
/** Linear in log frequency between the points, flat beyond the ends. */
double interpolateLogF (const std::vector<double>& hz, const std::vector<double>& db, double f) noexcept
{
    const size_t n = std::min (hz.size(), db.size());
    if (n == 0 || f <= 0.0) return 0.0;
    if (f <= hz.front()) return db.front();
    if (f >= hz[n - 1]) return db[n - 1];
    const auto it = std::upper_bound (hz.begin(), hz.begin() + (long) n, f);
    const size_t hi = (size_t) (it - hz.begin()), lo = hi - 1;
    const double t = std::log (f / hz[lo]) / std::log (hz[hi] / hz[lo]);
    return db[lo] + t * (db[hi] - db[lo]);
}

bool isNumber (const juce::String& s)
{
    const auto t = s.trim();
    return t.isNotEmpty() && t.containsOnly ("0123456789.-+eE") && t.containsAnyOf ("0123456789");
}

/** The fields of a CSV line (commas; quotes may hold commas, "" is a quote). */
juce::StringArray splitCsv (const juce::String& line)
{
    juce::StringArray out;
    juce::String cur;
    bool quoted = false;
    for (int i = 0; i < line.length(); ++i)
    {
        const auto c = line[i];
        if (quoted)
        {
            if (c == '"' && i + 1 < line.length() && line[i + 1] == '"') { cur << '"'; ++i; }
            else if (c == '"') quoted = false;
            else cur << juce::String::charToString (c);
        }
        else if (c == '"') quoted = true;
        else if (c == ',') { out.add (cur.trim()); cur = {}; }
        else cur << juce::String::charToString (c);
    }
    out.add (cur.trim());
    return out;
}

juce::String quoted (const juce::String& s)
{
    return "\"" + s.replace ("\"", "\"\"") + "\"";
}
} // namespace

// ------------------------------------------------------------------ response

double Response::at (double f) const noexcept { return interpolateLogF (hz, db, f); }

Response parseResponse (const juce::String& text, juce::String& error)
{
    std::vector<std::pair<double, double>> pts;
    for (auto line : juce::StringArray::fromLines (text))
    {
        line = line.trim();
        if (line.isEmpty()) continue;
        juce::StringArray tok;
        if (line.containsAnyOf (";\t"))
        {
            // semicolons or tabs: a comma is a decimal comma
            tok.addTokens (line.replaceCharacter (',', '.'), ";\t", "\"");
        }
        else
            tok.addTokens (line, ", ", "\"");
        tok.trim();
        tok.removeEmptyStrings();
        if (tok.size() < 2 || ! isNumber (tok[0]) || ! isNumber (tok[1])) continue;   // a header or a comment
        const double f = tok[0].getDoubleValue(), d = tok[1].getDoubleValue();
        if (f > 0.0 && std::isfinite (d)) pts.emplace_back (f, d);
    }
    Response r;
    if (pts.empty()) { error = "No frequency and dB pairs were found in the file."; return r; }
    std::sort (pts.begin(), pts.end());
    for (const auto& [f, d] : pts)
        if (r.hz.empty() || f > r.hz.back()) { r.hz.push_back (f); r.db.push_back (d); }
    return r;
}

// ------------------------------------------------------------------ bands

Bands thirdOctaveDbfs (const float* x, int n, double rate)
{
    Bands b;
    if (x == nullptr || n <= 0 || rate <= 0.0) return b;
    AudioEngine eng;
    if (! eng.initNew (1, rate) || ! eng.appendSamples (x, n)) return b;
    sc::measures::ThirdOctaveParams p;
    p.t0 = 0.0;
    p.t1 = (double) n / rate;
    p.channels = { 0 };
    p.mode = sc::measures::ThirdOctaveParams::Mode::Iec;
    p.fftSize = 8192;
    const auto r = sc::measures::thirdOctave (eng, p, nullptr, {});
    if (r.channels.empty()) return b;
    const auto& ch = r.channels.front();
    for (size_t i = 0; i < r.centresHz.size() && i < ch.relDb.size(); ++i)
    {
        b.hz.push_back (r.centresHz[i]);
        b.db.push_back ((double) ch.relDb[i] + ch.cal.levelOffsetDb);
    }
    return b;
}

// ------------------------------------------------------------------ compensation

double Compensation::at (double f) const noexcept
{
    return valid() ? interpolateLogF (hz, correctionDb, f) : 0.0;
}

double Compensation::fullScalePa() const noexcept
{
    return 2e-5 * std::pow (10.0, fullScaleDbSpl / 20.0);
}

Compensation compare (const Bands& ref, double refFullScaleDbSpl, const Response& refResponse,
                      const Bands& phone, juce::String& error)
{
    Compensation c;
    const size_t n = ref.hz.size();
    if (n == 0 || phone.hz.size() != n || ref.db.size() != n || phone.db.size() != n)
    {
        error = "The two takes could not be compared.";
        return c;
    }
    const double maxR = *std::max_element (ref.db.begin(), ref.db.end());
    const double maxP = *std::max_element (phone.db.begin(), phone.db.end());
    std::vector<double> refSpl (n), gap (n);
    std::vector<bool> ok (n);
    std::vector<double> mid;
    for (size_t b = 0; b < n; ++b)
    {
        refSpl[b] = ref.db[b] + refFullScaleDbSpl - refResponse.at (ref.hz[b]);   // the true level there
        gap[b] = refSpl[b] - phone.db[b];
        ok[b] = ref.db[b] > maxR - 45.0 && phone.db[b] > maxP - 45.0 && ref.db[b] > -140.0 && phone.db[b] > -140.0;
        if (ok[b] && ref.hz[b] >= 250.0 && ref.hz[b] <= 4000.0) mid.push_back (gap[b]);
    }
    if (mid.empty())
    {
        error = "Not enough sound between 250 Hz and 4 kHz in both takes: play the pink noise (or a steady broadband sound) louder.";
        return c;
    }
    std::sort (mid.begin(), mid.end());
    const double cal = mid.size() % 2 == 1 ? mid[mid.size() / 2] : 0.5 * (mid[mid.size() / 2 - 1] + mid[mid.size() / 2]);
    if (cal < 60.0 || cal > 200.0)
    {
        error = "The calibration found (full scale = " + juce::String (cal, 1) + " dB-SPL) is out of range: check the "
                "reference microphone's calibration, and that both takes heard the same sound.";
        return c;
    }
    c.fullScaleDbSpl = cal;
    c.hz = ref.hz;
    c.referenceDbSpl = refSpl;
    c.phoneDbfs = phone.db;
    c.trusted = ok;
    c.correctionDb.assign (n, 0.0);
    for (size_t b = 0; b < n; ++b)
    {
        // an untrusted band takes the nearest trusted band's correction
        size_t src = b;
        if (! ok[b])
        {
            size_t best = n;
            for (size_t k = 0; k < n; ++k)
                if (ok[k] && (best == n || (k > b ? k - b : b - k) < (best > b ? best - b : b - best))) best = k;
            src = best;
        }
        if (src < n) c.correctionDb[b] = juce::jlimit (-40.0, 40.0, gap[src] - cal);
    }
    c.when = juce::Time::getCurrentTime();
    return c;
}

juce::String Compensation::toCsv() const
{
    juce::String s;
    s << "# SpectroCraft microphone compensation: add correction_db to this microphone's levels\n";
    s << "input," << quoted (input) << "\n";
    s << "reference," << quoted (reference) << "\n";
    s << "created," << when.toISO8601 (true) << "\n";
    s << "full_scale_db_spl," << juce::String (fullScaleDbSpl, 3) << "\n";
    s << "frequency_hz,correction_db,trusted,reference_db_spl,mic_dbfs\n";
    for (size_t b = 0; b < hz.size() && b < correctionDb.size(); ++b)
    {
        s << juce::String (hz[b], 1) << "," << juce::String (correctionDb[b], 2) << ","
          << ((b < trusted.size() && ! trusted[b]) ? "0" : "1") << ","
          << (b < referenceDbSpl.size() ? juce::String (referenceDbSpl[b], 2) : juce::String()) << ","
          << (b < phoneDbfs.size() ? juce::String (phoneDbfs[b], 2) : juce::String()) << "\n";
    }
    return s;
}

Compensation Compensation::fromCsv (const juce::String& text, juce::String& error)
{
    Compensation c;
    bool rows = false;
    for (auto line : juce::StringArray::fromLines (text))
    {
        line = line.trim();
        if (line.isEmpty() || line.startsWithChar ('#')) continue;
        const auto f = splitCsv (line);
        if (! rows)
        {
            const auto key = f[0].toLowerCase();
            if (key == "input") c.input = f[1];
            else if (key == "reference") c.reference = f[1];
            else if (key == "created") c.when = juce::Time::fromISO8601 (f[1]);
            else if (key == "full_scale_db_spl") c.fullScaleDbSpl = f[1].getDoubleValue();
            else if (key == "frequency_hz") rows = true;
            continue;
        }
        if (f.size() < 2 || ! isNumber (f[0]) || ! isNumber (f[1])) continue;
        c.hz.push_back (f[0].getDoubleValue());
        c.correctionDb.push_back (f[1].getDoubleValue());
        c.trusted.push_back (f.size() < 3 || f[2].trim() != "0");
        if (f.size() > 3 && isNumber (f[3])) c.referenceDbSpl.push_back (f[3].getDoubleValue());
        if (f.size() > 4 && isNumber (f[4])) c.phoneDbfs.push_back (f[4].getDoubleValue());
    }
    if (! c.valid() || c.input.isEmpty() || c.fullScaleDbSpl <= 0.0)
    {
        error = "This is not a microphone compensation file.";
        return {};
    }
    return c;
}

juce::var Compensation::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("input", input);
    o->setProperty ("reference", reference);
    o->setProperty ("fullScaleDbSpl", fullScaleDbSpl);
    juce::Array<juce::var> h, d;
    for (size_t b = 0; b < hz.size() && b < correctionDb.size(); ++b)
    {
        h.add (hz[b]);
        d.add (correctionDb[b]);
    }
    o->setProperty ("hz", h);
    o->setProperty ("db", d);
    return juce::var (o);
}

Compensation Compensation::fromVar (const juce::var& v)
{
    Compensation c;
    if (! v.isObject()) return c;
    c.input = v.getProperty ("input", "").toString();
    c.reference = v.getProperty ("reference", "").toString();
    c.fullScaleDbSpl = (double) v.getProperty ("fullScaleDbSpl", 0.0);
    const auto* h = v.getProperty ("hz", {}).getArray();
    const auto* d = v.getProperty ("db", {}).getArray();
    if (h == nullptr || d == nullptr || h->size() != d->size()) return {};
    for (int i = 0; i < h->size(); ++i)
    {
        c.hz.push_back ((double) (*h)[i]);
        c.correctionDb.push_back ((double) (*d)[i]);
    }
    return c;
}

// ------------------------------------------------------------------ files

juce::File folder()
{
    return MobileDocument::recordingsFolder().getChildFile ("Calibrations");
}

juce::File fileFor (const juce::String& input)
{
    return folder().getChildFile (juce::File::createLegalFileName (input.isNotEmpty() ? input : juce::String ("Microphone"))
                                  + " compensation.csv");
}

Compensation forInput (const juce::String& input)
{
    const auto f = fileFor (input);
    if (! f.existsAsFile()) return {};
    juce::String err;
    auto c = Compensation::fromCsv (f.loadFileAsString(), err);
    return c.input == input ? c : Compensation {};
}

bool save (const Compensation& c, juce::String& error)
{
    folder().createDirectory();
    const auto f = fileFor (c.input);
    if (! f.replaceWithText (c.toCsv()))
    {
        error = "Could not write " + f.getFileName() + ".";
        return false;
    }
    return true;
}
} // namespace scm::miccomp
