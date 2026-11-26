#include "SmPose.h"

namespace scm::smap
{
namespace
{
constexpr double kBehindPx = 1000.0;

void residual (const Pose& P, const Vec3& X, const Pixel& obs, const Intrinsics& K, double& ru, double& rv) noexcept
{
    double u = 0.0, v = 0.0;
    if (! K.project (P * X, u, v)) { ru = kBehindPx; rv = kBehindPx; return; }
    ru = u - obs.u;
    rv = v - obs.v;
}

/** Normal equations of the pose problem at P: JtJ (6x6), Jtr (6), weighted cost. */
double buildNormal (const Pose& P, const std::vector<Vec3>& world, const std::vector<Pixel>& px, const Intrinsics& K,
                    double huberPx, std::vector<double>& JtJ, std::vector<double>& Jtr)
{
    JtJ.assign (36, 0.0);
    Jtr.assign (6, 0.0);
    double cost = 0.0;
    constexpr double eps = 1e-6;
    for (size_t i = 0; i < world.size(); ++i)
    {
        double r0u, r0v;
        residual (P, world[i], px[i], K, r0u, r0v);
        const double rn = std::hypot (r0u, r0v);
        const double w = huberWeight (rn, huberPx);
        cost += rn <= huberPx ? rn * rn : 2.0 * huberPx * rn - huberPx * huberPx;
        double J[2][6];
        for (int k = 0; k < 6; ++k)
        {
            Vec3 dw, dt;
            if (k < 3) dw[k] = eps; else dt[k - 3] = eps;
            double pu, pv, mu, mv;
            residual (P.perturbed (dw, dt), world[i], px[i], K, pu, pv);
            residual (P.perturbed (-dw, -dt), world[i], px[i], K, mu, mv);
            J[0][k] = (pu - mu) / (2.0 * eps);
            J[1][k] = (pv - mv) / (2.0 * eps);
        }
        for (int a = 0; a < 6; ++a)
        {
            Jtr[(size_t) a] += w * (J[0][a] * r0u + J[1][a] * r0v);
            for (int b = 0; b < 6; ++b)
                JtJ[(size_t) (a * 6 + b)] += w * (J[0][a] * J[0][b] + J[1][a] * J[1][b]);
        }
    }
    return cost;
}

double robustCost (const Pose& P, const std::vector<Vec3>& world, const std::vector<Pixel>& px, const Intrinsics& K,
                   double huberPx)
{
    double cost = 0.0;
    for (size_t i = 0; i < world.size(); ++i)
    {
        double ru, rv;
        residual (P, world[i], px[i], K, ru, rv);
        const double rn = std::hypot (ru, rv);
        cost += rn <= huberPx ? rn * rn : 2.0 * huberPx * rn - huberPx * huberPx;
    }
    return cost;
}

double medianError (const Pose& P, const std::vector<Vec3>& world, const std::vector<Pixel>& px, const Intrinsics& K)
{
    std::vector<double> e;
    e.reserve (world.size());
    for (size_t i = 0; i < world.size(); ++i)
    {
        double ru, rv;
        residual (P, world[i], px[i], K, ru, rv);
        e.push_back (std::hypot (ru, rv));
    }
    return median (e);
}
} // namespace

bool homographyDlt (const std::vector<std::array<double, 2>>& src,
                    const std::vector<std::array<double, 2>>& dst,
                    std::array<double, 9>& H)
{
    const size_t n = src.size();
    if (n < 4 || dst.size() != n) return false;

    // Hartley normalisation of both point sets
    auto normaliser = [] (const std::vector<std::array<double, 2>>& p, double& mx, double& my, double& s)
    {
        mx = 0.0; my = 0.0;
        for (const auto& q : p) { mx += q[0]; my += q[1]; }
        mx /= (double) p.size(); my /= (double) p.size();
        double d = 0.0;
        for (const auto& q : p) d += std::hypot (q[0] - mx, q[1] - my);
        d /= (double) p.size();
        s = d > 1e-15 ? std::sqrt (2.0) / d : 0.0;
        return s > 0.0;
    };
    double sx, sy, ss, dx, dy, ds;
    if (! normaliser (src, sx, sy, ss) || ! normaliser (dst, dx, dy, ds)) return false;

    std::vector<double> AtA (81, 0.0);
    for (size_t i = 0; i < n; ++i)
    {
        const double x = (src[i][0] - sx) * ss, y = (src[i][1] - sy) * ss;
        const double u = (dst[i][0] - dx) * ds, v = (dst[i][1] - dy) * ds;
        const double r1[9] = { -x, -y, -1.0, 0.0, 0.0, 0.0, u * x, u * y, u };
        const double r2[9] = { 0.0, 0.0, 0.0, -x, -y, -1.0, v * x, v * y, v };
        for (int a = 0; a < 9; ++a)
            for (int b = 0; b < 9; ++b)
                AtA[(size_t) (a * 9 + b)] += r1[a] * r1[b] + r2[a] * r2[b];
    }
    const auto eig = symmetricEigen (AtA, 9);
    const auto& h = eig.vectors.front();
    // denormalise: H = Tdst^-1 * Hn * Tsrc
    const double Hn[9] = { h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8] };
    const double Ts[9] = { ss, 0.0, -ss * sx, 0.0, ss, -ss * sy, 0.0, 0.0, 1.0 };
    const double Tdi[9] = { 1.0 / ds, 0.0, dx, 0.0, 1.0 / ds, dy, 0.0, 0.0, 1.0 };
    double tmp[9] = {}, out[9] = {};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k)
                tmp[r * 3 + c] += Hn[r * 3 + k] * Ts[k * 3 + c];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k)
                out[r * 3 + c] += Tdi[r * 3 + k] * tmp[k * 3 + c];
    if (std::abs (out[8]) < 1e-300) return false;
    for (int i = 0; i < 9; ++i) H[(size_t) i] = out[i] / out[8];
    return std::isfinite (H[0]);
}

double reprojectionRms (const Pose& P, const std::vector<Vec3>& world, const std::vector<Pixel>& px, const Intrinsics& K)
{
    if (world.empty()) return 0.0;
    double s = 0.0;
    for (size_t i = 0; i < world.size(); ++i)
    {
        double ru, rv;
        residual (P, world[i], px[i], K, ru, rv);
        s += ru * ru + rv * rv;
    }
    return std::sqrt (s / (double) world.size());
}

double refinePose (Pose& P, const std::vector<Vec3>& world, const std::vector<Pixel>& px,
                   const Intrinsics& K, double huberPx, int maxIterations)
{
    if (world.size() < 3 || px.size() != world.size()) return reprojectionRms (P, world, px, K);
    std::vector<double> JtJ, Jtr, step;
    double lambda = 1e-3;
    double cost = buildNormal (P, world, px, K, huberPx, JtJ, Jtr);
    for (int it = 0; it < maxIterations; ++it)
    {
        bool improved = false;
        for (int tries = 0; tries < 10 && ! improved; ++tries)
        {
            auto A = JtJ;
            for (int d = 0; d < 6; ++d) A[(size_t) (d * 6 + d)] *= (1.0 + lambda);
            for (int d = 0; d < 6; ++d) A[(size_t) (d * 6 + d)] += 1e-12;
            std::vector<double> negJtr (6);
            for (int d = 0; d < 6; ++d) negJtr[(size_t) d] = -Jtr[(size_t) d];
            if (! choleskySolve (A, step, negJtr, 6)) { lambda *= 10.0; continue; }
            const Pose trial = P.perturbed ({ step[0], step[1], step[2] }, { step[3], step[4], step[5] });
            const double c = robustCost (trial, world, px, K, huberPx);
            if (c < cost)
            {
                const double gain = cost - c;
                P = trial;
                lambda = std::max (1e-9, lambda * 0.3);
                improved = true;
                cost = buildNormal (P, world, px, K, huberPx, JtJ, Jtr);
                if (gain < 1e-12 * std::max (1.0, cost)) return reprojectionRms (P, world, px, K);
            }
            else
                lambda *= 10.0;
        }
        if (! improved) break;
    }
    return reprojectionRms (P, world, px, K);
}

Pose mirroredTagPose (const Pose& p) noexcept
{
    const Vec3 v = p.t.normalised();
    const Vec3 n = p.R.col (2);
    const Vec3 n2 = v * (2.0 * n.dot (v)) - n;
    const Vec3 axis = n.cross (n2);
    const double s = axis.norm(), c = n.dot (n2);
    if (s < 1e-9) return p;
    const double ang = std::atan2 (s, c);
    return { expRotation (axis / s * ang) * p.R, p.t };
}

std::vector<PoseCandidate> tagPoseCandidates (const TagObservation& o, double tagSize, const Intrinsics& K)
{
    const auto corners = tagCorners (tagSize);
    std::vector<std::array<double, 2>> src, dst;
    std::vector<Vec3> world;
    std::vector<Pixel> px;
    for (size_t i = 0; i < 4; ++i)
    {
        src.push_back ({ corners[i].x, corners[i].y });
        double x, y;
        K.normalise (o.corners[i].u, o.corners[i].v, x, y);
        dst.push_back ({ x, y });
        world.push_back (corners[i]);
        px.push_back (o.corners[i]);
    }
    std::array<double, 9> H {};
    if (! homographyDlt (src, dst, H)) return {};
    Vec3 h1 { H[0], H[3], H[6] }, h2 { H[1], H[4], H[7] }, h3 { H[2], H[5], H[8] };
    const double n1 = h1.norm(), n2 = h2.norm();
    if (n1 < 1e-15 || n2 < 1e-15) return {};
    double s = 2.0 / (n1 + n2);
    if (h3.z * s < 0.0) s = -s;   // the tag is in front of the camera
    const Vec3 r1 = h1 * s, r2 = h2 * s;
    Pose first { nearestRotation (Mat3::fromColumns (r1, r2, r1.cross (r2))), h3 * s };

    std::vector<PoseCandidate> out;
    PoseCandidate a { first, 0.0 };
    a.rmsPx = refinePose (a.camFromTag, world, px, K, 1e9, 30);
    out.push_back (a);
    PoseCandidate b { mirroredTagPose (a.camFromTag), 0.0 };
    b.rmsPx = refinePose (b.camFromTag, world, px, K, 1e9, 30);
    const bool distinct = rotationAngle (a.camFromTag.R, b.camFromTag.R) > 1e-3
                          || (a.camFromTag.t - b.camFromTag.t).norm() > 1e-4 * a.camFromTag.t.norm();
    if (distinct && b.camFromTag.t.z > 0.0) out.push_back (b);
    std::sort (out.begin(), out.end(), [] (const auto& x, const auto& y) { return x.rmsPx < y.rmsPx; });
    return out;
}

Resection resect (const std::vector<TagObservation>& obs, const std::map<int, Pose>& worldFromTag,
                  double tagSize, const Intrinsics& K, double maxRmsPx)
{
    Resection res;
    const auto corners = tagCorners (tagSize);
    std::vector<const TagObservation*> known;
    for (const auto& o : obs)
        if (worldFromTag.count (o.id) != 0) known.push_back (&o);
    if (known.empty()) { res.problem = "No mapped tag in the picture."; return res; }

    auto gather = [&] (const std::vector<const TagObservation*>& use, std::vector<Vec3>& W, std::vector<Pixel>& P)
    {
        W.clear(); P.clear();
        for (const auto* o : use)
        {
            const auto& T = worldFromTag.at (o->id);
            for (size_t i = 0; i < 4; ++i) { W.push_back (T * corners[i]); P.push_back (o->corners[i]); }
        }
    };

    std::vector<Vec3> W;
    std::vector<Pixel> Px;
    gather (known, W, Px);

    // initial pose: every single-tag candidate, scored on all the corners
    Pose best;
    double bestScore = 1e300;
    for (const auto* o : known)
        for (const auto& c : tagPoseCandidates (*o, tagSize, K))
        {
            const Pose cand = c.camFromTag * worldFromTag.at (o->id).inverse();
            const double sc = medianError (cand, W, Px, K);
            if (sc < bestScore) { bestScore = sc; best = cand; }
        }
    if (bestScore >= 1e300) { res.problem = "The tags' corners are degenerate."; return res; }

    refinePose (best, W, Px, K, 2.0);

    // drop tags that disagree with the rest (moved board, wrong detection), then refine again
    std::vector<double> tagErr;
    for (const auto* o : known)
    {
        const auto& T = worldFromTag.at (o->id);
        std::vector<Vec3> w;
        std::vector<Pixel> p;
        for (size_t i = 0; i < 4; ++i) { w.push_back (T * corners[i]); p.push_back (o->corners[i]); }
        tagErr.push_back (reprojectionRms (best, w, p, K));
    }
    if (known.size() >= 3)
    {
        const double lim = std::max (3.0 * median (tagErr), maxRmsPx);
        std::vector<const TagObservation*> keep;
        for (size_t i = 0; i < known.size(); ++i)
        {
            if (tagErr[i] <= lim) keep.push_back (known[i]);
            else res.rejectedTags.push_back (known[i]->id);
        }
        if (! res.rejectedTags.empty() && keep.size() >= 2)
        {
            known = keep;
            gather (known, W, Px);
            refinePose (best, W, Px, K, 2.0);
        }
        else
            res.rejectedTags.clear();
    }

    res.camFromWorld = best;
    res.rmsPx = reprojectionRms (best, W, Px, K);
    res.tagsUsed = (int) known.size();
    for (const auto* o : known) res.tagIds.push_back (o->id);

    // uncertainty of the camera centre: sigma^2 (J^T J)^-1 mapped to C = -R^T t
    {
        std::vector<double> JtJ, Jtr;
        buildNormal (best, W, Px, K, 1e9, JtJ, Jtr);
        const double dof = std::max (1.0, 2.0 * (double) W.size() - 6.0);
        // rms is per point (two coordinates): SSR = N rms^2, variance = SSR / (2N - 6)
        const double sigma2 = res.rmsPx * res.rmsPx * (double) W.size() / dof;
        std::vector<double> cov (36, 0.0), col;
        bool okCov = true;
        for (int k = 0; k < 6 && okCov; ++k)
        {
            std::vector<double> e (6, 0.0);
            e[(size_t) k] = 1.0;
            okCov = choleskySolve (JtJ, col, e, 6);
            for (int r = 0; r < 6 && okCov; ++r) cov[(size_t) (r * 6 + k)] = col[(size_t) r] * sigma2;
        }
        if (okCov)
        {
            // dC = -R^T [t]x dw - R^T dt  ->  J_C = [ -R^T [t]x , -R^T ]
            const Mat3 Rt = best.R.transposed();
            const Mat3 A = (Rt * skew (best.t)) * -1.0;
            const Mat3 B = Rt * -1.0;
            double var = 0.0;
            for (int r = 0; r < 3; ++r)
            {
                double row[6];
                for (int c = 0; c < 3; ++c) { row[c] = A (r, c); row[c + 3] = B (r, c); }
                for (int a = 0; a < 6; ++a)
                    for (int b = 0; b < 6; ++b) var += row[a] * cov[(size_t) (a * 6 + b)] * row[b];
            }
            res.positionSigma = std::sqrt (std::max (0.0, var));
        }
        else
            res.positionSigma = 1000.0;   // no covariance: never an exact fix
    }

    // One square cannot tell its two planar poses apart at a distance: the
    // mirrored one can fit as well and puts the camera metres away.
    res.ok = res.tagsUsed >= 2 && res.rmsPx <= maxRmsPx && best.t.norm() > 0.0 && res.positionSigma < 1000.0;
    if (res.tagsUsed < 2) res.problem = "Only one mapped tag in the picture.";
    else if (! res.ok) res.problem = "The tags do not agree (error " + std::to_string ((int) std::lround (res.rmsPx)) + " px).";
    return res;
}
} // namespace scm::smap
