/**
 * SpectroCraft - Picture scaling for the Video window (JUCE side)
 *
 * juce::Image in and out around the scaler core (VideoScalerCore.h: the
 * methods, their kernels and why it is fast), plus the menu / settings names
 * of the methods.
 */
#pragma once

#include <JuceHeader.h>

#include "VideoScalerCore.h"

namespace sc {

/** Menu and settings name: "Lanczos", "Bicubic" or "Nearest". */
juce::String videoUpscalingName (VideoUpscaling m);
/** Inverse of videoUpscalingName (case-insensitive); anything else = Lanczos. */
VideoUpscaling videoUpscalingFromName (const juce::String& s);

class VideoScaler
{
public:
    VideoScaler() = default;

    /** Renders into `dst` (dstW x dstH ARGB; reused when it already has that
        size, reallocated otherwise) the part of `src` seen through
        `srcArea`. srcArea is in pixels of the picture as displayed, that is
        after turning `src` clockwise by `rotation` degrees (0, 90, 180 or
        270): x along the displayed width, y downwards. It may reach past the
        picture: the edge pixels repeat. Returns false (dst untouched) for an
        invalid source, area or size. */
    bool render (const juce::Image& src, int rotation, juce::Rectangle<double> srcArea,
                 int dstW, int dstH, VideoUpscaling method, juce::Image& dst);

    /** Worker threads used on top of the calling thread (default: up to 3,
        one less than the number of cores). 0 = everything on the caller. */
    void setMaxWorkerThreads (int n) { core_.setMaxWorkerThreads (n); }

    /** Weight tables rebuilt so far (tests: the same geometry reuses them). */
    int weightTableBuilds() const noexcept { return core_.weightTableBuilds(); }

private:
    vscale::Resampler core_;

    JUCE_DECLARE_NON_COPYABLE (VideoScaler)
};

} // namespace sc
