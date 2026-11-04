// Regression tests for the 2026-11 audit fixes (persistence). Included by tests/UnitTests.cpp;
// uses its helpers (CHECK, progress, writeWav, makeTempFile, ...).

// ---- small raw-RIFF builders (byte-exact control over chunk layout) ----
static void persist_put32 (std::string& v, uint32_t x)
{
    for (int i = 0; i < 4; ++i)
        v += (char) ((x >> (8 * i)) & 0xff);
}

static void persist_put16 (std::string& v, uint16_t x)
{
    v += (char) (x & 0xff);
    v += (char) ((x >> 8) & 0xff);
}

// One RIFF chunk; `pad` adds the word-alignment byte after odd payloads.
static std::string persist_chunk (const char* id, const std::string& payload, bool pad = true)
{
    std::string c (id, 4);
    persist_put32 (c, (uint32_t) payload.size());
    c += payload;
    if (pad && (payload.size() & 1) != 0)
        c += '\0';
    return c;
}

// fmt chunk: PCM 16-bit mono.
static std::string persist_fmt (uint32_t rate)
{
    std::string p;
    persist_put16 (p, 1);   // PCM
    persist_put16 (p, 1);   // mono
    persist_put32 (p, rate);
    persist_put32 (p, rate * 2);
    persist_put16 (p, 2);
    persist_put16 (p, 16);
    return persist_chunk ("fmt ", p);
}

// "cue " chunk with one 24-byte cue point.
static std::string persist_cue1 (uint32_t id, uint32_t position, uint32_t sampleOffset)
{
    std::string p;
    persist_put32 (p, 1);
    persist_put32 (p, id);
    persist_put32 (p, position);
    p += "data";
    persist_put32 (p, 0);
    persist_put32 (p, 0);
    persist_put32 (p, sampleOffset);
    return persist_chunk ("cue ", p);
}

// LIST/adtl with one labl entry.
static std::string persist_adtlLabel (uint32_t id, const std::string& text)
{
    std::string labl;
    persist_put32 (labl, id);
    labl += text;
    labl += '\0';
    return persist_chunk ("LIST", "adtl" + persist_chunk ("labl", labl));
}

static bool persist_writeRiff (const juce::File& f, const std::string& body)
{
    std::string d = "RIFF";
    persist_put32 (d, (uint32_t) (4 + body.size()));
    d += "WAVE";
    d += body;
    f.deleteFile();
    return f.replaceWithData (d.data(), d.size());
}

static void persist_sineWav (const juce::File& f)
{
    writeWav (f, 1, 8000.0, 4000, [] (int, int64_t fr, double rate)
             { return 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                               * 300.0 * (double) fr / rate); });
}

static bool persist_readScmd (const juce::File& wav, sc::MetaBundle& b)
{
    std::string json;
    return readRiffChunkPayload (wav, "SCMD", json) && sc::parseBundle (json, b);
}

// ---------------------------------------------------------------------------
// migrateFileMetadata (CLI --meta-mode) used to pass the .markers.json path to
// loadSidecar (which appends ".markers.json" itself), lost the legacy markers
// and then deleted the sidecar.
static void testPersistMigrateLegacyMarkers()
{
    const juce::File wav = makeTempFile ("persist_mig_legacy.wav");
    persist_sineWav (wav);
    const juce::File legacy (wav.getFullPathName() + ".markers.json");

    MarkerManager src;
    const int id = src.addPointMarker (0.25, "Gear {down}");
    src.setComment (id, "line1\nline2 \"q\"");
    CHECK (src.saveSidecar (wav), "persist mig: legacy sidecar written");

    std::string err;
    CHECK (sc::migrateFileMetadata (wav, sc::MetaSaveTarget::Embedded, &err),
           "persist mig: legacy -> embedded succeeds: " + err);

    sc::MetaBundle b;
    CHECK (persist_readScmd (wav, b), "persist mig: SCMD chunk written");
    CHECK (b.markers.size() == 1, "persist mig: legacy marker carried into SCMD");
    if (b.markers.size() == 1)
    {
        CHECK (b.markers[0].name == "Gear {down}", "persist mig: marker name intact");
        CHECK (b.markers[0].comment == "line1\nline2 \"q\"", "persist mig: comment unescaped");
        CHECK (std::abs (b.markers[0].start - 0.25) < 1e-9, "persist mig: marker time intact");
    }

    MarkerManager cue;
    CHECK (cue.loadFromWavFile (wav, 8000.0) && cue.count() == 1
               && std::abs (cue.markers()[0].start - 0.25) < 1e-6,
           "persist mig: cue chunk rebuilt from the legacy marker");
    CHECK (!legacy.existsAsFile(), "persist mig: consumed legacy sidecar removed");

    legacy.deleteFile();
    wav.deleteFile();
}

// An unreadable legacy sidecar must stop the migration instead of being deleted.
static void testPersistMigrateKeepsUnreadableSidecar()
{
    const juce::File wav = makeTempFile ("persist_mig_bad.wav");
    persist_sineWav (wav);
    const juce::File legacy (wav.getFullPathName() + ".markers.json");
    legacy.replaceWithText ("{ \"markers\": [ { \"id\": 1, \"name\": \"trunc");

    std::string err;
    CHECK (!sc::migrateFileMetadata (wav, sc::MetaSaveTarget::Embedded, &err),
           "persist mig: unreadable legacy sidecar reported");
    CHECK (legacy.existsAsFile(), "persist mig: unreadable legacy sidecar kept");
    CHECK (!sc::wavHasMetadataChunks (wav), "persist mig: WAV left untouched");

    legacy.deleteFile();
    wav.deleteFile();
}

// The unified .meta.json must win over the SCMD chunk (FileDoc::load order).
static void testPersistMigrateUnifiedBeatsScmd()
{
    const juce::File wav = makeTempFile ("persist_mig_prio.wav");
    persist_sineWav (wav);

    sc::MetaBundle scmdB;
    Marker ms; ms.id = 1; ms.name = "FromScmd"; ms.start = 0.1;
    scmdB.markers.push_back (ms);
    RiffPatchEntry e;
    std::memcpy (e.fourcc, "SCMD", 4);
    e.payload = sc::serialiseBundle (scmdB, false);
    std::vector<RiffPatchEntry> entries { e };
    CHECK (patchRiffChunks (wav, entries), "persist prio: SCMD added");

    sc::MetaBundle uniB;
    Marker mu; mu.id = 2; mu.name = "FromUnified"; mu.start = 0.2;
    uniB.markers.push_back (mu);
    const juce::File unified = sc::fileMetaSidecarPath (wav);
    unified.replaceWithText (sc::serialiseBundle (uniB, true));

    CHECK (sc::migrateFileMetadata (wav, sc::MetaSaveTarget::Embedded), "persist prio: migrate");
    sc::MetaBundle b;
    CHECK (persist_readScmd (wav, b) && b.markers.size() == 1
               && b.markers[0].name == "FromUnified",
           "persist prio: unified .meta.json markers win over SCMD");
    CHECK (!unified.existsAsFile(), "persist prio: unified sidecar consolidated");

    unified.deleteFile();
    wav.deleteFile();
}

// ---------------------------------------------------------------------------
// Legacy .markers.json: escapes decoded, braces in text, double precision.
static void testPersistLegacySidecarUnescape()
{
    const juce::File wav = makeTempFile ("persist_sidecar.wav");
    MarkerManager src;
    const int a = src.addPointMarker (5000.123456, "Config {gear down}");
    src.setComment (a, "two\nlines, a \\ and \"quotes\" }");
    src.addRegionMarker (10.0, 12.5, "Second");
    CHECK (src.saveSidecar (wav), "persist sidecar: written");

    MarkerManager mm;
    CHECK (mm.loadSidecar (wav), "persist sidecar: loads");
    CHECK (mm.count() == 2, "persist sidecar: both markers recovered");
    if (mm.count() == 2)
    {
        const Marker& m0 = mm.markers()[0];   // sorted by start: Second first
        const Marker& m1 = mm.markers()[1];
        CHECK (m0.name == "Second" && std::abs (m0.end - 12.5) < 1e-9,
               "persist sidecar: region intact");
        CHECK (m1.name == "Config {gear down}", "persist sidecar: brace in name kept");
        CHECK (m1.comment == "two\nlines, a \\ and \"quotes\" }", "persist sidecar: comment unescaped");
        CHECK (std::abs (m1.start - 5000.123456) < 1e-6, "persist sidecar: time keeps double precision");
    }
    juce::File (wav.getFullPathName() + ".markers.json").deleteFile();
}

// ---------------------------------------------------------------------------
// loadFromWavFile: odd-sized foreign chunk (with its RIFF pad byte) before cue.
static void testPersistWavMarkersAfterOddChunk()
{
    const juce::File f = makeTempFile ("persist_odd.wav");
    const std::string body = persist_fmt (8000)
                           + persist_chunk ("junk", "abc")            // 3 bytes + pad
                           + persist_cue1 (1, 800, 800)
                           + persist_adtlLabel (1, "Hi")
                           + persist_chunk ("data", std::string (16000, '\0'));
    CHECK (persist_writeRiff (f, body), "persist odd: wav written");

    MarkerManager mm;
    CHECK (mm.loadFromWavFile (f, 8000.0), "persist odd: markers found after odd chunk");
    CHECK (mm.count() == 1 && std::abs (mm.markers()[0].start - 0.1) < 1e-9
               && mm.markers()[0].name == "Hi",
           "persist odd: cue position and label read");
    f.deleteFile();
}

// cue point with dwPosition = 0 and the real position in dwSampleOffset.
static void testPersistCueSampleOffset()
{
    const juce::File f = makeTempFile ("persist_cueoff.wav");
    const std::string body = persist_fmt (8000)
                           + persist_cue1 (1, 0, 1600)
                           + persist_chunk ("data", std::string (16000, '\0'));
    CHECK (persist_writeRiff (f, body), "persist cue: wav written");

    MarkerManager mm;
    CHECK (mm.loadFromWavFile (f, 8000.0) && mm.count() == 1
               && std::abs (mm.markers()[0].start - 0.2) < 1e-9,
           "persist cue: dwSampleOffset used when dwPosition is 0");
    f.deleteFile();
}

// ---------------------------------------------------------------------------
// patchRiffChunks streams chunks; a last odd chunk without its pad byte (common
// writer bug) no longer makes the whole metadata save fail.
static void testPersistPatchOddLastChunk()
{
    const juce::File f = makeTempFile ("persist_patch.wav");
    const std::string body = persist_fmt (8000)
                           + persist_chunk ("data", std::string (16000, '\0'))
                           + persist_chunk ("junk", "xyz", false);   // pad missing at EOF
    CHECK (persist_writeRiff (f, body), "persist patch: wav written");

    RiffPatchEntry e;
    std::memcpy (e.fourcc, "SCMD", 4);
    e.payload = "{\"a\":1}";
    std::vector<RiffPatchEntry> entries { e };
    std::string err;
    CHECK (patchRiffChunks (f, entries, &err), "persist patch: odd last chunk tolerated: " + err);

    std::string junk, scmd;
    CHECK (readRiffChunkPayload (f, "junk", junk) && junk == "xyz",
           "persist patch: foreign chunk preserved");
    CHECK (readRiffChunkPayload (f, "SCMD", scmd) && scmd == "{\"a\":1}",
           "persist patch: SCMD appended");
    f.deleteFile();
}

// A declared chunk size larger than the file is rejected (no huge allocation).
static void testPersistOversizedChunk()
{
    const juce::File f = makeTempFile ("persist_huge.wav");
    std::string body = persist_fmt (8000);
    body += "SCMD";
    persist_put32 (body, 0x10000000u);   // declared 256 MB, only 4 bytes follow
    body += "{}  ";
    CHECK (persist_writeRiff (f, body), "persist huge: wav written");

    std::string out;
    CHECK (!readRiffChunkPayload (f, "SCMD", out) && out.empty(),
           "persist huge: oversized chunk rejected before allocating");
    f.deleteFile();
}

// ---------------------------------------------------------------------------
// bkdk chunk whose Calibration field is unreadable must not force Pascal.
static void testPersistBkdkUnreadableCalibration()
{
    const juce::File wav = makeTempFile ("persist_bkdk.wav");
    persist_sineWav (wav);

    std::vector<std::string> f = { "2.10", "date" };
    for (int k = 0; k < 11; ++k)
        f.push_back (k == 7 ? "n/a" : "x");
    f.push_back ("END");
    std::string payload;
    for (const auto& s : f) { payload += s; payload += '\0'; }

    RiffPatchEntry e;
    std::memcpy (e.fourcc, "bkdk", 4);
    e.payload = payload;
    std::vector<RiffPatchEntry> entries { e };
    CHECK (patchRiffChunks (wav, entries), "persist bkdk: chunk added");

    sc::BkdkData bk;
    CHECK (sc::parseBkdkPayload (payload, bk) && !bk.hasCalibrationFor (0)
               && bk.calibrationFor (0) == 1.0,
           "persist bkdk: unreadable calibration detected");

    Application app;
    CHECK (app.openFile (wav), "persist bkdk: open");
    if (!app.docs.empty())
    {
        const FileDoc& d = *app.docs[0];
        CHECK (d.scale.unit != ScaleCalibration::Unit::Pascal && !d.scale.isCalibrated(),
               "persist bkdk: no fictitious Pascal calibration");
        CHECK (!d.originalBkdkPayload.empty(), "persist bkdk: raw chunk still preserved");
    }
    wav.deleteFile();
}

// ---------------------------------------------------------------------------
// MetaDirtyFlag: a background save may only clear the flag when nothing
// changed since its snapshot.
static void testPersistMetaDirtyGeneration()
{
    MetaDirtyFlag flag;
    CHECK (!flag, "persist dirty: starts clean");
    flag = true;
    const uint64_t snap = flag.changeCount();
    flag = true;   // edit made while the save runs
    flag.clearIfUnchangedSince (snap);
    CHECK ((bool) flag, "persist dirty: edit during save keeps the doc dirty");
    const uint64_t snap2 = flag.changeCount();
    flag.clearIfUnchangedSince (snap2);
    CHECK (!flag, "persist dirty: unchanged since snapshot -> clean");
    flag = true;
    flag = false;
    CHECK (!flag, "persist dirty: plain assignment still works");
}

// ---------------------------------------------------------------------------
// CSV import: h:mm:ss time codes, non-numeric begin rejected, ';' + decimal comma.
static void testPersistCsvImport()
{
    const juce::File f = makeTempFile ("persist_markers.csv");
    f.replaceWithText (juce::String (
        "Title,Begin,End,Comment\r\n"
        "Long,1:02:03.5,-,\r\n"
        "Junk,abc,-,\r\n"
        "Short,0:04.5,0:05,\r\n"));
    MarkerManager mm;
    CHECK (mm.loadCsv (f, 10000.0) == 2, "persist csv: non-numeric begin skipped");
    if (mm.count() == 2)
    {
        CHECK (std::abs (mm.markers()[0].start - 4.5) < 1e-9
                   && std::abs (mm.markers()[0].end - 5.0) < 1e-9,
               "persist csv: m:ss region");
        CHECK (std::abs (mm.markers()[1].start - 3723.5) < 1e-9, "persist csv: h:mm:ss time code");
    }

    const juce::File g = makeTempFile ("persist_markers_fr.csv");
    g.replaceWithText (juce::String (
        "Title;Begin;End;Comment\r\n"
        "Freins;12,5;14,25;note, avec virgule\r\n"));
    MarkerManager fr;
    CHECK (fr.loadCsv (g, 100.0) == 1, "persist csv: ';' separated file imported");
    if (fr.count() == 1)
        CHECK (std::abs (fr.markers()[0].start - 12.5) < 1e-9
                   && std::abs (fr.markers()[0].end - 14.25) < 1e-9
                   && fr.markers()[0].comment == "note, avec virgule",
               "persist csv: decimal comma and comma inside a field");

    MarkerManager ex;
    ex.addPointMarker (1.000125, "P");
    const juce::File h = makeTempFile ("persist_markers_out.csv");
    CHECK (ex.saveCsv (h), "persist csv: export");
    MarkerManager back;
    CHECK (back.loadCsv (h, 10.0) == 1 && std::abs (back.markers()[0].start - 1.000125) < 1e-9,
           "persist csv: export keeps sub-millisecond precision");
    f.deleteFile(); g.deleteFile(); h.deleteFile();
}

// ---------------------------------------------------------------------------
// Legacy .selections.json parser: \u escapes decoded (juce::JSON), integral
// coordinates in the unified bundle not zeroed.
static void testPersistSelectionsParsing()
{
    std::vector<SavedSpectralSelection> out;
    const juce::String json = "{ \"version\": 1, \"selections\": [ { \"name\": \"Caf\\u00e9\", "
                              "\"shapes\": [ { \"kind\": \"poly\", \"sub\": 0, "
                              "\"pts\": [0.1,100,0.5,100,0.3,900] } ] } ] }";
    CHECK (deserialiseSavedSelections (json, out) && out.size() == 1, "persist sel: legacy parses");
    if (out.size() == 1)
    {
        CHECK (out[0].name == juce::String::fromUTF8 ("Caf\xc3\xa9"), "persist sel: \\u escape decoded");
        CHECK (out[0].data.count() == 1 && out[0].data.contains (0.3, 300.0),
               "persist sel: polygon restored");
    }
    CHECK (!deserialiseSavedSelections ("{ \"selections\": [ { \"shapes\": [] } ] }", out) && out.empty(),
           "persist sel: entry without name rejected, nothing kept");

    sc::MetaBundle b;
    CHECK (sc::parseBundle ("{\"selections\":[{\"name\":\"I\",\"shapes\":[{\"kind\":\"poly\",\"sub\":0,"
                            "\"pts\":[[1,200],[2,200],[2,400]]}]}]}", b)
               && b.selections.size() == 1 && b.selections[0].data.count() == 1,
           "persist sel: bundle with integral points parses");
    if (b.selections.size() == 1 && b.selections[0].data.count() == 1)
    {
        const auto& pts = b.selections[0].data.shapes()[0].pts;
        CHECK (pts.size() == 3 && pts[0].first == 1.0 && pts[0].second == 200.0,
               "persist sel: integral coordinates not zeroed");
    }

    SpectralSelection subOnly;
    subOnly.addRect (0.0, 1.0, 0.0, 100.0, true);
    CHECK (!subOnly.empty() && !subOnly.hasAdditiveShape(), "persist sel: subtract-only has no additive shape");
    subOnly.addRect (0.0, 1.0, 0.0, 100.0, false, false);
    CHECK (subOnly.hasAdditiveShape(), "persist sel: additive shape detected");
}

static void runRegression_persistence()
{
    testPersistMigrateLegacyMarkers();
    testPersistMigrateKeepsUnreadableSidecar();
    testPersistMigrateUnifiedBeatsScmd();
    testPersistLegacySidecarUnescape();
    testPersistWavMarkersAfterOddChunk();
    testPersistCueSampleOffset();
    testPersistPatchOddLastChunk();
    testPersistOversizedChunk();
    testPersistBkdkUnreadableCalibration();
    testPersistMetaDirtyGeneration();
    testPersistCsvImport();
    testPersistSelectionsParsing();
}
