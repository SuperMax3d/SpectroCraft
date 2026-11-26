// SpectroCraft Mobile - sound map: values between the measured points.
//
// Gaussian-weighted mean of the points' mean-square pressure (the values are
// levels in dB, averaged as energy and converted back: a local spatial average
// of the level, which is what an undersampled interference pattern still
// supports), each point with its own width (the distance to its third nearest
// neighbour, so dense parts stay sharp). A cell farther than `maskFactor`
// widths from every point stays empty: the map never extends the data beyond
// where it was measured.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace scm::smap
{
struct Grid
{
    double x0 = 0.0, y0 = 0.0;   // centre of cell (0, 0)
    double cell = 0.05;          // metres
    int nx = 0, ny = 0;
    std::vector<float> values;   // row-major, iy * nx + ix
    std::vector<uint8_t> valid;

    double xAt (int ix) const noexcept { return x0 + cell * ix; }
    double yAt (int iy) const noexcept { return y0 + cell * iy; }
};

/** Median distance from each point to its nearest neighbour (0 with < 2 points). */
double typicalSpacing (const std::vector<std::array<double, 2>>& xy);

/** Each point's width: the distance to its k-th nearest neighbour, at least `minWidth`. */
std::vector<double> adaptiveWidths (const std::vector<std::array<double, 2>>& xy, int k = 3, double minWidth = 0.15);

/** Fills `grid` (its geometry set by the caller) from levels in dB. */
void interpolate (Grid& grid, const std::vector<std::array<double, 2>>& xy, const std::vector<double>& levelsDb,
                  const std::vector<double>& widths, double maskFactor = 1.5);

/** Energy mean (dB) of the grid's filled cells: the map's reference for relative levels. */
double gridEnergyMeanDb (const Grid& grid);
} // namespace scm::smap
