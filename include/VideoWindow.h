/**
 * SpectroCraft - Video window
 *
 * A separate, resizable window that shows the picture of the active
 * document's video file at the document time: the playback position while
 * playing, otherwise the time under the mouse over the spectrogram /
 * waveform (when "Video Follows Mouse Hover" is on) or the playhead. The
 * sound itself is the document audio played by the AudioEngine, so the
 * picture is a silent slave of that time and stays in step with edits of
 * the selection, the playhead and the transport.
 */
#pragma once

#include <JuceHeader.h>

#include "VideoFrames.h"

#include <functional>
#include <map>

/** Small round-cornered icon button drawn over the picture (rotate, zoom). */
class VideoToolButton : public juce::Button
{
public:
    enum class Icon { RotateCW, ZoomIn, ZoomOut };
    VideoToolButton (const juce::String& name, Icon i);
    void paintButton (juce::Graphics& g, bool highlighted, bool down) override;

private:
    Icon icon;
};

/** Paints one video frame letterboxed (with the track's display rotation
    plus the user's), the frame time and a status line. The corner buttons
    rotate the picture by 90 degrees and zoom in / out; a drag pans the
    zoomed picture and a double-click fits it again. */
class VideoView : public juce::Component
{
public:
    static constexpr float kZoomStep = 1.25f;
    static constexpr float kMaxZoom = 8.0f;

    VideoView();

    /** trackRotation: the file's own display rotation (0/90/180/270). */
    void setFrame (const sc::VideoFrame& f, int trackRotation);
    void clearFrame();
    /** Centered message shown instead of (or, when no frame, as) the picture. */
    void setMessage (const juce::String& m);
    /** Small notice at the bottom (e.g. audio and video lengths differ). */
    void setNotice (const juce::String& n);

    const juce::String& getMessage() const noexcept { return message; }
    const juce::String& getNotice() const noexcept { return notice; }
    /** Where the picture is drawn (letterboxed, zoomed, panned), empty
        without a frame. */
    juce::Rectangle<float> pictureBounds() const;

    // ---- Corner buttons (also callable directly) ----
    void rotateClockwise();
    void zoomIn();
    void zoomOut();
    /** Back to the whole picture (zoom 1, no pan). */
    void resetZoom();
    float getZoom() const noexcept { return zoom; }
    juce::Point<float> getPan() const noexcept { return pan; }
    /** Offset of the zoomed picture from the centre, clamped so no empty band
        opens where there is picture left (what a drag does). */
    void setPan (juce::Point<float> p);
    /** Rotation chosen with the button, on top of the track's (0..270). */
    int getUserRotation() const noexcept { return userRotation; }
    void setUserRotation (int degrees);
    /** Track rotation + user rotation, 0/90/180/270. */
    int totalRotation() const noexcept { return (trackRotation + userRotation) % 360; }
    /** Called when the rotation button changes the user rotation. */
    std::function<void (int)> onUserRotationChanged;
    /** The view was resized or zoomed: the decode size may need to change. */
    std::function<void()> onDisplaySizeChanged;
    /** Longest side the picture can take on screen, in logical pixels
        (view size x zoom): what the decoder must deliver for a sharp image. */
    int neededLongSide() const;

    VideoToolButton& rotateButton() noexcept { return rotateBtn; }
    VideoToolButton& zoomInButton() noexcept { return zoomInBtn; }
    VideoToolButton& zoomOutButton() noexcept { return zoomOutBtn; }

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;

private:
    juce::Rectangle<float> fittedBounds() const;   // zoom 1, centred
    void setZoomKeepingCentre (float z);
    void clampPan();
    void updateButtons();

    sc::VideoFrame frame;
    int trackRotation = 0, userRotation = 0;
    float zoom = 1.0f;
    juce::Point<float> pan, panAtDragStart;
    juce::String message, notice;
    VideoToolButton rotateBtn { "Rotate 90 degrees", VideoToolButton::Icon::RotateCW };
    VideoToolButton zoomInBtn { "Zoom in", VideoToolButton::Icon::ZoomIn };
    VideoToolButton zoomOutBtn { "Zoom out", VideoToolButton::Icon::ZoomOut };
};

class VideoWindow : public juce::DocumentWindow,
                    private juce::Timer
{
public:
    /** timeSource gives the document time to show (seconds); it is polled
        at 60 Hz while the window is visible. */
    explicit VideoWindow (std::function<double()> timeSource);
    ~VideoWindow() override;

    /** Shows this file's picture (empty File = the active document has no
        video). Re-setting the current file is a no-op. displayName is the
        document name shown in the title. */
    void setVideoFile (const juce::File& f, const juce::String& displayName);
    const juce::File& getVideoFile() const noexcept { return videoFile; }

    /** Length of the document audio, to flag a mismatch with the video
        (after edits that changed the duration). <= 0 = unknown. */
    void setAudioDuration (double seconds);

    /** Message thread: the file set by setVideoFile() finished opening.
        ok = a picture can be shown; otherwise noTrack tells whether the file
        merely has no video track (nothing worth showing). */
    std::function<void (bool ok, bool noTrack)> onVideoOpened;
    /** The user closed the window with its close button. */
    std::function<void()> onUserClosed;
    /** Key presses the window does not use (transport shortcuts). */
    std::function<bool (const juce::KeyPress&)> onKeyPressed;

    sc::VideoFramePump& pump() noexcept { return framePump; }
    VideoView& view() noexcept { return videoView; }
    /** Polls the time source once (tests; the timer does it at 60 Hz). */
    void pollTime();

    void closeButtonPressed() override;
    bool keyPressed (const juce::KeyPress& key) override;
    void visibilityChanged() override;

private:
    void timerCallback() override;
    void updateNotice();
    void updateDecodeSize();
    void moved() override;

    std::function<double()> timeSource;
    std::map<juce::String, int> rotationByFile;   // user rotation per video, this session
    sc::VideoFramePump framePump;
    VideoView videoView;
    juce::File videoFile;
    juce::String docName;
    double audioDuration = 0.0;
};
