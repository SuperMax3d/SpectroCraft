#include "SmTagDetector.h"
#include <JuceHeader.h>

extern "C"
{
#include "apriltag.h"
#include "tag36h11.h"
}

namespace scm::smap
{
GrayImage toGray (const juce::Image& image)
{
    GrayImage g;
    if (! image.isValid()) return g;
    g.width = image.getWidth();
    g.height = image.getHeight();
    g.pixels.resize ((size_t) g.width * (size_t) g.height);
    const juce::Image::BitmapData bd (image, juce::Image::BitmapData::readOnly);
    for (int y = 0; y < g.height; ++y)
        for (int x = 0; x < g.width; ++x)
        {
            const auto c = bd.getPixelColour (x, y);
            const int l = (299 * c.getRed() + 587 * c.getGreen() + 114 * c.getBlue() + 500) / 1000;
            g.at (x, y) = (uint8_t) juce::jlimit (0, 255, l);
        }
    return g;
}

struct TagDetector::Impl
{
    apriltag_family_t* family = nullptr;
    apriltag_detector_t* detector = nullptr;
};

TagDetector::TagDetector (int threads) : impl (std::make_unique<Impl>())
{
    impl->family = tag36h11_create();
    impl->detector = apriltag_detector_create();
    apriltag_detector_add_family_bits (impl->detector, impl->family, 1);
    impl->detector->nthreads = std::max (1, threads);
    impl->detector->quad_sigma = 0.0f;
    impl->detector->refine_edges = true;
    impl->detector->decode_sharpening = 0.25;
}

TagDetector::~TagDetector()
{
    apriltag_detector_destroy (impl->detector);
    tag36h11_destroy (impl->family);
}

std::vector<TagObservation> TagDetector::detect (const GrayImage& g, int maxId) const
{
    std::vector<TagObservation> out;
    if (g.width <= 0 || g.height <= 0) return out;
    // Full resolution up to about 2.5 Mpixel (the far tags are small); above
    // that, quads are found on a half-size picture and the edges refined on
    // the full one.
    impl->detector->quad_decimate = (double) g.width * g.height > 2.6e6 ? 2.0f : 1.0f;

    image_u8_t im { g.width, g.height, g.width, const_cast<uint8_t*> (g.pixels.data()) };
    zarray_t* dets = apriltag_detector_detect (impl->detector, &im);
    for (int i = 0; i < zarray_size (dets); ++i)
    {
        apriltag_detection_t* d = nullptr;
        zarray_get (dets, i, &d);
        if (d->hamming > maxHamming || d->decision_margin < minMargin) continue;
        if (maxId >= 0 && d->id > maxId) continue;
        TagObservation o;
        o.id = d->id;
        o.margin = d->decision_margin;
        o.hamming = d->hamming;
        // AprilTag puts the origin at the first pixel's top-left corner; the
        // camera model (like OpenCV) puts pixel centres on integers
        for (size_t c = 0; c < 4; ++c) o.corners[c] = { d->p[c][0] - 0.5, d->p[c][1] - 0.5 };
        out.push_back (o);
    }
    apriltag_detections_destroy (dets);

    // the same id twice (a reflection, a second print): keep neither, the position would be ambiguous
    std::sort (out.begin(), out.end(), [] (const auto& a, const auto& b) { return a.id < b.id; });
    std::vector<TagObservation> unique;
    for (size_t i = 0; i < out.size(); ++i)
    {
        const bool dupPrev = i > 0 && out[i - 1].id == out[i].id;
        const bool dupNext = i + 1 < out.size() && out[i + 1].id == out[i].id;
        if (! dupPrev && ! dupNext) unique.push_back (out[i]);
    }
    return unique;
}

int TagDetector::numIds()
{
    apriltag_family_t* f = tag36h11_create();
    const int n = (int) f->ncodes;
    tag36h11_destroy (f);
    return n;
}

std::vector<bool> TagDetector::tagCells (int id)
{
    std::vector<bool> cells;
    apriltag_family_t* f = tag36h11_create();
    if (id >= 0 && (uint32_t) id < f->ncodes)
    {
        image_u8_t* im = apriltag_to_image (f, (uint32_t) id);
        cells.resize ((size_t) (im->width * im->height));
        for (int y = 0; y < im->height; ++y)
            for (int x = 0; x < im->width; ++x)
                cells[(size_t) (y * im->width + x)] = im->buf[y * im->stride + x] == 0;
        image_u8_destroy (im);
    }
    tag36h11_destroy (f);
    return cells;
}
} // namespace scm::smap
