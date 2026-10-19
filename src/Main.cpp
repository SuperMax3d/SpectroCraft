/**
 * SpectroCraft - Main entry point
 * Supports offline modes:
 *   --test-spectral <in.wav> <out.png> [width height t0 t1]
 *   --test-noise-suppression <in.wav> <out.wav> [modelId]   (AI builds only)
 *       modelId: denseunet (default) | deepfilternet2 | deepfilternet3
 *   --generate-test-files [dir]
 *   --run-unit-tests
 */

#include <JuceHeader.h>
#include "Application.h"
#include "CrashTrace.h"
#include "MainWindow.h"
#include "SpectralEngine.h"
#include "PngWriter.h"
#include "Settings.h"
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    #include "NoiseSuppression.h"
#endif
#include <vector>
#include <cmath>
#include <exception>

// ---- Offline test hooks (implemented in tests/UnitTests.cpp) ----
extern int scRunUnitTests();
extern bool scGenerateTestFiles (const juce::String& dir);

// juce::File(String) asserts on relative paths, so resolve them against the
// current working directory first (CLI args are often relative).
static juce::File resolvePath (const juce::String& p)
{
    if (p.startsWithChar ('\\') || p.containsChar (':'))
        return juce::File (p);
    return juce::File::getCurrentWorkingDirectory().getChildFile (p);
}

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
// Minimal 32-bit float RIFF writer for the --test-noise-suppression hook.
static bool writeFloatWav(const juce::File& file, int nchans, double rate,
                          const std::vector<std::vector<float>>& data)
{
    if (nchans <= 0 || data.empty() || data[0].empty()) return false;

    const uint32_t frames = (uint32_t) data[0].size();
    const uint32_t blockAlign = (uint32_t) nchans * 4u;
    const uint32_t dataSize = frames * blockAlign;

    file.deleteFile();   // FileOutputStream opens in append mode
    juce::FileOutputStream out (file);
    if (out.failedToOpen()) return false;

    // This build is Windows-only (little-endian), so the native-order writers
    // are already little-endian.
    auto w16 = [&out] (uint16_t v) { out.writeShort ((short) v); };
    auto w32 = [&out] (uint32_t v) { out.writeInt ((int) v); };

    out.write ("RIFF", 4);
    w32 (36 + dataSize);
    out.write ("WAVE", 4);
    out.write ("fmt ", 4);
    w32 (16);                    // fmt chunk size
    w16 (1);                     // PCM format tag
    w16 ((uint16_t) nchans);
    w32 ((uint32_t) rate);
    w32 ((uint32_t) (rate * blockAlign));   // byte rate
    w16 ((uint16_t) blockAlign);
    w16 (32);                    // bits per sample
    out.write ("data", 4);
    w32 (dataSize);

    for (uint32_t f = 0; f < frames; ++f)
        for (int c = 0; c < nchans; ++c)
        {
            int32_t bits;
            std::memcpy (&bits, &data[c][(size_t) f], sizeof (bits));
            out.writeInt ((int) bits);
        }
    return true;
}
#endif

class SpectroCraftApplication : public juce::JUCEApplication
{
public:
    SpectroCraftApplication()
    {
#if defined (_WIN32)
        scCrash::install(); // trace real crashes; pass through driver FATAL_APP_EXIT events
#endif
    }

    const juce::String getApplicationName() override { return "SpectroCraft"; }
    const juce::String getApplicationVersion() override { return "1.0.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        try
        {
            initialiseInternal (commandLine);
        }
        catch (const std::exception& ex)
        {
            std::fprintf (stderr, "[init] C++ exception: %s\n", ex.what());
            throw;
        }
    }

    void initialiseInternal (const juce::String& commandLine)
    {
        // Simple whitespace tokenisation of the command line
        juce::StringArray args;
        {
            juce::String cur;
            for (int i = 0; i < commandLine.length(); ++i)
            {
                const char32_t c = commandLine[(size_t) i];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                {
                    if (!cur.isEmpty()) { args.add (cur); cur.clear(); }
                }
                else
                    cur += c;
            }
            if (!cur.isEmpty())
                args.add (cur);
        }

        for (int i = 0; i < args.size(); ++i)
        {
            if (args[i] == "--test-spectral")
            {
                if (i + 2 >= args.size())
                {
                    std::cout << "usage: --test-spectral <in.wav> <out.png> [w h t0 t1]\n";
                    quit();
                    return;
                }
                const juce::File in  = resolvePath (args[++i]);
                const juce::File out = resolvePath (args[++i]);
                int w = 1024, h = 512;
                double t0 = -1, t1 = -1;
                if (i + 4 < args.size())
                {
                    w = args[++i].getIntValue();
                    h = args[++i].getIntValue();
                    t0 = args[++i].getFloatValue();
                    t1 = args[++i].getFloatValue();
                }

                AudioEngine engine;
                if (!engine.loadFile (in))
                {
                    std::cout << "failed to open " << in.getFullPathName() << "\n";
                    quit();
                    return;
                }

                SpectralEngine spectral;
                Settings s;
                spectral.setColourMap (ColourMap::Classic);

                std::vector<int> chans;
                for (int c = 0; c < engine.numChannels(); ++c)
                    chans.push_back (c);

                double tt0 = t0 < 0 ? 0.0 : t0;
                double tt1 = (t1 <= tt0) ? engine.duration() : t1;
                std::vector<int> allChans;
                for (int c = 0; c < engine.numChannels(); ++c)
                    allChans.push_back (c);
                std::vector<uint8_t> rgba;
                spectral.renderToPixels (engine, s, w, h, tt0, tt1, allChans, rgba);
                const bool ok = PngWriter::writePng (out, w, h, rgba);
                std::cout << (ok ? "wrote " : "failed to write ") << out.getFullPathName() << "\n";
                quit();
                return;
            }
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
            else if (args[i] == "--test-noise-suppression")
            {
                if (i + 2 >= args.size())
                {
                    std::cout << "usage: --test-noise-suppression <in.wav> <out.wav> [modelId]\n";
                    quit();
                    return;
                }
                const juce::File in  = resolvePath (args[++i]);
                const juce::File out = resolvePath (args[++i]);
                std::string modelId = "denseunet";
                if (i + 1 < args.size())
                    modelId = args[++i].toStdString ();

                AudioEngine engine;
                if (!engine.loadFile (in))
                {
                    std::cout << "failed to open " << in.getFullPathName() << "\n";
                    quit();
                    return;
                }

                const int nch = engine.numChannels ();
                const double rate = engine.sampleRate ();
                const int64_t frames = engine.numSamples ();
                std::vector<std::vector<float>> data (nch);
                for (int c = 0; c < nch; ++c)
                {
                    data[c].resize ((size_t) frames);
                    if (!engine.readSamples (c, 0, frames, data[c].data()))
                    {
                        std::cout << "failed to read audio\n";
                        quit();
                        return;
                    }
                }

                int lastPct = -1;
                std::string err;
                const bool isDf = modelId == "deepfilternet2" || modelId == "deepfilternet3";
                const auto progress = [&lastPct] (float f) {
                    const int pct = (int) (f * 100.0);
                    if (pct >= lastPct + 10 || pct == 100)
                    {
                        lastPct = std::max (lastPct, pct);
                        std::cout << "  " << lastPct << "%\n";
                        std::flush (std::cout);
                    }
                    return true;
                };
                const bool ok = isDf
                    ? sc::runDeepFilterPipeline (modelId, "CPU", rate, nch, data,
                                                 0.f, false, progress, &err)
                    : sc::runDenseUnetPipeline ("denseunet", "CPU", rate, nch, data,
                                                progress, &err);

                if (!ok)
                {
                    std::cout << "noise suppression failed: " << err << "\n";
                    quit();
                    return;
                }

                const bool wrote = writeFloatWav (out, nch, rate, data);
                std::cout << (wrote ? "wrote " : "failed to write ") << out.getFullPathName() << "\n";
                quit();
                return;
            }
#endif
            else if (args[i] == "--run-unit-tests")
            {
                const int rc = scRunUnitTests();
                std::cout << (rc == 0 ? "all unit tests passed\n" : "unit tests FAILED\n");
                quit();
                return;
            }
            else if (args[i] == "--generate-test-files")
            {
                const juce::File dir = resolvePath ((i + 1 < args.size()) ? args[i + 1] : ".");
                const bool ok = scGenerateTestFiles (dir.getFullPathName());
                std::cout << (ok ? "generated test files in " : "failed generating in ")
                          << dir.getFullPathName() << "\n";
                quit();
                return;
            }
            else if (args[i].length() > 0 && !args[i].startsWith ("--"))
            {
                // Open audio file passed on the command line. Routing through
                // MainWindow::openFile gives large files their async load with
                // a progress dialog, exactly like File > Open.
                const juce::File f = resolvePath (args[i]);
                if (f.existsAsFile())
                {
                    app = std::make_unique<Application>();
                    mainWindow = std::make_unique<MainWindow> (*app, getApplicationName());
                    mainWindow->openFile (f);
                    return;
                }
            }
        }

        app = std::make_unique<Application>();
        mainWindow = std::make_unique<MainWindow> (*app, getApplicationName());
    }

    void shutdown() override
    {
        mainWindow.reset();
        app.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }
    void anotherInstanceStarted (const juce::String&) override {}

private:
    std::unique_ptr<Application> app;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (SpectroCraftApplication)
