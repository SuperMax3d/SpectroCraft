// SpectroCraft Mobile - the Map tab: sound maps (docs/sound-map.md).
//
//   View     the map (top or side), a band (a slider: hold the finger still
//            for single steps), absolute or relative levels; tap a point for
//            its values, its take, or to delete it
//   Survey   pictures of the tags (two or more in each), then Build map
//   Measure  countdown, picture, take, picture: a point at a known position
//            (also Background takes, source off, and Reference takes at a
//            fixed spot for the source's drift)
//
// Survey and Measure use the camera (the preview fills the pane), keep the
// screen on and hold the UI in landscape (more tags per picture, the same
// principal point). The tab takes the whole screen below the top bar.
// MainScreen owns the camera and the recordings: the pane asks through the
// hooks below and never opens, closes or films with the camera itself.
#pragma once
#include <JuceHeader.h>
#include "AppContext.h"
#include "SoundMapView.h"
#include "StepSlider.h"
#include "soundmap/SmProject.h"

namespace scm
{
class SoundMapPane : public juce::Component, private juce::Timer
{
public:
    explicit SoundMapPane (AppContext&);
    ~SoundMapPane() override;

    // set by MainScreen
    std::function<bool (const juce::File&)> startTake;                             // no video
    std::function<juce::File()> stopTake;                                          // the saved WAV
    std::function<void (bool, std::function<void (juce::String)>)> useCamera;      // on / off, done (error)
    std::function<void (const juce::File&)> openTake;                              // in the main views

    /** App to the background / back. */
    void suspended();
    void resumed();

    void paint (juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;
    void parentHierarchyChanged() override;

    /** Development hooks (MobileMain): open a map folder, choose the mode (0 View, 1 Survey, 2 Measure). */
    void openMap (const juce::File& folder);
    void setModeForTest (int m) { setMode ((Mode) juce::jlimit (0, 2, m)); }
    /** Development hook: a finger left on the band slider's step `i` (View mode). */
    void touchBandForTest (int i) { bandSlider.touchForTest (i); }
    /** Development hook: the side view. */
    void showSideForTest() { if (projButton.getButtonText() == "Top" && projButton.onClick) projButton.onClick(); }

private:
    enum class Mode { View, Survey, Measure };
    enum class Step { Idle, Countdown, PhotoBefore, Recording, PhotoAfter, Analysing };

    void timerCallback() override;
    void setMode (Mode m);
    void updateCamera();
    void cameraReady (const juce::String& error);
    void releaseCamera();
    /** The pane left the screen (other tab, background): no camera, no half-done step. */
    void leaving();
    void refreshMapList();
    void loadMap (const juce::File& folder);
    void newMap();
    void saveProject();
    void showMenu();
    void showSettings();
    void shareFile (const juce::File& f);
    /** Menu > Export map (ZIP): the options with their sizes, then the ZIP (on
        the worker) and the share sheet. */
    void exportZip();
    void writeZip (std::shared_ptr<const smap::SoundMapProject::ExportPlan>, bool withPointPictures, bool withSurveyPictures);
    bool exporting = false;   // a ZIP is being written: no measuring, no menu meanwhile
    juce::File zipToShare;    // finished while the tab was not on screen: shared when it is back
    void shareReadyZip();
    void updateControls();
    void updateBands();
    void layoutBandRow (juce::Rectangle<int>);
    void showPoint (int number);
    juce::String mapSummary() const;

    // survey
    void takeSurveyPicture();
    void buildMap();

    // measure
    void startMeasure();
    void cancelMeasure();
    void photoBefore();
    void beforeLocated (const juce::Image& image, const std::vector<smap::TagObservation>& tags, int w, int h, int hold,
                        const juce::String& picture);
    void finishTake();
    void photoAfter();
    void analyse();
    /** Rewrites every take's position in its WAV (after the map or the offset changed). */
    void restampAll();
    /** When the open document is this take: its position, in memory and (no
        unsaved edits) in its file, here on the message thread, never from the
        worker, so a save of the document cannot race the worker. False when
        the take is not the open document. */
    bool stampIfOpen (const smap::MapPoint& p, juce::String& error);
    int restampPending = 0;   // restamp jobs queued: no deletion meanwhile
    void measureFailed (const juce::String& why);

    /** Saves the picture (JPEG) and finds its tags on the worker thread; `done` runs on the message thread. */
    void detectInBackground (const juce::Image& image, const juce::File& jpeg,
                             std::function<void (std::vector<smap::TagObservation>, int, int, juce::String)> done);

    AppContext& ctx;
    std::optional<smap::SoundMapProject> project;
    juce::Array<juce::File> mapFolders;
    Mode mode = Mode::View;

    // camera
    bool cameraWanted = false, cameraOn = false, cameraOpening = false;
    bool cameraRequested = false;   // useCamera (true) was called and not yet undone
    juce::Time cameraSince;
    juce::Component::SafePointer<juce::Component> preview;
    int savedOrientations = 0;

    // measure state
    Step step = Step::Idle;
    juce::Time stepUntil;
    int currentPoint = -1;
    smap::PointKind nextKind = smap::PointKind::Point;
    bool afterPictureMissing = false;

    // widgets
    juce::ComboBox mapBox;
    juce::TextButton viewButton { "View" }, surveyButton { "Survey" }, measureButton { "Measure" }, menuButton { "..." };
    juce::Label status;                   // the camera modes' message line
    juce::TextButton photoButton { "Photo" }, buildButton { "Build map" };
    juce::TextButton goButton { "Measure" }, cancelButton { "Cancel" };
    juce::ComboBox kindBox;
    juce::Label info;                     // survey / measure / point details
    SoundMapView view;
    juce::TextButton prevBand { "<" }, nextBand { ">" }, relButton { "Abs" }, projButton { "Top" };
    StepSlider bandSlider;                // the bands, low to high, then LAeq and LZeq apart
    juce::TextButton openTakeButton { "Open take" }, deletePointButton { "Delete point" };
    std::vector<double> bands;            // the slider's steps (nominal Hz, then -1 LA, 0 LZ)

    juce::ThreadPool worker { 1 };
    juce::ScopedMessageBox confirmBox, shareBox;
    std::unique_ptr<juce::AlertWindow> settingsWindow;

    JUCE_DECLARE_WEAK_REFERENCEABLE (SoundMapPane)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundMapPane)
};
} // namespace scm
