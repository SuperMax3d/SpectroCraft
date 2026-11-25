#include "VideoPane.h"
#include "MobileStyle.h"
#include "VideoSync.h"

namespace scm
{
namespace
{
constexpr int kLoadRetryMs = 400;
constexpr int kMaxLoadAttempts = 25;      // ~10 s: the camera finishes writing the movie after the take
constexpr juce::uint32 kCorrectionPauseMs = 600;

/** The camera's picture: portrait when the phone is held upright. */
double cameraAspect()
{
   #if JUCE_IOS || JUCE_ANDROID
    const auto o = juce::Desktop::getInstance().getCurrentOrientation();
    if (o == juce::Desktop::upright || o == juce::Desktop::upsideDown)
        return 9.0 / 16.0;
   #endif
    return 16.0 / 9.0;
}
} // namespace

VideoPane::VideoPane()
{
    setOpaque (true);
}

VideoPane::~VideoPane()
{
    *alive = false;
    stopTimer();
    if (live != nullptr) removeChildComponent (live.getComponent());
}

void VideoPane::setLive (juce::Component* viewer)
{
    if (viewer == live.getComponent()) return;
    if (live != nullptr) removeChildComponent (live.getComponent());
    live = viewer;
    if (viewer != nullptr) addChildComponent (viewer);
    updateVisibility();
    resized();
    if (onLayoutChanged) onLayoutChanged();
}

void VideoPane::setMovie (const juce::File& movie, double offsetSecs)
{
    if (movie == movieFile)
    {
        offset = offsetSecs;
        return;
    }
    movieFile = movie;
    offset = offsetSecs;
    player.reset();
    movieSize = {};
    lastSeek = -1.0;
    load = Load::None;
    loadAttempts = 0;
    ++loadGeneration;
    if (movieFile != juce::File()) startLoad();
    else stopTimer();   // the timer only loads
    updateVisibility();
    if (onLayoutChanged) onLayoutChanged();
}

void VideoPane::startLoad()
{
    load = Load::Loading;
    ++loadAttempts;
    player = std::make_unique<juce::VideoComponent> (false);
    player->setInterceptsMouseClicks (false, false);
    addChildComponent (*player);
    resized();
    player->loadAsync (juce::URL (movieFile),
                       [this, token = alive, generation = loadGeneration] (const juce::URL&, juce::Result r)
    {
        if (! *token || generation != loadGeneration) return;
        if (r.wasOk())
        {
            load = Load::Ready;
            player->setAudioVolume (0.0f);   // the recording's WAV is the sound
            lastSeek = -1.0;
            updateVisibility();
            if (onLayoutChanged) onLayoutChanged();
        }
        else
        {
            // Just after a take the camera may still be writing the movie: try
            // again from the timer (which replaces the player; not here, inside
            // the player's own callback).
            load = loadAttempts < kMaxLoadAttempts ? Load::None : Load::Failed;
            if (load == Load::Failed) repaint();
        }
    });
    startTimer (kLoadRetryMs);
}

void VideoPane::timerCallback()
{
    if (load == Load::None && movieFile != juce::File())
    {
        startLoad();
        return;
    }
    // The movie's picture size arrives a little after it opens.
    if (load == Load::Ready && player != nullptr)
    {
        const auto s = player->getVideoNativeSize();
        if (! s.isEmpty() && s != movieSize)
        {
            movieSize = s;
            if (onLayoutChanged) onLayoutChanged();
        }
        if (! movieSize.isEmpty()) stopTimer();
    }
    else if (load != Load::Loading)
        stopTimer();
}

void VideoPane::setPreferMovie (bool b)
{
    if (b == preferMovie) return;
    preferMovie = b;
    updateVisibility();
    if (onLayoutChanged) onLayoutChanged();
}

void VideoPane::setFilming (bool b)
{
    if (b == filming) return;
    filming = b;
    repaint();
}

void VideoPane::setCollapsed (bool c)
{
    if (c == collapsed) return;
    collapsed = c;
    updateVisibility();
    if (onLayoutChanged) onLayoutChanged();
}

bool VideoPane::showingMovie() const noexcept
{
    return movieFile != juce::File() && (live == nullptr || preferMovie);
}

double VideoPane::pictureAspect() const
{
    if (showingMovie() && ! movieSize.isEmpty())
        return (double) movieSize.getWidth() / (double) movieSize.getHeight();
    return cameraAspect();   // the live picture, or a guess for a movie still opening
}

void VideoPane::updateVisibility()
{
    const bool movie = showingMovie();
    if (live != nullptr) live->setVisible (! collapsed && ! movie);
    if (player != nullptr) player->setVisible (! collapsed && movie && load == Load::Ready);
    if (player != nullptr && (collapsed || ! movie) && player->isPlaying()) player->stop();
    repaint();
}

void VideoPane::sync (double docTime, bool docPlaying)
{
    if (player == nullptr || load != Load::Ready || ! player->isVisible()) return;

    video::SyncInput in;
    in.docTime = docTime;
    in.docPlaying = docPlaying;
    in.offset = offset;
    in.duration = player->getVideoDuration();
    in.moviePos = player->getPlayPosition();
    in.moviePlaying = player->isPlaying();
    in.lastSeek = lastSeek;
    const auto now = juce::Time::getMillisecondCounter();
    in.mayCorrect = now - lastCorrectionMs > kCorrectionPauseMs;

    const auto a = video::syncStep (in);
    if (a.pause) player->stop();
    if (a.seek)
    {
        player->setPlayPosition (a.seekTo);
        lastSeek = a.seekTo;
        if (in.moviePlaying || a.play) lastCorrectionMs = now;
    }
    if (a.play) { player->play(); lastSeek = -1.0; }
}

juce::String VideoPane::titleText() const
{
    juce::String t;
    if (showingMovie())
    {
        t = "Video";
        if (load == Load::Failed) t << " - could not be read";
        else if (load != Load::Ready) t << " - opening...";
    }
    else
        t = filming ? "Camera - REC" : "Camera";
    t << (collapsed ? "   (tap to show)" : "   (tap to hide)");
    return t;
}

void VideoPane::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
    auto header = getLocalBounds().removeFromTop (video::kHeaderHeight);
    g.setColour (style::barBackground);
    g.fillRect (header);
    g.setColour (filming && ! showingMovie() ? style::record.brighter (0.3f) : style::text);
    g.setFont (style::font (13.0f, true));
    g.drawFittedText (titleText(), header.reduced (style::kGap, 0), juce::Justification::centredLeft, 1);
}

void VideoPane::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (video::kHeaderHeight);
    if (live != nullptr) live->setBounds (r);
    if (player != nullptr) player->setBounds (r);
}

void VideoPane::mouseUp (const juce::MouseEvent& e)
{
    if (e.getMouseDownY() > video::kHeaderHeight || e.mouseWasDraggedSinceMouseDown()) return;
    setCollapsed (! collapsed);
    if (onCollapsedChanged) onCollapsedChanged (collapsed);
}
} // namespace scm
