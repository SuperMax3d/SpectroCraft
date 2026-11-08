/**
 * SpectroCraft - Video window (see include/VideoWindow.h)
 */
#include <JuceHeader.h>

#include "VideoWindow.h"

//==============================================================================
void applyImageAdjustments (juce::Image& image, double contrast, double brightness)
{
    if (!image.isValid()) return;
    const bool neutral = std::abs (contrast - 1.0) < 1e-6 && std::abs (brightness) < 1e-6;
    if (neutral) return;

    juce::Image::BitmapData bd (image, juce::Image::BitmapData::readWrite);
    const double off = brightness * 255.0;
    auto adjust = [contrast, off] (int v)
    {
        // Brightness first, then contrast around mid-grey.
        return (uint8) std::clamp (128.0 + ((double) v + off - 128.0) * contrast, 0.0, 255.0);
    };

    if (bd.pixelFormat == juce::Image::ARGB && bd.pixelStride == 4)
    {
        for (int y = 0; y < bd.height; ++y)
            for (int x = 0; x < bd.width; ++x)
            {
                auto* p = bd.getPixelPointer (x, y);   // B,G,R,A in memory
                for (int c = 0; c < 3; ++c) p[c] = adjust (p[c]);
            }
        return;
    }

    // Other formats: per-pixel access (slow, but the pump's frames are ARGB).
    for (int y = 0; y < bd.height; ++y)
        for (int x = 0; x < bd.width; ++x)
        {
            const auto col = bd.getPixelColour (x, y);
            bd.setPixelColour (x, y, juce::Colour ((uint8) adjust (col.getRed()),
                                                   (uint8) adjust (col.getGreen()),
                                                   (uint8) adjust (col.getBlue())));
        }
}

//==============================================================================
VideoView::VideoView()
{
    setOpaque (true);
    addAndMakeVisible (zoomBar_);
    zoomBar_.onChange = [this] (double lo, double hi) { applyZoomFromBar (lo, hi); };
}

void VideoView::setMessage (const juce::String& m)
{
    if (message_ == m) return;
    message_ = m;
    repaint();
}

void VideoView::setNotice (const juce::String& n)
{
    if (notice_ == n) return;
    notice_ = n;
    repaint();
}

void VideoView::clearFrame()
{
    // A new picture starts whole: the zoom/pan of the previous file is gone.
    frame_ = {};
    zoom_ = 1.0f;
    pan_ = {};
    syncZoomBar();
    repaint();
}

void VideoView::setUserRotation (int degrees)
{
    userRotation_ = (((degrees / 90) * 90) % 360 + 360) % 360;
    clampPan();   // the picture's aspect turned: the pan limits changed
    syncZoomBar();
    repaint();
}

void VideoView::setAdjustments (float zoomIn, juce::Point<float> panIn)
{
    // Restores a saved per-track state without firing onAdjustmentChanged.
    // Works before the first frame arrives: clampPan() runs once the picture
    // is in (setFrame), so an out-of-range pan stored here gets fixed then.
    zoom_ = (float) std::clamp ((double) zoomIn, 1.0, (double) kMaxZoom);
    pan_ = panIn;
    if (frame_.image.isValid()) { clampPan(); syncZoomBar(); }
    repaint();
}

int VideoView::neededLongSide() const
{
    return (int) std::ceil ((float) std::max (getWidth(), getHeight()) * zoom_);
}

void VideoView::setFrame (const sc::VideoFrame& frame, int trackRotationDeg)
{
    frame_ = frame;
    trackRot_ = ((trackRotationDeg % 360) + 360) % 360;

    clampPan();
    syncZoomBar();
    repaint();
}

juce::Rectangle<float> VideoView::fittedBounds() const
{
    if (!frame_.image.isValid()) return {};
    const int rot = totalRotation();
    float w = (float) frame_.image.getWidth(), h = (float) frame_.image.getHeight();
    if (rot == 90 || rot == 270) std::swap (w, h);

    // Fit the picture into the plot area (the view minus the zoom/pan bar).
    const auto area = plotArea();
    const float scale = std::min (area.getWidth() / w, area.getHeight() / h);
    return { (area.getX() + area.getWidth() - w * scale) / 2.0f,
             (area.getY() + area.getHeight() - h * scale) / 2.0f,
             w * scale, h * scale };
}

juce::Rectangle<float> VideoView::pictureBounds() const
{
    auto F = fittedBounds();
    if (F.isEmpty()) return {};
    // The picture is the fitted rectangle scaled about its centre, shifted by pan_.
    const auto c = F.getCentre();
    juce::Rectangle<float> P { 0.0f, 0.0f, F.getWidth() * zoom_, F.getHeight() * zoom_ };
    return P.withCentre (c + pan_);
}

void VideoView::clampPan()
{
    const auto P = pictureBounds();
    if (P.isEmpty()) { pan_ = {}; return; }
    const auto A = plotArea();
    const float maxX = std::max (0.0f, (P.getWidth() - A.getWidth()) / 2.0f);
    const float maxY = std::max (0.0f, (P.getHeight() - A.getHeight()) / 2.0f);
    pan_.x = (float) std::clamp ((double) pan_.x, -(double) maxX, (double) maxX);
    pan_.y = (float) std::clamp ((double) pan_.y, -(double) maxY, (double) maxY);
}

void VideoView::setPan (juce::Point<float> p)
{
    if (!frame_.image.isValid()) return;
    pan_ = p;
    clampPan();
    syncZoomBar();
    repaint();
    if (onAdjustmentChanged) onAdjustmentChanged ();
}

void VideoView::zoomIn()  { zoomAt (plotArea().getCentre(), kZoomStep); }
void VideoView::zoomOut() { zoomAt (plotArea().getCentre(), 1.0f / kZoomStep); }

void VideoView::resetZoom()
{
    zoom_ = 1.0f;
    pan_ = {};
    syncZoomBar();
    repaint();
    if (onAdjustmentChanged) onAdjustmentChanged ();
}

void VideoView::zoomAt (juce::Point<float> at, float factor)
{
    const auto P = pictureBounds();
    if (P.isEmpty()) return;
    const float newZ = (float) std::clamp ((double) zoom_ * factor, 1.0, (double) kMaxZoom);
    if (newZ == zoom_) { syncZoomBar(); return; }

    // Keep the image point under `at` fixed: pan so that fitted point maps to at.
    const auto F = fittedBounds();
    const juce::Point<float> imgPt = (at - P.getTopLeft()) / zoom_;
    pan_ = { (float) (at.x - imgPt.x * newZ + 0.5 * F.getWidth() * newZ - F.getCentreX()),
             (float) (at.y - imgPt.y * newZ + 0.5 * F.getHeight() * newZ - F.getCentreY()) };
    zoom_ = newZ;
    clampPan();
    syncZoomBar();
    repaint();
    if (onAdjustmentChanged) onAdjustmentChanged ();
}

void VideoView::zoomToRegion (const juce::Rectangle<float>& viewRect)
{
    const auto A = plotArea();
    const auto P = pictureBounds();
    if (P.isEmpty() || viewRect.isEmpty()) return;

    // The dragged box in fitted-image coordinates.
    const juce::Rectangle<float> fr = { (viewRect.getX() - P.getX()) / zoom_,
                                        (viewRect.getY() - P.getY()) / zoom_,
                                        viewRect.getWidth() / zoom_,
                                        viewRect.getHeight() / zoom_ };
    const auto F = fittedBounds();

    // Uniform zoom so the whole region is visible, centred in the plot area.
    const double zD = std::min ((double) kMaxZoom,
                                std::min ((double) A.getWidth() / fr.getWidth(),
                                         (double) A.getHeight() / fr.getHeight()));
    float z = (float) zD;
    pan_ = { (float) (A.getCentreX() - fr.getCentreX() * z + 0.5 * F.getWidth() * z - F.getCentreX()),
             (float) (A.getCentreY() - fr.getCentreY() * z + 0.5 * F.getHeight() * z - F.getCentreY()) };
    zoom_ = z;
    clampPan();
    syncZoomBar();
    repaint();
    if (onAdjustmentChanged) onAdjustmentChanged ();
}

void VideoView::rotateClockwise()
{
    if (!frame_.image.isValid()) return;
    userRotation_ = (userRotation_ + 90) % 360;
    if (onUserRotationChanged) onUserRotationChanged (userRotation_);
    clampPan();
    syncZoomBar();
    repaint();
}

void VideoView::rotateCounterClockwise()
{
    if (!frame_.image.isValid()) return;
    userRotation_ = ((userRotation_ - 90) % 360 + 360) % 360;
    if (onUserRotationChanged) onUserRotationChanged (userRotation_);
    clampPan();
    syncZoomBar();
    repaint();
}

void VideoView::syncZoomBar()
{
    const auto F = fittedBounds();
    if (F.isEmpty())
    {
        zoomBar_.setVisible (false);
        barRangeH_ = -1.0;
        return;
    }
    zoomBar_.setVisible (true);

    const double fh = (double) F.getHeight();
    if (fh != barRangeH_)   // setRange repaints: only when the frame size changed
    {
        barRangeH_ = fh;
        zoomBar_.setRange (0.0, fh);
    }

    // The visible part of the picture height in fitted coordinates.
    const auto P = pictureBounds();
    const auto A = plotArea();
    const double top = std::clamp (double((A.getY() - P.getY()) / zoom_), 0.0, fh);
    const double bot = std::clamp (double((A.getBottom() - P.getY()) / zoom_), 0.0, fh);
    zoomBar_.setWindow (top, bot);
}

void VideoView::applyZoomFromBar (double lo, double hi)
{
    const auto A = plotArea();
    const auto F = fittedBounds();
    if (F.isEmpty() || hi <= lo) return;

    // The bar window is the visible part of the picture height: its size sets
    // the zoom, its centre stays at the middle of the plot area.
    zoom_ = (float) std::min ((double) A.getHeight() / (hi - lo), (double) kMaxZoom);
    const double mid = 0.5 * (lo + hi);
    pan_.y = (float) (A.getCentreY() - mid * zoom_ - F.getCentreY() + 0.5 * F.getHeight() * zoom_);
    clampPan();
    syncZoomBar();
    repaint();
    if (onAdjustmentChanged) onAdjustmentChanged ();
}

void VideoView::handleContextMenuId (int id)
{
    switch (id)
    {
        case kMenuZoomIn:     zoomIn(); break;
        case kMenuZoomOut:    zoomOut(); break;
        case kMenuZoomFit:    resetZoom(); break;
        case kMenuRotateCW:   rotateClockwise(); break;
        case kMenuRotateCCW:  rotateCounterClockwise(); break;
        case kMenuAlwaysOnTop:
            if (onToggleAlwaysOnTop)
                onToggleAlwaysOnTop (!(isAlwaysOnTop ? isAlwaysOnTop() : false));
            break;
        case kMenuDock:
            if (onDockToggle) onDockToggle ();
            break;
        default: break;
    }
}

void VideoView::showContextMenu (const juce::Point<int>& at)
{
    // Headless test views have no owner callbacks and must not enter a modal loop.
    if (!onDockToggle && !onToggleAlwaysOnTop) return;

    juce::PopupMenu m;
    m.addItem (kMenuZoomIn, "Zoom In");
    m.addItem (kMenuZoomOut, "Zoom Out");
    m.addItem (kMenuZoomFit, "Zoom Fit");
    m.addSeparator();
    // fromUTF8: plain const char* literals are treated as ASCII by juce::String,
    // which would mangle the UTF-8 degree sign (stray 'A').
    m.addItem (kMenuRotateCW, juce::String::fromUTF8 ("Rotate 90\u00B0 Clockwise"));
    m.addItem (kMenuRotateCCW, juce::String::fromUTF8 ("Rotate 90\u00B0 Counter-Clockwise"));
    if (onToggleAlwaysOnTop)
        m.addItem (kMenuAlwaysOnTop, "Always On Top", true, isAlwaysOnTop ? isAlwaysOnTop() : false);
    if (onDockToggle)
        m.addItem (kMenuDock, docked ? "Undock to Window" : "Dock Panel");

    const int id = m.showMenu (juce::PopupMenu::Options ()
                                   .withTargetComponent (this)
                                   .withTargetScreenArea (juce::Rectangle<int> {}.withPosition (localPointToGlobal (at))));
    if (id != 0) handleContextMenuId (id);
}

void VideoView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    if (frame_.image.isValid())
    {
        const auto r = pictureBounds();
        const int rot = totalRotation();
        const float iw = (float) frame_.image.getWidth(), ih = (float) frame_.image.getHeight();
        const bool quarter = rot == 90 || rot == 270;
        const float s = r.getWidth() / (quarter ? ih : iw);
        // Draw the image unrotated, then rotate/scale/translate it into place.
        const auto t = juce::AffineTransform::translation (-iw * 0.5f, -ih * 0.5f)
                           .rotated ((float) rot * juce::MathConstants<float>::pi / 180.0f)
                           .scaled (s)
                           .translated (r.getCentreX(), r.getCentreY());
        g.setImageResamplingQuality (zoom_ > 2.0f ? juce::Graphics::lowResamplingQuality
                                                 : juce::Graphics::mediumResamplingQuality);
        g.drawImageTransformed (frame_.image, t, false);
    }
    else if (!message_.isEmpty())
    {
        // No picture at all: a centred placeholder ("No video for this
        // document", decoder errors). Never drawn over an actual frame.
        g.setColour (juce::Colours::lightgrey);
        g.setFont (14.0f);
        g.drawFittedText (message_, 24, 8, getWidth() - 48, getHeight() - 16,
                          juce::Justification::centred, 3);
    }

    // Right-drag box-zoom rubber band: the same translucent blue as the time
    // selection in the tracks.
    if (rightDrag_)
    {
        const auto zr = juce::Rectangle<float> (rightStart_, rightNow_).toNearestInt();
        g.setColour (juce::Colour::fromRGBA (70, 130, 255, 80));
        g.fillRect (zr);
        g.setColour (juce::Colour::fromRGB (96, 150, 255).withAlpha (0.9f));
        g.drawRect (zr, 1);
    }
}

void VideoView::resized()
{
    zoomBar_.setBounds (getWidth() - kBarWidth, 0, kBarWidth, getHeight());
    clampPan();
    if (onDisplaySizeChanged) onDisplaySizeChanged ();
}

void VideoView::mouseDown (const juce::MouseEvent& e)
{
    if (!frame_.image.isValid()) return;

    // Right button: box-zoom drag. A plain right-click (no significant
    // movement) still shows the context menu, deferred to mouseUp.
    if (e.mods.isRightButtonDown() && !e.mods.isLeftButtonDown())
    {
        rightDrag_ = true;
        rightStart_ = rightNow_ = e.getPosition().toFloat();
        return;
    }

    dragStart_ = e.getPosition().toFloat();
}

void VideoView::mouseDrag (const juce::MouseEvent& e)
{
    if (rightDrag_)
    {
        rightNow_ = e.getPosition().toFloat();
        repaint();
        return;
    }
    if (frame_.image.isValid() && dragStart_ != juce::Point<float>())
        setPan (pan_ + (e.getPosition().toFloat() - dragStart_));
}

void VideoView::mouseUp (const juce::MouseEvent& e)
{
    if (rightDrag_)
    {
        rightDrag_ = false;
        const auto end = e.getPosition().toFloat();
        const auto r = juce::Rectangle<float> (rightStart_, end);
        if (r.getWidth() < 4.0f && r.getHeight() < 4.0f)
            showContextMenu (e.getPosition().roundToInt());   // plain right-click
        else
            zoomToRegion (r);
        repaint();
        return;
    }
    dragStart_ = {};
}

void VideoView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (frame_.image.isValid() && e.getNumberOfClicks() >= 2) resetZoom();
}

void VideoView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (!frame_.image.isValid() || wheel.deltaY == 0.0f) return;
    zoomAt (e.getPosition().toFloat(), wheel.deltaY > 0.0f ? kZoomStep : 1.0f / kZoomStep);
}

//==============================================================================
VideoTracksPanel::VideoTracksPanel()
{
    setOpaque (true);
}

VideoTracksPanel::~VideoTracksPanel() = default;

void VideoTracksPanel::setTracks (std::vector<Track> t)
{
    // Refit the view only when the track "shape" changed (files or lengths),
    // never for offset-only updates: a live band drag must not re-zoom.
    auto sameShape = [this] (const std::vector<Track>& a, const std::vector<Track>& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].file.getFullPathName() != b[i].file.getFullPathName()
                || std::abs (a[i].durationSecs - b[i].durationSecs) > 1e-9)
                return false;
        return true; };

    const bool refit = !sameShape (tracks_, t);
    tracks_ = std::move (t);
    if (selected_ >= (int) tracks_.size()) selected_ = -1;
    if (refit && (!tracks_.empty() || audioDuration_ > 0.0)) zoomFit();
    repaint();
}

void VideoTracksPanel::setSoloFlags (std::vector<bool> s)
{
    soloFlags_ = std::move (s);
    repaint();
}

void VideoTracksPanel::setSelected (int i)
{
    if (selected_ == i) return;
    selected_ = i;
    repaint();
}

void VideoTracksPanel::setAudioDuration (double secs)
{
    if (std::abs (secs - audioDuration_) < 1e-9) return;
    audioDuration_ = secs;
    zoomFit();   // the green band defines the timeline's extent
    repaint();
}

juce::Rectangle<int> VideoTracksPanel::rowRect (int i) const
{
    return { 0, kRulerH + i * kRowH, getWidth(), kRowH };
}

double VideoTracksPanel::timeAtX (float x) const
{
    const double span = view_.getLength();
    if (span <= 0.0 || getWidth() <= 0) return view_.getStart();
    return view_.getStart() + ((double) x / (double) getWidth()) * span;
}

float VideoTracksPanel::xForTime (double t) const
{
    const double span = view_.getLength();
    if (span <= 0.0 || getWidth() <= 0) return 0.0f;
    return (float) ((t - view_.getStart()) / span * (double) getWidth());
}

void VideoTracksPanel::zoomFit()
{
    double lo = 0.0, hi = audioDuration_;
    for (const auto& t : tracks_)
    {
        lo = std::min (lo, t.offsetSecs);
        if (t.durationSecs >= 0.0) hi = std::max (hi, t.offsetSecs + t.durationSecs);
    }
    if (hi <= lo) hi = lo + 1.0;
    view_ = { lo, hi };
}

void VideoTracksPanel::zoomAbout (double tSecs, double factor)
{
    const double span = view_.getLength();
    if (span <= 0.0) return;

    // Never zoom out past eight times the fitted extent.
    double fitLo = 0.0, fitHi = audioDuration_;
    for (const auto& t : tracks_)
    {
        fitLo = std::min (fitLo, t.offsetSecs);
        if (t.durationSecs >= 0.0) fitHi = std::max (fitHi, t.offsetSecs + t.durationSecs);
    }
    if (fitHi <= fitLo) fitHi = fitLo + 1.0;
    const double maxSpan = (fitHi - fitLo) * 8.0 + 1.0;

    const double newSpan = std::clamp (span * factor, 0.001, maxSpan);
    // Keep `tSecs` under the same screen position.
    const double frac = (tSecs - view_.getStart()) / span;
    const double lo = tSecs - frac * newSpan;
    view_ = { lo, lo + newSpan };
    repaint();
}

void VideoTracksPanel::zoomIn()  { zoomAbout (0.5 * (view_.getStart() + view_.getEnd()), 1.0 / kZoomStep); }
void VideoTracksPanel::zoomOut() { zoomAbout (0.5 * (view_.getStart() + view_.getEnd()), kZoomStep); }

int VideoTracksPanel::trackIndexAt (const juce::Point<int>& p) const
{
    if (p.y < kRulerH || p.x < 0 || p.x >= getWidth()) return -1;
    const int row = (p.y - kRulerH) / kRowH;
    if (row < 0 || row >= (int) tracks_.size()) return -1;

    const auto& t = tracks_[row];
    double endT = view_.getEnd();
    if (t.durationSecs >= 0.0) endT = std::min (endT, t.offsetSecs + t.durationSecs);
    const float x1 = std::max (0.0f, xForTime (t.offsetSecs));
    const float x2 = std::min ((float) getWidth(), xForTime (endT));
    return (p.x >= (int) x1 && p.x < (int) x2) ? row : -1;
}

void VideoTracksPanel::handleContextMenuId (int id, int trackIdx)
{
    switch (id)
    {
        case kMenuZoomIn:  zoomIn(); break;
        case kMenuZoomOut: zoomOut(); break;
        case kMenuZoomFit: zoomFit(); break;
        default:
            if (onMenuItem) onMenuItem (id, trackIdx);
            break;
    }
}

void VideoTracksPanel::showContextMenuAt (const juce::Point<int>& at)
{
    const int idx = trackIndexAt (at);

    juce::PopupMenu m;
    m.addItem (kMenuZoomIn, "Zoom In");
    m.addItem (kMenuZoomOut, "Zoom Out");
    m.addItem (kMenuZoomFit, "Zoom Fit");
    if (idx >= 0 && onMenuItem)
    {
        m.addSeparator();
        m.addItem (kMenuSyncAudio, "Sync based on Audio");
        m.addItem (kMenuAlign, "Align...");
        m.addItem (kMenuCopyAudio, "Copy Audio to New Channel(s)...");
        m.addItem (kMenuAddTrack, "Add Video Track...");
        m.addItem (kMenuRemoveTrack, "Remove Track");
        const bool solo = idx < (int) soloFlags_.size() && soloFlags_[idx];
        m.addItem (kMenuSolo, "Solo", true, solo);
        m.addItem (kMenuAdjustImage, "Adjust Image...");
    }

    const int id = m.showMenu (juce::PopupMenu::Options ()
                                   .withTargetComponent (this)
                                   .withTargetScreenArea (juce::Rectangle<int> {}.withPosition (localPointToGlobal (at))));
    if (id != 0) handleContextMenuId (id, idx);
}

static double niceTickStep (double span)
{
    for (const double s : { 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0, 600.0 })
        if (span / s <= 8.0) return s;
    return 900.0;
}

void VideoTracksPanel::paintRuler (juce::Graphics& g, const juce::Rectangle<int>& r) const
{
    // The green band: the main audio's length on the timeline.
    if (audioDuration_ > 0.0)
    {
        const float x1 = std::max (0.0f, xForTime (0.0));
        const float x2 = std::min ((float) r.getWidth(), xForTime (audioDuration_));
        if (x2 > x1 + 1e-3f)
        {
            g.setColour (juce::Colour::fromRGB (80, 200, 120).withAlpha (0.55f));
            g.fillRect ((int) x1, r.getBottom() - 6, (int) (x2 - x1), 4);
        }
    }

    const double span = view_.getLength();
    if (span <= 0.0) return;

    const double step = niceTickStep (span);
    g.setColour (juce::Colours::white.withAlpha (0.7f));
    g.setFont (11.0f);
    for (double t = std::ceil (view_.getStart() / step) * step; t <= view_.getEnd(); t += step)
    {
        const float x = xForTime (t);
        if (x < 0.0f || x > (float) r.getWidth()) continue;
        g.drawLine ((int) x, r.getY(), (int) x, r.getBottom() - 8);
        const int dec = step >= 1.0 ? 0 : (step >= 0.1 ? 1 : 2);
        g.drawText (juce::String (t, dec), (int) x + 3, r.getY() + 1, 64, r.getHeight() - 8,
                    juce::Justification::left, false);
    }
}

void VideoTracksPanel::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds();
    g.fillAll (juce::Colour::fromRGB (28, 32, 38));

    paintRuler (g, juce::Rectangle<int> (0, 0, std::max (1, b.getWidth()), kRulerH));

    static const juce::Colour trackColours[] = {
        juce::Colour::fromRGB (70, 130, 255),   // blue
        juce::Colour::fromRGB (240, 160, 60),   // orange
        juce::Colour::fromRGB (90, 200, 120),   // green
        juce::Colour::fromRGB (220, 90, 200),   // magenta
        juce::Colour::fromRGB (240, 210, 80),   // yellow
        juce::Colour::fromRGB (80, 200, 220),   // cyan
        juce::Colour::fromRGB (255, 120, 160),  // pink
        juce::Colour::fromRGB (150, 170, 90) }; // olive

    for (int i = 0; i < (int) tracks_.size(); ++i)
    {
        const auto row = rowRect (i);
        if (row.isEmpty()) continue;
        const auto& tr = tracks_[i];

        double endT = view_.getEnd();
        if (tr.durationSecs >= 0.0) endT = std::min (endT, tr.offsetSecs + tr.durationSecs);
        const float x1 = std::max (0.0f, xForTime (tr.offsetSecs));
        const float x2 = std::min ((float) b.getWidth(), xForTime (endT));
        if (x2 <= x1 + 1e-3f) continue;   // band off-screen

        auto band = juce::Rectangle<int> ((int) x1, row.getY(), (int) (x2 - x1), row.getHeight());
        const auto col = trackColours[i % 8];
        // The selected band is drawn at full opacity; the white border marks it.
        g.setColour (selected_ == i ? col : col.withAlpha (0.85f));
        g.fillRect (band);

        if ((int) soloFlags_.size() > i && soloFlags_[i])
        {   g.setColour (juce::Colours::yellow); g.fillRect (band.removeFromTop (3)); }
        if (selected_ == i)
        {   g.setColour (juce::Colours::white); g.drawRect (band, 2); }

        const juce::String label = tr.file.getFileName() + "  @" + juce::String (tr.offsetSecs, 2) + " s";
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.setFont (12.0f);
        g.drawFittedText (label, band.reduced (6, 2), juce::Justification::left, 1);
    }
}

void VideoTracksPanel::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isRightButtonDown() && !e.mods.isLeftButtonDown())
    {   showContextMenuAt (e.getPosition()); return; }

    const int idx = trackIndexAt (e.getPosition());
    if (idx >= 0)
    {
        dragBand_ = true;
        dragIdx_ = idx;
        dragStartOffset_ = tracks_[idx].offsetSecs;
        dragStartPt_ = e.getPosition();
        if (selected_ != idx) setSelected (idx);
        if (onSelectTrack) onSelectTrack (idx);
    }
    else if (selected_ != -1 || onSelectTrack != nullptr)
    {
        // Clicked the ruler or empty space: clear the selection.
        if (selected_ != -1) setSelected (-1);
        if (onSelectTrack) onSelectTrack (-1);
    }
}

void VideoTracksPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (!dragBand_ || dragIdx_ < 0 || dragIdx_ >= (int) tracks_.size()) return;

    const double dt = timeAtX ((float) e.getPosition().x) - timeAtX ((float) dragStartPt_.x);
    const double off = dragStartOffset_ + dt;
    tracks_[dragIdx_].offsetSecs = off;   // local copy so the band follows at once
    repaint();
    if (onOffsetChanged) onOffsetChanged (dragIdx_, off);
}

void VideoTracksPanel::mouseUp (const juce::MouseEvent&)
{
    dragBand_ = false;
    dragIdx_ = -1;
}

void VideoTracksPanel::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (wheel.deltaY == 0.0f) return;
    zoomAbout (timeAtX ((float) e.getPosition().x), wheel.deltaY > 0.0f ? 1.0 / kZoomStep : kZoomStep);
}

//==============================================================================
VideoAdjustDialog::VideoAdjustDialog (juce::Component* parent)
    : juce::DocumentWindow ("Adjust Image", Colours::black, 0, false),
      contrast_ ("Contrast"), brightness_ ("Brightness")
{
    (void) parent;
    setUsingNativeTitleBar (true);
    setResizable (false, false);
    setResizeLimits (320, 104, 320, 104);

    contrast_.setRange (0.0, 2.0, 0.01);
    contrast_.setValue (1.0);
    brightness_.setRange (-1.0, 1.0, 0.01);
    brightness_.setValue (0.0);
    auto live = [this] { if (onValuesChanged) onValuesChanged (contrast_.getValue(), brightness_.getValue()); };
    contrast_.onValueChange = live;
    brightness_.onValueChange = live;

    applyAll_.setButtonText ("Apply to All");
    reset_.setButtonText ("Reset");
    closeBtn_.setButtonText ("Close");
    applyAll_.onClick = [this] { if (onApplyToAll) onApplyToAll (); };
    reset_.onClick = [this] { if (onReset) onReset (); };
    closeBtn_.onClick = [this] { setVisible (false); };

    contrastLbl_.setText ("Contrast", juce::NotificationType::dontSendNotification);
    brightnessLbl_.setText ("Brightness", juce::NotificationType::dontSendNotification);
    // Pointers: under JUCE_DEBUG, ResizableWindow hides the base-class
    // reference overload of addChildComponent().
    addChildComponent (&contrastLbl_);
    addChildComponent (&brightnessLbl_);
    addChildComponent (&contrast_);
    addChildComponent (&brightness_);
    addChildComponent (&applyAll_);
    addChildComponent (&reset_);
    addChildComponent (&closeBtn_);

    centreWithSize (320, 104);
    addToDesktop ();   // only now that the members are initialised: correct caption style
}

void VideoAdjustDialog::resized()
{
    const int w = getWidth();
    contrastLbl_.setBounds (12, 16, 78, 20);
    brightnessLbl_.setBounds (12, 48, 78, 20);
    contrast_.setBounds (96, 12, std::max (0, w - 108), 24);
    brightness_.setBounds (96, 44, std::max (0, w - 108), 24);
    applyAll_.setBounds (12, 76, 96, 24);
    reset_.setBounds (114, 76, 72, 24);
    closeBtn_.setBounds (std::max (0, w - 100), 76, 88, 24);
}

void VideoAdjustDialog::setValues (double contrast, double brightness)
{
    if (std::abs (contrast - contrast_.getValue()) > 1e-9) contrast_.setValue (contrast);
    if (std::abs (brightness - brightness_.getValue()) > 1e-9) brightness_.setValue (brightness);
}

//==============================================================================
static double durationOf (const std::map<juce::String, double>& durations, const juce::File& f)
{
    const auto it = durations.find (f.getFullPathName());
    return it != durations.end() ? it->second : -1.0;
}

/** True when a track with this offset/length covers document time t. */
static bool coversTime (double offsetSecs, double durationSecs, double t)
{
    if (t < offsetSecs) return false;
    if (durationSecs < 0.0) return true;   // unknown length: assume it continues
    return t < offsetSecs + durationSecs;
}

//==============================================================================
VideoWindow::VideoWindow (std::function<double()> getTime)
    : juce::DocumentWindow ("Video", Colours::black, juce::DocumentWindow::allButtons, false),
      getTime_ (std::move (getTime))
{
    setUsingNativeTitleBar (true);
    setResizable (true, false);
    setResizeLimits (160, 90, 8192, 8192);

    // Content: the picture on top, the track list below.
    contentHost_ = std::make_unique<VideoContentHost>();
    contentHost_->addChildComponent (&videoView);
    contentHost_->addChildComponent (&tracksPanel);
    setContentNonOwned (contentHost_.get(), false);
    centreWithSize (640, 360 + VideoContentHost::kPanelH);
    setWantsKeyboardFocus (true);

    videoView.setMessage ("No video for this document.");

    framePump.onOpened = [this] (bool ok)
    {
        if (ok)
        {
            if (currentFile != juce::File())
                durations_[currentFile.getFullPathName()] = framePump.info().duration;
            videoView.setMessage ({});
            syncPanelDurations();   // the length is known now: bands can fit/cover
            refreshDisplayed();     // coverage may have changed with the real length
            pollTime();             // first frame at the current time, right away
        }
        else if (currentFile != juce::File())
        {
            videoView.clearFrame();
            videoView.setMessage (framePump.hasNoVideoTrack()
                                      ? juce::String ("This file has no video track.")
                                      : "The video cannot be shown.\n" + framePump.error());
        }
        updateNotice();
        if (onVideoOpened) onVideoOpened (ok, framePump.hasNoVideoTrack());
    };
    framePump.onFrame = [this]
    {
        const auto& fr0 = framePump.frame();
        if (!fr0.image.isValid())
        {   videoView.setFrame (fr0, 0); return; }

        int rot = framePump.info().rotation;
        if (displayed_ >= 0 && displayed_ < (int) tracks_.size()) rot += tracks_[displayed_].rotation;

        const bool neutral = std::abs (contrast_ - 1.0) < 1e-6 && std::abs (brightness_) < 1e-6;
        if (neutral)
            videoView.setFrame (fr0, rot);
        else
        {
            auto fr = fr0;   // copy: the pump's frame must stay untouched
            applyImageAdjustments (fr.image, contrast_, brightness_);
            videoView.setFrame (fr, rot);
        }
    };

    videoView.onDisplaySizeChanged = [this] { updateDecodeSize(); };
    videoView.onUserRotationChanged = [this] (int) { saveDisplayedAdjustments (); };
    videoView.onAdjustmentChanged = [this] { saveDisplayedAdjustments (); };

    tracksPanel.onSelectTrack = [this] (int i)
    {
        selected_ = i;
        refreshDisplayed();   // the clicked track renders above while it covers t
        if (onSelectTrackChanged) onSelectTrackChanged (i);
    };
    tracksPanel.onOffsetChanged = [this] (int i, double off)
    {
        if (i < 0 || i >= (int) tracks_.size()) return;
        tracks_[i].offsetSecs = off;
        refreshDisplayed();   // the displayed track may change at a boundary
        if (onTrackOffsetChanged) onTrackOffsetChanged (i, off);
    };
    tracksPanel.onMenuItem = [this] (int id, int idx)
    {
        if (onTrackMenuAction) onTrackMenuAction (id, idx);
    };

    updateDecodeSize();   // before the first open: a 4K movie never decodes at 4K for a small window

    // Only now that requiredButtons is initialised does the peer compute the
    // caption style: min/max/close all appear.
    addToDesktop ();
}

VideoWindow::~VideoWindow()
{
    stopTimer();
    framePump.onOpened = nullptr;
    framePump.onFrame = nullptr;
    videoView.onDisplaySizeChanged = nullptr;
    videoView.onUserRotationChanged = nullptr;
    videoView.onAdjustmentChanged = nullptr;
    tracksPanel.onSelectTrack = nullptr;
    tracksPanel.onOffsetChanged = nullptr;
    tracksPanel.onMenuItem = nullptr;

    if (contentHost_ != nullptr)
    {
        contentHost_->removeChildComponent (&videoView);
        contentHost_->removeChildComponent (&tracksPanel);
    }
    clearContentComponent ();
    contentHost_.reset();   // safe now: no children left to dangle
}

void VideoWindow::setTracks (const std::vector<sc::VideoTrackData>& t, int selected)
{
    tracks_ = t;
    selected_ = (selected >= 0 && selected < (int) tracks_.size()) ? selected : -1;
    syncPanelDurations();
    refreshDisplayed();
}

void VideoWindow::setVideoFile (const juce::File& file, const juce::String&)
{
    std::vector<sc::VideoTrackData> t;
    if (file != juce::File())
    {
        sc::VideoTrackData td;
        td.file = file;
        for (const auto& old : tracks_)
            if (old.file == file) { td = old; break; }   // keep this file's adjustments
        t.push_back (std::move (td));
    }
    setTracks (t, -1);
}

void VideoWindow::setAudioDuration (double seconds)
{
    if (std::abs (seconds - audioDuration_) < 1e-9) return;
    audioDuration_ = seconds;
    tracksPanel.setAudioDuration (seconds);   // green band + refit the view
    updateNotice();
}

void VideoWindow::syncPanelDurations()
{
    std::vector<VideoTracksPanel::Track> pt;
    pt.reserve (tracks_.size());
    for (const auto& tr : tracks_)
    {
        VideoTracksPanel::Track p;
        p.file = tr.file;
        p.offsetSecs = tr.offsetSecs;
        p.durationSecs = durationOf (durations_, tr.file);
        pt.push_back (std::move (p));
    }
    tracksPanel.setTracks (std::move (pt));
    tracksPanel.setSelected (selected_);

    std::vector<bool> solo;
    solo.reserve (tracks_.size());
    for (const auto& tr : tracks_) solo.push_back (tr.solo);
    tracksPanel.setSoloFlags (std::move (solo));
}

int VideoWindow::displayedTrackAt (double tSecs) const
{
    if (tracks_.empty()) return -1;

    int topmostCovered = -1;
    for (size_t i = 0; i < tracks_.size(); ++i)
        if (coversTime (tracks_[i].offsetSecs, durationOf (durations_, tracks_[i].file), tSecs))
            topmostCovered = (int) i;

    if (selected_ >= 0 && selected_ < (int) tracks_.size()
        && coversTime (tracks_[selected_].offsetSecs, durationOf (durations_, tracks_[selected_].file), tSecs))
        return selected_;
    if (topmostCovered >= 0) return topmostCovered;

    // Nothing covers this time: hold the nearest track's picture (the pump
    // clamps to its first/last frame, as before offsets existed).
    int best = -1;
    double bestDist = 0.0;
    for (size_t i = 0; i < tracks_.size(); ++i)
    {
        const auto& tr = tracks_[i];
        const double lo = tr.offsetSecs;
        const double dur = durationOf (durations_, tr.file);
        const double end = dur >= 0.0 ? lo + dur : std::numeric_limits<double>::infinity();
        const double d = tSecs < lo ? lo - tSecs : (tSecs > end ? tSecs - end : 0.0);
        if (best < 0 || d < bestDist - 1e-9) { best = (int) i; bestDist = d; }
    }
    return best;
}

void VideoWindow::refreshDisplayed()
{
    const double t = getTime_ ? getTime_ () : 0.0;
    switchTo (displayedTrackAt (t));
}

void VideoWindow::switchTo (int d)
{
    if (d == displayed_) return;
    displayed_ = d;

    if (d < 0)
    {
        currentFile = juce::File();
        framePump.open (juce::File());
        videoView.clearFrame();
        videoView.setMessage ("No video for this document.");
    }
    else
    {
        const auto& td = tracks_[d];
        if (td.file != currentFile)
        {
            currentFile = td.file;
            videoView.clearFrame();   // also resets the zoom/pan to fit
            setTitle ("Video - " + td.file.getFileName());
            if (!sc::videoDecodingAvailable())
                videoView.setMessage ("Video pictures cannot be shown on this platform.");
            else
                videoView.setMessage ("Opening " + td.file.getFileName() + " ...");
            framePump.open (td.file);
        }
        applyTrackAdjustments (d);   // restore this track's zoom/pan/rotation
    }

    updateNotice();
    if (onDisplayedTrackChanged) onDisplayedTrackChanged (d);
}

void VideoWindow::applyTrackAdjustments (int i)
{
    if (i < 0 || i >= (int) tracks_.size()) return;
    const auto& td = tracks_[i];
    videoView.setAdjustments (td.zoom, { td.panX, td.panY });
    videoView.setUserRotation (td.rotation);
    contrast_ = td.contrast;
    brightness_ = td.brightness;
    pollTime();   // re-decode the current frame so new adjustments apply at once
}

void VideoWindow::saveDisplayedAdjustments()
{
    if (displayed_ < 0 || displayed_ >= (int) tracks_.size()) return;
    auto& td = tracks_[displayed_];
    td.zoom = videoView.getZoom();
    td.panX = videoView.getPan().x;
    td.panY = videoView.getPan().y;
    td.rotation = videoView.getUserRotation();
    if (onTrackAdjustmentsSaved) onTrackAdjustmentsSaved (td);
}

void VideoWindow::setAdjustmentsLive (double contrast, double brightness)
{
    contrast_ = contrast;
    brightness_ = brightness;
    if (displayed_ >= 0 && displayed_ < (int) tracks_.size())
    {
        tracks_[displayed_].contrast = contrast;
        tracks_[displayed_].brightness = brightness;
        saveDisplayedAdjustments();   // persist for this track
    }
    pollTime();   // re-decode the current frame with the new values
}

void VideoWindow::updateNotice()
{
    juce::String n;
    const double vd = framePump.isOpen() ? framePump.info().duration : 0.0;
    if (vd > 0.0 && audioDuration_ > 0.0 && std::abs (vd - audioDuration_) > 0.5)
        n = "Audio (" + juce::String (audioDuration_, 2) + " s) and video (" + juce::String (vd, 2)
            + " s) lengths differ: the picture follows the document time.";
    videoView.setNotice (n);
}

void VideoWindow::pollTime()
{
    if (!getTime_ || !framePump.isOpen()) return;
    double t = getTime_ ();
    if (displayed_ >= 0 && displayed_ < (int) tracks_.size())
        t -= tracks_[displayed_].offsetSecs;   // document time -> this track's own time
    framePump.request (t);
}

void VideoWindow::timerCallback()
{
    refreshDisplayed();   // auto-switch when the displayed track ends/starts
    pollTime();
}

void VideoWindow::visibilityChanged()
{
    juce::DocumentWindow::visibilityChanged ();
    // While docked the window is hidden but must keep polling frames for the panel.
    if (isVisible() || docked_)
    {
        updateDecodeSize();
        startTimerHz (60);
        pollTime();
    }
    else
        stopTimer ();
}

void VideoWindow::moved()
{
    juce::DocumentWindow::moved ();
    updateDecodeSize ();   // may now be on a display with another density
}

bool VideoWindow::keyPressed (const juce::KeyPress& key)
{
    // Space, arrows, Home/End... belong to the main window's transport.
    if (onKeyPressed && onKeyPressed (key))
        return true;
    return juce::DocumentWindow::keyPressed (key);
}

void VideoWindow::closeButtonPressed()
{
    setVisible (false);
    if (onUserClosed) onUserClosed ();
}

void VideoWindow::dockTo (VideoDockHost& host)
{
    if (auto* p = videoView.getParentComponent())   // out of the content host
        p->removeChildComponent (&videoView);
    host.attach (&videoView);          // into the panel tab
    setDocked (true);
    setVisible (false);
}

void VideoWindow::undock()
{
    if (!docked_) return;
    if (auto* p = videoView.getParentComponent())   // out of the dock host
        p->removeChildComponent (&videoView);
    // Back into the content host with the picture first (resized() indexes by order).
    contentHost_->removeChildComponent (&tracksPanel);
    contentHost_->addChildComponent (&videoView);
    contentHost_->addChildComponent (&tracksPanel);
    setDocked (false);
}

void VideoWindow::setDocked (bool d)
{
    if (docked_ == d) return;
    docked_ = d;
    videoView.docked = d;   // context-menu label: "Dock Panel" / "Undock to Window"
    visibilityChanged ();   // start/stop the timer for the new mode
}

void VideoWindow::updateDecodeSize()
{
    // Physical pixels: twice the logical size on a Retina / 200 % display.
    double scale = 1.0;
    if (auto* d = juce::Desktop::getInstance().getDisplays().getDisplayForRect (videoView.getScreenBounds()))
        scale = std::max (1.0, d->scale);
    framePump.setDisplayLongSide ((int) std::ceil (videoView.neededLongSide() * scale));
}

bool VideoWindow::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (sc::isVideoPictureCandidate (juce::File (f))) return true;
    return false;
}

void VideoWindow::filesDropped (const juce::StringArray& files, int, int)
{
    if (onVideoDropped == nullptr) return;
    for (const auto& f : files)
        if (sc::isVideoPictureCandidate (juce::File (f)))
            onVideoDropped (juce::File (f));
}
