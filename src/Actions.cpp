// SpectroCraft - recorded actions (Photoshop-style record/replay).
#include "JuceHeader.h"

#include "Application.h"
#include "Actions.h"
#include "PythonScripts.h"

#include <atomic>
#include <algorithm>

namespace sc {

// ------------------------------------------------------------- JSON plumbing

juce::var makeVarObject (std::initializer_list<std::pair<juce::String, juce::var>> props)
{
    auto obj = new juce::DynamicObject();
    for (const auto& kv : props)
        obj->setProperty (kv.first, kv.second);
    return juce::var (obj);
}

juce::var ActionStep::toJson() const
{
    return makeVarObject ({
        { "type", type },
        { "params", params.isVoid () ? juce::var () : params },
        { "prompt", prompt } });
}

ActionStep ActionStep::fromJson (const juce::var& v, std::string* errOut)
{
    auto fail = [&errOut] (const juce::String& msg) {
        if (errOut != nullptr) *errOut = msg.toStdString ();
        return ActionStep {};
    };

    if (!v.isObject())
        return fail ("step must be a JSON object");

    const auto typeVar = v.getProperty ("type", juce::var());
    if (!typeVar.isString() || typeVar.toString().trim().isEmpty())
        return fail ("step is missing its \"type\" string");

    ActionStep s;
    s.type = typeVar.toString().trim();
    s.params = v.getProperty ("params", juce::var());
    const auto promptVar = v.getProperty ("prompt", juce::var());
    if (!promptVar.isVoid())
        s.prompt = (bool) promptVar;
    return s;
}

juce::var Action::toJson() const
{
    juce::var stepsArr;
    for (const auto& s : steps)
        stepsArr.append (s.toJson());
    return makeVarObject ({ { "name", name }, { "steps", stepsArr } });
}

// ------------------------------------------------------------- ActionStore

int ActionStore::add (const juce::String& name)
{
    const auto n = name.trim();
    if (n.isEmpty()) return -1;
    for (size_t i = 0; i < actions_.size (); ++i)
        if (actions_[(size_t) i].name == n) return -1;   // duplicate name

    Action a;
    a.name = n;
    actions_.push_back (std::move (a));
    return (int) actions_.size () - 1;
}

bool ActionStore::rename (int index, const juce::String& newName)
{
    if (!valid (index)) return false;
    const auto n = newName.trim();
    if (n.isEmpty()) return false;
    for (size_t i = 0; i < actions_.size (); ++i)
        if (i != (size_t) index && actions_[(size_t) i].name == n) return false;

    actions_[(size_t) index].name = n;
    return true;
}

void ActionStore::removeSelected (const std::vector<int>& indices)
{
    if (indices.empty()) return;
    auto keep = [&] (int i) {
        for (int j : indices)
            if (j == i) return false;
        return true;
    };

    std::vector<Action> kept;
    kept.reserve (actions_.size());
    for (int i = 0; i < (int) actions_.size (); ++i)
        if (keep (i)) kept.push_back (std::move (actions_[(size_t) i]));
    actions_ = std::move (kept);
}

juce::String ActionStore::toJson() const
{
    juce::var arr;
    for (const auto& a : actions_)
    {
        juce::var steps;
        for (const auto& s : a.steps)
            steps.append (s.toJson());
        arr.append (makeVarObject ({ { "name", a.name }, { "steps", steps } }));
    }
    return juce::JSON::toString (arr);
}

bool ActionStore::loadFromJson (const juce::String& json, std::string* errOut)
{
    if (json.trim().isEmpty()) { actions_.clear (); return true; }

    auto fail = [&errOut] (const juce::String& msg) {
        if (errOut != nullptr) *errOut = msg.toStdString ();
        return false;
    };

    const juce::var root = juce::JSON::parse (json);
    if (!root.isArray())
        return fail ("actions must be a JSON array");

    std::vector<Action> parsed;
    for (int i = 0; i < root.size (); ++i)
    {
        const auto v = root[(size_t) i];
        if (!v.isObject() || !v.getProperty ("name", juce::var()).isString())
            return fail ("action #" + juce::String (i + 1) + " is missing its \"name\" string");

        Action a;
        a.name = v.getProperty ("name", juce::var()).toString().trim();
        if (a.name.isEmpty())
            return fail ("action #" + juce::String (i + 1) + " has an empty name");

        const auto stepsVar = v.getProperty ("steps", juce::var());
        if (!stepsVar.isVoid() && !stepsVar.isArray())
            return fail ("\"steps\" of action \"" + a.name + "\" must be an array");

        for (int j = 0; j < stepsVar.size (); ++j)
        {
            std::string err;
            const auto s = ActionStep::fromJson (stepsVar[(size_t) j], &err);
            if (s.type.isEmpty())
                return fail ("step #" + juce::String (j + 1) + " of action \"" + a.name
                             + "\": " + err);
            a.steps.push_back (s);
        }

        for (size_t k = 0; k < parsed.size (); ++k)
            if (parsed[(size_t) k].name == a.name)
                return fail ("duplicate action name \"" + a.name + "\"");
        parsed.push_back (std::move (a));
    }

    actions_ = std::move (parsed);
    return true;
}

// ------------------------------------------------------------- pretty JSON

namespace {

juce::String jsonEscape (const juce::String& s)
{
    const auto utf = s.toStdString();
    std::string out;
    for (unsigned char c : utf)
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";    break;
            case '\r': out += "\\r";    break;
            case '\t': out += "\\t";    break;
            default:
                if (c < 0x20)
                {
                    // JSON requires escaping every other control character.
                    char buf[8];
                    std::snprintf (buf, sizeof (buf), "\\u%04x", c);
                    out += buf;
                }
                else
                    out += (char) c;
                break;
        }
    return juce::String::fromUTF8 (out.c_str());
}

void prettyRec (const juce::var& v, int depth, std::string& out)
{
    const auto pad = [depth] { return std::string ((size_t) depth * 2, ' '); };

    if (v.isArray())
    {
        if (v.size () == 0) { out += "[]"; return; }
        out += "[\n";
        for (int i = 0; i < v.size (); ++i)
        {
            out += pad ();
            prettyRec (v[(size_t) i], depth + 1, out);
            if (i + 1 < v.size ()) out += ",";
            out += "\n";
        }
        out += pad () + "]";
    }
    else if (v.isObject())
    {
        const auto props = v.getDynamicObject()->getProperties();
        if (props.isEmpty()) { out += "{}"; return; }
        out += "{\n";
        int i = 0;
        for (const auto& kv : props)
        {
            out += pad () + "\"" + jsonEscape (kv.name.toString()).toStdString() + "\": ";
            prettyRec (kv.value, depth + 1, out);
            if (++i < props.size ()) out += ",";
            out += "\n";
        }
        out += pad () + "}";
    }
    else if (v.isString())
        out += "\"" + jsonEscape (v.toString()).toStdString() + "\"";
    else if (v.isBool())
        out += (bool) v ? "true" : "false";
    else if (isNumericVar (v))
        out += v.toString().toStdString ();
    else
        out += "null";
}

} // namespace

juce::String jsonPrettyPrint (const juce::var& v)
{
    std::string s;
    prettyRec (v, 0, s);
    return juce::String::fromUTF8 (s.c_str());
}

// ------------------------------------------------------------- step registry

bool isKnownActionType (const juce::String& type)
{
    using namespace actionTypes;
    return type == kSilence        || type == kFadeIn       || type == kFadeOut
        || type == kNormalize      || type == kChangeGain   || type == kReverse
        || type == kInvert         || type == kTimeStretch  || type == kCatTimeScale
        || type == kFilterEq       || type == kEnvelope     || type == kGenSilence
        || type == kGenNoise       || type == kGenTone      || type == kResample
        || type == kTranscribe     || type == kTrim         || type == kDelete
        || type == kPython         || type == kNoiseSuppress;
}

namespace {

double numParam (const juce::var& p, const char* key, double def)
{
    const auto v = p.getProperty (key, juce::var());
    return isNumericVar (v) ? (double) v : def;
}

bool hasNumParam (const juce::var& p, const char* key)
{
    const auto v = p.getProperty (key, juce::var());
    return isNumericVar (v);
}

std::vector<sc::eq::EqPoint> eqPointsFromJson (const juce::var& p)
{
    // Two accepted encodings: the compact "f,g;f,g" string produced by
    // sc::eq::serializePoints() (what recording stores), or a JSON array of
    // {"f": ..., "g": ...} objects (hand-written actions).
    const auto ptsVar = p.getProperty ("points", juce::var());
    if (ptsVar.isString())
        return sc::eq::parsePoints (ptsVar.toString());

    std::vector<sc::eq::EqPoint> pts;
    const auto arr = ptsVar;
    if (!arr.isArray()) return pts;
    for (int i = 0; i < arr.size (); ++i)
    {
        const auto v = arr[(size_t) i];
        if (!v.isObject()) continue;
        sc::eq::EqPoint pt;
        pt.freqHz = numParam (v, "f", 1000.0);
        pt.gainDb = numParam (v, "g", 0.0);
        pts.push_back (pt);
    }
    std::sort (pts.begin (), pts.end (),
               [] (const auto& a, const auto& b) { return a.freqHz < b.freqHz; });
    return pts;
}

std::vector<sc::env::EnvPoint> envPointsFromJson (const juce::var& p)
{
    // Same dual encoding as eqPointsFromJson(): "t,g;t,g" string or object array.
    const auto ptsVar = p.getProperty ("points", juce::var());
    if (ptsVar.isString())
        return sc::env::parsePoints (ptsVar.toString());

    std::vector<sc::env::EnvPoint> pts;
    const auto arr = ptsVar;
    if (!arr.isArray()) return pts;
    for (int i = 0; i < arr.size (); ++i)
    {
        const auto v = arr[(size_t) i];
        if (!v.isObject()) continue;
        sc::env::EnvPoint pt;
        pt.tSec   = numParam (v, "t", 0.0);
        pt.gainDb = numParam (v, "g", 0.0);
        pts.push_back (pt);
    }
    std::sort (pts.begin (), pts.end (),
               [] (const auto& a, const auto& b) { return a.tSec < b.tSec; });
    return pts;
}

std::vector<std::vector<double>> remixWeightsFromJson (const juce::var& p)
{
    std::vector<std::vector<double>> w;
    const auto arr = p.getProperty ("weights", juce::var());
    if (!arr.isArray()) return w;
    for (int i = 0; i < arr.size (); ++i)
    {
        const auto rowVar = arr[(size_t) i];
        std::vector<double> row;
        if (rowVar.isArray())
            for (int j = 0; j < rowVar.size (); ++j)
                row.push_back ((double) rowVar[(size_t) j]);
        w.push_back (std::move (row));
    }
    return w;
}

} // namespace

bool validateStepParams (const ActionStep& s, std::string* errOut)
{
    auto fail = [&errOut] (const juce::String& msg) {
        if (errOut != nullptr) *errOut = msg.toStdString ();
        return false;
    };

    using namespace actionTypes;
    const auto p = s.params;

    switch (s.type.getCharPointer()[0])
    {
        case 'c':
            if (s.type == kChangeGain && !hasNumParam (p, "db"))
                return fail ("changeGain needs a numeric \"db\" value");
            break;
        case 't':
            if (s.type == kTimeStretch && !hasNumParam (p, "pct"))
                return fail ("timeStretch needs a numeric \"pct\" value");
            else if (s.type == kTranscribe)
            {
                const auto m = p.getProperty ("modelId", juce::var());
                if (!m.isString() || m.toString().isEmpty())
                    return fail ("transcribe needs a \"modelId\" string");
            }
            break;
        case 'r':
            if (s.type == kResample)
            {
                const double rate = numParam (p, "rate", 0.0);
                if (!hasNumParam (p, "rate") || rate <= 0.0)
                    return fail ("resample needs a positive numeric \"rate\"");
            }
            break;
        case 'g':
            if ((s.type == kGenSilence || s.type == kGenNoise || s.type == kGenTone)
                && !hasNumParam (p, "dur"))
                return fail ("generate steps need a numeric \"dur\" value");
            break;
        case 'f':
            if (s.type == kFilterEq && eqPointsFromJson (p).empty())
                return fail ("filterEQ needs at least one point in \"points\"");
            break;
        case 'e':
            if (s.type == kEnvelope && envPointsFromJson (p).empty())
                return fail ("envelope needs at least one point in \"points\"");
            break;
        case 'n':
            if (s.type == kNoiseSuppress)
            {
                const auto m = p.getProperty ("modelId", juce::var());
                if (!m.isString() || m.toString().isEmpty())
                    return fail ("noiseSuppress needs a \"modelId\" string");
            }
            break;
        default:
            break;   // parameterless steps are always valid
    }

    if (!isKnownActionType (s.type))
        return fail ("unknown step type \"" + s.type + "\"");
    return true;
}

// ------------------------------------------------------------- apply one step

bool applyActionStep (Application& app, const ActionStep& s, std::string* errOut)
{
    auto fail = [&errOut] (const juce::String& msg) {
        if (errOut != nullptr) *errOut = msg.toStdString ();
        return false;
    };

    using namespace actionTypes;
    const auto p = s.params;

    if (!app.audioEngine().isFileLoaded())
        return fail ("no file is loaded");

    switch (s.type.getCharPointer()[0])
    {
        case 's':   // silence
            return app.silence (errOut);

        case 'f':
            if (s.type == kFadeIn)      return app.fadeIn (errOut);
            if (s.type == kFadeOut)     return app.fadeOut (errOut);
            if (s.type == kFilterEq)    return app.applyFilterEq (eqPointsFromJson (p), errOut);
            break;

        case 'n':   // normalize / noise suppress
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
            if (s.type == kNoiseSuppress)
                return app.noiseSuppress (
                    p.getProperty ("modelId", juce::var()).toString().toStdString (),
                    "auto",
                    (float) numParam (p, "attenLimDb", 0.0),
                    false, nullptr, errOut);
#endif
            return app.normalize (errOut);

        case 'c':
            if (s.type == kChangeGain)  return app.changeGain ((float) numParam (p, "db", 0.0), errOut);
            if (s.type == kCatTimeScale)
                return app.contentAwareTimeScale (
                    (bool) p.getProperty ("reduceGaps", juce::var()),
                    numParam (p, "percentile", 95.0),
                    numParam (p, "speedFactor", 1.25), errOut);
            break;

        case 'r':
            if (s.type == kReverse)     return app.reverse (errOut);
            if (s.type == kResample)
            {
                const double rate = numParam (p, "rate", 0.0);
                if (rate <= 0.0)        return fail ("resample: invalid rate");
                auto weights = remixWeightsFromJson (p);
                std::atomic<bool> noCancel;
                const bool ok = app.audioEngine().resampleTo (
                    rate, nullptr, &noCancel, errOut,
                    weights.empty () ? nullptr : &weights);
                if (!ok) return false;
                app.audioEngine().rebuildMipmaps (nullptr);
                return true;
            }
            break;

        case 'i':   // invert
            return app.invertSelection (errOut);

        case 't':
            if (s.type == kTimeStretch)
                return app.timeStretch ((float) numParam (p, "pct", 0.0), errOut);
            if (s.type == kTranscribe)
            {
                const auto m = p.getProperty ("modelId", juce::var());
                const auto l = p.getProperty ("language", juce::var());
                return app.transcribe (m.toString().toStdString (),
                                       l.toString().toStdString (),
                                       (bool) p.getProperty ("translate", juce::var()),
                                       (bool) p.getProperty ("fast", juce::var()),
                                       nullptr, errOut);
            }
            if (s.type == kTrim)        return app.trimSelection (errOut);
            break;

        case 'd':   // delete
            return app.del (errOut);

        case 'g':
            if (s.type == kGenSilence)  return app.generateSilence (numParam (p, "dur", 1.0), errOut);
            if (s.type == kGenNoise)
            {
                // type: the name string ("white"/"pink"/"brown") or its enum value.
                sc::NoiseType nt = sc::NoiseType::White;
                const auto tv = p.getProperty ("type", juce::var());
                if (tv.isString())
                {
                    const auto ts = tv.toString().toLowerCase();
                    if (ts == "pink")       nt = sc::NoiseType::Pink;
                    else if (ts == "brown") nt = sc::NoiseType::Brown;
                }
                else if (isNumericVar (tv))
                    nt = (sc::NoiseType) std::clamp ((int) std::lround ((double) tv), 0, 2);

                return app.generateNoise (nt,
                                          (float) numParam (p, "intensity", 1.0),
                                          numParam (p, "dur", 1.0), errOut);
            }
            if (s.type == kGenTone)
            {
                // tones: array of {"f": Hz, "a": amp} objects or [Hz, amp] pairs.
                std::vector<sc::ToneSpec> tones;
                const auto arr = p.getProperty ("tones", juce::var());
                if (arr.isArray())
                    for (int i = 0; i < arr.size (); ++i)
                    {
                        const auto v = arr[(size_t) i];
                        sc::ToneSpec t;
                        if (v.isObject())
                        {
                            t.freqHz    = numParam (v, "f", 440.0);
                            t.amplitude = (float) numParam (v, "a", 0.5);
                        }
                        else if (v.isArray() && v.size () >= 2)
                        {
                            const auto fv = v[0];
                            const auto av = v[1];
                            t.freqHz    = isNumericVar (fv) ? (double) fv : 440.0;
                            t.amplitude = isNumericVar (av) ? (float) (double) av : 0.5f;
                        }
                        else continue;
                        tones.push_back (t);
                    }
                if (tones.empty())      return fail ("genTone: no tones recorded");

                // shape: the name string or its enum value.
                int shapeId = 0;
                const auto sv = p.getProperty ("shape", juce::var());
                if (sv.isString())
                {
                    const auto ss = sv.toString().toLowerCase();
                    if (ss == "square")        shapeId = 1;
                    else if (ss == "sawtooth") shapeId = 2;
                    else if (ss == "triangle") shapeId = 3;
                }
                else if (isNumericVar (sv))
                    shapeId = (int) std::lround ((double) sv);

                const sc::WaveShape shape = (sc::WaveShape) std::clamp (shapeId, 0, 3);
                return app.generateTone (tones, shape, numParam (p, "dur", 1.0), errOut);
            }
            break;

        case 'p':   // python
        {
            const auto pathVar = p.getProperty ("script", juce::var());
            if (!pathVar.isString() || pathVar.toString().isEmpty())
                return fail ("python: no script path recorded");
            const juce::File script (pathVar.toString());
            if (!script.existsAsFile())
                return fail ("python: script not found: " + script.getFileName());

            std::string pyCmd;
            if (!sc::py::findPython (&pyCmd))
                return fail ("python: no Python interpreter found");

            const juce::String text = script.loadFileAsString ();
            return app.runPythonScript (script, text, pyCmd, nullptr, errOut);
        }

        default:
            return fail ("unknown step type \"" + s.type + "\"");
    }

    return fail ("unhandled step type \"" + s.type + "\"");
}

// ------------------------------------------------------------- batch runner

std::vector<juce::String> parseFormatList (const juce::String& text)
{
    std::vector<juce::String> out;
    for (const auto& tok : juce::StringArray::fromTokens (text, ";,", ""))
    {
        auto e = tok.trim ();
        if (e.isEmpty()) continue;
        while (e.startsWith ("*")) e = e.substring (1);
        if (!e.startsWith ("."))  e = "." + e;
        out.push_back (e.toLowerCase());
    }
    return out;
}

namespace {

juce::String applyRenamePattern (const juce::String& pattern, const juce::File& src)
{
    auto name = src.getFileNameWithoutExtension();
    if (name.isEmpty()) name = "audio";
    return pattern.replace ("%Filename%", name);
}

void collectBatchFiles (const juce::File& folder, bool recursive,
                        const std::vector<juce::String>& exts,
                        std::vector<std::pair<juce::File, juce::String>>& out)
{
    // Combined wildcard, e.g. "*.wav;*.mp3".
    juce::StringArray wilds;
    for (const auto& e : exts)
        wilds.add ("*" + e);

    const auto files = folder.findChildFiles (juce::File::findFiles, recursive,
                                              wilds.joinIntoString (";"));

    for (const auto& f : files)
    {
        if (f.isDirectory()) continue;
        const auto ext = f.getFileExtension().toLowerCase();
        bool match = false;
        for (const auto& e : exts)
            if (ext == e) { match = true; break; }
        if (!match) continue;

        // Relative path inside the input folder, used to mirror subfolders.
        juce::String rel;
        if (recursive)
        {
            const auto full = f.getFullPathName();
            const auto pref = folder.getFullPathName() + juce::File::getSeparatorString();
            if (full.startsWith (pref)) rel = full.substring (pref.length());
        }
        out.emplace_back (f, rel);
    }
}

} // namespace

bool runActionBatch (const Action& action,
                     const juce::File& inputFolder,
                     const juce::File& outputFolder,
                     const std::vector<juce::String>& formats,
                     const juce::String& renamePattern,
                     bool includeSubfolders,
                     const juce::File& tempStorePath,
                     std::function<bool(float)> onProgress,
                     std::function<void(const juce::String&)> onStatus,
                     const std::atomic<bool>* cancelOut,
                     std::string* summaryOut)
{
    auto fail = [&summaryOut] (const juce::String& msg) {
        if (summaryOut != nullptr) *summaryOut = msg.toStdString ();
        return false;
    };

    if (!inputFolder.isDirectory())
        return fail ("Input folder does not exist");
    if (formats.empty())
        return fail ("No input formats specified");
    if (action.steps.empty())
        return fail ("The action has no steps to run");

    std::vector<std::pair<juce::File, juce::String>> jobs;
    collectBatchFiles (inputFolder, includeSubfolders, formats, jobs);
    if (jobs.empty())
        return fail ("No files matching the input formats were found in "
                     + inputFolder.getFullPathName());

    int done = 0, failed = 0;
    const int total = (int) jobs.size();

    for (int i = 0; i < total; ++i)
    {
        if (cancelOut != nullptr && cancelOut->load()) break;

        const auto& src = jobs[(size_t) i].first;
        const juce::String rel = jobs[(size_t) i].second;

        // Output path: mirror the relative folder structure, rename the file.
        juce::File out (outputFolder);
        if (!rel.isEmpty())
            out = out.getChildFile (rel);   // includes parent dirs when recursive
        const auto newName = applyRenamePattern (renamePattern, src);
        if (!newName.trim().isEmpty())
            out = out.getSiblingFile (newName);

        if (!out.getParentDirectory().isDirectory())
            out.getParentDirectory().createDirectory();

        // Isolated headless application: its own engine + a throwaway recent-
        // store so the user's settings are never touched.
        Application app (tempStorePath);
        if (!app.openFile (src))
        {
            ++failed;
            if (onStatus) onStatus ("FAILED to load " + src.getFileName());
            continue;
        }

        // Selection-based steps act on the whole file in batch mode.
        app.selectAll();

        bool ok = true;
        std::string err;
        for (const auto& step : action.steps)
        {
            if (!applyActionStep (app, step, &err))
            {
                ok = false;
                break;
            }
        }
        if (ok && !app.audioEngine().saveFileAs (out))
        {
            ok = false;
            err = "saving failed";
        }

        if (ok) ++done; else ++failed;
        if (onStatus) onStatus ((ok ? "OK  " : "FAIL ") + src.getFileName()
                                + (ok ? "" : " - " + juce::String::fromUTF8 (err.c_str())));
        if (onProgress) onProgress ((float) (i + 1) / (float) total);
    }

    if (summaryOut != nullptr)
    {
        juce::String s = "Processed ";
        s += total;
        s += " file(s): ";
        s += done;
        s += " saved, ";
        s += failed;
        s += " failed";
        s += ((cancelOut != nullptr && cancelOut->load()) ? ", cancelled." : ".");
        *summaryOut = s.toStdString ();
    }
    return true;
}

} // namespace sc
