// SpectroCraft Mobile - application entry point (issue #73).
#include <JuceHeader.h>
#include "AppContext.h"
#include "AudioRoute.h"
#include "LiveAudio.h"
#include "MainScreen.h"
#include "MobileDocument.h"
#include "MobileStyle.h"
#include "Recorder.h"
#include "TimeViewport.h"
#include "VideoCapture.h"

namespace scm
{
juce::String AppContext::inputKey() const
{
    if (route::platformHasRoutes())
    {
        const auto id = route::currentInputId();
        for (const auto& in : route::availableInputs())
            if (in.id == id) return in.name;
    }
    return audio.inputName();
}

cal::InputCalibration AppContext::currentInputCalibration() const
{
    return calibrations.get (inputKey());
}

void showMessage (const juce::String& title, const juce::String& text)
{
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon, title, text);
}

class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (const juce::String& name, juce::Component* content)
        : DocumentWindow (name, style::appBackground, DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content, false);
       #if JUCE_IOS || JUCE_ANDROID
        setFullScreen (true);
       #else
        setResizable (true, true);
        centreWithSize (430, 900);   // a phone-sized window for development
       #endif
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

class MobileApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        juce::PropertiesFile::Options o;
        o.applicationName = "SpectroCraftMobile";
        o.filenameSuffix = ".settings";
        o.folderName = "SpectroCraftMobile";
        o.osxLibrarySubFolder = "Application Support";
        props = std::make_unique<juce::PropertiesFile> (o);

        audio = std::make_unique<LiveAudio>();
        document = std::make_unique<MobileDocument>();
        recorder = std::make_unique<Recorder> (*audio, *document);
        view = std::make_unique<TimeViewport>();
        video = std::make_unique<VideoCapture>();
        context = std::make_unique<AppContext> (AppContext { *audio, *document, *recorder, *view, *video, *props,
                                                             cal::Store (props.get()) });
        auto* screen = new MainScreen (*context);
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), screen);

        audio->onDeviceChanged = [screen] { screen->audioReady ({}); };
       #if ! (JUCE_IOS || JUCE_ANDROID)
        // Development hooks (desktop builds only):
        //   SC_MOBILE_NO_AUDIO=1          do not open the microphone
        //   SC_MOBILE_OPEN=<wav>          open a file at start
        //   SC_MOBILE_TAB=<0..5>          show a panel (Files, Analyse, Generate, Calibrate, Input, Edit)
        //   SC_MOBILE_SNAPSHOT=<png>      after 2.5 s, write the window to a PNG and quit
        //   SC_MOBILE_SIZE=<w>x<h>        window size (e.g. 844x390 for landscape)
        const auto env = [] (const char* n) { return juce::SystemStats::getEnvironmentVariable (n, {}); };
        if (env ("SC_MOBILE_SIZE").containsChar ('x'))
            mainWindow->setSize (env ("SC_MOBILE_SIZE").upToFirstOccurrenceOf ("x", false, false).getIntValue(),
                                 env ("SC_MOBILE_SIZE").fromFirstOccurrenceOf ("x", false, false).getIntValue());
        if (env ("SC_MOBILE_OPEN").isNotEmpty())
        {
            juce::String err;
            document->open (juce::File (env ("SC_MOBILE_OPEN")), err);
        }
        if (env ("SC_MOBILE_TAB").isNotEmpty())
            screen->showTab (env ("SC_MOBILE_TAB").getIntValue());
        if (env ("SC_MOBILE_SNAPSHOT").isNotEmpty())
        {
            const juce::File png (env ("SC_MOBILE_SNAPSHOT"));
            juce::Timer::callAfterDelay (2500, [this, png]
            {
                if (mainWindow != nullptr)
                    if (auto* c = mainWindow->getContentComponent())
                    {
                        const auto img = c->createComponentSnapshot (c->getLocalBounds(), true, 2.0f);
                        png.deleteFile();
                        juce::FileOutputStream out (png);
                        juce::PNGImageFormat().writeImageToStream (img, out);
                    }
                quit();
            });
        }
        if (env ("SC_MOBILE_NO_AUDIO") == "1")
            return;
       #endif
        audio->open (props->getIntValue ("inputChannels", 1), [screen] (juce::String err)
        {
            screen->audioReady (err);
        });
    }

    void shutdown() override
    {
        if (recorder != nullptr && recorder->isRecording())
        {
            juce::String err;
            recorder->stop (err);
        }
        mainWindow.reset();
        context.reset();
        video.reset();
        recorder.reset();
        audio.reset();      // the callback stops before the document goes
        document.reset();
        view.reset();
        if (props) props->saveIfNeeded();
        props.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void suspended() override
    {
        if (props) props->saveIfNeeded();
        if (mainWindow != nullptr)
            if (auto* s = dynamic_cast<MainScreen*> (mainWindow->getContentComponent())) s->suspended();
    }

    void resumed() override {}

    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String&) override {}

private:
    style::LookAndFeel lookAndFeel;
    std::unique_ptr<juce::PropertiesFile> props;
    std::unique_ptr<LiveAudio> audio;
    std::unique_ptr<MobileDocument> document;
    std::unique_ptr<Recorder> recorder;
    std::unique_ptr<TimeViewport> view;
    std::unique_ptr<VideoCapture> video;
    std::unique_ptr<AppContext> context;
    std::unique_ptr<MainWindow> mainWindow;
};
} // namespace scm

START_JUCE_APPLICATION (scm::MobileApplication)
