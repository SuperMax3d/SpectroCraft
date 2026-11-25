// SpectroCraft Mobile - sound map tests (docs/sound-map.md).
//
// The geometry is checked end to end on a synthetic 24 ft bowl: 16 tags on
// the rim, pictures rendered from cameras inside the bowl (perspective, lens
// distortion, blur, noise), detected by the real AprilTag detector, surveyed,
// then used to locate other pictures. Every truth is known, so the errors are
// measured, not guessed.
#include <JuceHeader.h>
#include "soundmap/SmInterp.h"
#include "soundmap/SmLevels.h"
#include "soundmap/SmMarkersPdf.h"
#include "soundmap/SmPose.h"
#include "soundmap/SmProject.h"
#include "soundmap/SmSurvey.h"
#include "soundmap/SmTagDetector.h"
#include "FileDoc.h"
#include <cmath>

using namespace scm::smap;

namespace
{
// ------------------------------------------------------------------ scene

struct Scene
{
    double tagSize = 0.16;
    double rimRadius = 3.658;           // 24 ft across
    double tagAboveDeck = 0.20;         // tag centres above the deck (z = 0)
    std::map<int, Pose> tags;           // truth, map frame
    Lens lens { 0.755, 0.03, 0.0, 0.004, -0.003 };   // truth: principal point 7.7 px right, 5.8 px up of the centre
    int width = 1920, height = 1080;
};

Scene makeBowl (int count = 16, double leanDeg = 15.0)
{
    Scene s;
    for (int i = 0; i < count; ++i)
    {
        const double th = 2.0 * kPi * i / count;
        const double lean = leanDeg * kPi / 180.0;
        const Vec3 pos { (s.rimRadius + 0.10) * std::cos (th), (s.rimRadius + 0.10) * std::sin (th), s.tagAboveDeck };
        // face toward the centre, top leaning toward the bowl
        const Vec3 n = Vec3 { -std::cos (th) * std::cos (lean), -std::sin (th) * std::cos (lean), -std::sin (lean) }.normalised();
        const Vec3 up { 0.0, 0.0, 1.0 };
        const Vec3 y = (up - n * up.dot (n)).normalised();
        const Vec3 x = y.cross (n);
        s.tags[i] = { Mat3::fromColumns (x, y, n), pos };
    }
    return s;
}

double bowlSurfaceZ (double r)
{
    // flat bottom 2.4 m deep, then a transition up to the coping
    return -2.4 + 2.4 * std::pow (std::clamp (r / 3.658, 0.0, 1.0), 3.0);
}

/** A camera 1.2 m above the bowl's surface at (x, y), looking toward the rim
    at angle `lookDeg` (degrees, map frame), with a slight roll. */
Pose cameraAt (double x, double y, double lookDeg, double rollDeg, double pitchDeg = 0.0)
{
    const Vec3 c { x, y, bowlSurfaceZ (std::hypot (x, y)) + 1.2 };
    const double a = lookDeg * kPi / 180.0;
    const Vec3 target { 3.7 * std::cos (a), 3.7 * std::sin (a), 0.15 + std::tan (pitchDeg * kPi / 180.0) * 3.0 };
    const Vec3 z = (target - c).normalised();
    const Vec3 up { 0.0, 0.0, 1.0 };
    Vec3 yDown = -(up - z * up.dot (z)).normalised();
    const double r = rollDeg * kPi / 180.0;
    Vec3 xRight = yDown.cross (z);
    const Vec3 xr = xRight * std::cos (r) + yDown * std::sin (r);
    yDown = z.cross (xr);
    const Mat3 R = Mat3::fromRows (xr, yDown, z);
    return { R, -(R * c) };
}

struct View
{
    int id;
    TagObservation truth;   // exact corners
};

/** The tags a camera sees well enough (in the picture, facing it, big enough). */
std::vector<View> visibleTags (const Scene& s, const Pose& camFromMap, Hold hold = Hold::LandscapeTopLeft)
{
    std::vector<View> out;
    const auto K = Intrinsics::forPicture (s.lens, s.width, s.height, hold);
    const Vec3 camCentre = camFromMap.inverse().t;
    const auto corners = tagCorners (s.tagSize);
    for (const auto& [id, T] : s.tags)
    {
        const Vec3 toCam = (camCentre - T.t).normalised();
        if (toCam.dot (T.R.col (2)) < std::cos (75.0 * kPi / 180.0)) continue;
        TagObservation o;
        o.id = id;
        bool ok = true;
        for (size_t c = 0; c < 4 && ok; ++c)
        {
            double u, v;
            ok = K.project (camFromMap * (T * corners[c]), u, v) && u > 8 && v > 8 && u < s.width - 8 && v < s.height - 8;
            o.corners[c] = { u, v };
        }
        if (ok && o.sizePx() >= 20.0) out.push_back ({ id, o });
    }
    return out;
}

/** Renders the tags (white board with a one-cell margin beyond the quiet zone)
    over a grey background, 3x3 supersampling, slight blur and noise. */
GrayImage render (const Scene& s, const Pose& camFromMap, juce::Random& rng, Hold hold = Hold::LandscapeTopLeft)
{
    GrayImage g;
    g.width = s.width; g.height = s.height;
    g.pixels.assign ((size_t) g.width * (size_t) g.height, 0);
    for (int y = 0; y < g.height; ++y)
        for (int x = 0; x < g.width; ++x)
            g.at (x, y) = (uint8_t) (110 + 40 * x / g.width + 20 * y / g.height);

    const auto K = Intrinsics::forPicture (s.lens, s.width, s.height, hold);
    const Pose mapFromCam = camFromMap.inverse();
    const Vec3 c = mapFromCam.t;
    const double cell = s.tagSize / 8.0;
    const auto views = visibleTags (s, camFromMap, hold);
    for (const auto& v : views)
    {
        const Pose& T = s.tags.at (v.id);
        const Pose tagFromMap = T.inverse();
        const auto bits = TagDetector::tagCells (v.id);
        // board corners (12 x 12 cells) -> pixel bounding box
        double u0 = 1e9, v0 = 1e9, u1 = -1e9, v1 = -1e9;
        for (double bx : { -6.0, 6.0 })
            for (double by : { -6.0, 6.0 })
            {
                double u, vv;
                if (! K.project (camFromMap * (T * Vec3 { bx * cell, by * cell, 0.0 }), u, vv)) continue;
                u0 = std::min (u0, u); u1 = std::max (u1, u); v0 = std::min (v0, vv); v1 = std::max (v1, vv);
            }
        const int px0 = std::max (0, (int) std::floor (u0) - 2), px1 = std::min (g.width - 1, (int) std::ceil (u1) + 2);
        const int py0 = std::max (0, (int) std::floor (v0) - 2), py1 = std::min (g.height - 1, (int) std::ceil (v1) + 2);
        const Vec3 o = tagFromMap * c;
        for (int py = py0; py <= py1; ++py)
            for (int px = px0; px <= px1; ++px)
            {
                double sum = 0.0;
                int hits = 0;
                for (int sy = 0; sy < 3; ++sy)
                    for (int sx = 0; sx < 3; ++sx)
                    {
                        double nx, ny;
                        K.normalise (px + (sx + 0.5) / 3.0 - 0.5, py + (sy + 0.5) / 3.0 - 0.5, nx, ny);
                        const Vec3 d = tagFromMap.R * (mapFromCam.R * Vec3 { nx, ny, 1.0 });
                        if (std::abs (d.z) < 1e-12) continue;
                        const double t = -o.z / d.z;
                        if (t <= 0.0) continue;
                        const double X = o.x + t * d.x, Y = o.y + t * d.y;
                        const double cx = X / cell, cy = Y / cell;
                        if (std::abs (cx) > 6.0 || std::abs (cy) > 6.0) continue;
                        ++hits;
                        bool black = false;
                        if (std::abs (cx) < 5.0 && std::abs (cy) < 5.0)
                        {
                            const int col = juce::jlimit (0, 9, (int) std::floor (cx + 5.0));
                            const int row = juce::jlimit (0, 9, (int) std::floor (5.0 - cy));
                            black = bits[(size_t) (row * 10 + col)];
                        }
                        sum += black ? 25.0 : 230.0;
                    }
                if (hits == 0) continue;
                const double bg = g.at (px, py);
                g.at (px, py) = (uint8_t) juce::jlimit (0.0, 255.0, (sum + bg * (9 - hits)) / 9.0);
            }
    }
    // blur (binomial 3x3) and noise
    GrayImage b = g;
    for (int y = 1; y < g.height - 1; ++y)
        for (int x = 1; x < g.width - 1; ++x)
        {
            int acc = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    acc += g.at (x + dx, y + dy) * (dx == 0 ? 2 : 1) * (dy == 0 ? 2 : 1);
            const int n = rng.nextInt (7) - 3;
            b.at (x, y) = (uint8_t) juce::jlimit (0, 255, acc / 16 + n);
        }
    return b;
}

double gauss (juce::Random& r)
{
    // Box-Muller
    const double u1 = std::max (1e-12, r.nextDouble()), u2 = r.nextDouble();
    return std::sqrt (-2.0 * std::log (u1)) * std::cos (2.0 * kPi * u2);
}

/** Survey cameras: a turn at the centre, then looks across from near the walls. */
std::vector<Pose> surveyCameras()
{
    std::vector<Pose> cams;
    for (int k = 0; k < 12; ++k) cams.push_back (cameraAt (0.1, -0.1, k * 30.0, 2.0 * std::sin (k)));
    for (int k = 0; k < 16; ++k)
    {
        const double a = k * 22.5 + 8.0;
        const double r = 2.4 + 0.4 * std::sin (k * 1.3);
        const double x = r * std::cos (a * kPi / 180.0), y = r * std::sin (a * kPi / 180.0);
        cams.push_back (cameraAt (x, y, a + 180.0 + 25.0 * std::sin (k * 0.7), 3.0 * std::cos (k), 8.0));
    }
    return cams;
}

/** Survey pictures are mostly taken with the phone's top to the left, every
    third one the other way round (the principal point then moves). */
Hold surveyHold (size_t i)
{
    return i % 3 == 2 ? Hold::LandscapeTopRight : Hold::LandscapeTopLeft;
}

/** Measurement cameras spread over the bowl. */
std::vector<Pose> measureCameras()
{
    std::vector<Pose> cams;
    for (int k = 0; k < 20; ++k)
    {
        const double r = 0.3 + 2.8 * std::fmod (k * 0.618, 1.0);
        const double a = k * 137.5;
        const double x = r * std::cos (a * kPi / 180.0), y = r * std::sin (a * kPi / 180.0);
        cams.push_back (cameraAt (x, y, a + 180.0 + 20.0 * std::cos (k), 4.0 * std::sin (k * 2.1), 5.0));
    }
    return cams;
}

double tagCentreRms (const std::map<int, Pose>& a, const std::map<int, Pose>& b, int& compared)
{
    double s = 0.0;
    compared = 0;
    for (const auto& [id, p] : a)
        if (const auto it = b.find (id); it != b.end()) { s += (p.t - it->second.t).dot (p.t - it->second.t); ++compared; }
    return compared > 0 ? std::sqrt (s / compared) : 1e9;
}

struct RigidFit
{
    double rms = 1e9, angleDeg = 0.0, shift = 0.0;
};

/** Best rigid transform taking a's tag centres onto b's (Kabsch): the
    residual is the map's shape error, the transform its frame error. */
RigidFit rigidFit (const std::map<int, Pose>& a, const std::map<int, Pose>& b)
{
    std::vector<Vec3> P, Q;
    for (const auto& [id, p] : a)
        if (const auto it = b.find (id); it != b.end()) { P.push_back (p.t); Q.push_back (it->second.t); }
    RigidFit f;
    if (P.size() < 3) return f;
    Vec3 mp, mq;
    for (size_t i = 0; i < P.size(); ++i) { mp += P[i]; mq += Q[i]; }
    mp = mp / (double) P.size(); mq = mq / (double) Q.size();
    // rotation from the cross-covariance: nearestRotation of sum q p^T
    Mat3 H = Mat3::zero();
    for (size_t i = 0; i < P.size(); ++i)
    {
        const Vec3 p = P[i] - mp, q = Q[i] - mq;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) H (r, c) += q[r] * p[c];
    }
    const Mat3 R = nearestRotation (H);
    const Vec3 t = mq - R * mp;
    double s = 0.0;
    for (size_t i = 0; i < P.size(); ++i) { const Vec3 d = R * P[i] + t - Q[i]; s += d.dot (d); }
    f.rms = std::sqrt (s / (double) P.size());
    f.angleDeg = logRotation (R).norm() * 180.0 / kPi;
    f.shift = (t + R * mp - mp).norm();   // how far the tags' centroid moves
    return f;
}

void writeSine (const juce::File& f, double amplitude, double hz, double seconds, double rate,
                ScaleCalibration::Unit unit = ScaleCalibration::Unit::None, double fullScale = 0.0)
{
    FileDoc d;
    std::vector<std::vector<float>> data (1, std::vector<float> ((size_t) (seconds * rate), 0.0f));
    for (size_t i = 0; i < data[0].size(); ++i)
        data[0][i] = (float) (amplitude * std::sin (2.0 * kPi * hz * (double) i / rate));
    d.engine.initNew (1, rate, &data);
    d.scale.unit = unit;
    d.scale.fullScaleValue = fullScale;
    d.engine.saveFileAs (f);
}
} // namespace

// ======================================================================

class SoundMapMathTests : public juce::UnitTest
{
public:
    SoundMapMathTests() : juce::UnitTest ("Sound map: geometry", "SpectroCraftMobile") {}

    void runTest() override
    {
        juce::Random rng (73);

        beginTest ("rotation exp / log round trip, poses compose and invert");
        {
            for (int i = 0; i < 50; ++i)
            {
                Vec3 w { rng.nextDouble() - 0.5, rng.nextDouble() - 0.5, rng.nextDouble() - 0.5 };
                w = w.normalised() * (rng.nextDouble() * 3.1);
                const Mat3 R = expRotation (w);
                expectWithinAbsoluteError (R.determinant(), 1.0, 1e-12);
                expectWithinAbsoluteError ((logRotation (R) - w).norm(), 0.0, 1e-8);
            }
            const Pose a { expRotation ({ 0.3, -0.2, 0.9 }), { 1.0, 2.0, 3.0 } };
            const Pose b = a * a.inverse();
            expectWithinAbsoluteError (rotationAngle (b.R, Mat3::identity()), 0.0, 1e-12);
            expectWithinAbsoluteError (b.t.norm(), 0.0, 1e-12);
            const Mat3 noisy = expRotation ({ 0.2, 0.1, -0.4 }) * 1.07;
            expectWithinAbsoluteError (rotationAngle (nearestRotation (noisy), expRotation ({ 0.2, 0.1, -0.4 })), 0.0, 1e-9);
            // singular (rank 2, as points in a plane give) and reflected inputs still give the rotation
            Mat3 flat = expRotation ({ -0.7, 0.3, 1.2 });
            for (int r = 0; r < 3; ++r) flat (r, 2) = 0.0;
            const Mat3 nr = nearestRotation (flat);
            expectWithinAbsoluteError (nr.determinant(), 1.0, 1e-9);
            expectWithinAbsoluteError (rotationAngle (nr, expRotation ({ -0.7, 0.3, 1.2 })), 0.0, 1e-9);
            Mat3 mirrored = expRotation ({ 0.5, 0.5, 0.0 });
            for (int r = 0; r < 3; ++r) mirrored (r, 0) = -mirrored (r, 0);
            expectWithinAbsoluteError (nearestRotation (mirrored).determinant(), 1.0, 1e-9);
        }

        beginTest ("symmetric eigen and Cholesky");
        {
            const std::vector<double> A { 4, 1, 0, 1, 3, 1, 0, 1, 2 };
            const auto e = symmetricEigen (A, 3);
            for (int k = 0; k < 3; ++k)
                for (int r = 0; r < 3; ++r)
                {
                    double av = 0.0;
                    for (int c = 0; c < 3; ++c) av += A[(size_t) (r * 3 + c)] * e.vectors[(size_t) k][(size_t) c];
                    expectWithinAbsoluteError (av, e.values[(size_t) k] * e.vectors[(size_t) k][(size_t) r], 1e-10);
                }
            std::vector<double> x;
            expect (choleskySolve (A, x, { 1, 2, 3 }, 3));
            for (int r = 0; r < 3; ++r)
            {
                double s = 0.0;
                for (int c = 0; c < 3; ++c) s += A[(size_t) (r * 3 + c)] * x[(size_t) c];
                expectWithinAbsoluteError (s, (double) (r + 1), 1e-12);
            }
        }

        beginTest ("camera model: distortion round trip; phone holds are rotations");
        {
            const auto K = Intrinsics::forPicture ({ 0.75, 0.05, -0.01 }, 1920, 1080);
            for (double u : { 10.0, 500.0, 960.0, 1900.0 })
                for (double v : { 5.0, 540.0, 1070.0 })
                {
                    double x, y, uu, vv;
                    K.normalise (u, v, x, y);
                    expect (K.project ({ x * 2.0, y * 2.0, 2.0 }, uu, vv));
                    expectWithinAbsoluteError (uu, u, 1e-6);
                    expectWithinAbsoluteError (vv, v, 1e-6);
                }
            for (int h = 0; h < 4; ++h)
            {
                const Mat3 M = Mat3::fromColumns (bodyToCamera ({ 1, 0, 0 }, (Hold) h), bodyToCamera ({ 0, 1, 0 }, (Hold) h),
                                                  bodyToCamera ({ 0, 0, 1 }, (Hold) h));
                expectWithinAbsoluteError (M.determinant(), 1.0, 1e-12, "hold " + juce::String (h));
            }
            // portrait: the phone's top is up in the picture (camera y is down)
            expect (bodyToCamera ({ 0, 1, 0 }, Hold::Portrait).y < 0.0);
            // landscape, top to the left: the phone's right side (body +x) is up in the picture
            expect (bodyToCamera ({ 1, 0, 0 }, Hold::LandscapeTopLeft).y < 0.0);
            expect (bodyToCamera ({ 1, 0, 0 }, Hold::LandscapeTopRight).y > 0.0);
            // the back camera looks away from the screen
            expect (bodyToCamera ({ 0, 0, -1 }, Hold::Portrait).z > 0.0);
        }

        beginTest ("homography and single-tag pose: the true pose is a candidate");
        {
            const Scene s = makeBowl();
            const auto K = Intrinsics::forPicture (s.lens, s.width, s.height);
            int checked = 0;
            for (const auto& cam : surveyCameras())
                for (const auto& v : visibleTags (s, cam))
                {
                    const auto cands = tagPoseCandidates (v.truth, s.tagSize, K);
                    expect (! cands.empty());
                    const Pose truth = cam * s.tags.at (v.id);
                    double bestT = 1e9, bestR = 1e9;
                    for (const auto& c : cands)
                    {
                        bestT = std::min (bestT, (c.camFromTag.t - truth.t).norm());
                        bestR = std::min (bestR, rotationAngle (c.camFromTag.R, truth.R));
                    }
                    expectWithinAbsoluteError (bestT, 0.0, 1e-6, "tag " + juce::String (v.id));
                    expectWithinAbsoluteError (bestR, 0.0, 1e-6);
                    ++checked;
                }
            expect (checked > 60, "views checked: " + juce::String (checked));
        }

        beginTest ("resection from the true map: centimetre with 0.3 px noise");
        {
            const Scene s = makeBowl();
            const auto K = Intrinsics::forPicture (s.lens, s.width, s.height);
            std::vector<double> errs;
            int fewTags = 0, withinSigma = 0;
            for (const auto& cam : measureCameras())
            {
                auto views = visibleTags (s, cam);
                std::vector<TagObservation> obs;
                for (auto& v : views)
                {
                    for (auto& c : v.truth.corners) { c.u += 0.3 * gauss (rng); c.v += 0.3 * gauss (rng); }
                    obs.push_back (v.truth);
                }
                if (obs.size() < 2) { ++fewTags; continue; }
                const auto r = resect (obs, s.tags, s.tagSize, K);
                expect (r.ok, r.problem);
                const double e = (r.cameraCentre() - cam.inverse().t).norm();
                errs.push_back (e);
                if (e <= 2.5 * r.positionSigma) ++withinSigma;
                if (juce::SystemStats::getEnvironmentVariable ("SC_SOUNDMAP_DEBUG", {}).isNotEmpty())
                {
                    const Vec3 c = cam.inverse().t;
                    logMessage ("cam at " + juce::String (c.x, 2) + "," + juce::String (c.y, 2) + "," + juce::String (c.z, 2)
                                + " tags " + juce::String (r.tagsUsed) + " err " + juce::String (e * 1000.0, 1) + " mm sigma "
                                + juce::String (r.positionSigma * 1000.0, 1) + " mm rms " + juce::String (r.rmsPx, 2)
                                + " rejected " + juce::String ((int) r.rejectedTags.size()));
                }
                expect (r.positionSigma > 0.0 && r.positionSigma < 0.05, "sigma " + juce::String (r.positionSigma));
            }
            // 16 tags, 1080p, 0.3 px: about 1 cm (the precision study gives the table)
            expect (fewTags <= 2, "cameras seeing fewer than 2 tags: " + juce::String (fewTags));
            expect (median (errs) < 0.02, "median error " + juce::String (median (errs) * 1000.0, 1) + " mm");
            expect (*std::max_element (errs.begin(), errs.end()) < 0.06,
                    "max error " + juce::String (*std::max_element (errs.begin(), errs.end()) * 1000.0, 1) + " mm");
            expect (withinSigma >= (int) errs.size() * 8 / 10,
                    "errors within 2.5 sigma: " + juce::String (withinSigma) + " of " + juce::String ((int) errs.size()));
            logMessage ("resection: median " + juce::String (median (errs) * 1000.0, 1) + " mm over "
                        + juce::String ((int) errs.size()) + " cameras");
        }

        beginTest ("one tag is not a position; the reported uncertainty matches the errors");
        {
            const Scene s = makeBowl();
            const auto K = Intrinsics::forPicture (s.lens, s.width, s.height);
            const Pose cam = cameraAt (0.0, 0.0, 0.0, 0.0);
            auto views = visibleTags (s, cam);
            expect (! views.empty());
            const auto one = resect ({ views.front().truth }, s.tags, s.tagSize, K);
            expect (! one.ok && one.tagsUsed == 1, "one tag refused: " + juce::String (one.problem));

            // RMS of the camera-centre error vs the reported sigma (should agree)
            juce::Random r2 (99);
            double errSq = 0.0, sigSq = 0.0;
            int n = 0;
            for (const auto& c : measureCameras())
            {
                std::vector<TagObservation> obs;
                for (auto v : visibleTags (s, c))
                {
                    for (auto& k : v.truth.corners) { k.u += 0.3 * gauss (r2); k.v += 0.3 * gauss (r2); }
                    obs.push_back (v.truth);
                }
                const auto res = resect (obs, s.tags, s.tagSize, K);
                if (! res.ok) continue;
                const double e = (res.cameraCentre() - c.inverse().t).norm();
                errSq += e * e;
                sigSq += res.positionSigma * res.positionSigma;
                ++n;
            }
            const double ratio = std::sqrt (errSq / std::max (1, n)) / std::sqrt (sigSq / std::max (1, n));
            expect (ratio > 0.6 && ratio < 1.6, "RMS error / RMS sigma = " + juce::String (ratio, 2));
            logMessage ("resection: RMS error / RMS reported sigma = " + juce::String (ratio, 2) + " over " + juce::String (n) + " cameras");
        }

        beginTest ("a moved tag is dropped by the resection");
        {
            const Scene s = makeBowl();
            const auto K = Intrinsics::forPicture (s.lens, s.width, s.height);
            auto moved = s.tags;
            const Pose cam = cameraAt (-1.5, 0.3, 0.0, 0.0);   // looks at tags 15, 0, 1 and neighbours
            const auto views = visibleTags (s, cam);
            expect (views.size() >= 3, "tags in view: " + juce::String ((int) views.size()));
            moved[views[1].id].t += Vec3 { 0.0, 0.0, 0.25 };   // the map thinks it is 25 cm higher
            std::vector<TagObservation> obs;
            for (const auto& v : views) obs.push_back (v.truth);
            const auto r = resect (obs, moved, s.tagSize, K);
            expect (r.ok, r.problem);
            expect (std::find (r.rejectedTags.begin(), r.rejectedTags.end(), views[1].id) != r.rejectedTags.end(),
                    "the moved tag is rejected");
            expectWithinAbsoluteError ((r.cameraCentre() - cam.inverse().t).norm(), 0.0, 0.005);
        }

        beginTest ("bowl frame: centre of the circle, z up, x toward tag 0");
        {
            const Scene s = makeBowl();
            // the same tags in an arbitrary frame
            const Pose arb { expRotation ({ 0.4, -1.1, 0.7 }), { 5.0, -2.0, 1.0 } };
            std::map<int, Pose> inArb;
            for (const auto& [id, p] : s.tags) inArb[id] = arb * p;
            const auto f = bowlFrame (inArb, s.tagAboveDeck);
            expect (f.circle);
            expectWithinAbsoluteError (f.radius, s.rimRadius + 0.10, 1e-6);
            for (const auto& [id, p] : s.tags)
                expectWithinAbsoluteError (((f.mapFromRef * inArb[id]).t - p.t).norm(), 0.0, 1e-9);
        }
    }
};

class SoundMapSurveyTests : public juce::UnitTest
{
public:
    SoundMapSurveyTests() : juce::UnitTest ("Sound map: survey", "SpectroCraftMobile") {}

    void runTest() override
    {
        juce::Random rng (2401);

        beginTest ("survey from noisy corners: tags to millimetres, focal length found from a 7 % wrong start");
        {
            const Scene s = makeBowl();
            std::vector<SurveyPhoto> photos;
            const auto cams = surveyCameras();
            for (size_t i = 0; i < cams.size(); ++i)
            {
                const auto& cam = cams[i];
                SurveyPhoto p;
                p.width = s.width; p.height = s.height;
                p.hold = surveyHold (i);
                for (auto v : visibleTags (s, cam, p.hold))
                {
                    for (auto& c : v.truth.corners) { c.u += 0.3 * gauss (rng); c.v += 0.3 * gauss (rng); }
                    p.tags.push_back (v.truth);
                }
                photos.push_back (p);
            }
            SurveySettings st;
            st.tagSize = s.tagSize;
            st.lens.fRel = 0.70;
            st.tagHeightAboveDeck = s.tagAboveDeck;
            const auto r = solveSurvey (photos, st);
            expect (r.ok, r.problem);
            expectEquals ((int) r.tags.size(), 16);
            int n = 0;
            const double rms = tagCentreRms (r.tags, s.tags, n);
            const auto fit = rigidFit (r.tags, s.tags);
            expect (fit.rms < 0.01, "tag centres after a rigid fit: RMS " + juce::String (fit.rms * 1000.0, 2) + " mm");
            expect (fit.angleDeg < 0.3 && fit.shift < 0.01,
                    "map frame vs truth: " + juce::String (fit.angleDeg, 3) + " deg, " + juce::String (fit.shift * 1000.0, 1) + " mm");
            expect (rms < 0.015, "tag centres RMS in the map frame " + juce::String (rms * 1000.0, 2) + " mm");
            expectWithinAbsoluteError (r.lens.fRel, s.lens.fRel, 0.006);
            expectWithinAbsoluteError (r.lens.k1, s.lens.k1, 0.02);
            expectWithinAbsoluteError (r.lens.ppx, s.lens.ppx, 0.002);
            expectWithinAbsoluteError (r.lens.ppy, s.lens.ppy, 0.002);
            expect (r.rmsPx < 0.5, "reprojection " + juce::String (r.rmsPx, 2) + " px");
            expect (r.frame.circle);
            expectWithinAbsoluteError (r.frame.radius, s.rimRadius + 0.10, 0.01);
            logMessage ("survey (corners): tags " + juce::String (rms * 1000.0, 2) + " mm RMS (shape "
                        + juce::String (fit.rms * 1000.0, 2) + " mm, frame " + juce::String (fit.angleDeg, 3) + " deg / "
                        + juce::String (fit.shift * 1000.0, 1) + " mm), f " + juce::String (r.lens.fRel, 4)
                        + ", k1 " + juce::String (r.lens.k1, 4) + ", pp " + juce::String (r.lens.ppx * 1920.0, 1) + " / "
                        + juce::String (r.lens.ppy * 1920.0, 1) + " px (truth 7.7 / -5.8), " + juce::String (r.rmsPx, 2) + " px");
        }

        beginTest ("too little to work with is refused with a reason");
        {
            const auto r0 = solveSurvey ({}, {});
            expect (! r0.ok && ! r0.problem.empty());
            SurveyPhoto lone;
            lone.width = 1920; lone.height = 1080;
            TagObservation o;
            o.id = 3;
            o.corners = { Pixel { 100, 200 }, Pixel { 200, 200 }, Pixel { 200, 100 }, Pixel { 100, 100 } };
            lone.tags = { o };
            const auto r1 = solveSurvey ({ lone, lone }, {});
            expect (! r1.ok && ! r1.problem.empty());
        }
    }
};

class SoundMapImageTests : public juce::UnitTest
{
public:
    SoundMapImageTests() : juce::UnitTest ("Sound map: rendered pictures end to end", "SpectroCraftMobile") {}

    void runTest() override
    {
        juce::Random rng (24);
        const Scene s = makeBowl();
        TagDetector detector (4);

        beginTest ("detector: ids and corners (order as printed) on rendered pictures");
        std::vector<SurveyPhoto> photos;
        {
            std::vector<double> cornerErr;
            double meanDu = 0.0, meanDv = 0.0;
            int nCorners = 0;
            int expected = 0, found = 0, wrong = 0;
            const auto cams = surveyCameras();
            for (size_t i = 0; i < cams.size(); ++i)
            {
                const auto& cam = cams[i];
                const auto hold = surveyHold (i);
                const auto img = render (s, cam, rng, hold);
                const auto obs = detector.detect (img, 15);
                const auto truth = visibleTags (s, cam, hold);
                expected += (int) truth.size();
                for (const auto& o : obs)
                {
                    const auto it = std::find_if (truth.begin(), truth.end(), [&] (const View& v) { return v.id == o.id; });
                    if (it == truth.end()) { ++wrong; continue; }
                    ++found;
                    for (size_t c = 0; c < 4; ++c)
                    {
                        const double du = o.corners[c].u - it->truth.corners[c].u, dv = o.corners[c].v - it->truth.corners[c].v;
                        cornerErr.push_back (std::hypot (du, dv));
                        meanDu += du; meanDv += dv; ++nCorners;
                    }
                }
                SurveyPhoto p;
                p.width = img.width; p.height = img.height;
                p.hold = hold;
                p.tags = obs;
                photos.push_back (p);
            }
            expectEquals (wrong, 0, "no tag that is not in the picture");
            expect (found >= expected * 9 / 10, "found " + juce::String (found) + " of " + juce::String (expected));
            const double med = median (cornerErr);
            expect (med < 0.25, "median corner error " + juce::String (med, 2) + " px");
            logMessage ("detector: " + juce::String (found) + "/" + juce::String (expected) + " tags, median corner error "
                        + juce::String (med, 3) + " px, mean offset " + juce::String (meanDu / std::max (1, nCorners), 3) + ", "
                        + juce::String (meanDv / std::max (1, nCorners), 3) + " px");
        }

        beginTest ("survey on detected tags, then the phone located from new pictures");
        {
            SurveySettings st;
            st.tagSize = s.tagSize;
            st.lens.fRel = 0.75;
            st.tagHeightAboveDeck = s.tagAboveDeck;
            const auto r = solveSurvey (photos, st);
            expect (r.ok, r.problem);
            int n = 0;
            const double rms = tagCentreRms (r.tags, s.tags, n);
            expectEquals (n, 16);
            const auto fit = rigidFit (r.tags, s.tags);
            expect (fit.rms < 0.004, "tag centres after a rigid fit: RMS " + juce::String (fit.rms * 1000.0, 2) + " mm");
            expect (rms < 0.015, "tag centres RMS " + juce::String (rms * 1000.0, 2) + " mm");
            expectWithinAbsoluteError (r.lens.fRel, s.lens.fRel, 0.01);
            for (const auto& w : r.warnings) logMessage ("survey warning: " + juce::String (w));

            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc-soundmap-test-e2e");
            dir.deleteRecursively();
            dir.createDirectory();
            SoundMapProject project;
            project.folder = dir;
            project.settings.tagSizeMm = s.tagSize * 1000.0;
            project.settings.micOffsetCm = { 2.0, -13.0, -0.5 };   // the bottom microphone, portrait
            project.setMap (r);

            std::vector<double> errs;
            int refused = 0;
            const auto mcams = measureCameras();
            for (size_t i = 0; i < mcams.size(); ++i)
            {
                const auto& cam = mcams[i];
                const auto hold = i % 2 == 0 ? Hold::LandscapeTopLeft : Hold::LandscapeTopRight;
                const auto img = render (s, cam, rng, hold);
                const auto obs = detector.detect (img, 15);
                const auto fix = project.locate (obs, img.width, img.height, (int) hold);
                if (! fix.ok) { ++refused; continue; }
                const Vec3 micTruth = cam.inverse() * bodyToCamera (project.settings.micOffsetCm / 100.0, hold);
                errs.push_back ((fix.mic - micTruth).norm());
            }
            expect (refused <= 2, "pictures refused: " + juce::String (refused));
            expect (! errs.empty());
            if (! errs.empty())
            {
                const double med = median (errs), worst = *std::max_element (errs.begin(), errs.end());
                expect (med < 0.015, "median microphone error " + juce::String (med * 1000.0, 1) + " mm");
                expect (worst < 0.05, "worst microphone error " + juce::String (worst * 1000.0, 1) + " mm");
                logMessage ("end to end: tags " + juce::String (rms * 1000.0, 1) + " mm RMS (shape " + juce::String (fit.rms * 1000.0, 1)
                            + " mm); microphone median "
                            + juce::String (med * 1000.0, 1) + " mm, worst " + juce::String (worst * 1000.0, 1) + " mm over "
                            + juce::String ((int) errs.size()) + " points");
            }
            dir.deleteRecursively();
        }
    }
};

class SoundMapDataTests : public juce::UnitTest
{
public:
    SoundMapDataTests() : juce::UnitTest ("Sound map: levels, map, files", "SpectroCraftMobile") {}

    void runTest() override
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc-soundmap-test-data");
        dir.deleteRecursively();
        dir.createDirectory();

        beginTest ("A weighting");
        {
            expectWithinAbsoluteError (aWeightingDb (1000.0), 0.0, 0.01);
            expectWithinAbsoluteError (aWeightingDb (100.0), -19.1, 0.1);
            expectWithinAbsoluteError (aWeightingDb (63.0), -26.2, 0.1);
            expectWithinAbsoluteError (aWeightingDb (4000.0), 1.0, 0.1);
            expectEquals (bandLabel (1000.0), juce::String ("1k"));
            expectEquals (bandLabel (1250.0), juce::String ("1.25k"));
            expectEquals (bandLabel (63.0), juce::String ("63"));
            expectEquals (nominalFor (1258.9), 1250.0);
            expectEquals (nominalFor (50.12), 50.0);
        }

        beginTest ("levels of a take: a 1 kHz tone, uncalibrated and in pascal");
        {
            const auto a = dir.getChildFile ("tone.wav");
            writeSine (a, 0.1, 1000.0, 4.0, 48000.0);
            juce::String err;
            const auto l = analyseTake (a, 0.25, 0.25, err);
            expect (l.valid(), err);
            const double rmsDb = 20.0 * std::log10 (0.1 / std::sqrt (2.0));
            expectEquals (l.unit, juce::String ("dBFS"));
            expectWithinAbsoluteError (l.lzeq, rmsDb, 0.05);
            const int b = l.bandFor (1000.0);
            expect (b >= 0);
            if (b >= 0) expectWithinAbsoluteError (l.db[(size_t) b], rmsDb, 0.5);
            expectWithinAbsoluteError (l.laeq, rmsDb, 0.5);
            expectWithinAbsoluteError (l.seconds, 3.5, 1e-6);

            const auto c = dir.getChildFile ("tone-pa.wav");
            writeSine (c, 0.1, 1000.0, 4.0, 48000.0, ScaleCalibration::Unit::Pascal, 20.0);
            const auto lc = analyseTake (c, 0.25, 0.25, err);
            expect (lc.valid() && lc.calibrated, err);
            const double spl = 20.0 * std::log10 (0.1 / std::sqrt (2.0) * 20.0 / 2e-5);
            expectEquals (lc.unit, juce::String ("dB-SPL"));
            expectWithinAbsoluteError (lc.lzeq, spl, 0.05);
            const int bc = lc.bandFor (1000.0);
            expect (bc >= 0);
            if (bc >= 0) expectWithinAbsoluteError (lc.db[(size_t) bc], spl, 0.5);
            logMessage ("1 kHz band: " + juce::String (b >= 0 ? l.db[(size_t) b] : 0.0, 2) + " dBFS (tone "
                        + juce::String (rmsDb, 2) + "), " + juce::String (bc >= 0 ? lc.db[(size_t) bc] : 0.0, 2)
                        + " dB-SPL (tone " + juce::String (spl, 2) + ")");
        }

        beginTest ("a take too short for one period, or silent, gives no levels");
        {
            const auto shortTake = dir.getChildFile ("short.wav");
            writeSine (shortTake, 0.1, 1000.0, 2.0, 48000.0);
            juce::String err;
            expect (! analyseTake (shortTake, 0.5, 0.3, err).valid() && err.contains ("too short"), err);
            const auto silent = dir.getChildFile ("silent.wav");
            writeSine (silent, 0.0, 1000.0, 4.0, 48000.0);
            err = {};
            expect (! analyseTake (silent, 0.5, 0.3, err).valid() && err.contains ("silent"), err);
        }

        beginTest ("map checks: a point without a position after its take is not trusted; units do not mix");
        {
            SoundMapProject p;
            MapPoint a;
            a.number = 1; a.pending = false;
            a.before.ok = true; a.before.sigmaM = 0.005; a.before.mic = { 1, 0, -2 };
            a.updatePosition();
            expect (a.located && ! a.checked() && ! a.good(), "no after fix: located but not checked");
            a.after = a.before;
            a.updatePosition();
            expect (a.good());
            a.after.mic = { 1.08, 0, -2 };
            a.updatePosition();
            expect (a.moved() && ! a.good(), "moved 8 cm");

            auto levelPoint = [] (int n, PointKind k, const char* unit, double db, int minute)
            {
                MapPoint q;
                q.number = n; q.kind = k; q.pending = false;
                q.time = juce::Time (2026, 10, 25, 12, minute);
                q.levels.unit = unit;
                q.levels.centresHz = { 1000.0 };
                q.levels.db = { db };
                return q;
            };
            p.points = { levelPoint (1, PointKind::Point, "dB-SPL", 80.0, 0), levelPoint (2, PointKind::Point, "dB-SPL", 82.0, 1),
                         levelPoint (3, PointKind::Point, "dBFS", -40.0, 2), levelPoint (4, PointKind::Background, "dBFS", -70.0, 1),
                         levelPoint (5, PointKind::Background, "dB-SPL", 50.0, 5) };
            expectEquals (p.mapUnit(), juce::String ("dB-SPL"));
            expectWithinAbsoluteError (p.snrDb (p.points[0], 1000.0), 30.0, 1e-9, "background in the same unit, though later");
            expectWithinAbsoluteError (p.snrDb (p.points[2], 1000.0), 30.0, 1e-9);
        }

        beginTest ("periodic pink source: equal band levels, the same in every 10 s window");
        {
            const auto f = dir.getChildFile ("pink.wav");
            expect (writePeriodicPink (f, 48000.0, 30.0));
            juce::String err;
            const auto a = analyseTake (f, 1.0, 19.0, err);    // seconds 1 .. 11
            const auto b = analyseTake (f, 13.0, 7.0, err);    // seconds 13 .. 23
            expect (a.valid() && b.valid(), err);
            double lo = 1e9, hi = -1e9, worstDiff = 0.0;
            for (double band : { 50.0, 63.0, 100.0, 250.0, 1000.0, 4000.0, 8000.0, 16000.0 })
            {
                const int ia = a.bandFor (band), ib = b.bandFor (band);
                expect (ia >= 0 && ib >= 0, "band " + juce::String (band));
                if (ia < 0 || ib < 0) continue;
                lo = std::min (lo, a.db[(size_t) ia]);
                hi = std::max (hi, a.db[(size_t) ia]);
                worstDiff = std::max (worstDiff, std::abs (a.db[(size_t) ia] - b.db[(size_t) ib]));
                logMessage ("  pink " + bandLabel (band) + ": " + juce::String (a.db[(size_t) ia], 2) + " / " + juce::String (b.db[(size_t) ib], 2));
            }
            expect (hi - lo < 1.5, "pink: band levels within " + juce::String (hi - lo, 2) + " dB");
            expect (worstDiff < 0.15, "two windows differ by " + juce::String (worstDiff, 3) + " dB");
            logMessage ("periodic pink: bands 50 Hz-16 kHz within " + juce::String (hi - lo, 2) + " dB; two 10 s windows within "
                        + juce::String (worstDiff, 3) + " dB");
        }

        beginTest ("interpolation: values at the points, nothing far from them");
        {
            std::vector<std::array<double, 2>> xy { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
            std::vector<double> v { 70, 80, 75, 85 };
            expectWithinAbsoluteError (typicalSpacing (xy), 1.0, 1e-12);
            scm::smap::Grid g;
            g.x0 = -2.0; g.y0 = -2.0; g.cell = 0.05; g.nx = 101; g.ny = 101;
            const auto widths = adaptiveWidths (xy);
            expectWithinAbsoluteError (widths[0], std::sqrt (2.0), 1e-9, "third nearest neighbour");
            interpolate (g, xy, v, std::vector<double> (4, 0.3), 2.5);
            const auto at = [&g] (double x, double y) { return (size_t) (std::lround ((y - g.y0) / g.cell) * g.nx + std::lround ((x - g.x0) / g.cell)); };
            expect (g.valid[at (0, 0)] && g.valid[at (1, 1)]);
            expectWithinAbsoluteError ((double) g.values[at (0, 0)], 70.0, 2.0);
            expectWithinAbsoluteError ((double) g.values[at (1, 1)], 85.0, 2.0);
            expect (! g.valid[at (-1.5, -1.5)], "no value 2 m from every point");
            const double mid = g.values[at (0.5, 0.5)];
            expect (g.valid[at (0.5, 0.5)] && mid > 70.0 && mid < 85.0);
            // the middle is an energy mean: closer to the loud points than the dB mean (77.5)
            expectWithinAbsoluteError (mid, 10.0 * std::log10 ((1e7 + 1e8 + std::pow (10.0, 7.5) + std::pow (10.0, 8.5)) / 4.0), 0.05);
            const double mean = gridEnergyMeanDb (g);
            expect (mean > 70.0 && mean < 85.0);
        }

        beginTest ("project: JSON round trip, CSV, locate refuses without a map");
        {
            SoundMapProject p;
            p.folder = dir.getChildFile ("map");
            p.folder.createDirectory();
            p.name = "map";
            p.settings.tagSizeMm = 200.0;
            p.settings.micOffsetCm = { 1, -12, 0 };
            SurveyPicture sp;
            sp.image = "survey/S001.jpg";
            sp.width = 1920; sp.height = 1080;
            TagObservation o;
            o.id = 4;
            o.corners = { Pixel { 1, 2 }, Pixel { 3, 4 }, Pixel { 5, 6 }, Pixel { 7, 8 } };
            o.margin = 55.5;
            sp.tags = { o };
            p.survey = { sp };
            expect (! p.locate ({ o }, 1920, 1080, 0).ok, "no map yet");
            p.map.built = true;
            p.map.lens = { 0.76, 0.02, 0.0 };
            p.map.tags[0] = Pose { expRotation ({ 0.1, 0.2, 0.3 }), { 1, 2, 3 } };
            p.map.tags[1] = Pose { Mat3::identity(), { -1, 0, 0.5 } };
            p.map.rmsPx = 0.31;
            MapPoint q;
            q.number = 7;
            q.wav = "points/P007.wav";
            q.position = { 0.5, -1.25, -2.0 };
            q.located = true;
            q.pending = false;
            q.sigmaM = 0.004;
            q.movedM = 0.012;
            q.tagsBefore = { o };
            q.before.ok = true;
            q.before.tags = 4;
            q.levels.centresHz = { 1000.0, 1258.9 };
            q.levels.db = { 81.25, 79.5 };
            q.levels.unit = "dB-SPL";
            q.levels.lzeq = 88.0;
            q.levels.laeq = 86.5;
            p.points = { q };
            juce::String err;
            expect (p.save (err), err);
            const auto back = SoundMapProject::load (p.folder, err);
            expect (back.has_value(), err);
            if (back)
            {
                expectEquals (back->settings.tagSizeMm, 200.0);
                expectEquals (back->settings.micOffsetCm.y, -12.0);
                expectEquals ((int) back->survey.size(), 1);
                expectEquals (back->survey[0].tags[0].id, 4);
                expectEquals (back->survey[0].tags[0].corners[3].v, 8.0);
                expect (back->map.built);
                expectWithinAbsoluteError (rotationAngle (back->map.tags.at (0).R, p.map.tags.at (0).R), 0.0, 1e-12);
                expectEquals ((int) back->points.size(), 1);
                expect (! back->points[0].pending);
                expectEquals ((int) back->points[0].tagsBefore.size(), 1);
                expectEquals (back->points[0].levels.db[1], 79.5);
                expectEquals (back->points[0].position.y, -1.25);
                expectEquals (back->nextPointNumber(), 8);
                const auto csv = back->toCsv();
                expect (csv.startsWith ("point,kind,x_m,y_m,z_m"), csv);
                expect (csv.contains ("L_1kHz") && csv.contains ("L_1.25kHz"), csv);
                expect (csv.contains ("\n7,point,0.500,-1.250,-2.000,1,1.2,4,"), csv);
            }
        }

        beginTest ("marker sheets: a valid PDF, one page per tag, at the exact size");
        {
            juce::MemoryBlock mb;
            expect (markersPdf ({ 0, 1, 2 }, 160.0, paperFor (160.0), mb));
            const auto s = mb.toString();
            expect (s.startsWith ("%PDF-1.4"));
            expect (s.contains ("/Count 3"));
            expect (s.contains ("%%EOF"));
            // xref offsets point at their objects
            const int xrefAt = s.lastIndexOf ("\nxref\n") + 1;
            const auto lines = juce::StringArray::fromLines (s.substring (xrefAt));
            int okObjects = 0;
            for (int i = 3; i < lines.size() && lines[i].endsWith (" n "); ++i)
            {
                const auto off = lines[i].substring (0, 10).getLargeIntValue();
                if (s.substring ((int) off).startsWith (juce::String (i - 2) + " 0 obj")) ++okObjects;
            }
            expectEquals (okObjects, 4 + 2 * 3);
            // 160 mm = 453.543 pt: the black square
            expect (s.contains ("453.543 453.543 re\nf"), "black square at 160 mm");
            expect (paperFor (160.0).name == "Letter" && paperFor (200.0).name == "Tabloid" && paperFor (230.0).name == "12 x 16 in");
            expect (markersPdf ({ 5 }, 230.0, paperFor (230.0), mb) && mb.toString().contains ("[0 0 864.000 1152.000]"),
                    "a 230 mm tag on a 12 x 16 in page");
            expect (! markersPdf ({ 600 }, 160.0, paperFor (160.0), mb), "id out of the family");
            expect (writeMarkersPdf (dir.getChildFile ("tags.pdf"), { 0 }, 160.0));
        }

        dir.deleteRecursively();
    }
};


/** Not a check: a table of the expected precision for docs/sound-map.md
    (SC_SOUNDMAP_STUDY=1). Camera-centre sigma from the resection fit at
    0.3 px corner noise, over points spread in the bowl. */
class SoundMapStudy : public juce::UnitTest
{
public:
    SoundMapStudy() : juce::UnitTest ("Sound map: precision study", "SpectroCraftMobile") {}

    void runTest() override
    {
        beginTest ("precision study (SC_SOUNDMAP_STUDY=1)");
        if (juce::SystemStats::getEnvironmentVariable ("SC_SOUNDMAP_STUDY", {}).isEmpty()) return;
        juce::Random rng (5);
        {
            // survey precision: 28 pictures, the same twice (other noise), plus 40 measurement pictures
            const Scene s = makeBowl();
            auto noisyPhoto = [&] (const Pose& cam)
            {
                SurveyPhoto p;
                p.width = s.width; p.height = s.height;
                for (auto v : visibleTags (s, cam))
                {
                    for (auto& c : v.truth.corners) { c.u += 0.3 * gauss (rng); c.v += 0.3 * gauss (rng); }
                    p.tags.push_back (v.truth);
                }
                return p;
            };
            std::vector<SurveyPhoto> base, twice, more;
            for (const auto& cam : surveyCameras()) base.push_back (noisyPhoto (cam));
            twice = base;
            for (const auto& cam : surveyCameras()) twice.push_back (noisyPhoto (cam));
            more = base;
            for (int rep = 0; rep < 2; ++rep)
                for (const auto& cam : measureCameras()) more.push_back (noisyPhoto (cam));
            SurveySettings st;
            st.tagSize = s.tagSize;
            st.tagHeightAboveDeck = s.tagAboveDeck;
            for (const auto* set : { &base, &twice, &more })
            {
                const auto r = solveSurvey (*set, st);
                const auto fit = rigidFit (r.tags, s.tags);
                logMessage ("STUDY survey " + juce::String ((int) set->size()) + " pictures: shape " + juce::String (fit.rms * 1000.0, 2)
                            + " mm, frame " + juce::String (fit.angleDeg, 3) + " deg, rms " + juce::String (r.rmsPx, 3) + " px");
            }
            // the same 28 pictures started from the truth: the adjustment's own limit
        }
        {
            // calibration of the reported sigma: RMS error / RMS sigma over many cameras
            const Scene s = makeBowl (16);
            const auto K = Intrinsics::forPicture (s.lens, s.width, s.height);
            std::vector<double> ratios;
            double errSq = 0.0, sigSq = 0.0;
            int n = 0, beyond2 = 0;
            for (int k = 0; k < 400; ++k)
            {
                const double r = 0.2 + 3.0 * std::fmod (k * 0.618, 1.0);
                const double a = k * 137.5;
                const Pose cam = cameraAt (r * std::cos (a * kPi / 180.0), r * std::sin (a * kPi / 180.0), a + 180.0 + (k % 3 - 1) * 35.0, 0.0, 5.0);
                std::vector<TagObservation> obs;
                for (auto v : visibleTags (s, cam))
                {
                    for (auto& c : v.truth.corners) { c.u += 0.3 * gauss (rng); c.v += 0.3 * gauss (rng); }
                    obs.push_back (v.truth);
                }
                const auto rr = resect (obs, s.tags, s.tagSize, K);
                if (! rr.ok) continue;
                const double e = (rr.cameraCentre() - cam.inverse().t).norm();
                errSq += e * e; sigSq += rr.positionSigma * rr.positionSigma; ++n;
                if (e > 2.0 * rr.positionSigma) ++beyond2;
                ratios.push_back (e / rr.positionSigma);
            }
            logMessage ("STUDY sigma: RMS error / RMS sigma " + juce::String (std::sqrt (errSq / n) / std::sqrt (sigSq / n), 2)
                        + ", median error/sigma " + juce::String (median (ratios), 2) + ", beyond 2 sigma "
                        + juce::String (100.0 * beyond2 / n, 1) + " %, n " + juce::String (n));
        }
        for (int tagCount : { 16, 24 })
            for (double tagMm : { 160.0, 200.0 })
                for (int res : { 1920, 4032 })
                    for (double offset : { 0.0, 35.0, 60.0 })
                        for (int nPics : { 1, 2 })
                        {
                            Scene s = makeBowl (tagCount);
                            s.tagSize = tagMm / 1000.0;
                            s.width = res;
                            s.height = res == 1920 ? 1080 : 3024;
                            const auto K = Intrinsics::forPicture (s.lens, s.width, s.height);
                            std::vector<double> errs;
                            int few = 0;
                            for (int k = 0; k < 60; ++k)
                            {
                                const double r = 0.2 + 3.0 * std::fmod (k * 0.618, 1.0);
                                const double a = k * 137.5;
                                const double x = r * std::cos (a * kPi / 180.0), y = r * std::sin (a * kPi / 180.0);
                                const double side = (k % 2 == 0 ? 1.0 : -1.0) * offset;
                                const Pose cam = cameraAt (x, y, a + 180.0 + side, 0.0, 5.0);
                                Vec3 sum;
                                int ok = 0;
                                for (int pic = 0; pic < nPics; ++pic)
                                {
                                    std::vector<TagObservation> obs;
                                    for (auto v : visibleTags (s, cam))
                                    {
                                        for (auto& c : v.truth.corners) { c.u += 0.3 * gauss (rng); c.v += 0.3 * gauss (rng); }
                                        obs.push_back (v.truth);
                                    }
                                    const auto rr = resect (obs, s.tags, s.tagSize, K);
                                    if (rr.ok && rr.tagsUsed >= 2) { sum += rr.cameraCentre(); ++ok; }
                                }
                                if (ok == 0) { ++few; continue; }
                                errs.push_back (((sum / ok) - cam.inverse().t).norm());
                            }
                            std::sort (errs.begin(), errs.end());
                            const double p50 = errs.empty() ? 0.0 : errs[errs.size() / 2];
                            const double p90 = errs.empty() ? 0.0 : errs[(size_t) (0.9 * (double) (errs.size() - 1))];
                            logMessage ("STUDY tags " + juce::String (tagCount) + " size " + juce::String (tagMm, 0) + " mm, "
                                        + juce::String (s.width) + " px, aim +" + juce::String (offset, 0) + " deg, "
                                        + juce::String (nPics) + " pic: median " + juce::String (p50 * 1000.0, 1)
                                        + " mm, p90 " + juce::String (p90 * 1000.0, 1) + " mm, no fix " + juce::String (few) + "/60");
                        }
    }
};


/** Not a check: writes a demo sound map for screenshots of the Map tab
    (SC_SOUNDMAP_DEMO=<recordings folder>): the synthetic bowl, its survey, a
    polar layout of points with made-up levels (louder near the axis in the
    high bands, as a focusing bowl), a background and two reference takes. */
class SoundMapDemo : public juce::UnitTest
{
public:
    SoundMapDemo() : juce::UnitTest ("Sound map: demo map", "SpectroCraftMobile") {}

    void runTest() override
    {
        beginTest ("tag sheets (SC_SOUNDMAP_PRINT=<pdf>, SC_SOUNDMAP_PRINT_TAGS, SC_SOUNDMAP_PRINT_MM)");
        if (const auto pdf = juce::SystemStats::getEnvironmentVariable ("SC_SOUNDMAP_PRINT", {}); pdf.isNotEmpty())
        {
            const int count = juce::SystemStats::getEnvironmentVariable ("SC_SOUNDMAP_PRINT_TAGS", "24").getIntValue();
            const double mm = juce::SystemStats::getEnvironmentVariable ("SC_SOUNDMAP_PRINT_MM", "160").getDoubleValue();
            std::vector<int> ids;
            for (int i = 0; i < count; ++i) ids.push_back (i);
            expect (writeMarkersPdf (juce::File (pdf), ids, mm));
            logMessage ("tag sheets: " + pdf);
        }

        beginTest ("detect tags in a picture (SC_SOUNDMAP_DETECT=<image file>)");
        if (const auto img = juce::SystemStats::getEnvironmentVariable ("SC_SOUNDMAP_DETECT", {}); img.isNotEmpty())
        {
            const auto image = juce::ImageFileFormat::loadFrom (juce::File (img));
            expect (image.isValid(), "picture readable");
            TagDetector detector (2);
            for (const auto& o : detector.detect (toGray (image)))
                logMessage ("DETECT id " + juce::String (o.id) + " margin " + juce::String (o.margin, 0) + " corners (bl, br, tr, tl): "
                            + juce::String (o.corners[0].u, 1) + "," + juce::String (o.corners[0].v, 1) + "  "
                            + juce::String (o.corners[1].u, 1) + "," + juce::String (o.corners[1].v, 1) + "  "
                            + juce::String (o.corners[2].u, 1) + "," + juce::String (o.corners[2].v, 1) + "  "
                            + juce::String (o.corners[3].u, 1) + "," + juce::String (o.corners[3].v, 1));
        }

        beginTest ("demo map (SC_SOUNDMAP_DEMO=<folder>)");
        const auto out = juce::SystemStats::getEnvironmentVariable ("SC_SOUNDMAP_DEMO", {});
        if (out.isEmpty()) return;
        juce::Random rng (11);
        const Scene s = makeBowl (24);
        std::vector<SurveyPhoto> photos;
        for (const auto& cam : surveyCameras())
        {
            SurveyPhoto p;
            p.width = s.width; p.height = s.height;
            for (auto v : visibleTags (s, cam))
            {
                for (auto& c : v.truth.corners) { c.u += 0.3 * gauss (rng); c.v += 0.3 * gauss (rng); }
                p.tags.push_back (v.truth);
            }
            photos.push_back (p);
        }
        SoundMapProject project;
        project.folder = juce::File (out).getChildFile ("SoundMaps").getChildFile ("demo-bowl");
        project.folder.deleteRecursively();
        project.folder.createDirectory();
        project.name = "demo-bowl";
        project.settings.tagHeightAboveDeckCm = 20.0;
        for (const auto& p : photos)
        {
            SurveyPicture sp;
            sp.width = p.width; sp.height = p.height; sp.tags = p.tags;
            project.survey.push_back (sp);
        }
        SurveySettings st = project.surveySettings();
        st.tagSize = s.tagSize;
        const auto r = solveSurvey (photos, st);
        expect (r.ok, r.problem);
        project.setMap (r);

        int n = 0;
        const auto t0 = juce::Time::getCurrentTime() - juce::RelativeTime::minutes (40);
        auto addPoint = [&] (double x, double y, PointKind kind)
        {
            const double a = std::atan2 (y, x) * 180.0 / kPi;
            const Pose cam = cameraAt (x, y, a + 180.0 + 35.0, 0.0, 6.0);
            std::vector<TagObservation> obs;
            for (auto v : visibleTags (s, cam))
            {
                for (auto& c : v.truth.corners) { c.u += 0.3 * gauss (rng); c.v += 0.3 * gauss (rng); }
                obs.push_back (v.truth);
            }
            MapPoint q;
            q.number = ++n;
            q.kind = kind;
            q.pending = false;
            q.wav = "points/P" + juce::String (n).paddedLeft ('0', 3) + ".wav";
            q.time = t0 + juce::RelativeTime::seconds (25.0 * n);
            q.width = s.width; q.height = s.height;
            q.tagsBefore = obs;
            q.before = project.locate (obs, s.width, s.height, q.hold);
            q.after = q.before;
            q.updatePosition();
            q.levels.unit = "dB-SPL";
            q.levels.calibrated = true;
            const double rr = std::hypot (x, y);
            double e = 0.0;
            for (double f : { 63.0, 80.0, 100.0, 125.0, 160.0, 200.0, 250.0, 315.0, 400.0, 500.0, 630.0, 800.0, 1000.0,
                              1250.0, 1600.0, 2000.0, 2500.0, 3150.0, 4000.0, 5000.0, 6300.0, 8000.0 })
            {
                double l = 72.0 - 4.0 * rr + 9.0 * std::exp (-std::pow (rr / (0.25 + 120.0 / f), 2.0))
                           + 2.0 * std::cos (2.0 * kPi * rr * f / 343.0) * std::exp (-f / 800.0) + 0.4 * gauss (rng);
                if (kind == PointKind::Background) l = 45.0 - 6.0 * std::log10 (f / 63.0) + gauss (rng);
                if (kind == PointKind::Reference) l = 74.0 + 0.1 * n / 10.0 + 0.1 * gauss (rng);
                q.levels.centresHz.push_back (f);
                q.levels.db.push_back (l);
                e += std::pow (10.0, (l + aWeightingDb (f)) / 10.0);
            }
            q.levels.laeq = 10.0 * std::log10 (e);
            q.levels.lzeq = q.levels.laeq + 2.0;
            project.points.push_back (q);
        };
        addPoint (0.4, 0.0, PointKind::Reference);
        addPoint (-0.8, 1.2, PointKind::Background);
        addPoint (0.0, 0.05, PointKind::Point);
        for (double ring : { 0.5, 1.0, 1.5, 2.0, 2.5, 3.0 })
        {
            const int count = ring < 1.2 ? 8 : 12;
            for (int k = 0; k < count; ++k)
            {
                const double a = 2.0 * kPi * (k + (ring > 1.2 ? 0.5 : 0.0)) / count;
                addPoint (ring * std::cos (a), ring * std::sin (a), PointKind::Point);
            }
        }
        addPoint (0.4, 0.0, PointKind::Reference);
        juce::String err;
        expect (project.save (err), err);
        logMessage ("demo map: " + project.folder.getFullPathName() + ", " + juce::String ((int) project.points.size()) + " takes");
    }
};

static SoundMapMathTests soundMapMathTests;
static SoundMapSurveyTests soundMapSurveyTests;
static SoundMapImageTests soundMapImageTests;
static SoundMapDataTests soundMapDataTests;
static SoundMapStudy soundMapStudy;
static SoundMapDemo soundMapDemo;
