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
