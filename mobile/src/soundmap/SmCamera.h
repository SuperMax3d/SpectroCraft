// SpectroCraft Mobile - sound map: the camera model, tag observations and the
// phone's body frame.
//
// The lens is shared by every photo of a map: focal length relative to the
// picture's long side (so it survives rotated pictures and other resolutions),
// two radial distortion terms on normalised coordinates, and the principal
// point's offset from the centre. JUCE hands the stills over upright for the
// UI's orientation (vendored JUCE patched on iOS 17+), so the sensor's offset
// turns with the way the phone is held: it is stored in one reference hold and
// rotated per picture. Pixel centres are on integer coordinates (OpenCV's
// convention).
#pragma once
#include "SmMath.h"
#include <array>
#include <vector>

namespace scm::smap
{
/** How the phone was held when the picture was taken (the picture is upright
    in that orientation). Body frame: x right and y up seen from the screen in
    portrait, z out of the screen; the back camera looks along -z. */
enum class Hold
{
    Portrait = 0,
    PortraitUpsideDown = 1,
    LandscapeTopLeft = 2,    // the phone's top (earpiece) to the left: JUCE's rotatedAntiClockwise
    LandscapeTopRight = 3,   // the phone's top to the right: JUCE's rotatedClockwise
};

/** A body-frame vector in the camera frame of an upright picture taken
    holding the phone as `hold`. */
inline Vec3 bodyToCamera (const Vec3& b, Hold hold) noexcept
{
    // The camera's z is the back camera's axis, -body z, in every hold.
    switch (hold)
    {
        case Hold::Portrait:           return { b.x, -b.y, -b.z };
        case Hold::PortraitUpsideDown: return { -b.x, b.y, -b.z };
        case Hold::LandscapeTopLeft:   return { -b.y, -b.x, -b.z };   // picture up = body +x
        case Hold::LandscapeTopRight:  return { b.y, b.x, -b.z };     // picture up = body -x
    }
    return { b.x, -b.y, -b.z };
}

inline Hold holdFromInt (int h) noexcept
{
    return (Hold) std::clamp (h, 0, 3);
}

/** A picture-plane vector given in the reference hold's picture (landscape,
    top to the left) as seen in a picture held as `hold`. */
inline void rotatePictureVector (double x, double y, Hold hold, double& outX, double& outY) noexcept
{
    // camera(hold) = M_hold * body and camera(ref) = M_ref * body, so
    // camera(hold) = M_hold * M_ref^T * camera(ref)
    const Vec3 body = { -y, -x, 0.0 };   // M_ref^T * (x, y, 0): inverse of LandscapeTopLeft's map
    const Vec3 c = bodyToCamera (body, hold);
    outX = c.x;
    outY = c.y;
}

struct Lens
{
    double fRel = 0.75;              // focal length / long side of the picture
    double k1 = 0.0, k2 = 0.0;       // radial distortion (normalised coordinates)
    double ppx = 0.0, ppy = 0.0;     // principal point offset from the centre / long side,
                                     // in the reference hold's picture (landscape, top left)
};

struct Intrinsics
{
    int width = 0, height = 0;
    double f = 0.0, cx = 0.0, cy = 0.0, k1 = 0.0, k2 = 0.0;

    static Intrinsics forPicture (const Lens& lens, int w, int h, Hold hold = Hold::LandscapeTopLeft) noexcept
    {
        Intrinsics k;
        k.width = w; k.height = h;
        const double longSide = (double) std::max (w, h);
        k.f = lens.fRel * longSide;
        double dx = 0.0, dy = 0.0;
        rotatePictureVector (lens.ppx, lens.ppy, hold, dx, dy);
        k.cx = 0.5 * (double) (w - 1) + dx * longSide;   // pixel centres on integers
        k.cy = 0.5 * (double) (h - 1) + dy * longSide;
        k.k1 = lens.k1; k.k2 = lens.k2;
        return k;
    }

    /** Camera-frame point -> pixel. False behind the camera (or too close to the plane z = 0). */
    bool project (const Vec3& pc, double& u, double& v) const noexcept
    {
        if (pc.z < 1e-6) return false;
        const double x = pc.x / pc.z, y = pc.y / pc.z;
        const double r2 = x * x + y * y;
        const double d = 1.0 + k1 * r2 + k2 * r2 * r2;
        u = f * x * d + cx;
        v = f * y * d + cy;
        return true;
    }

    /** Pixel -> undistorted normalised coordinates (x / z, y / z). */
    void normalise (double u, double v, double& x, double& y) const noexcept
    {
        const double xd = (u - cx) / f, yd = (v - cy) / f;
        x = xd; y = yd;
        for (int i = 0; i < 20; ++i)
        {
            const double r2 = x * x + y * y;
            const double d = 1.0 + k1 * r2 + k2 * r2 * r2;
            if (d <= 0.1) break;
            x = xd / d; y = yd / d;
        }
    }
};

struct Pixel
{
    double u = 0.0, v = 0.0;
};

/** One tag found in a picture. Corners as printed: 0 bottom-left, 1
    bottom-right, 2 top-right, 3 top-left (AprilTag's order, apriltag.c). */
struct TagObservation
{
    int id = -1;
    std::array<Pixel, 4> corners {};
    double margin = 0.0;   // the detector's decision margin
    int hamming = 0;       // corrected bits

    /** Length of the longest side, in pixels. */
    double sizePx() const noexcept
    {
        double m = 0.0;
        for (int i = 0; i < 4; ++i)
        {
            const auto& a = corners[(size_t) i];
            const auto& b = corners[(size_t) ((i + 1) % 4)];
            m = std::max (m, std::hypot (a.u - b.u, a.v - b.v));
        }
        return m;
    }
};

/** The corners of a tag of black-square side `size`, in the tag frame (same order as TagObservation). */
inline std::array<Vec3, 4> tagCorners (double size) noexcept
{
    const double h = 0.5 * size;
    return { Vec3 { -h, -h, 0.0 }, Vec3 { h, -h, 0.0 }, Vec3 { h, h, 0.0 }, Vec3 { -h, h, 0.0 } };
}

} // namespace scm::smap
