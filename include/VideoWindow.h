/**
 * SpectroCraft - Video window
 *
 * A separate resizable window showing the picture of a video file in step
 * with its audio: it polls MainWindow::videoTimeNow() at 60 Hz and asks its
 * own sc::VideoFramePump for "the frame displayed at t". The pump decodes on
 * a worker thread; this class only displays.
 *
 * The picture can also be DOCKED into the left panel (a tab like Tables):
 * dockTo()/undock() move the single VideoView between the window and the
 * dock host, so zoom/pan/rotation state survives the switch. While docked
 * the standalone window stays hidden but keeps polling frames.
 */
#pragma once

#include <JuceHeader.h>

#include "ScrollBars.h"
#include "VideoFrames.h"

//==============================================================================
/** The picture of a video file, letterboxed and centred in its plot area.
    Zoom/pan state is per view; the frame itself comes from the owner's
    VideoFramePump (one decoder shared by window mode and docked mode). */
class VideoView : public juce::Component
{
public:
    static constexpr float kZoomStep = 1.25f;   // one wheel notch / menu step
    static constexpr float kMaxZoom = 8.0f;     // hard zoom ceiling

    /** Context-menu item ids (handleContextMenuId). */
    enum { kMenuZoomIn = 1, kMenuZoomOut, kMenuZoomFit,
           kMenuRotateCW, kMenuRotateCCW, kMenuAlwaysOnTop, kMenuDock };

    VideoView();

    void setFrame (const sc::VideoFrame& frame, int trackRotationDeg);
    /** Drops the picture (a new file is being opened). */
    void clearFrame();
    /** Sets the user's rotation directly (per-file restore); multiples of 90. */
    void setUserRotation (int degrees);
    /** Long side in logical pixels the current zoom needs decoded. */
    int neededLongSide() const;

    const juce::Image* image() const noexcept { return &frame_.image; }
    double time() const noexcept { return frame_.time; }
    /** Total clockwise rotation: the track's plus the user's. */
    int totalRotation() const noexcept { return (trackRot_ + userRotation_) % 360; }

    // Zoom / pan ----------------------------------------------------------
    /** The picture now: fitted bounds scaled by zoom_ about their centre and
        shifted by pan_. Empty when there is no frame. */
    juce::Rectangle<float> pictureBounds() const;
    float getZoom() const noexcept { return zoom_; }
    juce::Point<float> getPan() const noexcept { return pan_; }
    void setPan (juce::Point<float> p);   // clamped to the picture edges
    void zoomIn();                        // one step, keeps the centre fixed
    void zoomOut();                       // one step; never below fit
    void resetZoom();                     // back to fit (double-click)
    /** Multiplies the zoom by factor so that the image point under `at`
        stays put (mouse wheel). */
    void zoomAt (juce::Point<float> at, float factor);
    /** Zooms so the dragged view rectangle fills the plot area (right-drag
        box zoom; both axes, centred on the region). */
    void zoomToRegion (const juce::Rectangle<float>& viewRect);

    // Rotation ------------------------------------------------------------
    int getUserRotation() const noexcept { return userRotation_; }
    void rotateClockwise();              // +90 degrees
    void rotateCounterClockwise();       // -90 degrees

    /** The right-hand zoom/pan bar (like the spectrogram's frequency bar):
        the thumb shows the visible part of the picture height; dragging its
        centre pans, dragging an edge zooms. */
    ValueScrollBar* getZoomBar() noexcept { return &zoomBar_; }

    // Context menu --------------------------------------------------------
    /** Actions that reach outside the view (set by the owner); a missing
        callback hides its menu item. */
    std::function<void (bool)> onToggleAlwaysOnTop;
    std::function<bool()> isAlwaysOnTop;
    std::function<void()> onDockToggle;
    bool docked = false;                 // menu label: "Dock Panel" / "Undock"

    /** The view's size changed: the owner may want to re-decode at a new size. */
    std::function<void()> onDisplaySizeChanged;
    /** User rotation in degrees; the owner saves it per file. */
    std::function<void (int)> onUserRotationChanged;
    /** Runs one context-menu item (the popup and the tests both call this). */
    void handleContextMenuId (int id);

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    void setMessage (const juce::String& m);
    void setNotice (const juce::String& n);
    juce::String getMessage() const noexcept { return message_; }
    juce::String getNotice() const noexcept { return notice_; }

private:
    // The picture area: everything except the right-hand zoom/pan bar.
    static constexpr int kBarWidth = 14;
    juce::Rectangle<float> plotArea() const
    {
        const auto lb = getLocalBounds();
        return { 0.0f, 0.0f,
                 (float) std::max (1, lb.getWidth() - kBarWidth),
                 (float) lb.getHeight() };
    }

    /** The frame at zoom 1: aspect kept, centred in the plot area. */
    juce::Rectangle<float> fittedBounds() const;

    void clampPan();
    void syncZoomBar();                  // thumb <- current zoom/pan
    void applyZoomFromBar (double lo, double hi);   // zoom/pan <- bar window
    void showContextMenu (const juce::Point<int>& at);

    sc::VideoFrame frame_;              // latest decoded picture
    int trackRot_ = 0;                   // clockwise rotation stored in the file
    int userRotation_ = 0;               // extra clockwise turns chosen by the user
    float zoom_ = 1.0f;                  // 1 = fit to the view
    juce::Point<float> pan_;             // picture offset from centre, clamped

    juce::String message_;               // "No video for this document" / error
    juce::String notice_;                 // audio/video length mismatch warning

    ValueScrollBar zoomBar_;             // right-hand zoom/pan bar
    double barRangeH_ = -1.0;            // last range passed to the bar

    bool rightDrag_ = false;             // right-button box-zoom drag in flight
    juce::Point<float> rightStart_, rightNow_;
    juce::Point<float> dragStart_;        // left-button pan drag start (empty = none)

    JUCE_DECLARE_NON_COPYABLE (VideoView)
};

//==============================================================================
/** Tab host for a docked video view: keeps its single child filling itself. */
class VideoDockHost : public juce::Component
{
public:
    void attach (juce::Component* c)
    {
        if (c != nullptr && getNumChildComponents() == 0) addChildComponent (c);
        resized();
    }
    juce::Component* child() const noexcept
    {   return getNumChildComponents() > 0 ? getChildComponent (0) : nullptr; }

private:
    void resized() override
    {
        for (auto* c : getChildren()) c->setBounds (getLocalBounds());
    }
};

//==============================================================================
/** The standalone Video window. Owns the frame pump and polls the document
    time from getTime(); the picture is shown in its VideoView, which can be
    moved into a dock host while this window stays hidden (docked mode). */
class VideoWindow : public juce::DocumentWindow,
                    private juce::Timer
{
public:
    explicit VideoWindow (std::function<double()> getTime);
    ~VideoWindow() override;

    void setVideoFile (const juce::File& file, const juce::String& docName);
    /** Audio length in seconds; a mismatch with the video track raises a
        notice on the picture. */
    void setAudioDuration (double secs);

    VideoView& view() noexcept { return videoView; }
    sc::VideoFramePump& pump() noexcept { return framePump; }
    juce::File getVideoFile() const noexcept { return currentFile; }
    /** Asks the pump for the frame at the owner's current time. */
    void pollTime();

    // Docking: moves the single VideoView between this window and a host.
    void dockTo (VideoDockHost& host);
    void undock();
    bool isDocked() const noexcept { return docked_; }
    void setDocked (bool d);

    /** The user closed the window (X); tests call it directly. */
    void closeButtonPressed() override;

    /** Called by the owner when the user closes the window (X). */
    std::function<void()> onUserClosed;
    /** Transport keys typed while this window has focus are forwarded here. */
    std::function<bool (const juce::KeyPress&)> onKeyPressed;
    /** The file finished opening: ok, and whether it simply had no picture. */
    std::function<void (bool ok, bool noTrack)> onVideoOpened;

private:
    void timerCallback() override;
    void visibilityChanged() override;
    void moved() override;
    bool keyPressed (const juce::KeyPress& key) override;

    void updateNotice();                 // audio/video length mismatch warning
    void updateDecodeSize();             // decode at the shown size, not 4K

    std::function<double()> getTime_;
    VideoView videoView;
    sc::VideoFramePump framePump;
    juce::File currentFile;
    double audioDuration_ = -1.0;        // seconds, -1 = unknown
    bool docked_ = false;
    std::map<juce::String, int> rotationByFile;   // user rotation per video, this session

    JUCE_DECLARE_NON_COPYABLE (VideoWindow)
};
