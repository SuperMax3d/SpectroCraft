// Sessions (issue #21, experimental): other files on a document's timeline,
// one after the other or in parallel lanes, kept in one composite engine and
// written back to their own files on save. Model: include/Session.h.

namespace scsessiontest
{
    // Sample f of channel ch of a test file "tag": distinct per file, channel
    // and position, so every copy and move can be checked exactly.
    inline float sig (int tag, int ch, int64_t f)
    {
        return (float) (0.01 * tag + 0.001 * ch) + (float) ((f % 97) * 1e-5);
    }

    inline bool sampleIs (const AudioEngine& e, int ch, int64_t pos, float want)
    {
        float v = 0.0f;
        return e.readSamples (ch, pos, 1, &v) && std::abs (v - want) < 1e-6f;
    }

    inline juce::MemoryBlock bytesOf (const juce::File& f)
    {
        juce::MemoryBlock mb;
        f.loadFileAsData (mb);
        return mb;
    }
}

static void regrSessions_model()
{
    progress ("regression sessions: model (lanes, fit, snapping, warps, composite, JSON)");
    using namespace sc::session;

    Session s;
    s.laneChannels = { 2, 1 };
    s.clips = { { juce::File ("/tmp/main.wav"), 0, 0, 100 },
                { juce::File ("/tmp/b.wav"), 0, 100, 50 },
                { juce::File ("/tmp/c.wav"), 1, 20, 40 } };
    CHECK (s.active () && s.totalChannels () == 3 && s.laneFirstChannel (1) == 2 && s.laneOfChannel (2) == 1
               && s.laneOfChannel (1) == 0 && s.extent () == 150,
           "sessions: lanes map to consecutive channels; extent is the last file's end");
    CHECK (s.clipAt (0, 120) == 1 && s.clipAt (2, 30) == 2 && s.clipAt (2, 5) == -1 && s.clipAt (1, 99) == 0,
           "sessions: the file under a channel and a sample");

    CHECK (! fits (s, 1, 0, 50) && fits (s, 1, 0, 100) && fits (s, 2, 1, 0) && fits (s, 1, 0, 400)
               && ! fits (s, 1, 0, -1),
           "sessions: files of one lane never overlap");
    CHECK (firstFreeOffset (s, 1, 0, 30) == 100, "sessions: first free place after the file in the way");

    CHECK (snapOffset (s, 2, 97, 5) == 100 && snapOffset (s, 2, 63, 5) == 60   // end on 100
               && snapOffset (s, 2, 30, 5) == 30 && snapOffset (s, 2, 45, 5, { 47 }) == 47,
           "sessions: Shift-snapping to file starts and ends, 0 and extra targets");

    {
        Session w = s;
        warpForDeletion (w, 90, 110);   // cuts the end of main and the start of b
        CHECK (w.clips[0].length == 90 && w.clips[1].offset == 90 && w.clips[1].length == 40
                   && w.clips[2].offset == 20 && w.clips[2].length == 40,
               "sessions: a deletion shortens and shifts the files it touches");
        Session gone = s;
        warpForDeletion (gone, 15, 70);   // swallows c entirely
        CHECK (gone.clips.size () == 2 && gone.clips[0].length == 45,
               "sessions: a file fully deleted leaves the session");
        Session ins = s;
        warpForInsertion (ins, 100, 10);   // at the main/b boundary: main grows, b moves
        CHECK (ins.clips[0].length == 110 && ins.clips[1].offset == 110 && ins.clips[1].length == 50,
               "sessions: an insertion at a boundary extends the file before it");
        Session ins0 = s;
        warpForInsertion (ins0, 0, 5);
        CHECK (ins0.clips[0].offset == 0 && ins0.clips[0].length == 105 && ins0.clips[2].offset == 25,
               "sessions: the main file stays at 0 and absorbs an insertion at the start");
        Session st = s;
        warpForStretch (st, 0, 100, 200);   // main doubled
        CHECK (st.clips[0].length == 200 && st.clips[1].offset == 200 && st.clips[2].offset == 40
                   && st.clips[2].length == 80,
               "sessions: a stretch scales the files inside it and shifts the ones after");
    }

    {
        std::vector<std::vector<std::vector<float>>> data = {
            { std::vector<float> (100, 1.0f), std::vector<float> (100, 2.0f) },
            { std::vector<float> (50, 3.0f), std::vector<float> (50, 4.0f) },
            { std::vector<float> (40, 5.0f) } };
        const auto comp = buildComposite (s, data);
        CHECK (comp.size () == 3 && comp[0].size () == 150 && comp[0][99] == 1.0f && comp[0][100] == 3.0f
                   && comp[1][149] == 4.0f && comp[2][19] == 0.0f && comp[2][20] == 5.0f && comp[2][60] == 0.0f,
               "sessions: the composite places each file in its lane, silence elsewhere");
    }

    {
        const juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_session_json");
        dir.getChildFile ("sub").createDirectory ();
        const juce::File mainF = dir.getChildFile ("main.wav");
        const juce::File other = dir.getChildFile ("sub").getChildFile ("b.wav");
        other.replaceWithText ("x");
        Session j;
        j.laneChannels = { 2, 2 };
        j.showBorders = false;
        j.clips = { { mainF, 0, 0, 10 }, { other, 1, 7, 3 } };
        const juce::String json = juce::JSON::toString (toVar (j, dir));
        const auto back = fromVar (juce::JSON::parse (json), dir, mainF, 2, 10);
        CHECK (json.contains ("\"sub/b.wav\"") && back.clips.size () == 2 && back.clips[1].file == other
                   && back.clips[1].lane == 1 && back.clips[1].offset == 7 && back.laneChannels == j.laneChannels
                   && ! back.showBorders && back.clips[0].file == mainF && back.clips[0].length == 10,
               "sessions: JSON round trip with a path relative to the main file's folder");
        dir.deleteRecursively ();
    }
}

static void regrSessions_documents()
{
    progress ("regression sessions: add, move, remove, undo, save, reopen");
    using namespace scsessiontest;

    const juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_sessions_doc");
    dir.deleteRecursively ();
    dir.createDirectory ();
    const juce::File fm = dir.getChildFile ("main.wav"), fb = dir.getChildFile ("b.wav"),
                     fc = dir.getChildFile ("c.wav"), fd = dir.getChildFile ("d16k.wav");
    writeWav (fm, 2, 8000.0, 8000, [] (int ch, int64_t f, double) { return sig (1, ch, f); });
    writeWav (fb, 2, 8000.0, 4000, [] (int ch, int64_t f, double) { return sig (2, ch, f); });
    writeWav (fc, 1, 8000.0, 6000, [] (int ch, int64_t f, double) { return sig (3, ch, f); });
    writeWav (fd, 2, 16000.0, 1000, [] (int ch, int64_t f, double) { return sig (4, ch, f); });
    const juce::File store = dir.getChildFile ("store.recent");

    {
        Application app (store);
        CHECK (app.openFile (fm), "sessions: main file opens");
        FileDoc* d = app.doc ();
        if (d == nullptr) return;
        auto& eng = d->engine;

        std::string err;
        CHECK (! app.addSessionFile (fd, false, &err) && juce::String (err).contains ("Hz"),
               "sessions: a file at another sample rate is refused (" + juce::String (err) + ")");
        CHECK (! app.addSessionFile (fc, false, &err) && juce::String (err).contains ("parallel"),
               "sessions: a mono file cannot follow a stereo main file in its lane");

        CHECK (app.addSessionFile (fb, false, &err), "sessions: add b after the main file (" + juce::String (err) + ")");
        CHECK (d->session.active () && eng.numChannels () == 2 && eng.numSamples () == 12000
                   && d->session.clips[1].offset == 8000
                   && sampleIs (eng, 1, 7999, sig (1, 1, 7999)) && sampleIs (eng, 0, 8000, sig (2, 0, 0))
                   && sampleIs (eng, 1, 11999, sig (2, 1, 3999)),
               "sessions: b follows the main file, sample for sample");

        CHECK (app.addSessionFile (fc, true, &err), "sessions: add c in parallel");
        CHECK (eng.numChannels () == 3 && eng.numSamples () == 12000 && d->session.numLanes () == 2
                   && sampleIs (eng, 2, 0, sig (3, 0, 0)) && sampleIs (eng, 2, 5999, sig (3, 0, 5999))
                   && sampleIs (eng, 2, 6000, 0.0f) && app.visibleChannels.size () == 3,
               "sessions: c gets its own lane (a third channel) from 0");

        app.undoStep ();
        CHECK (eng.numChannels () == 2 && d->session.numLanes () == 1 && d->session.clips.size () == 2,
               "sessions: undo takes c out again");
        app.redoStep ();
        CHECK (eng.numChannels () == 3 && d->session.clips.size () == 3, "sessions: redo puts it back");

        CHECK (! app.moveSessionFile (1, 0, 4000, &err), "sessions: b cannot move onto the main file");
        CHECK (! app.moveSessionFile (2, 0, 0, &err), "sessions: a mono file cannot go to the stereo lane");
        CHECK (app.moveSessionFile (2, 1, 2000, &err), "sessions: move c by 2000 samples");
        CHECK (d->session.clips[2].offset == 2000 && sampleIs (eng, 2, 1999, 0.0f)
                   && sampleIs (eng, 2, 2000, sig (3, 0, 0)) && sampleIs (eng, 2, 7999, sig (3, 0, 5999)),
               "sessions: c's audio moved with it");
        app.undoStep ();
        CHECK (d->session.clips[2].offset == 0 && sampleIs (eng, 2, 0, sig (3, 0, 0)), "sessions: undo the move");
        app.redoStep ();

        // Save untouched: only the main file is written, with its own region.
        const auto bBefore = bytesOf (fb), cBefore = bytesOf (fc);
        CHECK (app.saveDocument (*d, fm, false, &err), "sessions: save");
        {
            std::unique_ptr<juce::AudioFormatReader> r (AudioEngine ().createReaderFor (fm));
            CHECK (r != nullptr && r->numChannels == 2 && r->lengthInSamples == 8000,
                   "sessions: the main file keeps its own channels and length");
        }
        CHECK (bytesOf (fb) == bBefore && bytesOf (fc) == cBefore,
               "sessions: files that were not edited are not rewritten");

        // Edit inside b (1.1..1.3 s, every channel), save: b is rewritten, c is not.
        app.setSelection ({ 1.1, 1.3 });
        app.changeGain (6.0f);
        CHECK (app.saveDocument (*d, fm, false, &err), "sessions: save after editing b");
        CHECK (bytesOf (fb) != bBefore && bytesOf (fc) == cBefore, "sessions: the edited file is rewritten, only it");
        {
            AudioEngine b;
            CHECK (b.loadFile (fb) && b.numSamples () == 4000 && sampleIs (b, 0, 1000, sig (2, 0, 1000) * std::pow (10.0f, 0.3f)),
                   "sessions: b on disk carries the gain");
        }

        // A save through the engine alone (batch, CLI) never writes the composite.
        const juce::File plain = dir.getChildFile ("plain.wav");
        CHECK (eng.saveFile (plain), "sessions: engine save");
        {
            std::unique_ptr<juce::AudioFormatReader> r (AudioEngine ().createReaderFor (plain));
            CHECK (r != nullptr && r->numChannels == 2 && r->lengthInSamples == 8000,
                   "sessions: any engine save is limited to the main file's region");
        }
    }

    // Reopen: the session comes back from the main file's metadata.
    {
        Application app (store);
        CHECK (app.openFile (fm), "sessions: reopen the main file");
        FileDoc* d = app.doc ();
        if (d == nullptr) return;
        auto& eng = d->engine;
        CHECK (d->session.clips.size () == 3 && eng.numChannels () == 3 && eng.numSamples () == 12000
                   && d->session.clips[2].offset == 2000 && sampleIs (eng, 2, 2000, sig (3, 0, 0))
                   && ! eng.hasUnsavedChanges () && ! app.docIsUnsaved (*d),
               "sessions: reopening rebuilds the composite, unmodified (clips "
               + juce::String ((int) d->session.clips.size ()) + ", ch " + juce::String (eng.numChannels ())
               + ", len " + juce::String (eng.numSamples ()) + ", pending " + juce::String ((int) d->pendingSession.isObject ())
               + ", unsaved " + juce::String ((int) eng.hasUnsavedChanges ()) + "/" + juce::String ((int) app.docIsUnsaved (*d))
               + ", warnings " + app.lastSessionWarnings.joinIntoString ("; ") + ")");
        if (d->session.clips.size () != 3) { dir.deleteRecursively (); return; }

        // A deletion across files warps them: 0.5..1.25 s removed.
        app.setSelection ({ 0.5, 1.25 });
        app.del ();
        CHECK (d->session.clips[0].length == 4000 && d->session.clips[1].offset == 4000
                   && d->session.clips[1].length == 2000 && d->session.clips[2].offset == 2000
                   && d->session.clips[2].length == 2000 && eng.numSamples () == 6000,
               "sessions: a deletion shortens and shifts the files");
        app.undoStep ();
        CHECK (d->session.clips[0].length == 8000 && d->session.clips[1].offset == 8000,
               "sessions: undo restores the files' layout");

        std::string err;
        CHECK (app.removeSessionFile (2, &err) && eng.numChannels () == 2 && d->session.clips.size () == 2,
               "sessions: remove c (its lane goes away)");
        CHECK (fc.existsAsFile (), "sessions: a removed file stays on disk");
    }
    dir.deleteRecursively ();
}

static void regrSessions_overlayAndDrag()
{
    progress ("regression sessions: file boxes, borders, Move Files drag with Shift snapping");
    using namespace sc::session;

    // 3 rows (lane 0 stereo, lane 1 mono), 1000 px for 0..1.5 s at 8 kHz.
    Session s;
    s.laneChannels = { 2, 1 };
    s.clips = { { juce::File ("/tmp/main.wav"), 0, 0, 8000 },
                { juce::File ("/tmp/b.wav"), 0, 8000, 4000 },
                { juce::File ("/tmp/c.wav"), 1, 0, 6000 } };
    ViewGeometry g;
    g.rowChannel = { 0, 1, 2 };
    g.rows = { { 0, 20, 1040, 100 }, { 0, 125, 1040, 100 }, { 0, 230, 1040, 100 } };
    g.plot = { 40, 20, 1000, 310 };
    g.rate = 8000.0;
    g.timeToX = [] (double t) { return 40.0 + t / 1.5 * 1000.0; };
    g.xToTime = [] (int x) { return (x - 40) * 1.5 / 1000.0; };

    const auto boxes = clipBoxes (s, g);
    CHECK (boxes.size () == 3 && boxes[0].box == juce::Rectangle<int> (40, 20, 667, 205)
               && boxes[1].box.getX () == 706 && boxes[1].box.getRight () == 1040
               && boxes[2].box == juce::Rectangle<int> (40, 230, 500, 100),
           "sessions: each file covers its lane's rows over its time span");
    CHECK (laneAtY (s, g, 150) == 0 && laneAtY (s, g, 300) == 1 && laneAtY (s, g, 5) == -1,
           "sessions: the lane under the mouse");

    MoveGesture m;
    CHECK (! m.begin (s, g, { 100, 60 }), "sessions: the main file cannot be dragged");
    CHECK (m.begin (s, g, { 100, 280 }) && m.active (), "sessions: a drag starts on c");
    m.drag (s, g, { 200, 280 }, false, 0.0);   // +100 px = +0.15 s = +1200 samples
    CHECK (m.target ().clip == 2 && m.target ().lane == 1 && m.target ().offset == 1200 && m.target ().valid,
           "sessions: dragging moves the file by the mouse distance");
    m.drag (s, g, { 200, 60 }, false, 0.0);    // onto the stereo lane
    CHECK (m.target ().lane == 0 && ! m.target ().valid, "sessions: a lane with another channel count is refused");
    m.drag (s, g, { 105, 280 }, true, 0.0);    // ~0.0075 s: snaps back to 0
    CHECK (m.target ().offset == 0 && m.target ().valid, "sessions: Shift snaps to the start of the timeline");
    m.drag (s, g, { 100 + 167 - 3, 280 }, true, 0.0);   // c's end near the main file's end (8000)
    CHECK (m.target ().offset == 2000, "sessions: Shift snaps c's end onto another file's end");
    const auto t = m.end ();
    CHECK (! m.active () && t.clip == 2 && t.offset == 2000, "sessions: the drag ends with its target");

    // Borders paint without touching anything outside the views' image.
    juce::Image img (juce::Image::ARGB, 1100, 340, true);
    {
        juce::Graphics gr (img);
        paintBorders (gr, s, g);
    }
    CHECK (img.getPixelAt (40, 100).getAlpha () > 0 && img.getPixelAt (300, 100).getAlpha () == 0,
           "sessions: the outline is drawn on the file's edge only");
}

static void regrSessions_window()
{
    progress ("regression sessions: Edit > Session menu and the rows bar");
    using namespace scsessiontest;
    const juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_sessions_ui");
    dir.deleteRecursively ();
    dir.createDirectory ();
    const juce::File fm = dir.getChildFile ("main.wav"), fc = dir.getChildFile ("c.wav");
    writeWav (fm, 2, 8000.0, 8000, [] (int ch, int64_t f, double) { return sig (1, ch, f); });
    writeWav (fc, 1, 8000.0, 6000, [] (int ch, int64_t f, double) { return sig (3, ch, f); });
    {
        Application app (dir.getChildFile ("store.recent"));
        CHECK (app.openFile (fm), "sessions ui: main opens");
        std::unique_ptr<MainWindow> win (new MainWindow (app, "sessions ui"));
        win->setSize (1200, 800);
        const auto names = win->getMenuBarNames ();
        const int eIdx = names.indexOf ("Edit");
        auto findItem = [] (const juce::PopupMenu& m, const juce::String& text) -> const juce::PopupMenu::Item* {
            for (juce::PopupMenu::MenuItemIterator it (m, true); it.next ();)
                if (it.getItem ().text == text) return &it.getItem ();
            return nullptr;
        };
        {
            const auto em = win->getMenuForIndex (eIdx, "Edit");
            const auto* add = findItem (em, "Add File in Parallel...");
            const auto* mv = findItem (em, "Move Files");
            CHECK (findItem (em, "Session (experimental)") != nullptr && add != nullptr && add->isEnabled
                       && mv != nullptr && ! mv->isEnabled,
                   "sessions ui: Edit > Session offers Add; Move Files waits for a session");
            CHECK (win->sessionScrollBar () != nullptr && ! win->sessionScrollBar ()->isVisible (),
                   "sessions ui: no rows bar without a session");
        }

        std::string err;
        CHECK (app.addSessionFile (fc, true, &err), "sessions ui: add c in parallel");
        win->syncSharedState ();
        {
            const auto em = win->getMenuForIndex (eIdx, "Edit");
            const auto* mv = findItem (em, "Move Files");
            const auto* borders = findItem (em, "Show File Borders");
            CHECK (mv != nullptr && mv->isEnabled && ! mv->isTicked && borders != nullptr && borders->isTicked
                       && findItem (em, "c.wav") != nullptr,
                   "sessions ui: with a session, Move Files, borders on, and c in Remove File");
        }
        win->menuItemSelected (MainWindow::EditSessionMoveTool, eIdx);
        win->menuItemSelected (MainWindow::EditSessionBorders, eIdx);
        CHECK (app.sessionMoveTool && ! app.doc ()->session.showBorders,
               "sessions ui: Move Files arms the tool; Show File Borders toggles");
        win->menuItemSelected (MainWindow::EditSessionBorders, eIdx);

        auto* bar = win->sessionScrollBar ();
        CHECK (bar != nullptr && bar->isVisible (), "sessions ui: the rows bar appears with the session");
        if (bar != nullptr)
        {
            bar->applyWindow (0.0, 1.0);   // the bottom row only: c's channel
            CHECK (app.visibleChannels == std::vector<int> { 2 }, "sessions ui: the bar shows a window of rows");
            bar->applyWindow (0.0, 3.0);
            CHECK (app.visibleChannels.size () == 3, "sessions ui: and all of them again");
        }

        // The view's own geometry hits c in its lane.
        if (auto* wv = win->waveformViewComp ())
        {
            const auto geo = wv->sessionGeometry ();
            const auto boxes = sc::session::clipBoxes (app.doc ()->session, geo);
            CHECK (geo.rowChannel.size () == 3 && boxes.size () == 2 && boxes[1].clip == 1,
                   "sessions ui: the waveform view's rows carry both files");
        }

        win->menuItemSelected (MainWindow::EditSessionRemoveBase + 1, eIdx);
        CHECK (! app.doc ()->session.active () && app.audioEngine ().numChannels () == 2,
               "sessions ui: Remove File > c.wav ends the session");
        win->syncSharedState ();
        CHECK (! win->sessionScrollBar ()->isVisible (), "sessions ui: the rows bar goes away");
    }
    dir.deleteRecursively ();
}

static void runRegression_sessions()
{
    regrSessions_model();
    regrSessions_documents();
    regrSessions_overlayAndDrag();
    regrSessions_window();
}
