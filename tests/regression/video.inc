// Video window: picture decoding (VideoFrames), the background frame pump,
// movie audio on macOS (AVFoundation) and the MainWindow wiring (the
// picture follows playback and the playhead).
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
           && std::abs (r.getCentreY() - 143.0f) < 0.5f, "video view: 3:2 frame letterboxed in the plot area");
    v.setFrame (fr, 90);
    r = v.pictureBounds();
    CHECK (std::abs (r.getWidth() - 190.67f) < 0.5f && std::abs (r.getHeight() - 286.0f) < 0.5f,
           "video view: rotated 90 degrees -> portrait area");

    v.setFrame (fr, 0);

    // Rotation and zoom are driven by methods (context menu, wheel, keys).
    int notified = -1;
    v.onUserRotationChanged = [&] (int deg) { notified = deg; };
    v.rotateClockwise();
    r = v.pictureBounds();
    CHECK (v.getUserRotation() == 90 && notified == 90, "video view: rotate clockwise turns by 90 degrees");
    CHECK (std::abs (r.getWidth() - 190.67f) < 0.5f && std::abs (r.getHeight() - 286.0f) < 0.5f,
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
    // move 1001 px sideways and ~620 px vertically in a 286x286 plot area.
    v.setPan ({ 5000.0f, -5000.0f });
    CHECK (std::abs (v.getPan().x - 1001.0f) < 0.5f && std::abs (v.getPan().y + 619.7f) < 0.5f,
           "video view: pan clamped to the picture edges");
    r = v.pictureBounds();
    CHECK (std::abs (r.getX()) < 0.5f && std::abs (r.getBottom() - 286.0f) < 0.5f,
           "video view: clamped pan puts the picture edge on the view edge");
    for (int i = 0; i < 40; ++i) v.zoomOut();
    CHECK (v.getZoom() == 1.0f && v.getPan().isOrigin(), "video view: back at fit, no pan left");
    v.resetZoom();
    CHECK (v.getZoom() == 1.0f, "video view: double-click / reset fits the picture again");

    // Mouse wheel zooms about the cursor position.
    {
        juce::MouseWheelDetails wd;
        wd.deltaY = 1.0f;
        v.mouseWheelMove (mkMouse (v, 143, 143, {}), wd);
        CHECK (std::abs (v.getZoom() - VideoView::kZoomStep) < 1e-6f && v.getPan().isOrigin(),
               "video view: wheel up at the centre zooms in");
        // Off-centre: the point under the cursor stays put (horizontal axis;
        // at x1.25 the picture is still shorter than the plot area vertically).
        v.resetZoom();
        v.mouseWheelMove (mkMouse (v, 100, 143, {}), wd);
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
        // The dragged region's centre (130, 92.33 in fitted coords) now sits
        // at the plot-area centre (143, 143).
        CHECK (std::abs ((P.getX() + 130.0f * z) - 143.0f) < 1.0f
               && std::abs ((P.getY() + 92.333f * z) - 143.0f) < 1.0f,
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
        const double Fh = 190.667;   // fitted height of the 96x64 frame in a 286x286 plot area
        CHECK (std::abs ((double) bar->thumbRect().getY()) < 1.0
               && std::abs ((double) bar->thumbRect().getBottom() - bar->getHeight()) < 1.0,
               "video view: at fit the thumb fills the track");

        const double lo = 0.25 * Fh, hi = 0.75 * Fh;
        bar->applyWindow (lo, hi);
        CHECK (std::abs (v.getZoom() - 286.0 / (hi - lo)) < 1e-3f,
               "video view: the bar window sets the zoom");
        // The visible fitted range is [lo, hi]: with this geometry that puts
        // pan.y at zero (the picture's centre stays on screen).
        CHECK (std::abs (v.getPan().y) < 0.5f,
               "video view: the bar window centres its midpoint");

        const double dx = 10.0;
        const float pyBefore = v.getPan().y;
        bar->applyWindow (lo + dx, hi + dx);
        CHECK (std::abs (v.getZoom() - 286.0 / (hi - lo)) < 1e-3f
               && std::abs (v.getPan().y - (pyBefore - dx * v.getZoom())) < 0.5f,
               "video view: dragging the thumb pans without changing the zoom");
    }

    // The bottom bar mirrors the vertical one on the horizontal axis.
    {
        v.resetZoom();
        auto* hbar = v.getHZoomBar();
        CHECK (hbar->isVisible(), "video view: the horizontal bar is shown with a picture");
        const double Fw = 286.0;   // at fit the frame width fills the plot area
        CHECK (std::abs ((double) hbar->thumbRect().getX()) < 1.0
               && std::abs ((double) hbar->thumbRect().getRight() - hbar->getWidth()) < 1.0,
               "video view: at fit the horizontal thumb fills the track");

        const double lo = 0.25 * Fw, hi = 0.75 * Fw;
        hbar->applyWindow (lo, hi);
        CHECK (std::abs (v.getZoom() - 286.0 / (hi - lo)) < 1e-3f,
               "video view: the horizontal bar window sets the zoom");
        CHECK (std::abs (v.getPan().x) < 0.5f && std::abs (v.getPan().y) < 0.5f,
               "video view: the horizontal bar centres its midpoint and leaves pan.y alone");

        const double dx = 10.0;
        const float pxBefore = v.getPan().x;
        hbar->applyWindow (lo + dx, hi + dx);
        CHECK (std::abs (v.getZoom() - 286.0 / (hi - lo)) < 1e-3f
               && std::abs (v.getPan().x - (pxBefore - dx * v.getZoom())) < 0.5f,
               "video view: dragging the horizontal thumb pans without changing the zoom");
        v.resetZoom();
    }

    // Context-menu actions run through one handler (the popup and the tests).
    {
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
    // New users get the Video tab in the top panel group, so a new movie
    // shows up docked there instead of in a standalone window.
    CHECK (win->isPanelOpen ("video") && !vw->isVisible()
           && vw->view().getParentComponent() == win->getVideoDockHost(),
           "video window: shown automatically in the default Video tab");
    CHECK (vw->view().getNotice().isEmpty(), "video window: audio and video lengths agree");

    auto shownIndex = [&] { return videoFrameIndexOf (pump.frame().image); };
    auto showAt = [&] (int expected)
    {
        vw->pollTime();
        pump.waitIdle (10000);
        return scPumpUntil ([&] { return shownIndex() == expected; }, 5000);
    };

    // The test window is on screen: a real mouse resting over it would feed
    // hover times. Keep the MainWindow's hover callback and detach the views
    // from the real mouse while this runs.
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

    {
        const juce::StringArray names = win->getMenuBarNames();
        const int wIdx = names.indexOf ("Window");

        // While docked in its tab (the new-user default), Window > Video
        // toggles it like any other panel: closing hides the picture (no
        // standalone window pops up), opening docks it again.
        win->menuItemSelected (MainWindow::WindowVideo, wIdx);
        CHECK (!win->isPanelOpen ("video") && !vw->isVisible(),
               "video window: Window > Video hides the docked picture");
        win->menuItemSelected (MainWindow::WindowVideo, wIdx);
        CHECK (win->isPanelOpen ("video") && !vw->isVisible(),
               "video window: Window > Video opens the Video panel again");

        // Docked into the left panel like Tables; undocked back to its own window.
        win->setVideoDocked (true);
        CHECK (win->isPanelOpen ("video") && !vw->isVisible(),
               "video dock: in the panel, standalone window hidden");
        auto* host = win->getVideoDockHost();
        CHECK (vw->view().getParentComponent() == host,
               "video dock: the view lives in the host component");
        const int hw = host->getWidth(), hh = host->getHeight();
        auto& list = vw->getTracksPanel();
        CHECK (list.getParentComponent() == host && list.isVisible()
                   && list.getBounds().getY() + list.getHeight() == hh
                   && list.getWidth() == hw,
               "video dock: the track list sits at the bottom of the panel");
        auto* sep = host->getChildComponent (2);
        CHECK (sep != nullptr && sep->isVisible()
                   && sep->getBounds() == juce::Rectangle<int> (0, list.getY() - VideoContentHost::kSplitH,
                                                               hw, VideoContentHost::kSplitH),
               "video dock: the separator sits above the track list");
        CHECK (vw->view().isVisible()
                   && vw->view().getBounds() == juce::Rectangle<int> (0, 0, hw, sep != nullptr ? sep->getY() : hh),
               "video dock: the picture fills the rest of the panel");
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

        // Issue #10: Window > Video sits with the other panels, right after
        // Drawing (no separator), and there is no Dock / Undock item any
        // more: the Video panel floats by dragging its tab out, like every
        // other panel.
        auto windowItems = [&] ()
        {
            std::vector<juce::PopupMenu::Item> items;
            const auto wm = win->getMenuForIndex (wIdx, "Window");
            for (juce::PopupMenu::MenuItemIterator it (wm, false); it.next ();)
                items.push_back (it.getItem());
            return items;
        };
        auto videoItem = [&] () -> juce::PopupMenu::Item
        {
            for (const auto& it : windowItems ())
                if (it.itemID == MainWindow::WindowVideo) return it;
            return {};
        };
        {
            const auto items = windowItems ();
            bool afterDrawing = false, dockItem = false;
            for (size_t i = 0; i + 1 < items.size (); ++i)
                if (items[i].text == "Drawing")
                    afterDrawing = items[i + 1].itemID == MainWindow::WindowVideo
                                   && items[i + 1].text == "Video";
            for (const auto& it : items)
                dockItem = dockItem || it.text.containsIgnoreCase ("Dock");
            CHECK (afterDrawing, "video menu: Video follows Drawing in the panel list");
            CHECK (! dockItem, "video menu: no Dock / Undock Video Panel item");
        }

        // Standalone window shown: the item is checked and hides it.
        CHECK (vw->isVisible () && videoItem ().isTicked,
               "video menu: checked while the picture shows in its own window");
        win->menuItemSelected (MainWindow::WindowVideo, wIdx);
        CHECK (! vw->isVisible () && ! win->isPanelOpen ("video") && ! videoItem ().isTicked,
               "video menu: unchecking hides the standalone picture");
        // Checking it opens the Video panel with the picture docked in it.
        win->menuItemSelected (MainWindow::WindowVideo, wIdx);
        CHECK (win->isPanelOpen ("video") && ! vw->isVisible ()
                   && vw->view().getParentComponent () == win->getVideoDockHost()
                   && videoItem ().isTicked,
               "video menu: checking opens the Video panel");
        // Unchecking closes the panel without popping the standalone window.
        win->menuItemSelected (MainWindow::WindowVideo, wIdx);
        CHECK (! win->isPanelOpen ("video") && ! vw->isVisible () && ! win->isVideoDocked ()
                   && ! videoItem ().isTicked,
               "video menu: unchecking closes the panel, no window pops up");
        win->setVideoDocked (false);   // restore the standalone state
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

    // Clicking selects; clicking the time ruler moves the main playhead;
    // empty space below the tracks still clears the selection.
    int sel = -99; double seekT = -1.0;
    p.onSelectTrack = [&] (int i) { sel = i; };
    p.onSeekTime = [&] (double t) { seekT = t; };
    p.mouseDown (mkMouse (p, xa, 20 + 13, {}));
    CHECK (sel == 0 && p.getSelected() == 0, "tracks panel: a click selects the track");
    p.mouseDown (mkMouse (p, xa, 10, {}));
    CHECK (std::abs (seekT - p.timeAtX ((float) xa)) < 1e-9 && sel == 0,
           "tracks panel: a ruler click moves the playhead");

    // Dragging on the ruler - the panel's time axis - selects that time range,
    // like the waveform's ruler and the dedicated track's axis. A plain click
    // still only seeks, and neither gesture moves the view.
    double selA = 1e9, selB = -1.0;
    p.onSelectTimeRange = [&] (const juce::Range<double>& r) { selA = r.getStart(); selB = r.getEnd(); };
    const auto rBefore = p.getViewRange();
    const int xr1 = (int) p.xForTime (3.0), xr2 = (int) p.xForTime (6.0);
    seekT = -1.0;
    p.mouseDown (mkMouse (p, xr1, 10, {}));
    p.mouseUp   (mkMouse (p, xr1, 10, {}));
    CHECK (selA > 1e8, "tracks panel: a ruler click does not select a range");
    p.mouseDown (mkMouse (p, xr1, 10, {}));
    p.mouseDrag (mkMouse (p, xr2, 10, {}));
    p.mouseUp   (mkMouse (p, xr2, 10, {}));
    CHECK (std::abs (selA - 3.0) < 1e-6 && std::abs (selB - 6.0) < 1e-6,
           "tracks panel: dragging the ruler selects the dragged time range");
    const auto rAfter = p.getViewRange();
    CHECK (std::abs (rAfter.getStart() - rBefore.getStart()) < 1e-9
        && std::abs (rAfter.getEnd() - rBefore.getEnd()) < 1e-9,
           "tracks panel: selecting on the ruler does not move the view");
    p.mouseDown (mkMouse (p, xg, 20 + 3 * 26 + 5, {}));   // empty row below the two bands
    CHECK (sel == -1 && p.getSelected() == -1, "tracks panel: empty space still clears it");

    // Dragging a band moves its offset by the dragged time.
    double off = -99; int offIdx = -99;
    p.onOffsetChanged = [&] (int i, double o) { offIdx = i; off = o; };
    const juce::ModifierKeys lmb (juce::ModifierKeys::leftButtonModifier);
    const int xEnd = (int) p.xForTime (4.5);
    p.mouseDown (mkMouse (p, xa, 20 + 13, lmb));
    p.mouseDrag (mkMouse (p, xEnd, 20 + 13, lmb));
    p.mouseUp   (mkMouse (p, xEnd, 20 + 13, lmb));
    CHECK (offIdx == 0 && std::abs (off - 2.0) < 0.05, "tracks panel: dragging moves the offset");

    // Shift-drag snaps to the audio start/end and other tracks' edges;
    // plain dragging keeps its raw offset.
    {
        VideoTracksPanel::Track c, d;
        c.file = juce::File ("/tmp/c.mp4"); c.offsetSecs = 0.0; c.durationSecs = 5.0;
        d.file = juce::File ("/tmp/d.avi"); d.offsetSecs = 7.0; d.durationSecs = 3.0;
        p.setTracks ({ c, d });   // refits to [0, 10]

        const juce::ModifierKeys shiftLmb (juce::ModifierKeys::leftButtonModifier
                                           | juce::ModifierKeys::shiftModifier);
        double off2 = -99; int offIdx2 = -99;
        p.onOffsetChanged = [&] (int i, double o) { offIdx2 = i; off2 = o; };

        const int xD = (int) p.xForTime (8.5);   // inside D [7, 10)
        // Raw offset lands ~0.2 s short of C's end at 5.0: within the snap range.
        p.mouseDown (mkMouse (p, xD, 20 + 26 + 13, shiftLmb));
        p.mouseDrag (mkMouse (p, (int) p.xForTime (6.7), 20 + 26 + 13, shiftLmb));
        CHECK (offIdx2 == 1 && std::abs (off2 - 5.0) < 1e-9,
               "tracks panel: shift-drag snaps to another track's end");

        // D now sits at [5, 8): grab it there; the raw offset lands ~0.1 s
        // before the start of the audio: within the snap range for 0.
        const int xD3 = (int) p.xForTime (6.5);
        p.mouseDown (mkMouse (p, xD3, 20 + 26 + 13, shiftLmb));
        p.mouseDrag (mkMouse (p, (int) p.xForTime (1.4), 20 + 26 + 13, shiftLmb));
        CHECK (std::abs (off2 - 0.0) < 1e-9, "tracks panel: shift-drag snaps to the start");

        // Without Shift the same drag keeps its raw offset (~5.2 s).
        const int xD2 = (int) p.xForTime (1.5);   // D now sits at [0, 3)
        p.mouseDown (mkMouse (p, xD2, 20 + 26 + 13, lmb));
        p.mouseDrag (mkMouse (p, (int) p.xForTime (6.7), 20 + 26 + 13, lmb));
        CHECK (std::abs (off2 - 5.2) < 0.05 && std::abs (off2 - 5.0) > 1e-9,
               "tracks panel: plain dragging does not snap");
    }

    // The main playhead is drawn as a red line from the ruler to the bottom.
    p.setPlayhead (1.25);
    CHECK (std::abs (p.getPlayhead() - 1.25) < 1e-9, "tracks panel: the playhead is stored");
    {
        juce::Image img (juce::Image::RGB, 300, 20 + 4 * 26, true);
        juce::Graphics g (img);
        p.paint (g);
        const int x = (int) p.xForTime (1.25);
        auto px = [&] (int xx, int yy) -> std::pair<int, int>   // (R, G); bytes are B,G,R[,A]
        {
            juce::Image::BitmapData bd (img, juce::Image::BitmapData::readWrite);
            const uint8_t* q = bd.getPixelPointer (xx, yy);
            return { (int) q[2], (int) q[1] };
        };
        CHECK (px (x, 5).first > 200 && px (x, 5).second < 80,
               "tracks panel: the playhead line is red in the ruler");
        CHECK (px (x, 100).first > 200 && px (x, 100).second < 80,
               "tracks panel: the playhead line reaches the bottom of the panel");
    }

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

// The shared time selection is drawn as a translucent blue band across the
// ruler and every row, the same style the waveform and spectral views use.
static void testVideoTracksSelectionBand()
{
    VideoTracksPanel p;
    p.setSize (300, p.kRulerH + 2 * p.kRowH);
    p.setAudioDuration (10.0);

    VideoTracksPanel::Track a;
    a.file = juce::File ("/tmp/a.mp4"); a.offsetSecs = 0.0; a.durationSecs = 5.0;
    p.setTracks ({ a });

    const int h = p.getHeight();
    juce::Image base (juce::Image::ARGB, 300, h, true);
    { juce::Graphics g0 (base); p.paint (g0); }   // no selection yet

    p.setSelection ({ 2.0, 4.0 });
    CHECK (p.getSelection() == juce::Range<double> (2.0, 4.0),
           "tracks panel: the selection is remembered");

    juce::Image img (juce::Image::ARGB, 300, h, true);
    { juce::Graphics g (img); p.paint (g); }

    // The fill is RGBA(70,130,255,80): it pulls the pixel towards blue. The
    // comparison is against the identical no-selection render, so whatever the
    // ruler or a band paints underneath does not matter.
    auto blueShift = [] (const juce::Colour& px, const juce::Colour& under)
    { return (px.getBlue() - px.getRed()) - (under.getBlue() - under.getRed()) > 40; };

    const int xIn  = (int) p.xForTime (3.0);   // inside [2, 4] s
    const int xOut = (int) p.xForTime (6.0);   // outside it
    // Row 2 is empty (one track only): the fill lands on the dark background.
    // Row 1 holds track A, whose colour is the very same blue as the fill, so
    // blending it changes nothing measurable - the ruler proves that row too.
    const int yRow = p.kRulerH + p.kRowH + 13;
    CHECK (blueShift (img.getPixelAt (xIn, 5), base.getPixelAt (xIn, 5)),
           "tracks panel: the ruler is tinted inside the selection");
    CHECK (blueShift (img.getPixelAt (xIn, yRow), base.getPixelAt (xIn, yRow)),
           "tracks panel: the rows are tinted inside the selection");
    CHECK (img.getPixelAt (xOut, yRow) == base.getPixelAt (xOut, yRow),
           "tracks panel: nothing changes outside the selection");

    // Clearing removes the band: the render is identical to the baseline again.
    p.setSelection ({});
    CHECK (p.getSelection().isEmpty(), "tracks panel: an empty range clears the selection");
    juce::Image cleared (juce::Image::ARGB, 300, h, true);
    { juce::Graphics g2 (cleared); p.paint (g2); }
    CHECK (cleared.getPixelAt (xIn, yRow) == base.getPixelAt (xIn, yRow),
           "tracks panel: the band is gone after clearing");
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

    // No selection: the topmost covering track wins (index 0 is drawn first).
    app.doc()->selectedVideoTrack = -1;
    win->syncVideoWindow();
    app.setPlayhead (1.2);
    CHECK (scPumpUntil ([&] { return vw->displayedTrack() == 0; }, 2000),
           "multi-track: without a selection the topmost covering track wins");

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

//==============================================================================
// The user's flow: the Video tab is docked in the left panel while only an
// audio document is open, then a movie is opened afterwards. Frames must
// reach the picture living inside the panel.
static void testDockedVideoReceivesFrames()
{
    if (!sc::videoDecodingAvailable())
    {   scSkip ("docked frames: no picture decoder on this platform"); return; }
    const juce::File f = videoFixtureOrSkip();
    if (!f.existsAsFile()) return;

    const juce::File store = makeTempFile ("video_dock_frames.recent");
    store.deleteFile();
    Application app (store);
    std::unique_ptr<MainWindow> win (new MainWindow (app, "docked frames test"));
    auto cleanup = [&] { win.reset(); store.deleteFile(); };

    const juce::File wav = makeTempFile ("video_dock_frames.wav");
    writeWav (wav, 1, 48000.0, 48000, [] (int, int64_t, double) { return 0.0f; });
    CHECK (app.openFile (wav), "docked frames: the wav opens");

    // Dock the Video tab while no video is present yet.
    win->setVideoDocked (true);
    auto* vw = win->getVideoWindow();
    CHECK (vw != nullptr && vw->isDocked(), "docked frames: docked with no video");
    if (vw == nullptr) { cleanup(); return; }
    auto* host = win->getVideoDockHost();
    CHECK (vw->view().getParentComponent() == host,
           "docked frames: the picture lives in the panel host");
    const int hw = host->getWidth(), hh = host->getHeight();
    auto& list = vw->getTracksPanel();
    CHECK (list.getParentComponent() == host && list.isVisible()
               && list.getBounds().getY() + list.getHeight() == hh
               && list.getWidth() == hw,
           "docked frames: the track list is in the panel too");

    // Open the movie afterwards, like a drag & drop onto the main window.
    if (!app.openFile (f))
    {   scSkip ("docked frames: the .mp4 audio cannot be opened in this build"); cleanup(); return; }

    CHECK (scPumpUntil ([&] { return vw->view().image() != nullptr && vw->view().image()->isValid(); }, 10000),
           "docked frames: a frame reaches the docked picture");
    CHECK (vw->view().isVisible(), "docked frames: the picture is actually shown");
    CHECK (vw->displayedTrack() == 0, "docked frames: the implicit track is displayed");

    cleanup();
}

// ---------------------------------------------------------------------------
// "Sync based on Audio": the two-stage correlation math (platform-independent).

static void testVideoSyncMath()
{
    // decimateTo: length and averaging.
    {
        std::vector<float> in;
        const int n = 48000;   // 1 s @ 48 kHz, constant 0.5
        in.assign ((size_t) n, 0.5f);
        auto out = sc::decimateTo (in, 48000.0, 2000.0);
        CHECK (std::abs ((double) out.size () - 2000) <= 1, "sync math: decimation length");
        bool allHalf = true;
        for (float v : out)
            if (v != 0.5f) { allHalf = false; break; }
        CHECK (allHalf, "sync math: box averaging preserves constants");

        auto same = sc::decimateTo (in, 48000.0, 96000.0);
        CHECK (same.size () == in.size (), "sync math: no decimation when outRate >= inRate");
    }

    // LCG noise: deterministic and non-periodic, so NCC peaks only at the true lag.
    auto makeNoise = [] (int64_t n, uint32_t seed)
    {
        std::vector<float> v ((size_t) n);
        uint32_t s = seed;
        for (auto& x : v)
        {
            s = s * 1664525u + 1013904223u;
            x = (float) ((int32_t) (s >> 8)) / 2147483648.0f * 0.5f;
        }
        return v;
    };

    const double rate = 48000.0;
    // doc: 8 s of noise; vid: an exact copy of doc[2.5 .. 5.5]. The true lag
    // sits on the coarse grid (step >= 0.5 s), where white noise correlates.
    {
        auto doc = makeNoise ((int64_t) (8 * rate), 12345u);
        const int64_t off = (int64_t) std::llround (2.5 * rate);
        std::vector<float> vid (doc.begin () + off, doc.begin () + off + (int64_t) (3 * rate));

        auto docDs = sc::decimateTo (doc, rate, 2000.0);
        auto vidDs = sc::decimateTo (vid, rate, 2000.0);
        double step = 0.0;
        const double coarse = sc::videoSyncCoarse (docDs, vidDs, &step);
        CHECK (!std::isnan (coarse) && step > 0.0, "sync math: coarse finds the lag");
        CHECK (std::abs (coarse - 2.5) <= step + 1e-9, "sync math: coarse within one grid step");

        // Fine stage with exactly the windows the handler builds.
        const double sA = std::max (0.0, coarse - step);
        const double eB = std::min (8.0, coarse + 5.0 + step);
        std::vector<float> docWin (doc.begin () + (int64_t) std::llround (sA * rate),
                                   doc.begin () + (int64_t) std::llround (eB * rate));
        const double vEnd = std::min (3.0, 5.0 + step);
        std::vector<float> vidWin (vid.begin (), vid.begin () + (int64_t) std::llround (vEnd * rate));
        const double fine = sc::videoSyncFine (docWin, rate, sA, vidWin, rate, coarse, step);
        CHECK (!std::isnan (fine), "sync math: fine stage refines");
        CHECK (std::abs (fine - 2.5) <= 3.0 / rate + 1e-9, "sync math: fine within a few samples");
    }

    // Negative lag: the video starts before the audio.
    {
        auto vid = makeNoise ((int64_t) (4 * rate), 777u);
        const int64_t shift = (int64_t) std::llround (1.0 * rate);   // on the coarse grid
        std::vector<float> doc ((size_t) vid.size (), 0.0f);
        for (int64_t i = 0; i + shift < (int64_t) vid.size (); ++i)
            doc[(size_t) i] = vid[(size_t) (i + shift)];

        auto docDs = sc::decimateTo (doc, rate, 2000.0);
        auto vidDs = sc::decimateTo (vid, rate, 2000.0);
        double step = 0.0;
        const double coarse = sc::videoSyncCoarse (docDs, vidDs, &step);
        CHECK (!std::isnan (coarse) && std::abs (coarse + 1.0) <= step + 1e-9,
               "sync math: negative lag found coarsely");

        const double sA = std::max (0.0, coarse - step);
        const double eB = std::min (4.0, coarse + 5.0 + step);
        std::vector<float> docWin (doc.begin () + (int64_t) std::llround (sA * rate),
                                   doc.begin () + (int64_t) std::llround (eB * rate));
        const double vEnd = std::min (4.0, 5.0 + step);
        std::vector<float> vidWin (vid.begin (), vid.begin () + (int64_t) std::llround (vEnd * rate));
        const double fine = sc::videoSyncFine (docWin, rate, sA, vidWin, rate, coarse, step);
        CHECK (!std::isnan (fine) && std::abs (fine + 1.0) <= 3.0 / rate + 1e-9,
               "sync math: negative lag refined");
    }

    // Independent noise must not correlate.
    {
        auto a = sc::decimateTo (makeNoise ((int64_t) (8 * rate), 1u), rate, 2000.0);
        auto b = sc::decimateTo (makeNoise ((int64_t) (3 * rate), 2u), rate, 2000.0);
        CHECK (std::isnan (sc::videoSyncCoarse (a, b)), "sync math: uncorrelated -> NaN");
    }

    // A silent document carries no alignment information.
    {
        std::vector<float> a ((size_t) (8 * 2000), 0.0f);
        auto b = sc::decimateTo (makeNoise ((int64_t) (3 * rate), 9u), rate, 2000.0);
        CHECK (std::isnan (sc::videoSyncCoarse (a, b)), "sync math: silence -> NaN");
    }
}

// End to end: the track context-menu actions wired through MainWindow.
// (Regression: onTrackMenuAction was declared but never assigned, so every
// one of these items did nothing.)
static void testVideoTrackMenuActions()
{
    if (!sc::videoDecodingAvailable())
    {   scSkip ("track menu: no picture decoder on this platform"); return; }
    const juce::File f = videoFixtureOrSkip();
    if (!f.existsAsFile()) return;
    if (!sc::videoContainerExtensions().contains (".mp4"))
    {   scSkip ("track menu: the .mp4 audio cannot be opened in this build"); return; }

    const juce::File store = makeTempFile ("video_trackmenu.recent");
    store.deleteFile();
    Application app (store);
    std::unique_ptr<MainWindow> win (new MainWindow (app, "video track menu test"));

    auto cleanup = [&] { win.reset (); store.deleteFile (); };

    const juce::File wav = makeTempFile ("video_trackmenu.wav");
    writeWav (wav, 1, 48000.0, 96000, [] (int, int64_t, double) { return 0.0f; });
    CHECK (app.openFile (wav), "track menu: the wav opens");

    // The silent wav stays the document; the movie is only a video track.
    FileDoc* d = app.doc ();
    auto& eng = app.audioEngine ();
    sc::VideoTrackData first;
    first.file = f;
    d->setVideoTracks ({ first }, nullptr);
    win->syncVideoWindow();

    auto* vw = win->getVideoWindow();
    CHECK (vw != nullptr, "track menu: window created for the track");
    if (vw == nullptr) { cleanup (); return; }

    // The root-cause fix itself: MainWindow wires the track-menu callback.
    CHECK (vw->onTrackMenuAction != nullptr, "track menu: onTrackMenuAction is wired");

    auto& p = vw->getTracksPanel();

    // Solo: toggles the flag and installs / removes the engine's solo audio.
    p.handleContextMenuId (VideoTracksPanel::kMenuSolo, 0);
    CHECK (!d->videoTracks.empty () && d->videoTracks[0].solo,
           "track menu: solo toggles the track flag");
    CHECK (eng.hasSoloAudio (), "track menu: solo audio installed for playback");

    p.handleContextMenuId (VideoTracksPanel::kMenuSolo, 0);
    CHECK (!d->videoTracks.empty () && !d->videoTracks[0].solo,
           "track menu: solo toggles off again");
    CHECK (!eng.hasSoloAudio (), "track menu: solo audio removed");

    // Remove track: the list shrinks and no implicit track is resurrected.
    {
        std::vector<sc::VideoTrackData> t;
        sc::VideoTrackData a; a.file = f;
        sc::VideoTrackData b;
        b.file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                     .getChildFile ("video_trackmenu_second.mp4");
        t.push_back (a);
        t.push_back (b);
        d->setVideoTracks (t, nullptr);
        CHECK (d->videoTracks.size () == 2, "track menu: two tracks set");

        p.handleContextMenuId (VideoTracksPanel::kMenuRemoveTrack, 1);
        CHECK (d->videoTracks.size () == 1 && d->videoTracks[0].file == f,
               "track menu: remove drops the second track");
        CHECK (d->videoFile == juce::File (), "track menu: no implicit track resurrected");

        p.handleContextMenuId (VideoTracksPanel::kMenuRemoveTrack, 0);
        CHECK (d->videoTracks.empty () && d->videoFile == juce::File (),
               "track menu: removing the last track leaves none");
    }

    // Copy audio to new channels: decodes the movie's own audio into fresh
    // channels at the track's offset (the fixture is a 1 kHz sine, ~2 s).
    {
        std::vector<sc::VideoTrackData> t;
        sc::VideoTrackData a; a.file = f;
        t.push_back (a);
        d->setVideoTracks (t, nullptr);

        const int oldN = eng.numChannels ();
        p.handleContextMenuId (VideoTracksPanel::kMenuCopyAudio, 0);
        CHECK (scPumpUntil ([&] { return sc::WorkProgressDialog::numActive () == 0; }, 30000),
               "track menu: the copy job finished in time");
        CHECK (eng.numChannels () == oldN + 1, "track menu: one new channel added");

        const int64_t n = eng.numSamples ();
        std::vector<float> mid ((size_t) 48000), tail ((size_t) 48000);
        CHECK (eng.readSamples (1, 24000, 48000, mid.data ()), "track menu: new channel readable");
        double peak = 0.0;
        for (float v : mid) peak = std::max (peak, (double) std::abs (v));
        CHECK (peak > 0.3, "track menu: the movie's audio landed in the new channel");

        if (n > 96000 + 48000)
        {
            CHECK (eng.readSamples (1, n - 48000, 48000, tail.data ()), "track menu: tail readable");
            double tailPeak = 0.0;
            for (float v : tail) tailPeak = std::max (tailPeak, (double) std::abs (v));
            CHECK (tailPeak < 1e-6, "track menu: silence after the movie's audio ends");
        }
    }

    // Sync based on Audio against a silent document: no correlation, so the
    // offset stays put and an alert is raised (swallowed in headless mode).
    {
        sc::alert::clearSuppressedMessages ();
        p.handleContextMenuId (VideoTracksPanel::kMenuSyncAudio, 0);
        CHECK (scPumpUntil ([&] { return sc::WorkProgressDialog::numActive () == 0; }, 30000),
               "track menu: the sync job finished in time");
        const auto tracks = d->videoTracks;
        CHECK (!tracks.empty () && std::abs (tracks[0].offsetSecs) < 1e-9,
               "track menu: no correlation leaves the offset unchanged");
        bool sawAlert = false;
        for (const auto& m : sc::alert::suppressedMessages ())
            if (m.startsWith ("Sync based on Audio:")) { sawAlert = true; break; }
        CHECK (sawAlert, "track menu: the failed sync explains itself");
    }

    cleanup ();
}

// ---------------------------------------------------------------------------
// Picture adjustments must not compound: re-rendering the same frame with the
// same settings gives identical pixels. The old shallow copy adjusted the
// pump's frame in place, so every redraw deepened the effect and reset could
// never recover it. The fixture's first frame is all black; brightness lifts
// it to mid-grey once - a compounding render would lift it again.
static void testVideoAdjustmentNoCompounding()
{
    if (!sc::videoDecodingAvailable())
    {   scSkip ("adjust compounding: no picture decoder on this platform"); return; }
    const juce::File f = videoFixtureOrSkip();
    if (!f.existsAsFile()) return;

    const juce::File store = makeTempFile ("video_adjust.recent");
    store.deleteFile();
    Application app (store);
    std::unique_ptr<MainWindow> win (new MainWindow (app, "adjust compounding test"));
    auto cleanup = [&] { win.reset(); store.deleteFile(); };

    if (!app.openFile (f))
    {   scSkip ("adjust compounding: the .mp4 audio cannot be opened in this build"); cleanup(); return; }
    auto* vw = win->getVideoWindow();
    CHECK (vw != nullptr, "adjust compounding: the window was created");
    if (vw == nullptr) { cleanup(); return; }
    vw->view().setSize (480, 360);   // the headless window's content stays unsized

    CHECK (scPumpUntil ([&] { return vw->view().image() != nullptr && vw->view().image()->isValid(); }, 10000),
           "adjust compounding: a frame reached the view");
    const auto P = vw->view().pictureBounds();
    if (!vw->view().image() || !vw->view().image()->isValid() || P.isEmpty()) { cleanup(); return; }

    // Sample points across the picture (the transport is stopped, so this
    // frame stays put while we re-render it).
    std::vector<std::pair<int, int>> pts;
    for (int i = 1; i < 4; ++i)
        for (int j = 1; j < 4; ++j)
            pts.push_back ({ (int) P.getX() + i * (int) P.getWidth() / 4,
                             (int) P.getY() + j * (int) P.getHeight() / 4 });

    auto sample = [&]
    {
        const juce::Image snap = vw->view().createComponentSnapshot (vw->view().getLocalBounds(), true, 1.0f);
        std::vector<int> v;
        for (const auto& p : pts)
            v.push_back (snap.getPixelAt (p.first, p.second).getRed());
        return v;
    };

    const auto base = sample();
    vw->setAdjustmentsLive (1.0, 0.5);   // brightness +0.5: black -> mid-grey
    const auto once = sample();
    CHECK (once != base, "adjust compounding: the adjustment changes the picture");

    // Re-rendering the same frame with the same settings must not deepen it.
    vw->redrawCurrentFrame ();
    vw->redrawCurrentFrame ();
    CHECK (sample() == once, "adjust compounding: repeated renders keep the same pixels");

    // Reset recovers the original picture exactly.
    vw->setAdjustmentsLive (1.0, 0.0);
    CHECK (sample() == base, "adjust compounding: reset restores the unadjusted frame");

    cleanup();
}

// ---------------------------------------------------------------------------
// The picture/track-list split: a draggable separator keeps its ratio across
// resizes, and the corner button collapses/expands the track list.
static void testVideoHostSplitAndCollapse()
{
    VideoContentHost host;
    VideoView v;
    VideoTracksPanel p;
    TrackSplitter s;
    host.addAndMakeVisible (v);
    host.addAndMakeVisible (p);
    host.addAndMakeVisible (s);
    host.setSize (300, 400);   // triggers the first layout

    CHECK (p.getBounds() == juce::Rectangle<int> (0, 400 - VideoContentHost::kPanelH, 300, VideoContentHost::kPanelH),
           "host split: first layout uses the preferred track height");
    CHECK (s.getBounds() == juce::Rectangle<int> (0, 400 - VideoContentHost::kPanelH - VideoContentHost::kSplitH,
                                                  300, VideoContentHost::kSplitH),
           "host split: the separator sits above the track list");
    CHECK (v.getBounds() == juce::Rectangle<int> (0, 0, 300, 400 - VideoContentHost::kPanelH - VideoContentHost::kSplitH),
           "host split: the picture fills the rest");

    // Dragging the separator down by 50 px shrinks the track list; the
    // picture takes the freed space.
    host.applySplitDelta (50.0);
    CHECK (p.getHeight() == VideoContentHost::kPanelH - 50,
           "host split: dragging down shrinks the tracks");
    CHECK (v.getHeight() == 400 - p.getHeight() - VideoContentHost::kSplitH,
           "host split: the picture takes the freed space");

    // The ratio survives a resize of the host.
    const double share = (double) p.getHeight() / 400.0;
    host.setSize (300, 800);
    CHECK (std::abs ((double) p.getHeight() - std::llround (800.0 * share)) <= 1.0,
           "host split: the ratio is kept across resizes");

    // Collapse hides the tracks and the separator; expand brings them back.
    host.setTracksCollapsed (true);
    CHECK (! p.isVisible() && ! s.isVisible() && host.isTracksCollapsed(),
           "host collapse: tracks and separator hidden");
    CHECK (v.getBounds() == juce::Rectangle<int> (0, 0, 300, 800),
           "host collapse: the picture fills the whole host");
    host.setTracksCollapsed (false);
    CHECK (p.isVisible() && s.isVisible() && ! host.isTracksCollapsed(),
           "host expand: tracks and separator are back");

    // The corner button reports its click to the owner.
    int clicks = 0;
    CollapseToggle c;
    c.onClick = [&] { ++clicks; };
    const juce::ModifierKeys lmb (juce::ModifierKeys::leftButtonModifier);
    c.mouseDown (mkMouse (c, 7, 7, lmb));
    c.mouseUp   (mkMouse (c, 7, 7, lmb));
    CHECK (clicks == 1, "collapse button: a click fires the callback");
}

// ---------------------------------------------------------------------------
// The track list's scrollbars: the time bar always mirrors the view; the row
// bar appears only when the rows overflow and drives the vertical offset.
static void testVideoTracksPanelScrollbars()
{
    VideoTracksPanel p;
    std::vector<VideoTracksPanel::Track> ts;
    for (int i = 0; i < 8; ++i)
    {
        VideoTracksPanel::Track t;
        t.file = juce::File ("/tmp/t" + juce::String (i) + ".mp4");
        t.offsetSecs = i * 1.0;
        t.durationSecs = 2.0;
        ts.push_back (t);
    }

    p.setAudioDuration (10.0);
    p.setSize (300, 124);
    CHECK (!p.getVScroll()->isVisible(), "track scroll: no row bar while the rows fit");

    p.setTracks (ts);   // refits to [0, max(audio, tracks)] = [0, 10]
    const auto vr = p.getViewRange();
    CHECK (std::abs (vr.getStart()) < 1e-9 && std::abs (vr.getEnd() - 10.0) < 1e-9,
           "track scroll: the fit covers all tracks");

    // Eight rows need 208 px but only 92 are available: the row bar appears.
    CHECK (p.getVScroll()->isVisible(), "track scroll: the row bar appears when rows overflow");
    const int avail = p.getHeight() - VideoTracksPanel::kBarW - VideoTracksPanel::kRulerH;
    CHECK (avail == 92, "track scroll: the available row height is as expected");

    // The time bar mirrors the fitted extent and the current view.
    auto* hsb = p.getHScroll();
    CHECK (hsb->isVisible(), "track scroll: the time bar is shown");
    const auto th = hsb->thumbRect();
    CHECK (th.getWidth() > 0 && th.getX() >= 0, "track scroll: the time thumb spans the view");

    // Dragging the row bar down by one row shows the next rows.
    const double rowsTotal = 8 * VideoTracksPanel::kRowH;   // 208 px of rows
    CHECK (std::abs (p.getScrollY()) < 1e-9, "track scroll: starts at the top");
    p.getVScroll()->applyWindow (rowsTotal - (VideoTracksPanel::kRowH + avail), rowsTotal - VideoTracksPanel::kRowH);
    CHECK (std::abs (p.getScrollY() - VideoTracksPanel::kRowH) < 1e-9,
           "track scroll: the bar drives the row offset");

    // Hit-testing follows the scrolled rows.
    const int xa = (int) p.xForTime (1.5);   // inside track 1 [1, 3)
    CHECK (p.trackIndexAt ({ xa, VideoTracksPanel::kRulerH + 13 }) == 1,
           "track scroll: hit-testing accounts for the offset");

    // Fewer rows than fit hide the row bar again and reset the offset.
    p.setTracks ({ ts[0], ts[1] });
    CHECK (!p.getVScroll()->isVisible() && std::abs (p.getScrollY()) < 1e-9,
           "track scroll: the row bar hides again when the rows fit");
}

// ---------------------------------------------------------------------------
// Saving a document opened from a video file must keep the reference to that
// video in the metadata bundle (as one materialised track), so reopening the
// saved .wav brings the picture back.
static void testVideoImplicitTrackPersisted()
{
    FileDoc d;
    CHECK (d.buildScmdJson().empty(), "implicit track: nothing to embed without content");

    d.videoFile = juce::File ("/tmp/movie.mp4");
    const std::string json = d.buildScmdJson();
    CHECK (!json.empty(), "implicit track: the video reference makes the bundle worth embedding");

    sc::MetaBundle b;
    CHECK (sc::parseBundle (juce::String (json), b, nullptr), "implicit track: the bundle parses back");
    CHECK (b.haveVideoTracks && b.videoTracks.size () == 1
               && b.videoTracks[0].file == d.videoFile
               && std::abs (b.videoTracks[0].offsetSecs) < 1e-9,
           "implicit track: one materialised track with the video file");

    // An explicit list is never duplicated by the implicit track.
    sc::VideoTrackData t;
    t.file = juce::File ("/tmp/other.mp4");
    d.videoTracks.push_back (t);
    const std::string json2 = d.buildScmdJson();
    sc::MetaBundle b2;
    CHECK (sc::parseBundle (juce::String (json2), b2, nullptr)
               && b2.videoTracks.size () == 1 && b2.videoTracks[0].file == t.file,
           "implicit track: explicit tracks are kept as-is");

    // The external .meta.json form carries the same materialisation.
    const std::string meta = d.buildMetaJson();
    sc::MetaBundle b3;
    CHECK (sc::parseBundle (juce::String (meta), b3, nullptr)
               && b3.videoTracks.size () == 1 && b3.videoTracks[0].file == t.file,
           "implicit track: the meta.json form matches");
}


//==============================================================================
// Left-drag pan: the picture follows the pointer one to one. The old code
// added the whole distance since the mouse-down point on every event, so the
// pan sped up and lagged behind a change of direction.
static void testVideoViewPanFollowsPointer()
{
    progress ("video view: a left-drag pans the picture one to one");
    VideoView v;
    v.setSize (300, 300);
    sc::VideoFrame fr;
    fr.image = juce::Image (juce::Image::ARGB, 96, 64, true);
    fr.time = 0.0;
    v.setFrame (fr, 0);
    for (int i = 0; i < 8; ++i) v.zoomIn();   // x5.96: room to pan both ways
    v.setPan ({ 0.0f, 0.0f });

    const juce::ModifierKeys lmb (juce::ModifierKeys::leftButtonModifier);
    v.mouseDown (mkMouse (v, 100, 100, lmb));
    v.mouseDrag (mkMouse (v, 110, 100, lmb));
    CHECK (std::abs (v.getPan().x - 10.0f) < 1e-3f && std::abs (v.getPan().y) < 1e-3f,
           "video pan: 10 px of drag move the picture 10 px");
    v.mouseDrag (mkMouse (v, 120, 104, lmb));
    CHECK (std::abs (v.getPan().x - 20.0f) < 1e-3f && std::abs (v.getPan().y - 4.0f) < 1e-3f,
           "video pan: the picture stays under the pointer (no acceleration)");
    v.mouseDrag (mkMouse (v, 115, 104, lmb));
    CHECK (std::abs (v.getPan().x - 15.0f) < 1e-3f,
           "video pan: turning back moves the picture back at once");
    v.mouseUp (mkMouse (v, 115, 104, lmb));

    // Against an edge: the picture stops there, and turning back moves it
    // back immediately (no dead zone to unwind first).
    v.setPan ({ 1.0e6f, 0.0f });
    const float maxX = v.getPan().x;
    CHECK (maxX > 50.0f, "video pan: the zoomed picture has room to pan");
    v.setPan ({ maxX - 5.0f, 0.0f });
    v.mouseDown (mkMouse (v, 100, 100, lmb));
    v.mouseDrag (mkMouse (v, 160, 100, lmb));   // 60 px, only 5 px of room
    CHECK (std::abs (v.getPan().x - maxX) < 1e-3f, "video pan: stops at the picture edge");
    v.mouseDrag (mkMouse (v, 150, 100, lmb));   // 10 px back
    CHECK (std::abs (v.getPan().x - (maxX - 10.0f)) < 1e-3f,
           "video pan: after the edge, turning back moves at once");
    v.mouseUp (mkMouse (v, 150, 100, lmb));

    // A drag that started without a picture never pans.
    v.clearFrame();
    v.mouseDown (mkMouse (v, 100, 100, lmb));
    v.setFrame (fr, 0);
    v.mouseDrag (mkMouse (v, 150, 100, lmb));
    CHECK (v.getPan().isOrigin(), "video pan: no pan from a drag started before the picture");
    v.mouseUp (mkMouse (v, 150, 100, lmb));
}

//==============================================================================
// Dragging a band up/down reorders the tracks; left/right keeps moving the
// offset. The first pixels pick the axis, so a reorder never nudges the
// offset.
static void testVideoTracksReorderDrag()
{
    progress ("video tracks: dragging a band up/down changes the track order");
    CHECK (videoTrackIndexAfterMove (0, 0, 2) == 2 && videoTrackIndexAfterMove (1, 0, 2) == 0
           && videoTrackIndexAfterMove (2, 0, 2) == 1 && videoTrackIndexAfterMove (3, 0, 2) == 3
           && videoTrackIndexAfterMove (-1, 0, 2) == -1,
           "video reorder: indexes after a move down");
    CHECK (videoTrackIndexAfterMove (2, 2, 0) == 0 && videoTrackIndexAfterMove (0, 2, 0) == 1
           && videoTrackIndexAfterMove (1, 2, 0) == 2 && videoTrackIndexAfterMove (3, 2, 0) == 3,
           "video reorder: indexes after a move up");
    {
        std::vector<int> v { 10, 11, 12, 13 };
        moveVideoTrack (v, 0, 2);
        CHECK ((v == std::vector<int> { 11, 12, 10, 13 }), "video reorder: move down");
        moveVideoTrack (v, 2, 0);
        CHECK ((v == std::vector<int> { 10, 11, 12, 13 }), "video reorder: move up restores");
        moveVideoTrack (v, 1, 9);
        CHECK ((v == std::vector<int> { 10, 11, 12, 13 }), "video reorder: out-of-range target ignored");
    }

    VideoTracksPanel p;
    p.setSize (300, VideoTracksPanel::kRulerH + 4 * VideoTracksPanel::kRowH + VideoTracksPanel::kBarW);
    p.setAudioDuration (10.0);
    VideoTracksPanel::Track a, b, c;
    a.file = juce::File ("/tmp/sc_reorder_a.mp4"); a.durationSecs = 10.0;
    b.file = juce::File ("/tmp/sc_reorder_b.mp4"); b.durationSecs = 10.0;
    c.file = juce::File ("/tmp/sc_reorder_c.mp4"); c.durationSecs = 10.0;
    p.setTracks ({ a, b, c });

    int movedFrom = -1, movedTo = -1, offsetCalls = 0;
    p.onTrackMoved = [&] (int f, int t) { movedFrom = f; movedTo = t; };
    p.onOffsetChanged = [&] (int, double) { ++offsetCalls; };
    const juce::ModifierKeys lmb (juce::ModifierKeys::leftButtonModifier);
    const int x = (int) p.xForTime (5.0);
    const int row0 = VideoTracksPanel::kRulerH + VideoTracksPanel::kRowH / 2;
    const int row2 = row0 + 2 * VideoTracksPanel::kRowH;

    p.mouseDown (mkMouse (p, x, row0, lmb));
    p.mouseDrag (mkMouse (p, x + 1, row0 + 2, lmb));   // under the threshold
    CHECK (p.pendingDropRow() == -1 && offsetCalls == 0 && p.getTracks()[0].offsetSecs == 0.0,
           "video reorder: a tiny move picks no axis yet");
    p.mouseDrag (mkMouse (p, x + 2, row2, lmb));       // mostly vertical
    CHECK (p.pendingDropRow() == 2 && offsetCalls == 0 && p.getTracks()[0].offsetSecs == 0.0,
           "video reorder: a vertical drag targets the third row and keeps the offset");
    {
        // While dragging, a white line marks where the band lands (below row 2).
        juce::Image img (juce::Image::RGB, p.getWidth(), p.getHeight(), true);
        juce::Graphics g (img);
        p.paint (g);
        const int ly = VideoTracksPanel::kRulerH + 3 * VideoTracksPanel::kRowH;
        const auto c0 = img.getPixelAt (p.getWidth() - 20, ly);
        CHECK (c0.getRed() > 230 && c0.getGreen() > 230 && c0.getBlue() > 230,
               "video reorder: the insertion line is drawn below the target row");
    }
    p.mouseUp (mkMouse (p, x + 2, row2, lmb));
    CHECK (movedFrom == 0 && movedTo == 2, "video reorder: released on row 2 -> track 0 moves to 2");
    CHECK (p.getTracks()[0].file == b.file && p.getTracks()[1].file == c.file
           && p.getTracks()[2].file == a.file, "video reorder: the list shows the new order");
    CHECK (p.getSelected() == 2, "video reorder: the moved track stays selected");
    CHECK (p.pendingDropRow() == -1, "video reorder: nothing pending after the release");

    // Dropped back on its own row: no move.
    movedFrom = -1;
    p.mouseDown (mkMouse (p, x, row2, lmb));
    p.mouseDrag (mkMouse (p, x, row2 + 8, lmb));
    p.mouseUp (mkMouse (p, x, row2 + 8, lmb));
    CHECK (movedFrom == -1, "video reorder: dropping on the same row changes nothing");

    // A mostly horizontal drag moves the offset only.
    p.mouseDown (mkMouse (p, x, row0, lmb));
    p.mouseDrag (mkMouse (p, x + 40, row0 + 6, lmb));
    p.mouseUp (mkMouse (p, x + 40, row0 + 6, lmb));
    CHECK (movedFrom == -1 && offsetCalls > 0 && p.getTracks()[0].offsetSecs > 0.5,
           "video reorder: a horizontal drag moves the offset, not the order");

    // A lone track has no order to change: its offset follows from the first pixel.
    VideoTracksPanel q;
    q.setSize (300, VideoTracksPanel::kRulerH + 4 * VideoTracksPanel::kRowH + VideoTracksPanel::kBarW);
    q.setAudioDuration (10.0);
    q.setTracks ({ a });
    double off = -1.0;
    q.onOffsetChanged = [&] (int, double o) { off = o; };
    q.mouseDown (mkMouse (q, x, row0, lmb));
    q.mouseDrag (mkMouse (q, x + 2, row0 + 1, lmb));
    q.mouseUp (mkMouse (q, x + 2, row0 + 1, lmb));
    CHECK (off > 0.0 && off < 0.2, "video reorder: a single track's offset moves from the first pixel");
}

//==============================================================================
// The reorder reaches the document (no picture decoder needed: the files do
// not even exist, only the list matters).
static void testVideoTrackReorderSavedInDocument()
{
    progress ("video tracks: a reorder reaches the document's track list");
    const juce::File store = makeTempFile ("video_reorder.recent");
    store.deleteFile();
    const juce::File wav = makeTempFile ("video_reorder.wav");
    writeWav (wav, 1, 48000.0, 48000, [] (int, int64_t, double) { return 0.0f; });
    {
        Application app (store);
        std::unique_ptr<MainWindow> win (new MainWindow (app, "video reorder test"));
        CHECK (app.openFile (wav), "video reorder doc: the wav opens");
        FileDoc* d = app.doc();
        if (d != nullptr)
        {
            sc::VideoTrackData a, b, c;
            a.file = juce::File ("/tmp/sc_reorder_doc_a.mp4");
            b.file = juce::File ("/tmp/sc_reorder_doc_b.mp4"); b.offsetSecs = 0.25;
            c.file = juce::File ("/tmp/sc_reorder_doc_c.mp4"); c.offsetSecs = 0.5;
            d->setVideoTracks ({ a, b, c });
            win->syncVideoWindow();
            auto* vw = win->getVideoWindow();
            CHECK (vw != nullptr, "video reorder doc: the window exists for the tracks");
            if (vw != nullptr)
            {
                auto& p = vw->getTracksPanel();
                p.setSize (300, VideoTracksPanel::kRulerH + 4 * VideoTracksPanel::kRowH + VideoTracksPanel::kBarW);
                const juce::ModifierKeys lmb (juce::ModifierKeys::leftButtonModifier);
                const int x = (int) p.xForTime (0.75);   // every band covers it
                const int row0 = VideoTracksPanel::kRulerH + VideoTracksPanel::kRowH / 2;
                const int row2 = row0 + 2 * VideoTracksPanel::kRowH;
                d->metaDirty = false;
                p.mouseDown (mkMouse (p, x, row2, lmb));   // selects track 2 (c)
                p.mouseDrag (mkMouse (p, x, row0, lmb));
                p.mouseUp (mkMouse (p, x, row0, lmb));
                CHECK (d->videoTracks.size() == 3 && d->videoTracks[0].file == c.file
                       && d->videoTracks[1].file == a.file && d->videoTracks[2].file == b.file,
                       "video reorder doc: the document holds the new order");
                CHECK (std::abs (d->videoTracks[0].offsetSecs - 0.5) < 1e-12,
                       "video reorder doc: the moved track keeps its offset");
                CHECK (d->selectedVideoTrack == 0, "video reorder doc: the selection follows the track");
                CHECK ((bool) d->metaDirty, "video reorder doc: the new order is saved with the file");
            }
        }
        win.reset();
    }
    wav.deleteFile();
    store.deleteFile();
}

//==============================================================================
static juce::Image videoScalerRandomImage (int w, int h, int seed)
{
    juce::Random rnd (seed);
    juce::Image img (juce::Image::ARGB, w, h, false);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            img.setPixelAt (x, y, juce::Colour ((juce::uint8) rnd.nextInt (256), (juce::uint8) rnd.nextInt (256),
                                                (juce::uint8) rnd.nextInt (256), (juce::uint8) 255));
    return img;
}

static bool videoScalerSamePixels (const juce::Image& a, const juce::Image& b)
{
    if (a.getWidth() != b.getWidth() || a.getHeight() != b.getHeight()) return false;
    for (int y = 0; y < a.getHeight(); ++y)
        for (int x = 0; x < a.getWidth(); ++x)
            if (a.getPixelAt (x, y).getARGB() != b.getPixelAt (x, y).getARGB()) return false;
    return true;
}

// Kernels, exactness (identity, rotation, nearest blocks, constants, ramps),
// weight reuse and multi-threading of the picture scaler.
static void testVideoScaler()
{
    progress ("video scaler: kernels, identity, rotation, nearest, ramps, threads");
    using sc::VideoUpscaling;
    const VideoUpscaling all[] = { VideoUpscaling::lanczos, VideoUpscaling::bicubic, VideoUpscaling::nearest };

    for (const auto m : { VideoUpscaling::lanczos, VideoUpscaling::bicubic })
    {
        bool zeros = true;
        for (int k = 1; k <= 4; ++k)
            zeros = zeros && std::abs (sc::videoScalerKernel (m, k)) < 1e-9
                          && std::abs (sc::videoScalerKernel (m, -k)) < 1e-9;
        CHECK (std::abs (sc::videoScalerKernel (m, 0.0) - 1.0) < 1e-12 && zeros,
               "video scaler: " + sc::videoUpscalingName (m) + " is 1 at 0 and 0 at the other integers");
    }
    CHECK (std::abs (sc::videoScalerKernel (VideoUpscaling::lanczos, 0.5) - 0.607927) < 1e-5
           && sc::videoScalerKernel (VideoUpscaling::lanczos, 3.0) == 0.0,
           "video scaler: Lanczos a = 3");
    CHECK (std::abs (sc::videoScalerKernel (VideoUpscaling::bicubic, 0.5) - 0.575) < 1e-12
           && std::abs (sc::videoScalerKernel (VideoUpscaling::bicubic, 1.5) + 0.075) < 1e-12,
           "video scaler: Bicubic A = -0.6 (swscale's default)");
    for (const auto m : all)
        CHECK (sc::videoUpscalingFromName (sc::videoUpscalingName (m)) == m,
               "video scaler: name round trip " + sc::videoUpscalingName (m));
    CHECK (sc::videoUpscalingFromName ("nearest") == VideoUpscaling::nearest
           && sc::videoUpscalingFromName ("bogus") == VideoUpscaling::lanczos,
           "video scaler: names are case-insensitive, unknown = Lanczos");

    // 1:1 gives the picture back for every method.
    const juce::Image src = videoScalerRandomImage (13, 9, 1234);
    for (const auto m : all)
    {
        sc::VideoScaler s;
        juce::Image out;
        const bool ok = s.render (src, 0, { 0.0, 0.0, 13.0, 9.0 }, 13, 9, m, out);
        CHECK (ok && videoScalerSamePixels (out, src),
               "video scaler: 1:1 is the identity (" + sc::videoUpscalingName (m) + ")");
    }

    // Quarter turns: exactly the picture turned clockwise.
    for (const int rot : { 90, 180, 270 })
    {
        const bool quarter = rot != 180;
        const int wd = quarter ? 9 : 13, hd = quarter ? 13 : 9;
        juce::Image expected (juce::Image::ARGB, wd, hd, false);
        for (int v = 0; v < hd; ++v)
            for (int u = 0; u < wd; ++u)
            {
                int x = u, y = v;
                if (rot == 90)       { x = v;          y = 9 - 1 - u; }
                else if (rot == 180) { x = 13 - 1 - u; y = 9 - 1 - v; }
                else                 { x = 13 - 1 - v; y = u; }
                expected.setPixelAt (u, v, src.getPixelAt (x, y));
            }
        for (const auto m : { VideoUpscaling::lanczos, VideoUpscaling::nearest })
        {
            sc::VideoScaler s;
            juce::Image out;
            s.render (src, rot, { 0.0, 0.0, (double) wd, (double) hd }, wd, hd, m, out);
            CHECK (videoScalerSamePixels (out, expected),
                   "video scaler: " + juce::String (rot) + " degrees = the picture turned clockwise ("
                   + sc::videoUpscalingName (m) + ")");
        }
    }

    // Nearest x4: blocks of the source pixels.
    {
        sc::VideoScaler s;
        juce::Image out;
        s.render (src, 0, { 0.0, 0.0, 13.0, 9.0 }, 52, 36, VideoUpscaling::nearest, out);
        bool blocks = true;
        for (int y = 0; y < 36 && blocks; ++y)
            for (int x = 0; x < 52; ++x)
                if (out.getPixelAt (x, y).getARGB() != src.getPixelAt (x / 4, y / 4).getARGB()) { blocks = false; break; }
        CHECK (blocks, "video scaler: Nearest x4 repeats each source pixel 4x4");
    }

    // A flat colour stays exactly flat (normalised weights, no ringing).
    {
        juce::Image flat (juce::Image::ARGB, 8, 6, false);
        flat.clear (flat.getBounds(), juce::Colour (100, 150, 200));
        for (const auto m : all)
        {
            sc::VideoScaler s;
            juce::Image out;
            s.render (flat, 0, { 0.7, 0.3, 5.1, 4.2 }, 37, 29, m, out);
            bool same = true;
            for (int y = 0; y < out.getHeight() && same; ++y)
                for (int x = 0; x < out.getWidth(); ++x)
                    if (out.getPixelAt (x, y) != juce::Colour (100, 150, 200)) { same = false; break; }
            CHECK (same, "video scaler: a flat colour stays flat (" + sc::videoUpscalingName (m) + ")");
        }
    }

    // A linear ramp enlarged x3.7 stays a ramp inside (the filters interpolate).
    {
        juce::Image ramp (juce::Image::ARGB, 40, 4, false);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 40; ++x)
                ramp.setPixelAt (x, y, juce::Colour ((juce::uint8) (6 * x), (juce::uint8) (6 * x), (juce::uint8) (6 * x)));
        const int outW = 148;   // 40 * 3.7
        for (const auto m : { VideoUpscaling::lanczos, VideoUpscaling::bicubic })
        {
            sc::VideoScaler s;
            juce::Image out;
            s.render (ramp, 0, { 0.0, 0.0, 40.0, 4.0 }, outW, 8, m, out);
            double worst = 0.0;
            for (int i = 20; i < outW - 20; ++i)
            {
                const double c = (i + 0.5) * 40.0 / outW - 0.5;   // source position of pixel i
                worst = std::max (worst, std::abs (out.getPixelAt (i, 4).getRed() - 6.0 * c));
            }
            CHECK (worst <= 2.0, "video scaler: a ramp stays linear (" + sc::videoUpscalingName (m)
                                 + ", worst " + juce::String (worst, 2) + ")");
        }
    }

    // A step: the filters make a smooth edge, Nearest keeps it hard.
    {
        juce::Image step (juce::Image::ARGB, 8, 2, false);
        for (int x = 0; x < 8; ++x)
            for (int y = 0; y < 2; ++y)
                step.setPixelAt (x, y, x < 4 ? juce::Colours::black : juce::Colours::white);
        auto greys = [&] (VideoUpscaling m)
        {
            sc::VideoScaler s;
            juce::Image out;
            s.render (step, 0, { 0.0, 0.0, 8.0, 2.0 }, 64, 4, m, out);
            int n = 0;
            for (int x = 0; x < 64; ++x)
            {
                const int r = out.getPixelAt (x, 1).getRed();
                if (r > 20 && r < 235) ++n;
            }
            return n;
        };
        CHECK (greys (VideoUpscaling::nearest) == 0, "video scaler: Nearest keeps the edge hard");
        CHECK (greys (VideoUpscaling::lanczos) >= 4 && greys (VideoUpscaling::bicubic) >= 4,
               "video scaler: Lanczos / Bicubic make a smooth edge");
    }

    // The same geometry reuses the weight tables; a pan only rebuilds one axis.
    {
        sc::VideoScaler s;
        juce::Image out;
        s.render (src, 0, { 1.0, 1.0, 8.0, 6.0 }, 40, 30, VideoUpscaling::lanczos, out);
        const int builds = s.weightTableBuilds();
        s.render (src, 0, { 1.0, 1.0, 8.0, 6.0 }, 40, 30, VideoUpscaling::lanczos, out);
        CHECK (s.weightTableBuilds() == builds, "video scaler: the same geometry reuses its weights");
        s.render (src, 0, { 1.5, 1.0, 8.0, 6.0 }, 40, 30, VideoUpscaling::lanczos, out);
        CHECK (s.weightTableBuilds() == builds + 1, "video scaler: a horizontal pan rebuilds one axis");
    }

    // Worker threads give exactly the single-threaded result (up and down).
    {
        const juce::Image big = videoScalerRandomImage (160, 120, 99);
        for (const auto m : all)
            for (const auto& [ow, oh] : { std::pair<int, int> { 700, 500 }, std::pair<int, int> { 53, 41 } })
            {
                sc::VideoScaler one, many;
                one.setMaxWorkerThreads (0);
                many.setMaxWorkerThreads (3);
                juce::Image a, b;
                one.render (big, 90, { 3.25, 2.5, 110.0, 150.0 }, ow, oh, m, a);
                many.render (big, 90, { 3.25, 2.5, 110.0, 150.0 }, ow, oh, m, b);
                CHECK (videoScalerSamePixels (a, b), "video scaler: threads change nothing ("
                       + sc::videoUpscalingName (m) + ", " + juce::String (ow) + "x" + juce::String (oh) + ")");
            }
    }

    // Bad input is refused, the output left alone.
    {
        sc::VideoScaler s;
        juce::Image out;
        CHECK (!s.render (juce::Image(), 0, { 0.0, 0.0, 1.0, 1.0 }, 4, 4, VideoUpscaling::lanczos, out)
               && !s.render (src, 0, { 0.0, 0.0, 0.0, 1.0 }, 4, 4, VideoUpscaling::lanczos, out)
               && !s.render (src, 0, { 0.0, 0.0, 1.0, 1.0 }, 0, 4, VideoUpscaling::lanczos, out)
               && !out.isValid(),
               "video scaler: invalid input is refused");
    }
}

//==============================================================================
// The picture's Upscaling submenu, the scaled painting, and the track
// actions' menu ids (they used to alias the rotate items).
static void testVideoViewUpscaling()
{
    progress ("video view: Upscaling menu, scaled painting, track-action ids");
    VideoView v;
    v.setSize (300, 300);
    CHECK (v.getUpscaling() == sc::VideoUpscaling::lanczos, "video upscaling: Lanczos by default");
    int changes = 0;
    auto last = sc::VideoUpscaling::lanczos;
    v.onUpscalingChanged = [&] (sc::VideoUpscaling m) { ++changes; last = m; };
    v.handleContextMenuId (VideoView::kMenuUpscaleBicubic);
    CHECK (v.getUpscaling() == sc::VideoUpscaling::bicubic && changes == 1
           && last == sc::VideoUpscaling::bicubic, "video upscaling: the menu picks Bicubic");
    v.handleContextMenuId (VideoView::kMenuUpscaleBicubic);
    CHECK (changes == 1, "video upscaling: picking the current method again notifies nothing");
    v.setUpscaling (sc::VideoUpscaling::nearest);
    CHECK (changes == 1 && v.getUpscaling() == sc::VideoUpscaling::nearest,
           "video upscaling: restoring a saved method does not notify");

    // Track actions in the picture's menu run the action, never a rotation.
    sc::VideoFrame fr;
    fr.image = juce::Image (juce::Image::ARGB, 8, 8, true);
    fr.time = 0.0;
    v.setFrame (fr, 0);
    int actId = -1, actIdx = -1;
    v.getDisplayedTrack = [] { return 3; };
    v.onTrackMenuAction = [&] (int id, int idx) { actId = id; actIdx = idx; };
    v.handleContextMenuId (VideoView::kMenuTrackActionBase + VideoTracksPanel::kMenuSyncAudio);
    CHECK (actId == VideoTracksPanel::kMenuSyncAudio && actIdx == 3 && v.getUserRotation() == 0,
           "video menu: 'Sync based on Audio' runs the track action, no rotation");
    v.handleContextMenuId (VideoView::kMenuTrackActionBase + VideoTracksPanel::kMenuAlign);
    CHECK (actId == VideoTracksPanel::kMenuAlign && v.getUserRotation() == 0,
           "video menu: 'Align...' runs the track action, no rotation");
    actId = -1;
    v.handleContextMenuId (VideoView::kMenuRotateCW);
    CHECK (v.getUserRotation() == 90 && actId == -1, "video menu: the rotate item only rotates");
    v.setUserRotation (0);

    // Painting an 8x8 checkerboard at x8: the screen shows about one source
    // pixel boundary. Nearest keeps the two colours, the filters blend them.
    juce::Image checker (juce::Image::ARGB, 8, 8, false);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            checker.setPixelAt (x, y, ((x + y) & 1) ? juce::Colours::white : juce::Colours::black);
    fr.image = checker;
    v.setFrame (fr, 0);
    for (int i = 0; i < 12; ++i) v.zoomIn();
    auto greys = [&] (sc::VideoUpscaling m)
    {
        v.setUpscaling (m);
        juce::Image img (juce::Image::RGB, 300, 300, true);
        {
            juce::Graphics g (img);
            v.paint (g);
        }
        int n = 0;
        for (int y = 0; y < 286; y += 2)
            for (int x = 0; x < 286; x += 2)
            {
                const int r = img.getPixelAt (x, y).getRed();
                if (r > 25 && r < 230) ++n;
            }
        return n;
    };
    CHECK (greys (sc::VideoUpscaling::nearest) == 0, "video upscaling: Nearest paints the raw pixels");
    CHECK (greys (sc::VideoUpscaling::lanczos) > 500, "video upscaling: Lanczos paints smooth transitions");
    CHECK (greys (sc::VideoUpscaling::bicubic) > 500, "video upscaling: Bicubic paints smooth transitions");
    const int builds = v.scaler().weightTableBuilds();
    {
        juce::Image img (juce::Image::RGB, 300, 300, true);
        juce::Graphics g (img);
        v.paint (g);
    }
    CHECK (v.scaler().weightTableBuilds() == builds, "video upscaling: a plain repaint reuses the scaled picture");
}

//==============================================================================
// The Upscaling choice is kept in the settings store across sessions.
static void testVideoUpscalingRemembered()
{
    progress ("video view: the Upscaling choice is remembered between sessions");
    const juce::File store = makeTempFile ("video_upscaling.recent");
    store.deleteFile();
    const juce::File wav = makeTempFile ("video_upscaling.wav");
    writeWav (wav, 1, 48000.0, 4800, [] (int, int64_t, double) { return 0.0f; });
    {
        Application app (store);
        app.recentStore->setValue ("video_upscaling", "Bicubic");
        std::unique_ptr<MainWindow> win (new MainWindow (app, "video upscaling test"));
        CHECK (app.openFile (wav), "video upscaling: the wav opens");
        if (FileDoc* d = app.doc())
        {
            sc::VideoTrackData t;
            t.file = juce::File ("/tmp/sc_upscaling_missing.mp4");
            d->setVideoTracks ({ t });
            win->syncVideoWindow();
        }
        auto* vw = win->getVideoWindow();
        CHECK (vw != nullptr && vw->view().getUpscaling() == sc::VideoUpscaling::bicubic,
               "video upscaling: the saved choice is restored");
        if (vw != nullptr)
            vw->view().handleContextMenuId (VideoView::kMenuUpscaleNearest);
        CHECK (app.recentStore->getValue ("video_upscaling") == "Nearest",
               "video upscaling: a new choice is saved at once");
        win.reset();
    }
    {
        Application app2 (store);
        CHECK (app2.recentStore->getValue ("video_upscaling") == "Nearest",
               "video upscaling: the next session reads it back");
    }
    wav.deleteFile();
    store.deleteFile();
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
    testVideoTracksSelectionBand();
    testVideoWindowMultiTrack();
    testDockedVideoReceivesFrames();
    testVideoAdjustmentNoCompounding();
    testVideoHostSplitAndCollapse();
    testVideoTracksPanelScrollbars();
    testVideoImplicitTrackPersisted();
    testVideoSyncMath();
    testVideoTrackMenuActions();
    testVideoViewPanFollowsPointer();
    testVideoTracksReorderDrag();
    testVideoTrackReorderSavedInDocument();
    testVideoScaler();
    testVideoViewUpscaling();
    testVideoUpscalingRemembered();
}