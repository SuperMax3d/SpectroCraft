// Regression tests for the per-file axis model behind the Scale form
// (include/AxisScale.h, Application::setAxisEntries). Included by
// tests/UnitTests.cpp; uses its helpers (CHECK, progress, writeWav, ...).

static bool regrAxes_near (double a, double b, double eps = 1e-6)
{
    return std::abs (a - b) <= eps;
}

static sc::axes::Entry regrAxes_axis (sc::axes::Kind k, const juce::String& name,
                                      int units, double scale, sc::axes::Type type)
{
    sc::axes::Entry e;
    e.kind   = k;
    e.name   = name;
    e.units  = units;
    e.scale  = scale;
    e.type   = type;
    e.visible = true;
    return e;
}

// ---------------------------------------------------------------- A1: model

static void testAxisModelAndSharedGeometry()
{
    progress ("regression axes#A1: default axes, shared widths, undo");
    const juce::File store = makeTempFile ("axes_model.recent");
    store.deleteFile ();
    const juce::File wav = makeTempFile ("axes_model.wav");
    writeWav (wav, 1, 8000.0, 8000,
              [] (int, int64_t f, double) { return 0.25f * (float) std::sin ((double) f * 0.2); });

    Application app (store);
    CHECK (app.openFile (wav), "axes: open wav");
    if (app.doc() == nullptr) return;

    // A file without stored axis state gets exactly one axis per kind, with the
    // built-in names, and the historical 60 px strip / 24 px ruler.
    CHECK (sc::axes::countKind (app.axisEntries, sc::axes::Kind::Time) == 1,
           "one time axis by default");
    CHECK (sc::axes::countKind (app.axisEntries, sc::axes::Kind::Amplitude) == 1,
           "one amplitude axis by default");
    CHECK (sc::axes::countKind (app.axisEntries, sc::axes::Kind::Frequency) == 1,
           "one frequency axis by default");
    CHECK (app.axisStripWidth() == kLeftAxisWidth,
           "one axis per kind keeps the 60 px strip: " + juce::String (app.axisStripWidth()));
    CHECK (app.rulerStackHeight() == 24,
           "one time axis keeps the 24 px ruler: " + juce::String (app.rulerStackHeight()));

    // Adding a second amplitude axis widens the SHARED strip: every track
    // reserves the same width, so the waveform plot narrows by one axis.
    auto list = app.axisEntries;
    list.push_back (regrAxes_axis (sc::axes::Kind::Amplitude, "Amplitude V", 3, 2.0,
                                   sc::axes::Type::AddAxis));
    CHECK (app.setAxisEntries (list, "Scale"), "adding an amplitude axis is accepted");
    const int shared = kLeftAxisWidth + sc::axes::extraStripWidth();
    CHECK (app.axisStripWidth() == shared,
           "the shared strip grows by a narrow extra strip, not a whole one: "
               + juce::String (app.axisStripWidth()));

    WaveformView wv (app);
    wv.setSize (400, 200);
    wv.setVisibleChannels ({ 0 });
    wv.setViewRange (0.0, 1.0);
    CHECK (wv.axisW() == shared, "the waveform uses the shared width");
    CHECK (wv.plotRect().getWidth() == 400 - shared,
           "the waveform reserves the shared width for its axes");

    // The extra strip lies outside the main one, on the axis' side: the plot
    // starts after the whole shared width, the main axis hugs the plot and the
    // extra axis sits further out.
    // On the left the channel label's room (the gutter) belongs to the
    // outermost strip: the main strip gives it up to the extra one (#27).
    const int gutter = sc::axes::channelGutter();
    const auto s0 = sc::axes::verticalStrip (0, 2, shared, shared, 400, 0, 200, false, gutter);
    const auto s1 = sc::axes::verticalStrip (1, 2, shared, shared, 400, 0, 200, false, gutter);
    CHECK (s0.getWidth() == kLeftAxisWidth - gutter && s0.getRight() == shared,
           "the main axis strip hugs the plot");
    CHECK (s1.getWidth() == sc::axes::extraStripWidth() + gutter && s1.getX() == 0
               && s1.getRight() == s0.getX(),
           "the extra axis sits outside the main one and holds the channel label's room");
    const auto only = sc::axes::verticalStrip (0, 1, kLeftAxisWidth, kLeftAxisWidth, 400, 0, 200, false, gutter);
    CHECK (only.getWidth() == kLeftAxisWidth && only.getX() == 0, "a single axis keeps its 60 px strip");

    // Every strip keeps the same room for its labels whatever the count: the
    // extra axis' labels used to get 10 px on the left (they read "...").
    const auto lab0 = sc::axes::verticalLabelRect (0, 2, shared, shared, 400, 0, 200, false, gutter);
    const auto lab1 = sc::axes::verticalLabelRect (1, 2, shared, shared, 400, 0, 200, false, gutter);
    const auto labOnly = sc::axes::verticalLabelRect (0, 1, kLeftAxisWidth, kLeftAxisWidth, 400, 0, 200,
                                                      false, gutter);
    CHECK (lab0.getWidth() == labOnly.getWidth() && lab0.getRight() >= shared - 4 && lab0.getRight() <= shared,
           "left: the main labels keep their width, next to the plot: " + juce::String (lab0.getX()));
    CHECK (lab1.getWidth() == sc::axes::extraStripWidth() - 4 && lab1.getX() == gutter,
           "left: the extra labels get their strip's width past the channel label: "
               + juce::String (lab1.getWidth()));
    const auto r0s = sc::axes::verticalStrip (0, 2, shared, 0, 400 - shared, 0, 200, true, gutter);
    const auto r1s = sc::axes::verticalStrip (1, 2, shared, 0, 400 - shared, 0, 200, true, gutter);
    CHECK (r0s.getX() == 400 - shared && r0s.getWidth() == kLeftAxisWidth
               && r1s.getX() == r0s.getRight() && r1s.getRight() == 400,
           "right: the main strip hugs the plot, the extra one fills the rest");
    // Frequency labels skip the row centre, so they keep no room for the
    // channel label (gutter 0): the main ones get the strip's full width
    // ("10.0 kHz" was cut to "10.0 k" on a left axis).
    const auto f0 = sc::axes::verticalLabelRect (0, 1, kLeftAxisWidth, kLeftAxisWidth, 400, 0, 200, false, 0);
    const auto f1 = sc::axes::verticalLabelRect (1, 2, shared, shared, 400, 0, 200, false, 0);
    CHECK (f0.getWidth() == kLeftAxisWidth - 4 && f1.getWidth() == sc::axes::extraStripWidth() - 4
               && f1.getX() == 2,
           "frequency labels use their strips' whole width: " + juce::String (f0.getWidth()));
    const auto r0 = sc::axes::timeStrip (0, 0, 400, 24, 176, 24, false);
    CHECK (r0.getY() == 0 && r0.getHeight() == 24, "the main ruler keeps its place and height");

    // A second time axis stacks a narrower ruler and reserves both.
    auto l2 = app.axisEntries;
    l2.push_back (regrAxes_axis (sc::axes::Kind::Time, "Time ss.xxxx", 1, 1.0,
                                 sc::axes::Type::AddAxis));
    CHECK (app.setAxisEntries (l2, "Scale"), "adding a time axis is accepted");
    CHECK (app.rulerStackHeight() == 24 + sc::axes::extraRulerHeight(),
           "the ruler stack grows by one narrow strip: " + juce::String (app.rulerStackHeight()));
    const int stack = app.rulerStackHeight();
    const auto t1 = sc::axes::timeStrip (1, 0, 400, stack, 200 - stack, stack, false);
    // A ruler above the plot hangs the other way: the FIRST axis in the form is
    // the one closest to the track, so the main strip hugs the plot edge and the
    // extra one stacks above it.
    const auto m1 = sc::axes::timeStrip (0, 0, 400, stack, 200 - stack, stack, false);
    CHECK (m1.getY() == stack - 24 && m1.getHeight() == 24,
           "the main ruler hugs the top edge of the plot");
    CHECK (t1.getY() == 0 && t1.getHeight() == sc::axes::extraRulerHeight(),
           "the extra ruler stacks above the main one, away from the track");
    CHECK (wv.plotRect().getHeight() == 200 - stack, "the plot reserves the whole stack");

    // Every axis change is one history step that restores the previous axes.
    CHECK (app.undo().canUndo(), "axis changes are recorded");
    app.undoStep ();   // removes the time axis
    CHECK (app.rulerStackHeight() == 24, "undo restores the single-ruler stack");
    app.undoStep ();   // removes the amplitude axis
    CHECK (app.axisStripWidth() == kLeftAxisWidth, "undo restores the single-axis width");
    CHECK (sc::axes::countKind (app.axisEntries, sc::axes::Kind::Amplitude) == 1,
           "undo restores the axis list itself");

    // An empty list is refused: every kind keeps at least its main axis.
    std::string err;
    CHECK (! app.setAxisEntries ({}, "Scale", &err), "an empty axis list is rejected");

    wav.deleteFile ();
    store.deleteFile ();
}

// ------------------------------------------------------------- A2: readouts

static void testAxisFooterReadouts()
{
    progress ("regression axes#A2: Footer Only axes drive the readouts");
    const juce::File store = makeTempFile ("axes_footer.recent");
    store.deleteFile ();
    const juce::File wav = makeTempFile ("axes_footer.wav");
    writeWav (wav, 1, 8000.0, 16000,
              [] (int, int64_t f, double rate)
              { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                                * 440.0 * (double) f / rate); });

    Application app (store);
    CHECK (app.openFile (wav), "axes: open wav for the readout test");
    if (app.doc() == nullptr) return;

    // A "Footer Only" frequency axis adds a readout and draws no strip.
    const auto rel = regrAxes_axis (sc::axes::Kind::Frequency, "Rel", 2, 0.0025,
                                    sc::axes::Type::FooterOnly);
    auto list = app.axisEntries;
    list.push_back (rel);
    CHECK (app.setAxisEntries (list, "Scale"), "adding a footer axis is accepted");
    CHECK (app.axisStripWidth() == kLeftAxisWidth,
           "a footer-only axis does not widen the shared strip");
    CHECK (sc::axes::extras (app.axisEntries, sc::axes::Kind::Frequency).empty(),
           "a footer-only axis is not an extra strip");

    const juce::String suffix = FFTAnalysisView::altFreqSuffix (app, 1000.0);
    CHECK (suffix.contains ("Rel=" + sc::axes::formatFrequency (rel, 1000.0, 4000.0)),
           "the footer axis appears in the frequency readout: " + suffix);

    // A "Footer Only" time axis gets a Go to Time field that parses in its own
    // units (here 24 fps "frames": displayed = t * 24).
    const auto frames = regrAxes_axis (sc::axes::Kind::Time, "Frames", 1, 24.0,
                                       sc::axes::Type::FooterOnly);
    auto l2 = app.axisEntries;
    l2.push_back (frames);
    CHECK (app.setAxisEntries (l2, "Scale"), "adding a footer time axis is accepted");

    const auto f = MainWindow::computeGoToTimeFields (app);
    bool hasField = false;
    for (const auto& a : f.alts)
        if (a.second == "Frames" && a.first >= 0) hasField = true;
    CHECK (hasField, "the footer time axis gets a Go to Time field");

    double t = -1.0;
    CHECK (MainWindow::resolveGoToTime (app, f, "", { "60" }, t) && regrAxes_near (t, 60.0 / 24.0),
           "a frame count resolves through its scale");

    // The main axis' own offset and scale apply to every one of its units.
    auto l3 = app.axisEntries;
    for (auto& e : l3)
        if (e.kind == sc::axes::Kind::Time && e.type == sc::axes::Type::AddAxis)
        { e.units = 1; e.offset = 120.0; e.scale = 2.0; }
    CHECK (app.setAxisEntries (l3, "Scale"), "editing the main time axis is accepted");
    CHECK (regrAxes_near (app.timeOffsetSecs, 120.0), "the main axis carries its offset");
    CHECK (regrAxes_near (app.timeScaleFactor, 2.0), "the main axis carries its scale");
    CHECK (app.timeToDisplay (1.0) == 242.0, "displayed = (t + offset) * scale");
    CHECK (regrAxes_near (sc::axes::parseTime (sc::axes::primary (app.axisEntries, sc::axes::Kind::Time)
                                        ? *sc::axes::primary (app.axisEntries, sc::axes::Kind::Time)
                                        : sc::axes::Entry {}, "242", 8000.0, t)
                   ? t : -1.0, 1.0),
           "the same text parses back to the real time");

    wav.deleteFile ();
    store.deleteFile ();
}

// ------------------------------------------------------------ A3: migration

static void testAxisMigrationAndPersistence()
{
    progress ("regression axes#A3: legacy state migrates, entries persist");

    // The old alternative references and the custom frequency factor become
    // entries: a footer-only readout and a scaled axis.
    sc::axes::Legacy legacy;
    legacy.specFreqScaleMode = 1;
    legacy.specFreqFactor    = 0.5;
    legacy.altFreq.push_back ({ "Alt 1", { 1000.0, 2.5 } });
    legacy.altTime.push_back ({ "Frames", { 1.0, 24.0 } });
    bool hadLegacy = false;
    const auto migrated = sc::axes::migrateLegacy (legacy, hadLegacy);
    CHECK (hadLegacy, "legacy state is detected");
    CHECK (sc::axes::countKind (migrated, sc::axes::Kind::Frequency) >= 2,
           "the custom factor and the alternative reference become frequency axes");
    bool footerFreq = false, footerTime = false;
    for (const auto& e : migrated)
    {
        if (e.type != sc::axes::Type::FooterOnly) continue;
        if (e.kind == sc::axes::Kind::Frequency && regrAxes_near (e.scale, 2.5 / 1000.0)) footerFreq = true;
        if (e.kind == sc::axes::Kind::Time && regrAxes_near (e.scale, 24.0)) footerTime = true;
    }
    CHECK (footerFreq, "an alternative frequency reference maps to a footer axis with its ratio");
    CHECK (footerTime, "an alternative time reference maps to a footer axis with its ratio");

    // The entries survive the metadata round trip (the SCMD chunk / sidecar).
    const auto list = sc::axes::defaults();
    const juce::var v = sc::axes::toJson (list);
    auto* obj = new juce::DynamicObject ();   // the var takes ownership
    obj->setProperty ("axes", v);
    const juce::var objVar (obj);
    sc::axes::List back;
    bool hadKey = false;
    CHECK (sc::axes::fromJson (objVar["axes"], back, hadKey) && hadKey,
           "the axis list parses back out of the metadata");
    CHECK (back.size() == list.size() && back == list, "every field survives the round trip");

    // A missing key is reported as missing, not as a broken list.
    bool hadKey2 = true;
    sc::axes::List none;
    CHECK (! sc::axes::fromJson (juce::var(), none, hadKey2) && ! hadKey2,
           "no axes key means no axes");
}

// ------------------------------------------- A4: the form lays its columns out
// Three columns (Time / Amplitude / Frequency), one block per axis, its
// settings under its name, reordered by dragging the name. A DocumentWindow
// whose resized() skips the base class leaves its content component at 0 x 0:
// the form opens as an empty frame, which is what it used to do.
static void testScaleFormLaysOutRows()
{
    progress ("regression axes#A4: the Scale form lays out its three columns");
    const juce::File store = makeTempFile ("axes_form.recent");
    store.deleteFile ();
    const juce::File wav = makeTempFile ("axes_form.wav");
    writeWav (wav, 1, 8000.0, 8000,
              [] (int, int64_t f, double) { return 0.1f * (float) std::sin ((double) f * 0.1); });

    Application app (store);
    CHECK (app.openFile (wav), "axes: open wav for the form");
    if (app.doc() == nullptr) return;

    ScaleFormWindow form (app);
    form.setVisible (true);
    form.setBounds (0, 0, 1000, 420);

    CHECK (form.numRows() == (int) app.axisEntries.size(),
           "one block per axis of the document");
    CHECK (form.numRows() >= 3, "the default file shows its three axes");
    CHECK (form.contentBounds().getWidth() > 600 && form.contentBounds().getHeight() > 200,
           "the form's content area has a real size");

    // One block per kind, each in its own column, filling the width.
    int xTime = -1, xAmp = -1, xFreq = -1;
    for (int i = 0; i < (int) app.axisEntries.size(); ++i)
    {
        const auto r = form.rowBounds (i);
        const auto kind = app.axisEntries[(size_t) i].kind;
        CHECK (r.getWidth() > 150 && r.getHeight() >= 90, "the block is laid out");
        CHECK (form.columnIndexOf (i) == (int) kind, "an axis sits in its own column");
        if (kind == sc::axes::Kind::Time)         xTime = r.getX();
        else if (kind == sc::axes::Kind::Amplitude) xAmp = r.getX();
        else                                        xFreq = r.getX();
    }
    CHECK (xTime >= 0 && xTime < xAmp && xAmp < xFreq,
           "the columns are Time, then Amplitude, then Frequency");

    // A second axis of a kind goes under the first one in the same column.
    sc::axes::List extra = app.axisEntries;
    extra.push_back (regrAxes_axis (sc::axes::Kind::Amplitude, "Second amplitude",
                                    0, 1.0, sc::axes::Type::AddAxis));
    std::string err;
    CHECK (app.setAxisEntries (extra, "test", &err), "extra axis accepted");
    form.refreshFromApp();
    CHECK (form.numRows() == (int) app.axisEntries.size(), "the form shows the new axis");
    CHECK (form.blocksInColumn (sc::axes::Kind::Amplitude) == 2,
           "the Amplitude column now stacks two axes");
    const int firstIdx = 1;                       // the file's amplitude axis
    const int secondIdx = (int) app.axisEntries.size() - 1;
    const auto r1 = form.rowBounds (firstIdx), r2 = form.rowBounds (secondIdx);
    CHECK (r1.getX() == r2.getX() && r2.getY() > r1.getBottom(),
           "the second axis is stacked below the first in the same column");
    CHECK (r2.getWidth() > 150, "the new block is laid out too");

    // Dragging a name moves it inside its own column - and only there.
    CHECK (form.moveBlockInColumn (secondIdx, 0), "the block moved in its column");
    CHECK (app.axisEntries[1].name == "Second amplitude"
               && app.axisEntries[2].name != "Second amplitude",
           "the document order follows the column");
    CHECK (app.axisEntries[0].kind == sc::axes::Kind::Time,
           "the other columns are untouched");
    form.refreshFromApp();
    // The two amplitude entries, wherever they are in the document now: the one
    // that comes first in the column is the one drawn higher.
    int a0 = -1, a1 = -1;
    for (int i = 0; i < (int) app.axisEntries.size(); ++i)
        if (app.axisEntries[(size_t) i].kind == sc::axes::Kind::Amplitude)
            { if (a0 < 0) a0 = i; else a1 = i; }
    CHECK (a0 >= 0 && a1 >= 0, "both amplitude axes are still there");
    CHECK (app.axisEntries[(size_t) a0].name == "Second amplitude",
           "the moved axis is the column's first entry now");
    CHECK (form.rowBounds (a0).getY() < form.rowBounds (a1).getY()
               && form.rowBounds (a0).getX() == form.rowBounds (a1).getX(),
           "the form redraws the new order, in the same column");
    CHECK (! form.moveBlockInColumn (a0, 0),
           "moving a block to where it already is does nothing");

    // The remove button only exists where there is something to remove: a
    // column with a single axis has none, a column with two has one per axis.
    CHECK (! form.removeButtonVisibleFor (0),
           "the Time column has one axis, so it has no remove button");
    CHECK (form.removeButtonVisibleFor (a0) && form.removeButtonVisibleFor (a1),
           "a column with two axes shows the remove button on both");

    // The eye acts on its own axis, not on whichever one it likes.
    const int before0 = app.axisEntries[0].visible, beforeA0 = app.axisEntries[a0].visible;
    form.clickEyeFor (a1);
    CHECK (app.axisEntries[(size_t) a1].visible == false,
           "clicking an eye hides that axis");
    CHECK (app.axisEntries[(size_t) a0].visible == beforeA0
               && app.axisEntries[0].visible == before0,
           "the other axes are untouched");
    form.refreshFromApp();
    CHECK (app.axisEntries[(size_t) a1].visible == false, "the hidden state is kept");

    // Removing one of two axes works, and the last one of a column cannot be
    // removed (its button is not there).
    form.clickRemoveFor (a1);
    CHECK ((int) sc::axes::countKind (app.axisEntries, sc::axes::Kind::Amplitude) == 1,
           "the remove button removes its own axis");
    form.refreshFromApp();
    CHECK (! form.removeButtonVisibleFor (a0),
           "the column is back to a single axis: no remove button");

    form.setVisible (false);
}

// -------------------------------- A5: the ruler at the bottom, Hz axis labels
static void testAxisRulerAtBottomAndUnits()
{
    progress ("regression axes#A5: ruler moved to the bottom, extra axis units");
    const juce::File store = makeTempFile ("axes_ruler.recent");
    store.deleteFile ();
    const juce::File wav = makeTempFile ("axes_ruler.wav");
    writeWav (wav, 1, 8000.0, 8000,
              [] (int, int64_t f, double) { return 0.2f * (float) std::sin ((double) f * 0.3); });

    Application app (store);
    CHECK (app.openFile (wav), "axes: open wav");
    if (app.doc() == nullptr) return;

    app.settings.trackRulerAtBottom = true;
    SpectralView sv (app);
    sv.setSize (400, 200);
    sv.setVisibleChannels ({ 0 });
    sv.setViewRange (0.0, 1.0);

    const auto pr = sv.plotRect();
    const int stack = app.rulerStackHeight();
    CHECK (pr.getBottom() == 200 - stack, "the plot gives way to the ruler band");
    const auto strip = sc::axes::timeStrip (0, pr.getX(), pr.getWidth(),
                                            pr.getY(), pr.getBottom(), stack, true);
    CHECK (strip.getY() == pr.getBottom() && strip.getBottom() == 200,
           "the ruler fills the band below the plot, not the plot itself");
    CHECK (strip.getY() > pr.getY(), "the ruler does not overlap the spectrogram");

    // A second time axis stacks below the plot too, main first.
    auto l2 = app.axisEntries;
    l2.push_back (regrAxes_axis (sc::axes::Kind::Time, "Time ss.xxxx", 1, 1.0,
                                 sc::axes::Type::AddAxis));
    CHECK (app.setAxisEntries (l2, "Scale"), "a second time axis is accepted");
    const int stack2 = app.rulerStackHeight();
    const auto pr2 = sv.plotRect();
    const auto main2 = sc::axes::timeStrip (0, pr2.getX(), pr2.getWidth(),
                                            pr2.getY(), pr2.getBottom(), stack2, true);
    const auto extra2 = sc::axes::timeStrip (1, pr2.getX(), pr2.getWidth(),
                                             pr2.getY(), pr2.getBottom(), stack2, true);
    CHECK (main2.getY() == pr2.getBottom() && main2.getHeight() == 24,
           "the main ruler still hugs the plot edge");
    CHECK (extra2.getY() == main2.getBottom()
               && extra2.getHeight() == sc::axes::extraRulerHeight()
               && extra2.getBottom() == 200,
           "the extra ruler sits between the main one and the bottom edge");
    app.undoStep ();

    // An extra frequency axis in Hz labels its strip in Hz.
    sc::axes::Entry hz;
    hz.kind = sc::axes::Kind::Frequency; hz.units = 0; hz.scale = 1.0;
    CHECK (sc::axes::formatFrequencyAxis (hz, 500.0, 4000.0) == "500 Hz",
           "the axis label spells out Hz");
    CHECK (sc::axes::formatFrequencyAxis (hz, 1500.0, 4000.0) == "1.5 kHz",
           "and kHz above 1000");
    sc::axes::Entry pct = hz; pct.units = 1; pct.scale = 100.0 / 4000.0;
    CHECK (sc::axes::formatFrequencyAxis (pct, 2000.0, 4000.0).endsWith ("%"),
           "the percent unit keeps its own suffix");
}


// ------------- A6: the crosshair arithmetic and a block's control geometry
static void testAxisPickArithmeticAndControls()
{
    progress ("regression axes#A6: playhead / selection / reference arithmetic, controls");
    const juce::File store = makeTempFile ("axes_pick.recent");
    store.deleteFile ();
    const juce::File wav = makeTempFile ("axes_pick.wav");
    writeWav (wav, 1, 8000.0, 8000,
              [] (int, int64_t f, double) { return 0.5f * (float) std::sin ((double) f * 0.2); });

    Application app (store);
    CHECK (app.openFile (wav), "axes: open wav");
    if (app.doc() == nullptr) return;

    const double rate = 8000.0, nyq = 4000.0;

    // ---- an offset that makes the playhead show a chosen time
    {
        auto e = regrAxes_axis (sc::axes::Kind::Time, "Time", 0, 1.0, sc::axes::Type::AddAxis);
        e.offset = sc::axes::offsetForDisplayedValue (e, 5.0, 10.0, rate);
        CHECK (regrAxes_near (sc::axes::displayValueTime (e, 5.0, rate), 10.0),
               "the offset makes the playhead display the typed time");
        CHECK (sc::axes::formatTime (e, 5.0, rate).contains ("10"),
               "and the axis label says so: " + sc::axes::formatTime (e, 5.0, rate));

        // The scale is taken into account, not assumed to be 1.
        auto s = e; s.scale = 2.0; s.offset = 0.0;
        s.offset = sc::axes::offsetForDisplayedValue (s, 5.0, 10.0, rate);
        CHECK (regrAxes_near (sc::axes::displayValueTime (s, 5.0, rate), 10.0, 1e-9),
               "an existing scale is respected");

        // Sample units: the value is a sample count.
        auto sm = e; sm.units = 2; sm.offset = 0.0;
        sm.offset = sc::axes::offsetForDisplayedValue (sm, 1.0, 8000.0, rate);
        CHECK (regrAxes_near (sc::axes::displayValueTime (sm, 1.0, rate), 8000.0, 1e-6),
               "sample units display the typed sample count");
    }

    // ---- a selection's two ends give the offset and the scale
    {
        auto e = regrAxes_axis (sc::axes::Kind::Time, "Time", 0, 1.0, sc::axes::Type::AddAxis);
        CHECK (sc::axes::solveTimePoints (e, 1.0, 10.0, 3.0, 30.0, rate),
               "two points give a scale");
        CHECK (regrAxes_near (e.scale, 10.0) && regrAxes_near (e.offset, 0.0),
               "10 s per second here: scale 10, no offset");
        CHECK (regrAxes_near (sc::axes::displayValueTime (e, 1.0, rate), 10.0)
                   && regrAxes_near (sc::axes::displayValueTime (e, 3.0, rate), 30.0),
               "both ends of the selection display the typed values");

        auto d = regrAxes_axis (sc::axes::Kind::Time, "Time", 0, 1.0, sc::axes::Type::AddAxis);
        CHECK (! sc::axes::solveTimePoints (d, 1.0, 5.0, 3.0, 5.0, rate),
               "two identical values give no scale factor");
        CHECK (regrAxes_near (d.scale, 1.0) && regrAxes_near (d.offset, 0.0),
               "and nothing is changed in that case");

        auto sm = regrAxes_axis (sc::axes::Kind::Time, "Samples", 2, 1.0, sc::axes::Type::AddAxis);
        CHECK (! sc::axes::solveTimePoints (sm, 1.0, 8000.0, 2.0, 16000.0, rate),
               "sample units report that they have no scale factor");
        CHECK (regrAxes_near (sc::axes::displayValueTime (sm, 1.0, rate), 8000.0, 1e-6)
                   && regrAxes_near (sc::axes::displayValueTime (sm, 2.0, rate), 16000.0, 1e-6),
               "the offset alone still matches both ends");
    }

    // ---- a clicked reference gives the scale of an amplitude or frequency axis
    {
        auto a = regrAxes_axis (sc::axes::Kind::Amplitude, "Linear", 1, 1.0, sc::axes::Type::AddAxis);
        a.scale = sc::axes::scaleForReference (a, 0.5, 1.0, nyq);
        CHECK (regrAxes_near (a.scale, 2.0), "a clicked amplitude gives its scale");
        CHECK (sc::axes::formatAmplitude (a, 0.5, 0) == "1.000",
               "the clicked point now shows the typed value: " + sc::axes::formatAmplitude (a, 0.5, 0));

        auto db = regrAxes_axis (sc::axes::Kind::Amplitude, "dB", 0, 1.0, sc::axes::Type::AddAxis);
        db.scale = sc::axes::scaleForReference (db, 0.5, 0.0, nyq);   // 0 dB at 0.5
        CHECK (regrAxes_near (db.scale, 2.0), "a dB reference inverts the dB law");
        CHECK (sc::axes::formatAmplitude (db, 0.5, 0) == "0.0",
               "the clicked point reads 0 dB: " + sc::axes::formatAmplitude (db, 0.5, 0));

        auto f = regrAxes_axis (sc::axes::Kind::Frequency, "Hz", 0, 1.0, sc::axes::Type::AddAxis);
        f.scale = sc::axes::scaleForReference (f, 1000.0, 2000.0, nyq);
        CHECK (regrAxes_near (f.scale, 2.0), "a clicked frequency gives its scale");
        CHECK (sc::axes::formatFrequencyAxis (f, 1000.0, nyq) == "2 kHz",
               "the clicked line now reads 2 kHz: " + sc::axes::formatFrequencyAxis (f, 1000.0, nyq));

        auto pc = regrAxes_axis (sc::axes::Kind::Frequency, "%", 1, 1.0, sc::axes::Type::AddAxis);
        pc.scale = sc::axes::scaleForReference (pc, 2000.0, 50.0, nyq);
        CHECK (regrAxes_near (pc.scale, 1.0), "a percent reference uses the Nyquist span");
        CHECK (sc::axes::formatFrequencyAxis (pc, 2000.0, nyq) == "50.00 %",
               "and reads as a percent: " + sc::axes::formatFrequencyAxis (pc, 2000.0, nyq));

        CHECK (! std::isfinite (sc::axes::scaleForReference (f, 0.0, 2000.0, nyq)),
               "a reference at zero gives no scale (the app says so instead)");
    }

    // ---- the controls of a block: same size, side by side, inside the fields
    auto l2 = app.axisEntries;
    l2.push_back (regrAxes_axis (sc::axes::Kind::Amplitude, "Second amplitude", 1, 1.0,
                                 sc::axes::Type::AddAxis));
    CHECK (app.setAxisEntries (l2, "Scale"), "a second amplitude axis is accepted");

    ScaleFormWindow form (app);
    form.setVisible (true);
    form.setBounds (0, 0, 1000, 460);

    int ampMain = -1, ampSecond = -1, timeIdx = -1;
    for (int i = 0; i < (int) app.axisEntries.size(); ++i)
    {
        const auto& e = app.axisEntries[(size_t) i];
        if (e.kind == sc::axes::Kind::Time) timeIdx = i;
        if (e.kind == sc::axes::Kind::Amplitude)
            if (ampMain < 0) ampMain = i; else ampSecond = i;
    }
    CHECK (ampMain >= 0 && ampSecond >= 0 && timeIdx >= 0, "the form has the axes it needs");

    for (const int idx : { ampMain, ampSecond, timeIdx })
    {
        const auto c = form.controlsFor (idx);
        CHECK (c.valid, "the block's controls are laid out");
        CHECK (c.eye.getWidth() > 0 && c.eye.getWidth() == c.del.getWidth(),
               "the remove button is the same size as the visibility eye");
        CHECK (c.del.getX() > c.eye.getRight() && c.del.getX() - c.eye.getRight() <= 8,
               "the two buttons sit together, not at opposite ends of the row");
        CHECK (c.del.getRight() <= c.typeField.getRight()
                   && c.del.getRight() <= c.unitsField.getRight(),
               "the remove button never passes the right edge of the text boxes below it");
        CHECK (c.eye.getBottom() <= c.scaleField.getY(),
               "the buttons are above the fields, not over them");
        CHECK (c.scalePick.getWidth() > 0 && c.scalePick.getY() == c.scaleField.getY()
                   && c.scalePick.getX() > c.scaleField.getRight()
                   && c.scalePick.getRight() <= c.typeField.getRight(),
               "the Scale crosshair sits in its own row, inside the column");
        if (idx == timeIdx)
            CHECK (c.offsetPick.getWidth() > 0 && c.offsetPick.getY() == c.offsetField.getY()
                       && c.offsetPick.getX() > c.offsetField.getRight(),
                   "the Offset crosshair sits in the Offset row");
        else
            CHECK (c.offsetPick.getWidth() == 0, "only a time axis has an Offset crosshair");
    }

    // The eye says what it does.
    CHECK (form.eyeTooltipFor (ampMain) == "Hide Axis",
           "a visible axis' eye is a request to hide it: " + form.eyeTooltipFor (ampMain));
    form.clickEyeFor (ampMain);
    CHECK (form.eyeTooltipFor (ampMain) == "Show Axis",
           "once hidden, it asks to show it again");
    form.clickEyeFor (ampMain);

    form.setVisible (false);
    app.undoStep ();
}

static void runRegression_axes()
{
    testAxisModelAndSharedGeometry();
    testAxisFooterReadouts();
    testAxisMigrationAndPersistence();
    testScaleFormLaysOutRows();
    testAxisRulerAtBottomAndUnits();
    testAxisPickArithmeticAndControls();
}
