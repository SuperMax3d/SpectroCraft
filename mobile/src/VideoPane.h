// SpectroCraft Mobile - the video next to the waveform and the spectrogram.
//
// It shows either the live camera (Video on: what the next take will film) or
// the open recording's movie, which follows the playhead: it plays with the
// sound and shows the frame under the playhead while paused. MainScreen gives
// it its own part of the views (video::split); a tap on its title row hides
// the picture (the row stays, to show it again).
//
// The pictures are native views (iOS / Android), drawn above the app's own
// drawing, so nothing is drawn over them: the title row sits above the picture.
#pragma once
#include <JuceHeader.h>
#include <functional>
#include <memory>

namespace scm
{
class VideoPane : public juce::Component, private juce::Timer
{
public:
    VideoPane();
    ~VideoPane() override;

    /** The live camera picture (owned by VideoCapture), or nullptr. */
    void setLive (juce::Component* viewer);
    /** The open recording's movie and the document time of its first frame;
        {} = none. A movie still being written (just after a take) is read
        again until it opens. */
    void setMovie (const juce::File& movie, double offsetSecs);
    /** The movie rather than the live picture (while a take is played back). */
    void setPreferMovie (bool);
    /** The title row says REC while the camera films. */
    void setFilming (bool);

    /** Something to show: the live picture, or a movie. */
    bool hasContent() const noexcept { return live != nullptr || movieFile != juce::File(); }
    bool isCollapsed() const noexcept { return collapsed; }
    void setCollapsed (bool);
    /** Width / height of the picture shown (a guess until a movie is open). */
    double pictureAspect() const;
    bool showingMovie() const noexcept;

    /** 30 Hz: the movie follows the recording's playhead. */
    void sync (double docTime, bool docPlaying);

    std::function<void()> onLayoutChanged;            // content, picture shape or collapse changed
    std::function<void (bool)> onCollapsedChanged;    // a tap on the title row

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    enum class Load { None, Loading, Ready, Failed };

    void timerCallback() override;
    void startLoad();
    void updateVisibility();
    juce::String titleText() const;

    juce::Component::SafePointer<juce::Component> live;   // owned by VideoCapture
    std::unique_ptr<juce::VideoComponent> player;
    juce::File movieFile;
    double offset = 0.0;
    Load load = Load::None;
    int loadAttempts = 0, loadGeneration = 0;
    juce::Rectangle<int> movieSize;          // the movie's picture, once known
    bool preferMovie = false, collapsed = false, filming = false;
    double lastSeek = -1.0;
    juce::uint32 lastCorrectionMs = 0;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VideoPane)
};
} // namespace scm
