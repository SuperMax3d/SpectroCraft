/**
 * SpectroCraft - Video window (see include/VideoWindow.h)
 */

#include "VideoWindow.h"

#include <cmath>

//==============================================================================
VideoToolButton::VideoToolButton (const juce::String& name, Icon i)
    : juce::Button (name), icon (i)
{
    setTooltip (name);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void VideoToolButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (juce::Colours::black.withAlpha (down ? 0.85f : highlighted ? 0.75f : 0.55f));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (juce::Colours::white.withAlpha (isEnabled() ? 0.35f : 0.15f));
    g.drawRoundedRectangle (r, 4.0f, 1.0f);

    const auto c = r.getCentre();
    const float k = std::min (r.getWidth(), r.getHeight()) * 0.28f;   // icon half-size
    g.setColour (juce::Colours::white.withAlpha (isEnabled() ? 1.0f : 0.35f));
    const juce::PathStrokeType stroke (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    if (icon == Icon::RotateCW)
    {
        // Three-quarter circle, arrow head at its end (clockwise).
        juce::Path arc;
        const float a0 = -juce::MathConstants<float>::pi * 0.35f;
        const float a1 = juce::MathConstants<float>::pi * 1.25f;
        arc.addCentredArc (c.x, c.y, k, k, 0.0f, a0, a1, true);
        g.strokePath (arc, stroke);
        const juce::Point<float> tip (c.x + k * std::sin (a1), c.y - k * std::cos (a1));
        juce::Path head;
        head.addTriangle (tip.x - k * 0.55f, tip.y - k * 0.15f,
                          tip.x + k * 0.35f, tip.y - k * 0.55f,
                          tip.x + k * 0.1f, tip.y + k * 0.45f);
        g.fillPath (head);
        return;
    }

    juce::Path p;
    p.startNewSubPath (c.x - k, c.y);
    p.lineTo (c.x + k, c.y);
    if (icon == Icon::ZoomIn)
    {
        p.startNewSubPath (c.x, c.y - k);
        p.lineTo (c.x, c.y + k);
    }
    g.strokePath (p, stroke);
}

//==============================================================================
VideoView::VideoView()
{
    setOpaque (true);
    for (auto* b : { &rotateBtn, &zoomInBtn, &zoomOutBtn })
    {
        addChildComponent (b);
        b->setWantsKeyboardFocus (false);
    }
    rotateBtn.onClick = [this] { rotateClockwise(); };
    zoomInBtn.onClick = [this] { zoomIn(); };
    zoomOutBtn.onClick = [this] { zoomOut(); };
    updateButtons();
}

void VideoView::setFrame (const sc::VideoFrame& f, int rot)
{
    const bool sizeChanged = ! frame.image.isValid() || ! f.image.isValid()
                             || frame.image.getBounds() != f.image.getBounds();
    frame = f;
    trackRotation = ((rot % 360) + 360) % 360;
    if (sizeChanged)
        clampPan();
    updateButtons();
    repaint();
}

void VideoView::clearFrame()
{
    frame = {};
    updateButtons();
    repaint();
}

void VideoView::setMessage (const juce::String& m)
{
    if (m == message) return;
    message = m;
    repaint();
}

void VideoView::setNotice (const juce::String& n)
{
    if (n == notice) return;
    notice = n;
    repaint();
}

juce::Rectangle<float> VideoView::fittedBounds() const
{
    if (! frame.image.isValid())
        return {};
    const bool quarter = totalRotation() == 90 || totalRotation() == 270;
    const float w = (float) (quarter ? frame.image.getHeight() : frame.image.getWidth());
    const float h = (float) (quarter ? frame.image.getWidth() : frame.image.getHeight());
    auto area = getLocalBounds().toFloat();
    if (w <= 0.0f || h <= 0.0f || area.isEmpty())
        return {};
    const float s = std::min (area.getWidth() / w, area.getHeight() / h);
    return juce::Rectangle<float> (w * s, h * s).withCentre (area.getCentre());
}

juce::Rectangle<float> VideoView::pictureBounds() const
{
    const auto fit = fittedBounds();
    if (fit.isEmpty())
        return {};
    const auto c = getLocalBounds().toFloat().getCentre() + pan;
    return juce::Rectangle<float> (fit.getWidth() * zoom, fit.getHeight() * zoom).withCentre (c);
}

void VideoView::clampPan()
{
    // The zoomed picture may move until its edge reaches the view's edge,
    // never further (no empty band opens on a side that has picture left).
    const auto fit = fittedBounds();
    const auto area = getLocalBounds().toFloat();
    const float mx = std::max (0.0f, (fit.getWidth() * zoom - area.getWidth()) * 0.5f);
    const float my = std::max (0.0f, (fit.getHeight() * zoom - area.getHeight()) * 0.5f);
    pan = { juce::jlimit (-mx, mx, pan.x), juce::jlimit (-my, my, pan.y) };
}

void VideoView::setZoomKeepingCentre (float z)
{
    z = juce::jlimit (1.0f, kMaxZoom, z);
    if (z == zoom) return;
    // The picture point at the view centre stays there.
    pan = pan * (z / zoom);
    zoom = z;
    clampPan();
    updateButtons();
    repaint();
    if (onDisplaySizeChanged) onDisplaySizeChanged();
}

int VideoView::neededLongSide() const
{
    return (int) std::ceil ((float) std::max (getWidth(), getHeight()) * zoom);
}

void VideoView::zoomIn()  { setZoomKeepingCentre (zoom * kZoomStep); }
void VideoView::zoomOut() { setZoomKeepingCentre (zoom / kZoomStep < 1.0001f ? 1.0f : zoom / kZoomStep); }

void VideoView::resetZoom()
{
    const bool changed = zoom != 1.0f;
    zoom = 1.0f;
    pan = {};
    updateButtons();
    repaint();
    if (changed && onDisplaySizeChanged) onDisplaySizeChanged();
}

void VideoView::setUserRotation (int degrees)
{
    userRotation = (((degrees / 90) * 90) % 360 + 360) % 360;
    clampPan();   // the picture's aspect turned: the pan limits changed
    repaint();
}

void VideoView::rotateClockwise()
{
    // Keep what is at the view centre in the centre: the pan turns with
    // the picture.
    pan = { -pan.y, pan.x };
    setUserRotation (userRotation + 90);
    if (onUserRotationChanged) onUserRotationChanged (userRotation);
}

void VideoView::updateButtons()
{
    const bool show = frame.image.isValid();
    for (auto* b : { &rotateBtn, &zoomInBtn, &zoomOutBtn })
        b->setVisible (show);
    zoomInBtn.setEnabled (zoom < kMaxZoom - 1.0e-4f);
    zoomOutBtn.setEnabled (zoom > 1.0f + 1.0e-4f);
}

void VideoView::resized()
{
    // Top-right corner: [rotate] [-] [+]
    constexpr int sz = 24, gap = 4, margin = 6;
    int x = getWidth() - margin - sz;
    for (auto* b : { &zoomInBtn, &zoomOutBtn, &rotateBtn })
    {
        b->setBounds (x, margin, sz, sz);
        x -= sz + gap;
    }
    clampPan();
    if (onDisplaySizeChanged) onDisplaySizeChanged();
}

void VideoView::mouseDown (const juce::MouseEvent&)
{
    panAtDragStart = pan;
}

void VideoView::setPan (juce::Point<float> p)
{
    pan = p;
    clampPan();
    repaint();
}

void VideoView::mouseDrag (const juce::MouseEvent& e)
{
    if (zoom > 1.0f)
        setPan (panAtDragStart + e.getOffsetFromDragStart().toFloat());
}

void VideoView::mouseDoubleClick (const juce::MouseEvent&)
{
    resetZoom();
}

void VideoView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    if (frame.image.isValid())
    {
        const auto r = pictureBounds();
        const int rot = totalRotation();
        const float iw = (float) frame.image.getWidth(), ih = (float) frame.image.getHeight();
        const bool quarter = rot == 90 || rot == 270;
        const float s = r.getWidth() / (quarter ? ih : iw);
        const auto t = juce::AffineTransform::translation (-iw * 0.5f, -ih * 0.5f)
                           .rotated ((float) rot * juce::MathConstants<float>::pi / 180.0f)
                           .scaled (s)
                           .translated (r.getCentreX(), r.getCentreY());
        g.setImageResamplingQuality (zoom > 2.0f ? juce::Graphics::lowResamplingQuality
                                                 : juce::Graphics::mediumResamplingQuality);
        g.drawImageTransformed (frame.image, t, false);

        // Time of the frame on screen (+ zoom when zoomed), top left.
        juce::String ts = juce::String (frame.time, 3) + " s";
        if (zoom > 1.0f)
            ts << "   x" << juce::String (zoom, 2);
        g.setFont (juce::FontOptions (13.0f));
        const int tw = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), ts) + 10;
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRoundedRectangle (6.0f, 6.0f, (float) tw, 18.0f, 3.0f);
        g.setColour (juce::Colours::white);
        g.drawText (ts, 6, 6, tw, 18, juce::Justification::centred, false);
    }

    if (message.isNotEmpty())
    {
        g.setColour (juce::Colours::lightgrey);
        g.setFont (juce::FontOptions (15.0f));
        g.drawFittedText (message, getLocalBounds().reduced (16), juce::Justification::centred, 4);
    }

    if (notice.isNotEmpty())
    {
        auto band = getLocalBounds().removeFromBottom (22);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillRect (band);
        g.setColour (juce::Colour (0xffffc04d));
        g.setFont (juce::FontOptions (12.5f));
        g.drawFittedText (notice, band.reduced (6, 0), juce::Justification::centredLeft, 1);
    }
}

//==============================================================================
VideoWindow::VideoWindow (std::function<double()> ts)
    : juce::DocumentWindow ("Video", juce::Colours::black, juce::DocumentWindow::closeButton, true),
      timeSource (std::move (ts))
{
    setUsingNativeTitleBar (true);
    setResizable (true, false);
    setResizeLimits (160, 90, 8192, 8192);
    setContentNonOwned (&videoView, false);
    centreWithSize (640, 360);
    setWantsKeyboardFocus (true);

    videoView.setMessage ("No video for this document.");

    framePump.onOpened = [this] (bool ok)
    {
        if (ok)
        {
            videoView.setMessage ({});
            pollTime();   // first frame at the current time, right away
        }
        else
        {
            videoView.clearFrame();
            videoView.setMessage (framePump.hasNoVideoTrack()
                                      ? juce::String ("This file has no video track.")
                                      : "The video cannot be shown.\n" + framePump.error());
        }
        updateNotice();
        if (onVideoOpened) onVideoOpened (ok, framePump.hasNoVideoTrack());
    };
    videoView.onDisplaySizeChanged = [this] { updateDecodeSize(); };
    videoView.onUserRotationChanged = [this] (int r)
    {
        if (videoFile != juce::File())
            rotationByFile[videoFile.getFullPathName()] = r;
    };
    framePump.onFrame = [this]
    {
        videoView.setFrame (framePump.frame(), framePump.info().rotation);
    };
    updateDecodeSize();   // before the first open: a 4K movie never decodes at 4K for a small window
}

VideoWindow::~VideoWindow()
{
    stopTimer();
    framePump.onOpened = nullptr;
    framePump.onFrame = nullptr;
    clearContentComponent();
}

void VideoWindow::setVideoFile (const juce::File& f, const juce::String& displayName)
{
    docName = displayName;
    setName (displayName.isNotEmpty() ? "Video - " + displayName : juce::String ("Video"));
    if (f == videoFile)
        return;

    videoFile = f;
    videoView.clearFrame();
    // A new picture starts whole, with the rotation chosen for it earlier
    // in this session (the window keeps one per file).
    videoView.resetZoom();
    const auto it = rotationByFile.find (f.getFullPathName());
    videoView.setUserRotation (it != rotationByFile.end() ? it->second : 0);
    if (f == juce::File())
    {
        framePump.open ({});
        videoView.setMessage ("No video for this document.");
    }
    else
    {
        if (! sc::videoDecodingAvailable())
            videoView.setMessage ("Video pictures cannot be shown on this platform.");
        else
            videoView.setMessage ("Opening " + f.getFileName() + " ...");
        framePump.open (f);
    }
    updateNotice();
}

void VideoWindow::setAudioDuration (double seconds)
{
    if (std::abs (seconds - audioDuration) < 1.0e-9) return;
    audioDuration = seconds;
    updateNotice();
}

void VideoWindow::updateNotice()
{
    juce::String n;
    const double vd = framePump.isOpen() ? framePump.info().duration : 0.0;
    if (vd > 0.0 && audioDuration > 0.0 && std::abs (vd - audioDuration) > 0.5)
        n = "Audio (" + juce::String (audioDuration, 2) + " s) and video (" + juce::String (vd, 2)
            + " s) lengths differ: the picture follows the document time.";
    videoView.setNotice (n);
}

void VideoWindow::updateDecodeSize()
{
    // Physical pixels: twice the logical size on a Retina / 200 % display.
    double scale = 1.0;
    if (auto* d = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds()))
        scale = std::max (1.0, d->scale);
    framePump.setDisplayLongSide ((int) std::ceil (videoView.neededLongSide() * scale));
}

void VideoWindow::moved()
{
    juce::DocumentWindow::moved();
    updateDecodeSize();   // may now be on a display with another density
}

void VideoWindow::pollTime()
{
    if (timeSource && framePump.isOpen())
        framePump.request (timeSource());
}

void VideoWindow::timerCallback()
{
    pollTime();
}

void VideoWindow::visibilityChanged()
{
    juce::DocumentWindow::visibilityChanged();
    if (isVisible())
    {
        startTimerHz (60);
        pollTime();
    }
    else
        stopTimer();
}

void VideoWindow::closeButtonPressed()
{
    setVisible (false);
    if (onUserClosed) onUserClosed();
}

bool VideoWindow::keyPressed (const juce::KeyPress& key)
{
    // Space, arrows, Home/End... belong to the main window's transport.
    if (onKeyPressed && onKeyPressed (key))
        return true;
    return juce::DocumentWindow::keyPressed (key);
}
