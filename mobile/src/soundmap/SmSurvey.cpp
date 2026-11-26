#include "SmSurvey.h"
#include <set>

namespace scm::smap
{
namespace
{
constexpr double kHuberPx = 1.5;
constexpr double kK1Sigma = 0.25;   // weak prior keeping k1 near 0 when the pictures cannot tell
constexpr double kPpSigma = 0.01;   // principal point within about 1 % of the long side
constexpr int kLensParams = 4;      // fRel, k1, ppx, ppy

struct Obs
{
    int photo = -1;
    int tag = -1;
    const TagObservation* o = nullptr;
    bool rejected = false;
};

struct State
{
    Lens lens;
    int refTag = -1;
    std::map<int, Pose> refFromTag;   // linked tags
    std::vector<bool> camOk;
    std::vector<Pose> camFromRef;
};

Intrinsics intrinsicsFor (const SurveyPhoto& p, const Lens& lens) noexcept
{
    return Intrinsics::forPicture (lens, p.width, p.height, p.hold);
}

/** The 8 residuals (pixels) of one tag observation. */
void blockResidual (const Pose& camFromRef, const Pose& refFromTag, const Intrinsics& K,
                    const std::array<Vec3, 4>& corners, const TagObservation& o, double r[8]) noexcept
{
    const Pose camFromTag = camFromRef * refFromTag;
    for (size_t c = 0; c < 4; ++c)
    {
        double u = 0.0, v = 0.0;
        if (! K.project (camFromTag * corners[c], u, v)) { r[2 * c] = 1000.0; r[2 * c + 1] = 1000.0; continue; }
        r[2 * c] = u - o.corners[c].u;
        r[2 * c + 1] = v - o.corners[c].v;
    }
}

double huberCost (const double r[8]) noexcept
{
    double c = 0.0;
    for (int k = 0; k < 4; ++k)
    {
        const double n = std::hypot (r[2 * k], r[2 * k + 1]);
        c += n <= kHuberPx ? n * n : 2.0 * kHuberPx * n - kHuberPx * kHuberPx;
    }
    return c;
}

double observationRms (const State& s, const std::vector<SurveyPhoto>& photos, const Obs& ob,
                       const std::array<Vec3, 4>& corners)
{
    double r[8];
    blockResidual (s.camFromRef[(size_t) ob.photo], s.refFromTag.at (ob.tag),
                   intrinsicsFor (photos[(size_t) ob.photo], s.lens), corners, *ob.o, r);
    double sum = 0.0;
    for (double v : r) sum += v * v;
    return std::sqrt (sum / 4.0);
}

bool active (const State& s, const Obs& ob) noexcept
{
    return ! ob.rejected && s.camOk[(size_t) ob.photo] && s.refFromTag.count (ob.tag) != 0;
}

/** Robust mean of poses: chordal mean of the rotations, median of the translations. */
Pose averagePoses (const std::vector<Pose>& ps)
{
    Mat3 sum = Mat3::zero();
    std::vector<double> xs, ys, zs;
    for (const auto& p : ps)
    {
        sum = sum + p.R;
        xs.push_back (p.t.x); ys.push_back (p.t.y); zs.push_back (p.t.z);
    }
    return { nearestRotation (sum), { median (xs), median (ys), median (zs) } };
}

// ------------------------------------------------------------------ start

struct Start
{
    State state;
    double score = 1e300;
};

Start initialise (const std::vector<SurveyPhoto>& photos, const std::vector<Obs>& obs, const Lens& lens, double tagSize)
{
    Start out;
    State& s = out.state;
    s.lens = lens;
    s.camOk.assign (photos.size(), false);
    s.camFromRef.assign (photos.size(), Pose {});

    // both planar solutions of every observation
    std::vector<std::vector<PoseCandidate>> cands (obs.size());
    std::vector<double> quality (obs.size(), 0.0);
    for (size_t i = 0; i < obs.size(); ++i)
    {
        if (obs[i].rejected) continue;
        cands[i] = tagPoseCandidates (*obs[i].o, tagSize, intrinsicsFor (photos[(size_t) obs[i].photo], lens));
        if (cands[i].empty()) continue;
        const double ratio = cands[i].size() > 1 ? cands[i][1].rmsPx / std::max (cands[i][0].rmsPx, 0.02) : 10.0;
        quality[i] = obs[i].o->sizePx() * std::clamp (ratio, 1.0, 10.0);
    }

    // Per picture, the planar solution of each tag. A tag whose two solutions
    // fit clearly differently keeps the better one. For an ambiguous tag, the
    // boards all standing upright decide: its solution whose printed-up axis
    // is closest to the clear tags' up axis. With no clear tag in the picture,
    // the solutions that make all the tags' up axes most nearly parallel win.
    // An ambiguous choice gets the lowest edge weight, so the spanning tree
    // prefers clear views.
    const auto ambiguous = [&cands] (size_t i)
    {
        return cands[i].size() > 1 && cands[i][1].rmsPx < 3.0 * std::max (cands[i][0].rmsPx, 0.02);
    };
    const auto upOf = [&cands] (size_t i, size_t c) { return cands[i][c].camFromTag.R.col (1); };
    std::vector<size_t> chosen (obs.size(), 0);
    std::map<int, std::vector<size_t>> byPhoto;
    std::map<int, int> seen;
    for (size_t i = 0; i < obs.size(); ++i)
        if (! cands[i].empty()) { byPhoto[obs[i].photo].push_back (i); ++seen[obs[i].tag]; }
    for (const auto& [photo, list] : byPhoto)
    {
        Vec3 up;
        for (size_t i : list)
            if (! ambiguous (i)) up += upOf (i, 0) * quality[i];
        if (up.norm() < 1e-12)
        {
            // no clear tag: try each solution of the largest tag as the reference
            size_t anchor = list.front();
            for (size_t i : list)
                if (obs[i].o->sizePx() > obs[anchor].o->sizePx()) anchor = i;
            double bestScore = -1e300;
            for (size_t a = 0; a < cands[anchor].size(); ++a)
            {
                const Vec3 ref = upOf (anchor, a);
                double score = 0.0;
                for (size_t i : list)
                    if (i != anchor)
                    {
                        double m = -1.0;
                        for (size_t c = 0; c < cands[i].size(); ++c) m = std::max (m, upOf (i, c).dot (ref));
                        score += m;
                    }
                if (score > bestScore) { bestScore = score; up = ref; chosen[anchor] = a; }
            }
        }
        up = up.normalised();
        for (size_t i : list)
        {
            if (! ambiguous (i)) continue;
            chosen[i] = upOf (i, 1).dot (up) > upOf (i, 0).dot (up) ? 1 : 0;
            quality[i] = obs[i].o->sizePx();   // ratio 1: the weakest kind of edge
        }
    }

    // best edge per pair of tags seen together
    struct Edge { double w = 0.0; Pose aFromB; };
    std::map<std::pair<int, int>, Edge> edges;
    for (const auto& [photo, list] : byPhoto)
        for (size_t x = 0; x < list.size(); ++x)
            for (size_t y = 0; y < list.size(); ++y)
            {
                if (x == y) continue;
                const size_t a = list[x], b = list[y];
                const double w = std::min (quality[a], quality[b]);
                auto& e = edges[{ obs[a].tag, obs[b].tag }];
                if (w > e.w)
                    e = { w, cands[a][chosen[a]].camFromTag.inverse() * cands[b][chosen[b]].camFromTag };
            }
    if (seen.empty()) return out;

    // reference: the most seen tag (lowest id on a tie)
    int ref = -1, refCount = -1;
    for (const auto& [id, n] : seen)
        if (n > refCount) { ref = id; refCount = n; }
    s.refTag = ref;
    s.refFromTag[ref] = Pose {};

    // maximum spanning tree from the reference
    for (;;)
    {
        double bestW = 0.0;
        std::pair<int, int> bestKey { -1, -1 };
        for (const auto& [key, e] : edges)
            if (s.refFromTag.count (key.first) != 0 && s.refFromTag.count (key.second) == 0 && e.w > bestW)
            {
                bestW = e.w;
                bestKey = key;
            }
        if (bestKey.first < 0) break;
        s.refFromTag[bestKey.second] = s.refFromTag[bestKey.first] * edges[bestKey].aFromB;
    }

    auto placeCameras = [&]
    {
        for (size_t p = 0; p < photos.size(); ++p)
        {
            std::vector<TagObservation> list;
            for (size_t i = 0; i < obs.size(); ++i)
                if (obs[i].photo == (int) p && ! obs[i].rejected) list.push_back (*obs[i].o);
            const auto r = resect (list, s.refFromTag, tagSize, intrinsicsFor (photos[p], lens), 1e9);
            s.camOk[p] = r.tagsUsed > 0 && r.camFromWorld.t.norm() > 0.0;
            if (s.camOk[p]) s.camFromRef[p] = r.camFromWorld;
        }
    };
    placeCameras();

    // re-estimate every tag from all its views, choosing the planar solution
    // that agrees with the current estimate; twice
    for (int round = 0; round < 2; ++round)
    {
        std::map<int, std::vector<Pose>> views;
        for (size_t i = 0; i < obs.size(); ++i)
        {
            const auto& ob = obs[i];
            if (cands[i].empty() || ! s.camOk[(size_t) ob.photo] || s.refFromTag.count (ob.tag) == 0 || ob.tag == ref) continue;
            const Pose& cur = s.refFromTag[ob.tag];
            const Pose refFromCam = s.camFromRef[(size_t) ob.photo].inverse();
            Pose best;
            double bestAng = 1e300;
            for (const auto& c : cands[i])
            {
                const Pose est = refFromCam * c.camFromTag;
                const double ang = rotationAngle (est.R, cur.R);
                if (ang < bestAng) { bestAng = ang; best = est; }
            }
            views[ob.tag].push_back (best);
        }
        for (auto& [id, list] : views)
            if (! list.empty()) s.refFromTag[id] = averagePoses (list);
        placeCameras();
    }

    // score: median error of the observations
    const auto corners = tagCorners (tagSize);
    std::vector<double> errs;
    for (const auto& ob : obs)
        if (active (s, ob)) errs.push_back (observationRms (s, photos, ob, corners));
    out.score = errs.empty() ? 1e300 : median (errs);
    return out;
}

// ------------------------------------------------------------ adjustment

struct Layout
{
    std::map<int, int> tagCol;      // tag id -> first column (not the reference)
    std::vector<int> camCol;        // photo -> first column, -1 if not used
    int lensCol = -1;               // fRel, k1, ppx, ppy
    int n = 0;
};

Layout layoutFor (const State& s, bool solveLens)
{
    Layout L;
    for (const auto& [id, p] : s.refFromTag)
        if (id != s.refTag) { L.tagCol[id] = L.n; L.n += 6; }
    L.camCol.assign (s.camOk.size(), -1);
    for (size_t p = 0; p < s.camOk.size(); ++p)
        if (s.camOk[p]) { L.camCol[p] = L.n; L.n += 6; }
    if (solveLens) { L.lensCol = L.n; L.n += kLensParams; }
    return L;
}

double totalCost (const State& s, const std::vector<SurveyPhoto>& photos, const std::vector<Obs>& obs,
                  const std::array<Vec3, 4>& corners, bool solveLens)
{
    double c = 0.0;
    for (const auto& ob : obs)
    {
        if (! active (s, ob)) continue;
        double r[8];
        blockResidual (s.camFromRef[(size_t) ob.photo], s.refFromTag.at (ob.tag),
                       intrinsicsFor (photos[(size_t) ob.photo], s.lens), corners, *ob.o, r);
        c += huberCost (r);
    }
    if (solveLens)
        c += (s.lens.k1 / kK1Sigma) * (s.lens.k1 / kK1Sigma) + (s.lens.ppx / kPpSigma) * (s.lens.ppx / kPpSigma)
             + (s.lens.ppy / kPpSigma) * (s.lens.ppy / kPpSigma);
    return c;
}

State applyStep (const State& s, const Layout& L, const std::vector<double>& d)
{
    State t = s;
    for (const auto& [id, col] : L.tagCol)
    {
        const auto c = (size_t) col;
        t.refFromTag[id] = s.refFromTag.at (id).perturbed ({ d[c], d[c + 1], d[c + 2] }, { d[c + 3], d[c + 4], d[c + 5] });
    }
    for (size_t p = 0; p < L.camCol.size(); ++p)
    {
        if (L.camCol[p] < 0) continue;
        const auto c = (size_t) L.camCol[p];
        t.camFromRef[p] = s.camFromRef[p].perturbed ({ d[c], d[c + 1], d[c + 2] }, { d[c + 3], d[c + 4], d[c + 5] });
    }
    if (L.lensCol >= 0)
    {
        t.lens.fRel = s.lens.fRel + d[(size_t) L.lensCol];
        t.lens.k1 = s.lens.k1 + d[(size_t) L.lensCol + 1];
        t.lens.ppx = s.lens.ppx + d[(size_t) L.lensCol + 2];
        t.lens.ppy = s.lens.ppy + d[(size_t) L.lensCol + 3];
    }
    return t;
}

void bundleAdjust (State& s, const std::vector<SurveyPhoto>& photos, const std::vector<Obs>& obs, double tagSize,
                   bool solveLens, int maxIterations)
{
    const auto corners = tagCorners (tagSize);
    const Layout L = layoutFor (s, solveLens);
    const int n = L.n;
    if (n == 0) return;

    double cost = totalCost (s, photos, obs, corners, solveLens);
    double lambda = 1e-4;
    std::vector<double> JtJ, Jtr, step;
    constexpr double eps = 1e-6;

    for (int it = 0; it < maxIterations; ++it)
    {
        JtJ.assign ((size_t) n * (size_t) n, 0.0);
        Jtr.assign ((size_t) n, 0.0);
        for (const auto& ob : obs)
        {
            if (! active (s, ob)) continue;
            const auto& photo = photos[(size_t) ob.photo];
            const Pose& cam = s.camFromRef[(size_t) ob.photo];
            const Pose& tag = s.refFromTag.at (ob.tag);
            double r0[8];
            blockResidual (cam, tag, intrinsicsFor (photo, s.lens), corners, *ob.o, r0);
            double w[4];
            for (int k = 0; k < 4; ++k) w[k] = huberWeight (std::hypot (r0[2 * k], r0[2 * k + 1]), kHuberPx);

            // columns of this block
            std::vector<int> cols;
            std::vector<std::array<double, 8>> J;
            auto addColumn = [&] (int col, const Pose& camP, const Pose& tagP, const Pose& camM, const Pose& tagM,
                                  const Lens& lensP, const Lens& lensM, double h)
            {
                double rp[8], rm[8];
                blockResidual (camP, tagP, intrinsicsFor (photo, lensP), corners, *ob.o, rp);
                blockResidual (camM, tagM, intrinsicsFor (photo, lensM), corners, *ob.o, rm);
                std::array<double, 8> c {};
                for (int k = 0; k < 8; ++k) c[(size_t) k] = (rp[k] - rm[k]) / (2.0 * h);
                cols.push_back (col);
                J.push_back (c);
            };
            if (const auto it2 = L.tagCol.find (ob.tag); it2 != L.tagCol.end())
                for (int k = 0; k < 6; ++k)
                {
                    Vec3 dw, dt;
                    if (k < 3) dw[k] = eps; else dt[k - 3] = eps;
                    addColumn (it2->second + k, cam, tag.perturbed (dw, dt), cam, tag.perturbed (-dw, -dt), s.lens, s.lens, eps);
                }
            if (const int cc = L.camCol[(size_t) ob.photo]; cc >= 0)
                for (int k = 0; k < 6; ++k)
                {
                    Vec3 dw, dt;
                    if (k < 3) dw[k] = eps; else dt[k - 3] = eps;
                    addColumn (cc + k, cam.perturbed (dw, dt), tag, cam.perturbed (-dw, -dt), tag, s.lens, s.lens, eps);
                }
            if (L.lensCol >= 0)
                for (int k = 0; k < kLensParams; ++k)
                {
                    Lens p = s.lens, m = s.lens;
                    double* pv[] = { &p.fRel, &p.k1, &p.ppx, &p.ppy };
                    double* mv[] = { &m.fRel, &m.k1, &m.ppx, &m.ppy };
                    *pv[k] += eps;
                    *mv[k] -= eps;
                    addColumn (L.lensCol + k, cam, tag, cam, tag, p, m, eps);
                }
            for (size_t a = 0; a < cols.size(); ++a)
            {
                double g = 0.0;
                for (int k = 0; k < 8; ++k) g += w[k / 2] * J[a][(size_t) k] * r0[k];
                Jtr[(size_t) cols[a]] += g;
                for (size_t b = 0; b < cols.size(); ++b)
                {
                    double h = 0.0;
                    for (int k = 0; k < 8; ++k) h += w[k / 2] * J[a][(size_t) k] * J[b][(size_t) k];
                    JtJ[(size_t) cols[a] * (size_t) n + (size_t) cols[b]] += h;
                }
            }
        }
        if (L.lensCol >= 0)
        {
            const auto k1 = (size_t) L.lensCol + 1, px = k1 + 1, py = k1 + 2;
            JtJ[k1 * (size_t) n + k1] += 1.0 / (kK1Sigma * kK1Sigma);
            Jtr[k1] += s.lens.k1 / (kK1Sigma * kK1Sigma);
            JtJ[px * (size_t) n + px] += 1.0 / (kPpSigma * kPpSigma);
            Jtr[px] += s.lens.ppx / (kPpSigma * kPpSigma);
            JtJ[py * (size_t) n + py] += 1.0 / (kPpSigma * kPpSigma);
            Jtr[py] += s.lens.ppy / (kPpSigma * kPpSigma);
        }

        bool accepted = false;
        for (int tries = 0; tries < 12 && ! accepted; ++tries)
        {
            auto A = JtJ;
            for (int d = 0; d < n; ++d)
            {
                auto& diag = A[(size_t) d * (size_t) n + (size_t) d];
                diag += lambda * std::max (diag, 1e-6) + 1e-12;
            }
            std::vector<double> rhs ((size_t) n);
            for (int d = 0; d < n; ++d) rhs[(size_t) d] = -Jtr[(size_t) d];
            if (! choleskySolve (A, step, rhs, n)) { lambda *= 10.0; continue; }
            State trial = applyStep (s, L, step);
            if (trial.lens.fRel < 0.2 || trial.lens.fRel > 5.0) { lambda *= 10.0; continue; }
            const double c = totalCost (trial, photos, obs, corners, solveLens);
            if (c < cost)
            {
                const double gain = cost - c;
                s = std::move (trial);
                cost = c;
                lambda = std::max (1e-10, lambda * 0.3);
                accepted = true;
                if (gain < 1e-10 * std::max (1.0, cost)) return;
            }
            else
                lambda *= 10.0;
        }
        if (! accepted) return;
    }
}

std::string idList (const std::vector<int>& ids)
{
    std::string s;
    for (size_t i = 0; i < ids.size(); ++i) s += (i ? ", " : "") + std::to_string (ids[i]);
    return s;
}
} // namespace

// ------------------------------------------------------------------ frame

Frame tagFrame (const std::map<int, Pose>& refFromTag, double tagCentreHeight)
{
    Frame f;
    if (refFromTag.empty()) { f.note = "No tag."; return f; }
    const int firstId = refFromTag.begin()->first;
    const Pose& first = refFromTag.begin()->second;
    std::vector<Vec3> c;
    Vec3 sumY, sumZ, m;
    for (const auto& [id, p] : refFromTag)
    {
        c.push_back (p.t);
        sumY += p.R.col (1);
        sumZ += p.R.col (2);
        m += p.t;
    }
    m = m / (double) c.size();

    Vec3 n;
    bool planar = false;
    if (c.size() >= 3)
    {
        std::vector<double> S (9, 0.0);
        for (const auto& p : c)
        {
            const Vec3 d = p - m;
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b) S[(size_t) (a * 3 + b)] += d[a] * d[b];
        }
        const auto e = symmetricEigen (S, 3);
        if (e.values[1] > 1e-6 * std::max (e.values[2], 1e-12))
        {
            n = { e.vectors[0][0], e.vectors[0][1], e.vectors[0][2] };
            planar = true;
        }
    }
    if (! planar)
    {
        // one or two tags (or all in a line): the first tag's printed up direction
        n = first.R.col (1);
        f.note = "Fewer than 3 tags apart: z follows tag " + std::to_string (firstId) + "'s upright direction.";
    }
    else
    {
        const Vec3 hint = std::abs (sumY.dot (n)) >= std::abs (sumZ.dot (n)) ? sumY : sumZ;
        if (hint.dot (n) < 0.0) n = -n;
    }
    n = n.normalised();

    // in-plane basis, e1 toward the first tag
    Vec3 e1 = (first.t - m) - n * (first.t - m).dot (n);
    if (e1.norm() < 1e-9) e1 = std::abs (n.x) < 0.9 ? Vec3 { 1, 0, 0 }.cross (n) : Vec3 { 0, 1, 0 }.cross (n);
    e1 = e1.normalised();
    const Vec3 e2 = n.cross (e1);

    double ox = 0.0, oy = 0.0;
    if (planar)
    {
        // Kasa circle fit: x^2 + y^2 + D x + E y + F = 0
        std::vector<double> A (9, 0.0), b (3, 0.0), sol;
        std::vector<std::array<double, 2>> q;
        for (const auto& p : c)
        {
            const double x = (p - m).dot (e1), y = (p - m).dot (e2), z = x * x + y * y;
            q.push_back ({ x, y });
            const double row[3] = { x, y, 1.0 };
            for (int i = 0; i < 3; ++i)
            {
                for (int j = 0; j < 3; ++j) A[(size_t) (i * 3 + j)] += row[i] * row[j];
                b[(size_t) i] -= row[i] * z;
            }
        }
        if (choleskySolve (A, sol, b, 3))
        {
            const double cx = -0.5 * sol[0], cy = -0.5 * sol[1];
            const double r2 = cx * cx + cy * cy - sol[2];
            if (r2 > 0.0)
            {
                const double r = std::sqrt (r2);
                double e = 0.0;
                for (const auto& p : q) { const double d = std::hypot (p[0] - cx, p[1] - cy) - r; e += d * d; }
                e = std::sqrt (e / (double) q.size());
                if (r < 200.0 && e < 0.2 * r)
                {
                    ox = cx; oy = cy;
                    f.circle = true;
                    f.radius = r;
                    f.circleRms = e;
                }
            }
        }
        if (! f.circle) f.note = "The tags are not on a circle: the origin is their centroid.";
    }

    const Vec3 origin = m + e1 * ox + e2 * oy - n * tagCentreHeight;
    Vec3 x = (first.t - origin) - n * (first.t - origin).dot (n);
    if (x.norm() < 1e-9) x = e1;
    x = x.normalised();
    const Vec3 y = n.cross (x);
    const Mat3 R = Mat3::fromRows (x, y, n);
    f.mapFromRef = { R, -(R * origin) };
    return f;
}

// ----------------------------------------------------------------- solver

SurveyResult solveSurvey (const std::vector<SurveyPhoto>& photos, const SurveySettings& settings)
{
    SurveyResult res;
    res.photoUsed.assign (photos.size(), false);
    res.cameras.assign (photos.size(), Pose {});
    const double tagSize = settings.tagSize;
    if (! (tagSize > 0.0)) { res.problem = "The tag size is not set."; return res; }

    std::vector<Obs> obs;
    std::set<int> allTags;
    for (size_t p = 0; p < photos.size(); ++p)
        for (const auto& o : photos[p].tags)
        {
            obs.push_back ({ (int) p, o.id, &o, false });
            allTags.insert (o.id);
        }
    if (allTags.size() < 2) { res.problem = "The pictures show fewer than two different tags."; return res; }

    // starting focal length
    std::vector<double> fCandidates { settings.lens.fRel };
    if (settings.searchFocal)
        for (double f : { 0.55, 0.62, 0.7, 0.78, 0.86, 0.95, 1.05, 1.2 })
            if (std::abs (f - settings.lens.fRel) > 0.02) fCandidates.push_back (f);
    State best;
    double bestScore = 1e300;
    for (double f : fCandidates)
    {
        Lens lens = settings.lens;
        lens.fRel = f;
        auto start = initialise (photos, obs, lens, tagSize);
        if (start.state.refFromTag.size() < 2) continue;
        if (fCandidates.size() > 1)
        {
            bundleAdjust (start.state, photos, obs, tagSize, false, 8);
            std::vector<double> errs;
            const auto corners = tagCorners (tagSize);
            for (const auto& ob : obs)
                if (active (start.state, ob)) errs.push_back (observationRms (start.state, photos, ob, corners));
            start.score = errs.empty() ? 1e300 : median (errs);
        }
        if (start.score < bestScore) { bestScore = start.score; best = std::move (start.state); }
    }
    if (best.refFromTag.size() < 2)
    {
        res.problem = "No picture shows two tags together: take pictures that each show two tags or more.";
        return res;
    }

    // full adjustment, then once more without the observations that do not fit
    const auto corners = tagCorners (tagSize);
    bundleAdjust (best, photos, obs, tagSize, settings.solveLens, 100);
    {
        std::vector<double> errs;
        for (const auto& ob : obs)
            if (active (best, ob)) errs.push_back (observationRms (best, photos, ob, corners));
        const double lim = std::max (2.0, 5.0 * median (errs));
        int rejected = 0;
        for (auto& ob : obs)
            if (active (best, ob) && observationRms (best, photos, ob, corners) > lim) { ob.rejected = true; ++rejected; }
        if (rejected > 0)
        {
            res.rejectedObservations = rejected;
            // a camera or a tag left without observations drops out
            std::map<int, int> tagCount;
            std::vector<int> camCount (photos.size(), 0);
            for (const auto& ob : obs)
                if (active (best, ob)) { ++tagCount[ob.tag]; ++camCount[(size_t) ob.photo]; }
            for (auto it = best.refFromTag.begin(); it != best.refFromTag.end();)
                it = (tagCount[it->first] == 0 && it->first != best.refTag) ? best.refFromTag.erase (it) : std::next (it);
            for (size_t p = 0; p < photos.size(); ++p)
                if (camCount[p] == 0) best.camOk[p] = false;
            bundleAdjust (best, photos, obs, tagSize, settings.solveLens, 100);
        }
    }

    // statistics
    std::map<int, double> sq;
    std::map<int, int> cnt;
    double total = 0.0;
    int nObs = 0;
    for (const auto& ob : obs)
    {
        if (! active (best, ob)) continue;
        const double e = observationRms (best, photos, ob, corners);
        sq[ob.tag] += e * e;
        ++cnt[ob.tag];
        total += e * e;
        ++nObs;
    }
    res.observations = nObs;
    res.rmsPx = nObs > 0 ? std::sqrt (total / nObs) : 0.0;
    for (const auto& [id, k] : cnt)
    {
        res.tagPhotos[id] = k;
        res.tagRmsPx[id] = std::sqrt (sq[id] / k);
        if (k == 1) res.weakTags.push_back (id);
    }
    for (int id : allTags)
        if (best.refFromTag.count (id) == 0) res.unlinkedTags.push_back (id);

    // the map's frame
    res.frame = tagFrame (best.refFromTag, settings.tagCentreHeight);
    const Pose refFromMap = res.frame.mapFromRef.inverse();
    for (const auto& [id, p] : best.refFromTag) res.tags[id] = res.frame.mapFromRef * p;
    for (size_t p = 0; p < photos.size(); ++p)
    {
        res.photoUsed[p] = best.camOk[p];
        if (best.camOk[p]) res.cameras[p] = best.camFromRef[p] * refFromMap;
    }
    res.lens = best.lens;

    if (! res.unlinkedTags.empty())
        res.warnings.push_back ("Tags " + idList (res.unlinkedTags) + " are not linked to the others: take a picture that shows each with a mapped tag.");
    if (! res.weakTags.empty())
        res.warnings.push_back ("Tags " + idList (res.weakTags) + " are in one picture only: take another picture of each.");
    if (res.rmsPx > 1.0)
        res.warnings.push_back ("The error is high (" + std::to_string (res.rmsPx).substr (0, 4)
                                + " px): a board may be bent or may have moved during the survey.");
    if (res.lens.fRel < 0.45 || res.lens.fRel > 1.6)
        res.warnings.push_back ("The focal length found is unusual: check the pictures (zoom, ultra-wide camera).");
    if (! res.frame.note.empty()) res.warnings.push_back (res.frame.note);

    res.ok = res.tags.size() >= 2 && nObs > 0;
    if (! res.ok) res.problem = "The survey could not place the tags.";
    return res;
}
} // namespace scm::smap
