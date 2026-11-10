// AI control lot 5: persistent journal, enriched patches (seq, time, op,
// params, source), history.list from the patches, History tab details.
// Included by tests/UnitTests.cpp after control.inc.

static juce::var jrEntries (Application& app, const juce::String& kind, juce::int64 since = 0)
{
    return ctlResult (ctlCall (app, "journal.read", scctl::obj ({ { "sinceSeq", (int) since }, { "limit", 10000 },
                                                                  { "kinds", scctl::arr ({ kind }) } })))
               .getProperty ("entries", {});
}

static void testJournal()
{
    // --- standalone journal: ring + JSONL file, read back beyond the ring ---------
    {
        const juce::File f = makeTempFile ("journal_test.jsonl");
        scctl::Journal j;
        j.setFile (f);
        const int n = (int) scctl::Journal::kCapacity + 50;
        for (int i = 0; i < n; ++i)
            j.append ("command", scctl::obj ({ { "cmd", "x" }, { "i", i } }));
        CHECK (j.lastSeq() == n && j.firstSeq() == 51, "journal: ring keeps the last 10 000 (first seq 51)");
        juce::StringArray lines;
        f.readLines (lines);
        lines.removeEmptyStrings();
        CHECK (lines.size() == n, "journal: every entry mirrored to the JSONL file (" + juce::String (lines.size()) + ")");
        const auto r = j.read (0, {}, 10);
        const auto e = r.getProperty ("entries", {});
        CHECK (e.size() == 10 && (int) e[0].getProperty ("seq", 0) == 1 && (int) e[9].getProperty ("seq", 0) == 10,
               "journal: entries older than the ring are read back from the file");
        CHECK (r.getProperty ("file", {}).toString() == f.getFullPathName(), "journal.read reports the file");
        const auto mid = j.read (40, {}, 20).getProperty ("entries", {});
        bool contiguous = mid.size() == 20;
        for (int i = 0; contiguous && i < 20; ++i)
            contiguous = (int) mid[i].getProperty ("seq", 0) == 41 + i;
        CHECK (contiguous, "journal: file and ring stitch together without a gap");
        const auto gap = j.reserveSeq();
        CHECK (gap == n + 1 && j.append ("command", {}) == n + 2, "journal: reserveSeq shares the counter");
        CHECK (scctl::Journal::sessionFile (1234).getFileName().startsWith ("session-1234-"),
               "journal: session file name");
    }

    // --- 2 MB rotation: entries older than the ring come back from the .1 part ------
    {
        const juce::File f = makeTempFile ("journal_rotate.jsonl");
        scctl::Journal j;
        j.setFile (f);
        const juce::String pad = juce::String::repeatedString ("p", 180);
        const int n = (int) scctl::Journal::kCapacity + 2000;
        for (int i = 0; i < n; ++i)
            j.append ("command", scctl::obj ({ { "cmd", "x" }, { "i", i }, { "pad", pad } }));
        const juce::File older = f.getSiblingFile (f.getFileNameWithoutExtension() + ".1.jsonl");
        CHECK (older.existsAsFile() && f.getSize() <= scctl::Journal::kRotateBytes + 1024
                   && older.getSize() <= scctl::Journal::kRotateBytes + 1024,
               "journal: rotates at 2 MB, the previous part kept");
        const auto e = j.read (0, {}, 5).getProperty ("entries", {});
        CHECK (e.size() == 5 && (int) e[0].getProperty ("seq", 0) == 1,
               "journal: entries older than the ring are read back from the rotated part");
    }

    // --- pruning keeps whole sessions (a session and its .1 part) -------------------
    {
        const juce::File dir = makeTempFile ("journal_prune");
        dir.createDirectory();
        const auto now = juce::Time::getCurrentTime();
        auto make = [&] (const juce::String& name, int ageMinutes) {
            const juce::File f = dir.getChildFile (name);
            f.replaceWithText ("{}\n");
            f.setLastModificationTime (now - juce::RelativeTime::minutes (ageMinutes));
        };
        // Odd pids above 999990: no such process (and never a Windows pid).
        const juce::String own = "session-" + juce::String (scctl::processId()) + "-2026-10-30.jsonl";
        make (own, 60);
        make ("session-999991-2026-11-01.jsonl", 50);
        make ("session-999993-2026-11-02.jsonl", 40);
        make ("session-999993-2026-11-02.1.jsonl", 41);
        make ("session-999995-2026-11-03.1.jsonl", 31);
        make ("session-999995-2026-11-03.jsonl", 30);
        scctl::Journal::pruneOldSessions (dir, 2);
        CHECK (!dir.getChildFile ("session-999991-2026-11-01.jsonl").existsAsFile()
                   && dir.getChildFile ("session-999993-2026-11-02.jsonl").existsAsFile()
                   && dir.getChildFile ("session-999993-2026-11-02.1.jsonl").existsAsFile()
                   && dir.getChildFile ("session-999995-2026-11-03.1.jsonl").existsAsFile(),
               "journal: pruning keeps the newest sessions with their rotated part");
        CHECK (dir.getChildFile (own).existsAsFile(), "journal: the journal of a running instance is never pruned");
        CHECK (scctl::Journal::kKeepSessions == 5, "journal: 5 sessions kept by default");
    }

    // --- patches carry their provenance ---------------------------------------------
    Application app;
    const juce::File wav = ctlMakeWav ("journal_doc.wav", 2, 8000.0, 16000);
    ctlCall (app, "doc.open", scctl::obj ({ { "path", wav.getFullPathName() } }));
    FileDoc& d = *app.doc();

    // API edit.
    const auto r = ctlCall (app, "step.run", scctl::obj ({ { "select", scctl::obj ({ { "start", 0.1 }, { "end", 0.5 } }) },
                                                         { "step", scctl::obj ({ { "type", "changeGain" },
                                                                                 { "params", scctl::obj ({ { "db", -3.0 } }) } }) } }));
    CHECK (ctlOk (r) && (juce::int64) r.getProperty ("patchSeq", 0) == d.undo.lastRecorded().seq,
           "patch: envelope reports patchSeq");
    {
        const Patch& p = d.undo.lastRecorded();
        CHECK (p.seq == 1 && p.timestampMs > 0 && p.opId == "step.run" && p.source == "test"
                   && scctl::toLine (p.params).contains ("changeGain"),
               "patch: API edit stamped with seq, time, command, params and client");
    }

    // Menu/dialog edit: recordActionStep announces it, then the edit runs.
    app.setSelection ({ 0.6, 0.9 });
    app.recordActionStep ("changeGain", scctl::obj ({ { "db", 6.0 } }));
    app.changeGain (6.0f);
    {
        const Patch& p = d.undo.lastRecorded();
        CHECK (p.seq == 2 && p.opId == "changeGain" && p.source == "menu"
                   && (double) p.params.getProperty ("db", 0) == 6.0,
               "patch: dialog edit stamped with its step type, params and source menu");
    }
    // Mouse edit without a step type.
    app.setSelection ({ 1.0, 1.2 });
    app.silence();
    CHECK (d.undo.lastRecorded().opId == "menu" && d.undo.lastRecorded().source == "menu",
           "patch: plain menu edit attributed to the window");

    // Journal: each patch once, as a "history recorded" entry, with its op.
    app.control().pollDocuments();
    const auto hist = jrEntries (app, "history");
    int recorded = 0;
    bool apiCredited = false, menuCredited = false;
    for (int i = 0; i < hist.size(); ++i)
        if (hist[i].getProperty ("what", {}).toString() == "recorded")
        {
            ++recorded;
            if (hist[i].getProperty ("opId", {}).toString() == "step.run" && hist[i].getProperty ("source", {}).toString() == "test")
                apiCredited = true;
            if (hist[i].getProperty ("opId", {}).toString() == "changeGain" && hist[i].getProperty ("source", {}).toString() == "menu")
                menuCredited = true;
        }
    CHECK (recorded == 3, "journal: one 'recorded' entry per patch (" + juce::String (recorded) + ")");
    CHECK (apiCredited && menuCredited, "journal: entries credit the API client and the menu");
    app.control().pollDocuments();
    CHECK (jrEntries (app, "history").size() == hist.size(), "journal: polling again adds nothing");

    // Undo/redo with the mouse path (Application::undoSteps) is journaled.
    app.undoStep();
    const auto afterUndo = jrEntries (app, "history");
    CHECK (afterUndo.size() == hist.size() + 1
               && afterUndo[afterUndo.size() - 1].getProperty ("what", {}).toString() == "undo",
           "journal: undo journaled");
    app.redoStep();

    // Markers (not in the undo stack) -> "meta" entries.
    const auto metaBefore = jrEntries (app, "meta").size();
    d.markers.addPointMarker (0.3, "door_slam");
    app.control().pollDocuments();
    const auto meta = jrEntries (app, "meta");
    CHECK (meta.size() == metaBefore + 1 && (int) meta[meta.size() - 1].getProperty ("markers", 0) == 1,
           "journal: marker change journaled as meta");

    // Resample drops the history: "cleared" with reason resample.
    sc::ActionStep rs;
    rs.type = "resample";
    rs.params = scctl::obj ({ { "rate", 16000.0 } });
    std::string err;
    CHECK (sc::applyActionStep (app, rs, &err), "journal: resample applied");
    app.control().pollDocuments();
    const auto h2 = jrEntries (app, "history");
    const auto lastH = h2[h2.size() - 1];
    CHECK (lastH.getProperty ("what", {}).toString() == "cleared" && lastH.getProperty ("reason", {}).toString() == "resample",
           "journal: history clear with reason resample " + scctl::toLine (lastH));

    // Close -> "doc closed".
    ctlCall (app, "doc.close", scctl::obj ({ { "confirm", true } }));
    const auto docs = jrEntries (app, "doc");
    CHECK (docs.size() >= 2 && docs[docs.size() - 1].getProperty ("what", {}).toString() == "closed",
           "journal: document open/close journaled");
}

static void testHistoryJson()
{
    Application app;
    const juce::File wav = ctlMakeWav ("history_json.wav", 1, 8000.0, 16000);
    ctlCall (app, "doc.open", scctl::obj ({ { "path", wav.getFullPathName() } }));
    for (const char* t : { "fadeIn", "invert", "reverse" })
        ctlCall (app, "step.run", scctl::obj ({ { "select", scctl::obj ({ { "start", 0.2 }, { "end", 0.8 } }) },
                                                { "step", scctl::obj ({ { "type", t } }) } }));
    auto h = ctlResult (ctlCall (app, "history.list"));
    auto st = h.getProperty ("states", {});
    CHECK (st.size() == 4, "history.list: load + 3 states");
    bool stamped = true;
    for (int i = 1; i < st.size(); ++i)
        stamped = stamped && (int) st[i].getProperty ("patchSeq", 0) == i
                  && st[i].getProperty ("ts", {}).toString().endsWithChar ('Z')
                  && st[i].getProperty ("opId", {}).toString() == "step.run"
                  && st[i].getProperty ("source", {}).toString() == "test";
    CHECK (stamped, "history.list: every state carries patchSeq, ts, opId, source from the patch");

    // Future states in redo order, also in the History tab.
    ctlCall (app, "history.undo", scctl::obj ({ { "steps", 2 } }));
    h = ctlResult (ctlCall (app, "history.list"));
    st = h.getProperty ("states", {});
    CHECK (st.size() == 4 && (bool) st[2].getProperty ("future", false) && (int) st[2].getProperty ("patchSeq", 0) == 2
               && (int) st[3].getProperty ("patchSeq", 0) == 3,
           "history.list: the next redo comes first among the future states");
    HistoryPanel panel (app);
    panel.refresh();
    CHECK (panel.numRows() == 4 && panel.rowText (2) == st[2].getProperty ("description", {}).toString()
               && panel.rowText (3) == st[3].getProperty ("description", {}).toString(),
           "History tab: future rows in redo order (first future row = next redo)");
    CHECK (panel.rowDetail (1).contains ("test") && panel.rowDetail (1).containsChar (':'),
           "History tab: time and API source shown for API edits");
    // Clicking the first future row redoes exactly that entry.
    const juce::String expect = panel.rowText (2);
    app.redoSteps (1);
    CHECK (app.undo().lastRecorded().description == expect, "History tab: first future row = the entry redo applies");
}

static void runRegression_journal()
{
    testJournal();
    testHistoryJson();
}
