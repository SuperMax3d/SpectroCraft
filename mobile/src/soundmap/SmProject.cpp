#include "SmProject.h"
#include "FileDoc.h"
#include "MobileDocument.h"
#include "RiffInfo.h"
#include "FileMeta.h"
#include "FileWatch.h"
#include "ScttChunk.h"
#include <cstring>
#include <limits>
#include <set>

namespace scm::smap
{
namespace
{
juce::var arr (std::initializer_list<double> v)
{
    juce::Array<juce::var> a;
    for (double d : v) a.add (d);
    return a;
}

juce::var vecVar (const Vec3& v) { return arr ({ v.x, v.y, v.z }); }

Vec3 varVec (const juce::var& v)
{
    if (! v.isArray() || v.size() < 3) return {};
    return { (double) v[0], (double) v[1], (double) v[2] };
}

juce::var poseVar (const Pose& p)
{
    auto* o = new juce::DynamicObject();
    juce::Array<juce::var> r;
    for (double d : p.R.m) r.add (d);
    o->setProperty ("R", r);
    o->setProperty ("t", vecVar (p.t));
    return o;
}

Pose varPose (const juce::var& v)
{
    Pose p;
    const auto r = v["R"];
    if (r.isArray() && r.size() == 9)
        for (int i = 0; i < 9; ++i) p.R.m[(size_t) i] = (double) r[i];
    p.t = varVec (v["t"]);
    return p;
}

juce::var obsVar (const TagObservation& o)
{
    auto* d = new juce::DynamicObject();
    d->setProperty ("id", o.id);
    juce::Array<juce::var> c;
    for (const auto& p : o.corners) { c.add (p.u); c.add (p.v); }
    d->setProperty ("c", c);
    d->setProperty ("m", o.margin);
    d->setProperty ("h", o.hamming);
    return d;
}

TagObservation varObs (const juce::var& v)
{
    TagObservation o;
    o.id = (int) v["id"];
    const auto c = v["c"];
    if (c.isArray() && c.size() == 8)
        for (int i = 0; i < 4; ++i) o.corners[(size_t) i] = { (double) c[2 * i], (double) c[2 * i + 1] };
    o.margin = (double) v["m"];
    o.hamming = (int) v["h"];
    return o;
}

juce::var obsListVar (const std::vector<TagObservation>& list)
{
    juce::Array<juce::var> a;
    for (const auto& o : list) a.add (obsVar (o));
    return a;
}

std::vector<TagObservation> varObsList (const juce::var& v)
{
    std::vector<TagObservation> out;
    if (auto* a = v.getArray())
        for (const auto& e : *a) out.push_back (varObs (e));
    return out;
}

juce::var levelsVar (const BandLevels& l)
{
    auto* o = new juce::DynamicObject();
    juce::Array<juce::var> f, d;
    for (double x : l.centresHz) f.add (x);
    for (double x : l.db) d.add (x);
    o->setProperty ("f", f);
    o->setProperty ("db", d);
    o->setProperty ("unit", l.unit);
    o->setProperty ("lzeq", l.lzeq);
    o->setProperty ("laeq", l.laeq);
    o->setProperty ("seconds", l.seconds);
    o->setProperty ("calibrated", l.calibrated);
    o->setProperty ("sampleRate", l.sampleRate);
    o->setProperty ("pipeline", l.pipeline);
    return o;
}

BandLevels varLevels (const juce::var& v)
{
    BandLevels l;
    if (auto* f = v["f"].getArray())
        for (const auto& x : *f) l.centresHz.push_back ((double) x);
    if (auto* d = v["db"].getArray())
        for (const auto& x : *d) l.db.push_back ((double) x);
    if (l.db.size() != l.centresHz.size()) { l.db.clear(); l.centresHz.clear(); }
    l.unit = v["unit"].toString();
    l.lzeq = v.hasProperty ("lzeq") ? (double) v["lzeq"] : -300.0;
    l.laeq = v.hasProperty ("laeq") ? (double) v["laeq"] : -300.0;
    l.seconds = (double) v["seconds"];
    l.calibrated = (bool) v["calibrated"];
    l.sampleRate = (double) v["sampleRate"];
    l.pipeline = v["pipeline"].toString();
    return l;
}

juce::var fixVar (const Fix& f)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("ok", f.ok);
    o->setProperty ("mic", vecVar (f.mic));
    o->setProperty ("pose", poseVar (f.camFromMap));
    o->setProperty ("tags", f.tags);
    o->setProperty ("rmsPx", f.rmsPx);
    o->setProperty ("sigmaM", f.sigmaM);
    if (f.problem.isNotEmpty()) o->setProperty ("problem", f.problem);
    return o;
}

Fix varFix (const juce::var& v)
{
    Fix f;
    f.ok = (bool) v["ok"];
    f.mic = varVec (v["mic"]);
    f.camFromMap = varPose (v["pose"]);
    f.tags = (int) v["tags"];
    f.rmsPx = (double) v["rmsPx"];
    f.sigmaM = (double) v["sigmaM"];
    f.problem = v["problem"].toString();
    return f;
}

double num (const juce::var& v, const char* key, double fallback)
{
    return v.hasProperty (key) ? (double) v[key] : fallback;
}

/** Energy sum of the bands from 250 Hz to 2 kHz: a broadband level that random
    errors and the room's low-frequency modes disturb little. */
double midBandsDb (const BandLevels& l) noexcept
{
    double e = 0.0;
    for (size_t i = 0; i < l.centresHz.size(); ++i)
    {
        const double n = nominalFor (l.centresHz[i]);
        if (n >= 250.0 && n <= 2000.0) e += std::pow (10.0, l.db[i] / 10.0);
    }
    return e > 0.0 ? 10.0 * std::log10 (e) : -300.0;
}
} // namespace

Hold holdFromDisplay (juce::Desktop::DisplayOrientation o) noexcept
{
    switch (o)
    {
        case juce::Desktop::upsideDown:           return Hold::PortraitUpsideDown;
        case juce::Desktop::rotatedClockwise:     return Hold::LandscapeTopRight;   // iOS LandscapeLeft, Android ROTATION_270
        case juce::Desktop::rotatedAntiClockwise: return Hold::LandscapeTopLeft;    // iOS LandscapeRight, Android ROTATION_90
        case juce::Desktop::upright:
        case juce::Desktop::allOrientations:
        default:                                  return Hold::Portrait;
    }
}

void MapPoint::updatePosition()
{
    Vec3 sum;
    double wSum = 0.0;
    for (const Fix* f : { &before, &after })
    {
        if (! f->ok) continue;
        const double s = std::max (f->sigmaM, 1e-4);
        const double w = 1.0 / (s * s);
        sum += f->mic * w;
        wSum += w;
    }
    located = wSum > 0.0;
    position = located ? sum / wSum : Vec3 {};
    sigmaM = located ? 1.0 / std::sqrt (wSum) : 0.0;
    movedM = before.ok && after.ok ? (before.mic - after.mic).norm() : 0.0;
}

juce::File SoundMapProject::rootFolder()
{
    return MobileDocument::recordingsFolder().getChildFile ("SoundMaps");
}

juce::Array<juce::File> SoundMapProject::listMaps()
{
    juce::Array<juce::File> out;
    for (const auto& f : rootFolder().findChildFiles (juce::File::findDirectories, false))
        if (f.getChildFile ("project.json").existsAsFile()) out.add (f);
    std::sort (out.begin(), out.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getChildFile ("project.json").getCreationTime() > b.getChildFile ("project.json").getCreationTime();
    });
    return out;
}

std::optional<SoundMapProject> SoundMapProject::create (juce::String& error)
{
    const auto root = rootFolder();
    if (! root.createDirectory())
    {
        error = "The sound maps folder could not be created.";
        return std::nullopt;
    }
    const auto base = "map-" + juce::Time::getCurrentTime().formatted ("%Y-%m-%d-%H%M");
    auto dir = root.getChildFile (base);
    for (int i = 2; dir.exists(); ++i) dir = root.getChildFile (base + "-" + juce::String (i));
    if (! dir.createDirectory() || ! dir.getChildFile ("survey").createDirectory() || ! dir.getChildFile ("points").createDirectory())
    {
        error = "The map's folder could not be created.";
        return std::nullopt;
    }
    SoundMapProject p;
    p.folder = dir;
    p.name = dir.getFileName();
    if (! p.save (error)) return std::nullopt;
    return p;
}

std::optional<SoundMapProject> SoundMapProject::load (const juce::File& folder, juce::String& error)
{
    const auto f = folder.getChildFile ("project.json");
    juce::var v;
    const auto parsed = juce::JSON::parse (f.loadFileAsString(), v);
    if (parsed.failed() || ! v.isObject())
    {
        error = "The map's file could not be read.";
        return std::nullopt;
    }
    return fromVar (v, folder, error);
}

bool SoundMapProject::save (juce::String& error) const
{
    folder.getChildFile ("survey").createDirectory();
    folder.getChildFile ("points").createDirectory();
    juce::TemporaryFile tmp (folder.getChildFile ("project.json"));
    if (! tmp.getFile().replaceWithText (juce::JSON::toString (toVar(), false)) || ! tmp.overwriteTargetFileWithTemporary())
    {
        error = "The map could not be saved.";
        return false;
    }
    return true;
}

juce::File SoundMapProject::nextSurveyPictureFile() const
{
    for (int n = (int) survey.size() + 1;; ++n)
    {
        const auto f = folder.getChildFile ("survey").getChildFile ("S" + juce::String (n).paddedLeft ('0', 3) + ".jpg");
        if (! f.exists()) return f;
    }
}

int SoundMapProject::nextPointNumber() const
{
    int n = 0;
    for (const auto& p : points) n = std::max (n, p.number);
    for (++n; pointFile (n, {}, "wav").exists(); ++n) {}
    return n;
}

juce::File SoundMapProject::pointFile (int number, const juce::String& suffix, const juce::String& ext) const
{
    return folder.getChildFile ("points").getChildFile ("P" + juce::String (number).paddedLeft ('0', 3) + suffix + "." + ext);
}

MapPoint* SoundMapProject::point (int number) noexcept
{
    for (auto& p : points)
        if (p.number == number) return &p;
    return nullptr;
}

std::vector<SurveyPhoto> SoundMapProject::surveyPhotos (bool withPoints) const
{
    std::vector<SurveyPhoto> out;
    auto add = [this, &out] (int w, int h, int hold, const std::vector<TagObservation>& tags)
    {
        SurveyPhoto p;
        p.width = w;
        p.height = h;
        p.hold = holdFromInt (hold);
        for (const auto& t : tags)
            if (t.id < settings.tagCount) p.tags.push_back (t);
        if (p.tags.size() >= 2 && w > 0 && h > 0) out.push_back (std::move (p));
    };
    for (const auto& s : survey) add (s.width, s.height, s.hold, s.tags);
    if (withPoints)
        for (const auto& p : points)
        {
            add (p.width, p.height, p.hold, p.tagsBefore);
            add (p.width, p.height, p.hold, p.tagsAfter);
        }
    return out;
}

SurveySettings SoundMapProject::surveySettings() const
{
    SurveySettings s;
    s.tagSize = settings.tagSizeMm / 1000.0;
    s.lens.fRel = settings.lensFRel;
    s.tagCentreHeight = settings.tagCentreHeightCm / 100.0;
    return s;
}

void SoundMapProject::setMap (const SurveyResult& r)
{
    map = {};
    map.built = r.ok;
    map.lens = r.lens;
    map.tags = r.tags;
    map.rmsPx = r.rmsPx;
    map.picturesUsed = (int) std::count (r.photoUsed.begin(), r.photoUsed.end(), true);
    map.radius = r.frame.circle ? r.frame.radius : 0.0;
    for (const auto& w : r.warnings) map.warnings.add (w);
    map.builtAt = juce::Time::getCurrentTime();
    relocatePoints();
}

void SoundMapProject::relocatePoints()
{
    for (auto& p : points)
    {
        p.before = p.tagsBefore.empty() ? Fix {} : locate (p.tagsBefore, p.width, p.height, p.hold);
        p.after = p.tagsAfter.empty() ? Fix {} : locate (p.tagsAfter, p.width, p.height, p.hold);
        p.updatePosition();
    }
}

double SoundMapProject::levelOf (const MapPoint& p, double band) noexcept
{
    if (! p.levels.valid()) return -300.0;
    if (band == 0.0) return p.levels.lzeq;
    if (band < 0.0) return p.levels.laeq;
    const int i = p.levels.bandFor (band);
    return i >= 0 ? p.levels.db[(size_t) i] : -300.0;
}

juce::String SoundMapProject::mapUnit() const
{
    std::map<juce::String, int> count;
    for (const auto& p : points)
        if (p.kind == PointKind::Point && ! p.pending && p.levels.valid()) ++count[p.levels.unit];
    juce::String best;
    int n = 0;
    for (const auto& [u, k] : count)
        if (k > n) { n = k; best = u; }
    return best;
}

double SoundMapProject::snrDb (const MapPoint& p, double band) const noexcept
{
    const MapPoint* bg = nullptr;
    double best = 1e300;
    for (const auto& q : points)
    {
        if (q.kind != PointKind::Background || q.pending || levelOf (q, band) < -250.0) continue;
        if (q.levels.unit != p.levels.unit) continue;
        const double dt = std::abs ((double) (q.time - p.time).inSeconds());
        if (dt < best) { best = dt; bg = &q; }
    }
    if (bg == nullptr) return std::numeric_limits<double>::infinity();
    return levelOf (p, band) - levelOf (*bg, band);
}

std::vector<std::pair<juce::Time, double>> SoundMapProject::drift (double band) const
{
    std::vector<const MapPoint*> refs;
    for (const auto& q : points)
        if (q.kind == PointKind::Reference && ! q.pending && q.levels.valid()) refs.push_back (&q);
    std::sort (refs.begin(), refs.end(), [] (const MapPoint* a, const MapPoint* b) { return a->time < b->time; });
    std::vector<std::pair<juce::Time, double>> out;
    if (refs.empty()) return out;
    const auto value = [band] (const MapPoint& q) { return band > 0.0 ? levelOf (q, band) : midBandsDb (q.levels); };
    const double first = value (*refs.front());
    for (const auto* q : refs)
        if (value (*q) > -250.0 && first > -250.0 && q->levels.unit == refs.front()->levels.unit)
            out.push_back ({ q->time, value (*q) - first });
    return out;
}

Fix SoundMapProject::locate (const std::vector<TagObservation>& tags, int width, int height, int hold) const
{
    Fix f;
    if (! map.built) { f.problem = "Build the map first."; return f; }
    const auto K = Intrinsics::forPicture (map.lens, width, height, holdFromInt (hold));
    const auto r = resect (tags, map.tags, settings.tagSizeMm / 1000.0, K);
    f.tags = r.tagsUsed;
    f.rmsPx = r.rmsPx;
    f.sigmaM = r.positionSigma;
    f.camFromMap = r.camFromWorld;
    f.ok = r.ok;
    f.problem = r.problem;
    if (r.tagsUsed > 0)
    {
        const Vec3 offsetCam = bodyToCamera (settings.micOffsetCm / 100.0, holdFromInt (hold));
        f.mic = r.camFromWorld.inverse() * offsetCam;
    }
    return f;
}

juce::var SoundMapProject::positionVar (const MapPoint& p) const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("map", name);
    o->setProperty ("point", p.number);
    o->setProperty ("kind", p.kind == PointKind::Background ? "background" : p.kind == PointKind::Reference ? "reference" : "point");
    o->setProperty ("located", p.located);
    if (p.located)   // no position: no numbers that could pass for one
    {
        o->setProperty ("x", p.position.x);
        o->setProperty ("y", p.position.y);
        o->setProperty ("z", p.position.z);
        o->setProperty ("sigma", p.sigmaM);
        o->setProperty ("checked", p.checked());
        o->setProperty ("moved", p.movedM);
    }
    o->setProperty ("unit", "m");
    juce::String frame = map.radius > 0.0 ? "origin at the centre of the tags' circle" : "origin at the centroid of the tags";
    frame << ", z up (z = 0 the set tag height below the tag centres)";
    if (! map.tags.empty()) frame << ", x toward tag " << map.tags.begin()->first;
    o->setProperty ("frame", frame);
    o->setProperty ("updated", juce::Time::getCurrentTime().toISO8601 (true));
    return o;
}

juce::String SoundMapProject::positionText (const MapPoint& p) const
{
    juce::String s = "Sound map " + name + ", P" + juce::String (p.number).paddedLeft ('0', 3) + " ("
                     + (p.kind == PointKind::Background ? "background" : p.kind == PointKind::Reference ? "reference" : "point") + "): ";
    if (! p.located) return s + "no position";
    s << "x " << juce::String (p.position.x, 3) << " m, y " << juce::String (p.position.y, 3) << " m, z "
      << juce::String (p.position.z, 3) << " m, +/- " << juce::roundToInt (p.sigmaM * 1000.0) << " mm";
    if (! p.checked()) s << ", not checked";
    else if (p.moved()) s << ", moved " << juce::String (p.movedM * 100.0, 1) << " cm";
    return s;
}

void SoundMapProject::stampDocument (FileDoc& d, const MapPoint& p) const
{
    d.soundMapPoint = positionVar (p);
    // the app's line is the comment's first line: replace it, keep the rest as it is
    juce::String rest = d.riffInfo["ICMT"];
    if (rest.startsWith ("Sound map " + name + ", P")) rest = rest.fromFirstOccurrenceOf ("\n", false, false);
    d.riffInfo["ICMT"] = positionText (p) + (rest.isNotEmpty() ? "\n" + rest : juce::String());
}

bool SoundMapProject::writeStamp (const FileDoc& d, const juce::File& wav, juce::String& error)
{
    // a .meta.json sidecar holds the metadata when it exists (it wins on load)
    const auto sidecar = sc::fileMetaSidecarPath (wav);
    const bool external = sidecar.existsAsFile();
    if (external)
    {
        sc::files::ScopedWrite guard (sidecar);
        if (! d.writeMetaSidecar (wav)) { error = "The take's metadata file could not be written."; return false; }
    }
    std::vector<RiffPatchEntry> entries;
    if (! external)
    {
        RiffPatchEntry e;
        std::memcpy (e.fourcc, "SCMD", 4);
        e.payload = d.buildScmdJson();
        e.present = ! e.payload.empty();
        entries.push_back (e);
    }
    RiffPatchEntry info;
    std::memcpy (info.fourcc, "LIST", 4);
    std::memcpy (info.listType, "INFO", 4);
    info.payload = sc::riffinfo::buildInfoListPayload (d.riffInfo);
    info.present = ! info.payload.empty();
    entries.push_back (info);
    RiffPatchEntry disp;
    std::memcpy (disp.fourcc, "DISP", 4);
    disp.payload = sc::riffinfo::buildDispPayload (d.riffInfo);
    disp.present = ! disp.payload.empty();
    entries.push_back (disp);
    std::string err;
    if (! patchRiffChunks (wav, entries, &err))
    {
        error = "The take's metadata could not be written.";
        return false;
    }
    return true;
}

bool SoundMapProject::stampWav (const MapPoint& p, juce::String& error) const
{
    const auto wav = file (p.wav);
    if (! wav.existsAsFile()) { error = "The take is missing."; return false; }
    FileDoc d;
    if (! d.load (wav)) { error = "The take could not be read."; return false; }
    stampDocument (d, p);
    return writeStamp (d, wav, error);
}

juce::String SoundMapProject::toCsv() const
{
    std::set<double> bands;
    for (const auto& p : points)
        for (double c : p.levels.centresHz)
            if (const double n = nominalFor (c); n > 0.0) bands.insert (n);
    juce::String s = "point,kind,x_m,y_m,z_m,located,moved_cm,tags,rms_px,sigma_cm,unit,LZeq,LAeq";
    for (double b : bands) s << ",L_" << bandLabel (b) << "Hz";
    s << ",wav\n";
    auto fmt = [] (double v, int d) { return juce::String (v, d); };
    for (const auto& p : points)
    {
        s << p.number << "," << (p.kind == PointKind::Background ? "background" : p.kind == PointKind::Reference ? "reference" : "point")
          << "," << fmt (p.position.x, 3) << "," << fmt (p.position.y, 3) << "," << fmt (p.position.z, 3) << ","
          << (p.located ? 1 : 0) << "," << fmt (p.movedM * 100.0, 1) << "," << p.before.tags << "," << fmt (p.before.rmsPx, 2) << ","
          << fmt (p.sigmaM * 100.0, 1) << "," << p.levels.unit << ","
          << (p.levels.lzeq > -250.0 ? fmt (p.levels.lzeq, 1) : juce::String()) << ","
          << (p.levels.laeq > -250.0 ? fmt (p.levels.laeq, 1) : juce::String());
        for (double b : bands)
        {
            const int i = p.levels.bandFor (b);
            s << "," << (i >= 0 ? fmt (p.levels.db[(size_t) i], 1) : juce::String());
        }
        s << "," << p.wav << "\n";
    }
    return s;
}

juce::var SoundMapProject::toVar() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("format", "spectrocraft-sound-map");
    root->setProperty ("version", 1);
    root->setProperty ("name", name);

    auto* st = new juce::DynamicObject();
    st->setProperty ("tagFamily", "tag36h11");
    st->setProperty ("tagSizeMm", settings.tagSizeMm);
    st->setProperty ("tagCount", settings.tagCount);
    st->setProperty ("takeSeconds", settings.takeSeconds);
    st->setProperty ("countdownSeconds", settings.countdownSeconds);
    st->setProperty ("micOffsetCm", vecVar (settings.micOffsetCm));
    st->setProperty ("tagCentreHeightCm", settings.tagCentreHeightCm);
    st->setProperty ("lensFRel", settings.lensFRel);
    root->setProperty ("settings", st);

    juce::Array<juce::var> sv;
    for (const auto& s : survey)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("image", s.image);
        o->setProperty ("width", s.width);
        o->setProperty ("height", s.height);
        o->setProperty ("hold", s.hold);
        o->setProperty ("tags", obsListVar (s.tags));
        sv.add (o);
    }
    root->setProperty ("survey", sv);

    auto* m = new juce::DynamicObject();
    m->setProperty ("built", map.built);
    auto* lens = new juce::DynamicObject();
    lens->setProperty ("fRel", map.lens.fRel);
    lens->setProperty ("k1", map.lens.k1);
    lens->setProperty ("k2", map.lens.k2);
    lens->setProperty ("ppx", map.lens.ppx);
    lens->setProperty ("ppy", map.lens.ppy);
    m->setProperty ("lens", lens);
    juce::Array<juce::var> tags;
    for (const auto& [id, p] : map.tags)
    {
        auto* t = new juce::DynamicObject();
        t->setProperty ("id", id);
        t->setProperty ("pose", poseVar (p));
        tags.add (t);
    }
    m->setProperty ("tags", tags);
    m->setProperty ("rmsPx", map.rmsPx);
    m->setProperty ("picturesUsed", map.picturesUsed);
    m->setProperty ("radius", map.radius);
    m->setProperty ("warnings", map.warnings);
    m->setProperty ("builtAt", map.builtAt.toISO8601 (true));
    root->setProperty ("map", m);

    juce::Array<juce::var> pts;
    for (const auto& p : points)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("number", p.number);
        o->setProperty ("kind", (int) p.kind);
        o->setProperty ("pending", p.pending);
        o->setProperty ("wav", p.wav);
        o->setProperty ("pictureBefore", p.pictureBefore);
        o->setProperty ("pictureAfter", p.pictureAfter);
        o->setProperty ("time", p.time.toISO8601 (true));
        o->setProperty ("hold", p.hold);
        o->setProperty ("width", p.width);
        o->setProperty ("height", p.height);
        o->setProperty ("tagsBefore", obsListVar (p.tagsBefore));
        o->setProperty ("tagsAfter", obsListVar (p.tagsAfter));
        o->setProperty ("before", fixVar (p.before));
        o->setProperty ("after", fixVar (p.after));
        o->setProperty ("position", vecVar (p.position));
        o->setProperty ("located", p.located);
        o->setProperty ("sigmaM", p.sigmaM);
        o->setProperty ("movedM", p.movedM);
        if (p.input.isNotEmpty()) o->setProperty ("input", p.input);
        o->setProperty ("levels", levelsVar (p.levels));
        if (p.note.isNotEmpty()) o->setProperty ("note", p.note);
        pts.add (o);
    }
    root->setProperty ("points", pts);
    return root;
}

std::optional<SoundMapProject> SoundMapProject::fromVar (const juce::var& v, const juce::File& folder, juce::String& error)
{
    if (v["format"].toString() != "spectrocraft-sound-map")
    {
        error = "This is not a sound map.";
        return std::nullopt;
    }
    SoundMapProject p;
    p.folder = folder;
    p.name = v["name"].toString();
    if (p.name.isEmpty()) p.name = folder.getFileName();

    const auto st = v["settings"];
    p.settings.tagSizeMm = num (st, "tagSizeMm", 160.0);
    p.settings.tagCount = juce::jlimit (1, 587, (int) num (st, "tagCount", 24.0));
    p.settings.takeSeconds = num (st, "takeSeconds", 10.0);
    p.settings.countdownSeconds = num (st, "countdownSeconds", 3.0);
    p.settings.micOffsetCm = varVec (st["micOffsetCm"]);
    p.settings.tagCentreHeightCm = num (st, "tagCentreHeightCm", 0.0);
    p.settings.lensFRel = num (st, "lensFRel", 0.75);

    if (auto* a = v["survey"].getArray())
        for (const auto& e : *a)
        {
            SurveyPicture s;
            s.image = e["image"].toString();
            s.width = (int) e["width"];
            s.height = (int) e["height"];
            s.hold = (int) num (e, "hold", (double) (int) Hold::LandscapeTopLeft);
            s.tags = varObsList (e["tags"]);
            p.survey.push_back (std::move (s));
        }

    const auto m = v["map"];
    p.map.built = (bool) m["built"];
    p.map.lens.fRel = num (m["lens"], "fRel", 0.75);
    p.map.lens.k1 = num (m["lens"], "k1", 0.0);
    p.map.lens.k2 = num (m["lens"], "k2", 0.0);
    p.map.lens.ppx = num (m["lens"], "ppx", 0.0);
    p.map.lens.ppy = num (m["lens"], "ppy", 0.0);
    if (auto* a = m["tags"].getArray())
        for (const auto& t : *a) p.map.tags[(int) t["id"]] = varPose (t["pose"]);
    p.map.rmsPx = num (m, "rmsPx", 0.0);
    p.map.picturesUsed = (int) num (m, "picturesUsed", 0.0);
    p.map.radius = num (m, "radius", 0.0);
    if (auto* a = m["warnings"].getArray())
        for (const auto& w : *a) p.map.warnings.add (w.toString());
    p.map.builtAt = juce::Time::fromISO8601 (m["builtAt"].toString());
    if (p.map.built && p.map.tags.size() < 2) p.map.built = false;

    if (auto* a = v["points"].getArray())
        for (const auto& e : *a)
        {
            MapPoint q;
            q.number = (int) e["number"];
            q.kind = (PointKind) juce::jlimit (0, 2, (int) e["kind"]);
            q.pending = (bool) e["pending"];
            q.wav = e["wav"].toString();
            q.pictureBefore = e["pictureBefore"].toString();
            q.pictureAfter = e["pictureAfter"].toString();
            q.time = juce::Time::fromISO8601 (e["time"].toString());
            q.hold = (int) num (e, "hold", (double) (int) Hold::LandscapeTopLeft);
            q.width = (int) e["width"];
            q.height = (int) e["height"];
            q.tagsBefore = varObsList (e["tagsBefore"]);
            q.tagsAfter = varObsList (e["tagsAfter"]);
            q.before = varFix (e["before"]);
            q.after = varFix (e["after"]);
            q.position = varVec (e["position"]);
            q.located = (bool) e["located"];
            q.sigmaM = (double) e["sigmaM"];
            q.movedM = (double) e["movedM"];
            q.input = e["input"].toString();
            q.levels = varLevels (e["levels"]);
            q.note = e["note"].toString();
            p.points.push_back (std::move (q));
        }
    return p;
}
} // namespace scm::smap
