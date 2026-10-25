/*
 * sc_extract_audio - extract the audio of any video/audio container to a
 * 32-bit float WAV, using the vendored minimal FFmpeg DLLs (extern/ffmpeg).
 *
 * This is both a usable command-line helper and the reference for the API
 * calls SpectroCraft needs (open -> pick audio stream -> decode -> resample
 * to interleaved float -> write WAV).
 *
 *   sc_extract_audio --list <input>
 *   sc_extract_audio [--progress] [--stream N] <input> <output.wav>
 *
 * N is the ordinal of the audio stream (0 = first audio stream, see --list).
 * Output keeps the source sample rate and channel count. Files larger than
 * 4 GB are written as RF64 (JUCE's WAV reader understands RF64).
 *
 * Licence: AGPL-3.0 like the rest of SpectroCraft. Links dynamically against
 * FFmpeg (LGPL-2.1-or-later).
 *
 * Exit codes: 0 ok, 1 usage, 2 cannot open input, 3 no such audio stream,
 *             4 decode error, 5 cannot write output.
 */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

/* ---------------------------------------------------------------- helpers */

#ifdef _WIN32
static char **utf8_argv(int *argc)
{
    int n = 0;
    LPWSTR *w = CommandLineToArgvW(GetCommandLineW(), &n);
    char **a = (char **)calloc((size_t)n + 1, sizeof(char *));
    for (int i = 0; i < n; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, NULL, 0, NULL, NULL);
        a[i] = (char *)malloc((size_t)len);
        WideCharToMultiByte(CP_UTF8, 0, w[i], -1, a[i], len, NULL, NULL);
    }
    *argc = n;
    LocalFree(w);
    return a;
}
#endif

static FILE *open_out(const char *utf8)
{
#ifdef _WIN32
    wchar_t w[32768];
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 32768)) return NULL;
    return _wfopen(w, L"wb");
#else
    return fopen(utf8, "wb");
#endif
}

static int seek64(FILE *f, int64_t off, int whence)
{
#ifdef _WIN32
    return _fseeki64(f, off, whence);
#else
    return fseeko(f, (off_t)off, whence);
#endif
}

static void p16(uint8_t *b, size_t *o, uint16_t v) { b[(*o)++] = (uint8_t)v; b[(*o)++] = (uint8_t)(v >> 8); }
static void p32(uint8_t *b, size_t *o, uint32_t v) { p16(b, o, (uint16_t)v); p16(b, o, (uint16_t)(v >> 16)); }
static void p64(uint8_t *b, size_t *o, uint64_t v) { p32(b, o, (uint32_t)v); p32(b, o, (uint32_t)(v >> 32)); }
static void ptag(uint8_t *b, size_t *o, const char *t) { memcpy(b + *o, t, 4); *o += 4; }

/*
 * Fixed-size header so it can be rewritten in place at the end:
 *   RIFF/RF64 | WAVE | JUNK/ds64 (28 bytes) | fmt | data header
 * If the data stays below 4 GB the JUNK chunk is left as padding and the file
 * is a normal WAV; otherwise it becomes ds64 and the file an RF64.
 */
static size_t write_header(FILE *f, int ch, int rate, uint64_t data_bytes, uint64_t frames)
{
    uint8_t b[160];
    size_t o = 0;
    const int ext = ch > 2;
    const uint32_t fmt_size = ext ? 40 : 16;
    const uint64_t hdr = 12 + 8 + 28 + 8 + fmt_size + 8;
    const uint64_t riff = hdr - 8 + data_bytes;
    const int rf64 = riff > 0xFFFFFFFFull;

    ptag(b, &o, rf64 ? "RF64" : "RIFF");
    p32(b, &o, rf64 ? 0xFFFFFFFFu : (uint32_t)riff);
    ptag(b, &o, "WAVE");

    ptag(b, &o, rf64 ? "ds64" : "JUNK");
    p32(b, &o, 28);
    p64(b, &o, rf64 ? riff : 0);
    p64(b, &o, rf64 ? data_bytes : 0);
    p64(b, &o, rf64 ? frames : 0);
    p32(b, &o, 0);

    ptag(b, &o, "fmt ");
    p32(b, &o, fmt_size);
    p16(b, &o, ext ? 0xFFFE : 3);                 /* WAVE_FORMAT_(EXTENSIBLE|IEEE_FLOAT) */
    p16(b, &o, (uint16_t)ch);
    p32(b, &o, (uint32_t)rate);
    p32(b, &o, (uint32_t)(rate * ch * 4));
    p16(b, &o, (uint16_t)(ch * 4));
    p16(b, &o, 32);
    if (ext) {
        static const uint8_t guid_float[16] = {3, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
        p16(b, &o, 22);                           /* cbSize */
        p16(b, &o, 32);                           /* valid bits */
        p32(b, &o, 0);                            /* channel mask: unspecified */
        memcpy(b + o, guid_float, 16);
        o += 16;
    }

    ptag(b, &o, "data");
    p32(b, &o, rf64 ? 0xFFFFFFFFu : (uint32_t)data_bytes);

    return fwrite(b, 1, o, f) == o ? o : 0;
}

/* A stream whose rate AND channel count are both unknown after probing cannot be
 * decoded (typically a misdetected elementary stream), so it is not offered. */
static int is_audio(const AVStream *st)
{
    const AVCodecParameters *p = st->codecpar;
    return p->codec_type == AVMEDIA_TYPE_AUDIO && (p->sample_rate > 0 || p->ch_layout.nb_channels > 0);
}

/* ------------------------------------------------------------ conversion */

typedef struct {
    SwrContext *swr;
    uint8_t *buf[1];
    int cap;
    int ch;
    int rate;
    uint64_t frames;
    FILE *out;
} Sink;

static int sink_write(Sink *s, const uint8_t **in, int in_count)
{
    int want = swr_get_out_samples(s->swr, in_count);
    if (want < 0) return want;
    if (want > s->cap) {
        av_freep(&s->buf[0]);
        int r = av_samples_alloc(s->buf, NULL, s->ch, want, AV_SAMPLE_FMT_FLT, 0);
        if (r < 0) return r;
        s->cap = want;
    }
    int got = swr_convert(s->swr, s->buf, s->cap, in, in_count);
    if (got < 0) return got;
    if (got > 0) {
        size_t bytes = (size_t)got * (size_t)s->ch * 4u;
        if (fwrite(s->buf[0], 1, bytes, s->out) != bytes) return AVERROR(EIO);
        s->frames += (uint64_t)got;
    }
    return got;
}

static int sink_init(Sink *s, const AVFrame *fr, int fallback_ch)
{
    AVChannelLayout in = {0}, out = {0};
    if (av_channel_layout_copy(&in, &fr->ch_layout) < 0) return -1;
    if (in.order == AV_CHANNEL_ORDER_UNSPEC || in.nb_channels <= 0)
        av_channel_layout_default(&in, in.nb_channels > 0 ? in.nb_channels : fallback_ch);
    s->ch = in.nb_channels;
    s->rate = fr->sample_rate;
    av_channel_layout_default(&out, s->ch);       /* same channel count, interleaved */
    int r = swr_alloc_set_opts2(&s->swr, &out, AV_SAMPLE_FMT_FLT, s->rate,
                                &in, (enum AVSampleFormat)fr->format, fr->sample_rate, 0, NULL);
    av_channel_layout_uninit(&in);
    av_channel_layout_uninit(&out);
    if (r < 0) return r;
    return swr_init(s->swr);
}

/* ------------------------------------------------------------------ main */

static int list_streams(AVFormatContext *fc)
{
    int n = 0;
    for (unsigned i = 0; i < fc->nb_streams; ++i) {
        const AVCodecParameters *p = fc->streams[i]->codecpar;
        if (!is_audio(fc->streams[i])) continue;
        AVDictionaryEntry *lang = av_dict_get(fc->streams[i]->metadata, "language", NULL, 0);
        double dur = fc->streams[i]->duration != AV_NOPTS_VALUE
                         ? fc->streams[i]->duration * av_q2d(fc->streams[i]->time_base)
                         : (fc->duration != AV_NOPTS_VALUE ? fc->duration / (double)AV_TIME_BASE : -1.0);
        printf("audio %d: codec=%s rate=%d channels=%d duration=%.3f lang=%s\n", n++,
               avcodec_get_name(p->codec_id), p->sample_rate, p->ch_layout.nb_channels, dur,
               lang ? lang->value : "-");
    }
    if (n == 0) printf("no audio streams\n");
    return 0;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    argv = utf8_argv(&argc);
#endif
    int list = 0, progress = 0, ordinal = 0;
    const char *in = NULL, *outp = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--progress")) progress = 1;
        else if (!strcmp(argv[i], "--stream") && i + 1 < argc) ordinal = atoi(argv[++i]);
        else if (!in) in = argv[i];
        else if (!outp) outp = argv[i];
    }
    if (!in || (!list && !outp)) {
        fprintf(stderr, "usage: %s --list <input>\n       %s [--progress] [--stream N] <input> <output.wav>\n", argv[0], argv[0]);
        return 1;
    }

    av_log_set_level(AV_LOG_ERROR);
    AVFormatContext *fc = NULL;
    if (avformat_open_input(&fc, in, NULL, NULL) < 0) { fprintf(stderr, "cannot open input\n"); return 2; }
    if (avformat_find_stream_info(fc, NULL) < 0) { fprintf(stderr, "cannot read stream info\n"); avformat_close_input(&fc); return 2; }
    if (list) { list_streams(fc); avformat_close_input(&fc); return 0; }

    int idx = -1, n = 0;
    for (unsigned i = 0; i < fc->nb_streams; ++i) {
        if (is_audio(fc->streams[i]) && n++ == ordinal) idx = (int)i;
        else fc->streams[i]->discard = AVDISCARD_ALL;       /* demuxer skips video etc. */
    }
    if (idx < 0) { fprintf(stderr, "no audio stream #%d\n", ordinal); avformat_close_input(&fc); return 3; }

    AVStream *st = fc->streams[idx];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *cc = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!cc || avcodec_parameters_to_context(cc, st->codecpar) < 0 || avcodec_open2(cc, codec, NULL) < 0) {
        fprintf(stderr, "no decoder for codec %s\n", avcodec_get_name(st->codecpar->codec_id));
        avcodec_free_context(&cc); avformat_close_input(&fc); return 3;
    }

    Sink s; memset(&s, 0, sizeof s);
    s.out = open_out(outp);
    if (!s.out) { fprintf(stderr, "cannot write output\n"); avcodec_free_context(&cc); avformat_close_input(&fc); return 5; }

    const double total = st->duration != AV_NOPTS_VALUE ? st->duration * av_q2d(st->time_base)
                       : (fc->duration != AV_NOPTS_VALUE ? fc->duration / (double)AV_TIME_BASE : 0.0);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    int rc = 0, last_pct = -1, flushing = 0;

    while (!flushing) {
        int r = av_read_frame(fc, pkt);
        if (r == AVERROR_EOF) { avcodec_send_packet(cc, NULL); flushing = 1; }
        else if (r < 0) { fprintf(stderr, "read error\n"); rc = 4; break; }
        else if (pkt->stream_index != idx) { av_packet_unref(pkt); continue; }
        else { r = avcodec_send_packet(cc, pkt); av_packet_unref(pkt); if (r < 0 && r != AVERROR_INVALIDDATA) { rc = 4; break; } }

        while ((r = avcodec_receive_frame(cc, fr)) >= 0) {
            if (!s.swr) {
                if (sink_init(&s, fr, cc->ch_layout.nb_channels > 0 ? cc->ch_layout.nb_channels : 2) < 0) { rc = 4; break; }
                if (!write_header(s.out, s.ch, s.rate, 0, 0)) { rc = 5; break; }
            }
            if (sink_write(&s, (const uint8_t **)fr->extended_data, fr->nb_samples) < 0) { rc = 5; break; }
            if (progress && total > 0 && s.rate > 0) {
                int pct = (int)(100.0 * ((double)s.frames / s.rate) / total);
                if (pct > 100) pct = 100;
                if (pct != last_pct) { printf("PROGRESS %d\n", pct); fflush(stdout); last_pct = pct; }
            }
            av_frame_unref(fr);
        }
        if (rc) break;
        if (r != AVERROR(EAGAIN) && r != AVERROR_EOF && r != AVERROR_INVALIDDATA) { rc = 4; break; }
    }

    if (!rc && s.swr) {                              /* drain the resampler */
        int got;
        while ((got = sink_write(&s, NULL, 0)) > 0) {}
        if (got < 0) rc = 5;
    }
    if (!rc && !s.swr) { fprintf(stderr, "stream contained no decodable audio\n"); rc = 4; }

    if (!rc) {
        const uint64_t bytes = s.frames * (uint64_t)s.ch * 4u;
        if (seek64(s.out, 0, SEEK_SET) != 0 || !write_header(s.out, s.ch, s.rate, bytes, s.frames)) rc = 5;
    }
    if (fclose(s.out) != 0 && !rc) rc = 5;
    if (rc) remove(outp);
    else if (progress) { printf("PROGRESS 100\n"); }

    av_freep(&s.buf[0]);
    swr_free(&s.swr);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&cc);
    avformat_close_input(&fc);
    return rc;
}
