// SpectroCraft Mobile - unit tests for a microphone's compensation against a
// reference microphone (issue #80; mobile/src/MicCompensation.h).
#include <JuceHeader.h>
#include "MicCompensation.h"
#include <cmath>

namespace
{
using namespace scm::miccomp;

class MicCompensationTests : public juce::UnitTest
{
public:
    MicCompensationTests() : juce::UnitTest ("mic compensation", "SpectroCraftMobile") {}

    void runTest() override
    {
        beginTest ("a maker's response file: CSV, semicolons with decimal commas, FRD with a phase column");
        {
            juce::String err;
            auto r = parseResponse ("Frequency (Hz),Response (dB)\n20,-1.5\n1000,0\n10000,2.0\n", err);
            expectEquals ((int) r.hz.size(), 3, err);
            expectWithinAbsoluteError (r.at (1000.0), 0.0, 1e-9);
            expectWithinAbsoluteError (r.at (100000.0), 2.0, 1e-9, "flat beyond the last point");
            expectWithinAbsoluteError (r.at (10.0), -1.5, 1e-9, "flat before the first point");
            // half way between 1 kHz and 10 kHz in log frequency
            expectWithinAbsoluteError (r.at (std::sqrt (1000.0 * 10000.0)), 1.0, 1e-9);
            r = parseResponse ("* MIC-123 calibration\nHz;dB\n100;-0,5\n1000;0,25\n", err);
            expectEquals ((int) r.hz.size(), 2);
            expectWithinAbsoluteError (r.at (100.0), -0.5, 1e-9);
            expectWithinAbsoluteError (r.at (1000.0), 0.25, 1e-9);
            r = parseResponse ("\"Sens Factor =-1.2dB\"\n1000 0.0 -3.5\n20 -2.0 12.0\n", err);
            expectEquals ((int) r.hz.size(), 2, "sorted, the phase ignored");
            expectWithinAbsoluteError (r.hz.front(), 20.0, 1e-9);
            err = {};
            r = parseResponse ("no numbers here\n", err);
            expect (r.empty() && err.isNotEmpty());
        }

        beginTest ("the comparison: the calibration from the middle bands, the rest per band");
        {
            // synthetic 1/3-octave takes: a broadband sound L (dBFS at the reference)
            Bands ref, phone;
            Response refResponse;
            refResponse.hz = { 20.0, 20000.0 };
            refResponse.db = { 2.0, 2.0 };   // the reference reads 2 dB high everywhere
            for (int i = 0; i < 31; ++i)
            {
                const double f = 1000.0 * std::pow (2.0, (i - 17) / 3.0);
                const double level = -30.0 - 0.5 * std::abs (i - 17);
                const double phoneDeviation = f >= 5000.0 ? 5.0 : 0.0;   // the phone mic is brighter
                ref.hz.push_back (f); ref.db.push_back (level);
                phone.hz.push_back (f); phone.db.push_back (level - 10.0 + phoneDeviation);
            }
            ref.db[0] = phone.db[0] = -150.0;   // no sound in the lowest band
            juce::String err;
            const auto c = compare (ref, 120.0, refResponse, phone, err);
            expect (c.valid(), err);
            // true level = L + 120 - 2; the phone reads L - 10 there: full scale = 128 dB-SPL
            expectWithinAbsoluteError (c.fullScaleDbSpl, 128.0, 1e-9);
            expectWithinAbsoluteError (c.at (1000.0), 0.0, 1e-9);
            expectWithinAbsoluteError (c.at (8000.0), -5.0, 1e-9, "the brightness is taken out");
            expect (! c.trusted[0], "a band without sound is not trusted");
            expectWithinAbsoluteError (c.correctionDb[0], c.correctionDb[1], 1e-9, "it takes its neighbour's value");
            expectWithinAbsoluteError (c.fullScalePa(), 2e-5 * std::pow (10.0, 128.0 / 20.0), 1e-12);

            // nothing heard between 250 Hz and 4 kHz
            auto quiet = phone;
            for (size_t b = 0; b < quiet.db.size(); ++b)
                if (quiet.hz[b] >= 200.0 && quiet.hz[b] <= 5000.0) quiet.db[b] = -150.0;
            err = {};
            expect (! compare (ref, 120.0, refResponse, quiet, err).valid() && err.isNotEmpty());
            // a reference calibration that cannot be right
            err = {};
            expect (! compare (ref, 10.0, refResponse, phone, err).valid() && err.contains ("out of range"));
        }

        beginTest ("the comparison from real takes: the same noise, 10 dB quieter at the phone");
        {
            const double rate = 48000.0;
            std::vector<float> a ((size_t) (rate * 4.0)), b (a.size());
            juce::Random rnd (7);
            for (size_t i = 0; i < a.size(); ++i)
            {
                a[i] = (rnd.nextFloat() * 2.0f - 1.0f) * 0.3f;
                b[i] = a[i] * (float) std::pow (10.0, -10.0 / 20.0);
            }
            const auto ra = thirdOctaveDbfs (a.data(), (int) a.size(), rate);
            const auto rb = thirdOctaveDbfs (b.data(), (int) b.size(), rate);
            expect (ra.hz.size() >= 24, "1/3-octave bands: " + juce::String ((int) ra.hz.size()));
            juce::String err;
            const auto c = compare (ra, 110.0, {}, rb, err);
            expect (c.valid(), err);
            expectWithinAbsoluteError (c.fullScaleDbSpl, 120.0, 0.05);
            double worst = 0.0;
            for (double v : c.correctionDb) worst = std::max (worst, std::abs (v));
            expect (worst < 0.05, "flat: no correction, worst " + juce::String (worst, 3) + " dB");
        }

        beginTest ("the CSV file and the recording's metadata keep it");
        {
            Compensation c;
            c.input = "iPhone \"Bottom\", Microphone";
            c.reference = "MVX2U (response: M30, 2026.csv)";
            c.when = juce::Time (2026, 10, 26, 10, 30);
            c.fullScaleDbSpl = 121.375;
            c.hz = { 20.0, 1000.0, 20000.0 };
            c.correctionDb = { 3.25, 0.0, -6.5 };
            c.trusted = { false, true, true };
            c.referenceDbSpl = { 50.0, 80.0, 60.0 };
            c.phoneDbfs = { -70.0, -41.4, -55.0 };
            juce::String err;
            const auto back = Compensation::fromCsv (c.toCsv(), err);
            expect (back.valid(), err);
            expectEquals (back.input, c.input);
            expectEquals (back.reference, c.reference);
            expectWithinAbsoluteError (back.fullScaleDbSpl, 121.375, 1e-3);
            expectWithinAbsoluteError (back.at (20000.0), -6.5, 1e-9);
            expect (! back.trusted[0] && back.trusted[1]);
            expect (back.when == c.when);
            const auto meta = Compensation::fromVar (c.toVar());
            expect (meta.valid());
            expectWithinAbsoluteError (meta.at (20.0), 3.25, 1e-9);
            err = {};
            expect (! Compensation::fromCsv ("frequency_hz,correction_db\n100,1\n", err).valid() && err.isNotEmpty(),
                    "no input or calibration: not a compensation file");
        }
    }
};

static MicCompensationTests micCompensationTests;
} // namespace
