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
        CHECK (! app.addSessionFile (fd, false, &err) && juce::String (err).contains ("Hz"),
               "sessions: a file at another sample rate is refused (" + juce::String (err) + ")");
        CHECK (! app.addSessionFile (fc, false, &err) && juce::String (err).contains ("channel"),
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
    progress ("regression sessions: Selection > Move Files (V), View > Show File Borders, rows bar");
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
            const auto* mv = findItem (sm, "Move Files\tV");
            const auto* borders = findItem (vm, "Show File Borders");
            CHECK (findItem (em, "Session (experimental)") == nullptr && findItem (em, "Add File in Parallel...") == nullptr,
                   "sessions ui: no Edit > Session menu any more (PR #24 review)");
            CHECK (mv != nullptr && ! mv->isEnabled && borders != nullptr && ! borders->isEnabled && borders->isTicked,
                   "sessions ui: Move Files and Show File Borders wait for a session; borders on by default");
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
            const auto* mv = findItem (sm, "Move Files\tV");
            const auto* borders = findItem (vm, "Show File Borders");
            CHECK (mv != nullptr && mv->isEnabled && ! mv->isTicked && borders != nullptr && borders->isEnabled
                       && borders->isTicked && app.sessionBordersShown (),
                   "sessions ui: with a session, Move Files and the borders (on)");
        }
        win->menuItemSelected (MainWindow::SelectionMoveFiles, sIdx);
        CHECK (app.sessionMoveTool, "sessions ui: Selection > Move Files arms the tool");
        win->keyPressed (juce::KeyPress ('s'), nullptr);
        CHECK (! app.sessionMoveTool, "sessions ui: a selection tool (S) puts it down");
        win->keyPressed (juce::KeyPress ('v'), nullptr);
        CHECK (app.sessionMoveTool, "sessions ui: V arms Move Files");
        win->keyPressed (juce::KeyPress ('v'), nullptr);
        CHECK (! app.sessionMoveTool, "sessions ui: V again puts it down");

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
// names and playback by track, Move Files snapping by default, the name tag
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
        CHECK (app.addSessionFilesAt ({ fg }, 1, 2000, &err) && d->session.clips[3].offset == 7000,
               "review b1: a file dropped on another one takes the first free place after it");
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
                const juce::Point<int> p ((int) geo.timeToX (0.5), geo.rows[(size_t) row0].getCentreY ());
                const auto inWin = win->getLocalPoint (wv, p);
                const size_t before = d->session.clips.size ();
                CHECK (win->dropFilesOnTracks ({ ff }, inWin.x, inWin.y) && d->session.clips.size () == before + 1
                           && d->session.clips.back ().lane == 0 && d->session.clips.back ().offset == 8000,
                       "review b1: dropped on the main track at 0.5 s, f goes to the first free place (8000)");
            }
            CHECK (! win->dropFilesOnTracks ({ ff }, 2, 2), "review b1: a drop outside the tracks is not for the session");
        }
        else CHECK (false, "review b1: waveform view");

        // Move Files snaps by default; Shift turns snapping off.
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
                app.sessionMoveTool = true;
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
                CHECK (! app.sessionMoveTool, "review b1: a selection tool puts Move Files down");
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

static void runRegression_sessions()
{
    regrSessions_model();
    regrSessions_documents();
    regrSessions_overlayAndDrag();
    regrSessions_window();
    regrSessions_reviewBatch1();
}
