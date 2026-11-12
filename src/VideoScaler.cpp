/**
 * SpectroCraft - Picture scaling for the Video window (see include/VideoScaler.h)
 *
 * The JUCE side only: the scaling itself is VideoScalerCore (optimised in
 * every build configuration).
 */
#include "VideoScaler.h"

namespace sc {

juce::String videoUpscalingName (VideoUpscaling m)
{
    switch (m)
    {
        case VideoUpscaling::bicubic: return "Bicubic";
        case VideoUpscaling::nearest: return "Nearest";
        case VideoUpscaling::lanczos: break;
    }
    return "Lanczos";
}

VideoUpscaling videoUpscalingFromName (const juce::String& s)
{
    if (s.equalsIgnoreCase ("Bicubic")) return VideoUpscaling::bicubic;
    if (s.equalsIgnoreCase ("Nearest")) return VideoUpscaling::nearest;
    return VideoUpscaling::lanczos;
}

bool VideoScaler::render (const juce::Image& srcIn, int rotation, juce::Rectangle<double> area,
                          int dstW, int dstH, VideoUpscaling method, juce::Image& dst)
{
    if (!srcIn.isValid() || dstW <= 0 || dstH <= 0
        || (int64_t) dstW * (int64_t) dstH > (int64_t) 16384 * 16384
        || !std::isfinite (area.getX()) || !std::isfinite (area.getY())
        || !(area.getWidth() > 0.0) || !(area.getHeight() > 0.0)
        || !std::isfinite (area.getWidth()) || !std::isfinite (area.getHeight()))
        return false;

    // The decoders deliver ARGB; anything else (tests, odd callers) is
    // converted once here.
    const juce::Image src = srcIn.getFormat() == juce::Image::ARGB
                                ? srcIn : srcIn.convertedToFormat (juce::Image::ARGB);
    const juce::Image::BitmapData sd (src, juce::Image::BitmapData::readOnly);
    if (sd.pixelStride != 4) return false;

    if (!(dst.isValid() && dst.getFormat() == juce::Image::ARGB
          && dst.getWidth() == dstW && dst.getHeight() == dstH))
        dst = juce::Image (juce::Image::ARGB, dstW, dstH, false);
    const juce::Image::BitmapData dd (dst, juce::Image::BitmapData::writeOnly);
    if (dd.pixelStride != 4) return false;

    return core_.render ({ sd.data, (std::ptrdiff_t) sd.lineStride, sd.width, sd.height }, rotation,
                         area.getX(), area.getY(), area.getWidth(), area.getHeight(),
                         { dd.data, (std::ptrdiff_t) dd.lineStride, dstW, dstH }, method);
}

} // namespace sc
