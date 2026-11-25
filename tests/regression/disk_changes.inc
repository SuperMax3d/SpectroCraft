// Files changed or deleted on disk by another program (issue #70): the pure
// decision, the app's own writes, the background watcher (scanned by hand),
// the footer's alert and its three choices, File > Reload File and the
// doc.reload control command.

static void testFileChangedOnDisk()
{
    using sc::files::DiskStamp;
    using sc::files::DiskState;

    // The decision.
    {
        const DiskStamp known { true, 1000, 64 };
        const DiskStamp changed { true, 5000, 80 };
        const DiskStamp gone {};
        const DiskStamp again { true, 9000, 80 };   // no braces inside CHECK's arguments
        DiskStamp adopt = known;
        CHECK (sc::files::diskStateFor (known, known, false, std::nullopt, std::nullopt, &adopt) == DiskState::same,
               "disk #70: unchanged");
        CHECK (sc::files::diskStateFor (known, changed, false, std::nullopt, std::nullopt, &adopt) == DiskState::modified
                   && sc::files::diskStateFor (known, gone, false, std::nullopt, std::nullopt, &adopt) == DiskState::deleted,
               "disk #70: modified, deleted");
        CHECK (sc::files::diskStateFor (known, changed, true, std::nullopt, std::nullopt, &adopt) == DiskState::same,
               "disk #70: not while the app writes it");
        CHECK (sc::files::diskStateFor (known, changed, false, changed, std::nullopt, &adopt) == DiskState::same
                   && adopt == changed,
               "disk #70: the app's own write is adopted");
        CHECK (sc::files::diskStateFor (known, changed, false, std::nullopt, changed, nullptr) == DiskState::same
                   && sc::files::diskStateFor (known, again, false, std::nullopt, changed, nullptr)
                          == DiskState::modified,
               "disk #70: an ignored change stays quiet until the file changes again");
    }

    // The app's own writes.
    {
        const juce::File f = makeTempFile ("disk70_own.bin");
        bool inside = false;
        {
            const sc::files::ScopedWrite w (f);
            f.replaceWithText ("written by the app");
            inside = sc::files::isBeingWritten (f);
        }
        const auto own = sc::files::lastOwnWrite (f);
        CHECK (inside && ! sc::files::isBeingWritten (f) && own.has_value ()
                   && own->stamp == DiskStamp::of (f) && own->endMs <= sc::files::nowMs (),
               "disk #70: an own write is known while it runs and by the stamp it leaves");
        f.deleteFile ();
    }

    // The watcher, scanned by hand.
    {
        const juce::File f = makeTempFile ("disk70_watch.bin");
        f.replaceWithText ("abc");
        sc::files::DiskWatcher w;
        int calls = 0;
        w.onScanned = [&calls] { ++calls; };
        w.setFiles ({ f });
        CHECK (! w.seenOf (f).has_value (), "disk #70: nothing seen before a scan");
        const double before = sc::files::nowMs ();
        w.scanNow ();
        const auto seen = w.seenOf (f);
        CHECK (calls == 1 && seen.has_value () && seen->stamp == DiskStamp::of (f) && seen->atMs >= before,
               "disk #70: a scan sees the file and when it looked");
        f.deleteFile ();
        w.scanNow ();
        CHECK (w.seenOf (f).has_value () && ! w.seenOf (f)->stamp.exists, "disk #70: and sees it gone");
    }

    // The window: footer, choices, Reload File.
    auto rewrite = [] (const juce::File& f, int64_t frames, int secondsLater)
    {
        const auto t = f.getLastModificationTime ();
        writeWav (f, 1, 8000.0, frames, [] (int, int64_t i, double) { return (float) ((i % 50) - 25) * 0.01f; });
        // Coarse file systems keep whole seconds: move the time on for sure.
        f.setLastModificationTime (t + juce::RelativeTime::seconds (secondsLater));
    };
    const juce::File wav = makeTempFile ("disk70.wav");
    writeWav (wav, 1, 8000.0, 8000, [] (int, int64_t, double) { return 0.0f; });
    const juce::File store = makeTempFile ("disk70.recent");
    store.deleteFile ();
    {
        Application app (store);
        CHECK (app.openFile (wav), "disk #70: open");
        std::unique_ptr<MainWindow> win (new MainWindow (app, "disk 70"));
        win->setSize (1400, 800);
        auto* watcher = win->diskWatcherComp ();
        auto* bar = win->statusBarComp ();
        FileDoc* d = app.doc ();
        if (watcher == nullptr || bar == nullptr || d == nullptr) { CHECK (false, "disk #70: the parts exist"); return; }
        const int id = d->id;

        watcher->scanNow ();
        CHECK (d->diskState == DiskState::same && bar->getDiskAlert ().isEmpty (), "disk #70: nothing to report");

        // Saving it ourselves (audio, then the embedded metadata) is no change.
        std::string err;
        CHECK (app.saveDocument (*d, wav, false, &err), "disk #70: save");
        watcher->scanNow ();
        CHECK (d->diskState == DiskState::same, "disk #70: the app's own save is not reported");
        d->metaDirty = true;
        CHECK (app.saveActiveDocMeta (&err), "disk #70: save the metadata into the WAV");
        watcher->scanNow ();
        CHECK (d->diskState == DiskState::same && bar->getDiskAlert ().isEmpty (),
               "disk #70: nor the app's own metadata patch");

        // Another program rewrites it.
        rewrite (wav, 12000, 5);
        watcher->scanNow ();
        CHECK (d->diskState == DiskState::modified && bar->getDiskAlert () == "File Modified On Disk"
                   && ! bar->diskAlertBounds ().isEmpty (),
               "disk #70: the footer says File Modified On Disk");
        int clicks = 0;
        bar->onDiskAlertClicked = [&clicks] { ++clicks; };
        const auto c = bar->diskAlertBounds ().getCentre ().toFloat ();
        bar->mouseDown (juce::MouseEvent (juce::Desktop::getInstance ().getMainMouseSource (), c, {}, 0.0f, 0.0f, 0.0f,
                                          0.0f, 0.0f, bar, bar, juce::Time::getCurrentTime (), c,
                                          juce::Time::getCurrentTime (), 1, false));
        CHECK (clicks == 1, "disk #70: a click on it opens the choices");

        // Ignore: quiet until the file changes again.
        win->runDiskChoice (MainWindow::DiskIgnore);
        watcher->scanNow ();
        CHECK (d->diskState == DiskState::same && bar->getDiskAlert ().isEmpty (), "disk #70: Ignore");
        rewrite (wav, 16000, 10);
        watcher->scanNow ();
        CHECK (d->diskState == DiskState::modified, "disk #70: a new change is reported again");

        // Reload: the file as it is now, same document id, no history.
        win->runDiskChoice (MainWindow::DiskReloadOrClose);
        d = app.doc ();
        CHECK (d != nullptr && d->id == id && d->engine.numSamples () == 16000 && d->diskState == DiskState::same
                   && ! d->undo.canUndo () && bar->getDiskAlert ().isEmpty () && app.numDocs () == 1,
               "disk #70: Reload File reads it again in place");
        watcher->scanNow ();
        CHECK (d != nullptr && d->diskState == DiskState::same, "disk #70: and nothing to report after");

        // File > Reload File, on a clean document: no question.
        const int fIdx = win->getMenuBarNames ().indexOf ("File");
        const auto fileMenu = win->getMenuForIndex (fIdx, "File");
        bool enabled = false;
        for (juce::PopupMenu::MenuItemIterator it (fileMenu); it.next ();)
            if (it.getItem ().itemID == MainWindow::FileReload)
                enabled = it.getItem ().isEnabled && it.getItem ().text == "Reload File";
        CHECK (enabled, "disk #70: File > Reload File");
        rewrite (wav, 4000, 15);
        if (app.docIsUnsaved (*app.doc ()))
            CHECK (false, "disk #70: the reloaded document is clean (File > Reload File would ask)");
        else
            win->menuItemSelected (MainWindow::FileReload, fIdx);
        d = app.doc ();
        CHECK (d != nullptr && d->id == id && d->engine.numSamples () == 4000, "disk #70: File > Reload File reloads");

        // doc.reload: confirm when there are unsaved changes.
        d->metaDirty = true;
        CHECK (ctlCode (ctlCall (app, "doc.reload")) == "confirm_required", "disk #70: doc.reload asks to confirm");
        rewrite (wav, 6000, 20);
        CHECK (ctlOk (ctlCall (app, "doc.reload", scctl::obj ({ { "confirm", true } })))
                   && app.doc () != nullptr && app.doc ()->engine.numSamples () == 6000 && app.doc ()->id == id,
               "disk #70: doc.reload with confirm");
        d = app.doc ();

        // Deleted: the footer says so; Save File writes it back.
        CHECK (wav.deleteFile (), "disk #70: delete");
        watcher->scanNow ();
        CHECK (d->diskState == DiskState::deleted && bar->getDiskAlert () == "File Deleted On Disk",
               "disk #70: the footer says File Deleted On Disk");
        std::string reloadErr;
        CHECK (! app.reloadDoc (app.activeDocIndex, &reloadErr) && app.doc () == d && ! reloadErr.empty (),
               "disk #70: nothing to reload: the document stays");
        const int jobs = sc::WorkProgressDialog::numActive ();
        win->runDiskChoice (MainWindow::DiskSave);
        CHECK (regMwWaitJobsBackTo (jobs) && wav.existsAsFile (), "disk #70: Save File writes it back");
        watcher->scanNow ();
        CHECK (d->diskState == DiskState::same && bar->getDiskAlert ().isEmpty (), "disk #70: then nothing to report");

        // Deleted again: Close File (Discard changes).
        d->metaDirty = true;
        CHECK (wav.deleteFile (), "disk #70: delete again");
        watcher->scanNow ();
        win->runDiskChoice (MainWindow::DiskReloadOrClose);
        CHECK (app.numDocs () == 0 && bar->getDiskAlert ().isEmpty (), "disk #70: Close File discards it");
        win.reset ();
    }
    store.deleteFile ();
    wav.deleteFile ();
}
