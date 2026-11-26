#include "SmInterp.h"
#include <algorithm>
#include <cmath>

namespace scm::smap
{
double typicalSpacing (const std::vector<std::array<double, 2>>& xy)
{
    if (xy.size() < 2) return 0.0;
    std::vector<double> nn;
    for (size_t i = 0; i < xy.size(); ++i)
    {
        double best = 1e300;
        for (size_t j = 0; j < xy.size(); ++j)
            if (i != j) best = std::min (best, std::hypot (xy[i][0] - xy[j][0], xy[i][1] - xy[j][1]));
        nn.push_back (best);
    }
    std::nth_element (nn.begin(), nn.begin() + (long) (nn.size() / 2), nn.end());
    return nn[nn.size() / 2];
}

std::vector<double> adaptiveWidths (const std::vector<std::array<double, 2>>& xy, int k, double minWidth)
{
    std::vector<double> w (xy.size(), minWidth);
    for (size_t i = 0; i < xy.size(); ++i)
    {
        std::vector<double> d;
        for (size_t j = 0; j < xy.size(); ++j)
            if (i != j) d.push_back (std::hypot (xy[i][0] - xy[j][0], xy[i][1] - xy[j][1]));
        if (d.empty()) { w[i] = std::max (minWidth, 0.5); continue; }
        const size_t kk = std::min (d.size(), (size_t) std::max (1, k)) - 1;
        std::nth_element (d.begin(), d.begin() + (long) kk, d.end());
        w[i] = std::max (minWidth, d[kk]);
    }
    return w;
}

void interpolate (Grid& g, const std::vector<std::array<double, 2>>& xy, const std::vector<double>& levelsDb,
                  const std::vector<double>& widths, double maskFactor)
{
    const size_t cells = (size_t) std::max (0, g.nx) * (size_t) std::max (0, g.ny);
    g.values.assign (cells, 0.0f);
    g.valid.assign (cells, 0);
    if (xy.empty() || xy.size() != levelsDb.size() || widths.size() != xy.size()) return;
    // energies relative to the loudest point, to stay in range
    const double ref = *std::max_element (levelsDb.begin(), levelsDb.end());
    std::vector<double> e (xy.size()), inv2h2 (xy.size());
    for (size_t k = 0; k < xy.size(); ++k)
    {
        e[k] = std::pow (10.0, (levelsDb[k] - ref) / 10.0);
        inv2h2[k] = 1.0 / (2.0 * widths[k] * widths[k]);
    }
    for (int iy = 0; iy < g.ny; ++iy)
        for (int ix = 0; ix < g.nx; ++ix)
        {
            const double x = g.xAt (ix), y = g.yAt (iy);
            double wSum = 0.0, eSum = 0.0, nearest = 1e300;
            for (size_t k = 0; k < xy.size(); ++k)
            {
                const double dx = x - xy[k][0], dy = y - xy[k][1];
                const double d2 = dx * dx + dy * dy;
                nearest = std::min (nearest, std::sqrt (d2) / widths[k]);
                const double w = std::exp (-d2 * inv2h2[k]);
                wSum += w;
                eSum += w * e[k];
            }
            if (nearest > maskFactor || wSum < 1e-12 || eSum <= 0.0) continue;
            const size_t i = (size_t) iy * (size_t) g.nx + (size_t) ix;
            g.values[i] = (float) (ref + 10.0 * std::log10 (eSum / wSum));
            g.valid[i] = 1;
        }
}

double gridEnergyMeanDb (const Grid& g)
{
    double s = 0.0, ref = -1e300;
    size_t n = 0;
    for (size_t i = 0; i < g.values.size(); ++i)
        if (g.valid[i]) ref = std::max (ref, (double) g.values[i]);
    for (size_t i = 0; i < g.values.size(); ++i)
        if (g.valid[i]) { s += std::pow (10.0, ((double) g.values[i] - ref) / 10.0); ++n; }
    return n > 0 ? ref + 10.0 * std::log10 (s / (double) n) : 0.0;
}
} // namespace scm::smap
