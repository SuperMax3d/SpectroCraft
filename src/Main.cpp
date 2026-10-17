/**
 * SpectroCraft - Main entry point
 * Supports offline modes:
 *   --test-spectral <in.wav> <out.png> [width height t0 t1]
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
                // Open audio file passed on the command line
                const juce::File f = resolvePath (args[i]);
                if (f.existsAsFile())
                {
                    app = std::make_unique<Application>();
                    if (app->openFile (f))
                    {
                        mainWindow = std::make_unique<MainWindow> (*app, getApplicationName());
                        return;
                    }
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
