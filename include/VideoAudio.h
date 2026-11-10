/**
 * SpectroCraft - Video container audio extraction (FFmpeg on Windows,
 * AVFoundation on macOS)
 *
 * Opens video/audio containers (.mp4, .mkv, .avi, .webm, ...) through the
 * vendored FFmpeg runtime (extern/ffmpeg, LGPL-2.1+, separate DLLs on
 * Windows) or AVFoundation (macOS: .mp4, .mov, .m4v, .3gp), decodes only the
 * first audio stream and exposes it as a normal JUCE AudioFormatReader —
 * so the rest of SpectroCraft (streaming engine,
 * views, edits, saving) works exactly as for WAV/FLAC/etc. The picture is
 * shown by the Video window (VideoFrames.h); saving always produces plain
 * audio files.
 */
#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <functional>

namespace sc {

#if defined(SC_ENABLE_FFMPEG) && SC_ENABLE_FFMPEG
class VideoAudioFormat;   // Defined in src/VideoAudio.cpp when FFmpeg is enabled.
#endif

/** Registers the video-container audio format with a format manager so that
    files like .mp4/.mkv/.avi can be opened as their audio track. No-op on
    Linux and on Windows built with -DSPECTRO_ENABLE_FFMPEG=OFF.
    Call after registerBasicFormats() so the native formats keep priority on
    shared extensions (.wav, .mp3, .flac, ...). */
void registerVideoFormats (juce::AudioFormatManager&);

/** Space-separated list of container extensions that can be opened as audio
    (".mp4 .m4a .mkv ..."). Empty when no reader is available (Linux,
    Windows without FFmpeg), in which case no video file can be read at all.
    The File Open dialog and drag & drop use this so they always match what
    the engine can actually open. */
const juce::String& videoContainerExtensions();

// ---------------------------------------------------------------------------
// "Extract Audio Only": the audio track of a (large) video written to a WAV
// file, which is then opened instead of the video (MainWindow offers it when
// a video bigger than 200 MB is opened). Platform-independent: it reads
// through the same formats as the engine.

/** The audio track of a video file, as SpectroCraft reads it. */
struct VideoAudioInfo
{
    int channels = 0;
    double sampleRate = 0.0;
    juce::int64 frames = 0;

    double durationSecs() const noexcept { return sampleRate > 0.0 ? (double) frames / sampleRate : 0.0; }
    /** Size of the WAV file extractAudioToWav writes (32-bit float samples
        plus a few KB of header). */
    juce::int64 wavBytes() const noexcept { return frames * (juce::int64) channels * 4 + 4096; }
};

/** Reads the format of the file's audio track (opens it, decodes nothing
    worth mentioning). False when this build cannot read its audio. */
bool probeVideoAudio (const juce::File& video, VideoAudioInfo& out);

/** Writes the audio track of `video` to `wav`: 32-bit float WAV (the decoded
    samples unchanged; RF64 past 4 GB), the same format as Save As WAV. The
    data goes to a temporary file next to `wav` that replaces it only once
    complete: a failure or a cancel leaves no partial file and an existing
    `wav` untouched. onProgress (0..1) is called from the calling thread;
    run it on a worker thread. On failure *error says why ("Cancelled." for a
    cancel). */
bool extractAudioToWav (const juce::File& video, const juce::File& wav,
                        std::function<void (float)> onProgress = nullptr,
                        const std::atomic<bool>* cancel = nullptr,
                        juce::String* error = nullptr);

// ---------------------------------------------------------------------------
// "Sync based on Audio": two-stage normalized cross-correlation of a video's
// own audio against the document's audio (platform-independent math).

/** Box-decimates `in` from `inRate` to ~`outRate` Hz (averaging, no aliasing
    tricks). Returns `in` unchanged when outRate >= inRate. */
std::vector<float> decimateTo (const std::vector<float>& in, double inRate,
                               double outRate);

/** Coarse stage: normalized cross-correlation of two ~2 kHz-decimated
    channels (`docDs` = the document's first channel, `vidDs` = the video's).
    Returns the document time at which the video's start best sits (seconds;
    negative = the video starts before the audio) and, via stepOut, the grid
    step used. NaN when no lag has a meaningful correlation. */
double videoSyncCoarse (const std::vector<float>& docDs, const std::vector<float>& vidDs,
                        double* stepOut = nullptr);

/** Fine stage: refines `coarseLag` at full rate over +/- one coarse step.
    `docWin` is the document's first channel starting at document time
    `docWinStart`; `vidWin` is the video's own audio from its start. Returns
    the refined lag in seconds, or NaN when nothing correlates. */
double videoSyncFine (const std::vector<float>& docWin, double docRate,
                      double docWinStart,
                      const std::vector<float>& vidWin, double vidRate,
                      double coarseLag, double step);

} // namespace sc
