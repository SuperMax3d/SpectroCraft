// File Info panel (issue #67): the WAV's RIFF LIST/INFO fields and DISP
// title (read, written by Save and Save Metadata, preserved next to the
// marker chunks), basic Markdown for Comments / Subject, and the panel
// (closed by default, editable for WAV documents only).

static void testFileInfoPanel()
{
    namespace ri = sc::riffinfo;
    namespace md = sc::markdown;

    // The fields, in Audition's order.
    {
        juce::StringArray ids;
        for (const auto& f : ri::fields ()) ids.add (f.id);
        CHECK (ids.joinIntoString (",") == "DISP,IART,INAM,IGNR,IKEY,ISRF,IMED,IENG,ITCH,ISRC,ICOP,ISFT,ICRD,ICMT,ISBJ",
               "file info #67: the fields in Audition's order: " + ids.joinIntoString (","));
        CHECK (ri::fields ().back ().multiline && ri::fields ()[13].multiline && ! ri::fields ()[0].multiline,
               "file info #67: Comments and Subject are multi-line");
    }

    // Payloads: round trip (UTF-8, odd lengths, unknown ids kept), Latin-1, DISP.
    {
        ri::Values v;
        v["IART"] = juce::String::fromUTF8 ("Ren\xc3\xa9");   // odd byte count
        v["INAM"] = "Take 2";
        v["IPRD"] = "Album kept";                               // not shown, kept
        v["ICMT"] = "Line 1\n**bold** line 2";
        v["IGNR"] = "   ";                                      // blank: not written
        v["DISP"] = "Shown title";
        const auto info = ri::buildInfoListPayload (v);
        CHECK (info.rfind ("INFO", 0) == 0 && info.size () % 2 == 0 && info.find ("IGNR") == std::string::npos
                   && info.find ("DISP") == std::string::npos,
               "file info #67: INFO payload: word-aligned, no blank field, no DISP");
        ri::Values back;
        CHECK (ri::parseInfoListPayload (info, back) && ri::parseDispPayload (ri::buildDispPayload (v), back),
               "file info #67: parse");
        CHECK (back["IART"] == v["IART"] && back["IPRD"] == "Album kept" && back["ICMT"] == v["ICMT"]
                   && back["DISP"] == "Shown title" && back.count ("IGNR") == 0 && ri::sameText (v, back),
               "file info #67: round trip");
        std::string latin = "INFO";
        latin += "IART";
        latin += std::string ("\x04\0\0\0", 4);
        latin += std::string ("Jos\xe9", 4);   // not UTF-8: Latin-1
        ri::Values l;
        CHECK (ri::parseInfoListPayload (latin, l) && l["IART"] == juce::String::fromUTF8 ("Jos\xc3\xa9"),
               "file info #67: Latin-1 texts read as such");
        CHECK (ri::buildInfoListPayload ({}).empty () && ri::buildDispPayload ({}).empty (),
               "file info #67: nothing to write");
    }

    // Markdown.
    {
        const auto lines = md::parse ("# Title\n- item **b**\n2. second\nplain *it* and `x`\n---\n  - nested");
        CHECK (lines.size () == 6 && lines[0].kind == md::Line::Kind::heading1 && lines[1].kind == md::Line::Kind::bullet
                   && lines[2].kind == md::Line::Kind::numbered && lines[2].number == 2
                   && lines[4].kind == md::Line::Kind::rule && lines[5].indent == 1,
               "markdown #67: block kinds");
        const auto runs = md::parseInline ("a **b** *c* `d` e");
        CHECK (runs.size () == 7 && runs[1].text == "b" && runs[1].bold && runs[3].text == "c" && runs[3].italic
                   && runs[5].text == "d" && runs[5].code,
               "markdown #67: bold, italic, code");
        const auto plain = md::parseInline ("2 * 3 * 4 snake_case_name \\*x\\* **open");
        bool styled = false;
        juce::String all;
        for (const auto& r : plain) { styled = styled || r.bold || r.italic || r.code; all += r.text; }
        CHECK (! styled && all == "2 * 3 * 4 snake_case_name *x* **open",
               "markdown #67: spaced stars, snake_case, escapes, unclosed markers stay as typed (" + all + ")");
        CHECK (md::toAttributed ("# T\n**b** x", 12.0f, juce::Colours::white).getNumAttributes () >= 3,
               "markdown #67: the formatted text");
        juce::Range<int> sel;
        const auto w = md::wrap ("say hi now", { 4, 6 }, "**", sel);
        CHECK (w == "say **hi** now" && sel == juce::Range<int> (6, 8), "markdown #67: Bold wraps the selection");
        juce::Range<int> sel2;
        CHECK (md::wrap (w, sel, "**", sel2) == "say hi now" && sel2 == juce::Range<int> (4, 6),
               "markdown #67: and unwraps it");
        juce::Range<int> sel3;
        const auto p = md::prefixLines ("one\ntwo\nthree", { 1, 6 }, "- ", sel3);
        juce::Range<int> sel4;
        CHECK (p == "- one\n- two\nthree" && md::prefixLines (p, sel3, "- ", sel4) == "one\ntwo\nthree",
               "markdown #67: Bullet prefixes the selected lines, and removes it");
    }

    // A WAV with INFO and a marker: read, edited, saved, patched.
    const juce::File wav = makeTempFile ("file_info67.wav");
    writeWav (wav, 1, 8000.0, 8000, [] (int, int64_t i, double) { return (float) (i % 40) * 0.01f; });
    {
        RiffPatchEntry e;
        std::memcpy (e.fourcc, "LIST", 4);
        std::memcpy (e.listType, "INFO", 4);
        ri::Values v;
        v["IART"] = "Max";
        v["IPRD"] = "Kept";
        e.payload = ri::buildInfoListPayload (v);
        CHECK (patchRiffChunks (wav, { e }), "file info #67: a WAV with INFO");
    }
    const juce::File store = makeTempFile ("file_info67.recent");
    store.deleteFile ();
    {
        Application app (store);
        CHECK (app.openFile (wav), "file info #67: open");
        FileDoc* d = app.doc ();
        if (d == nullptr) return;
        CHECK (d->riffInfo["IART"] == "Max" && d->riffInfo["IPRD"] == "Kept", "file info #67: read at load");

        std::unique_ptr<MainWindow> win (new MainWindow (app, "file info 67"));
        win->setSize (1200, 800);
        CHECK (! win->isPanelOpen ("fileinfo"), "file info #67: closed by default");
        bool listed = false;
        const int wIdx = win->getMenuBarNames ().indexOf ("Window");
        const auto wm = win->getMenuForIndex (wIdx, "Window");
        for (juce::PopupMenu::MenuItemIterator it (wm, true); it.next ();)
            listed = listed || it.getItem ().text.startsWith ("File Info");
        CHECK (listed, "file info #67: Window > File Info");
        win->setPanelVisible ("fileinfo", true);
        CHECK (win->isPanelOpen ("fileinfo"), "file info #67: opens");

        juce::Component* found = nullptr;   // the panel, wherever the dock put it
        std::function<void (juce::Component*)> find = [&] (juce::Component* c)
        {
            if (found != nullptr || c == nullptr) return;
            if (c->getComponentID () == "file-info-panel") { found = c; return; }
            for (auto* k : c->getChildren ()) find (k);
        };
        find (win.get ());
        auto* panel = dynamic_cast<FileInfoPanel*> (found);
        CHECK (panel != nullptr, "file info #67: the panel exists");
        if (panel == nullptr) { win.reset (); return; }
        CHECK (panel->isEditable () && panel->editorFor ("IART")->getText () == "Max",
               "file info #67: the panel shows the fields");
        auto* name = panel->editorFor ("INAM");
        name->setText ("Take 3", false);
        name->onTextChange ();
        auto* comments = panel->markdownFor ("ICMT");
        comments->editor ().setText ("make this bold", false);
        comments->editor ().setHighlightedRegion ({ 10, 14 });
        comments->applyFormat (MarkdownField::bold);
        CHECK (d->riffInfo["INAM"] == "Take 3" && d->riffInfo["ICMT"] == "make this **bold**" && app.docIsUnsaved (*d),
               "file info #67: edits go to the document, which is now unsaved");
        comments->setPreview (true);
        juce::Image img (juce::Image::ARGB, comments->getWidth () > 0 ? comments->getWidth () : 300, 200, true);
        {
            juce::Graphics g (img);
            comments->paint (g);   // the preview draws without asserting
        }

        // Save Metadata (the marker chunks stay): the INFO is patched in.
        app.markers ().addPointMarker (0.5, "m1");
        std::string err;
        CHECK (app.saveActiveDocMeta (&err), "file info #67: Save Metadata");
        auto onDisk = ri::readFromWav (wav);
        CHECK (onDisk["INAM"] == "Take 3" && onDisk["ICMT"] == "make this **bold**" && onDisk["IPRD"] == "Kept"
                   && onDisk["IART"] == "Max",
               "file info #67: Save Metadata writes the fields");
        // A full Save writes them too (the WAV is written anew).
        d->riffInfo["DISP"] = "Display";
        CHECK (app.saveDocument (*d, wav, false, &err), "file info #67: Save");
        onDisk = ri::readFromWav (wav);
        CHECK (onDisk["DISP"] == "Display" && onDisk["INAM"] == "Take 3" && onDisk["IPRD"] == "Kept",
               "file info #67: Save writes the fields and the DISP title");
        win.reset ();
    }
    {
        Application app (store);
        CHECK (app.openFile (wav), "file info #67: reopen");
        CHECK (app.doc () != nullptr && app.doc ()->riffInfo["ICMT"] == "make this **bold**"
                   && app.markers ().count () == 1,
               "file info #67: fields and marker read back");
    }
    // Not a WAV: shown read-only with a note.
    {
        Application app (store);
        CHECK (app.newFile (1, 8000.0), "file info #67: a new document");
        if (auto* d = app.doc ())
            d->file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("file_info67.flac");
        FileInfoPanel panel (app);
        CHECK (! panel.isEditable () && panel.noteText ().contains ("WAV"), "file info #67: WAV only");
    }
    store.deleteFile ();
    wav.deleteFile ();
}
