// Regression tests for the 2026-11 audit fixes (audio_engine). Included by tests/UnitTests.cpp;
// uses its helpers (CHECK, progress, writeWav, makeTempFile, ...).

#include <condition_variable>
#include <cstdlib>
#if ! JUCE_WINDOWS
 #include <unistd.h>   // geteuid
#endif

// Distinct, easily recognisable value per channel and frame.
static float aeChanValue (int ch, int64_t f)
{
    return (float) (0.1 * (ch + 1)) + (float) (f % 97) * 1.0e-4f * (float) (ch % 2 == 0 ? 1 : -1);
}

// audio-engine#W0: readSamples() on a streamed file must return the requested
// channel, not channel 0 for every channel.
static void testAeStreamedReadHonoursChannel()
{
    progress ("regression audio-engine#W0: streamed readSamples reads the requested channel");
    const juce::File wav = makeTempFile ("ae_stream3.wav");
    const int64_t frames = 5000;
    writeWav (wav, 3, 48000.0, frames, [] (int ch, int64_t f, double) { return aeChanValue (ch, f); });

    AudioEngine eng;
    eng.setMaxInMemorySampleChannels (0);   // 0 = always stream
    CHECK (eng.loadFile (wav), "W0: load streamed file");
    CHECK (!eng.fullyInMemory (), "W0: file is streamed, not materialised");

    for (int ch = 0; ch < 3; ++ch)
    {
        std::vector<float> buf (300, -9.0f);
        CHECK (eng.readSamples (ch, 1234, 300, buf.data ()), "W0: streamed read succeeds");
        bool same = true;
        for (int i = 0; i < 300; ++i)
            if (std::abs (buf[(size_t) i] - aeChanValue (ch, 1234 + i)) > 1e-6f) { same = false; break; }
        CHECK (same, "W0: streamed channel " + juce::String (ch) + " holds its own data");
    }

    // The tail clamp still works: reading past EOF fills only what exists.
    std::vector<float> tail (10, -9.0f);
    CHECK (eng.readSamples (1, frames - 4, 10, tail.data ()), "W0: read at EOF succeeds");
    CHECK (std::abs (tail[3] - aeChanValue (1, frames - 1)) < 1e-6f, "W0: last frame of channel 1");

    // Playback of the streamed file: output channel N carries file channel N
    // (one output per channel; fewer outputs fold, see issue #18's test).
    eng.prepareToPlay (256, 48000.0);
    eng.play ();
    juce::AudioBuffer<float> out (3, 256);
    out.clear ();
    juce::AudioSourceChannelInfo info (&out, 0, 256);
    eng.getNextAudioBlock (info);
    bool playOk = true;
    for (int i = 0; i < 200 && playOk; ++i)
        for (int ch = 0; ch < 3; ++ch)
            if (std::abs (out.getSample (ch, i) - aeChanValue (ch, i)) > 1e-5f) { playOk = false; break; }
    CHECK (playOk, "W0: streamed playback outputs each file channel on its own output");
    eng.closeFile ();
    wav.deleteFile ();
}

// audio-engine#W5: a mono file plays on every output channel.
static void testAeMonoPlaysOnAllOutputs()
{
    progress ("regression audio-engine#W5: mono playback reaches every output");
    AudioEngine eng;
    std::vector<std::vector<float>> data (1, std::vector<float> (4000));
    for (int i = 0; i < 4000; ++i)
        data[0][(size_t) i] = aeChanValue (0, i);
    CHECK (eng.initNew (1, 48000.0, &data), "W5: mono document");
    eng.prepareToPlay (256, 48000.0);
    eng.play ();
    juce::AudioBuffer<float> out (2, 256);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 256; ++i)
            out.setSample (ch, i, 9.0f);   // stale data from a previous block
    juce::AudioSourceChannelInfo info (&out, 0, 256);
    eng.getNextAudioBlock (info);
    bool same = true;
    for (int i = 0; i < 200; ++i)
        if (out.getSample (1, i) != out.getSample (0, i)
            || std::abs (out.getSample (0, i) - aeChanValue (0, i)) > 1e-5f) { same = false; break; }
    CHECK (same, "W5: right output equals left output for a mono file");
}

// audio-engine NEW (recording append) + W1: appends grow geometric storage
// that `buffer` refers to; content must survive growth, an edit (which swaps
// in an owned buffer), undo, and further appends.
static void testAeRecordingAppendStorage()
{
    progress ("regression audio-engine: recording appendSamples (geometric storage) keeps content");
    AudioEngine eng;
    ::UndoManager um;
    eng.setUndoManager (&um);
    CHECK (eng.initNew (2, 48000.0), "rec: empty stereo document");

    const int tick = 480;
    std::vector<float> inter ((size_t) tick * 2);
    int64_t written = 0;
    auto appendTicks = [&] (int count)
    {
        bool ok = true;
        for (int t = 0; t < count; ++t)
        {
            for (int i = 0; i < tick; ++i)
                for (int ch = 0; ch < 2; ++ch)
                    inter[(size_t) i * 2 + (size_t) ch] = aeChanValue (ch, written + i);
            ok = eng.appendSamples (inter.data (), tick) && ok;
            written += tick;
        }
        return ok;
    };
    auto contentOk = [&] (int64_t from, int64_t to)
    {
        std::vector<float> b ((size_t) (to - from));
        for (int ch = 0; ch < 2; ++ch)
        {
            if (!eng.readSamples (ch, from, to - from, b.data ()))
                return false;
            for (int64_t i = from; i < to; ++i)
                if (b[(size_t) (i - from)] != aeChanValue (ch, i))
                    return false;
        }
        return true;
    };

    CHECK (appendTicks (400), "rec: 400 appends succeed");
    CHECK (eng.numSamples () == written, "rec: length matches appended frames");
    CHECK (contentOk (0, written), "rec: all appended frames read back exactly");

    // An edit swaps in an owned buffer; undo restores; appending resumes.
    CHECK (eng.deleteSamples ({ 1000, 3000 }), "rec: delete during the document's life");
    CHECK (eng.numSamples () == written - 2000, "rec: delete shortened the document");
    ::Patch p;
    CHECK (um.popUndo (p) && eng.applyPatch (p, false), "rec: undo the delete");
    CHECK (eng.numSamples () == written && contentOk (0, written), "rec: undo restored content");
    CHECK (appendTicks (50), "rec: appends after an edit succeed");
    CHECK (eng.numSamples () == written && contentOk (0, written), "rec: content intact after re-growth");

    // In-place overwrite (loop mode) writes into the recording storage.
    for (int i = 0; i < tick; ++i)
        for (int ch = 0; ch < 2; ++ch)
            inter[(size_t) i * 2 + (size_t) ch] = 0.75f;
    CHECK (eng.overwriteSamples (inter.data (), 100, tick), "rec: overwrite in range");
    float v = 0.0f;
    eng.readSamples (1, 100 + tick - 1, 1, &v);
    CHECK (v == 0.75f, "rec: overwrite visible through readSamples");
    eng.readSamples (1, 100 + tick, 1, &v);
    CHECK (v == aeChanValue (1, 100 + tick), "rec: overwrite did not spill past its range");
}

// audio-engine#W1: edits that swap the buffer while another thread plays
// and reads it must neither crash nor deliver garbage (the audio callback
// is now synchronised with every swap).
static void testAeSwapDuringPlayback()
{
    progress ("regression audio-engine#W1: buffer swaps while playing and reading");
    AudioEngine eng;
    std::vector<std::vector<float>> data (2, std::vector<float> (48000));
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 48000; ++i)
            data[(size_t) ch][(size_t) i] = aeChanValue (ch, i);
    CHECK (eng.initNew (2, 48000.0, &data), "W1: stereo document");
    eng.prepareToPlay (256, 48000.0);

    juce::AudioBuffer<float> clip (2, 777);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 777; ++i)
            clip.setSample (ch, i, 0.5f);

    // Non-deterministic by nature: the old race (use-after-free during a
    // swap) only shows when the timing falls badly; a sanitizer build is what
    // makes this test discriminating. Kept cheap and bounded: the two helper
    // threads yield on every pass (they no longer pin two cores while other
    // shards run), the edit loop stops after 150 pairs or 1.5 s, and a
    // watchdog turns a deadlocked edit (swap waiting forever for the audio
    // thread) into an immediate failure instead of a ctest timeout.
    std::atomic<bool> stopFlag { false };
    std::atomic<bool> badOutput { false };
    std::atomic<int> blocks { 0 }, reads { 0 };
    std::mutex wdMutex;
    std::condition_variable wdCv;
    bool editsDone = false;
    std::thread watchdog ([&] {
        std::unique_lock<std::mutex> lk (wdMutex);
        if (!wdCv.wait_for (lk, std::chrono::seconds (30), [&] { return editsDone; }))
        {
            std::cout << "FAIL: W1: paste/delete during playback deadlocked (no progress in 30 s)" << std::endl;
            std::_Exit (3);
        }
    });
    std::thread audio ([&] {
        juce::AudioBuffer<float> out (2, 256);
        while (!stopFlag.load ())
        {
            std::this_thread::yield ();
            ++blocks;
            if (!eng.isPlaying ()) { eng.setPosition (0.0); eng.play (); }
            juce::AudioSourceChannelInfo info (&out, 0, 256);
            eng.getNextAudioBlock (info);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                {
                    const float s = out.getSample (ch, i);
                    if (!std::isfinite (s) || std::abs (s) > 1.0f) badOutput = true;
                }
        }
    });
    std::thread readerThread ([&] {
        std::vector<float> b (4096);
        while (!stopFlag.load ())
        {
            std::this_thread::yield ();
            for (int ch = 0; ch < 2; ++ch)
                eng.readSamples (ch, 0, (int64_t) b.size (), b.data ());
            ++reads;
        }
    });

    // Let both threads get going so the first swaps really race them.
    const auto startWait = juce::Time::getMillisecondCounter ();
    while ((blocks.load () == 0 || reads.load () == 0)
           && juce::Time::getMillisecondCounter () - startWait < 2000)
        std::this_thread::yield ();

    bool editsOk = true;
    int pairs = 0;
    const auto t0 = juce::Time::getMillisecondCounter ();
    for (int k = 0; k < 150 && juce::Time::getMillisecondCounter () - t0 < 1500; ++k, ++pairs)
    {
        editsOk = eng.pasteAt (1000 + k * 13, clip) && editsOk;
        editsOk = eng.deleteSamples ({ 500, 500 + 777 }) && editsOk;
    }
    {
        std::lock_guard<std::mutex> lk (wdMutex);
        editsDone = true;
    }
    wdCv.notify_all ();
    stopFlag = true;
    audio.join ();
    readerThread.join ();
    watchdog.join ();
    CHECK (blocks.load () > 0 && reads.load () > 0,
           "W1: playback and reader threads ran during the edits ("
               + juce::String (blocks.load ()) + " blocks, " + juce::String (reads.load ()) + " reads, "
               + juce::String (pairs) + " edit pairs)");
    CHECK (pairs >= 10, "W1: at least 10 paste/delete pairs ran within 1.5 s (" + juce::String (pairs) + ")");
    CHECK (editsOk, "W1: all edits succeeded while playing");
    CHECK (!badOutput.load (), "W1: playback output stayed valid during swaps");
    CHECK (eng.numSamples () == 48000, "W1: paste/delete pairs kept the length");
}

// audio-engine#W7: resampleTo drops the undo history (its patches address
// the old rate's positions).
static void testAeResampleClearsUndo()
{
    progress ("regression audio-engine#W7: resample clears the undo history");
    AudioEngine eng;
    ::UndoManager um;
    eng.setUndoManager (&um);
    std::vector<std::vector<float>> data (1, std::vector<float> (8000, 0.25f));
    CHECK (eng.initNew (1, 8000.0, &data), "W7: document");
    CHECK (eng.silenceSamples ({ 100, 200 }), "W7: an undoable edit");
    CHECK (um.canUndo (), "W7: edit recorded");
    std::string err;
    CHECK (eng.resampleTo (16000.0, nullptr, nullptr, &err), "W7: resample succeeds");
    CHECK (!um.canUndo () && !um.canRedo (), "W7: history cleared by the resample");
}

// audio-engine#W7/W18: applyPatch rejects a patch inconsistent with the
// buffer; replaceSamples rejects mismatched rows instead of recording them.
static void testAeRejectInconsistentPatches()
{
    progress ("regression audio-engine#W7/W18: inconsistent patches and replacement data are rejected");
    AudioEngine eng;
    ::UndoManager um;
    eng.setUndoManager (&um);
    std::vector<std::vector<float>> data (2, std::vector<float> (1000, 0.25f));
    CHECK (eng.initNew (2, 8000.0, &data), "W18: document");

    ::Patch bad;
    bad.oldStart = 100; bad.oldEnd = 110;
    bad.newData.assign (2, std::vector<float> (10, 0.5f));
    bad.lengthDelta = 50;   // inconsistent: must be 0 for 10 -> 10 samples
    CHECK (!eng.applyPatch (bad, true), "W7: inconsistent patch rejected");
    CHECK (eng.numSamples () == 1000, "W7: rejected patch left the length unchanged");

    ::Patch stale;
    stale.oldStart = 990; stale.oldEnd = 1200;   // beyond the end of the buffer
    stale.oldData.assign (2, std::vector<float> (210, 0.0f));
    stale.lengthDelta = -210;
    CHECK (!eng.applyPatch (stale, true), "W7: out-of-bounds patch rejected");

    std::string err;
    std::vector<std::vector<float>> oneRow (1, std::vector<float> (10, 0.0f));
    CHECK (!eng.replaceSamples ({ 0, 10 }, oneRow, "bad", &err), "W18: row count mismatch rejected");
    std::vector<std::vector<float>> shortRows (2, std::vector<float> (5, 0.0f));
    CHECK (!eng.replaceSamples ({ 0, 10 }, shortRows, "bad", &err), "W18: row length mismatch rejected");
    CHECK (!um.canUndo (), "W18: nothing recorded for rejected replacements");

    // Appending more channels: the new channel is silent before the append,
    // also after undo + redo.
    juce::AudioBuffer<float> app3 (3, 10);
    for (int ch = 0; ch < 3; ++ch)
        for (int i = 0; i < 10; ++i)
            app3.setSample (ch, i, 0.5f);
    CHECK (eng.appendSamples (app3, "append 3ch"), "W18: append with a new channel");
    auto newChannelSilent = [&eng] {
        std::vector<float> b (1000);
        if (!eng.readSamples (2, 0, 1000, b.data ())) return false;
        for (float x : b) if (x != 0.0f) return false;
        float t = 0.0f;
        eng.readSamples (2, 1005, 1, &t);
        return t == 0.5f;
    };
    CHECK (eng.numChannels () == 3 && newChannelSilent (), "W18: new channel silent before the append");
    ::Patch p;
    CHECK (um.popUndo (p) && eng.applyPatch (p, false), "W18: undo the append");
    CHECK (eng.numChannels () == 2 && eng.numSamples () == 1000, "W18: undo restored the shape");
    CHECK (um.popRedo (p) && eng.applyPatch (p, true), "W18: redo the append");
    CHECK (eng.numChannels () == 3 && newChannelSilent (), "W18: redo keeps the new channel silent");
}

// audio-engine#W2: RF64 layout (forced here; used automatically past 4 GiB)
// is written correctly and read back by JUCE.
static void testAeRf64Save()
{
    progress ("regression audio-engine#W2: RF64 WAV save");
    AudioEngine eng;
    std::vector<std::vector<float>> data (2, std::vector<float> (1001));
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 1001; ++i)
            data[(size_t) ch][(size_t) i] = aeChanValue (ch, i);
    CHECK (eng.initNew (2, 44100.0, &data), "W2: document");
    eng.setForceRf64ForSave (true);
    const juce::File out = makeTempFile ("ae_rf64.wav");
    CHECK (eng.saveFile (out), "W2: RF64 save succeeds");

    juce::MemoryBlock mb;
    CHECK (out.loadFileAsData (mb) && mb.getSize () > 80, "W2: file written");
    const auto* b = static_cast<const uint8_t*> (mb.getData ());
    auto u32 = [b] (size_t o) { return (uint32_t) b[o] | ((uint32_t) b[o + 1] << 8)
                                       | ((uint32_t) b[o + 2] << 16) | ((uint32_t) b[o + 3] << 24); };
    auto u64 = [&u32] (size_t o) { return (uint64_t) u32 (o) | ((uint64_t) u32 (o + 4) << 32); };
    CHECK (std::memcmp (b, "RF64", 4) == 0 && u32 (4) == 0xffffffffu, "W2: RF64 header");
    CHECK (std::memcmp (b + 12, "ds64", 4) == 0 && u32 (16) == 28, "W2: ds64 chunk first");
    CHECK (u64 (20) == (uint64_t) mb.getSize () - 8, "W2: ds64 RIFF size = file size - 8");
    CHECK (u64 (28) == (uint64_t) 1001 * 2 * 4, "W2: ds64 data size");
    CHECK (u64 (36) == 1001, "W2: ds64 sample count");

    // fmt then data, the data chunk's 32-bit size holding the -1 sentinel.
    CHECK (std::memcmp (b + 48, "fmt ", 4) == 0 && u32 (52) == 16, "W2: fmt chunk after ds64");
    CHECK (std::memcmp (b + 72, "data", 4) == 0 && u32 (76) == 0xffffffffu, "W2: data chunk size = -1");

    // Read the samples back from the bytes rather than through JUCE: JUCE's
    // WavAudioFormatReader rejects a chunk whose 32-bit size exceeds the bytes
    // left in the file (`length > numBytesAvailable`), so it reads the -1 data
    // size of a forced RF64 file smaller than 4 GiB as zero frames. A real
    // RF64 file (> 4 GiB, the only case saveFile writes one) passes that check.
    // The forced file itself is valid EBU 3306: CoreAudio (afinfo) and ffmpeg
    // both read 2 ch x 1001 frames of float32 from it.
    const bool shape = u32 (56) == 0x00020003u   // WAVE_FORMAT_IEEE_FLOAT, 2 channels
                       && mb.getSize () >= 80 + (size_t) 1001 * 2 * 4;
    CHECK (shape, "W2: RF64 shape read back");
    bool same = shape;
    for (int i = 0; same && i < 1001; ++i)
        for (int ch = 0; ch < 2; ++ch)
        {
            float v;
            std::memcpy (&v, b + 80 + ((size_t) i * 2 + (size_t) ch) * 4, 4);
            if (v != aeChanValue (ch, i)) { same = false; break; }
        }
    CHECK (same, "W2: RF64 samples read back exactly");
    out.deleteFile ();
}

// audio-engine#W3/W16: saves go through a temporary file. A save that cannot
// complete leaves the existing file untouched; a successful one leaves no
// temporary behind; a streamed document saved onto its own source still
// reads correctly afterwards.
static void testAeAtomicSave()
{
    progress ("regression audio-engine#W3: atomic save");
    const juce::File dir = makeTempFile ("ae_atomic");
    dir.deleteRecursively ();
    dir.createDirectory ();
    const juce::File target = dir.getChildFile ("orig.wav");
    writeWav (target, 1, 8000.0, 500, [] (int, int64_t f, double) { return (float) f * 1e-3f; });
    juce::MemoryBlock before;
    target.loadFileAsData (before);

    AudioEngine eng;
    std::vector<std::vector<float>> data (2, std::vector<float> (300, 0.5f));
    CHECK (eng.initNew (2, 8000.0, &data), "W3: document");

   #if ! JUCE_WINDOWS
    // A directory without write permission: the old code could not delete
    // the target, so FileOutputStream appended a second RIFF to it and the
    // save "succeeded" with a corrupted file. Root ignores the permission
    // (CI containers): the directory would stay writable, so skip there.
    if (geteuid () == 0)
    {
        scSkip ("W3: read-only directory case skipped when running as root");
    }
    else
    {
        struct RestoreWritable
        {
            juce::File d;
            ~RestoreWritable () { d.setReadOnly (false); }   // even if a CHECK path returns early
        } restore { dir };
        dir.setReadOnly (true);
        const bool savedRo = eng.saveFile (target);
        dir.setReadOnly (false);
        juce::MemoryBlock after;
        target.loadFileAsData (after);
        CHECK (!savedRo, "W3: save into a read-only directory reports failure");
        CHECK (after == before, "W3: failed save left the existing file byte-identical");
    }
   #else
    scSkip ("W3: read-only directory case not run on Windows (directory read-only attribute does not block file creation)");
   #endif

    CHECK (eng.saveFile (target), "W3: normal save succeeds");
    CHECK (dir.getNumberOfChildFiles (juce::File::findFiles) == 1, "W3: no temporary file left behind");
    AudioEngine back;
    CHECK (back.loadFile (target) && back.numChannels () == 2 && back.numSamples () == 300,
           "W3: saved file replaced the original");
    back.closeFile ();

    // Streamed document saved onto its own source file, then read again.
    const juce::File src = dir.getChildFile ("stream.wav");
    writeWav (src, 2, 8000.0, 2000, [] (int ch, int64_t f, double) { return aeChanValue (ch, f); });
    AudioEngine st;
    st.setMaxInMemorySampleChannels (0);
    CHECK (st.loadFile (src) && !st.fullyInMemory (), "W16: streamed load");
    CHECK (st.saveFile (src), "W16: save a streamed document onto its source");
    std::vector<float> b (100);
    CHECK (st.readSamples (1, 1500, 100, b.data ()) && b[7] == aeChanValue (1, 1507),
           "W16: streamed document still reads after saving over its source");
    st.closeFile ();
    dir.deleteRecursively ();
}


// integration (panels#1): contentVersion() moves with EVERY content change,
// not only recording: the FFT panel and the spectrogram key their caches on it.
static void testAeContentVersionFollowsEdits ()
{
    progress ("regression audio-engine (integration): contentVersion follows edits, undo/redo, load, close");
    AudioEngine eng;
    ::UndoManager um;
    eng.setUndoManager (&um);
    std::vector<std::vector<float>> data (2, std::vector<float> (1000, 0.25f));
    int64_t v = eng.contentVersion ();
    CHECK (eng.initNew (2, 8000.0, &data) && eng.contentVersion () > v, "content version: new document bumps");

    v = eng.contentVersion ();
    std::string err;
    std::vector<std::vector<float>> rows (2, std::vector<float> (10, 0.5f));
    CHECK (eng.replaceSamples ({ 100, 110 }, rows, "edit", &err) && eng.contentVersion () > v,
           "content version: an in-place edit (same length) bumps");

    v = eng.contentVersion ();
    ::Patch p;
    CHECK (um.popUndo (p) && eng.applyPatch (p, false) && eng.contentVersion () > v, "content version: undo bumps");
    v = eng.contentVersion ();
    CHECK (um.popRedo (p) && eng.applyPatch (p, true) && eng.contentVersion () > v, "content version: redo bumps");

    v = eng.contentVersion ();
    CHECK (eng.deleteSamples ({ 0, 10 }, &err) && eng.contentVersion () > v, "content version: delete bumps");

    v = eng.contentVersion ();
    ::Patch bad;
    bad.oldStart = 100; bad.oldEnd = 110;
    bad.newData.assign (2, std::vector<float> (10, 0.5f));
    bad.lengthDelta = 50;
    CHECK (!eng.applyPatch (bad, true) && eng.contentVersion () == v,
           "content version: a rejected patch leaves it alone");

    v = eng.contentVersion ();
    eng.closeFile ();
    CHECK (eng.contentVersion () > v, "content version: close bumps");
}


// Channel mutes (channel numbers in the tracks): a muted channel's output is
// silent, the others untouched; a mono file's mute silences every output.
static void testAeChannelMute()
{
    progress ("regression audio-engine: muted channels play silence");
    AudioEngine eng;
    std::vector<std::vector<float>> data (3, std::vector<float> (4000));
    for (int ch = 0; ch < 3; ++ch)
        for (int i = 0; i < 4000; ++i)
            data[(size_t) ch][(size_t) i] = aeChanValue (ch, i);
    CHECK (eng.initNew (3, 48000.0, &data), "mute: 3-channel document");
    eng.setChannelMuted (1, true);
    CHECK (eng.isChannelMuted (1) && !eng.isChannelMuted (0) && !eng.isChannelMuted (2)
           && !eng.isChannelMuted (7) && !eng.isChannelMuted (-1), "mute: flags per channel");

    eng.prepareToPlay (256, 48000.0);
    eng.play ();
    juce::AudioBuffer<float> out (3, 256);
    juce::AudioSourceChannelInfo info (&out, 0, 256);
    eng.getNextAudioBlock (info);
    bool silent = true, others = true;
    for (int i = 0; i < 200; ++i)
    {
        silent = silent && out.getSample (1, i) == 0.0f;
        others = others && std::abs (out.getSample (0, i) - aeChanValue (0, i)) < 1e-5f
                        && std::abs (out.getSample (2, i) - aeChanValue (2, i)) < 1e-5f;
    }
    CHECK (silent && others, "mute: the muted channel is silent, the others play");

    eng.setChannelMuted (1, false);
    CHECK (eng.mutedChannels ().empty (), "mute: unmuting the last one leaves an empty list");
    eng.getNextAudioBlock (info);   // the next 256 samples
    bool back = true;
    for (int i = 0; i < 200; ++i)
        back = back && std::abs (out.getSample (1, i) - aeChanValue (1, 256 + i)) < 1e-5f;
    CHECK (back, "mute: unmuted, the channel plays again at once");

    eng.setMutedChannels ({ true, false, true });
    eng.closeFile ();
    CHECK (eng.mutedChannels ().empty () && !eng.isChannelMuted (0), "mute: closing the file clears the mutes");

    // Mono: one channel feeds every output, so its mute silences them all.
    AudioEngine mono;
    std::vector<std::vector<float>> md (1, std::vector<float> (4000, 0.5f));
    CHECK (mono.initNew (1, 48000.0, &md), "mute: mono document");
    mono.setChannelMuted (0, true);
    mono.prepareToPlay (256, 48000.0);
    mono.play ();
    juce::AudioBuffer<float> o2 (2, 256);
    juce::AudioSourceChannelInfo i2 (&o2, 0, 256);
    mono.getNextAudioBlock (i2);
    CHECK (o2.getMagnitude (0, 0, 256) == 0.0f && o2.getMagnitude (1, 0, 256) == 0.0f,
           "mute: a muted mono file is silent on every output");

    // ... also while a video track's own (stereo) audio is soloed.
    auto solo = std::make_shared<AudioEngine::SoloAudio> ();
    solo->data.assign (2, std::vector<float> (48000, 0.25f));
    solo->rate = 48000.0;
    mono.setSoloAudio (solo);
    mono.getNextAudioBlock (i2);
    CHECK (o2.getMagnitude (0, 0, 256) == 0.0f && o2.getMagnitude (1, 0, 256) == 0.0f,
           "mute: a muted mono document silences a soloed stereo video too");
    mono.setChannelMuted (0, false);
    mono.getNextAudioBlock (i2);
    CHECK (o2.getMagnitude (1, 0, 256) > 0.2f, "mute: unmuted, the soloed video plays on both outputs");
    mono.setSoloAudio (nullptr);
}

// Issue #18: a device with fewer outputs than the file has channels still
// plays every channel: channel ch folds onto output ch % outputs (2 outputs,
// 4 channels: 1+3 left, 2+4 right), attenuated by the number sharing it.
static void testAeFoldsChannelsOntoFewerOutputs()
{
    progress ("regression audio-engine#18: channels past the outputs fold onto them");
    CHECK (AudioEngine::playbackOutputFor (0, 2) == 0 && AudioEngine::playbackOutputFor (2, 2) == 0
           && AudioEngine::playbackOutputFor (3, 2) == 1 && AudioEngine::playbackOutputFor (1, 8) == 1
           && AudioEngine::playbackOutputFor (1, 0) == -1, "#18: channel -> output");
    CHECK (AudioEngine::playbackFoldGain (0, 4, 2) == 0.5f && AudioEngine::playbackFoldGain (1, 3, 2) == 1.0f
           && AudioEngine::playbackFoldGain (0, 3, 2) == 0.5f && AudioEngine::playbackFoldGain (0, 2, 8) == 1.0f
           && std::abs (AudioEngine::playbackFoldGain (1, 7, 3) - 1.0f / 2.0f) < 1e-6f
           && std::abs (AudioEngine::playbackFoldGain (0, 7, 3) - 1.0f / 3.0f) < 1e-6f,
           "#18: fold gain = 1 / channels sharing the output");

    for (const int nch : { 4, 3 })
    {
        const juce::String tag = " (" + juce::String (nch) + " channels, 2 outputs)";
        AudioEngine eng;
        std::vector<std::vector<float>> data ((size_t) nch, std::vector<float> (4000));
        for (int ch = 0; ch < nch; ++ch)
            for (int i = 0; i < 4000; ++i)
                data[(size_t) ch][(size_t) i] = aeChanValue (ch, i);
        CHECK (eng.initNew (nch, 48000.0, &data), "#18: document" + tag);
        eng.prepareToPlay (256, 48000.0);
        eng.play ();
        juce::AudioBuffer<float> out (2, 256);
        out.clear ();
        juce::AudioSourceChannelInfo info (&out, 0, 256);

        auto expected = [&] (int o, int64_t f, int mutedCh)
        {
            float sum = 0.0f;
            for (int ch = o; ch < nch; ch += 2)
                if (ch != mutedCh)
                    sum += aeChanValue (ch, f);
            return sum * AudioEngine::playbackFoldGain (o, nch, 2);
        };
        auto matches = [&] (int64_t base, int mutedCh)
        {
            for (int i = 0; i < 200; ++i)
                for (int o = 0; o < 2; ++o)
                    if (std::abs (out.getSample (o, i) - expected (o, base + i, mutedCh)) > 1e-5f)
                        return false;
            return true;
        };

        eng.getNextAudioBlock (info);
        CHECK (matches (0, -1), "#18: every channel is heard on its folded output" + tag);

        // A muted channel drops out of its output's mix; the others stay put.
        eng.setChannelMuted (2, true);
        eng.getNextAudioBlock (info);   // the next 256 samples
        CHECK (matches (256, 2), "#18: a muted folded channel leaves the mix, same gain for the rest" + tag);
    }
}

static void runRegression_audio_engine()
{
    testAeStreamedReadHonoursChannel ();
    testAeMonoPlaysOnAllOutputs ();
    testAeRecordingAppendStorage ();
    testAeSwapDuringPlayback ();
    testAeResampleClearsUndo ();
    testAeRejectInconsistentPatches ();
    testAeRf64Save ();
    testAeAtomicSave ();
    testAeContentVersionFollowsEdits ();
    testAeChannelMute ();
    testAeFoldsChannelsOntoFewerOutputs ();
}
