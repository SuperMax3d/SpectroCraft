// SpectroCraft Mobile - sound map: AprilTag 36h11 detection (the vendored
// AprilRobotics library, extern/apriltag).
//
// One detector per thread (the library's detector is not reentrant). Only
// detections with at most one corrected bit and a clear decision margin are
// kept: a false tag would place the phone somewhere else entirely.
#pragma once
#include "SmCamera.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace juce { class Image; }

namespace scm::smap
{
struct GrayImage
{
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;   // row-major, stride = width

    uint8_t& at (int x, int y) noexcept { return pixels[(size_t) y * (size_t) width + (size_t) x]; }
    uint8_t at (int x, int y) const noexcept { return pixels[(size_t) y * (size_t) width + (size_t) x]; }
};

/** Luma of a JUCE image (any pixel format). */
GrayImage toGray (const juce::Image& image);

class TagDetector
{
public:
    explicit TagDetector (int threads = 2);
    ~TagDetector();

    /** Tags in the picture; ids above maxId are ignored (-1: any id). */
    std::vector<TagObservation> detect (const GrayImage& image, int maxId = -1) const;

    double minMargin = 20.0;
    int maxHamming = 1;

    /** The 10 x 10 cells of a tag (the white quiet zone included), row 0 at the
        top of the printed tag: true = black. Empty for an id out of range. */
    static std::vector<bool> tagCells (int id);
    static int numIds();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace scm::smap
