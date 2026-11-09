// Regression tests for the 2026-11 audit fixes (views). Included by tests/UnitTests.cpp;
// uses its helpers (CHECK, progress, writeWav, makeTempFile, ...).

// Number of pixels of column x (rows y0..y1-1) that are identical in a and b,
// a's column read at ax and b's at bx.
static int viewsSameColumnPixels (const juce::Image& a, int ax, const juce::Image& b, int bx,
                                  int y0, int y1)
{
    int same = 0;
    for (int y = y0; y < y1; ++y)
        if (a.getPixelAt (ax, y) == b.getPixelAt (bx, y))
            ++same;
    return same;
}

// Runs every message already queued, and the ones those post in turn (a few
// rounds): a sentinel is posted behind them and the loop is pumped until it
// ran. Used before taking a reference image, so asynchronous updates queued
// by earlier setters (layout, view-range notifications) cannot land between
// the reference and the state under test. Shared by the later GUI fragments
// (panels.inc, mainwindow.inc are included after this file).
static bool regDrainMessages (int rounds = 3, int timeoutMs = 5000)
{
    bool all = true;
    for (int r = 0; r < rounds; ++r)
    {
        auto ran = std::make_shared<std::atomic<bool>> (false);
        if (!juce::MessageManager::callAsync ([ran] { ran->store (true); }))
            return false;
        all = scPumpUntil ([ran] { return ran->load (); }, timeoutMs) && all;
    }
    return all;
}

// True when an application message box containing `text` was raised since the
// last sc::alert::clearSuppressedMessages(). The runner puts sc::alert in
// headless mode: message boxes are recorded instead of shown, so a modal
// driver waiting for an AlertWindow never sees them.
static bool regAlertShown (const juce::String& text)
{
    for (const auto& m : sc::alert::suppressedMessages ())
        if (m.containsIgnoreCase (text))
            return true;
    return false;
}

// views#W1: the background PNG export reads the view only on the message
// thread (preparePngExport); the job run on a worker must not see later UI
// changes. saveAsPng() called from a worker takes the same snapshot path.
static void testViewsExportSnapshot()
{
    progress ("regression views#W1: PNG export job uses a message-thread snapshot");
    const juce::File dir = makeTempFile ("views_export");
    dir.createDirectory ();
    const juce::File store = makeTempFile ("views_export.recent");
    store.deleteFile ();
    const juce::File wav = dir.getChildFile ("views_src.wav");
    writeWav (wav, 2, 8000.0, 8000, [] (int ch, int64_t f, double)
        { return (float) (std::sin ((double) f * (0.05 + 0.03 * ch)) * 0.3); });

    Application app (store);
    CHECK (app.openFile (wav), "views#W1: open wav");
    std::unique_ptr<MainWindow> win (new MainWindow (app, "views export"));
    SpectralView* sv = win->spectralViewComp ();
    CHECK (sv != nullptr, "views#W1: spectral view exists");
    CHECK (app.doc () != nullptr, "views#W1: document open");
    if (sv == nullptr || app.doc () == nullptr) return;

    app.setSelection ({ 0.1, 0.3 });
    app.doc ()->spectralSel.addRect (0.2, 0.6, 200.0, 1500.0, false, true);
    sv->setSpectralSelectionChanged ();

    SpectralView::SpectralExportOptions all;
    all.axes = true;  all.markers = true;  all.selections = true;  all.drawings = true;

    const juce::File ref = dir.getChildFile ("ref.png");
    CHECK (sv->saveAsPng (ref, nullptr, all), "views#W1: synchronous export writes a PNG");

    const juce::File outJob = dir.getChildFile ("job.png");
    auto job = sv->preparePngExport (outJob, all);
    CHECK (job != nullptr, "views#W1: preparePngExport returns a job");

    // The UI moves on before the worker runs: other channels, window, axis.
    sv->setVisibleChannels ({ 1 });
    sv->setViewRange (0.5, 0.9);
    sv->setFrequencyRange (100.0, 2000.0);

    bool ok = false;
    std::thread worker ([&] { ok = job != nullptr && job (nullptr); });
    worker.join ();
    CHECK (ok, "views#W1: the job writes the PNG on a worker thread");
    CHECK (fileChecksum (outJob) == fileChecksum (ref),
           "views#W1: the job renders the state captured at prepare time");

    // A cancelled job writes nothing.
    const juce::File outCancel = dir.getChildFile ("cancel.png");
    std::atomic<bool> cancelled { true };
    auto job2 = sv->preparePngExport (outCancel, all);
    CHECK (job2 != nullptr && !job2 (&cancelled) && !outCancel.existsAsFile (),
           "views#W1: a cancelled job writes no file");

    // Legacy call from a worker thread: the snapshot hops to the message
    // thread, so the result equals a synchronous export of the current state.
    // The setters above queued asynchronous updates (layout, range
    // notifications); they used to run during the pump below, i.e. BETWEEN
    // the reference and the worker's snapshot, which made this check flaky.
    // Drain them first, and take the reference after the worker returned
    // (same settled state, nothing else touches the view meanwhile).
    CHECK (regDrainMessages (), "views#W1: message queue drained before the threaded export");
    const juce::File outThread = dir.getChildFile ("thread.png");
    std::atomic<bool> done { false }, giveUp { false };
    bool okThread = false;
    std::thread w2 ([&] { okThread = sv->saveAsPng (outThread, &giveUp, all); done = true; });
    const bool finished = scPumpUntil ([&done] { return done.load (); }, 20000);
    giveUp = true;   // never hang the suite if the message never ran
    w2.join ();
    CHECK (finished, "views#W1: the worker's snapshot request was served within 20 s");
    const juce::File ref2 = dir.getChildFile ("ref2.png");
    CHECK (sv->saveAsPng (ref2, nullptr, all), "views#W1: second synchronous export");
    CHECK (okThread && fileChecksum (outThread) == fileChecksum (ref2),
           "views#W1: saveAsPng from a worker thread exports the message-thread snapshot");
    win = nullptr;
    dir.deleteRecursively ();
}

// Export canvas layout: axes on the right / ruler at the bottom used to be
// drawn over the plot (canvas too small), the spectral-selection mask was
// offset by the live axis width in a no-axes export, and a stacked view
// (no own ruler) exported with axes overflowed the mask.
static void testViewsExportLayout()
{
    progress ("regression views#W1: export canvas layout + selection mask");
    const juce::File dir = makeTempFile ("views_layout");
    dir.createDirectory ();
    const juce::File store = makeTempFile ("views_layout.recent");
    store.deleteFile ();
    const juce::File wav = dir.getChildFile ("views_layout.wav");
    writeWav (wav, 1, 8000.0, 8000, [] (int, int64_t f, double)
        { return (float) (std::sin ((double) f * 0.07) * 0.3 + std::sin ((double) f * 0.9) * 0.2); });

    Application app (store);
    CHECK (app.openFile (wav), "views layout: open wav");
    std::unique_ptr<MainWindow> win (new MainWindow (app, "views layout"));
    SpectralView* sv = win->spectralViewComp ();
    CHECK (sv != nullptr, "views layout: spectral view exists");
    if (sv == nullptr || app.doc () == nullptr) return;
    sv->setViewRange (0.0, 1.0);

    const bool oldRight = app.settings.trackAxisOnRight;
    const bool oldBottom = app.settings.trackRulerAtBottom;
    app.settings.trackAxisOnRight = true;
    app.settings.trackRulerAtBottom = true;
    {
        const auto pr = sv->plotRect ();
        const int pw = pr.getWidth (), ph = pr.getHeight ();
        SpectralView::SpectralExportOptions axes;
        axes.axes = true;
        const juce::File outPlain = dir.getChildFile ("right_plain.png");
        const juce::File outAxes = dir.getChildFile ("right_axes.png");
        CHECK (sv->saveAsPng (outPlain), "views layout: plain export (axis right)");
        CHECK (sv->saveAsPng (outAxes, nullptr, axes), "views layout: axes export (axis right)");
        const auto da = pngDimensions (outAxes);
        CHECK (da.first == pw + kLeftAxisWidth && da.second == ph + SpectralView::kRulerH,
               "views layout: right axis / bottom ruler are added around the plot");
        const juce::Image ip = juce::ImageFileFormat::loadFrom (outPlain);
        const juce::Image ia = juce::ImageFileFormat::loadFrom (outAxes);
        if (ip.isValid () && ia.isValid () && ph > 8 && pw > 40)
        {
            // Plot at (0,0) in both: a column near the right edge of the plot
            // must still show the spectrum, not the axis strip (grid lines
            // may cross it, hence "most").
            const int same = viewsSameColumnPixels (ia, pw - 30, ip, pw - 30, 0, ph);
            CHECK (same > ph / 2, "views layout: the right axis strip does not cover the plot");
        }
        else
            CHECK (false, "views layout: exported PNGs decode");
    }
    app.settings.trackAxisOnRight = oldRight;
    app.settings.trackRulerAtBottom = oldBottom;

    // Spectral selection covering the whole plot, no time selection.
    app.setSelection ({ 0.0, 0.0 });
    app.doc ()->spectralSel.addRect (0.0, 1.0, 1.0, 4000.0, false, true);
    sv->setSpectralSelectionChanged ();
    {
        const auto pr = sv->plotRect ();
        const int ph = pr.getHeight ();
        SpectralView::SpectralExportOptions sel;
        sel.selections = true;
        const juce::File outPlain = dir.getChildFile ("sel_plain.png");
        const juce::File outSel = dir.getChildFile ("sel.png");
        CHECK (sv->saveAsPng (outPlain), "views layout: plain export");
        CHECK (sv->saveAsPng (outSel, nullptr, sel), "views layout: selection export");
        const juce::Image ip = juce::ImageFileFormat::loadFrom (outPlain);
        const juce::Image is = juce::ImageFileFormat::loadFrom (outSel);
        if (ip.isValid () && is.isValid () && ph > 8)
        {
            // x = 10 lies inside the first kLeftAxisWidth plot columns, which
            // the mask used to skip in a no-axes export.
            const int same = viewsSameColumnPixels (is, 10, ip, 10, 2, ph - 2);
            CHECK (same < (ph - 4) / 2,
                   "views layout: the selection fill covers the plot's left columns");
        }
        else
            CHECK (false, "views layout: selection PNGs decode");

        // Stacked view (no own ruler) + axes + selections: canvas taller than
        // the component's plot origin; must render (used to overflow the mask).
        sv->setShowOwnTimeRuler (false);
        const auto pr2 = sv->plotRect ();
        SpectralView::SpectralExportOptions allSel;
        allSel.axes = true;  allSel.selections = true;
        const juce::File outStacked = dir.getChildFile ("stacked.png");
        CHECK (sv->saveAsPng (outStacked, nullptr, allSel), "views layout: stacked axes+selection export");
        const auto ds = pngDimensions (outStacked);
        CHECK (ds.first == pr2.getWidth () + kLeftAxisWidth
               && ds.second == pr2.getHeight () + SpectralView::kRulerH,
               "views layout: stacked export adds the axes around the plot");
        sv->setShowOwnTimeRuler (true);
    }
}

// views#W4: the spectral selection raster is limited to the repaint clip;
// a narrow repaint (playhead column) must produce the same pixels as a full
// paint in that column. This is a non-regression guard for that optimisation
// (a clip/offset mistake in the clipped raster shows up as a mismatch); it
// does not prove that the raster IS clipped - the unclipped code passed it
// too. The column is first checked to really show the selection, so a
// selection drawn in neither paint cannot pass for "identical".
static void testViewsClippedSelectionPaint()
{
    progress ("regression views#W4: clipped repaint draws the same selection pixels as a full paint");
    const juce::File dir = makeTempFile ("views_clip");
    dir.createDirectory ();
    const juce::File store = makeTempFile ("views_clip.recent");
    store.deleteFile ();
    const juce::File wav = dir.getChildFile ("views_clip.wav");
    writeWav (wav, 1, 8000.0, 8000, [] (int, int64_t f, double)
        { return (float) (std::sin ((double) f * 0.11) * 0.3); });

    Application app (store);
    CHECK (app.openFile (wav), "views#W4: open wav");
    std::unique_ptr<MainWindow> win (new MainWindow (app, "views clip"));
    SpectralView* sv = win->spectralViewComp ();
    CHECK (sv != nullptr, "views#W4: spectral view exists");
    if (sv == nullptr || app.doc () == nullptr) return;
    if (sv->getWidth () < 200 || sv->getHeight () < 60)
        sv->setSize (800, 300);   // headless window may not be laid out
    sv->setViewRange (0.0, 1.0);
    app.setSelection ({ 0.0, 0.0 });
    const auto pr = sv->plotRect ();
    const int cx = pr.getX () + pr.getWidth () / 3;   // crosses the polygon
    const juce::Image noSel = sv->createComponentSnapshot (sv->getLocalBounds (), true, 1.0f);

    std::vector<std::pair<double, double>> pts {
        { 0.1, 300.0 }, { 0.8, 500.0 }, { 0.6, 3000.0 }, { 0.3, 2500.0 }, { 0.15, 1200.0 } };
    app.doc ()->spectralSel.addPolygon (pts, false, true);
    sv->setSpectralSelectionChanged ();

    const juce::Image full = sv->createComponentSnapshot (sv->getLocalBounds (), true, 1.0f);
    {
        // The compared column must actually cross the selection fill.
        const int unchanged = viewsSameColumnPixels (full, cx, noSel, cx, 0, sv->getHeight ());
        CHECK (sv->getHeight () - unchanged >= 10,
               "views#W4: the compared column shows the selection ("
                   + juce::String (sv->getHeight () - unchanged) + " pixels changed by it)");
    }
    const juce::Rectangle<int> col (cx - 2, 0, 5, sv->getHeight ());
    const juce::Image part = sv->createComponentSnapshot (col, true, 1.0f);
    // The selection fill is a whole-pixel mask blended at alpha ~80/255, so a
    // mask pixel set or cleared differently shifts a channel by 20+ levels.
    // The anti-aliased polygon border, on the other hand, is rasterised by the
    // native renderer (CoreGraphics on macOS), whose edge coverage differs by
    // 1-2 levels between a full and a clipped/offset snapshot (measured: 10
    // border pixels, none with the border stroke disabled), and by up to 6
    // levels at other view sizes (the Editor panel of the docking workspace:
    // 1032 x 316 px). Tolerate that, far below a mask pixel's 20+ levels.
    int diff = 0;
    for (int y = 0; y < sv->getHeight (); ++y)
        for (int dx = 0; dx < 5; ++dx)
        {
            const auto a = full.getPixelAt (cx - 2 + dx, y), b = part.getPixelAt (dx, y);
            const int d = std::max ({ std::abs ((int) a.getRed ()   - (int) b.getRed ()),
                                      std::abs ((int) a.getGreen () - (int) b.getGreen ()),
                                      std::abs ((int) a.getBlue ()  - (int) b.getBlue ()),
                                      std::abs ((int) a.getAlpha () - (int) b.getAlpha ()) });
            if (d > 8) ++diff;
        }
    CHECK (diff == 0, "views#W4: a 5 px repaint draws the same selection pixels as a full paint");
}

// views#W8: zoomed deeper than the bar's ~8 px minimum (from the views), the
// time bar must still pan and jump. views#W9: logarithmic value bar.
static void testViewsScrollBars()
{
    progress ("regression views#W8/W9: scrollbars at deep zoom + log frequency");
    TimeScrollBar bar;
    bar.setSize (800, 18);
    bar.setDuration (3600.0);          // minSpan = 8/800 * 3600 = 36 s
    double lastT0 = -1, lastT1 = -1;
    bool fired = false;
    bar.onChange = [&] (double t0, double t1) { lastT0 = t0; lastT1 = t1; fired = true; };
    bar.setWindow (100.0, 102.0);      // 2 s window, far below minSpan
    bar.jumpTo (2000.0);
    CHECK (fired && std::abs (lastT0 - 1999.0) < 1e-6 && std::abs (lastT1 - 2001.0) < 1e-6,
           "views#W8: jump works with a window below the bar's minimum span");
    fired = false;
    bar.applyWindow (2010.0, 2012.0);  // pan, same span
    CHECK (fired && std::abs (lastT0 - 2010.0) < 1e-9,
           "views#W8: pan works with a window below the bar's minimum span");
    fired = false;
    bar.applyWindow (2010.0, 2011.0);  // zoom in further from the bar: still refused
    CHECK (!fired, "views#W8: the bar itself still cannot zoom below its minimum");

    ValueScrollBar vb;
    vb.setSize (14, 400);
    vb.setRange (1.0, 24000.0);
    vb.setLogarithmic (true);
    double lo = -1, hi = -1;
    fired = false;
    vb.onChange = [&] (double l, double h) { lo = l; hi = h; fired = true; };
    vb.setWindow (100.0, 1000.0);
    {
        const double L = std::log (24000.0);
        const double y0 = (L - std::log (1000.0)) / L * 400.0;
        const double y1 = (L - std::log (100.0)) / L * 400.0;
        const auto tr = vb.thumbRect ();
        CHECK (std::abs (tr.getY () - y0) <= 1.0 && std::abs (tr.getBottom () - y1) <= 1.5,
               "views#W9: log value bar thumb follows the log axis");
    }
    vb.jumpToValue (1000.0);   // centred in log space: lo * hi = 1000^2
    CHECK (fired && std::abs (lo * hi - 1.0e6) < 1.0 && std::abs (hi / lo - 10.0) < 1e-6,
           "views#W9: log value bar jumps keeping the log span");

    // Linear bar, window below the ~8 px minimum: jump still works.
    ValueScrollBar lin;
    lin.setSize (14, 400);
    lin.setRange (0.0, 22050.0);       // minSpan ~ 441
    lin.setWindow (1000.0, 1100.0);
    fired = false;
    lin.onChange = [&] (double l, double h) { lo = l; hi = h; fired = true; };
    lin.jumpToValue (5000.0);
    CHECK (fired && std::abs (lo - 4950.0) < 1e-6 && std::abs (hi - 5050.0) < 1e-6,
           "views#W8: value bar jump works below its minimum span");
}

static void runRegression_views()
{
    testViewsExportSnapshot();
    testViewsExportLayout();
    testViewsClippedSelectionPaint();
    testViewsScrollBars();
}
