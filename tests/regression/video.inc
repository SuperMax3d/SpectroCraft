// Video window: picture decoding (VideoFrames), the background frame pump,
// movie audio on macOS (AVFoundation) and the MainWindow wiring (the
// picture follows playback, the playhead and the mouse hover).
// Included by tests/UnitTests.cpp; uses its helpers (CHECK, scSkip,
// scPumpUntil, findTestDataFile, makeTempFile, writeWav...).
//
// Fixture tests/data/video_frames_25fps.mp4 (ffmpeg, libx264 main profile,
// GOP 10 with 2 B-frames, AAC mono): 96x64, 25 fps, 50 frames, 2.0 s. Frame
// N shows its index in binary as six vertical bars (16 px each, bit b at
// x = 16 b .. 16 b + 15; white = 1); the audio is a 1 kHz sine at 0.5.

static const char* const kVideoFixture = "video_frames_25fps.mp4";
// 3840x2160 H.264 High, 25 fps, 25 frames, GOP 10 with B-frames; same bar
// code (each bar 640 px wide).
static const char* const kVideo4kFixture = "video_frames_4k.mp4";

// Frame index encoded in the bars of a decoded fixture frame, -1 if the image
// does not look like one.
// Both fixtures draw six equal bars across the width (96x64: 16 px each;
// the 4K one: 640 px each), so this works at any decoded size.
static int videoFrameIndexOf (const juce::Image& img)
{
    if (!img.isValid() || img.getWidth() < 12 || img.getHeight() < 2)
        return -1;
    int v = 0;
    for (int b = 0; b < 6; ++b)
    {
        const auto c = img.getPixelAt ((int) ((b + 0.5) * img.getWidth() / 6.0), img.getHeight() / 2);
        const float br = c.getBrightness();
        if (br > 0.75f)      v |= (1 << b);
        else if (br > 0.25f) return -1;   // neither black nor white: not a bar
    }
    return v;
}

static juce::File videoFixtureOrSkip()
{
    const juce::File f = findTestDataFile (kVideoFixture);
    if (!f.existsAsFile())
        scMissingFixture (kVideoFixture);
    return f;
}

static void testVideoDisplayTimeRule()
{
    // Playing: the playback position wins over everything.
    CHECK (sc::videoDisplayTime (true, 3.5, true, 1.0, 2.0) == 3.5, "video time: playing -> position");
    // Stopped, hover on and over a view: the hovered time.
    CHECK (sc::videoDisplayTime (false, 0.0, true, 1.0, 2.0) == 1.0, "video time: hover followed");
    // Stopped, hover off: the playhead.
    CHECK (sc::videoDisplayTime (false, 0.0, false, 1.0, 2.0) == 2.0, "video time: hover ignored when off");
    // Stopped, mouse outside the views (-1): the playhead.
    CHECK (sc::videoDisplayTime (false, 0.0, true, -1.0, 2.0) == 2.0, "video time: no hover -> playhead");

    // Extension gates: only containers that may carry a picture.
    CHECK (!sc::isVideoPictureCandidate (juce::File::getCurrentWorkingDirectory().getChildFile ("a.wav")),
           "video candidates: .wav is not one");
    CHECK (!sc::isVideoPictureCandidate (juce::File::getCurrentWorkingDirectory().getChildFile ("a.m4a")),
           "video candidates: .m4a (audio-only container) is not one");
    CHECK (sc::isVideoPictureCandidate (juce::File::getCurrentWorkingDirectory().getChildFile ("a.MP4"))
               == sc::videoDecodingAvailable(),
           "video candidates: .MP4 (any case) follows the platform support");
}

static void testVideoDecoderFrames()
{
    if (!sc::videoDecodingAvailable())
    {   scSkip ("video decoder: no picture decoder on this platform"); return; }
    const juce::File f = videoFixtureOrSkip();
    if (!f.existsAsFile()) return;

    juce::String err;
    bool noTrack = true;
    auto dec = sc::VideoDecoder::open (f, &err, &noTrack);
    if (dec == nullptr)
    {
#if defined(_WIN32)
        // Server images without the Media Foundation feature (or without an
        // H.264 decoder) cannot run this; the error must still be explicit.
        CHECK (err.isNotEmpty(), "video decoder: a failed open explains why");
        scSkip ("video decoder: Media Foundation cannot decode the fixture here: " + err);
        return;
#else
        CHECK (false, "video decoder: fixture opens (" + err + ")");
        return;
#endif
    }
    CHECK (!noTrack, "video decoder: fixture has a video track");

    const auto& inf = dec->info();
    CHECK (inf.width == 96 && inf.height == 64, "video decoder: picture size 96x64 (got "
           + juce::String (inf.width) + "x" + juce::String (inf.height) + ")");
    CHECK (std::abs (inf.frameRate - 25.0) < 0.01, "video decoder: 25 fps");
    CHECK (std::abs (inf.duration - 2.0) < 0.05, "video decoder: 2 s long");
    CHECK (inf.rotation == 0, "video decoder: no rotation");

    auto indexAt = [&] (double t) -> int
    {
        sc::VideoFrame fr;
        if (!dec->frameAt (t, fr)) return -100;
        return videoFrameIndexOf (fr.image);
    };

    // Playback: every frame in order, without seeking after the first one.
    bool allSeq = true;
    int firstBad = -1;
    for (int i = 0; i < 50; ++i)
        if (indexAt (i / 25.0 + 0.02) != i) { allSeq = false; if (firstBad < 0) firstBad = i; }
    CHECK (allSeq, "video decoder: sequential frames 0..49 match their time (first mismatch "
           + juce::String (firstBad) + ")");
    CHECK (dec->seekCount() == 1, "video decoder: sequential playback decodes forward (seeks: "
           + juce::String (dec->seekCount()) + ")");

    // Scrubbing: backward and long forward moves, exact frame boundaries,
    // before the start and past the end.
    int got = indexAt (37 / 25.0 + 0.02);
    CHECK (got == 37, "video decoder: backward to 1.50 s -> frame 37 (got " + juce::String (got) + ")");
    got = indexAt (7 / 25.0 + 0.02);
    CHECK (got == 7, "video decoder: backward across key frames -> frame 7 (got " + juce::String (got) + ")");
    CHECK (indexAt (10 / 25.0) == 10, "video decoder: exactly on a frame boundary -> that frame");
    CHECK (indexAt (0.0) == 0, "video decoder: t = 0 -> frame 0");
    CHECK (indexAt (1.9) == 47, "video decoder: jump forward > 1 s -> frame 47");
    CHECK (indexAt (-1.0) == 0, "video decoder: before the start -> first frame");
    CHECK (indexAt (5.0) == 49, "video decoder: past the end -> last frame");
    CHECK (indexAt (5.0) == 49, "video decoder: past the end, again (no decode)");

    // Random order (deterministic): always floor (t * 25).
    juce::Random rnd (1234);
    bool allRnd = true;
    juce::String bad;
    for (int k = 0; k < 40; ++k)
    {
        const int i = rnd.nextInt (50);
        const double t = i / 25.0 + 0.004 + 0.03 * rnd.nextDouble();
        const int got = indexAt (t);
        if (got != i) { allRnd = false; if (bad.isEmpty()) bad = juce::String (t, 3) + "->" + juce::String (got); }
    }
    CHECK (allRnd, "video decoder: random scrubbing lands on floor(t*fps) " + bad);

    // A file without any picture is reported as such, not as an error.
    const juce::File wav = makeTempFile ("video_no_picture.wav");
    writeWav (wav, 1, 48000.0, 4800, [] (int, int64_t, double) { return 0.0f; });
    bool none = false;
    juce::String e2;
    CHECK (sc::VideoDecoder::open (wav, &e2, &none) == nullptr, "video decoder: a WAV has no picture");
    CHECK (e2.isNotEmpty(), "video decoder: failed open has a message");
}

static void testVideoDecodeSizeRule()
{
    using P = juce::Point<int>;
    CHECK (sc::videoDecodeSizeFor (3840, 2160, 0) == P (3840, 2160), "decode size: 0 = source size");
    CHECK (sc::videoDecodeSizeFor (3840, 2160, 5000) == P (3840, 2160), "decode size: never above the source");
    CHECK (sc::videoDecodeSizeFor (3840, 2160, 2000) == P (3840, 2160), "decode size: 4K for a 2000 px picture");
    CHECK (sc::videoDecodeSizeFor (3840, 2160, 1280) == P (1920, 1080), "decode size: 1/2 for a 1280 px picture");
    CHECK (sc::videoDecodeSizeFor (3840, 2160, 900) == P (960, 540), "decode size: 1/4 for 900 px");
    CHECK (sc::videoDecodeSizeFor (3840, 2160, 100) == P (480, 270), "decode size: at most 1/8");
    CHECK (sc::videoDecodeSizeFor (640, 360, 100) == P (320, 180), "decode size: never below a 320 px long side");
    CHECK (sc::videoDecodeSizeFor (1080, 1920, 700) == P (540, 960), "decode size: portrait uses the long side");
    CHECK (sc::videoDecodeSizeFor (1918, 1078, 600) == P (960, 540), "decode size: even dimensions (959x539 -> 960x540)");
}

static void testVideoDecoder4k()
{
    if (!sc::videoDecodingAvailable())
    {   scSkip ("video 4K: no picture decoder on this platform"); return; }
    const juce::File f = findTestDataFile (kVideo4kFixture);
    if (!f.existsAsFile()) { scMissingFixture (kVideo4kFixture); return; }

    juce::String err;
    auto dec = sc::VideoDecoder::open (f, &err);
    if (dec == nullptr)
    {
#if defined(_WIN32)
        scSkip ("video 4K: Media Foundation cannot decode the fixture here: " + err);
        return;
#else
        CHECK (false, "video 4K: fixture opens (" + err + ")");
        return;
#endif
    }
    CHECK (dec->info().width == 3840 && dec->info().height == 2160, "video 4K: source is 3840x2160");

    // Shown in a ~1280 px (physical) picture: decoded at 1920x1080.
    dec->setNeededLongSide (1280);
    const bool scaled = dec->outputSize() == juce::Point<int> (1920, 1080);
#if defined(__APPLE__)
    CHECK (scaled, "video 4K: decode size follows the shown size (1920x1080)");
#else
    if (!scaled)
        scSkip ("video 4K: this Media Foundation cannot resize, frames stay 4K");
#endif

    const double t0 = juce::Time::getMillisecondCounterHiRes();
    bool allOk = true, sizeOk = true;
    for (int i = 0; i < 25; ++i)
    {
        sc::VideoFrame fr;
        if (!dec->frameAt (i / 25.0 + 0.02, fr) || videoFrameIndexOf (fr.image) != i)
            allOk = false;
        if (fr.image.getWidth() != dec->outputSize().x || fr.image.getHeight() != dec->outputSize().y)
            sizeOk = false;
    }
    const double msPerFrame = (juce::Time::getMillisecondCounterHiRes() - t0) / 25.0;
    CHECK (allOk, "video 4K: every frame decodes to the right picture");
    CHECK (sizeOk, "video 4K: frames have the decoder's output size");
    std::fprintf (stderr, "[video] 4K -> %dx%d: %.1f ms per frame\n",
                  dec->outputSize().x, dec->outputSize().y, msPerFrame);
    // Real time at 25 fps needs < 40 ms; Debug builds on shared CI runners
    // get some slack, the point is catching a full-size software path.
    CHECK (msPerFrame < 120.0, "video 4K: playback decode is fast enough ("
           + juce::String (msPerFrame, 1) + " ms per frame)");

    // Zooming in: back to the source size, same frame on screen.
    dec->setNeededLongSide (0);
    sc::VideoFrame big;
    CHECK (dec->frameAt (12 / 25.0 + 0.02, big) && videoFrameIndexOf (big.image) == 12,
           "video 4K: full-size frame after a size change");
    CHECK (big.image.getWidth() == 3840 && big.image.getHeight() == 2160, "video 4K: source size again");
}

static void testVideoFramePump()
{
    if (!sc::videoDecodingAvailable())
    {   scSkip ("video pump: no picture decoder on this platform"); return; }
    const juce::File f = videoFixtureOrSkip();
    if (!f.existsAsFile()) return;

    sc::VideoFramePump pump;
    pump.setDisplayLongSide (0);
    int opened = 0, frames = 0;
    bool openOk = false;
    pump.onOpened = [&] (bool ok) { ++opened; openOk = ok; };
    pump.onFrame = [&] { ++frames; };

    pump.request (1.0 + 0.02);
    pump.open (f);
    CHECK (scPumpUntil ([&] { return opened > 0 && (frames > 0 || !openOk); }, 10000),
           "video pump: open completes and delivers a frame");
    if (!openOk)
    {   scSkip ("video pump: fixture cannot be decoded here: " + pump.error()); return; }
    CHECK (pump.isOpen() && pump.info().width == 96, "video pump: info available on the message thread");
    CHECK (videoFrameIndexOf (pump.frame().image) == 25, "video pump: frame for the time requested before open");

    // Fast scrubbing: many requests, only the latest matters.
    for (int i = 0; i < 40; ++i)
        pump.request (i / 25.0 + 0.02);
    pump.request (0.44 + 0.02);   // frame 11
    CHECK (pump.waitIdle (10000), "video pump: worker settles");
    CHECK (scPumpUntil ([&] { return videoFrameIndexOf (pump.frame().image) == 11; }, 5000),
           "video pump: shows the latest request after a burst (frame 11)");

    // Same frame again: nothing new is published.
    const int before = frames;
    pump.request (0.44 + 0.03);
    CHECK (pump.waitIdle (10000), "video pump: idle after a same-frame request");
    scPumpUntil ([] { return false; }, 100);
    CHECK (frames == before, "video pump: a request inside the frame on screen publishes nothing");

    // Closing.
    pump.open ({});
    CHECK (pump.waitIdle (10000), "video pump: close settles");
    scPumpUntil ([&] { return opened >= 2; }, 5000);
    CHECK (!pump.isOpen() && !pump.frame().isValid(), "video pump: closed -> no info, no frame");
}

static void testVideoMovieAudio()
{
    // Audio of a movie: FFmpeg on Windows, AVFoundation on macOS.
    if (!sc::videoContainerExtensions().contains (".mp4"))
    {   scSkip ("movie audio: no .mp4 reader in this build"); return; }
    const juce::File f = videoFixtureOrSkip();
    if (!f.existsAsFile()) return;

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    sc::registerVideoFormats (fm);
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    CHECK (r != nullptr, "movie audio: reader created for the .mp4 fixture");
    if (r == nullptr) return;

    CHECK (r->sampleRate == 48000.0, "movie audio: 48 kHz");
    CHECK (r->numChannels == 1, "movie audio: mono");
    CHECK (std::abs (r->lengthInSamples - 96000) <= 2048, "movie audio: about 2 s ("
           + juce::String (r->lengthInSamples) + " samples)");

    const int n = (int) r->lengthInSamples;
    juce::AudioBuffer<float> all (1, n);
    CHECK (r->read (&all, 0, n, 0, true, false), "movie audio: whole read");

    // 1 kHz, amplitude 0.5, measured away from the AAC edges.
    const float* x = all.getReadPointer (0);
    const int a = 9600, b = std::min (n, 86400);
    double sq = 0.0;
    int crossings = 0;
    for (int i = a; i < b; ++i)
    {
        sq += (double) x[i] * x[i];
        if (i > a && (x[i - 1] < 0.0f) != (x[i] < 0.0f)) ++crossings;
    }
    const double rms = std::sqrt (sq / (b - a));
    const double hz = crossings / 2.0 / ((b - a) / 48000.0);
    CHECK (std::abs (rms - 0.5 / std::sqrt (2.0)) < 0.03, "movie audio: level of the 0.5 sine (rms "
           + juce::String (rms, 4) + ")");
    CHECK (std::abs (hz - 1000.0) < 10.0, "movie audio: 1 kHz (" + juce::String (hz, 1) + " Hz)");

    // Random access returns the same samples as the sequential decode.
    for (int start : { 50000, 12345, 70001 })
    {
        juce::AudioBuffer<float> part (1, 4096);
        CHECK (r->read (&part, 0, 4096, start, true, false), "movie audio: random read");
        float maxDiff = 0.0f;
        for (int i = 0; i < 4096 && start + i < n; ++i)
            maxDiff = std::max (maxDiff, std::abs (part.getSample (0, i) - x[start + i]));
        CHECK (maxDiff < 1.0e-3f, "movie audio: random read at " + juce::String (start)
               + " matches the sequential decode (max diff " + juce::String (maxDiff, 6) + ")");
    }
}

static void testVideoViewLayout()
{
    VideoView v;
    v.setSize (300, 300);
    CHECK (v.pictureBounds().isEmpty(), "video view: no frame -> no picture area");

    sc::VideoFrame fr;
    fr.image = juce::Image (juce::Image::ARGB, 96, 64, true);
    fr.time = 0.0;
    v.setFrame (fr, 0);
    auto r = v.pictureBounds();
    // The plot area is the view minus the 14 px right-hand zoom/pan bar.
    CHECK (std::abs (r.getWidth() - 286.0f) < 0.5f && std::abs (r.getHeight() - 190.67f) < 0.5f
           && std::abs (r.getCentreY() - 150.0f) < 0.5f, "video view: 3:2 frame letterboxed in the plot area");
    v.setFrame (fr, 90);
    r = v.pictureBounds();
    CHECK (std::abs (r.getWidth() - 200.0f) < 0.5f && std::abs (r.getHeight() - 300.0f) < 0.5f,
           "video view: rotated 90 degrees -> portrait area");

    v.setFrame (fr, 0);

    // Rotation and zoom are driven by methods (context menu, wheel, keys).
    int notified = -1;
    v.onUserRotationChanged = [&] (int deg) { notified = deg; };
    v.rotateClockwise();
    r = v.pictureBounds();
    CHECK (v.getUserRotation() == 90 && notified == 90, "video view: rotate clockwise turns by 90 degrees");
    CHECK (std::abs (r.getWidth() - 200.0f) < 0.5f && std::abs (r.getHeight() - 300.0f) < 0.5f,
           "video view: rotated picture refits (portrait)");
    v.rotateClockwise(); v.rotateClockwise(); v.rotateClockwise();
    CHECK (v.getUserRotation() == 0, "video view: four turns = back to 0");
    v.rotateCounterClockwise();
    CHECK (v.getUserRotation() == 270 && notified == 270, "video view: counter-clockwise wraps to 270");
    v.rotateClockwise();
    CHECK (v.getUserRotation() == 0, "video view: clockwise and counter-clockwise cancel");
    v.setFrame (fr, 90);
    CHECK (v.totalRotation() == 90, "video view: the track rotation adds to the user's");
    v.setFrame (fr, 0);

    // Zoom in / out by one step; never below fit, capped at kMaxZoom.
    CHECK (std::abs (v.getZoom() - 1.0f) < 1e-6f, "video view: starts at fit");
    v.zoomOut();
    CHECK (v.getZoom() == 1.0f, "video view: zoom never goes below fit");
    v.zoomIn();
    r = v.pictureBounds();
    CHECK (std::abs (v.getZoom() - VideoView::kZoomStep) < 1e-6f
           && std::abs (r.getWidth() - 286.0f * VideoView::kZoomStep) < 0.5f,
           "video view: zoom in enlarges the picture by one step");
    for (int i = 0; i < 30; ++i) v.zoomIn();
    CHECK (v.getZoom() == VideoView::kMaxZoom, "video view: zoom capped");

    // Pan (drag) stays inside the picture: at x8 the 2288x1525 picture can
    // move 1001 px sideways and ~613 px vertically in a 286x300 plot area.
    v.setPan ({ 5000.0f, -5000.0f });
    CHECK (std::abs (v.getPan().x - 1001.0f) < 0.5f && std::abs (v.getPan().y + 612.7f) < 0.5f,
           "video view: pan clamped to the picture edges");
    r = v.pictureBounds();
    CHECK (std::abs (r.getX()) < 0.5f && std::abs (r.getBottom() - 300.0f) < 0.5f,
           "video view: clamped pan puts the picture edge on the view edge");
    for (int i = 0; i < 40; ++i) v.zoomOut();
    CHECK (v.getZoom() == 1.0f && v.getPan().isOrigin(), "video view: back at fit, no pan left");
    v.resetZoom();
    CHECK (v.getZoom() == 1.0f, "video view: double-click / reset fits the picture again");

    // Mouse wheel zooms about the cursor position.
    {
        juce::MouseWheelDetails wd;
        wd.deltaY = 1.0f;
        v.mouseWheelMove (mkMouse (v, 143, 150, {}), wd);
        CHECK (std::abs (v.getZoom() - VideoView::kZoomStep) < 1e-6f && v.getPan().isOrigin(),
               "video view: wheel up at the centre zooms in");
        // Off-centre: the point under the cursor stays put (horizontal axis;
        // at x1.25 the picture is still shorter than the plot area vertically).
        v.resetZoom();
        v.mouseWheelMove (mkMouse (v, 100, 150, {}), wd);
        CHECK (std::abs (v.getPan().x - 10.75f) < 0.5f && std::abs (v.getPan().y) < 0.5f,
               "video view: wheel zoom keeps the cursor point fixed");
        v.resetZoom();
    }

    // Right-drag box zoom: the dragged region fills the plot area, centred.
    {
        const juce::ModifierKeys rmb (juce::ModifierKeys::rightButtonModifier);
        v.mouseDown (mkMouse (v, 60, 80, rmb));
        v.mouseDrag (mkMouse (v, 150, 140, rmb));
        v.mouseUp   (mkMouse (v, 200, 200, rmb));
        const float z = v.getZoom();
        CHECK (std::abs (z - 286.0f / 140.0f) < 1e-3f, "video view: right-drag zooms to the region");
        const auto P = v.pictureBounds();
        // The dragged region's centre (130, 85.33 in fitted coords) now sits
        // at the plot-area centre (143, 150).
        CHECK (std::abs ((P.getX() + 130.0f * z) - 143.0f) < 1.0f
               && std::abs ((P.getY() + 85.333f * z) - 150.0f) < 1.0f,
               "video view: right-drag region centred in the plot area");
        v.resetZoom();

        // A plain right-click (no significant movement) does not zoom.
        v.mouseDown (mkMouse (v, 60, 80, rmb));
        v.mouseUp   (mkMouse (v, 61, 81, rmb));
        CHECK (std::abs (v.getZoom() - 1.0f) < 1e-6f && v.getPan().isOrigin(),
               "video view: plain right-click leaves the zoom alone");
    }

    // The right-hand bar mirrors the vertical zoom/pan and drives it back.
    {
        auto* bar = v.getZoomBar();
        CHECK (bar->isVisible(), "video view: zoom bar shown with a picture");
        const double Fh = 190.667;   // fitted height of the 96x64 frame in a 286x300 plot area
        CHECK (std::abs ((double) bar->thumbRect().getY()) < 1.0
               && std::abs ((double) bar->thumbRect().getBottom() - bar->getHeight()) < 1.0,
               "video view: at fit the thumb fills the track");

        const double lo = 0.25 * Fh, hi = 0.75 * Fh;
        bar->applyWindow (lo, hi);
        CHECK (std::abs (v.getZoom() - 300.0 / (hi - lo)) < 1e-3f,
               "video view: the bar window sets the zoom");
        // The visible fitted range is [lo, hi]: with this geometry that puts
        // pan.y at zero (the picture's centre stays on screen).
        CHECK (std::abs (v.getPan().y) < 0.5f,
               "video view: the bar window centres its midpoint");

        const double dx = 10.0;
        const float pyBefore = v.getPan().y;
        bar->applyWindow (lo + dx, hi + dx);
        CHECK (std::abs (v.getZoom() - 300.0 / (hi - lo)) < 1e-3f
               && std::abs (v.getPan().y - (pyBefore - dx * v.getZoom())) < 0.5f,
               "video view: dragging the thumb pans without changing the zoom");
    }

    // Context-menu actions run through one handler (the popup and the tests).
    {
        int aot = -1;
        int dockCalls = 0;
        v.onToggleAlwaysOnTop = [&] (bool on) { aot = on ? 1 : 0; };
        v.isAlwaysOnTop = [] { return false; };
        v.handleContextMenuId (VideoView::kMenuAlwaysOnTop);
        CHECK (aot == 1, "video view: context menu turns always-on-top on");
        v.isAlwaysOnTop = [] { return true; };
        v.handleContextMenuId (VideoView::kMenuAlwaysOnTop);
        CHECK (aot == 0, "video view: context menu turns it off when already on");
        v.onDockToggle = [&] { ++dockCalls; };
        v.handleContextMenuId (VideoView::kMenuDock);
        CHECK (dockCalls == 1, "video view: context menu asks the owner to dock/undock");
        v.resetZoom();
        v.handleContextMenuId (VideoView::kMenuRotateCW);
        CHECK (v.getUserRotation() == 90, "video view: context menu rotates clockwise");
        v.handleContextMenuId (VideoView::kMenuRotateCCW);
        CHECK (v.getUserRotation() == 0, "video view: context menu rotates counter-clockwise");
        v.handleContextMenuId (VideoView::kMenuZoomIn);
        CHECK (std::abs (v.getZoom() - VideoView::kZoomStep) < 1e-6f,
               "video view: context menu zooms in");
        v.handleContextMenuId (VideoView::kMenuZoomFit);
        CHECK (v.getZoom() == 1.0f && v.getPan().isOrigin(), "video view: context menu fits the picture");
    }

    // The picture lands where pictureBounds says (rotation 0, opaque red).
    fr.image.clear (fr.image.getBounds(), juce::Colours::red);
    v.setFrame (fr, 0);
    const juce::Image snap = v.createComponentSnapshot (v.getLocalBounds(), true, 1.0f);
    CHECK (snap.getPixelAt (150, 150).getRed() > 200 && snap.getPixelAt (150, 150).getGreen() < 60,
           "video view: picture drawn in the middle");
    CHECK (snap.getPixelAt (150, 20).getRed() < 30, "video view: black bars outside the picture");
}

static void testVideoWindowFollowsDocument()
{
    if (!sc::videoDecodingAvailable())
    {   scSkip ("video window: no picture decoder on this platform"); return; }
    const juce::File f = videoFixtureOrSkip();
    if (!f.existsAsFile()) return;

    const juce::File store = makeTempFile ("video_window.recent");
    store.deleteFile();
    Application app (store);
    std::unique_ptr<MainWindow> win (new MainWindow (app, "video window test"));

    CHECK (win->getVideoWindow() == nullptr, "video window: not created without a video document");

    // An audio-only document never creates the window.
    const juce::File wav = makeTempFile ("video_window_audio.wav");
    writeWav (wav, 1, 48000.0, 48000, [] (int, int64_t, double) { return 0.0f; });
    CHECK (app.openFile (wav), "video window: wav opens");
    CHECK (app.doc() != nullptr && app.doc()->videoFile == juce::File(), "video window: wav has no video file");
    CHECK (win->getVideoWindow() == nullptr, "video window: still none for a wav");

    // Opening the movie: the document remembers it and the window opens it.
    if (!app.openFile (f))
    {   scSkip ("video window: the .mp4 audio cannot be opened in this build"); return; }
    CHECK (app.doc()->videoFile == f, "video window: document keeps its video file");
    auto* vw = win->getVideoWindow();
    CHECK (vw != nullptr && vw->getVideoFile() == f, "video window: created for the movie");
    if (vw == nullptr) return;

    auto& pump = vw->pump();
    CHECK (scPumpUntil ([&] { return pump.isOpen() || pump.error().isNotEmpty(); }, 10000),
           "video window: movie opened");
    if (!pump.isOpen())
    {   scSkip ("video window: picture cannot be decoded here: " + pump.error()); return; }
    CHECK (vw->isVisible(), "video window: shown automatically for a new movie");
    CHECK (vw->view().getNotice().isEmpty(), "video window: audio and video lengths agree");

    auto shownIndex = [&] { return videoFrameIndexOf (pump.frame().image); };
    auto showAt = [&] (int expected)
    {
        vw->pollTime();
        pump.waitIdle (10000);
        return scPumpUntil ([&] { return shownIndex() == expected; }, 5000);
    };

    // The test window is on screen: a real mouse resting over it would feed
    // hover times. Keep the MainWindow's hover callback (the wiring under
    // test) and detach the views from the real mouse while it runs.
    auto* sv = win->spectralViewComp();
    auto* wv = win->waveformViewComp();
    CHECK (sv != nullptr && sv->onHoverTime != nullptr, "video window: spectrogram reports hover");
    CHECK (wv != nullptr && wv->onHoverTime != nullptr, "video window: waveform reports hover too");
    if (sv == nullptr || wv == nullptr || !sv->onHoverTime) return;
    const auto hover = sv->onHoverTime;
    sv->onHoverTime = nullptr;
    wv->onHoverTime = nullptr;
    hover (-1.0);

    // Playhead (click in a view, Home/End, status bar): the picture follows.
    app.setPlayhead (1.0 + 0.02);
    CHECK (std::abs (win->videoTimeNow() - 1.02) < 1e-9, "video window: time = playhead when stopped (time "
           + juce::String (win->videoTimeNow(), 4) + ", hover " + juce::String (win->videoHoverTimeNow(), 4)
           + ", playhead " + juce::String (app.playhead, 4) + ", playing "
           + juce::String ((int) app.audioEngine().isPlaying()) + ", main shown "
           + juce::String ((int) win->isShowing()) + ")");
    CHECK (showAt (25), "video window: picture follows the playhead (frame 25)");

    // Hover over the spectrogram: the picture follows the mouse, then
    // returns to the playhead once it leaves. The option is off by default
    // for a new user; this test turns it on from the Window menu first.
    {
        const juce::StringArray names = win->getMenuBarNames();
        const int wIdx = names.indexOf ("Window");
        CHECK (!win->isVideoFollowingHover(), "video window: hover follow off by default");
        win->menuItemSelected (MainWindow::WindowVideoFollowHover, wIdx);
        CHECK (win->isVideoFollowingHover(), "video window: hover follow on from the menu");

        hover (7 / 25.0 + 0.02);
        CHECK (std::abs (win->videoTimeNow() - 0.30) < 1e-9, "video window: hover time used");
        CHECK (showAt (7), "video window: picture follows the hover (frame 7)");
        hover (-1.0);
        CHECK (showAt (25), "video window: back to the playhead when the mouse leaves");

        // Turned off from the Window menu: hover is ignored (and persisted).
        win->menuItemSelected (MainWindow::WindowVideoFollowHover, wIdx);
        CHECK (!win->isVideoFollowingHover(), "video window: hover toggle off");
        {
            juce::PropertiesFile s2 (store, juce::PropertiesFile::Options());
            CHECK (s2.getIntValue ("video_follow_hover", 1) == 0, "video window: hover toggle persisted");
        }
        hover (7 / 25.0 + 0.02);
        CHECK (std::abs (win->videoTimeNow() - 1.02) < 1e-9, "video window: hover ignored when off");
        hover (-1.0);
        win->menuItemSelected (MainWindow::WindowVideoFollowHover, wIdx);
        CHECK (win->isVideoFollowingHover(), "video window: hover toggle back on");

        // Window > Video hides and shows the window.
        win->menuItemSelected (MainWindow::WindowVideo, wIdx);
        CHECK (!vw->isVisible(), "video window: Window > Video hides it");
        win->menuItemSelected (MainWindow::WindowVideo, wIdx);
        CHECK (vw->isVisible(), "video window: Window > Video shows it again");

        // Docked into the left panel like Tables; undocked back to its own window.
        CHECK (!win->isPanelOpen ("video"), "video dock: tab closed by default");
        win->setVideoDocked (true);
        CHECK (win->isPanelOpen ("video") && !vw->isVisible(),
               "video dock: in the panel, standalone window hidden");
        CHECK (vw->view().getParentComponent() == win->getVideoDockHost(),
               "video dock: the view lives in the host component");
        win->setVideoDocked (false);
        CHECK (!win->isPanelOpen ("video") && vw->isVisible(),
               "video dock: undocked back to its own window");

        // Opening the Video tab from the Window menu docks it too; closing
        // the tab brings the picture back to its own window.
        win->setPanelVisible ("video", true);
        CHECK (win->isPanelOpen ("video") && !vw->isVisible()
               && vw->view().getParentComponent() == win->getVideoDockHost(),
               "video dock: opening the tab docks the picture");
        win->setPanelVisible ("video", false);
        CHECK (!win->isPanelOpen ("video") && vw->isVisible(),
               "video dock: closing the tab brings back its own window");
    }

    // An edit that changes the length raises the mismatch notice.
    vw->setAudioDuration (10.0);
    CHECK (vw->view().getNotice().isNotEmpty(), "video window: length mismatch flagged");
    vw->setAudioDuration (2.0);
    CHECK (vw->view().getNotice().isEmpty(), "video window: notice cleared when lengths agree");

    // Per-track picture adjustments survive a document switch: they are
    // stored with the video track in the document's metadata.
    vw->view().rotateClockwise();
    vw->view().zoomIn();
    CHECK (vw->view().getUserRotation() == 90, "video window: rotated via the context-menu action");

    // Switching to the wav: the window stays and says there is no video.
    app.activate (0);
    CHECK (vw->getVideoFile() == juce::File(), "video window: wav active -> no video file");
    CHECK (vw->view().getMessage().contains ("No video"), "video window: says the document has no video");
    app.activate (1);
    CHECK (vw->getVideoFile() == f, "video window: back to the movie");
    CHECK (vw->view().getUserRotation() == 90, "video window: the movie keeps its rotation");
    CHECK (std::abs (vw->view().getZoom() - VideoView::kZoomStep) < 1e-6f,
           "video window: the track's zoom is restored with it");

    // Closed by the user: switching documents does not bring it back.
    vw->closeButtonPressed();
    CHECK (!vw->isVisible(), "video window: close button hides it");
    app.activate (0);
    app.activate (1);
    scPumpUntil ([&] { return pump.isOpen(); }, 5000);
    scPumpUntil ([] { return false; }, 100);
    CHECK (!vw->isVisible(), "video window: stays closed for a movie the user closed it on");

    win.reset();
    store.deleteFile();
}

// The tracks panel: ruler + one coloured band per video, drag to offset,
// wheel zoom about the cursor, click to select, context-menu ids.
static void testVideoTracksPanelHeadless()
{
    VideoTracksPanel p;
    p.setSize (300, 20 + 4 * 26);   // ruler plus four rows

    // The view fits the audio length.
    p.setAudioDuration (10.0);
    const auto r0 = p.getViewRange();
    CHECK (std::abs (r0.getStart()) < 1e-9 && std::abs (r0.getEnd() - 10.0) < 1e-9,
           "tracks panel: the view fits the audio length");

    // Two tracks; the fit covers both the audio and the second track's end.
    VideoTracksPanel::Track a, b;
    a.file = juce::File ("/tmp/a.mp4"); a.offsetSecs = 0.0; a.durationSecs = 5.0;
    b.file = juce::File ("/tmp/b.avi"); b.offsetSecs = 7.0; b.durationSecs = 3.0;
    p.setTracks ({ a, b });
    CHECK (std::abs (p.getViewRange().getEnd() - 10.0) < 1e-9,
           "tracks panel: the fit covers audio and tracks");

    // Hit-testing: inside each band, on the ruler, in the gap between them.
    const int xa = (int) p.xForTime (2.5);   // inside A [0, 5)
    const int xb = (int) p.xForTime (8.0);   // inside B [7, 10)
    const int xg = (int) p.xForTime (6.0);   // gap: neither band
    CHECK (p.trackIndexAt ({ xa, 20 + 13 }) == 0, "tracks panel: hit in the first band");
    CHECK (p.trackIndexAt ({ xb, 20 + 26 + 13 }) == 1, "tracks panel: hit in the second band");
    CHECK (p.trackIndexAt ({ xa, 10 }) == -1, "tracks panel: the ruler is not a track");
    CHECK (p.trackIndexAt ({ xg, 20 + 13 }) == -1, "tracks panel: the gap between bands is empty");

    // Clicking selects; clicking the ruler clears.
    int sel = -99;
    p.onSelectTrack = [&] (int i) { sel = i; };
    p.mouseDown (mkMouse (p, xa, 20 + 13, {}));
    CHECK (sel == 0 && p.getSelected() == 0, "tracks panel: a click selects the track");
    p.mouseDown (mkMouse (p, xa, 10, {}));
    CHECK (sel == -1 && p.getSelected() == -1, "tracks panel: a ruler click clears it");

    // Dragging a band moves its offset by the dragged time.
    double off = -99; int offIdx = -99;
    p.onOffsetChanged = [&] (int i, double o) { offIdx = i; off = o; };
    const juce::ModifierKeys lmb (juce::ModifierKeys::leftButtonModifier);
    const int xEnd = (int) p.xForTime (4.5);
    p.mouseDown (mkMouse (p, xa, 20 + 13, lmb));
    p.mouseDrag (mkMouse (p, xEnd, 20 + 13, lmb));
    p.mouseUp   (mkMouse (p, xEnd, 20 + 13, lmb));
    CHECK (offIdx == 0 && std::abs (off - 2.0) < 0.05, "tracks panel: dragging moves the offset");

    // The wheel zooms about the cursor; the time under it stays put.
    {
        juce::MouseWheelDetails wd;
        wd.deltaY = 1.0f;
        const double tBefore = p.timeAtX ((float) xa);
        p.mouseWheelMove (mkMouse (p, xa, 20 + 13, {}), wd);
        CHECK (p.getViewRange().getLength() < 10.0 - 1e-6, "tracks panel: wheel up zooms in");
        CHECK (std::abs (p.timeAtX ((float) xa) - tBefore) < 1e-6,
               "tracks panel: the cursor time stays under the cursor");
    }

    // Context-menu ids: zoom items run locally; the rest go to the owner.
    int menuId = -99, menuIdx = -99;
    p.onMenuItem = [&] (int id, int i) { menuId = id; menuIdx = i; };
    p.handleContextMenuId (VideoTracksPanel::kMenuZoomFit, 1);
    CHECK (std::abs (p.getViewRange().getEnd() - 10.0) < 1e-9 && menuId == -99,
           "tracks panel: zoom fit runs locally");
    p.handleContextMenuId (VideoTracksPanel::kMenuSyncAudio, 1);
    CHECK (menuId == VideoTracksPanel::kMenuSyncAudio && menuIdx == 1,
           "tracks panel: sync goes to the owner with its track");
    p.handleContextMenuId (VideoTracksPanel::kMenuSolo, -1);
    CHECK (menuId == VideoTracksPanel::kMenuSolo && menuIdx == -1,
           "tracks panel: ruler items carry index -1");
}

// End to end: two tracks of the same movie on one document. The selected
// track renders above while it covers the time; when it ends, the topmost
// covering track takes over automatically.
static void testVideoWindowMultiTrack()
{
    if (!sc::videoDecodingAvailable())
    {   scSkip ("multi-track: no picture decoder on this platform"); return; }
    const juce::File f = videoFixtureOrSkip();
    if (!f.existsAsFile()) return;

    const juce::File store = makeTempFile ("video_multitrack.recent");
    store.deleteFile();
    Application app (store);
    std::unique_ptr<MainWindow> win (new MainWindow (app, "video multitrack test"));

    auto cleanup = [&] { win.reset(); store.deleteFile(); };

    const juce::File wav = makeTempFile ("video_multitrack.wav");
    writeWav (wav, 1, 48000.0, 48000, [] (int, int64_t, double) { return 0.0f; });
    CHECK (app.openFile (wav), "multi-track: the wav opens");

    if (!app.openFile (f))
    {   scSkip ("multi-track: the .mp4 audio cannot be opened in this build"); cleanup(); return; }
    auto* vw = win->getVideoWindow();
    CHECK (vw != nullptr, "multi-track: window created for the movie");
    if (vw == nullptr) { cleanup(); return; }

    auto& pump = vw->pump();
    CHECK (scPumpUntil ([&] { return pump.isOpen() || !pump.error().isEmpty(); }, 10000),
           "multi-track: the movie opened");
    if (!pump.isOpen())
    {   scSkip ("multi-track: picture cannot be decoded here: " + pump.error()); cleanup(); return; }

    // The auto-detected movie is one implicit track at offset 0.
    CHECK (vw->displayedTrack() == 0, "multi-track: the implicit track is displayed");

    // Materialise two tracks of the same movie: A @ 0 s [0,2), B @ -0.5 s [-0.5,1.5).
    {
        FileDoc* d = app.doc();
        sc::VideoTrackData a, b;
        a.file = f; a.offsetSecs = 0.0;
        b.file = f; b.offsetSecs = -0.5;
        d->setVideoTracks ({ a, b });
    }

    // Select B: it renders above A while both cover the time.
    app.doc()->selectedVideoTrack = 1;
    win->syncVideoWindow();
    CHECK (vw->displayedTrack() == 1, "multi-track: the selected track wins");

    auto shownIndex = [&] { return videoFrameIndexOf (pump.frame().image); };
    app.setPlayhead (1.2);
    vw->pollTime();
    pump.waitIdle (10000);
    CHECK (scPumpUntil ([&] { return shownIndex() == 42; }, 5000),
           "multi-track: the selected track's offset is applied (frame 42 at local time 1.7 s)");

    // B ends at 1.5 s while A still covers: the topmost covering track takes over.
    app.setPlayhead (1.8);
    CHECK (scPumpUntil ([&] { return vw->displayedTrack() == 0; }, 2000),
           "multi-track: auto-switch when the selected track ends");
    pump.waitIdle (10000);
    CHECK (scPumpUntil ([&] { return shownIndex() == 45; }, 5000),
           "multi-track: the fallback track shows its own frame (frame 45 at local time 1.8 s)");

    // Per-track adjustments persist into the document through the window.
    vw->view().zoomIn();   // fires onAdjustmentChanged -> saveDisplayedAdjustments
    CHECK (std::abs (app.doc()->videoTracks[0].zoom - VideoView::kZoomStep) < 1e-6f,
           "multi-track: zoom is persisted to the displayed track's metadata");
    CHECK (app.doc()->metaDirty, "multi-track: the document is marked dirty by the change");

    // Dropping another video file on the window adds it as a new track.
    const int before = (int) app.doc()->videoTracks.size();
    vw->filesDropped (juce::StringArray ({ f.getFullPathName() }), 0, 0);
    CHECK (app.doc()->videoTracks.size() == before + 1
               && app.doc()->selectedVideoTrack == before,
           "multi-track: dropping a video file adds and selects a new track");

    cleanup();
}

static void runRegression_video()
{
    testVideoDisplayTimeRule();
    testVideoDecoderFrames();
    testVideoDecodeSizeRule();
    testVideoDecoder4k();
    testVideoFramePump();
    testVideoMovieAudio();
    testVideoViewLayout();
    testVideoWindowFollowsDocument();
    testVideoTracksPanelHeadless();
    testVideoWindowMultiTrack();
}
