// SpectroCraft Mobile - unit tests for the video pane's geometry and for the
// movie's sync with the recording's playhead (mobile/src/VideoSync.h).
#include <JuceHeader.h>
#include "LoopBuffer.h"
#include "VideoSync.h"

namespace
{
using scm::video::split;
using scm::video::syncStep;
using scm::video::SyncInput;
using scm::video::kHeaderHeight;

class VideoTests : public juce::UnitTest
{
public:
    VideoTests() : juce::UnitTest ("video pane", "SpectroCraftMobile") {}

    void runTest() override
    {
        // ------------------------------------------------------------ layout
        beginTest ("a phone held upright: the portrait picture is a column beside the views");
        {
            const juce::Rectangle<int> views (0, 100, 390, 330);
            const auto s = split (views, 9.0 / 16.0, false);
            expect (s.sideColumn);
            expectEquals (s.pane.getX(), 0);
            expectEquals (s.pane.getHeight(), views.getHeight());
            expect (s.pane.getWidth() <= views.getWidth() / 2, "at most half the width");
            expect (s.pane.getWidth() >= 150, "a picture large enough to see: " + s.pane.toString());
            expectEquals (s.rest.getRight(), views.getRight());
            expect (! s.rest.intersects (s.pane));
            // the picture under the title row keeps its shape (no black bars)
            const double pictureAspect = (double) s.pane.getWidth() / (double) (s.pane.getHeight() - kHeaderHeight);
            expect (pictureAspect <= 9.0 / 16.0 + 0.01, juce::String (pictureAspect));
        }

        beginTest ("a landscape movie on an upright phone: a strip on top, as wide as the views");
        {
            const juce::Rectangle<int> views (0, 100, 390, 330);
            const auto s = split (views, 16.0 / 9.0, false);
            expect (! s.sideColumn);
            expectEquals (s.pane.getY(), views.getY());
            expectEquals (s.pane.getWidth(), views.getWidth());
            expect (s.pane.getHeight() <= views.getHeight() / 2, "at most half the height");
            expectEquals (s.rest.getBottom(), views.getBottom());
            expect (s.rest.getY() > s.pane.getBottom() - 1);
        }

        beginTest ("tall views (tablet portrait): a landscape picture fills the width");
        {
            const juce::Rectangle<int> views (0, 0, 800, 700);
            const auto s = split (views, 16.0 / 9.0, false);
            expect (! s.sideColumn);
            expectEquals (s.pane.getHeight(), 350);   // half the height
            expect (s.rest.getHeight() >= 340);
        }

        beginTest ("collapsed: only the title row, on top");
        {
            const juce::Rectangle<int> views (10, 50, 390, 330);
            for (double aspect : { 9.0 / 16.0, 16.0 / 9.0 })
            {
                const auto s = split (views, aspect, true);
                expect (s.pane == juce::Rectangle<int> (10, 50, 390, kHeaderHeight), s.pane.toString());
                expectEquals (s.rest.getY(), 50 + kHeaderHeight + scm::video::kGap);
                expectEquals (s.rest.getWidth(), 390);
            }
            expect (split ({}, 1.0, false).pane.isEmpty());
        }

        // ------------------------------------------------------------ sync
        beginTest ("play: the movie starts at the playhead minus its offset");
        {
            SyncInput in;
            in.docTime = 3.0; in.docPlaying = true; in.offset = 0.25; in.duration = 10.0;
            in.moviePos = 0.0; in.moviePlaying = false;
            const auto a = syncStep (in);
            expect (a.play && ! a.pause);
            expect (a.seek);
            expectWithinAbsoluteError (a.seekTo, 2.75, 1e-9);
        }

        beginTest ("playing: a small drift is left alone, a large one is corrected (not right after one)");
        {
            SyncInput in;
            in.docTime = 5.0; in.docPlaying = true; in.offset = 0.0; in.duration = 10.0;
            in.moviePlaying = true;
            in.moviePos = 5.1;
            auto a = syncStep (in);
            expect (! a.seek && ! a.play && ! a.pause);
            in.moviePos = 5.5;
            a = syncStep (in);
            expect (a.seek);
            expectWithinAbsoluteError (a.seekTo, 5.0, 1e-9);
            in.mayCorrect = false;
            expect (! syncStep (in).seek);
        }

        beginTest ("paused: the frame under the playhead, without a seek per tick");
        {
            SyncInput in;
            in.docTime = 4.0; in.docPlaying = false; in.offset = 1.0; in.duration = 10.0;
            in.moviePos = 0.0; in.moviePlaying = true;
            auto a = syncStep (in);
            expect (a.pause);
            expect (a.seek);
            expectWithinAbsoluteError (a.seekTo, 3.0, 1e-9);
            // the player reports its old position while the seek runs: no second seek
            in.moviePlaying = false; in.moviePos = 0.0; in.lastSeek = 3.0;
            a = syncStep (in);
            expect (! a.seek && ! a.pause && ! a.play);
            in.docTime = 4.5;   // the playhead moved
            a = syncStep (in);
            expect (a.seek);
            expectWithinAbsoluteError (a.seekTo, 3.5, 1e-9);
        }

        beginTest ("outside the movie: its first frame before, a frame near its end after");
        {
            SyncInput in;
            in.offset = 2.0; in.duration = 5.0;
            in.docTime = 1.0; in.docPlaying = true; in.moviePos = 1.0;
            auto a = syncStep (in);
            expect (! a.play);
            expect (a.seek);
            expectWithinAbsoluteError (a.seekTo, 0.0, 1e-9);
            in.docTime = 9.0; in.moviePos = 0.0; in.moviePlaying = true;
            a = syncStep (in);
            expect (a.pause && ! a.play);
            expect (a.seek);
            expectWithinAbsoluteError (a.seekTo, 5.0 - scm::video::kLastFrameMargin, 1e-9);
            // a negative offset (a wrapped loop: the movie started before the file)
            in = {};
            in.offset = -20.0; in.duration = 30.0; in.docTime = 0.0; in.docPlaying = true;
            a = syncStep (in);
            expect (a.play);
            expectWithinAbsoluteError (a.seekTo, 20.0, 1e-9);
        }

        beginTest ("duration not known yet: plays from the playhead");
        {
            SyncInput in;
            in.docTime = 2.0; in.docPlaying = true;
            const auto a = syncStep (in);
            expect (a.play);
            expectWithinAbsoluteError (a.seekTo, 2.0, 1e-9);
        }

        // ------------------------------------------------------------ loop take
        beginTest ("a wrapped loop dropped the frames before the file's first frame");
        {
            scm::LoopPlan plan;
            plan.capacity = 48000;
            plan.written = 30000;
            expectEquals (plan.droppedAtStop(), (int64_t) 0);
            plan.written = 48000;
            expectEquals (plan.droppedAtStop(), (int64_t) 0);
            plan.written = 96000 + 123;
            expectEquals (plan.droppedAtStop(), (int64_t) 48123);
            scm::LoopPlan unlimited;
            unlimited.written = 1000000;
            expectEquals (unlimited.droppedAtStop(), (int64_t) 0);
        }
    }
};

static VideoTests videoTests;
} // namespace
