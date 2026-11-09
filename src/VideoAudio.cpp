/**
 * SpectroCraft - Video container audio extraction (FFmpeg on Windows;
 * AVFoundation on macOS, see VideoAudio_mac.mm)
 */

#include "VideoAudio.h"

#if defined(SC_ENABLE_FFMPEG) && SC_ENABLE_FFMPEG

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#if defined(_WIN32)
    // JUCE's headers do not pull in windows.h on their own; keep NOMINMAX set
    // so the min/max macros never leak into later C++ headers.
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #undef max
    #undef min
#endif
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace sc {

//==============================================================================
static bool isPcmCodecId (AVCodecID id)
{
    switch (id)
    {
        case AV_CODEC_ID_PCM_S16LE:
        case AV_CODEC_ID_PCM_S16BE:
        case AV_CODEC_ID_PCM_S24LE:
        case AV_CODEC_ID_PCM_S24BE:
        case AV_CODEC_ID_PCM_S32LE:
        case AV_CODEC_ID_PCM_S32BE:
        case AV_CODEC_ID_PCM_U8:
            return true;
        default:
            return false;
    }
}

//==============================================================================
/** AudioFormatReader over an FFmpeg audio stream.

    Every readSamples() call seeks to the requested position (cheap: one
    demuxer index lookup + decoder flush) and decodes forward, positioning
    output sample-accurately using frame timestamps. Regions beyond EOF are
    zero-filled, matching JUCE's reader contract. */
class VideoAudioReader : public juce::AudioFormatReader
{
public:
    // streamToOwn is the InputStream the AudioFormatManager opened for us;
    // we keep it alive and delete it (FFmpeg opens its own file handle).
    VideoAudioReader (const juce::File& file, juce::InputStream* streamToOwn);
    ~VideoAudioReader() override;

private:
    bool readSamples (int* const* destChannels, int numDestChannels,
                      int startOffsetInDestBuffer, int64 startSampleInFile,
                      int numSamples) override;

    void seekToSample (int64 target);
    int  processFrame (AVFrame* fr, int* const* destChannels, int numDestChannels,
                       int& destOffset, int64& pos, int& budget);
    int64 frameStartSample (const AVFrame* fr) const;

    juce::InputStream* ownedStream_ = nullptr;
    AVFormatContext* fmt_ = nullptr;
    AVCodecContext*  dec_ = nullptr;
    SwrContext*      swr_ = nullptr;
    int              streamIdx_ = -1;
    bool             eof_ = false;

    JUCE_DECLARE_NON_COPYABLE (VideoAudioReader);
};

//==============================================================================
VideoAudioReader::VideoAudioReader (const juce::File& file, juce::InputStream* streamToOwn)
    : AudioFormatReader (nullptr, "FFmpeg audio"), ownedStream_ (streamToOwn)
{
#if defined(_WIN32)
    // The FFmpeg DLLs are delay-loaded (/DELAYLOAD), so a missing runtime would
    // otherwise abort the process with a SEH delay-load failure on the first
    // FFmpeg call below. Probe it for a graceful "cannot open" instead: the
    // standard search order (exe directory, then the SpectroCraft subfolder
    // added at startup by sc::setupRuntimeDllSearchPath) covers both layouts,
    // and success loads all four DLLs at once, so the delayed import stubs can
    // never fail afterwards.
    static const bool ffmpegRuntimePresent = [] {
        HMODULE h = ::LoadLibraryA ("avformat-61.dll");
        if (h == nullptr)
            std::fprintf (stderr, "[video] FFmpeg runtime not found - video container "
                                   "extraction is unavailable (Windows error %lu).\n",
                           (unsigned) GetLastError ());
        return h != nullptr;
    } ();
    if (!ffmpegRuntimePresent)
        return;   // numChannels stays 0 -> createReaderFor() reports failure
#endif

    usesFloatingPointData = true;
    bitsPerSample = 32;

    const std::string path = file.getFullPathName().toStdString();   // UTF-8
    if (avformat_open_input (&fmt_, path.c_str(), nullptr, nullptr) < 0 || fmt_ == nullptr)
        return;
    if (avformat_find_stream_info (fmt_, nullptr) < 0)
        return;

    for (unsigned i = 0; i < fmt_->nb_streams; ++i)
        if (fmt_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {   streamIdx_ = (int) i; break; }
    if (streamIdx_ < 0)
        return;

    AVStream* st = fmt_->streams[streamIdx_];
    sampleRate  = (double) st->codecpar->sample_rate;
    numChannels = st->codecpar->ch_layout.nb_channels > 0 ? (unsigned int) st->codecpar->ch_layout.nb_channels : 1u;
    if (sampleRate <= 0.0 || numChannels == 0)
        return;

    // Length. NB: st->nb_frames is only a *sample* count for PCM streams
    // (AVI's strh dwLength); for compressed formats like MP4/AAC it is the
    // number of packets (e.g. 647 AAC frames), so prefer the stream duration,
    // which is exact there (derived from the sample tables).
    const int64_t dur_tb = (st->duration != AV_NOPTS_VALUE ? st->duration : fmt_->duration);
    if (isPcmCodecId (st->codecpar->codec_id) && st->nb_frames > 0)
        lengthInSamples = st->nb_frames;
    else if (dur_tb > 0)
        lengthInSamples = av_rescale_q (dur_tb, st->time_base, AVRational { 1, (int) sampleRate });

    // Rare containers without a duration: estimate from file size / bitrate.
    if (lengthInSamples <= 0 && st->codecpar->bit_rate > 0 && file.getSize() > 0)
        lengthInSamples = (int64_t) ((double) file.getSize() * 8.0
                                     / (double) st->codecpar->bit_rate * sampleRate + 0.5);

    const AVCodec* codec = avcodec_find_decoder (st->codecpar->codec_id);
    if (codec == nullptr)
        return;
    dec_ = avcodec_alloc_context3 (codec);
    if (dec_ == nullptr || avcodec_parameters_to_context (dec_, st->codecpar) < 0)
        return;
    if (avcodec_open2 (dec_, codec, nullptr) < 0)
        return;

    // Native format/layout -> planar float at the stream's rate.
    SwrContext* tmp = nullptr;
    const int srRet = swr_alloc_set_opts2 (&tmp, &dec_->ch_layout, AV_SAMPLE_FMT_FLTP,
                                           (int) sampleRate, &st->codecpar->ch_layout,
                                           (AVSampleFormat) st->codecpar->format,
                                           st->codecpar->sample_rate, 0, nullptr);
    if (srRet < 0 || tmp == nullptr)
        return;
    if (swr_init (tmp) < 0)
    {   swr_free (&tmp); return; }
    swr_ = tmp;

    // Success — keep everything.
}

VideoAudioReader::~VideoAudioReader()
{
    delete ownedStream_;   // the manager's stream is ours once we were created
    if (swr_ != nullptr)
        swr_free (&swr_);
    if (dec_ != nullptr)
        avcodec_free_context (&dec_);
    if (fmt_ != nullptr)
        avformat_close_input (&fmt_);
}

//==============================================================================
int64 VideoAudioReader::frameStartSample (const AVFrame* fr) const
{
    AVStream* st = fmt_->streams[streamIdx_];
    if (fr->best_effort_timestamp != AV_NOPTS_VALUE)
        return av_rescale_q (fr->best_effort_timestamp, st->time_base, AVRational { 1, (int) sampleRate });
    // No timestamps: assume contiguous with what has been written so far.
    return -1;   // caller treats this as "unknown"
}

//==============================================================================
/** Converts one decoded frame to planar float and copies the part that falls
    inside [pos, pos + budget) into dest (starting at destOffset). Returns the
    number of samples written and advances pos/destOffset. */
int VideoAudioReader::processFrame (AVFrame* fr, int* const* destChannels, int numDestChannels,
                                    int& destOffset, int64& pos, int& budget)
{
    if (fr->nb_samples <= 0 || budget <= 0)
        return 0;

    const int64 S = frameStartSample (fr);
    if (S >= 0 && (S + fr->nb_samples <= pos || S >= pos + budget))
        return 0;   // entirely outside the requested window

    // Zero-fill any gap between what is written so far and this frame's start.
    const int64 gapEnd = (S < 0 ? pos : std::min<int64_t> (S, pos + budget));
    if (gapEnd > pos)
    {
        const int64 gap = gapEnd - pos;
        for (int c = 0; c < numDestChannels; ++c)
            if (destChannels[c] != nullptr)
                std::memset ((float*) destChannels[c] + destOffset, 0, (size_t) gap * sizeof (float));
        pos += gap;
        destOffset += (int) gap;
        budget -= (int) gap;
    }

    const int nSrcCh = dec_->ch_layout.nb_channels;
    std::vector<std::vector<float>> planes ((size_t) nSrcCh, std::vector<float> ((size_t) fr->nb_samples));
    std::vector<uint8_t*> outPtrs ((size_t) nSrcCh);
    for (int c = 0; c < nSrcCh; ++c)
        outPtrs[(size_t) c] = reinterpret_cast<uint8_t*> (planes[(size_t) c].data());

    const int conv = swr_convert (swr_, outPtrs.data(), fr->nb_samples,
                                  (const uint8_t**) fr->extended_data, fr->nb_samples);
    if (conv <= 0)
        return 0;

    const int64 from = std::max<int64_t> (pos, S < 0 ? pos : S);
    const int64 to   = std::min<int64_t> (from + budget, S < 0 ? pos + conv : S + conv);
    if (to <= from)
        return 0;

    const int skip = (int) (from - (S < 0 ? pos : S));
    const int take = (int) (to - from);
    for (int c = 0; c < numDestChannels && c < nSrcCh; ++c)
        if (destChannels[c] != nullptr)
            std::memcpy ((float*) destChannels[c] + destOffset, planes[(size_t) c].data() + skip,
                         (size_t) take * sizeof (float));

    pos += take;
    destOffset += take;
    budget -= take;
    return take;
}

//==============================================================================
void VideoAudioReader::seekToSample (int64 target)
{
    AVStream* st = fmt_->streams[streamIdx_];
    // Seek well before the target: after a flush, transform codecs (AAC,
    // Vorbis, Opus) rebuild their first frame without the previous frame's
    // overlap, and a backward seek can still land just past the target
    // (both gave wrong or missing samples on random reads of an .mp4).
    // processFrame() drops everything decoded before the requested window.
    constexpr int64 kSeekPreroll = 8192;
    const int64 preroll = std::max<int64> (kSeekPreroll, (int64) st->codecpar->seek_preroll);
    const int64 from = std::max<int64> (0, target - preroll);
    const int64_t ts = av_rescale_q (from, AVRational { 1, (int) sampleRate }, st->time_base);

    if (av_seek_frame (fmt_, streamIdx_, ts, AVSEEK_FLAG_BACKWARD) < 0)
        return;   // keep the current position; decoding continues from there
    avcodec_flush_buffers (dec_);
    eof_ = false;
}

//==============================================================================
bool VideoAudioReader::readSamples (int* const* destChannels, int numDestChannels,
                                    int startOffsetInDestBuffer, int64 startSampleInFile,
                                    int numSamples)
{
    // Zero-fill first: covers gaps between frames and everything beyond EOF.
    for (int c = 0; c < numDestChannels; ++c)
        if (destChannels[c] != nullptr)
            std::memset ((float*) destChannels[c] + startOffsetInDestBuffer, 0,
                         (size_t) numSamples * sizeof (float));

    if (streamIdx_ < 0 || numSamples <= 0)
        return true;

    seekToSample (startSampleInFile);

    int64 pos = startSampleInFile;
    int destOffset = startOffsetInDestBuffer;
    int budget = numSamples;

    while (budget > 0 && !eof_)
    {
        AVPacket* pkt = av_packet_alloc();
        if (pkt == nullptr)
            break;

        const int ret = av_read_frame (fmt_, pkt);
        if (ret < 0)
        {   // End of stream: flush the decoder to drain any buffered frames.
            eof_ = true;
            av_packet_free (&pkt);
            avcodec_send_packet (dec_, nullptr);
            for (;;)
            {   AVFrame* fr = av_frame_alloc();
                if (avcodec_receive_frame (dec_, fr) < 0)
                    break;
                processFrame (fr, destChannels, numDestChannels, destOffset, pos, budget);
                av_frame_free (&fr); }
            break;
        }

        if (pkt->stream_index == streamIdx_)
        {
            int sr = avcodec_send_packet (dec_, pkt);
            if (sr == AVERROR(EAGAIN))
            {   // Decoder still has output pending: drain it, then resend.
                for (;;)
                {   AVFrame* fr = av_frame_alloc();
                    if (avcodec_receive_frame (dec_, fr) < 0)
                        break;
                    processFrame (fr, destChannels, numDestChannels, destOffset, pos, budget);
                    av_frame_free (&fr); }
                sr = avcodec_send_packet (dec_, pkt);
            }
            if (sr >= 0)
            {
                for (;;)
                {   AVFrame* fr = av_frame_alloc();
                    if (avcodec_receive_frame (dec_, fr) < 0)
                        break;
                    processFrame (fr, destChannels, numDestChannels, destOffset, pos, budget);
                    av_frame_free (&fr); }
            }
        }

        av_packet_free (&pkt);   // we always own the packet (send does not take it)
    }

    return true;   // short reads / EOF are zero-filled, which is not an error
}

//==============================================================================
const juce::String& videoContainerExtensions()
{
    static const juce::String exts =
        ".mp4 .m4a .m4b .3gp .3g2 .mov .mkv .webm .avi .wmv .asf .flv "
        ".ts .m2ts .mts .ogv .wma .ac3 .eac3 .dts .dtshd .ape .wv .tta .amr .caf .mpc";
    return exts;
}

class VideoAudioFormat : public juce::AudioFormat
{
public:
    VideoAudioFormat()
        // Explicit StringRefs: a bare String would be ambiguous between the
        // (String, StringArray) and (StringRef, StringRef) constructors.
        : AudioFormat (juce::StringRef ("FFmpeg audio"),
                      juce::StringRef (videoContainerExtensions())) {}

    // We accept whatever the container holds: no fixed rate/bit-depth lists.
    juce::Array<int> getPossibleSampleRates() override { return {}; }
    juce::Array<int> getPossibleBitDepths()     override { return {}; }
    bool canDoStereo() override { return true; }
    bool canDoMono()   override { return true; }

    // Read-only format: no writers.
    std::unique_ptr<juce::AudioFormatWriter> createWriterFor (std::unique_ptr<juce::OutputStream>&,
                                                             const juce::AudioFormatWriterOptions&) override
    {   return nullptr; }

    juce::AudioFormatReader* createReaderFor (juce::InputStream* sourceStream,
                                              bool deleteStreamIfOpeningFails) override
    {
        // The manager hands us a FileInputStream for file-based loads; FFmpeg
        // opens its own handle, so we only need the path.
        auto* fin = dynamic_cast<juce::FileInputStream*> (sourceStream);
        if (fin == nullptr)
        {
            if (deleteStreamIfOpeningFails)
                delete sourceStream;
            return nullptr;
        }

        const juce::File file = fin->getFile();
        auto* r = new VideoAudioReader (file, sourceStream);
        // The constructor leaves numChannels == 0 on any failure.
        if (r->numChannels == 0)
        {   delete r;   // also deletes the stream it owns
            return nullptr; }
        return r;
    }
};

//==============================================================================
// The manager's knownFormats is an OwnedArray<AudioFormat>: it deletes every
// registered format on destruction, so the instance must be heap-allocated.
void registerVideoFormats (juce::AudioFormatManager& fm)
{
    fm.registerFormat (new VideoAudioFormat(), false);
}

} // namespace sc

#elif defined(__APPLE__)   // macOS: AVFoundation reader (src/VideoAudio_mac.mm)

namespace sc {
juce::AudioFormat* createAvfAudioFormat();   // src/VideoAudio_mac.mm

const juce::String& videoContainerExtensions()
{
    // What AVFoundation opens. .m4a stays with JUCE's CoreAudio format.
    static const juce::String exts = ".mp4 .mov .m4v .3gp .3g2";
    return exts;
}

void registerVideoFormats (juce::AudioFormatManager& fm)
{
    fm.registerFormat (createAvfAudioFormat(), false);
}
} // namespace sc

#else   // !SC_ENABLE_FFMPEG — no FFmpeg available on this platform/build.

namespace sc {
const juce::String& videoContainerExtensions()
{
    static const juce::String none;   // empty: nothing extra can be opened
    return none;
}

void registerVideoFormats (juce::AudioFormatManager& fm)
{
    (void) fm;
}
} // namespace sc

#endif

// ---------------------------------------------------------------------------
// "Sync based on Audio": two-stage normalized cross-correlation. Pure math on
// decoded float channels - platform independent (FFmpeg, AVFoundation or a
// test-generated vector all feed the same code).

namespace sc {

std::vector<float> decimateTo (const std::vector<float>& in, double inRate,
                               double outRate)
{
    if (in.empty() || outRate >= inRate) return in;
    const double ratio = inRate / outRate;
    const int64_t nOut = (int64_t) std::floor ((double) in.size() / ratio);
    std::vector<float> out ((size_t) nOut, 0.0f);
    for (int64_t i = 0; i < nOut; ++i)
    {
        const int64_t a = (int64_t) std::floor ((double) i * ratio);
        const int64_t b = (int64_t) std::ceil ((double) (i + 1) * ratio);
        if (b <= a)
        {   out[(size_t) i] = in[(size_t) std::min (a, (int64_t) in.size () - 1)]; continue; }
        double s = 0.0;
        int n = 0;
        for (int64_t j = std::max<int64_t> (0, a); j < std::min<int64_t> ((int64_t) in.size (), b); ++j)
        {   s += in[(size_t) j]; ++n; }
        out[(size_t) i] = n > 0 ? (float) (s / n) : 0.0f;
    }
    return out;
}

namespace
{
/** Pearson correlation of two equal-length segments (-1e308 when either is
    silent: a flat segment carries no alignment information). */
double pearsonSeg (const float* a, const float* b, int64_t n)
{
    if (n < 8) return -1e308;
    double ma = 0.0, mb = 0.0;
    for (int64_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= (double) n; mb /= (double) n;
    double num = 0.0, da = 0.0, db = 0.0;
    for (int64_t i = 0; i < n; ++i)
    {
        const double x = a[i] - ma, y = b[i] - mb;
        num += x * y; da += x * x; db += y * y;
    }
    if (da <= 1e-12 || db <= 1e-12) return -1e308;
    return num / std::sqrt (da * db);
}

constexpr double kSyncDecimRate = 2000.0;   // coarse stage resolution, Hz
} // namespace

double videoSyncCoarse (const std::vector<float>& docDs, const std::vector<float>& vidDs,
                        double* stepOut)
{
    if (stepOut != nullptr) *stepOut = 0.0;
    if (docDs.size() < 64 || vidDs.size() < 64) return std::nan ("");

    const int64_t Dn = (int64_t) docDs.size(), Vn = (int64_t) vidDs.size();
    const double Ds = (double) Dn / kSyncDecimRate, Vs = (double) Vn / kSyncDecimRate;

    // Coarse grid: the video's start may sit anywhere from -Vs to +Ds.
    const double span = Ds + Vs;
    const double step = std::max (0.5, span / 400.0);
    if (stepOut != nullptr) *stepOut = step;

    // Correlation window: up to 3 s of overlap, at least a few samples.
    const int64_t Wn = std::min<int64_t> ((int64_t) (3.0 * kSyncDecimRate),
                                         std::max<int64_t> (16, (int64_t) (std::min (Ds, Vs) * 0.9 * kSyncDecimRate)));

    double best = -1e308, bestLag = 0.0;
    for (double L = -Vs; L <= Ds + step * 0.5; L += step)
    {
        // Overlap of doc [max(0,L) .. min(D, L+V)] with vid [max(0,-L) .. ].
        const double s = std::max (0.0, L);
        const double e = std::min (Ds, L + Vs);
        if (e - s < 0.016) continue;   // less than ~32 samples: no information
        const int64_t i0 = (int64_t) std::llround (s * kSyncDecimRate);
        const int64_t j0 = (int64_t) std::llround (std::max (0.0, -L) * kSyncDecimRate);
        const int64_t n  = std::min<int64_t> (Wn, (int64_t) std::floor ((e - s) * kSyncDecimRate));
        if (i0 + n > Dn || j0 + n > Vn) continue;
        const double r = pearsonSeg (docDs.data () + i0, vidDs.data () + j0, n);
        if (r > best) { best = r; bestLag = L; }
    }

    if (best < 0.15) return std::nan ("");   // nothing correlates meaningfully
    return bestLag;
}

double videoSyncFine (const std::vector<float>& docWin, double docRate,
                      double docWinStart,
                      const std::vector<float>& vidWin, double vidRate,
                      double coarseLag, double step)
{
    if (docRate <= 0.0 || vidRate <= 0.0 || step <= 0.0
        || docWin.empty() || vidWin.empty())
        return std::nan ("");

    const int64_t Dn = (int64_t) docWin.size(), Vn = (int64_t) vidWin.size();
    // Document times covered by the two windows.
    const double dLo = docWinStart, dHi = docWinStart + (double) Dn / docRate;
    const double vHi = (double) Vn / vidRate;

    // Fine grid: ~1/64 of the coarse step, never coarser than one sample.
    const double fStep = std::max (1.0 / docRate, step / 64.0);
    const int64_t Wn = std::min<int64_t> ((int64_t) (5.0 * docRate),
                                         std::max<int64_t> (8,
                                             (int64_t) (std::min (dHi, vHi) * 0.9 * docRate)));

    double best = -1e308, bestLag = coarseLag;
    for (double L = coarseLag - step; L <= coarseLag + step + fStep * 0.5; L += fStep)
    {
        // Overlap of doc [max(dLo,L) .. min(dHi, L+vHi)] with vid [max(vLo,-L) .. ].
        const double s = std::max (dLo, L);
        const double e = std::min (dHi, L + vHi);
        if (e - s < 8.0 / docRate) continue;
        const int64_t i0 = (int64_t) std::llround ((s - dLo) * docRate);
        const int64_t j0 = (int64_t) std::llround ((s - L) * vidRate);   // s >= L always
        const int64_t n  = std::min<int64_t> (Wn, (int64_t) std::floor ((e - s) * docRate));
        if (i0 + n > Dn || j0 + n > Vn) continue;
        const double r = pearsonSeg (docWin.data () + i0, vidWin.data () + j0, n);
        if (r > best) { best = r; bestLag = L; }
    }

    return best < 0.15 ? std::nan ("") : bestLag;
}

} // namespace sc
