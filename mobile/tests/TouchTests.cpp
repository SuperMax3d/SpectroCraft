// SpectroCraft Mobile - unit tests for the touch controls' logic: the
// spectrogram's precision touch (mobile/src/PreciseTouch.h: the reticle,
// carrying on after a lift, the magnifier's place on the screen) and the number
// pad's entry (mobile/src/NumberEntry.h).
#include <JuceHeader.h>
#include "AnalysisRange.h"
#include "NumberEntry.h"
#include "PreciseTouch.h"

namespace
{
using namespace scm::precise;

class PreciseTouchTests : public juce::UnitTest
{
public:
    PreciseTouchTests() : juce::UnitTest ("precision touch", "SpectroCraftMobile") {}

    void runTest() override
    {
        const juce::Rectangle<float> plot (44.0f, 140.0f, 346.0f, 240.0f);

        // ------------------------------------------------------------ reticle
        beginTest ("the reticle moves a quarter as far as the finger, inside the plot");
        {
            Tracker t;
            t.begin (Mode::Point, { 200.0f, 250.0f });
            expect (t.mode() == Mode::Point);
            t.move ({ 240.0f, 230.0f }, plot);
            expectEquals (t.reticle().x, 210.0f);
            expectEquals (t.reticle().y, 245.0f);
            t.move ({ 2000.0f, -2000.0f }, plot);
            expectEquals (t.reticle().x, plot.getRight());
            expectEquals (t.reticle().y, plot.getY());
        }

        // ------------------------------------------------------- carrying on
        beginTest ("a finger put down soon after a lift carries on from the same reticle, as Edge");
        {
            Tracker t;
            t.begin (Mode::Point, { 200.0f, 250.0f });
            t.move ({ 280.0f, 250.0f }, plot);                 // reticle at 220
            expect (t.release (1000) == Mode::Point);
            expect (t.mode() == Mode::Waiting);
            expect (t.active() && ! t.fingerDown());
            expect (! t.expire (1000 + kResumeMs - 1));
            // put down somewhere else: the reticle does not jump to the finger
            expect (t.press ({ 100.0f, 300.0f }, 1000 + kResumeMs - 1));
            expect (t.mode() == Mode::Edge);
            expectEquals (t.reticle().x, 220.0f);
            t.move ({ 140.0f, 300.0f }, plot);
            expectEquals (t.reticle().x, 230.0f);
            // and again, as often as wanted
            expect (t.release (5000) == Mode::Edge);
            expect (t.press ({ 300.0f, 200.0f }, 5100));
            t.move ({ 260.0f, 200.0f }, plot);
            expectEquals (t.reticle().x, 220.0f);
        }

        beginTest ("after the pause, a finger is an ordinary touch");
        {
            Tracker t;
            t.begin (Mode::Point, { 200.0f, 250.0f });
            t.release (1000);
            expect (t.expire (1000 + kResumeMs));
            expect (t.mode() == Mode::None);
            expect (! t.press ({ 200.0f, 250.0f }, 1000 + kResumeMs + 10));

            // a press that comes before the timer noticed the end is not taken either
            t.begin (Mode::Edge, { 200.0f, 250.0f });
            t.release (2000);
            expect (! t.press ({ 200.0f, 250.0f }, 2000 + kResumeMs + 50));
            expect (t.mode() == Mode::None);
        }

        beginTest ("the millisecond counter wrapping round does not end the pause early");
        {
            Tracker t;
            t.begin (Mode::Point, { 200.0f, 250.0f });
            const juce::uint32 nearWrap = 0xFFFFFFFFu - 100u;
            t.release (nearWrap);
            expect (! t.expire (nearWrap + 50u));
            expect (t.press ({ 200.0f, 250.0f }, nearWrap + 500u));   // wrapped past zero
        }

        beginTest ("cancel (a second finger) ends it, and a finger up without precision changes nothing");
        {
            Tracker t;
            t.begin (Mode::Point, { 200.0f, 250.0f });
            t.cancel();
            expect (! t.active());
            expect (t.release (10) == Mode::None);
            expect (! t.active());
        }

        // ---------------------------------------------------------- the lens
        const juce::Rectangle<float> screen (0.0f, 47.0f, 390.0f, 763.0f);   // the safe area
        const float readoutW = 260.0f;

        beginTest ("the lens sits above the finger, with the readout above it");
        {
            const juce::Point<float> finger (200.0f, 400.0f);
            const auto p = place (finger, screen, readoutW);
            expect (p.above);
            expectEquals (p.lens.getCentreX(), finger.x);
            expectEquals (p.lens.getBottom(), finger.y - kLensGap);
            expect (p.readout.getBottom() <= p.lens.getY());
            expect (screen.contains (p.lens) && screen.contains (p.readout));
        }

        beginTest ("short room: still above the finger (over the views above), closer to it");
        {
            // a finger 180 pt under the top: the lens and readout fit with a smaller gap
            const juce::Point<float> finger (200.0f, screen.getY() + kReadoutH + kReadoutGap + kLensSize + 30.0f);
            const auto p = place (finger, screen, readoutW);
            expect (p.above);
            expectEquals (p.lens.getBottom(), finger.y - 30.0f);
            expectEquals (p.readout.getY(), screen.getY());
        }

        beginTest ("no room above: beside the finger, never over it");
        {
            for (float x : { 60.0f, 200.0f, 330.0f })
            {
                const juce::Point<float> finger (x, screen.getY() + 60.0f);
                const auto p = place (finger, screen, readoutW);
                expect (! p.above);
                expect (! p.lens.contains (finger), "x " + juce::String (x));
                expect (p.lens.getX() >= finger.x + kLensSide - 0.01f || p.lens.getRight() <= finger.x - kLensSide + 0.01f,
                        "x " + juce::String (x) + ": " + p.lens.toString());
                expect (screen.contains (p.lens) && screen.contains (p.readout), p.lens.toString());
            }
        }

        beginTest ("near a side: the lens stays on the screen, above the finger");
        {
            for (float x : { 2.0f, 388.0f })
            {
                const auto p = place ({ x, 500.0f }, screen, readoutW);
                expect (p.above);
                expect (screen.contains (p.lens) && screen.contains (p.readout), p.lens.toString());
                expect (p.lens.getBottom() < 500.0f);
            }
        }

        beginTest ("a readout wider than the screen is cut to it");
        {
            const auto p = place ({ 200.0f, 500.0f }, screen, 900.0f);
            expectEquals (p.readout.getWidth(), screen.getWidth());
            expectEquals (p.readout.getX(), screen.getX());
        }
    }
};

class NumberEntryTests : public juce::UnitTest
{
public:
    NumberEntryTests() : juce::UnitTest ("number pad", "SpectroCraftMobile") {}

    void runTest() override
    {
        using scm::numpad::Entry;
        using scm::numpad::format;

        beginTest ("the first key replaces the value shown, the next ones add to it");
        {
            Entry e (format (1000.0, 0), false);
            expectEquals (e.text(), juce::String ("1000"));
            expect (e.isFresh());
            expect (e.value() == 1000.0);
            e.digit (2);
            expect (! e.isFresh());
            e.digit (5);
            e.point();
            e.digit (5);
            expectEquals (e.text(), juce::String ("25.5"));
            expect (e.value() == 25.5);
            e.point();   // only one point
            expectEquals (e.text(), juce::String ("25.5"));
        }

        beginTest ("a range without positive values types negative numbers (dBFS)");
        {
            Entry e (format (-12.0, 1), true);
            expectEquals (e.text(), juce::String ("-12.0"));
            e.digit (2);
            e.digit (0);
            expect (e.value() == -20.0);
            Entry p ("-12.0", true);
            p.point();
            p.digit (5);
            expect (p.value() == -0.5);
        }

        beginTest ("sign, backspace, and what is not a number yet");
        {
            Entry e ("6.0", false);
            e.toggleSign();   // keeps the value
            expect (e.value() == -6.0);
            e.toggleSign();
            expect (e.value() == 6.0);
            Entry b ("94.0", false);
            b.backspace();    // fresh: clears
            expect (b.text().isEmpty());
            expect (! b.value().has_value());
            b.toggleSign();
            expect (! b.value().has_value(), "a lone minus");
            b.point();
            expectEquals (b.text(), juce::String ("-0."));
            b.digit (4);
            expect (b.value() == -0.4);
            b.backspace(); b.backspace(); b.backspace();
            expectEquals (b.text(), juce::String ("-"));
            expect (! b.value().has_value());
        }

        beginTest ("leading zeros are replaced; the length is bounded");
        {
            Entry e ("", false);
            e.digit (0);
            e.digit (0);
            e.digit (7);
            expectEquals (e.text(), juce::String ("7"));
            for (int i = 0; i < 40; ++i) e.digit (9);
            expectEquals (e.text().length(), Entry::kMaxLength);
        }

        beginTest ("an empty start (no value yet)");
        {
            Entry e ({}, false);
            expect (! e.value().has_value());
            e.digit (1);
            e.digit (2);
            e.digit (0);
            expect (e.value() == 120.0);
        }

        beginTest ("format: no decimals rounds, decimals are kept");
        {
            expectEquals (format (999.6, 0), juce::String ("1000"));
            expectEquals (format (94.0, 1), juce::String ("94.0"));
            expectEquals (format (-0.25, 2), juce::String ("-0.25"));
        }
    }
};

class AnalysisRangeTests : public juce::UnitTest
{
public:
    AnalysisRangeTests() : juce::UnitTest ("analysis range", "SpectroCraftMobile") {}

    void runTest() override
    {
        using namespace scm::analysis;
        RangeInput in;
        in.duration = 10.0;
        in.visible = { 2.0, 6.0 };
        in.windowSeconds = 8192.0 / 48000.0;

        beginTest ("nothing chosen: the visible range");
        expect (rangeFor (in) == juce::Range<double> (2.0, 6.0));

        beginTest ("#79: a playhead placed by hand: one FFT window around it, inside the file");
        in.playheadByHand = true;
        in.playhead = 4.0;
        auto r = rangeFor (in);
        expectWithinAbsoluteError (r.getLength(), in.windowSeconds, 1e-9);
        expectWithinAbsoluteError (r.getStart() + r.getLength() * 0.5, 4.0, 1e-9);
        in.playhead = 0.01;
        r = rangeFor (in);
        expectEquals (r.getStart(), 0.0);
        expectWithinAbsoluteError (r.getLength(), in.windowSeconds, 1e-9);
        in.playhead = 10.0;
        r = rangeFor (in);
        expectWithinAbsoluteError (r.getEnd(), 10.0, 1e-9);

        beginTest ("a selection wins over the playhead; recording: the last second");
        in.selection = { 1.0, 3.0 };
        expect (rangeFor (in) == juce::Range<double> (1.0, 3.0));
        in.live = true;
        expect (rangeFor (in) == juce::Range<double> (9.0, 10.0));
        beginTest ("#79: a pinch zooms about the fingers and pans with them");
        {
            // zoom x2 about the middle of a linear axis
            auto r = zoomAxis (0.0, 1000.0, 0.5, 0.5, 2.0, false, 0.0, 1000.0, 10.0);
            expectWithinAbsoluteError (r.first, 250.0, 1e-9);
            expectWithinAbsoluteError (r.second, 750.0, 1e-9);
            // the value under the fingers follows them (pan without zoom)
            r = zoomAxis (200.0, 600.0, 0.5, 0.75, 1.0, false, 0.0, 1000.0, 10.0);
            expectWithinAbsoluteError (r.first + 0.75 * (r.second - r.first), 400.0, 1e-9);
            // log axis: 100 Hz - 10 kHz (a ratio of 100) shrunk log2 (100) times
            // is one octave, still centred on 1 kHz
            r = zoomAxis (100.0, 10000.0, 0.5, 0.5, std::log2 (100.0), true, 5.0, 24000.0, 1.5);
            expectWithinAbsoluteError (std::sqrt (r.first * r.second), 1000.0, 1e-6);
            expectWithinAbsoluteError (r.second / r.first, 2.0, 1e-6);
            // limits: never past the bounds, never narrower than the minimum
            r = zoomAxis (0.0, 1000.0, 0.0, 0.0, 0.1, false, 0.0, 1000.0, 10.0);
            expect (r.first >= 0.0 && r.second <= 1000.0);
            r = zoomAxis (0.0, 1000.0, 0.5, 0.5, 1.0e6, false, 0.0, 1000.0, 10.0);
            expectWithinAbsoluteError (r.second - r.first, 10.0, 1e-9);
            r = zoomAxis (0.0, 1000.0, 0.5, 0.0, 1.0, false, 0.0, 1000.0, 10.0);   // dragged past the start
            expectEquals (r.first, 0.0);
        }

        beginTest ("#79: the stored curve's level at a frequency (bands and spectrum)");
        {
            const std::vector<double> bf { 100.0, 125.0, 160.0 }, bdb { -10.0, -20.0, -30.0 };
            expect (valueAt (bf, bdb, 126.0, true) == -20.0);
            expect (! valueAt (bf, bdb, 1000.0, true).has_value(), "outside every band");
            const std::vector<double> sf { 10.0, 20.0, 30.0 }, sdb { 0.0, -10.0, -20.0 };
            expectWithinAbsoluteError (*valueAt (sf, sdb, 25.0, false), -15.0, 1e-9);
            expect (! valueAt (sf, sdb, 40.0, false).has_value());
            expect (! valueAt ({}, {}, 40.0, false).has_value());
        }
    }
};

static PreciseTouchTests preciseTouchTests;
static AnalysisRangeTests analysisRangeTests;
static NumberEntryTests numberEntryTests;
} // namespace
