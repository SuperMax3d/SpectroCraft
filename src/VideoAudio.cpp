/**
 * SpectroCraft - Video container audio extraction (FFmpeg, Windows)
 */

#include "VideoAudio.h"

#if defined(SC_ENABLE_FFMPEG) && SC_ENABLE_FFMPEG

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#include <cstring>
#include <vector>

namespace sc {

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

    // Length: prefer an explicit sample count when the container provides one
    // (AVI's strh dwLength and MP4/MKV track headers store exact counts; the
    // duration field is often only an estimate).
    if (st->nb_frames > 0)
        lengthInSamples = st->nb_frames;
    else
    {
        const int64_t dur_tb = (st->duration != AV_NOPTS_VALUE ? st->duration : fmt_->duration);
        if (dur_tb > 0)
            lengthInSamples = av_rescale_q (dur_tb, st->time_base, AVRational { 1, (int) sampleRate });
    }

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
    const int64_t ts = av_rescale_q (target, AVRational { 1, (int) sampleRate }, st->time_base);

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
class VideoAudioFormat : public juce::AudioFormat
{
public:
    VideoAudioFormat()
        : AudioFormat ("FFmpeg audio",
                       ".mp4 .m4a .m4b .3gp .3g2 .mov .mkv .webm .avi .wmv .asf .flv "
                       ".ts .m2ts .mts .ogv .wma .ac3 .eac3 .dts .dtshd .ape .wv .tta .amr .caf .mpc") {}

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

#else   // !SC_ENABLE_FFMPEG — no FFmpeg available on this platform/build.

namespace sc {
void registerVideoFormats (juce::AudioFormatManager& fm)
{
    (void) fm;
}
} // namespace sc

#endif
