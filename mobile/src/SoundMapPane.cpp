#include "SoundMapPane.h"
#include "LiveAudio.h"
#include "MobileStyle.h"
#include "MobileDocument.h"
#include "Recorder.h"
#include "Telemetry.h"
#include "VideoCapture.h"
#include "soundmap/SmMarkersPdf.h"
#include "soundmap/SmTagDetector.h"
#include <set>

namespace scm
{
using namespace smap;

namespace
{
constexpr double kCameraSettleSeconds = 1.5;   // Android: the still session is ready a moment after the preview
constexpr double kRefuseSigmaM = 0.05;         // a point this uncertain is not measured

juce::String idsText (const std::vector<TagObservation>& tags)
{
    juce::StringArray s;
    for (const auto& t : tags) s.add (juce::String (t.id));
    return s.joinIntoString (" ");
}

juce::String metres (double v)
{
    return juce::String (v, 2);
}

juce::String kindName (PointKind k)
{
    return k == PointKind::Background ? "background" : k == PointKind::Reference ? "reference" : "point";
}

int currentHold()
{
    return (int) holdFromDisplay (juce::Desktop::getInstance().getCurrentOrientation());
}

juce::String bandText (double band)
{
    if (band == 0.0) return "LZeq";
    if (band < 0.0) return "LAeq";
    return bandLabel (band) + " Hz";
}
} // namespace

SoundMapPane::SoundMapPane (AppContext& c) : ctx (c)
{
    setOpaque (true);

    mapBox.setTextWhenNothingSelected ("No sound map");
    mapBox.onChange = [this]
    {
        const int i = mapBox.getSelectedId() - 1;
        if (juce::isPositiveAndBelow (i, mapFolders.size()) && (! project || project->folder != mapFolders[i]))
            loadMap (mapFolders[i]);
    };
    viewButton.onClick = [this] { setMode (Mode::View); };
    surveyButton.onClick = [this] { setMode (Mode::Survey); };
    measureButton.onClick = [this] { setMode (Mode::Measure); };
    menuButton.onClick = [this] { showMenu(); };
    for (auto* b : { &viewButton, &surveyButton, &measureButton }) b->setClickingTogglesState (false);

    status.setFont (style::font (14.0f));
    status.setJustificationType (juce::Justification::topLeft);
    status.setMinimumHorizontalScale (1.0f);
    info.setFont (style::font (13.0f));
    info.setJustificationType (juce::Justification::topLeft);
    info.setMinimumHorizontalScale (1.0f);

    photoButton.onClick = [this] { takeSurveyPicture(); };
    buildButton.onClick = [this] { buildMap(); };
    goButton.onClick = [this] { startMeasure(); };
    goButton.setColour (juce::TextButton::buttonColourId, style::record.darker (0.4f));
    cancelButton.onClick = [this] { cancelMeasure(); };
    kindBox.addItem ("Point", 1);
    kindBox.addItem ("Background (source off)", 2);
    kindBox.addItem ("Reference (fixed spot)", 3);
    kindBox.setSelectedId (1, juce::dontSendNotification);

    view.onPointTapped = [this] (int n) { showPoint (n); };
    bandBox.onChange = [this]
    {
        const int i = bandBox.getSelectedId() - 1;
        if (juce::isPositiveAndBelow (i, (int) bands.size())) view.setBand (bands[(size_t) i]);
        showPoint (view.selected());
    };
    prevBand.onClick = [this] { if (bandBox.getSelectedId() > 1) bandBox.setSelectedId (bandBox.getSelectedId() - 1); };
    nextBand.onClick = [this] { if (bandBox.getSelectedId() < bandBox.getNumItems()) bandBox.setSelectedId (bandBox.getSelectedId() + 1); };
    relButton.onClick = [this]
    {
        const bool rel = relButton.getButtonText() == "Abs";
        relButton.setButtonText (rel ? "Rel" : "Abs");
        view.setRelative (rel);
        showPoint (view.selected());
    };
    projButton.onClick = [this]
    {
        const bool side = projButton.getButtonText() == "Top";
        projButton.setButtonText (side ? "Side" : "Top");
        view.setProjection (side ? SoundMapView::Projection::Side : SoundMapView::Projection::Top);
    };
    openTakeButton.onClick = [this]
    {
        if (! project) return;
        if (auto* p = project->point (view.selected()); p != nullptr && openTake)
            openTake (project->file (p->wav));
    };
    deletePointButton.onClick = [this]
    {
        if (! project || step != Step::Idle || restampPending > 0) return;   // a rewrite of the takes is running
        const int n = view.selected();
        if (project->point (n) == nullptr) return;
        auto options = juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::WarningIcon, "Delete point",
            "Delete point " + juce::String (n) + ", its take and its pictures?", "Delete", "Cancel");
        confirmBox = juce::AlertWindow::showScopedAsync (options, [this, n] (int r)
        {
            if (r != 1 || ! project) return;
            for (auto it = project->points.begin(); it != project->points.end(); ++it)
                if (it->number == n)
                {
                    for (const auto& f : { it->wav, it->pictureBefore, it->pictureAfter })
                        if (f.isNotEmpty()) project->file (f).deleteFile();
                    project->points.erase (it);
                    break;
                }
            saveProject();
            view.setSelected (-1);
            view.refresh();
            showPoint (-1);
        });
    };

    for (auto* comp : std::initializer_list<juce::Component*> { &mapBox, &viewButton, &surveyButton, &measureButton, &menuButton,
                                                                &status, &photoButton, &buildButton, &goButton, &cancelButton,
                                                                &kindBox, &info, &view, &prevBand, &nextBand, &bandBox,
                                                                &relButton, &projButton, &openTakeButton, &deletePointButton })
        addChildComponent (comp);
    for (auto* comp : std::initializer_list<juce::Component*> { &mapBox, &viewButton, &surveyButton, &measureButton, &menuButton, &view })
        comp->setVisible (true);

    refreshMapList();
    const auto last = ctx.props.getValue ("soundMap");
    for (const auto& f : mapFolders)
        if (f.getFileName() == last) { loadMap (f); break; }
    if (! project && ! mapFolders.isEmpty()) loadMap (mapFolders.getFirst());
    setMode (Mode::View);
}

SoundMapPane::~SoundMapPane()
{
    stopTimer();
    worker.removeAllJobs (true, 20000);
    if (cameraRequested) releaseCamera();
}

// ------------------------------------------------------------------ maps

void SoundMapPane::refreshMapList()
{
    mapFolders = SoundMapProject::listMaps();
    mapBox.clear (juce::dontSendNotification);
    for (int i = 0; i < mapFolders.size(); ++i) mapBox.addItem (mapFolders[i].getFileName(), i + 1);
    if (project)
        for (int i = 0; i < mapFolders.size(); ++i)
            if (mapFolders[i] == project->folder) mapBox.setSelectedId (i + 1, juce::dontSendNotification);
}

void SoundMapPane::openMap (const juce::File& folder)
{
    loadMap (folder);
    refreshMapList();
}

void SoundMapPane::loadMap (const juce::File& folder)
{
    if (step != Step::Idle) return;
    juce::String err;
    auto p = SoundMapProject::load (folder, err);
    if (! p) { info.setText (err, juce::dontSendNotification); return; }
    project = std::move (p);
    ctx.props.setValue ("soundMap", project->folder.getFileName());
    view.setProject (&*project);
    updateBands();
    showPoint (-1);
    updateControls();
}

void SoundMapPane::newMap()
{
    if (step != Step::Idle) return;
    juce::String err;
    auto p = SoundMapProject::create (err);
    if (! p) { showMessage ("Sound map", err); return; }
    const auto folder = p->folder;
    refreshMapList();
    loadMap (folder);
    refreshMapList();
    telemetry::capture ("soundmap_created", {});
}

void SoundMapPane::saveProject()
{
    if (! project) return;
    juce::String err;
    if (! project->save (err)) info.setText (err, juce::dontSendNotification);
}

juce::String SoundMapPane::mapSummary() const
{
    if (! project) return "No sound map: New map in the menu (...).";
    juce::String s = project->name + ": ";
    if (project->map.built)
        s << (int) project->map.tags.size() << " tags mapped (" << juce::String (project->map.rmsPx, 2) << " px)";
    else
        s << (int) project->survey.size() << " survey pictures, no map yet";
    int points = 0, good = 0, bg = 0, ref = 0;
    for (const auto& p : project->points)
    {
        if (p.pending) continue;
        if (p.kind == PointKind::Background) ++bg;
        else if (p.kind == PointKind::Reference) ++ref;
        else { ++points; if (p.good()) ++good; }
    }
    s << "; " << points << " points (" << good << " good)";
    if (bg > 0) s << ", " << bg << " background";
    if (ref > 0)
    {
        s << ", " << ref << " reference";
        const auto d = project->drift (0.0);
        double worst = 0.0;
        for (const auto& [t, v] : d) worst = std::max (worst, std::abs (v));
        if (d.size() > 1) s << " (drift 250 Hz-2 kHz up to " << juce::String (worst, 1) << " dB)";
    }
    return s + ".";
}

void SoundMapPane::updateBands()
{
    bands = { 0.0, -1.0 };
    std::set<double> have;
    if (project)
        for (const auto& p : project->points)
            for (double c : p.levels.centresHz)
                if (const double n = nominalFor (c); n > 0.0) have.insert (n);
    bands.insert (bands.end(), have.begin(), have.end());
    const double current = view.band();
    bandBox.clear (juce::dontSendNotification);
    int sel = 1;
    for (size_t i = 0; i < bands.size(); ++i)
    {
        bandBox.addItem (bandText (bands[i]), (int) i + 1);
        if (std::abs (bands[i] - current) < 1e-9) sel = (int) i + 1;
    }
    bandBox.setSelectedId (sel, juce::dontSendNotification);
    view.setBand (bands[(size_t) sel - 1]);
}

void SoundMapPane::showPoint (int n)
{
    const MapPoint* p = project ? project->point (n) : nullptr;
    openTakeButton.setVisible (p != nullptr && mode == Mode::View && ! ctx.recorder.isRecording());
    deletePointButton.setVisible (p != nullptr && mode == Mode::View);
    if (mode == Mode::View)
    {
        if (p == nullptr) { info.setText (mapSummary(), juce::dontSendNotification); resized(); return; }
        juce::String s = "P" + juce::String (p->number) + " (" + kindName (p->kind) + ")";
        if (p->located)
            s << ": x " << metres (p->position.x) << ", y " << metres (p->position.y) << ", z " << metres (p->position.z)
              << " m, +/- " << juce::String (p->sigmaM * 1000.0, 0) << " mm";
        else
            s << ": no position";
        if (p->moved()) s << ", moved " << juce::String (p->movedM * 100.0, 1) << " cm";
        else if (p->kind == PointKind::Point && p->located && ! p->checked()) s << ", not checked (no position after the take)";
        const double band = view.band();
        const double v = SoundMapProject::levelOf (*p, band);
        s << "\n" << bandText (band) << ": ";
        if (v > -250.0)
        {
            s << juce::String (v, 1) << " " << p->levels.unit;
            const double snr = project->snrDb (*p, band);
            if (std::isfinite (snr)) s << ", " << juce::String (snr, 0) << " dB above the background";
        }
        else
            s << "no level";
        if (p->note.isNotEmpty()) s << "\n" << p->note;
        info.setText (s, juce::dontSendNotification);
        resized();
    }
}

// ---------------------------------------------------------------- modes

void SoundMapPane::setMode (Mode m)
{
    if (step != Step::Idle && m != mode) return;   // finish or cancel the measurement first
    mode = m;
    viewButton.setToggleState (m == Mode::View, juce::dontSendNotification);
    surveyButton.setToggleState (m == Mode::Survey, juce::dontSendNotification);
    measureButton.setToggleState (m == Mode::Measure, juce::dontSendNotification);
    cameraWanted = m != Mode::View;
    updateCamera();
    view.setSelected (-1);
    showPoint (-1);
    updateControls();
    resized();
}

void SoundMapPane::updateCamera()
{
    auto& desktop = juce::Desktop::getInstance();
    if (cameraWanted && isShowing())
    {
        if (cameraOn || cameraOpening || ! useCamera) return;
        // landscape: a wider picture holds more tags, and every picture of the
        // map then has the principal point on the same side
        if (savedOrientations == 0) savedOrientations = desktop.getOrientationsEnabled();
        desktop.setOrientationsEnabled (juce::Desktop::rotatedClockwise | juce::Desktop::rotatedAntiClockwise);
        juce::Desktop::setScreenSaverEnabled (false);
        cameraOpening = true;
        cameraRequested = true;
        status.setText ("Opening the camera...", juce::dontSendNotification);
        juce::WeakReference<SoundMapPane> weak (this);
        useCamera (true, [weak] (juce::String err) { if (auto* self = weak.get()) self->cameraReady (err); });
    }
    else if (cameraOn || cameraOpening)
        releaseCamera();
}

void SoundMapPane::cameraReady (const juce::String& err)
{
    cameraOpening = false;
    if (err.isNotEmpty() || ! cameraWanted || ! isShowing())
    {
        if (err.isNotEmpty()) status.setText ("Camera: " + err, juce::dontSendNotification);
        releaseCamera();
        updateControls();
        return;
    }
    cameraOn = true;
    cameraSince = juce::Time::getCurrentTime();
    preview = ctx.video.preview();
    if (preview != nullptr) addAndMakeVisible (preview.getComponent());
    status.setText (mode == Mode::Survey ? "Aim at two tags or more, hold still, Photo."
                                         : "Aim at three tags or more, near and far ones in the same picture.",
                    juce::dontSendNotification);
    startTimerHz (10);
    updateControls();
    resized();
}

void SoundMapPane::releaseCamera()
{
    if (preview != nullptr) removeChildComponent (preview.getComponent());
    preview = nullptr;
    cameraOn = false;
    cameraOpening = false;
    if (cameraRequested && useCamera) useCamera (false, {});   // also after a failed open: MainScreen forgets the Map's claim
    cameraRequested = false;
    if (savedOrientations != 0)
    {
        juce::Desktop::getInstance().setOrientationsEnabled (savedOrientations);
        savedOrientations = 0;
    }
    juce::Desktop::setScreenSaverEnabled (true);
    updateControls();
}

void SoundMapPane::visibilityChanged()
{
    if (! isShowing()) leaving();
    else updateCamera();
}

void SoundMapPane::parentHierarchyChanged()
{
    if (! isShowing()) leaving();
    else updateCamera();
}

void SoundMapPane::leaving()
{
    if (step == Step::Countdown || step == Step::Recording) cancelMeasure();
    else if (step == Step::PhotoBefore) measureFailed ("Cancelled.");
    if (cameraRequested) releaseCamera();
}

void SoundMapPane::suspended()
{
    // a take keeps recording in the background; anything before it is cancelled
    if (step == Step::Countdown || step == Step::PhotoBefore) measureFailed ("Interrupted: the app went to the background.");
    if (cameraRequested) releaseCamera();
}

void SoundMapPane::resumed()
{
    updateCamera();
}

// ----------------------------------------------------------------- timer

void SoundMapPane::timerCallback()
{
    const auto now = juce::Time::getCurrentTime();
    if (step == Step::Countdown)
    {
        const double left = (stepUntil - now).inSeconds();
        if (left <= 0.0) photoBefore();
        else status.setText ("Step away, hold still: picture in " + juce::String ((int) std::ceil (left)) + " s",
                             juce::dontSendNotification);
    }
    else if (step == Step::Recording)
    {
        const double left = (stepUntil - now).inSeconds();
        if (left <= 0.0) finishTake();
        else status.setText ("Recording P" + juce::String (currentPoint) + ": " + juce::String ((int) std::ceil (left))
                             + " s left. Keep still and quiet.", juce::dontSendNotification);
    }
    updateControls();
    if (step == Step::Idle && ! cameraOn) stopTimer();
}

void SoundMapPane::updateControls()
{
    const bool camModes = mode != Mode::View;
    const bool settled = cameraOn && (juce::Time::getCurrentTime() - cameraSince).inSeconds() >= kCameraSettleSeconds
                         && ! ctx.video.isTakingStill();
    const bool idle = step == Step::Idle;
    status.setVisible (camModes);
    photoButton.setVisible (mode == Mode::Survey);
    buildButton.setVisible (mode == Mode::Survey);
    kindBox.setVisible (mode == Mode::Measure);
    goButton.setVisible (mode == Mode::Measure);
    cancelButton.setVisible (mode == Mode::Measure);
    for (auto* c : std::initializer_list<juce::Component*> { &prevBand, &nextBand, &bandBox, &relButton, &projButton })
        c->setVisible (mode == Mode::View);
    info.setVisible (true);

    photoButton.setEnabled (project.has_value() && settled && idle);
    buildButton.setEnabled (project.has_value() && ! project->survey.empty() && idle);
    goButton.setEnabled (project.has_value() && settled && idle && project->map.built);
    cancelButton.setEnabled (step == Step::Countdown || step == Step::Recording);
    kindBox.setEnabled (idle);
    mapBox.setEnabled (idle);
    menuButton.setEnabled (idle);
    deletePointButton.setEnabled (idle && restampPending == 0);
    viewButton.setEnabled (idle);
    surveyButton.setEnabled (idle && project.has_value());
    measureButton.setEnabled (idle && project.has_value());
}

// ---------------------------------------------------------------- layout

void SoundMapPane::paint (juce::Graphics& g)
{
    g.fillAll (style::panelBackground);
    if (mode != Mode::View && preview == nullptr)
    {
        // where the preview goes
        auto r = getLocalBounds().withTrimmedTop (style::kTouch + style::kGap);
        r = getWidth() > getHeight() ? r.removeFromLeft ((int) (r.getWidth() * 0.6)) : r.removeFromTop ((int) (r.getHeight() * 0.45));
        g.setColour (style::plotBackground);
        g.fillRect (r.reduced (style::kGap));
    }
}

void SoundMapPane::resized()
{
    auto r = getLocalBounds().reduced (style::kGap);
    auto head = r.removeFromTop (style::kTouch - 4);
    menuButton.setBounds (head.removeFromRight (44));
    head.removeFromRight (4);
    for (auto* b : { &measureButton, &surveyButton, &viewButton })
    {
        b->setBounds (head.removeFromRight (juce::jmin (84, head.getWidth() / 5)));
        head.removeFromRight (3);
    }
    mapBox.setBounds (head);
    r.removeFromTop (style::kGap);

    if (mode == Mode::View)
    {
        if (r.getWidth() > r.getHeight() * 13 / 10)
        {
            // landscape: the map on the left, the rest beside it
            auto col = r.removeFromRight (juce::jmax (300, r.getWidth() * 2 / 5));
            r.removeFromRight (style::kGap);
            view.setBounds (r);
            auto controls = col.removeFromBottom (style::kTouch);
            const int u = controls.getWidth() / 6;
            prevBand.setBounds (controls.removeFromLeft (u).reduced (2, 2));
            nextBand.setBounds (controls.removeFromRight (u).reduced (2, 2));
            bandBox.setBounds (controls.reduced (2, 4));
            col.removeFromBottom (4);
            auto toggles = col.removeFromBottom (style::kTouch - 6);
            relButton.setBounds (toggles.removeFromLeft (toggles.getWidth() / 2).reduced (2, 0));
            projButton.setBounds (toggles.reduced (2, 0));
            col.removeFromBottom (4);
            if (openTakeButton.isVisible())
            {
                auto row = col.removeFromBottom (style::kTouch - 6);
                openTakeButton.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (2, 0));
                deletePointButton.setBounds (row.reduced (2, 0));
                col.removeFromBottom (4);
            }
            info.setBounds (col);
            return;
        }
        auto controls = r.removeFromBottom (style::kTouch);
        const int u = controls.getWidth() / 8;
        prevBand.setBounds (controls.removeFromLeft (u).reduced (2, 2));
        nextBand.setBounds (controls.removeFromRight (u).reduced (2, 2));
        projButton.setBounds (controls.removeFromRight (u + u / 2).reduced (2, 2));
        relButton.setBounds (controls.removeFromRight (u + u / 2).reduced (2, 2));
        bandBox.setBounds (controls.reduced (2, 4));
        r.removeFromBottom (style::kGap);
        if (openTakeButton.isVisible())
        {
            auto row = r.removeFromBottom (style::kTouch - 6);
            openTakeButton.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (2, 0));
            deletePointButton.setBounds (row.reduced (2, 0));
            r.removeFromBottom (4);
        }
        info.setBounds (r.removeFromBottom (54));
        r.removeFromBottom (4);
        view.setBounds (r);
        return;
    }

    // Survey / Measure: the preview, then the controls and a small map
    const bool landscape = r.getWidth() > r.getHeight();
    juce::Rectangle<int> previewArea, side;
    if (landscape)
    {
        previewArea = r.removeFromLeft ((int) (r.getWidth() * 0.6));
        r.removeFromLeft (style::kGap);
        side = r;
    }
    else
    {
        previewArea = r.removeFromTop ((int) (r.getHeight() * 0.45));
        r.removeFromTop (style::kGap);
        side = r;
    }
    if (preview != nullptr) preview->setBounds (previewArea);

    status.setBounds (side.removeFromTop (40));
    side.removeFromTop (4);
    auto row = side.removeFromTop (style::kTouch + 8);
    if (mode == Mode::Survey)
    {
        photoButton.setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (2, 0));
        buildButton.setBounds (row.reduced (2, 0));
    }
    else
    {
        cancelButton.setBounds (row.removeFromRight (row.getWidth() / 3).reduced (2, 0));
        goButton.setBounds (row.reduced (2, 0));
        side.removeFromTop (4);
        kindBox.setBounds (side.removeFromTop (style::kTouch - 8));
    }
    side.removeFromTop (4);
    info.setBounds (side.removeFromTop (juce::jmin (90, side.getHeight() / 2)));
    side.removeFromTop (4);
    view.setBounds (side);
}

// ---------------------------------------------------------------- photos

void SoundMapPane::detectInBackground (const juce::Image& image, const juce::File& jpeg,
                                       std::function<void (std::vector<TagObservation>, int, int, juce::String)> done)
{
    const int maxId = project ? project->settings.tagCount - 1 : -1;
    juce::WeakReference<SoundMapPane> weak (this);
    worker.addJob ([weak, image, jpeg, maxId, done]
    {
        juce::String err;
        {
            jpeg.getParentDirectory().createDirectory();
            jpeg.deleteFile();
            juce::FileOutputStream out (jpeg);
            juce::JPEGImageFormat fmt;
            fmt.setQuality (0.92f);
            if (! out.openedOk() || ! fmt.writeImageToStream (image, out)) err = "The picture could not be saved.";
        }
        const auto gray = toGray (image);
        TagDetector detector (2);
        juce::MessageManager::callAsync ([weak, found = detector.detect (gray, maxId), w = gray.width, h = gray.height, err, done]
        {
            if (weak.get() != nullptr && done) done (found, w, h, err);
        });
    });
}

void SoundMapPane::takeSurveyPicture()
{
    if (! project || ! cameraOn || step != Step::Idle) return;
    const int hold = currentHold();
    const auto jpeg = project->nextSurveyPictureFile();
    jpeg.create();   // reserve the name: the picture is written later, on the worker
    status.setText ("Taking the picture...", juce::dontSendNotification);
    photoButton.setEnabled (false);
    juce::WeakReference<SoundMapPane> weak (this);
    ctx.video.takeStill ([weak, jpeg, hold] (const juce::Image& image, const juce::String& err)
    {
        auto* self = weak.get();
        if (self == nullptr) return;
        if (! image.isValid() || currentHold() != hold)
        {
            jpeg.deleteFile();
            self->status.setText (image.isValid() ? juce::String ("The phone was turned while the picture was taken.")
                                                  : "Picture: " + err, juce::dontSendNotification);
            self->updateControls();
            return;
        }
        self->status.setText ("Finding the tags...", juce::dontSendNotification);
        self->detectInBackground (image, jpeg, [weak, jpeg, hold] (std::vector<TagObservation> tags, int w, int h, juce::String saveErr)
        {
            auto* me = weak.get();
            if (me == nullptr || ! me->project || ! jpeg.isAChildOf (me->project->folder)) { jpeg.deleteFile(); return; }
            if (tags.size() < 2)
            {
                jpeg.deleteFile();
                me->status.setText (juce::String ((int) tags.size()) + " tag in the picture: a survey picture needs two or more.",
                                    juce::dontSendNotification);
            }
            else
            {
                SurveyPicture sp;
                sp.image = me->project->relative (jpeg);
                sp.width = w;
                sp.height = h;
                sp.hold = hold;
                sp.tags = tags;
                me->project->survey.push_back (sp);
                me->saveProject();
                std::set<int> seen;
                for (const auto& s : me->project->survey)
                    for (const auto& t : s.tags) seen.insert (t.id);
                me->status.setText (jpeg.getFileNameWithoutExtension() + ": tags " + idsText (tags), juce::dontSendNotification);
                me->info.setText (juce::String ((int) me->project->survey.size()) + " survey pictures; "
                                  + juce::String ((int) seen.size()) + " of " + juce::String (me->project->settings.tagCount)
                                  + " tags seen." + (saveErr.isNotEmpty() ? " " + saveErr : juce::String()),
                                  juce::dontSendNotification);
            }
            me->updateControls();
        });
    });
}

void SoundMapPane::buildMap()
{
    if (! project || step != Step::Idle) return;
    const auto photos = project->surveyPhotos (true);
    const auto settings = project->surveySettings();
    if (photos.empty()) { info.setText ("Take survey pictures first.", juce::dontSendNotification); return; }
    buildButton.setEnabled (false);
    info.setText ("Building the map from " + juce::String ((int) photos.size()) + " pictures...", juce::dontSendNotification);
    const auto folder = project->folder;
    juce::WeakReference<SoundMapPane> weak (this);
    worker.addJob ([weak, photos, settings, folder]
    {
        auto solved = solveSurvey (photos, settings);
        juce::MessageManager::callAsync ([weak, result = std::move (solved), folder, n = (int) photos.size()]
        {
            auto* self = weak.get();
            if (self == nullptr || ! self->project || self->project->folder != folder) return;
            telemetry::capture ("soundmap_map_built", { { "ok", result.ok }, { "tags", (int) result.tags.size() },
                                                       { "pictures", n } });
            if (! result.ok)
            {
                self->info.setText ("No map: " + juce::String (result.problem), juce::dontSendNotification);
                self->updateControls();
                return;
            }
            self->project->setMap (result);
            self->saveProject();
            self->restampAll();
            self->view.refresh();
            juce::String s;
            s << (int) result.tags.size() << " tags mapped from " << self->project->map.picturesUsed << " pictures, error "
              << juce::String (result.rmsPx, 2) << " px";
            if (result.frame.circle) s << ", circle " << juce::String (2.0 * result.frame.radius, 2) << " m across";
            s << ".";
            for (const auto& w : result.warnings) s << "\n" << juce::String (w);
            self->info.setText (s, juce::dontSendNotification);
            self->updateControls();
        });
    });
}

// --------------------------------------------------------------- measure

void SoundMapPane::startMeasure()
{
    if (! project || step != Step::Idle || ! cameraOn) return;
    nextKind = (PointKind) juce::jlimit (0, 2, kindBox.getSelectedId() - 1);
    if (! project->map.built)
    {
        info.setText ("Build the map first (Survey).", juce::dontSendNotification);
        return;
    }
    if (ctx.recorder.isRecording())
    {
        info.setText ("A recording is running: stop it first.", juce::dontSendNotification);
        return;
    }
    // the phone must not play anything into its own takes
    ctx.audio.generator().setEnabled (false);
    currentPoint = project->nextPointNumber();
    afterPictureMissing = false;
    step = Step::Countdown;
    stepUntil = juce::Time::getCurrentTime() + juce::RelativeTime::seconds (project->settings.countdownSeconds);
    startTimerHz (10);
    timerCallback();
}

void SoundMapPane::cancelMeasure()
{
    if (step == Step::Recording && stopTake)
    {
        const auto saved = stopTake();
        if (project)
        {
            if (saved != juce::File()) saved.deleteFile();
            for (auto it = project->points.begin(); it != project->points.end(); ++it)
                if (it->number == currentPoint)
                {
                    if (it->pictureBefore.isNotEmpty()) project->file (it->pictureBefore).deleteFile();
                    project->points.erase (it);
                    break;
                }
            view.refresh();
            saveProject();
        }
    }
    if (step == Step::Countdown || step == Step::Recording)
    {
        step = Step::Idle;
        status.setText ("Cancelled.", juce::dontSendNotification);
    }
    updateControls();
}

void SoundMapPane::measureFailed (const juce::String& why)
{
    step = Step::Idle;
    status.setText (why, juce::dontSendNotification);
    updateControls();
}

void SoundMapPane::photoBefore()
{
    if (! project) { measureFailed ("No map."); return; }
    step = Step::PhotoBefore;
    status.setText ("Picture...", juce::dontSendNotification);
    const int hold = currentHold();
    const auto jpeg = project->pointFile (currentPoint, "a", "jpg");
    juce::WeakReference<SoundMapPane> weak (this);
    ctx.video.takeStill ([weak, jpeg, hold] (const juce::Image& image, const juce::String& err)
    {
        auto* self = weak.get();
        if (self == nullptr || self->step != Step::PhotoBefore) return;
        if (! image.isValid()) { self->measureFailed ("Picture: " + err); return; }
        if (currentHold() != hold) { self->measureFailed ("The phone was turned while the picture was taken."); return; }
        self->detectInBackground (image, jpeg, [weak, image, jpeg, hold] (std::vector<TagObservation> tags, int w, int h, juce::String)
        {
            auto* me = weak.get();
            if (me == nullptr || me->step != Step::PhotoBefore || ! me->project)
            {
                jpeg.deleteFile();   // cancelled meanwhile: the worker wrote it
                return;
            }
            me->beforeLocated (image, tags, w, h, hold, me->project->relative (jpeg));
        });
    });
}

void SoundMapPane::beforeLocated (const juce::Image&, const std::vector<TagObservation>& tags, int w, int h, int hold,
                                  const juce::String& picture)
{
    const Fix fix = project->locate (tags, w, h, hold);
    {
        // every take of a map has its x, y, z: background and reference takes too
        juce::String why;
        if (! fix.ok)
            why = "No position (" + (fix.problem.isNotEmpty() ? fix.problem : juce::String ("no tag")) + "). Aim at three tags or more.";
        else if (fix.sigmaM > kRefuseSigmaM)
            why = "The position is too uncertain (+/- " + juce::String (fix.sigmaM * 100.0, 0)
                  + " cm, " + juce::String (fix.tags) + " tags): aim at more tags, near and far ones together.";
        if (why.isNotEmpty())
        {
            project->file (picture).deleteFile();
            measureFailed (why);
            return;
        }
    }
    MapPoint p;
    p.number = currentPoint;
    p.kind = nextKind;
    p.pending = true;
    const auto wav = project->pointFile (currentPoint, {}, "wav");
    p.wav = project->relative (wav);
    p.pictureBefore = picture;
    p.time = juce::Time::getCurrentTime();
    p.hold = hold;
    p.width = w;
    p.height = h;
    p.tagsBefore = tags;
    p.before = fix;
    p.input = ctx.audio.inputName();
    p.updatePosition();
    project->points.push_back (p);
    view.refresh();   // the view keeps no pointer into the points, but its numbers follow
    saveProject();    // a lost take leaves a pending point, not an unknown file

    if (! startTake || ! startTake (wav))
    {
        project->file (picture).deleteFile();
        project->points.pop_back();
        view.refresh();
        saveProject();
        measureFailed ("The take did not start (a recording is running?).");
        return;
    }
    step = Step::Recording;
    stepUntil = juce::Time::getCurrentTime() + juce::RelativeTime::seconds (project->settings.takeSeconds);
    timerCallback();
}

void SoundMapPane::finishTake()
{
    // the take is saved with its position (the picture before it) in its metadata
    if (project)
        if (auto* pt = project->point (currentPoint); pt != nullptr && ctx.recorder.isRecording())
            if (auto* d = ctx.document.doc()) project->stampDocument (*d, *pt);
    const auto saved = stopTake ? stopTake() : juce::File();
    auto* p = project ? project->point (currentPoint) : nullptr;
    if (p == nullptr) { measureFailed ("The point is gone."); return; }
    if (saved == juce::File())
    {
        p->note = "The take was not saved.";
        saveProject();
        measureFailed (p->note);
        return;
    }
    if (saved != project->file (p->wav)) p->note = "Take saved as " + saved.getFileName() + " (the map folder was not writable).";
    p->wav = saved.isAChildOf (project->folder) ? project->relative (saved) : saved.getFullPathName();
    saveProject();
    photoAfter();
}

void SoundMapPane::photoAfter()
{
    step = Step::PhotoAfter;
    if (! cameraOn || ! project)
    {
        afterPictureMissing = true;
        analyse();
        return;
    }
    status.setText ("Picture after the take...", juce::dontSendNotification);
    const int hold = currentHold();
    const auto jpeg = project->pointFile (currentPoint, "b", "jpg");
    juce::WeakReference<SoundMapPane> weak (this);
    ctx.video.takeStill ([weak, jpeg, hold] (const juce::Image& image, const juce::String&)
    {
        auto* self = weak.get();
        if (self == nullptr || self->step != Step::PhotoAfter) return;
        if (! image.isValid() || currentHold() != hold) { self->afterPictureMissing = true; self->analyse(); return; }
        self->detectInBackground (image, jpeg, [weak, jpeg, hold] (std::vector<TagObservation> tags, int w, int h, juce::String)
        {
            auto* me = weak.get();
            if (me == nullptr || me->step != Step::PhotoAfter || ! me->project) return;
            if (auto* p = me->project->point (me->currentPoint))
            {
                p->pictureAfter = me->project->relative (jpeg);
                if (hold == p->hold && w == p->width && h == p->height)
                {
                    p->tagsAfter = tags;
                    p->after = me->project->locate (tags, w, h, hold);
                }
                else
                    p->note = "The phone was turned between the pictures.";
                p->updatePosition();
            }
            me->analyse();
        });
    });
}

void SoundMapPane::analyse()
{
    step = Step::Analysing;
    status.setText ("Levels...", juce::dontSendNotification);
    auto* point = project ? project->point (currentPoint) : nullptr;
    if (point == nullptr) { measureFailed ("The point is gone."); return; }
    if (afterPictureMissing && point->note.isEmpty()) point->note = "No picture after the take.";
    const auto wav = project->file (point->wav);
    const auto folder = project->folder;
    const int number = currentPoint;
    const SoundMapProject snapshot = *project;   // for the position's final value (both pictures)
    const MapPoint finalPoint = *point;
    juce::String openStampErr;
    const bool stampedHere = stampIfOpen (finalPoint, openStampErr);
    juce::WeakReference<SoundMapPane> weak (this);
    worker.addJob ([weak, wav, folder, number, snapshot, finalPoint, stampedHere, openStampErr]
    {
        juce::String stampErr = openStampErr;
        if (! stampedHere) snapshot.stampWav (finalPoint, stampErr);
        juce::String why;
        auto measured = analyseTake (wav, 0.5, 0.3, why);
        juce::MessageManager::callAsync ([weak, levels = std::move (measured), err = why, stampErr, folder, number]
        {
            auto* self = weak.get();
            if (self == nullptr) return;
            self->step = Step::Idle;
            if (! self->project || self->project->folder != folder) { self->updateControls(); return; }
            auto* p = self->project->point (number);
            if (p == nullptr) { self->updateControls(); return; }
            p->levels = levels;
            p->pending = false;
            if (! levels.valid()) p->note = (p->note.isNotEmpty() ? p->note + " " : juce::String()) + err;
            if (stampErr.isNotEmpty())
                p->note = (p->note.isNotEmpty() ? p->note + " " : juce::String()) + "Position not written into the take: " + stampErr;
            self->saveProject();
            self->updateBands();
            self->view.setSelected (number);
            self->view.refresh();

            juce::String s = "P" + juce::String (number) + " (" + kindName (p->kind) + ")";
            if (p->located)
                s << ": x " << metres (p->position.x) << ", y " << metres (p->position.y) << ", z " << metres (p->position.z)
                  << " m, +/- " << juce::String (p->sigmaM * 1000.0, 0) << " mm, " << p->before.tags << " tags";
            if (p->moved()) s << "\nMOVED " << juce::String (p->movedM * 100.0, 1) << " cm between the pictures: measure again.";
            if (levels.valid()) s << "\nLZeq " << juce::String (levels.lzeq, 1) << " " << levels.unit << ", LAeq " << juce::String (levels.laeq, 1);
            if (p->note.isNotEmpty()) s << "\n" << p->note;
            self->info.setText (s, juce::dontSendNotification);
            self->status.setText ("Done. Next point: move, aim, Measure.", juce::dontSendNotification);
            telemetry::capture ("soundmap_point", { { "kind", kindName (p->kind) }, { "located", p->located },
                                                   { "tags", p->before.tags }, { "moved", p->moved() } });
            self->updateControls();
        });
    });
}

// ------------------------------------------------------- positions in WAVs

bool SoundMapPane::stampIfOpen (const MapPoint& p, juce::String& error)
{
    if (! project || p.pending || p.wav.isEmpty() || ctx.recorder.isRecording()) return false;
    const auto wav = project->file (p.wav);
    auto* d = ctx.document.doc();
    if (d == nullptr || ctx.document.file() != wav) return false;
    project->stampDocument (*d, p);
    if (! ctx.document.hasUnsavedChanges())
        SoundMapProject::writeStamp (*d, wav, error);
    // with unsaved edits the document's next save writes the position with them
    return true;
}

void SoundMapPane::restampAll()
{
    if (! project || project->points.empty()) return;
    juce::File skip;
    juce::String openErr;
    for (const auto& p : project->points)
        if (stampIfOpen (p, openErr)) { skip = project->file (p.wav); break; }
    const SoundMapProject snapshot = *project;
    const auto folder = project->folder;
    ++restampPending;
    updateControls();
    juce::WeakReference<SoundMapPane> weak (this);
    worker.addJob ([weak, snapshot, folder, skip, openErr]
    {
        int failed = openErr.isNotEmpty() ? 1 : 0;
        for (const auto& p : snapshot.points)
        {
            if (p.pending || p.wav.isEmpty() || snapshot.file (p.wav) == skip) continue;
            juce::String err;
            if (! snapshot.stampWav (p, err)) ++failed;
        }
        juce::MessageManager::callAsync ([weak, folder, failed]
        {
            auto* self = weak.get();
            if (self == nullptr) return;
            self->restampPending = std::max (0, self->restampPending - 1);
            self->updateControls();
            if (! self->project || self->project->folder != folder) return;
            if (failed > 0)
                self->info.setText (self->info.getText() + "\n" + juce::String (failed)
                                    + " takes could not get their new position (missing or read-only).",
                                    juce::dontSendNotification);
        });
    });
}

// ------------------------------------------------------------------ menu

void SoundMapPane::shareFile (const juce::File& f)
{
    if (! f.existsAsFile()) return;
    shareBox = juce::ContentSharer::shareFilesScoped ({ juce::URL (f) }, [] (bool ok, const juce::String& err)
    {
        if (! ok && err.isNotEmpty()) showMessage ("Share", err);
    }, &menuButton);
}

void SoundMapPane::showMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "New map");
    m.addItem (2, "Settings...", project.has_value());
    m.addSeparator();
    m.addItem (3, "Tag sheets (PDF)...");
    m.addItem (4, "Source signal (periodic pink noise WAV)...");
    m.addItem (5, "Export CSV...", project.has_value() && ! project->points.empty());
    m.addItem (6, "Locate the points again", project.has_value() && project->map.built);
    m.addSeparator();
    m.addItem (7, "Delete this map...", project.has_value() && restampPending == 0);
    juce::WeakReference<SoundMapPane> weak (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton), [weak] (int r)
    {
        auto* self = weak.get();
        if (self == nullptr || r == 0) return;
        auto& pr = self->project;
        switch (r)
        {
            case 1: self->newMap(); break;
            case 2: self->showSettings(); break;
            case 3:
            {
                const double size = pr ? pr->settings.tagSizeMm : 160.0;
                const int count = pr ? pr->settings.tagCount : 24;
                const auto dir = pr ? pr->folder : SoundMapProject::rootFolder();
                dir.createDirectory();
                const auto f = dir.getChildFile ("tags-36h11-" + juce::String (count) + "x" + juce::String (size, 0) + "mm.pdf");
                std::vector<int> ids;
                for (int i = 0; i < count; ++i) ids.push_back (i);
                if (writeMarkersPdf (f, ids, size)) self->shareFile (f);
                else showMessage ("Tag sheets", "The PDF could not be written.");
                break;
            }
            case 4:
            {
                const auto f = SoundMapProject::rootFolder().getChildFile ("periodic-pink-48k-3min.wav");
                SoundMapProject::rootFolder().createDirectory();
                self->info.setText ("Writing the source signal...", juce::dontSendNotification);
                self->worker.addJob ([weak, f]
                {
                    const bool ok = f.existsAsFile() || writePeriodicPink (f, 48000.0, 180.0);
                    juce::MessageManager::callAsync ([weak, f, ok]
                    {
                        auto* me = weak.get();
                        if (me == nullptr) return;
                        me->info.setText (ok ? "Play this file in a loop on the fixed loudspeaker (another device)."
                                             : "The source signal could not be written.", juce::dontSendNotification);
                        if (ok) me->shareFile (f);
                    });
                });
                break;
            }
            case 5:
            {
                const auto f = pr->folder.getChildFile ("export.csv");
                if (f.replaceWithText (pr->toCsv())) self->shareFile (f);
                break;
            }
            case 6:
                pr->relocatePoints();
                self->saveProject();
                self->restampAll();
                self->view.refresh();
                self->showPoint (-1);
                break;
            case 7:
            {
                auto options = juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::WarningIcon, "Delete map",
                    "Delete " + pr->name + ", its pictures and its takes?", "Delete", "Cancel");
                self->confirmBox = juce::AlertWindow::showScopedAsync (options, [weak] (int answer)
                {
                    auto* me = weak.get();
                    if (me == nullptr || answer != 1 || ! me->project || me->step != Step::Idle || me->restampPending > 0) return;
                    const auto folder = me->project->folder;
                    me->view.setProject (nullptr);
                    me->project.reset();
                    folder.deleteRecursively();
                    me->refreshMapList();
                    if (! me->mapFolders.isEmpty()) me->loadMap (me->mapFolders.getFirst());
                    me->refreshMapList();
                    me->setMode (Mode::View);
                });
                break;
            }
            default: break;
        }
    });
}

void SoundMapPane::showSettings()
{
    if (! project) return;
    const auto& st = project->settings;
    settingsWindow = std::make_unique<juce::AlertWindow> ("Sound map settings", project->name, juce::MessageBoxIconType::NoIcon);
    settingsWindow->addTextEditor ("tag", juce::String (st.tagSizeMm, 1), "Tag size: black square (mm)");
    settingsWindow->addTextEditor ("count", juce::String (st.tagCount), "Number of tags (ids 0 to N-1)");
    settingsWindow->addTextEditor ("take", juce::String (st.takeSeconds, 1), "Take length (s)");
    settingsWindow->addTextEditor ("countdown", juce::String (st.countdownSeconds, 1), "Countdown before the picture (s)");
    settingsWindow->addTextEditor ("mic", juce::String (st.micOffsetCm.x, 1) + ", " + juce::String (st.micOffsetCm.y, 1) + ", "
                                              + juce::String (st.micOffsetCm.z, 1),
                                   "Microphone from the back camera, cm: x, y, z (screen view: x right, y up, z out)");
    settingsWindow->addTextEditor ("height", juce::String (st.tagCentreHeightCm, 1), "Height of the tag centres above z = 0 (cm)");
    settingsWindow->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    settingsWindow->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::WeakReference<SoundMapPane> weak (this);
    settingsWindow->enterModalState (true, juce::ModalCallbackFunction::create ([weak] (int r)
    {
        auto* self = weak.get();
        if (self == nullptr || self->settingsWindow == nullptr) return;
        auto& w = *self->settingsWindow;
        if (r == 1 && self->project)
        {
            auto& s = self->project->settings;
            const auto old = s;
            const auto num = [&w] (const char* k) { return w.getTextEditorContents (k).trim().getDoubleValue(); };
            s.tagSizeMm = juce::jlimit (20.0, 600.0, num ("tag"));
            s.tagCount = juce::jlimit (3, TagDetector::numIds(), (int) num ("count"));
            s.takeSeconds = juce::jlimit (3.0, 120.0, num ("take"));   // one period of the source after the trimmed ends
            s.countdownSeconds = juce::jlimit (0.0, 10.0, num ("countdown"));
            const auto parts = juce::StringArray::fromTokens (w.getTextEditorContents ("mic"), ", ;", {});
            juce::StringArray v;
            for (const auto& t : parts) if (t.trim().isNotEmpty()) v.add (t.trim());
            if (v.size() == 3)
                s.micOffsetCm = { juce::jlimit (-30.0, 30.0, v[0].getDoubleValue()), juce::jlimit (-30.0, 30.0, v[1].getDoubleValue()),
                                  juce::jlimit (-30.0, 30.0, v[2].getDoubleValue()) };
            s.tagCentreHeightCm = juce::jlimit (-100.0, 300.0, num ("height"));
            juce::String note;
            const bool geometry = std::abs (s.tagSizeMm - old.tagSizeMm) > 1e-9 || std::abs (s.tagCentreHeightCm - old.tagCentreHeightCm) > 1e-9
                                  || s.tagCount != old.tagCount;
            if (geometry && self->project->map.built)
            {
                self->project->map.built = false;
                note = "The tags changed: build the map again (Survey > Build map).";
            }
            else if ((s.micOffsetCm - old.micOffsetCm).norm() > 1e-9 && self->project->map.built)
            {
                self->project->relocatePoints();
                self->restampAll();
                note = "The points were located again with the new microphone offset.";
            }
            self->saveProject();
            self->view.refresh();
            self->info.setText (note.isNotEmpty() ? note : self->mapSummary(), juce::dontSendNotification);
            self->updateControls();
        }
        self->settingsWindow.reset();
    }), false);
}
} // namespace scm
