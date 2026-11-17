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
    {
        Session onlyTrack;
        onlyTrack.laneChannels = { 2, 2 };
        onlyTrack.clips = { { juce::File ("/tmp/main.wav"), 0, 0, 100 } };
        CHECK (onlyTrack.active () && onlyTrack.laneEmpty (1) && ! onlyTrack.laneEmpty (0),
               "sessions: an extra empty track makes a session (Add Track)");
    }
    CHECK (s.active () && s.totalChannels () == 3 && s.laneFirstChannel (1) == 2 && s.laneOfChannel (2) == 1
               && s.laneOfChannel (1) == 0 && s.extent () == 150,
           "sessions: lanes map to consecutive channels; extent is the last file's end");
    CHECK (s.clipAt (0, 120) == 1 && s.clipAt (2, 30) == 2 && s.clipAt (2, 5) == -1 && s.clipAt (1, 99) == 0,
           "sessions: the file under a channel and a sample");

    CHECK (! fits (s, 1, 0, 50) && fits (s, 1, 0, 100) && fits (s, 2, 1, 0) && fits (s, 1, 0, 400)
               && ! fits (s, 1, 0, -1),
           "sessions: files of one lane never overlap");
    CHECK (firstFreeOffset (s, 1, 0, 30) == 100, "sessions: first free place after the file in the way");
    {
        // PR #24 review: dropped on a file's first half = in front of it,
        // that file and the later ones of the lane shifted right.
        Session p3;
        p3.laneChannels = { 1 };
        p3.clips = { { juce::File ("/tmp/m.wav"), 0, 0, 100 }, { juce::File ("/tmp/y.wav"), 0, 200, 100 },
                     { juce::File ("/tmp/z.wav"), 0, 300, 50 }, { juce::File ("/tmp/x.wav"), 0, 500, 40 } };
        const auto front = placeWithInsert (p3, 3, 0, 220);   // on y's first half
        CHECK (front.clips[3].offset == 200 && front.clips[1].offset == 240 && front.clips[2].offset == 340
                   && front.clips[0].offset == 0,
               "sessions: in front of y, y and z pushed right by x's length");
        const auto after = placeWithInsert (p3, 3, 0, 270);   // on y's second half: after y, z is there
        CHECK (after.clips[3].offset == 350 && after.clips[1].offset == 200, "sessions: after y (past z)");
        const auto free = placeWithInsert (p3, 3, 0, 150);
        CHECK (free.clips[3].offset == 150, "sessions: a free place is kept as is");
        const auto onMain = placeWithInsert (p3, 3, 0, 10);
        CHECK (onMain.clips[3].offset == 0 && onMain.clips[0].offset == 40, "sessions: in front of the main file, it moves too");
    }

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
        j.laneChannels = { 2, 2, 1 };   // lane 2: an empty track (Add Track) is kept
        j.clips = { { mainF, 0, 0, 10 }, { other, 1, 7, 3 } };
        const juce::String json = juce::JSON::toString (toVar (j, dir));
        const auto back = fromVar (juce::JSON::parse (json), dir, mainF, 2, 10);
        CHECK (json.contains ("\"sub/b.wav\"") && back.clips.size () == 2 && back.clips[1].file == other
                   && back.clips[1].lane == 1 && back.clips[1].offset == 7 && back.laneChannels == j.laneChannels
                   && back.laneEmpty (2) && back.clips[0].file == mainF && back.clips[0].length == 10,
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
        // A conversion (rate or channels) is confirmed first: refused here.
        juce::String asked;
        app.onConfirmSessionConversion = [&asked] (const juce::String& what) { asked = what; return false; };
        CHECK (! app.addSessionFile (fd, false, &err) && juce::String (err).contains ("Hz") && asked.contains ("16000 Hz"),
               "sessions: a file at another sample rate asks first; refused, nothing changes (" + juce::String (err) + ")");
        CHECK (! app.addSessionFile (fc, false, &err) && juce::String (err).contains ("channel")
                   && asked.contains ("silent channel"),
               "sessions: a mono file after a stereo main file asks to get a silent channel");
        app.onConfirmSessionConversion = nullptr;
        CHECK (! d->session.active () && eng.numChannels () == 2 && eng.sampleRate () == 8000.0,
               "sessions: refused conversions leave the file alone");

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

        CHECK (app.moveSessionFile (1, 0, 4000, &err) && d->session.clips[1].offset == 8000,
               "sessions: dropped on the second half of the main file, b stays right after it");
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
        CHECK (app.removeSessionFile (2, &err) && eng.numChannels () == 3 && d->session.clips.size () == 2
                   && d->session.laneEmpty (1),
               "sessions: remove c: its track stays, empty (PR #24 review)");
        CHECK (fc.existsAsFile (), "sessions: a removed file stays on disk");
        CHECK (! app.removeSessionTrack (0, &err) && app.removeSessionTrack (1, &err) && eng.numChannels () == 2
                   && d->session.numLanes () == 1,
               "sessions: Remove Track takes the empty track away (never the main one)");
    }
    dir.deleteRecursively ();
}

static void regrSessions_overlayAndDrag()
{
    progress ("regression sessions: file boxes, borders, Move drag with Shift snapping");
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
    CHECK (m.begin (s, g, { 100, 60 }) && m.active (), "sessions: the main file can be dragged (PR #24 review)");
    m.drag (s, g, { 100, 280 }, false, 0.0);   // down to c's lane: the main file keeps its own
    CHECK (m.target ().clip == 0 && m.target ().lane == 0, "sessions: the main file stays on its own track");
    m.cancel ();
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
    progress ("regression sessions: Selection > Move (V), View > Show File Borders, rows bar");
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
        const int eIdx = names.indexOf ("Edit"), sIdx = names.indexOf ("Selection"), vIdx = names.indexOf ("View");
        auto findItem = [] (const juce::PopupMenu& m, const juce::String& text) -> const juce::PopupMenu::Item* {
            for (juce::PopupMenu::MenuItemIterator it (m, true); it.next ();)
                if (it.getItem ().text == text) return &it.getItem ();
            return nullptr;
        };
        {
            const auto em = win->getMenuForIndex (eIdx, "Edit");
            const auto sm = win->getMenuForIndex (sIdx, "Selection");
            const auto vm = win->getMenuForIndex (vIdx, "View");
            const auto* mv = findItem (sm, "Move\tV");
            const auto* borders = findItem (vm, "Show File Borders");
            CHECK (findItem (em, "Session (experimental)") == nullptr && findItem (em, "Add File in Parallel...") == nullptr,
                   "sessions ui: no Edit > Session menu any more (PR #24 review)");
            CHECK (findItem (sm, "Move Files\tV") == nullptr, "sessions ui: Move Files is called Move now (issue #41)");
            CHECK (mv != nullptr && mv->isEnabled && ! mv->isTicked && borders != nullptr && ! borders->isEnabled && borders->isTicked,
                   "sessions ui: Move works on any file (drawings); Show File Borders waits for a session; borders on by default");
            CHECK (! app.sessionBordersShown (), "sessions ui: a plain file never shows a border");
            CHECK (win->sessionScrollBar () != nullptr && ! win->sessionScrollBar ()->isVisible (),
                   "sessions ui: no rows bar without a session");
        }

        std::string err;
        CHECK (app.addSessionFile (fc, true, &err), "sessions ui: add c in parallel");
        win->syncSharedState ();
        {
            const auto sm = win->getMenuForIndex (sIdx, "Selection");
            const auto vm = win->getMenuForIndex (vIdx, "View");
            const auto* mv = findItem (sm, "Move\tV");
            const auto* borders = findItem (vm, "Show File Borders");
            CHECK (mv != nullptr && mv->isEnabled && ! mv->isTicked && borders != nullptr && borders->isEnabled
                       && borders->isTicked && app.sessionBordersShown (),
                   "sessions ui: with a session, Move and the borders (on)");
        }
        win->menuItemSelected (MainWindow::SelectionMoveFiles, sIdx);
        CHECK (app.moveTool, "sessions ui: Selection > Move arms the tool");
        win->keyPressed (juce::KeyPress ('s'), nullptr);
        CHECK (! app.moveTool, "sessions ui: a selection tool (S) puts it down");
        win->keyPressed (juce::KeyPress ('v'), nullptr);
        CHECK (app.moveTool, "sessions ui: V arms Move");
        win->keyPressed (juce::KeyPress ('v'), nullptr);
        CHECK (! app.moveTool, "sessions ui: V again puts it down");

        win->menuItemSelected (MainWindow::ViewShowFileBorders, vIdx);
        CHECK (! app.settings.sessionShowBorders && ! app.sessionBordersShown (), "sessions ui: View > Show File Borders toggles off");

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

        CHECK (app.runSessionContextItem (CtxSessionRemoveFile, 1, 1) && app.doc ()->session.active ()
                   && app.doc ()->session.laneEmpty (1) && app.audioEngine ().numChannels () == 3,
               "sessions ui: right-click > Remove File takes c out; its track stays");
        CHECK (app.runSessionContextItem (CtxSessionRemoveTrack, -1, 1) && ! app.doc ()->session.active ()
                   && app.audioEngine ().numChannels () == 2,
               "sessions ui: right-click > Remove Track ends the session");
        win->syncSharedState ();
        CHECK (! win->sessionScrollBar ()->isVisible (), "sessions ui: the rows bar goes away");
    }
    {
        Application app2 (dir.getChildFile ("store.recent"));
        CHECK (! app2.settings.sessionShowBorders, "sessions ui: Show File Borders off is remembered");
    }
    dir.deleteRecursively ();
}

// PR #24 review, batch 1: tracks added by hand and files dropped on them,
// names and playback by track, Move snapping by default, the name tag
// as a handle, the right-click items, and the spectrogram after a move that
// lengthens the timeline.
static void regrSessions_reviewBatch1()
{
    progress ("regression sessions: PR #24 review batch 1");
    using namespace scsessiontest;
    const juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_sessions_b1");
    dir.deleteRecursively ();
    dir.createDirectory ();
    const juce::File fm = dir.getChildFile ("main.wav"), fb = dir.getChildFile ("b.wav"),
                     fe = dir.getChildFile ("e.wav"), fg = dir.getChildFile ("g.wav"),
                     fc = dir.getChildFile ("c.wav"), ff = dir.getChildFile ("f.wav");
    writeWav (fm, 2, 8000.0, 8000, [] (int ch, int64_t f, double) { return sig (1, ch, f); });
    writeWav (fb, 2, 8000.0, 4000, [] (int ch, int64_t f, double) { return sig (2, ch, f); });
    writeWav (fe, 2, 8000.0, 2000, [] (int ch, int64_t f, double) { return sig (5, ch, f); });
    writeWav (fg, 2, 8000.0, 1000, [] (int ch, int64_t f, double) { return sig (6, ch, f); });
    writeWav (fc, 1, 8000.0, 6000, [] (int ch, int64_t f, double) { return sig (3, ch, f); });
    writeWav (ff, 2, 8000.0, 3000, [] (int ch, int64_t f, double) { return sig (7, ch, f); });
    {
        Application app (dir.getChildFile ("store.recent"));
        CHECK (app.openFile (fm), "review b1: main opens");
        FileDoc* d = app.doc ();
        if (d == nullptr) return;
        auto& eng = d->engine;
        std::unique_ptr<MainWindow> win (new MainWindow (app, "review b1"));
        win->setSize (1200, 800);
        std::string err;

        // Add Track, then two files dropped at 1000 on it: one after the other.
        CHECK (app.addSessionTrack (2, &err) && d->session.numLanes () == 2 && d->session.laneEmpty (1)
                   && eng.numChannels () == 4 && d->session.active (),
               "review b1: Add Track makes an empty stereo track (and a session)");
        CHECK (app.addSessionFilesAt ({ fb, fe }, 1, 1000, &err) && d->session.clips.size () == 3
                   && d->session.clips[1].lane == 1 && d->session.clips[1].offset == 1000
                   && d->session.clips[2].lane == 1 && d->session.clips[2].offset == 5000
                   && sampleIs (eng, 2, 1000, sig (2, 0, 0)) && sampleIs (eng, 3, 5000, sig (5, 1, 0)),
               "review b1: several files dropped on a track go one after the other (" + juce::String (err) + ")");
        CHECK (app.addSessionFilesAt ({ fg }, 1, 4500, &err) && d->session.clips[3].offset == 7000,
               "review b1: dropped on the second half of another file, it goes after (and past the next one)");
        CHECK (app.addSessionTrack (2, &err) && app.addSessionFilesAt ({ fc }, 2, 0, &err)
                   && d->session.laneChannels[2] == 1 && eng.numChannels () == 5,
               "review b1: an empty track takes the channels of the first file dropped on it");
        app.undoStep ();
        CHECK (d->session.laneChannels[2] == 2 && d->session.laneEmpty (2) && eng.numChannels () == 6,
               "review b1: undo gives the empty track back");
        app.redoStep ();
        CHECK (eng.numChannels () == 5, "review b1: redo");
        win->syncSharedState ();

        // Names and routing by track: L R | L R | 1.
        {
            juce::StringArray names;
            for (int ch = 0; ch < 5; ++ch) names.add (app.channelName (ch));
            CHECK (names.joinIntoString (",") == "L,R,L,R,1", "review b1: channels named by track: " + names.joinIntoString (","));
            const auto r = eng.playbackRoutes ();
            CHECK (r.size () == 5 && r[2].index == 0 && r[2].trackChannels == 2 && r[3].index == 1
                       && r[4].index == 0 && r[4].trackChannels == 1,
                   "review b1: playback routes follow the tracks");
            eng.prepareToPlay (256, 8000.0);
            eng.setPosition (0.0);
            eng.play ();
            juce::AudioBuffer<float> out (2, 256);
            out.clear ();
            juce::AudioSourceChannelInfo info (&out, 0, 256);
            eng.getNextAudioBlock (info);
            eng.stop ();
            bool ok = true;
            for (int i = 0; i < 200 && ok; ++i)
            {
                float c[5] = {};
                for (int ch = 0; ch < 5; ++ch) eng.readSamples (ch, i, 1, &c[ch]);
                // Left: both lefts and the mono track; right: both rights and the mono track.
                ok = std::abs (out.getSample (0, i) - (c[0] + c[2] + c[4]) / 3.0f) < 1e-5f
                  && std::abs (out.getSample (1, i) - (c[1] + c[3] + c[4]) / 3.0f) < 1e-5f;
            }
            CHECK (ok, "review b1: both lefts play left, both rights right, the mono track on both");
        }

        // A drop through the window lands on the track and time under the mouse.
        if (auto* wv = win->waveformViewComp ())
        {
            const auto geo = wv->sessionGeometry ();
            int row0 = -1;
            for (size_t i = 0; i < geo.rowChannel.size (); ++i)
                if (geo.rowChannel[i] == 0) row0 = (int) i;
            CHECK (row0 >= 0, "review b1: the main track has a row");
            if (row0 >= 0)
            {
                const juce::Point<int> p ((int) geo.timeToX (0.75), geo.rows[(size_t) row0].getCentreY ());
                const auto inWin = win->getLocalPoint (wv, p);
                const size_t before = d->session.clips.size ();
                CHECK (win->dropFilesOnTracks ({ ff }, inWin.x, inWin.y) && d->session.clips.size () == before + 1
                           && d->session.clips.back ().lane == 0 && d->session.clips.back ().offset == 8000,
                       "review b1: dropped on the main file's second half (0.75 s), f goes right after it (8000)");
            }
            CHECK (! win->dropFilesOnTracks ({ ff }, 2, 2), "review b1: a drop outside the tracks is not for the session");
        }
        else CHECK (false, "review b1: waveform view");

        // Move snaps by default; Shift turns snapping off.
        if (auto* wv = win->waveformViewComp ())
        {
            win->syncSharedState ();
            const int ci = 4;   // c, alone in track 2, at 0
            CHECK (d->session.clips[(size_t) ci].file == fc, "review b1: c is clip 4");
            auto geo = wv->sessionGeometry ();
            int rowC = -1;
            for (size_t i = 0; i < geo.rowChannel.size (); ++i)
                if (geo.rowChannel[i] == 4) rowC = (int) i;
            if (rowC >= 0)
            {
                const int y = geo.rows[(size_t) rowC].getCentreY ();
                const int x0 = (int) geo.timeToX (0.3);
                // c's start 3 px after b's start (1000): within the 8 px of a snap.
                const int dx = (int) std::lround (geo.timeToX (1000.0 / 8000.0) - geo.timeToX (0.0)) + 3;
                const juce::ModifierKeys lmb (juce::ModifierKeys::leftButtonModifier);
                const juce::ModifierKeys shiftLmb (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier);
                app.moveTool = true;
                wv->mouseDown (mkMouse (*wv, x0, y, lmb));
                wv->mouseDrag (mkMouse (*wv, x0 + dx, y, lmb));
                wv->mouseUp (mkMouse (*wv, x0 + dx, y, lmb));
                CHECK (d->session.clips[(size_t) ci].offset == 1000,
                       "review b1: without Shift, c snaps onto b's start (" + juce::String (d->session.clips[(size_t) ci].offset) + ")");
                app.undoStep ();
                geo = wv->sessionGeometry ();
                wv->mouseDown (mkMouse (*wv, x0, y, shiftLmb));
                wv->mouseDrag (mkMouse (*wv, x0 + dx, y, shiftLmb));
                wv->mouseUp (mkMouse (*wv, x0 + dx, y, shiftLmb));
                CHECK (d->session.clips[(size_t) ci].offset > 1000,
                       "review b1: with Shift, no snapping (" + juce::String (d->session.clips[(size_t) ci].offset) + ")");
                app.undoStep ();

                // The name tag is a handle whatever the tool.
                app.setSelectionTool (SpectralSelMode::TimeRange);
                CHECK (! app.moveTool, "review b1: a selection tool puts Move down");
                geo = wv->sessionGeometry ();
                const auto boxes = sc::session::clipBoxes (d->session, geo);
                juce::Rectangle<int> tag;
                for (const auto& b : boxes)
                    if (b.clip == ci) tag = sc::session::nameTag (d->session, geo, b);
                CHECK (! tag.isEmpty () && sc::session::clipAtNameTag (d->session, geo, tag.getCentre ()) == ci,
                       "review b1: c's name tag is found under the mouse");
                const auto tc = tag.getCentre ();
                wv->mouseDown (mkMouse (*wv, tc.x, tc.y, lmb));
                wv->mouseDrag (mkMouse (*wv, tc.x + 20, tc.y, lmb));
                wv->mouseUp (mkMouse (*wv, tc.x + 20, tc.y, lmb));
                CHECK (d->session.clips[(size_t) ci].offset > 0, "review b1: dragging c's name moved it with the Time Range tool");
                app.undoStep ();
            }
            else CHECK (false, "review b1: c's row");
        }

        // Right-click items.
        {
            auto texts = [] (const juce::PopupMenu& m)
            {
                juce::StringArray t;
                for (juce::PopupMenu::MenuItemIterator it (m); it.next ();)
                    if (it.getItem ().text.isNotEmpty ()) t.add (it.getItem ().text);
                return t.joinIntoString ("|");
            };
            juce::PopupMenu onRef, onMain;
            app.addSessionContextItems (onRef, 1, 1);
            app.addSessionContextItems (onMain, 0, 0);
            CHECK (texts (onRef) == "Add Track|Remove File" && texts (onMain) == "Add Track",
                   "review b1: right-click offers Add Track, and Remove File on a referenced file only");
            CHECK (app.addSessionTrack (2, &err), "review b1: one more track");
            juce::PopupMenu onEmpty;
            app.addSessionContextItems (onEmpty, -1, d->session.numLanes () - 1);
            CHECK (texts (onEmpty) == "Add Track|Remove Track", "review b1: Remove Track on an empty track");
            app.undoStep ();
        }

        // The spectrogram follows a move that lengthens the timeline (the old
        // place of the file went silent but kept its picture: issue in review).
        {
            Settings s = app.settings;
            auto peak = [&] ()
            {
                std::vector<float> db;
                app.spectralEngine.renderToDb (eng, s, 64, 64, 0.0, 0.25, { 4 }, db);
                float m = -1000.0f;
                for (float v : db) m = std::max (m, v);
                return m;
            };
            const float before = peak ();
            CHECK (app.moveSessionFile (4, 2, 12000, &err) && eng.numSamples () == 18000,
                   "review b1: c moves to the end, the timeline grows (" + juce::String (err) + ")");
            CHECK (eng.lastRewriteVersion () == eng.contentVersion (), "review b1: the move counts as a rewrite");
            const float after = peak ();
            CHECK (before > after + 30.0f, "review b1: c's old place is drawn silent ("
                                              + juce::String (before, 1) + " -> " + juce::String (after, 1) + " dB)");
        }
    }
    dir.deleteRecursively ();
}

// PR #24 review, batch 2: the main file moves along the timeline, carrying
// what touches it; a file dropped in front of another pushes it.
static void regrSessions_reviewBatch2()
{
    progress ("regression sessions: PR #24 review batch 2 (main file moves, insert in front)");
    using namespace scsessiontest;
    const juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_sessions_b2");
    dir.deleteRecursively ();
    dir.createDirectory ();
    const juce::File fm = dir.getChildFile ("main.wav"), fb = dir.getChildFile ("b.wav"), fx = dir.getChildFile ("x.wav");
    writeWav (fm, 2, 8000.0, 8000, [] (int ch, int64_t f, double) { return sig (1, ch, f); });
    writeWav (fb, 2, 8000.0, 4000, [] (int ch, int64_t f, double) { return sig (2, ch, f); });
    writeWav (fx, 2, 8000.0, 2000, [] (int ch, int64_t f, double) { return sig (8, ch, f); });
    const juce::File store = dir.getChildFile ("store.recent");
    {
        Application app (store);
        CHECK (app.openFile (fm), "review b2: main opens");
        FileDoc* d = app.doc ();
        if (d == nullptr) return;
        auto& eng = d->engine;
        std::string err;

        CHECK (app.addSessionTrack (2, &err) && app.addSessionFilesAt ({ fb }, 1, 0, &err), "review b2: b on a second track");
        // Things on the main file (0..1 s) and one past it (2 s, on b's time only).
        app.markers ().addPointMarker (0.5, "OnMain");
        app.markers ().addPointMarker (1.4, "OnTimeline");
        DrawingItem arrow;
        arrow.kind = DrawingItem::Kind::Arrow;
        arrow.points = { { 0.2, 100.0 }, { 0.4, 300.0 } };
        app.addDrawing (arrow);

        // Move the main file to 0.25 s (2000 samples).
        CHECK (app.moveSessionFile (0, 0, 2000, &err), "review b2: the main file moves (" + juce::String (err) + ")");
        auto markerAt = [&] (const juce::String& name)
        {
            for (const auto& m : d->markers.markers ()) if (m.name == name) return m.start;
            return -1.0;
        };
        CHECK (d->session.clips[0].offset == 2000 && sampleIs (eng, 0, 1999, 0.0f) && sampleIs (eng, 0, 2000, sig (1, 0, 0))
                   && sampleIs (eng, 2, 0, sig (2, 0, 0)),
               "review b2: the main file's audio starts at 0.25 s, b did not move");
        CHECK (std::abs (markerAt ("OnMain") - 0.75) < 1e-9 && std::abs (markerAt ("OnTimeline") - 1.4) < 1e-9,
               "review b2: the marker on the main file follows it, the other stays");
        CHECK (d->drawings.size () == 1 && std::abs (d->drawings[0].points[0].first - 0.45) < 1e-9,
               "review b2: the drawing on the main file follows it");
        app.undoStep ();
        CHECK (d->session.clips[0].offset == 0 && std::abs (markerAt ("OnMain") - 0.5) < 1e-9
                   && std::abs (d->drawings[0].points[0].first - 0.2) < 1e-9 && sampleIs (eng, 0, 0, sig (1, 0, 0)),
               "review b2: undo puts the main file and what it carried back");
        app.redoStep ();
        CHECK (d->session.clips[0].offset == 2000 && std::abs (markerAt ("OnMain") - 0.75) < 1e-9, "review b2: redo");

        // Saving writes the main file's own region and its markers in its own time.
        CHECK (app.saveDocument (*d, fm, false, &err), "review b2: save with the main file moved");
        {
            AudioEngine back;
            CHECK (back.loadFile (fm) && back.numSamples () == 8000 && sampleIs (back, 0, 0, sig (1, 0, 0)),
                   "review b2: the main file on disk is its own audio, from its own start");
            MarkerManager riff;
            riff.loadFromWavFile (fm, 8000.0);
            bool cueOk = false;
            for (const auto& m : riff.markers ()) if (m.name == "OnMain") cueOk = std::abs (m.start - 0.5) < 1e-3;
            CHECK (cueOk, "review b2: its RIFF cue sits in the file's own time (0.5 s)");
        }
    }
    {
        Application app (store);
        CHECK (app.openFile (fm), "review b2: reopen");
        FileDoc* d = app.doc ();
        if (d == nullptr) return;
        CHECK (d->session.clips.size () == 2 && d->session.clips[0].offset == 2000
                   && sampleIs (d->engine, 0, 2000, sig (1, 0, 0)),
               "review b2: the main file comes back where it was moved");
        double onMain = -1.0;
        for (const auto& m : d->markers.markers ()) if (m.name == "OnMain") onMain = m.start;
        CHECK (std::abs (onMain - 0.75) < 1e-9, "review b2: and so do its markers");

        // x dropped on the first half of the main file: in front of it, the
        // main file pushed right by x's length with what it carries.
        std::string err;
        CHECK (app.addSessionFilesAt ({ fx }, 0, 2500, &err) && d->session.clips.back ().offset == 2000
                   && d->session.clips[0].offset == 4000 && sampleIs (d->engine, 0, 4000, sig (1, 0, 0)),
               "review b2: x goes in front of the main file, which moves right (" + juce::String (err) + ")");
        onMain = -1.0;
        for (const auto& m : d->markers.markers ()) if (m.name == "OnMain") onMain = m.start;
        CHECK (std::abs (onMain - 1.0) < 1e-9, "review b2: the pushed main file's marker follows it");
        app.undoStep ();
        CHECK (d->session.clips[0].offset == 2000 && d->session.clips.size () == 2, "review b2: undo the insert");
    }
    dir.deleteRecursively ();
}

// PR #24 review, batch 2: trimming a file by its edges hides its start or
// end without deleting it; saving writes the whole file back.
static void regrSessions_trim()
{
    progress ("regression sessions: trimming a file's edges (PR #24 review)");
    using namespace scsessiontest;
    using namespace sc::session;

    {   // split / join / fingerprint
        std::vector<std::vector<float>> whole { { 1, 2, 3, 4, 5, 6 }, { 7, 8, 9, 10, 11, 12 } }, vis;
        Clip c;
        const bool split = splitTrimmed (whole, 2, 1, vis, c.head, c.tail);
        const std::vector<float> mid { 3, 4, 5 }, head1 { 7, 8 }, tail0 { 6 };
        CHECK (split && vis[0] == mid && (*c.head)[1] == head1 && (*c.tail)[0] == tail0,
               "sessions trim: a file splits into hidden start, visible part, hidden end");
        c.headLength = 2; c.tailLength = 1;
        CHECK (joinTrimmed (c, vis) == whole, "sessions trim: and joins back whole");
        HiddenAudio h, t;
        CHECK (! splitTrimmed (whole, 4, 2, vis, h, t), "sessions trim: trims cannot hide the whole file");
    }

    const juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_sessions_trim");
    dir.deleteRecursively ();
    dir.createDirectory ();
    const juce::File fm = dir.getChildFile ("main.wav"), fb = dir.getChildFile ("b.wav");
    writeWav (fm, 1, 8000.0, 8000, [] (int ch, int64_t f, double) { return sig (1, ch, f); });
    writeWav (fb, 1, 8000.0, 4000, [] (int ch, int64_t f, double) { return sig (2, ch, f); });
    const juce::File store = dir.getChildFile ("store.recent");
    {
        Application app (store);
        CHECK (app.openFile (fm), "sessions trim: main opens");
        FileDoc* d = app.doc ();
        if (d == nullptr) return;
        auto& eng = d->engine;
        std::string err;
        CHECK (app.addSessionTrack (1, &err) && app.addSessionFilesAt ({ fb }, 1, 1000, &err), "sessions trim: b at 1000 on track 2");
        const uint64_t hashWhole = d->session.clips[1].savedHash;

        // Hide b's first 500 and last 1000 samples.
        CHECK (app.trimSessionFile (1, 1500, 2500, &err) && d->session.clips[1].headLength == 500
                   && d->session.clips[1].tailLength == 1000 && sampleIs (eng, 1, 1499, 0.0f)
                   && sampleIs (eng, 1, 1500, sig (2, 0, 500)) && sampleIs (eng, 1, 3999, sig (2, 0, 2999))
                   && sampleIs (eng, 1, 4000, 0.0f),
               "sessions trim: b shows only its middle (" + juce::String (err) + ")");
        CHECK (! app.trimSessionFile (1, 900, 3000, &err), "sessions trim: not past the file's own start");
        CHECK (! app.trimSessionFile (0, 0, 4000, &err), "sessions trim: not the main file");
        uint64_t h = 0;
        {
            const auto& c = d->session.clips[1];
            h = regionHash (d->session, 1, [&eng] (int ch, int64_t st, int64_t n, float* dst) { return eng.readSamples (ch, st, n, dst); });
            CHECK (h == hashWhole && c.savedHash == hashWhole, "sessions trim: a trim alone leaves the file unchanged (no rewrite)");
        }
        // Drag the start back out: the hidden samples come back.
        CHECK (app.trimSessionFile (1, 1200, 2800, &err) && sampleIs (eng, 1, 1200, sig (2, 0, 200)),
               "sessions trim: hidden samples come back when the edge goes out again");
        app.undoStep ();
        CHECK (d->session.clips[1].offset == 1500 && sampleIs (eng, 1, 1500, sig (2, 0, 500)), "sessions trim: undo");

        // Edit the visible part, save: the whole file is written, hidden parts included.
        app.setSelection ({ 0.25, 0.3 });   // 2000..2400 = b's samples 1000..1400
        app.changeGain (6.0f);
        CHECK (app.saveDocument (*d, fm, false, &err), "sessions trim: save");
        AudioEngine back;
        CHECK (back.loadFile (fb) && back.numSamples () == 4000 && sampleIs (back, 0, 0, sig (2, 0, 0))
                   && sampleIs (back, 0, 3999, sig (2, 0, 3999))
                   && sampleIs (back, 0, 1100, sig (2, 0, 1100) * std::pow (10.0f, 0.3f)),
               "sessions trim: b on disk is whole, with the edit in its middle");
    }
    {
        Application app (store);
        CHECK (app.openFile (fm), "sessions trim: reopen");
        FileDoc* d = app.doc ();
        if (d == nullptr) return;
        CHECK (d->session.clips.size () == 2 && d->session.clips[1].headLength == 500 && d->session.clips[1].tailLength == 1000
                   && d->session.clips[1].offset == 1500 && d->session.clips[1].length == 2500
                   && sampleIs (d->engine, 1, 1500, sig (2, 0, 500)) && ! app.docIsUnsaved (*d),
               "sessions trim: the trims come back with the session");
    }

    // The gesture: an edge within 4 px, clamped to the file and its neighbours.
    {
        Session s;
        s.laneChannels = { 1, 1 };
        s.clips = { { juce::File ("/tmp/m.wav"), 0, 0, 8000 }, { juce::File ("/tmp/b.wav"), 1, 1500, 2500 },
                    { juce::File ("/tmp/n.wav"), 1, 5000, 1000 } };
        s.clips[1].headLength = 500;
        s.clips[1].tailLength = 1000;
        ViewGeometry g;
        g.rowChannel = { 0, 1 };
        g.rows = { { 0, 0, 840, 100 }, { 0, 105, 840, 100 } };
        g.plot = { 40, 0, 800, 205 };
        g.rate = 8000.0;
        g.timeToX = [] (double t) { return 40.0 + t * 800.0; };   // 1 s = 800 px, 10 samples/px
        g.xToTime = [] (int x) { return (x - 40) / 800.0; };
        bool left = false;
        CHECK (TrimGesture::edgeAt (s, g, { 190, 150 }, &left) == 1 && left
                   && TrimGesture::edgeAt (s, g, { 441, 150 }, &left) == 1 && ! left
                   && TrimGesture::edgeAt (s, g, { 300, 150 }) == -1 && TrimGesture::edgeAt (s, g, { 40, 50 }) == -1,
               "sessions trim: the edges of a referenced file are handles, not its middle, not the main file");
        TrimGesture t;
        CHECK (t.begin (s, g, { 190, 150 }), "sessions trim: a drag starts on b's left edge");
        t.drag (s, g, { 40, 150 }, false, 0.0);   // way left: stops at the file's start (1000)
        CHECK (t.target ().offset == 1000 && t.target ().length == 3000, "sessions trim: not past the hidden start");
        t.end ();
        CHECK (t.begin (s, g, { 440, 150 }), "sessions trim: a drag starts on b's right edge");
        t.drag (s, g, { 800, 150 }, false, 0.0);  // way right: the neighbour at 5000 stops it
        CHECK (t.target ().offset == 1500 && t.target ().length == 3500, "sessions trim: not over the next file");
    }
    dir.deleteRecursively ();
}

// PR #24 review, batch 2: the same file several times; every copy stays
// identical - an edit of one is copied to the others in a chained step.
static void regrSessions_copies()
{
    progress ("regression sessions: the same file several times (linked copies)");
    using namespace scsessiontest;
    const juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_sessions_copies");
    dir.deleteRecursively ();
    dir.createDirectory ();
    const juce::File fm = dir.getChildFile ("main.wav"), fb = dir.getChildFile ("b.wav");
    writeWav (fm, 1, 8000.0, 8000, [] (int ch, int64_t f, double) { return sig (1, ch, f); });
    writeWav (fb, 1, 8000.0, 4000, [] (int ch, int64_t f, double) { return sig (2, ch, f); });
    {
        Application app (dir.getChildFile ("store.recent"));
        CHECK (app.openFile (fm), "copies: main opens");
        FileDoc* d = app.doc ();
        if (d == nullptr) return;
        auto& eng = d->engine;
        std::string err;
        CHECK (app.addSessionTrack (1, &err) && app.addSessionFilesAt ({ fb }, 1, 0, &err)
                   && app.addSessionFilesAt ({ fb }, 1, 5000, &err) && d->session.clips.size () == 3
                   && sampleIs (eng, 1, 5000, sig (2, 0, 0)),
               "copies: the same file twice on a track (" + juce::String (err) + ")");
        CHECK (! app.syncLinkedSessionFiles (), "copies: nothing to do while nothing changed");

        // Edit the first copy (0.1..0.2 s = its samples 800..1600).
        app.setSelection ({ 0.1, 0.2 });
        app.changeGain (6.0f);
        const float g = std::pow (10.0f, 0.3f);
        CHECK (sampleIs (eng, 1, 1000, sig (2, 0, 1000) * g) && sampleIs (eng, 1, 6000, sig (2, 0, 1000)),
               "copies: the edit changed the first copy only, for now");
        CHECK (app.syncLinkedSessionFiles () && sampleIs (eng, 1, 6000, sig (2, 0, 1000) * g)
                   && sampleIs (eng, 1, 5000, sig (2, 0, 0)),
               "copies: the second copy follows the edit");
        CHECK (! app.syncLinkedSessionFiles (), "copies: and once is enough");
        app.undoStep ();
        CHECK (sampleIs (eng, 1, 1000, sig (2, 0, 1000)) && sampleIs (eng, 1, 6000, sig (2, 0, 1000)),
               "copies: one undo takes the edit and its copy back");
        app.redoStep ();
        CHECK (sampleIs (eng, 1, 1000, sig (2, 0, 1000) * g) && sampleIs (eng, 1, 6000, sig (2, 0, 1000) * g),
               "copies: one redo brings both");
        CHECK (! app.syncLinkedSessionFiles (), "copies: nothing more to copy after undo / redo");

        // A copy added now has the edited content.
        CHECK (app.addSessionFilesAt ({ fb }, 1, 10000, &err) && sampleIs (eng, 1, 11000, sig (2, 0, 1000) * g),
               "copies: a new copy takes the current (edited) content");

        // Save: the file is written once, with the edit; the copies count as saved.
        CHECK (app.saveDocument (*d, fm, false, &err), "copies: save");
        AudioEngine back;
        CHECK (back.loadFile (fb) && back.numSamples () == 4000 && sampleIs (back, 0, 1000, sig (2, 0, 1000) * g),
               "copies: b on disk carries the edit");
        const auto before = bytesOf (fb);
        CHECK (app.saveDocument (*d, fm, false, &err) && bytesOf (fb) == before, "copies: saving again does not rewrite b");

        // A deletion inside the first copy shortens the others too.
        app.setSelection ({ 0.0625, 0.125 });   // samples 500..1000 of the first copy
        app.del ();
        app.syncLinkedSessionFiles ();
        CHECK (d->session.clips[1].length == 3500 && d->session.clips[2].length == 3500 && d->session.clips[3].length == 3500,
               "copies: the copies take the shorter content");
    }
    dir.deleteRecursively ();
}

// PR #24 review, batch 3: files at another rate or channel count are
// converted to the higher of the two; edited referenced files are saved as
// copies or over the originals, asked once and remembered.
static void regrSessions_reviewBatch3()
{
    progress ("regression sessions: PR #24 review batch 3 (conversions, copies or originals)");
    using namespace scsessiontest;
    const juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("sc_sessions_b3");
    dir.deleteRecursively ();
    dir.createDirectory ();
    const juce::File fm = dir.getChildFile ("main.wav"), f16 = dir.getChildFile ("hi16k.wav"),
                     f4 = dir.getChildFile ("lo4k.wav"), fmono = dir.getChildFile ("mono.wav"),
                     fst = dir.getChildFile ("stereo.wav"), fb = dir.getChildFile ("b.wav");
    writeWav (fm, 2, 8000.0, 8000, [] (int ch, int64_t f, double) { return sig (1, ch, f); });
    writeWav (f16, 2, 16000.0, 2000, [] (int, int64_t f, double r) { return 0.3f * (float) std::sin (6.283 * 500.0 * (double) f / r); });
    writeWav (f4, 1, 4000.0, 1000, [] (int, int64_t f, double r) { return 0.3f * (float) std::sin (6.283 * 300.0 * (double) f / r); });
    writeWav (fmono, 1, 8000.0, 1000, [] (int ch, int64_t f, double) { return sig (3, ch, f); });
    writeWav (fst, 2, 8000.0, 1000, [] (int ch, int64_t f, double) { return sig (4, ch, f); });
    writeWav (fb, 2, 8000.0, 2000, [] (int ch, int64_t f, double) { return sig (2, ch, f); });
    const juce::File store = dir.getChildFile ("store.recent");

    {   // Rates: the session goes up to a higher one; a lower file goes up to the session's.
        Application app (store);
        CHECK (app.openFile (fm), "review b3: main opens");
        FileDoc* d = app.doc ();
        if (d == nullptr) return;
        auto& eng = d->engine;
        std::string err;
        juce::String asked;
        app.onConfirmSessionConversion = [&asked] (const juce::String& what) { asked = what; return true; };
        app.markers ().addPointMarker (0.5, "Half");
        CHECK (app.addSessionFilesAt ({ f16 }, -1, 4000, &err) && eng.sampleRate () == 16000.0
                   && d->session.clips[0].length == 16000 && d->session.clips[1].offset == 8000
                   && d->session.clips[1].length == 2000 && asked.contains ("16000 Hz"),
               "review b3: a 16 kHz file takes the session to 16 kHz, places doubled (" + juce::String (err) + ")");
        CHECK (std::abs (d->markers.markers ()[0].start - 0.5) < 1e-9, "review b3: markers stay at their time");
        app.undoStep ();
        CHECK (eng.sampleRate () == 8000.0 && ! d->session.active () && eng.numSamples () == 8000,
               "review b3: undo goes back to 8 kHz");
        app.redoStep ();
        CHECK (eng.sampleRate () == 16000.0 && d->session.clips.size () == 2, "review b3: redo");

        CHECK (app.addSessionFilesAt ({ f4 }, -1, 0, &err) && d->session.clips[2].length == 4000
                   && d->session.clips[2].savedHash == 0 && asked.contains ("4000 Hz"),
               "review b3: a 4 kHz file is resampled up to the session (and will be written)");
        app.onAskReferencedSaveMode = [] (const juce::StringArray&) { return 1; };   // overwrite
        CHECK (app.saveDocument (*d, fm, false, &err), "review b3: save");
        {
            AudioEngine back;
            CHECK (back.loadFile (f4) && back.sampleRate () == 16000.0 && back.numSamples () == 4000
                       && back.loadFile (fm) && back.sampleRate () == 16000.0 && back.numSamples () == 16000,
                   "review b3: the converted files are saved at the session's rate");
        }
    }
    {   // Channels: the larger count, new channels silent.
        writeWav (fm, 2, 8000.0, 8000, [] (int ch, int64_t f, double) { return sig (1, ch, f); });
        sc::fileMetaSidecarPath (fm).deleteFile ();
        Application app (dir.getChildFile ("store2.recent"));
        CHECK (app.openFile (fm), "review b3: a fresh main");
        FileDoc* d = app.doc ();
        if (d == nullptr) return;
        auto& eng = d->engine;
        std::string err;
        CHECK (app.addSessionFilesAt ({ fmono }, 0, 8000, &err) && d->session.laneChannels[0] == 2
                   && sampleIs (eng, 0, 8000, sig (3, 0, 0)) && sampleIs (eng, 1, 8000, 0.0f),
               "review b3: a mono file after a stereo one gets a silent second channel");
        const juce::File fm2 = dir.getChildFile ("mono2.wav");   // f4 was saved at 16 kHz above
        writeWav (fm2, 1, 8000.0, 2000, [] (int ch, int64_t f, double) { return sig (5, ch, f); });
        CHECK (app.addSessionTrack (1, &err) && app.addSessionFilesAt ({ fm2 }, 1, 0, &err)
                   && d->session.laneChannels[1] == 1 && eng.sampleRate () == 8000.0, "review b3: a mono track with a file");
        CHECK (app.addSessionFilesAt ({ fst }, 1, 4000, &err) && d->session.laneChannels[1] == 2
                   && eng.numChannels () == 4 && sampleIs (eng, 3, 0, 0.0f) && sampleIs (eng, 3, 4000, sig (4, 1, 0)),
               "review b3: a stereo file widens the mono track; its first file gets a silent channel");
    }
    {   // Copies or originals: asked once, remembered with the session.
        writeWav (fm, 2, 8000.0, 8000, [] (int ch, int64_t f, double) { return sig (1, ch, f); });
        sc::fileMetaSidecarPath (fm).deleteFile ();
        const juce::File store3 = dir.getChildFile ("store3.recent");
        {
            Application app (store3);
            CHECK (app.openFile (fm), "review b3: main for the save choice");
            FileDoc* d = app.doc ();
            if (d == nullptr) return;
            std::string err;
            int asks = 0;
            int answer = 0;
            app.onAskReferencedSaveMode = [&] (const juce::StringArray& files) { ++asks; return files.contains ("b.wav") ? answer : -1; };
            CHECK (app.addSessionFilesAt ({ fb }, 0, 8000, &err), "review b3: b after the main file");
            CHECK (app.saveDocument (*d, fm, false, &err) && asks == 0, "review b3: nothing edited, nothing asked");
            app.setSelection ({ 1.05, 1.1 });   // inside b
            app.changeGain (6.0f);
            const auto bBefore = bytesOf (fb);
            answer = 0;   // not now
            app.saveDocument (*d, fm, false, &err);
            CHECK (asks == 1 && bytesOf (fb) == bBefore && d->session.referencedSave == sc::session::Session::SaveMode::ask,
                   "review b3: Not Now saves nothing of b and asks again later");
            answer = 2;   // copies
            CHECK (app.saveDocument (*d, fm, false, &err) && asks == 2, "review b3: asked again, Save as Copies");
            const juce::File copy = dir.getChildFile ("b_copy.wav");
            AudioEngine back;
            CHECK (bytesOf (fb) == bBefore && copy.existsAsFile () && back.loadFile (copy)
                       && sampleIs (back, 0, 400, sig (2, 0, 400) * std::pow (10.0f, 0.3f))
                       && d->session.clips[1].file == copy && d->session.clips[1].ownCopy,
                   "review b3: b stays as it was, b_copy.wav has the edit and is the session's file now");
            app.setSelection ({ 1.15, 1.2 });
            app.changeGain (-6.0f);
            CHECK (app.saveDocument (*d, fm, false, &err) && asks == 2 && ! dir.getChildFile ("b_copy2.wav").exists (),
                   "review b3: the choice is remembered; the copy is written in place");
        }
        {
            Application app (store3);
            CHECK (app.openFile (fm), "review b3: reopen");
            FileDoc* d = app.doc ();
            CHECK (d != nullptr && d->session.clips.size () == 2 && d->session.clips[1].file == dir.getChildFile ("b_copy.wav")
                       && d->session.referencedSave == sc::session::Session::SaveMode::copies,
                   "review b3: the session refers to the copy, and remembers the choice");
        }
    }
    dir.deleteRecursively ();
}

static void runRegression_sessions()
{
    regrSessions_model();
    regrSessions_documents();
    regrSessions_overlayAndDrag();
    regrSessions_window();
    regrSessions_reviewBatch1();
    regrSessions_reviewBatch2();
    regrSessions_trim();
    regrSessions_copies();
    regrSessions_reviewBatch3();
}
