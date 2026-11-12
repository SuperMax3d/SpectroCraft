/**
 * SpectroCraft - Video picture scaler core (Lanczos / Bicubic / Nearest)
 *
 * The Video window shows a decoded frame scaled to the window, often
 * magnified (zoom) and possibly turned by quarter turns. This resamples the
 * part of the frame that is on screen straight to physical display pixels:
 *
 *   - Lanczos (default): windowed sinc, a = 3, so 6 taps per axis when
 *     enlarging - the kernel of FFmpeg's swscale SWS_LANCZOS with its
 *     default parameter;
 *   - Bicubic: cubic convolution with A = -0.6 (4 taps), the default of
 *     swscale's SWS_BICUBIC (B = 0, C = 0.6);
 *   - Nearest: each screen pixel shows the nearest source pixel (the raw
 *     pixels, blocky when enlarged).
 *
 * Like swscale, when the picture is shrunk the kernel is stretched by the
 * shrink factor (it then averages neighbouring pixels instead of aliasing),
 * and the weights of every output pixel are normalised to a sum of one.
 *
 * Kept cheap enough for every frame of a playing video:
 *   - separable: a horizontal pass, then a vertical pass;
 *   - the weights are computed once per geometry and reused for every frame
 *     of a playing video (only zooming, panning or resizing recomputes them);
 *   - only the visible part of the picture is processed, at exactly the
 *     number of physical pixels it covers, so drawing the result is a 1:1
 *     copy;
 *   - the output is split into bands of rows run in parallel on a few worker
 *     threads; the inner loops work on whole pixels with 4-wide SIMD (SSE2 on
 *     x64, NEON on arm64, plain floats elsewhere);
 *   - plain C++ without JUCE, so src/videoscale/CMakeLists.txt can compile it
 *     optimised in Debug builds too (Debug is ~70x slower otherwise).
 *
 * The JUCE side (juce::Image in and out, menu names) is VideoScaler.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace sc {

/** Picture scaling method of the Video window (Upscaling submenu). */
enum class VideoUpscaling { lanczos = 0, bicubic = 1, nearest = 2 };

/** The 1-D kernel of a method at distance x, in source pixels at 1:1 (tests). */
double videoScalerKernel (VideoUpscaling m, double x);

namespace vscale {

/** Pixels of 4 bytes (byte 3 = alpha, premultiplied), rows lineStride bytes
    apart (negative strides are fine). */
struct ConstPixels
{
    const uint8_t* data = nullptr;
    std::ptrdiff_t lineStride = 0;
    int width = 0, height = 0;
};

struct Pixels
{
    uint8_t* data = nullptr;
    std::ptrdiff_t lineStride = 0;
    int width = 0, height = 0;
};

/** The scaler: weight tables, worker threads and scratch memory, reused from
    one frame to the next. One thread at a time (the Video window paints on
    the message thread). */
class Resampler
{
public:
    Resampler();
    ~Resampler();

    /** Renders into dst the part of src seen through the area (x, y, w, h).
        The area is in pixels of the picture as displayed, that is after
        turning src clockwise by `rotation` degrees (0, 90, 180 or 270): x
        along the displayed width, y downwards. It may reach past the
        picture: the edge pixels repeat. False for invalid input. */
    bool render (const ConstPixels& src, int rotation,
                 double areaX, double areaY, double areaW, double areaH,
                 const Pixels& dst, VideoUpscaling method);

    /** Worker threads used on top of the calling thread (default: up to 3,
        one less than the number of cores). 0 = everything on the caller. */
    void setMaxWorkerThreads (int n);

    /** Weight tables rebuilt so far (tests: the same geometry reuses them). */
    int weightTableBuilds() const noexcept;

    struct Axis;      // per-axis filter weights
    class Pool;       // worker threads

private:
    std::unique_ptr<Axis> ax_, ay_;
    std::unique_ptr<Pool> pool_;
    int maxWorkers_ = -1;                        // -1 = not decided yet
    std::vector<std::vector<float>> scratch_;    // per thread (index 0 = caller)
    int tableBuilds_ = 0;

    Resampler (const Resampler&) = delete;
    Resampler& operator= (const Resampler&) = delete;
};

} // namespace vscale
} // namespace sc
