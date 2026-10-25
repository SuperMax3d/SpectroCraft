/**
 * SpectroCraft - Video container audio extraction (FFmpeg, Windows)
 *
 * Opens video/audio containers (.mp4, .mkv, .avi, .webm, ...) through the
 * vendored FFmpeg runtime (extern/ffmpeg, LGPL-2.1+, separate DLLs on
 * Windows), decodes only the first audio stream and exposes it as a normal
 * JUCE AudioFormatReader — so the rest of SpectroCraft (streaming engine,
 * views, edits, saving) works exactly as for WAV/FLAC/etc. The video track
 * is ignored entirely; saving always produces plain audio files.
 */
#pragma once

#include <JuceHeader.h>

namespace sc {

#if defined(SC_ENABLE_FFMPEG) && SC_ENABLE_FFMPEG
class VideoAudioFormat;   // Defined in src/VideoAudio.cpp when FFmpeg is enabled.
#endif

/** Registers the video-container audio format with a format manager so that
    files like .mp4/.mkv/.avi can be opened as their audio track. No-op when
    built without FFmpeg (non-Windows or -DSPECTRO_ENABLE_FFMPEG=OFF).
    Call after registerBasicFormats() so the native formats keep priority on
    shared extensions (.wav, .mp3, .flac, ...). */
void registerVideoFormats (juce::AudioFormatManager&);

/** Space-separated list of container extensions that can be opened as audio
    (".mp4 .m4a .mkv ..."). Empty when built without FFmpeg, in which case no
    video file can be read at all. The File Open dialog and drag & drop use this
    so they always match what the engine can actually open. */
const juce::String& videoContainerExtensions();

} // namespace sc
