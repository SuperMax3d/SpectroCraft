/**
 * SpectroCraft - Video window (see include/VideoWindow.h)
 */
#include <JuceHeader.h>

#include "VideoWindow.h"

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
}

void VideoView::zoomIn()  { zoomAt (plotArea().getCentre(), kZoomStep); }
void VideoView::zoomOut() { zoomAt (plotArea().getCentre(), 1.0f / kZoomStep); }

void VideoView::resetZoom()
{
    zoom_ = 1.0f;
    pan_ = {};
    syncZoomBar();
    repaint();
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
    m.addItem (kMenuRotateCW, "Rotate 90° Clockwise");
    m.addItem (kMenuRotateCCW, "Rotate 90° Counter-Clockwise");
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
VideoWindow::VideoWindow (std::function<double()> getTime)
    : juce::DocumentWindow ("Video", Colours::black, juce::DocumentWindow::allButtons, false),
      getTime_ (std::move (getTime))
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
        if (currentFile != juce::File())
            rotationByFile[currentFile.getFullPathName()] = r;
    };
    framePump.onFrame = [this]
    {
        videoView.setFrame (framePump.frame(), framePump.info().rotation);
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
    clearContentComponent ();
}

void VideoWindow::setVideoFile (const juce::File& file, const juce::String& docName)
{
    if (file == currentFile) return;

    currentFile = file;
    videoView.clearFrame();   // also resets the zoom/pan to fit
    // Restore the rotation chosen for this file earlier in this session.
    const auto it = rotationByFile.find (file.getFullPathName());
    videoView.setUserRotation (it != rotationByFile.end() ? it->second : 0);

    if (file == juce::File())
    {
        framePump.open ({});
        videoView.setMessage ("No video for this document.");
    }
    else
    {
        setTitle ("Video - " + docName);
        if (! sc::videoDecodingAvailable())
            videoView.setMessage ("Video pictures cannot be shown on this platform.");
        else
            videoView.setMessage ("Opening " + file.getFileName() + " ...");
        framePump.open (file);
    }
    updateNotice();
}

void VideoWindow::setAudioDuration (double seconds)
{
    if (std::abs (seconds - audioDuration_) < 1.0e-9) return;
    audioDuration_ = seconds;
    updateNotice();
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
    if (getTime_ && framePump.isOpen())
        framePump.request (getTime_ ());
}

void VideoWindow::timerCallback()
{
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
    clearContentComponent ();          // detach the view from this window
    host.attach (&videoView);          // into the panel tab
    setDocked (true);
    setVisible (false);
}

void VideoWindow::undock()
{
    if (!docked_) return;
    if (auto* p = videoView.getParentComponent())  // out of the dock host
        p->removeChildComponent (&videoView);
    setContentNonOwned (&videoView, false);        // back as this window's content
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
