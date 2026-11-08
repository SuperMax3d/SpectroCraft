/**
 * SpectroCraft - Video window
 *
 * A separate resizable window showing the picture of a video file in step
 * with its audio: it polls MainWindow::videoTimeNow() at 60 Hz and asks its
 * own sc::VideoFramePump for "the frame displayed at t". The pump decodes on
 * a worker thread; this class only displays.
 *
 * Several video tracks can be attached to one document (VideoTracksPanel):
 * the main audio defines the timeline, each track is a coloured band that
 * can be dragged left/right to change its time offset. Clicking a band makes
 * that picture render above; clicking outside any band shows the topmost
 * available video at the current time instead. Each track keeps its own
 * zoom/pan/rotation/contrast/brightness, and "Solo" plays the track's own
 * audio instead of the document's while it is displayed.
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
#include "FileMeta.h"

//==============================================================================
/** Applies contrast/brightness to a frame in place (see VideoWindow).
    Neutral values (contrast 1, brightness 0) are a no-op. */
void applyImageAdjustments (juce::Image& image, double contrast, double brightness);

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
    /** Sets the user's rotation directly (per-track restore); multiples of 90. */
    void setUserRotation (int degrees);
    /** Restores a saved zoom/pan pair for this track (no callback fired). */
    void setAdjustments (float zoomIn, juce::Point<float> panIn);
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
    /** User rotation in degrees; the owner saves it per track. */
    std::function<void (int)> onUserRotationChanged;
    /** The user zoomed/panned; the owner persists it for the displayed track. */
    std::function<void()> onAdjustmentChanged;
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
/** The track list under the picture: a time ruler with a green band for the
    main audio's length, and one coloured band per attached video. Bands can
    be dragged left/right to change their offset; the wheel zooms the whole
    timeline about the cursor; clicking a band selects it (clicking empty
    space clears the selection); right-click shows the track context menu
    (on the ruler/empty area: only the zoom items). */
class VideoTracksPanel : public juce::Component
{
public:
    /** One row in the panel. durationSecs < 0 = not known yet; the band is
        then drawn to the right edge of the view and treated as covering at
        least the audio length for selection purposes. */
    struct Track
    {
        juce::File file;
        double offsetSecs = 0.0;
        double durationSecs = -1.0;
    };

    /** Context-menu item ids (handleContextMenuId). */
    enum { kMenuZoomIn = 1, kMenuZoomOut, kMenuZoomFit, kMenuSyncAudio,
           kMenuAlign, kMenuCopyAudio, kMenuAddTrack, kMenuRemoveTrack,
           kMenuSolo, kMenuAdjustImage, kMenuAlwaysOnTop, kMenuDock };

    VideoTracksPanel();
    ~VideoTracksPanel() override;

    void setTracks (std::vector<Track> t);
    const std::vector<Track>& getTracks() const noexcept { return tracks_; }
    /** Per-track solo flags (for the context menu's checkmark). */
    void setSoloFlags (std::vector<bool> s);
    void setSelected (int i);            // -1 = no explicit selection
    int getSelected() const noexcept { return selected_; }
    void setAudioDuration (double secs);

    /** The main document's playhead position; drawn as a red line from the
        ruler to the bottom of the panel (-1 = hidden). */
    void setPlayhead (double tSecs);
    double getPlayhead() const noexcept { return playhead_; }

    /** The visible time range in seconds. */
    juce::Range<double> getViewRange() const noexcept { return view_; }
    void zoomIn();                       // one step about the centre
    void zoomOut();                      // one step; never wider than fit
    void zoomFit();                      // back to [0, max(audio, tracks)]
    double timeAtX (float x) const;
    float xForTime (double t) const;

    /** The track whose band contains the point (-1 = ruler or empty area). */
    int trackIndexAt (const juce::Point<int>& p) const;

    /** Runs one context-menu item for `trackIdx` (-1 = ruler/empty area);
        the popup and the tests both call this. */
    void handleContextMenuId (int id, int trackIdx);

    // Public like VideoView's: the regression tests drive them directly.
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    // Owner callbacks (message thread).
    std::function<void(int)> onSelectTrack;            // -1 = clear selection
    std::function<void(int, double)> onOffsetChanged;  // index, new offset secs
    /** A non-zoom context-menu item was chosen: id and track index (-1 =
        ruler/empty area). */
    std::function<void(int, int)> onMenuItem;
    /** The time ruler was clicked at this document time: move the playhead.
        Only called when wired (the standalone window's panel always is). */
    std::function<void(double)> onSeekTime;

    // Window-level actions offered by the track list's context menu too, so
    // they stay reachable while docked even without a video loaded. Kept in
    // sync with VideoView's by the owner.
    bool docked = false;
    std::function<void(bool)> onToggleAlwaysOnTop;
    std::function<bool()> isAlwaysOnTop;
    std::function<void()> onDockToggle;

private:
    static constexpr int kRulerH = 20;   // time ruler row height
    static constexpr int kRowH = 26;     // one track row height
    static constexpr double kZoomStep = 1.5;

    juce::Rectangle<int> rowRect (int i) const;
    void zoomAbout (double tSecs, double factor);
    void showContextMenuAt (const juce::Point<int>& at);
    void paintRuler (juce::Graphics& g, const juce::Rectangle<int>& r) const;

    std::vector<Track> tracks_;
    int selected_ = -1;
    double audioDuration_ = 0.0;
    juce::Range<double> view_{ 0.0, 1.0 };   // visible time range (seconds)

    bool dragBand_ = false;               // band-offset drag in flight
    int dragIdx_ = -1;
    double dragStartOffset_ = 0.0;
    juce::Point<int> dragStartPt_;
    float snapX_ = -1.0f;                 // shift-drag anchor indicator (-1 = none)
    std::vector<bool> soloFlags_;
    double playhead_ = -1.0;              // main document's cursor, secs

    JUCE_DECLARE_NON_COPYABLE (VideoTracksPanel)
};

//==============================================================================
/** "Adjust Image" dialog: live contrast/brightness sliders for the displayed
    video track, plus Apply to All / Reset / Close. */
class VideoAdjustDialog : public juce::DocumentWindow
{
public:
    explicit VideoAdjustDialog (juce::Component* parent);

    void setValues (double contrast, double brightness);

    /** Live slider values (message thread). */
    std::function<void(double, double)> onValuesChanged;   // contrast, brightness
    std::function<void()> onApplyToAll;
    std::function<void()> onReset;

private:
    void resized() override;

    juce::Label contrastLbl_, brightnessLbl_;
    juce::Slider contrast_, brightness_;
    juce::TextButton applyAll_, reset_, closeBtn_;

    JUCE_DECLARE_NON_COPYABLE (VideoAdjustDialog)
};

/**==========================================================================
 * Content of the standalone window: picture on top, track list below.
 */
class VideoContentHost : public juce::Component
{
public:
    static constexpr int kPanelH = 20 + 4 * 26;   // ruler + four track rows

private:
    void resized() override
    {
        if (getNumChildComponents() < 2) return;
        const int h = getHeight();
        auto* pic  = getChildComponent (0);
        auto* list = getChildComponent (1);
        pic->setBounds (0, 0, getWidth(), std::max (1, h - kPanelH));
        list->setBounds (0, h - kPanelH, getWidth(), kPanelH);
    }
};

/** Tab host for a docked video window: the picture on top and the track
    list below, mirroring VideoContentHost's layout. */
class VideoDockHost : public juce::Component
{
public:
    void attach (juce::Component* c)
    {
        if (c != nullptr && !isParentOf (c)) addChildComponent (c);
        resized();
    }
    juce::Component* child() const noexcept
    {   return getNumChildComponents() > 0 ? getChildComponent (0) : nullptr; }

private:
    void resized() override
    {
        if (getNumChildComponents() >= 2)
        {
            const int h = getHeight();
            auto* pic  = getChildComponent (0);
            auto* list = getChildComponent (1);
            pic->setBounds (0, 0, getWidth(), std::max (1, h - VideoContentHost::kPanelH));
            list->setBounds (0, h - VideoContentHost::kPanelH, getWidth(), VideoContentHost::kPanelH);
        }
        else
            for (auto* c : getChildren()) c->setBounds (getLocalBounds());
    }
};

//==============================================================================
/** The standalone Video window. Owns the frame pump and polls the document
    time from getTime(); the picture is shown in its VideoView, which can be
    moved into a dock host while this window stays hidden (docked mode).
    Below the view sits the track list (VideoTracksPanel) with one row per
    attached video. */
class VideoWindow : public juce::DocumentWindow,
                    private juce::Timer,
                    private juce::FileDragAndDropTarget
{
public:
    explicit VideoWindow (std::function<double()> getTime);
    ~VideoWindow() override;

    /** Replaces the document's video tracks. `selected` is the transient UI
        selection (-1 = follow the topmost available track). The displayed
        picture switches to whatever covers the current time. */
    void setTracks (const std::vector<sc::VideoTrackData>& t, int selected);
    /** Single-file convenience (tests / legacy callers): one implicit track.
        Existing adjustments for the same file are kept. */
    void setVideoFile (const juce::File& file, const juce::String& docName);

    /** Audio length in seconds; a mismatch with the video track raises a
        notice on the picture and sets the ruler's green band. */
    void setAudioDuration (double secs);

    VideoView& view() noexcept { return videoView; }
    VideoTracksPanel& getTracksPanel() noexcept { return tracksPanel; }
    sc::VideoFramePump& pump() noexcept { return framePump; }
    juce::File getVideoFile() const noexcept { return currentFile; }
    /** The track currently shown (-1 = none). */
    int displayedTrack() const noexcept { return displayed_; }
    /** Which track covers document time t: the explicit selection if it is
        in range, else the topmost (last) available track. -1 = none. */
    int displayedTrackAt (double tSecs) const;

    /** Asks the pump for the frame at the owner's current time. */
    void pollTime();
    /** Re-evaluates which track is displayed and switches files if needed. */
    void refreshDisplayed();

    // Per-track picture adjustments ---------------------------------------
    double getContrast() const noexcept { return contrast_; }
    double getBrightness() const noexcept { return brightness_; }
    /** Sets the displayed track's contrast/brightness live (re-applies to
        the current frame). */
    void setAdjustmentsLive (double contrast, double brightness);

    // Docking: moves the single VideoView between this window and a host.
    void dockTo (VideoDockHost& host);
    void undock();
    bool isDocked() const noexcept { return docked_; }
    void setDocked (bool d);

    /** The user closed the window (X); tests call it directly. */
    void closeButtonPressed() override;

    // FileDragAndDropTarget -------------------------------------------------
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    /** Called by the owner when the user closes the window (X). */
    std::function<void()> onUserClosed;
    /** Transport keys typed while this window has focus are forwarded here. */
    std::function<bool (const juce::KeyPress&)> onKeyPressed;
    /** The file finished opening: ok, and whether it simply had no picture. */
    std::function<void (bool ok, bool noTrack)> onVideoOpened;
    /** The user selected a track in the panel (-1 = cleared). */
    std::function<void(int)> onSelectTrackChanged;
    /** A band was dragged to a new offset: index and seconds. */
    std::function<void(int, double)> onTrackOffsetChanged;
    /** The displayed track's picture adjustments changed; the updated data
        is passed back for persistence. */
    std::function<void(const sc::VideoTrackData&)> onTrackAdjustmentsSaved;
    /** A non-zoom track-menu action: panel menu id, track index (-1 = none).
        See VideoTracksPanel's enum. */
    std::function<void(int, int)> onTrackMenuAction;
    /** The displayed track changed (index, -1 = none); the owner may update
        solo playback and status. */
    std::function<void(int)> onDisplayedTrackChanged;
    /** A video file was dropped onto this window. */
    std::function<void(const juce::File&)> onVideoDropped;

private:
    void timerCallback() override;
    void visibilityChanged() override;
    void moved() override;
    bool keyPressed (const juce::KeyPress& key) override;

    void updateNotice();                 // audio/video length mismatch warning
    void updateDecodeSize();             // decode at the shown size, not 4K
    void syncPanelDurations();           // push tracks/durations/solo into the panel
    void applyTrackAdjustments (int i);  // restore track i's zoom/pan/rotation
    void saveDisplayedAdjustments();     // persist view state into displayed_
    void switchTo (int d);               // change the displayed track

    std::function<double()> getTime_;
    std::unique_ptr<VideoContentHost> contentHost_;
    VideoView videoView;
    VideoTracksPanel tracksPanel;
    sc::VideoFramePump framePump;
    juce::File currentFile;
    double audioDuration_ = -1.0;        // seconds, -1 = unknown
    bool docked_ = false;

    std::vector<sc::VideoTrackData> tracks_;   // the document's video tracks
    int selected_ = -1;                        // explicit selection (-1 = auto)
    int displayed_ = -1;                       // track currently shown
    std::map<juce::String, double> durations_;  // file path -> seconds (session)

    double contrast_ = 1.0;                  // displayed track's adjustments
    double brightness_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE (VideoWindow)
};
