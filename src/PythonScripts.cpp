// SpectroCraft - Python script panel support.
#include "PythonScripts.h"
#include "MarkerManager.h"
#include "AI/Sha256.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#if JUCE_WINDOWS
#  include <windows.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <signal.h>
#  include <poll.h>
#  include <cerrno>
#  include <climits>
#  include <cstdlib>
#  include <cstring>
#  if defined(__linux__)
#    include <sys/prctl.h>
#  endif
extern char** environ;
#endif

namespace sc { namespace py {

// ============================================================================
// Example scripts (standard library only - no pip packages required).
//
// SpectroCraft exchanges IEEE float32 WAV files, which Python's stdlib `wave`
// module cannot read (it only handles PCM). Each script therefore embeds a
// small RIFF/WAVE helper that reads float32 or PCM 16/24/32 and writes
// float32. The helpers are concatenated in at build time from kWavIo.
// ============================================================================

static const char* kWavIo = R"PY(
import array, struct


def read_wav(path):
    # Little-endian RIFF/WAVE reader: IEEE float32 or PCM 16/32.
    with open(path, "rb") as f:
        head = f.read(12)
        if len(head) < 12 or head[:4] != b"RIFF" or head[8:12] != b"WAVE":
            raise SystemExit("not a RIFF/WAVE file: %r" % path)
        tag, nch, rate, width = None, 0, 0, 0
        data = b""
        while True:
            hdr = f.read(8)
            if len(hdr) < 8:
                break
            cid, size = struct.unpack("<4sI", hdr)
            if cid == b"fmt ":
                body = f.read(size)
                tag, nch, rate = struct.unpack("<HHI", body[:8])
                bits = struct.unpack("<H", body[14:16])[0]
                width = bits // 8   # bytes per sample
            elif cid == b"data":
                data = f.read(size)
            else:
                f.seek(size + (size & 1), 1)
    if tag not in (1, 3):
        raise SystemExit("unsupported WAV format tag %r" % (tag,))
    n = len(data) // width
    if tag == 3:
        x = array.array("f")
        x.frombytes(data[:n * 4])
    else:
        code = {2: "h", 4: "i"}.get(width)
        if code is None:
            raise SystemExit("unsupported PCM bit depth %d" % width)
        raw = array.array(code)
        raw.frombytes(data[:n * width])
        scale = float(1 << (8 * width - 1))
        x = array.array("f", [v / scale for v in raw])
    return rate, nch, x


def write_wav(path, rate, nch, x):
    # Little-endian IEEE float32 RIFF/WAVE writer.
    data = x.tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 3, nch, rate, rate * nch * 4, nch * 4, 32))
        f.write(b"data" + struct.pack("<I", len(data)))
        f.write(data)
)PY";

static juce::String thresholdScript ()
{
    const std::string s = std::string (R"PY(
# SpectroCraft example: hard threshold (noise gate).
# Zeros every sample whose absolute value is below THRESHOLD.
#
# Called by SpectroCraft as:
#   python Threshold.py <input.wav> <output.wav> <meta.json>
# input.wav  - float32 WAV of the selected range (or whole file), all channels
# meta.json  - sample rate, channel count, selection times, markers, ...
# You must write output.wav with the SAME sample rate, channel count and length.

import sys

)PY") + kWavIo + R"PY(

THRESHOLD = 0.01   # linear amplitude (0..1); raise to gate more aggressively


def main():
    in_path, out_path, meta_path = sys.argv[1:4]
    rate, nch, x = read_wav(in_path)
    for i in range(len(x)):
        if abs(x[i]) < THRESHOLD:
            x[i] = 0.0
    write_wav(out_path, rate, nch, x)


main()
)PY";
    return juce::String (s.c_str ());
}

static juce::String dynamicProcessingScript ()
{
    const std::string s = std::string (R"PY(
# SpectroCraft example: dynamic processing - a simple per-channel compressor.
# An envelope follower drives gain reduction above THRESHOLD_DB with the given
# RATIO; ATTACK_MS / RELEASE_MS shape the follower.
#
# Called by SpectroCraft as:
#   python "Dynamic processing.py" <input.wav> <output.wav> <meta.json>

import sys, math

)PY") + kWavIo + R"PY(

THRESHOLD_DB = -20.0   # gain reduction starts above this level (dBFS)
RATIO        = 4.0     # e.g. 4:1
ATTACK_MS    = 5.0
RELEASE_MS   = 100.0


def main():
    in_path, out_path, meta_path = sys.argv[1:4]
    rate, nch, x = read_wav(in_path)
    n = len(x) // nch
    out = array.array("f", [0.0]) * (n * nch)

    for ch in range(nch):
        env = 0.0
        rel = math.exp(-1.0 / max(1e-3, RELEASE_MS * rate / 1000.0))
        atk = math.exp(-1.0 / max(1e-3, ATTACK_MS * rate / 1000.0))
        for i in range(n):
            s = x[ch + i * nch]
            mag = abs(s)
            coef = atk if mag > env else rel
            env = coef * env + (1.0 - coef) * mag
            level_db = 20.0 * math.log10(env + 1e-9)
            if level_db > THRESHOLD_DB:
                gr_db = (level_db - THRESHOLD_DB) * (1.0 - 1.0 / RATIO)
                g = 10.0 ** (-gr_db / 20.0)
            else:
                g = 1.0
            out[ch + i * nch] = s * g

    write_wav(out_path, rate, nch, out)


main()
)PY";
    return juce::String (s.c_str ());
}

static juce::String modulationScript ()
{
    const std::string s = std::string (R"PY(
# SpectroCraft example: modulation - ring modulation plus optional tremolo.
#   y = x * sin(2*pi*RM_HZ*t) * (1 - TREM_DEPTH/2 + TREM_DEPTH/2*sin(2*pi*TREM_HZ*t))
#
# Called by SpectroCraft as:
#   python Modulation.py <input.wav> <output.wav> <meta.json>

import sys, math

)PY") + kWavIo + R"PY(

RM_HZ      = 50.0    # ring-modulator frequency (Hz)
TREM_HZ    = 4.0     # tremolo (amplitude LFO) rate (Hz)
TREM_DEPTH = 0.5     # 0..1


def main():
    in_path, out_path, meta_path = sys.argv[1:4]
    rate, nch, x = read_wav(in_path)
    n = len(x) // nch
    out = array.array("f", [0.0]) * (n * nch)

    for i in range(n):
        t = i / rate
        rm = math.sin(2.0 * math.pi * RM_HZ * t)
        trem = 1.0 - TREM_DEPTH / 2.0 + TREM_DEPTH / 2.0 * math.sin(2.0 * math.pi * TREM_HZ * t)
        m = rm * trem
        for ch in range(nch):
            out[ch + i * nch] = x[ch + i * nch] * m

    write_wav(out_path, rate, nch, out)


main()
)PY";
    return juce::String (s.c_str ());
}

static juce::String echoScript ()
{
    const std::string s = std::string (R"PY(
# SpectroCraft example: add echo.
# Appends decaying repeats of the input DELAY_MS apart:
#   y[n] = x[n] + ECHO_GAIN * sum_k FEEDBACK^k * x[n - k*DELAY_SAMPLES]
# Samples before the start of the processed range are treated as zero, so
# early echoes are partial. The dry signal is kept at full level.
#
# Called by SpectroCraft as:
#   python "Add Echo.py" <input.wav> <output.wav> <meta.json>

import sys

)PY") + kWavIo + R"PY(

DELAY_MS  = 120.0   # time between repeats (ms)
FEEDBACK  = 0.4     # each repeat is this fraction of the previous one (0..1)
ECHO_GAIN = 0.8     # overall level of the echo tail


def main():
    in_path, out_path, meta_path = sys.argv[1:4]
    rate, nch, x = read_wav(in_path)
    n = len(x) // nch
    delay = int(round(DELAY_MS * rate / 1000.0))
    if delay <= 0:
        write_wav(out_path, rate, nch, x)   # nothing to echo
        return

    out = array.array("f", [0.0]) * (n * nch)
    for ch in range(nch):
        for i in range(n):
            v = x[ch + i * nch]
            g = ECHO_GAIN
            j = i - delay
            while j >= 0:
                v += g * x[ch + j * nch]
                g *= FEEDBACK
                if abs(g) < 1e-6:
                    break
                j -= delay
            out[ch + i * nch] = v

    write_wav(out_path, rate, nch, out)


main()
)PY";
    return juce::String (s.c_str ());
}

// Template written by the panel's Add button.
static juce::String newScriptTemplateText ()
{
    const std::string s = std::string (R"PY(
# SpectroCraft Python script
#
# Called as:  python <this file> <input.wav> <output.wav> <meta.json>
#   input.wav  - float32 WAV of the selected range (or whole file), all channels
#   meta.json  - sample rate, channel count, selection times, markers, ...
# To change the audio, write output.wav with the SAME sample rate and channel
# count (the length may differ).
#
# Optional structured result: write a JSON object to meta["resultJson"]
# (output.wav then becomes optional - an analysis script may write only this).
# It is returned to control-API callers (python.run); this panel does not show
# it yet. No NaN/Infinity (not JSON): use None. Format:
#   {"version": 1,
#    "timeBase": "input",       # "input": seconds from the start of input.wav
#                               # "document": seconds from the start of the file
#    "markers": [{"time": 1.5, "end": 2.0, "label": "peak"}],   # end optional
#    "text": "free text for the caller",
#    "measures": {"rms_db": -12.3},                             # any JSON object
#    "tables": [{"name": "bands", "columns": ["f", "dB"], "rows": [[1000, -3.1]]}]}

import json, sys

)PY") + kWavIo + R"PY(


def main():
    in_path, out_path, meta_path = sys.argv[1:4]
    with open(meta_path, encoding="utf-8") as f:
        meta = json.load(f)
    rate, nch, x = read_wav(in_path)

    # TODO: process the samples here (x is a channel-interleaved float32 array)

    write_wav(out_path, rate, nch, x)

    # Optional: return markers / text / measures as well.
    # with open(meta["resultJson"], "w", encoding="utf-8") as f:
    #     json.dump({"version": 1, "text": "done"}, f)


main()
)PY";
    return juce::String (s.c_str ());
}

static juce::String syncAndMergeScript ()
{
    const std::string s = std::string (R"PY(
# SpectroCraft example: sync and merge repeated signals (stacking).
#
# The recording contains N_REPETITIONS copies of the same signal, one after
# another (for example a hammer tap recorded four times in a single take).
# The first copy is used as reference; cross-correlation finds where every
# other copy starts, all copies are aligned sample-accurately and mixed
# together. Averaging N copies reduces uncorrelated noise by about
# 10*log10(sqrt(N)) dB (roughly 3 dB for 4 copies).
#
# Called by SpectroCraft as:
#   python "Sync And Merge.py" <input.wav> <output.wav> <meta.json>
# The output is ONE aligned copy, so it is much shorter than the input.

import sys, math

)PY") + kWavIo + R"PY(

N_REPETITIONS      = 4        # how many copies of the signal are in the file
SEARCH_MARGIN_FRAC = 0.45     # search +/- this fraction of a segment around each expected position
MIX_MODE           = "average"   # "average" (noise reduction) or "sum"

COARSE_RATE = 1000           # internal rate for the coarse correlation stage


def next_pow2(n):
    p = 1
    while p < n:
        p <<= 1
    return p


def fft(x):
    """In-place iterative radix-2 complex FFT; len(x) must be a power of two."""
    n = len(x)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            x[i], x[j] = x[j], x[i]
    length = 2
    while length <= n:
        ang = -2.0 * math.pi / length
        wlen = complex(math.cos(ang), math.sin(ang))
        half = length >> 1
        for i in range(0, n, length):
            w = 1.0 + 0.0j
            for k in range(i, i + half):
                u = x[k]
                v = x[k + half] * w
                x[k] = u + v
                x[k + half] = u - v
                w *= wlen
        length <<= 1


def ifft(x):
    n = len(x)
    for i in range(n):
        x[i] = x[i].conjugate()
    fft(x)
    for i in range(n):
        x[i] = x[i].conjugate() / n


def main():
    in_path, out_path, meta_path = sys.argv[1:4]
    rate, nch, x = read_wav(in_path)
    T = len(x) // nch

    N = max(2, int(N_REPETITIONS))
    L = T // N                       # nominal segment length (samples)
    if L < 64:
        raise SystemExit("file too short for %d repetitions" % N)

    # Mono mixdown at full rate.
    mono = [0.0] * T
    for i in range(T):
        base = i * nch
        s = 0.0
        for c in range(nch):
            s += x[base + c]
        mono[i] = s / nch

    # Coarse stage: decimate to COARSE_RATE by block averaging, then find each
    # instance with one FFT-based cross-correlation against the first segment.
    k = max(1, int(round(rate / float(COARSE_RATE))))
    Tc = (T + k - 1) // k
    mc = [0.0] * Tc
    for i in range(Tc):
        a = i * k
        b = min(a + k, T)
        s = 0.0
        for j in range(a, b):
            s += mono[j]
        mc[i] = s / (b - a)

    Lc = max(16, (L + k - 1) // k)   # coarse reference length
    S = next_pow2(Tc + Lc - 1)
    if S > (1 << 20):
        raise SystemExit("recording too long for the built-in correlator; "
                         "select just the region containing the repetitions")

    A = [complex(v, 0.0) for v in mc[:Lc]] + [0j] * (S - Lc)
    B = [complex(v, 0.0) for v in mc]     + [0j] * (S - Tc)
    fft(A)
    fft(B)
    P = [A[i] * B[i].conjugate() for i in range(S)]
    ifft(P)
    # ifft(A*conj(B))[k] carries lag -k, so the positive-lag correlation is
    # read back mirrored: C[s] = P[(S-s) mod S].
    C = [P[(S - s) % S].real for s in range(Tc - Lc + 1)]   # C[s]: ref vs file shifted by s

    # Instance 0 is the reference at position 0; find the rest around their
    # nominal positions (i * L).
    m = int(Lc * SEARCH_MARGIN_FRAC)
    starts = [0]
    for i in range(1, N):
        lo = max(0, i * Lc - m)
        hi = min(len(C) - 1, i * Lc + m)
        best, bv = lo, None
        for s in range(lo, hi + 1):
            v = C[s]
            if bv is None or v > bv:
                best, bv = s, v
        starts.append(best * k)

    # Fine stage: refine each start with direct correlation at full rate.
    ref = mono[:L]
    for i in range(1, N):
        c0 = starts[i]
        lo = max(0, c0 - k)
        hi = min(T - L, c0 + 2 * k)
        best, bv = lo, None
        for s in range(lo, hi + 1):
            v = 0.0
            for j in range(L):
                v += ref[j] * mono[s + j]
            if bv is None or v > bv:
                best, bv = s, v
        starts[i] = best

    # Mix all aligned copies into one output of length L.
    g = 1.0 / N if MIX_MODE != "sum" else 1.0
    out = [0.0] * (L * nch)
    for i in range(N):
        st = starts[i]
        for j in range(L):
            bi = (st + j) * nch
            bo = j * nch
            for c in range(nch):
                out[bo + c] += x[bi + c] * g

    write_wav(out_path, rate, nch, array.array("f", out))


main()
)PY";
    return juce::String (s.c_str ());
}

static juce::String makeLoopScript ()
{
    const std::string s = std::string (R"PY(
# SpectroCraft example: make the file loop seamlessly.
#
# Takes the last PCT percent of the file and overlaps it with the first PCT
# percent, crossfading them together (the tail fades out while the head fades
# in). The result is shorter by exactly that overlap, and when played on
# loop there is no hard cut at the end: the ending flows straight into the
# beginning.
#
# Called by SpectroCraft as:
#   python "Make Loop.py" <input.wav> <output.wav> <meta.json>

import sys, math

)PY") + kWavIo + R"PY(

PCT  = 20            # percent of duration to overlap (1..50)
FADE = "equal-power" # or "linear"


def main():
    in_path, out_path, meta_path = sys.argv[1:4]
    rate, nch, x = read_wav(in_path)
    T = len(x) // nch

    pct = min(50.0, max(1.0, float(PCT)))
    O = int(round(T * pct / 100.0))
    if O < 2:
        raise SystemExit("file too short to overlap")
    O = min(O, T // 2)

    head = x[:O * nch]
    tail = x[(T - O) * nch:]
    mid  = x[O * nch:(T - O) * nch]

    out = array.array("f")
    if FADE == "linear":
        for i in range(O):
            w1 = float(i) / (O - 1)      # head weight: 0 -> 1
            w2 = 1.0 - w1                # tail weight: 1 -> 0
            b = i * nch
            for c in range(nch):
                out.append(head[b + c] * w1 + tail[b + c] * w2)
    else:   # equal-power (sine/cosine) crossfade
        for i in range(O):
            a = math.pi * float(i) / (2.0 * (O - 1))
            w1, w2 = math.sin(a), math.cos(a)
            b = i * nch
            for c in range(nch):
                out.append(head[b + c] * w1 + tail[b + c] * w2)
    out.extend(mid)

    write_wav(out_path, rate, nch, out)


main()
)PY";
    return juce::String (s.c_str ());
}

// ============================================================================
// Script folder management
// ============================================================================

juce::File scriptFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                .getChildFile ("SpectroCraft")
                .getChildFile ("python");
}

// The six built-in example scripts: file name -> original body. Shared by
// ensureExampleScripts() and defaultScriptText().
static std::vector<std::pair<juce::String, juce::String>> exampleBodies()
{
    return {
        { "Dynamic processing.py",  dynamicProcessingScript () },
        { "Threshold.py",           thresholdScript () },
        { "Modulation.py",          modulationScript () },
        { "Add Echo.py",             echoScript () },
        { "Sync And Merge.py",       syncAndMergeScript () },
        { "Make Loop.py",            makeLoopScript () }
    };
}

void ensureExampleScripts (const juce::File& dir)
{
    if (!dir.exists())
        dir.createDirectory ();

    for (const auto& e : exampleBodies())
    {
        const juce::File f = dir.getChildFile (e.first);
        if (!f.existsAsFile())
            f.replaceWithText (e.second);   // never overwrite an existing script
    }
}

juce::String defaultScriptText (const juce::String& fileName)
{
    for (const auto& e : exampleBodies())
        if (e.first == fileName) return e.second;
    return {};
}

juce::String newScriptTemplate() { return newScriptTemplateText (); }

std::vector<juce::File> listScripts (const juce::File& dir)
{
    std::vector<juce::File> out;
    if (!dir.isDirectory()) return out;

    for (const auto& e : dir.findChildFiles (juce::File::findFiles, false, "*.py"))
        out.push_back (e);

    std::sort (out.begin (), out.end (),
               [] (const juce::File& a, const juce::File& b) {
                   return a.getFileName ().toLowerCase () < b.getFileName ().toLowerCase ();
               });
    return out;
}

juce::StringArray splitCommand (const std::string& cmd)
{
    juce::StringArray tokens = juce::StringArray::fromTokens (
        juce::String::fromUTF8 (cmd.c_str ()).trim (), true);
    for (auto& t : tokens)
        t = t.unquoted ();
    tokens.removeEmptyStrings ();
    return tokens;
}

// ============================================================================
// Metadata JSON
// ============================================================================

juce::String buildMetaJson (double sampleRate, int numChannels, double fileDurationSec,
                            juce::Range<double> selectionSecs, bool wholeFile,
                            const juce::String& fileName,
                            const std::vector<Marker>& markers,
                            const ScriptExchange* exchange)
{
    auto root = new juce::DynamicObject ();
    root->setProperty ("sampleRate",        (juce::int64) sampleRate);
    root->setProperty ("numChannels",       numChannels);
    root->setProperty ("fileDurationSec",   fileDurationSec);
    root->setProperty ("selectionStartSec", selectionSecs.getStart ());
    root->setProperty ("selectionEndSec",   selectionSecs.getEnd ());
    root->setProperty ("wholeFile",         wholeFile);
    root->setProperty ("fileName",          fileName);

    juce::Array<juce::var> markerArray;
    for (const auto& m : markers)
    {
        auto mj = new juce::DynamicObject ();
        mj->setProperty ("name",    m.name);
        mj->setProperty ("comment", m.comment);
        mj->setProperty ("start",   m.start);
        // Point markers have end == -1 in the sidecar format; keep that.
        // Region ends are seconds, like start: keep the fractional part.
        mj->setProperty ("end",     (m.end < 0.0) ? juce::var ((juce::int64) -1)
                                                   : juce::var (m.end));
        markerArray.add (juce::var (mj));
    }
    root->setProperty ("markers", juce::var (markerArray));

    if (exchange != nullptr)
    {
        root->setProperty ("inputWav",       exchange->inWav.getFullPathName ());
        root->setProperty ("outputWav",      exchange->outWav.getFullPathName ());
        root->setProperty ("resultJson",     exchange->resultJson.getFullPathName ());
        root->setProperty ("resultContract", resultContract ());
    }

    return juce::JSON::toString (juce::var (root), true);
}

// ============================================================================
// Exchange files
// ============================================================================

void ScriptExchange::cleanup() const
{
    inWav.deleteFile ();
    outWav.deleteFile ();
    metaJson.deleteFile ();
    resultJson.deleteFile ();
}

ScriptExchange makeExchange (const juce::File& dir, const juce::String& stamp, double inputStartSec)
{
    if (!dir.exists ())
        dir.createDirectory ();
    ScriptExchange ex;
    ex.inWav      = dir.getChildFile ("in_"     + stamp + ".wav");
    ex.outWav     = dir.getChildFile ("out_"    + stamp + ".wav");
    ex.metaJson   = dir.getChildFile ("meta_"   + stamp + ".json");
    ex.resultJson = dir.getChildFile ("result_" + stamp + ".json");
    ex.inputStartSec = inputStartSec;
    ex.cleanup ();   // a stale file must never pass for this run's output
    return ex;
}

// ============================================================================
// result.json contract (version 1)
// ============================================================================

juce::var resultContract()
{
    auto c = new juce::DynamicObject ();
    c->setProperty ("version", 1);
    c->setProperty ("file", "Optional. Write one JSON object (UTF-8) to the path in \"resultJson\". "
                            "When it is written, outputWav becomes optional.");
    c->setProperty ("timeBase", "\"input\" (default: seconds from the start of inputWav) or "
                                "\"document\" (seconds from the start of the file).");
    c->setProperty ("markers", "[{\"time\": seconds, \"end\": seconds (optional, >= time; "
                               "omit, null or -1 for a point), \"label\": string (optional)}]");
    c->setProperty ("text", "string (optional)");
    c->setProperty ("measures", "object (optional, free-form)");
    c->setProperty ("tables", "[{\"name\": string (optional), \"columns\": [string], "
                              "\"rows\": [[value per column]]}] (optional)");
    return juce::var (c);
}

const char* resultContractText()
{
    return R"TXT(Script result contract (result.json, version 1)

A script is called as:  python <script> <input.wav> <output.wav> <meta.json>

- output.wav (optional): float32 WAV with the input's sample rate and channel
  count; it replaces the processed range (its length may differ).
- result.json (optional): a JSON object written to meta["resultJson"]:
    {
      "version": 1,
      "timeBase": "input",        // or "document"; "input" = seconds from the
                                  // start of input.wav (meta["selectionStartSec"]
                                  // in document time)
      "markers": [ {"time": 1.25, "end": 2.0, "label": "climb"},
                   {"time": 3.5, "label": "click"} ],    // end optional
      "text": "Peak level -3.1 dBFS at 1.25 s",
      "measures": { "rms_db": -18.2, "peak_db": -3.1 },  // any JSON object
      "tables": [ {"name": "bands", "columns": ["Hz", "dB"],
                   "rows": [[1000, -20.5], [2000, -22.0]]} ]
    }
  Every field is optional; unknown fields and null values are ignored. A
  script may write result.json without output.wav (analysis only). An
  invalid result.json fails the run and leaves the document unchanged.
  NaN / Infinity are not JSON: write null instead (json.dump(...,
  allow_nan=False) catches them). The result is returned to control-API
  callers (python.run); the Python panel does not display it yet.
- The script must exit with code 0. stdin is empty (EOF); stdout/stderr are
  captured as UTF-8.)TXT";
}

namespace {

bool isNumber (const juce::var& v)
{
    return v.isInt () || v.isInt64 () || v.isDouble ();
}

} // namespace

ScriptResult parseScriptResult (const juce::String& json, double inputStartSec)
{
    auto invalid = [] (const juce::String& msg)
    {
        ScriptResult e;
        e.present = true;
        e.error = msg;
        return e;
    };

    juce::var v;
    const juce::Result parsed = juce::JSON::parse (json, v);
    if (parsed.failed ())
        return invalid ("not valid JSON (" + parsed.getErrorMessage () + ")");
    auto* obj = v.getDynamicObject ();
    if (obj == nullptr || v.isArray ())
        return invalid ("the top level must be a JSON object");

    ScriptResult r;
    r.present = true;
    r.raw = v;

    // A top-level null (Python None) counts as absent, like nested ones.
    auto has = [obj] (const char* key)
    {
        const juce::var& x = obj->getProperty (key);
        return obj->hasProperty (key) && !x.isVoid () && !x.isUndefined ();
    };

    if (has ("version"))
    {
        const juce::var ver = obj->getProperty ("version");
        if (!isNumber (ver) || (double) ver != 1.0)
            return invalid ("unsupported version " + ver.toString () + " (expected 1)");
    }

    if (has ("timeBase"))
    {
        const juce::var tb = obj->getProperty ("timeBase");
        if (!tb.isString () || (tb.toString () != "input" && tb.toString () != "document"))
            return invalid ("\"timeBase\" must be \"input\" or \"document\"");
        r.timeBase = tb.toString ();
    }
    const double offset = r.timeBase == "input" ? inputStartSec : 0.0;

    if (has ("markers"))
    {
        const juce::var ms = obj->getProperty ("markers");
        if (!ms.isArray ())
            return invalid ("\"markers\" must be an array");
        if (ms.size () > 100000)
            return invalid ("too many markers (" + juce::String (ms.size ()) + ", at most 100000)");
        for (int i = 0; i < ms.size (); ++i)
        {
            const juce::var m = ms[i];
            const juce::String where = "markers[" + juce::String (i) + "]";
            if (m.getDynamicObject () == nullptr || m.isArray ())
                return invalid (where + " must be an object");
            const juce::var t = m.getProperty ("time", juce::var ());
            if (!isNumber (t) || !std::isfinite ((double) t) || (double) t < 0.0)
                return invalid (where + ".time must be a finite number >= 0");
            ScriptMarker sm;
            sm.time = (double) t;
            const juce::var e = m.getProperty ("end", juce::var ());
            if (!e.isVoid () && !e.isUndefined ())
            {
                if (!isNumber (e) || !std::isfinite ((double) e))
                    return invalid (where + ".end must be a finite number");
                const double ev = (double) e;
                if (ev >= 0.0 && ev < sm.time)
                    return invalid (where + ".end is before its time");
                sm.end = ev >= 0.0 ? ev : -1.0;   // negative (e.g. -1) = point marker
            }
            const juce::var l = m.getProperty ("label", juce::var ());
            if (!l.isVoid () && !l.isUndefined ())
            {
                if (!l.isString ())
                    return invalid (where + ".label must be a string");
                sm.label = l.toString ();
            }
            sm.docTime = sm.time + offset;
            sm.docEnd  = sm.end >= 0.0 ? sm.end + offset : -1.0;
            r.markers.push_back (sm);
        }
    }

    if (has ("text"))
    {
        const juce::var t = obj->getProperty ("text");
        if (!t.isString ())
            return invalid ("\"text\" must be a string");
        r.text = t.toString ();
    }

    if (has ("measures"))
    {
        const juce::var ms = obj->getProperty ("measures");
        if (ms.getDynamicObject () == nullptr || ms.isArray ())
            return invalid ("\"measures\" must be an object");
        r.measures = ms;
    }

    if (has ("tables"))
    {
        const juce::var ts = obj->getProperty ("tables");
        if (!ts.isArray ())
            return invalid ("\"tables\" must be an array");
        for (int i = 0; i < ts.size (); ++i)
        {
            const juce::var t = ts[i];
            const juce::String where = "tables[" + juce::String (i) + "]";
            if (t.getDynamicObject () == nullptr || t.isArray ())
                return invalid (where + " must be an object");
            ScriptTable st;
            const juce::var name = t.getProperty ("name", juce::var ());
            if (!name.isVoid () && !name.isUndefined ())
            {
                if (!name.isString ())
                    return invalid (where + ".name must be a string");
                st.name = name.toString ();
            }
            const juce::var cols = t.getProperty ("columns", juce::var ());
            if (!cols.isArray () || cols.size () == 0)
                return invalid (where + ".columns must be a non-empty array of strings");
            for (int c = 0; c < cols.size (); ++c)
            {
                if (!cols[c].isString ())
                    return invalid (where + ".columns must be a non-empty array of strings");
                st.columns.add (cols[c].toString ());
            }
            const juce::var rows = t.getProperty ("rows", juce::var ());
            if (!rows.isVoid () && !rows.isUndefined ())
            {
                if (!rows.isArray ())
                    return invalid (where + ".rows must be an array of arrays");
                for (int ri = 0; ri < rows.size (); ++ri)
                {
                    if (!rows[ri].isArray () || rows[ri].size () != st.columns.size ())
                        return invalid (where + ".rows[" + juce::String (ri) + "] must be an array of "
                                        + juce::String (st.columns.size ()) + " values");
                    st.rows.add (rows[ri]);
                }
            }
            r.tables.push_back (std::move (st));
        }
    }

    return r;
}

ScriptResult readScriptResult (const juce::File& file, double inputStartSec)
{
    if (!file.existsAsFile ())
        return {};
    if (file.getSize () > (int64_t) 16 * 1024 * 1024)
    {
        ScriptResult e;
        e.present = true;
        e.error = "larger than 16 MiB";
        return e;
    }
    return parseScriptResult (file.loadFileAsString (), inputStartSec);
}

// ============================================================================
// Script identity and location
// ============================================================================

juce::String scriptSha256 (const juce::File& script)
{
    juce::MemoryBlock mb;
    if (!script.existsAsFile () || !script.loadFileAsData (mb))
        return {};
    return juce::String (sc::ai::sha256Hex (mb.getData (), mb.getSize ()));
}

// The file a path finally designates, symbolic links (and, on Windows,
// junctions) resolved; the input itself when it cannot be resolved.
static juce::File resolveLinks (const juce::File& f)
{
#if JUCE_WINDOWS
    HANDLE h = CreateFileW (f.getFullPathName ().toWideCharPointer (), 0,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return f;
    std::vector<wchar_t> buf (32768);
    const DWORD n = GetFinalPathNameByHandleW (h, buf.data (), (DWORD) buf.size (),
                                               FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle (h);
    if (n == 0 || n >= (DWORD) buf.size ())
        return f;
    std::wstring s (buf.data (), n);
    if (s.rfind (L"\\\\?\\UNC\\", 0) == 0)  s = L"\\\\" + s.substr (8);
    else if (s.rfind (L"\\\\?\\", 0) == 0)  s = s.substr (4);
    return juce::File (juce::String (s.c_str ()));
#else
    char buf[PATH_MAX];
    if (realpath (f.getFullPathName ().toRawUTF8 (), buf) == nullptr)
        return f;
    return juce::File (juce::String::fromUTF8 (buf));
#endif
}

bool isUnderScriptFolder (const juce::File& script, juce::String* reasonOut, const juce::File& folder,
                          juce::File* resolvedOut)
{
    auto refuse = [reasonOut] (const char* reason)
    {
        if (reasonOut != nullptr) *reasonOut = reason;
        return false;
    };
    if (reasonOut != nullptr) reasonOut->clear ();
    if (resolvedOut != nullptr) *resolvedOut = juce::File ();

    if (!script.existsAsFile ())
        return refuse ("not_found");
    if (!script.hasFileExtension ("py"))
        return refuse ("not_python");

    const juce::File real    = resolveLinks (script);
    const juce::File realDir = resolveLinks (folder);
    if (!real.existsAsFile ())
        return refuse ("not_found");
    if (!real.hasFileExtension ("py"))   // "x.py" linking to a non-script file
        return refuse ("not_python");
    if (!folder.isDirectory () || !real.isAChildOf (realDir))
        return refuse ("outside_script_folder");
    if (resolvedOut != nullptr) *resolvedOut = real;
    return true;
}

// ============================================================================
// Float WAV I/O for the temp-file exchange
// ============================================================================

bool writeFloatWav (const juce::File& file, const std::vector<std::vector<float>>& data,
                    double sampleRate)
{
    if (data.empty () || data[0].empty ()) return false;

    const int nch = (int) data.size ();
    const int64_t n = (int64_t) data[0].size ();
    for (const auto& ch : data)
        if ((int64_t) ch.size () != n) return false;   // ragged input

    juce::AudioBuffer<float> buf (nch, (int) n);
    for (int c = 0; c < nch; ++c)
        std::copy (data[(size_t) c].begin (), data[(size_t) c].end (),
                   buf.getWritePointer (c));

    file.deleteFile ();   // FileOutputStream appends to existing files!
    auto fileStream = std::make_unique<juce::FileOutputStream> (file);
    if (fileStream->failedToOpen ()) return false;
    std::unique_ptr<juce::OutputStream> stream (std::move (fileStream));

    const juce::AudioFormatWriterOptions options =
        juce::AudioFormatWriterOptions {}
            .withNumChannels (nch)
            .withSampleRate (sampleRate)
            .withBitsPerSample (32)
            .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

    auto writer = juce::WavAudioFormat ().createWriterFor (stream, options);
    if (!writer) return false;
    return writer->writeFromAudioSampleBuffer (buf, 0, (int) n);
}

bool readFloatWav (const juce::File& file, double* rateOut, int* nchOut,
                   std::vector<std::vector<float>>& out)
{
    if (!file.existsAsFile ()) return false;

    juce::AudioFormatManager fm;
    fm.registerBasicFormats ();
    auto* r = fm.createReaderFor (file);
    if (r == nullptr) return false;

    const int nch = r->numChannels;
    const int64_t n = r->lengthInSamples;
    const double rate = r->sampleRate;
    if (nch <= 0 || n <= 0 || n > (int64_t) 0x7fffffff)
    { delete r; return false; }

    juce::AudioBuffer<float> buf (nch, (int) n);
    const bool ok = r->read (&buf, 0, (int) n, 0, true, true);
    delete r;
    if (!ok) return false;

    out.resize ((size_t) nch);
    for (int c = 0; c < nch; ++c)
        out[(size_t) c].assign (buf.getReadPointer (c), buf.getReadPointer (c) + n);

    if (rateOut != nullptr) *rateOut = rate;
    if (nchOut  != nullptr) *nchOut  = nch;
    return true;
}

// ============================================================================
// Process launching
// ============================================================================

juce::String decodeProcessOutput (const char* data, size_t size)
{
    if (data == nullptr || size == 0)
        return {};

    // A NUL byte would end the string early (JUCE strings are NUL-terminated):
    // replace each by U+FFFD (EF BF BD in UTF-8; valid in no ANSI code page,
    // so on that path it becomes '?' or similar, never a truncation).
    std::string cleaned;
    if (std::memchr (data, 0, size) != nullptr)
    {
        cleaned.reserve (size + 16);
        for (size_t i = 0; i < size; ++i)
        {
            if (data[i] == 0) cleaned += "\xEF\xBF\xBD";
            else              cleaned += data[i];
        }
        data = cleaned.data ();
        size = cleaned.size ();
    }

    const bool fitsInt = size <= (size_t) (std::numeric_limits<int>::max) ();
    if (fitsInt && juce::CharPointer_UTF8::isValidString (data, (int) size))
        return juce::String::fromUTF8 (data, (int) size);

#if JUCE_WINDOWS
    // Not UTF-8: a child that ignored PYTHONIOENCODING (or is not Python)
    // wrote in the ANSI code page.
    if (fitsInt)
    {
        const int n = MultiByteToWideChar (CP_ACP, 0, data, (int) size, nullptr, 0);
        if (n > 0)
        {
            std::wstring w ((size_t) n, L'\0');
            MultiByteToWideChar (CP_ACP, 0, data, (int) size, w.data (), n);
            return juce::String (w.c_str ());
        }
    }
#endif
    // Latin-1: every byte maps to one code point, nothing is lost.
    std::vector<juce::juce_wchar> u32 (size + 1, 0);
    for (size_t i = 0; i < size; ++i)
    {
        const auto b = (unsigned char) data[i];
        u32[i] = b == 0 ? (juce::juce_wchar) 0xFFFD : (juce::juce_wchar) b;
    }
    return juce::String (juce::CharPointer_UTF32 (u32.data ()));
}

// RunOptions::onStdoutLine: hands every complete line of bytes[from..] to cb
// and advances `from` past it; with flushPartial, also the unterminated tail.
static void deliverStdoutLines (const std::string& bytes, size_t& from,
                                const std::function<void (const juce::String&)>& cb,
                                bool flushPartial)
{
    if (cb == nullptr) return;
    for (;;)
    {
        const size_t nl = bytes.find ('\n', from);
        const bool tail = nl == std::string::npos;
        if (tail && (!flushPartial || from >= bytes.size ()))
            return;
        size_t end = tail ? bytes.size () : nl;
        const size_t next = tail ? bytes.size () : nl + 1;
        if (end > from && bytes[end - 1] == '\r') --end;
        if (end > from)
            cb (decodeProcessOutput (bytes.data () + from, end - from));
        from = next;
        if (tail) return;
    }
}

#if JUCE_WINDOWS

static std::wstring toWide (const juce::String& s)
{
    return std::wstring (s.toWideCharPointer ());
}

// Quoting that CommandLineToArgvW / the MSVC CRT (and so Python) undo
// exactly: wrap in quotes when needed, double the backslashes that precede a
// quote or the closing quote, escape embedded quotes.
static std::wstring quoteArg (const juce::String& a)
{
    const std::wstring w = toWide (a);
    if (!w.empty () && w.find_first_of (L" \t\n\v\"") == std::wstring::npos)
        return w;
    std::wstring q = L"\"";
    for (size_t i = 0; ; ++i)
    {
        size_t nBack = 0;
        while (i < w.size () && w[i] == L'\\') { ++i; ++nBack; }
        if (i == w.size ())
        {
            q.append (nBack * 2, L'\\');
            break;
        }
        if (w[i] == L'"')
        {
            q.append (nBack * 2 + 1, L'\\');
            q += L'"';
        }
        else
        {
            q.append (nBack, L'\\');
            q += w[i];
        }
    }
    q += L'"';
    return q;
}

// The current environment as a CreateProcessW block (UTF-16, sorted by name
// case-insensitively, double-NUL terminated), with the Python UTF-8 variables
// forced when asked.
static std::vector<wchar_t> buildEnvironmentBlock (bool pythonUtf8)
{
    std::vector<std::wstring> vars;
    if (LPWCH env = GetEnvironmentStringsW ())
    {
        for (const wchar_t* p = env; *p != 0; p += wcslen (p) + 1)
            vars.emplace_back (p);
        FreeEnvironmentStringsW (env);
    }
    // Names may start with '=' (per-drive current directories: "=C:=C:\x").
    auto nameOf = [] (const std::wstring& v) { return v.substr (0, v.find (L'=', 1)); };
    auto sameName = [] (const std::wstring& a, const wchar_t* b)
    {
        return CompareStringOrdinal (a.c_str (), (int) a.size (), b, -1, TRUE) == CSTR_EQUAL;
    };
    if (pythonUtf8)
    {
        vars.erase (std::remove_if (vars.begin (), vars.end (), [&] (const std::wstring& v) {
                        const std::wstring n = nameOf (v);
                        return sameName (n, L"PYTHONUTF8") || sameName (n, L"PYTHONIOENCODING");
                    }), vars.end ());
        vars.emplace_back (L"PYTHONUTF8=1");
        vars.emplace_back (L"PYTHONIOENCODING=utf-8");
    }
    std::stable_sort (vars.begin (), vars.end (), [&] (const std::wstring& a, const std::wstring& b) {
        const std::wstring na = nameOf (a), nb = nameOf (b);
        return CompareStringOrdinal (na.c_str (), (int) na.size (), nb.c_str (), (int) nb.size (), TRUE)
               == CSTR_LESS_THAN;
    });

    std::vector<wchar_t> block;
    for (const auto& v : vars)
    {
        block.insert (block.end (), v.begin (), v.end ());
        block.push_back (0);
    }
    if (block.empty ())
        block.push_back (0);
    block.push_back (0);
    return block;
}

ProcessResult runProcessEx (const std::string& cmd, const juce::StringArray& args,
                            const RunOptions& options, const std::function<bool ()>& shouldContinue)
{
    ProcessResult res;
    const double t0 = juce::Time::getMillisecondCounterHiRes ();

    // Build the command line: "exe" arg1 arg2 ... (cmd is UTF-8).
    std::wstring cmdLine = quoteArg (juce::String::fromUTF8 (cmd.c_str ()));
    for (const auto& a : args)
        cmdLine += L' ' + quoteArg (a);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof (sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    HANDLE outR = INVALID_HANDLE_VALUE, outW = INVALID_HANDLE_VALUE;
    HANDLE errR = INVALID_HANDLE_VALUE, errW = INVALID_HANDLE_VALUE;
    if (!CreatePipe (&outR, &outW, &sa, 1 << 16))
        return res;
    if (!CreatePipe (&errR, &errW, &sa, 1 << 16))
    {
        CloseHandle (outR);
        CloseHandle (outW);
        return res;
    }
    // Only the write ends belong to the child; our read ends must not be inherited.
    SetHandleInformation (outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation (errR, HANDLE_FLAG_INHERIT, 0);

    // stdin = NUL: a script reading stdin gets EOF at once instead of
    // blocking on (or consuming) whatever stdin SpectroCraft itself has.
    HANDLE nulIn = CreateFileW (L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si {};
    si.cb = sizeof (si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outW;
    si.hStdError  = errW;
    si.hStdInput  = nulIn != INVALID_HANDLE_VALUE ? nulIn : nullptr;

    // A job object holds the child and everything it starts, so a timeout or
    // a cancel kills the whole tree (TerminateProcess alone would orphan
    // e.g. the python.exe that the "py" launcher starts).
    HANDLE job = CreateJobObjectW (nullptr, nullptr);
    if (job != nullptr && options.killTreeOnExit)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li {};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject (job, JobObjectExtendedLimitInformation, &li, sizeof (li));
    }

    std::vector<wchar_t> envBlock = buildEnvironmentBlock (options.pythonUtf8);

    // CREATE_SUSPENDED: the child joins the job before it can start anything.
    // CREATE_NO_WINDOW: python.exe is a console program; without it Windows
    // flashes a console window over the GUI for every run (output goes
    // through the pipes anyway).
    PROCESS_INFORMATION pi {};
    const BOOL created = CreateProcessW (nullptr, cmdLine.data (), nullptr, nullptr, TRUE,
                                         CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                                         envBlock.data (), nullptr, &si, &pi);
    CloseHandle (outW);
    CloseHandle (errW);
    if (nulIn != INVALID_HANDLE_VALUE)
        CloseHandle (nulIn);
    if (!created)
    {
        CloseHandle (outR);
        CloseHandle (errR);
        if (job != nullptr) CloseHandle (job);
        res.elapsedMs = juce::Time::getMillisecondCounterHiRes () - t0;
        return res;   // executable not found / failed to start
    }
    res.started = true;
    const bool inJob = job != nullptr && AssignProcessToJobObject (job, pi.hProcess) != FALSE;
    ResumeThread (pi.hThread);

    // The pipes are drained while the child runs: a child that writes more
    // than the pipe buffer would otherwise block forever on a full pipe while
    // we wait for it to exit. PeekNamedPipe keeps the reads non-blocking. The
    // raw bytes are collected and decoded once at the end, so a multi-byte
    // UTF-8 sequence split across two reads stays intact.
    std::string outBytes, errBytes;
    auto drain = [] (HANDLE h, std::string& dest)
    {
        char tmp[8192];
        for (;;)
        {
            DWORD avail = 0;
            if (!PeekNamedPipe (h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0)
                return;
            const DWORD want = avail < (DWORD) sizeof (tmp) ? avail : (DWORD) sizeof (tmp);
            DWORD got = 0;
            if (!ReadFile (h, tmp, want, &got, nullptr) || got == 0)
                return;
            dest.append (tmp, (size_t) got);
        }
    };

    // Termination is asynchronous: wait (bounded) until every process of the
    // job is gone, so the caller can delete the exchange files they held open.
    auto waitJobEmpty = [&] (int maxMs)
    {
        for (int waited = 0; inJob && waited < maxMs; waited += 10)
        {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info {};
            if (!QueryInformationJobObject (job, JobObjectBasicAccountingInformation,
                                            &info, sizeof (info), nullptr)
                || info.ActiveProcesses == 0)
                return;
            Sleep (10);
        }
    };

    auto killTree = [&]
    {
        if (inJob) TerminateJobObject (job, 1);
        else       TerminateProcess (pi.hProcess, 1);
        WaitForSingleObject (pi.hProcess, 5000);
        waitJobEmpty (3000);
    };

    bool killed = false;
    size_t linesFrom = 0;
    uint32_t lastPoll = juce::Time::getMillisecondCounter ();
    for (;;)
    {
        const DWORD w = WaitForSingleObject (pi.hProcess, 20);
        drain (outR, outBytes);
        drain (errR, errBytes);
        deliverStdoutLines (outBytes, linesFrom, options.onStdoutLine, false);
        if (w == WAIT_OBJECT_0) break;
        if (options.timeoutMs > 0
            && juce::Time::getMillisecondCounterHiRes () - t0 >= (double) options.timeoutMs)
        {
            res.timedOut = true;
            killed = true;
            killTree ();
            break;
        }
        const uint32_t now = juce::Time::getMillisecondCounter ();
        if (now - lastPoll < 200) continue;
        lastPoll = now;
        if (shouldContinue != nullptr && !shouldContinue ())
        {
            res.cancelled = true;
            killed = true;
            killTree ();
            break;
        }
    }

    DWORD rc = (DWORD) -1;
    if (!killed)
        GetExitCodeProcess (pi.hProcess, &rc);
    res.exitCode = killed ? -1 : (int) rc;

    // Whatever the child wrote just before exiting is still in the pipes.
    drain (outR, outBytes);
    drain (errR, errBytes);
    deliverStdoutLines (outBytes, linesFrom, options.onStdoutLine, true);
    res.stdoutText = decodeProcessOutput (outBytes.data (), outBytes.size ());
    res.stderrText = decodeProcessOutput (errBytes.data (), errBytes.size ());

    CloseHandle (outR);
    CloseHandle (errR);
    CloseHandle (pi.hProcess);
    CloseHandle (pi.hThread);
    // Grandchildren left behind by a child that exited normally. (Closing a
    // KILL_ON_JOB_CLOSE job would do it too, but asynchronously.)
    if (!killed && options.killTreeOnExit && inJob)
    {
        TerminateJobObject (job, 1);
        waitJobEmpty (3000);
    }
    if (job != nullptr)
        CloseHandle (job);

    res.elapsedMs = juce::Time::getMillisecondCounterHiRes () - t0;
    return res;
}

#else   // POSIX

// PATH lookup done before fork(): execvp is not async-signal-safe, and the
// child needs a custom environment (execve).
static std::string resolveExecutable (const std::string& cmd)
{
    auto isExe = [] (const std::string& p)
    {
        struct stat st {};
        return stat (p.c_str (), &st) == 0 && S_ISREG (st.st_mode) && access (p.c_str (), X_OK) == 0;
    };
    if (cmd.empty ())
        return {};
    if (cmd.find ('/') != std::string::npos)
        return isExe (cmd) ? cmd : std::string ();

    const char* pathEnv = std::getenv ("PATH");
    const std::string path = (pathEnv != nullptr && *pathEnv != 0) ? pathEnv : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    for (;;)
    {
        const size_t colon = path.find (':', start);
        std::string dir = path.substr (start, colon == std::string::npos ? std::string::npos : colon - start);
        if (dir.empty ()) dir = ".";
        const std::string cand = dir + "/" + cmd;
        if (isExe (cand))
            return cand;
        if (colon == std::string::npos)
            break;
        start = colon + 1;
    }
    return {};
}

ProcessResult runProcessEx (const std::string& cmd, const juce::StringArray& args,
                            const RunOptions& options, const std::function<bool ()>& shouldContinue)
{
    ProcessResult res;
    const double t0 = juce::Time::getMillisecondCounterHiRes ();
    auto finish = [&res, t0] { res.elapsedMs = juce::Time::getMillisecondCounterHiRes () - t0; return res; };

    const std::string exe = resolveExecutable (cmd);
    if (exe.empty ())
        return finish ();   // not found: never started

    // Build argv and envp before fork(): only async-signal-safe calls are
    // allowed in the child of a multi-threaded process (no malloc there).
    std::vector<std::string> argStore;
    argStore.push_back (cmd);
    for (const auto& a : args)
        argStore.push_back (a.toStdString ());   // UTF-8
    std::vector<char*> argv;
    for (auto& a : argStore)
        argv.push_back (a.data ());
    argv.push_back (nullptr);

    std::vector<std::string> envStore;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e)
    {
        const std::string v (*e);
        if (options.pythonUtf8
            && (v.rfind ("PYTHONUTF8=", 0) == 0 || v.rfind ("PYTHONIOENCODING=", 0) == 0))
            continue;
        envStore.push_back (v);
    }
    if (options.pythonUtf8)
    {
        envStore.push_back ("PYTHONUTF8=1");
        envStore.push_back ("PYTHONIOENCODING=utf-8");
    }
    std::vector<char*> envp;
    for (auto& v : envStore)
        envp.push_back (v.data ());
    envp.push_back (nullptr);

    // Pipes. Every end is moved above stderr (a parent started with 0/1/2
    // closed would otherwise hand out fds the child's dup2 calls clobber),
    // and every end is close-on-exec, set
    // atomically where possible (pipe2 on Linux): a concurrent fork + exec
    // in another thread must not inherit them (a leaked exec-status write end
    // would delay our "did exec succeed" read, a leaked read end is a leak).
    auto makePipe = [] (int fds[2]) -> bool
    {
#if defined(__linux__)
        if (pipe2 (fds, O_CLOEXEC) != 0) return false;
#else
        if (pipe (fds) != 0) return false;
        fcntl (fds[0], F_SETFD, FD_CLOEXEC);
        fcntl (fds[1], F_SETFD, FD_CLOEXEC);
#endif
        for (int i = 0; i < 2; ++i)
            if (fds[i] <= STDERR_FILENO)
            {
                const int moved = fcntl (fds[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
                close (fds[i]);
                fds[i] = moved;
            }
        if (fds[0] < 0 || fds[1] < 0)
        {
            if (fds[0] >= 0) close (fds[0]);
            if (fds[1] >= 0) close (fds[1]);
            return false;
        }
        // The child dup2's its ends onto 0/1/2; dup2 clears close-on-exec
        // on the copy, so the originals can all stay close-on-exec.
        return true;
    };

    int outp[2] = { -1, -1 };
    int errp[2] = { -1, -1 };
    int execp[2] = { -1, -1 };   // reports an exec failure to the parent
    if (!makePipe (outp)) return finish ();
    if (!makePipe (errp)) { close (outp[0]); close (outp[1]); return finish (); }
    if (!makePipe (execp))
    {
        close (outp[0]); close (outp[1]);
        close (errp[0]); close (errp[1]);
        return finish ();
    }

    const pid_t pid = fork ();
    if (pid < 0)
    {
        close (outp[0]); close (outp[1]);
        close (errp[0]); close (errp[1]);
        close (execp[0]); close (execp[1]);
        return finish ();
    }

    if (pid == 0)   // child: async-signal-safe calls only
    {
        setpgid (0, 0);   // own process group: the whole tree can be signalled
#if defined(__linux__)
        prctl (PR_SET_PDEATHSIG, SIGKILL);   // dies with the (worker thread of the) parent
#endif
        sigset_t none;
        sigemptyset (&none);
        sigprocmask (SIG_SETMASK, &none, nullptr);
        signal (SIGPIPE, SIG_DFL);

        // All pipe fds are > 2 here, so these dup2 calls never clobber one.
        const int devNull = open ("/dev/null", O_RDONLY);
        if (devNull >= 0)
        {
            dup2 (devNull, STDIN_FILENO);
            if (devNull != STDIN_FILENO) close (devNull);
        }
        else
            close (STDIN_FILENO);
        dup2 (outp[1], STDOUT_FILENO);
        dup2 (errp[1], STDERR_FILENO);
        close (outp[0]); close (outp[1]);
        close (errp[0]); close (errp[1]);
        close (execp[0]);

        execve (exe.c_str (), argv.data (), envp.data ());
        const int e = errno;
        (void) ! write (execp[1], &e, sizeof (e));
        _exit (127);
    }

    setpgid (pid, pid);   // also from here: no window where the group does not exist yet
    close (outp[1]);
    close (errp[1]);
    close (execp[1]);

    // EOF = exec succeeded (the close-on-exec end closed); data = errno. The
    // wait is bounded: should another thread's fork have inherited the write
    // end in the tiny window before close-on-exec was set (macOS has no
    // pipe2), the EOF would only come when that unrelated process exits.
    // An exec failure is reported within microseconds.
    int execErr = 0;
    ssize_t n = 0;
    {
        const double execDeadline = juce::Time::getMillisecondCounterHiRes () + 2000.0;
        for (;;)
        {
            struct pollfd pfd { execp[0], POLLIN, 0 };
            const int pr = poll (&pfd, 1, 20);
            if (pr > 0)
            {
                do { n = read (execp[0], &execErr, sizeof (execErr)); } while (n < 0 && errno == EINTR);
                break;
            }
            if (pr < 0 && errno != EINTR)
                break;
            if (juce::Time::getMillisecondCounterHiRes () >= execDeadline)
                break;   // assume started; a later failure shows as exit code 127
        }
    }
    close (execp[0]);
    if (n == (ssize_t) sizeof (execErr))
    {
        int st = 0;
        waitpid (pid, &st, 0);
        close (outp[0]);
        close (errp[0]);
        return finish ();   // could not exec: not started
    }
    res.started = true;

    // Drain the pipes while the child runs (a child that fills a pipe would
    // otherwise block forever while we wait for it to exit). Raw bytes are
    // decoded once at the end so split UTF-8 sequences stay intact.
    std::string outBytes, errBytes;
    bool outOpen = true, errOpen = true;
    auto pump = [&] (int timeoutMs)
    {
        struct pollfd fds[2];
        int nfds = 0;
        if (outOpen) { fds[nfds].fd = outp[0]; fds[nfds].events = POLLIN; fds[nfds].revents = 0; ++nfds; }
        if (errOpen) { fds[nfds].fd = errp[0]; fds[nfds].events = POLLIN; fds[nfds].revents = 0; ++nfds; }
        if (nfds == 0)
        {
            if (timeoutMs > 0) juce::Thread::sleep (timeoutMs);
            return false;
        }
        if (poll (fds, (nfds_t) nfds, timeoutMs) <= 0)
            return false;
        bool gotData = false;
        for (int i = 0; i < nfds; ++i)
        {
            if (fds[i].revents == 0) continue;
            const bool isOut = fds[i].fd == outp[0];
            char tmp[8192];
            const ssize_t got = read (fds[i].fd, tmp, sizeof (tmp));
            if (got > 0)
            {
                (isOut ? outBytes : errBytes).append (tmp, (size_t) got);
                gotData = true;
            }
            else if (got == 0 || errno != EINTR)
                (isOut ? outOpen : errOpen) = false;   // EOF / error
        }
        return gotData;
    };

    int status = 0;
    bool reaped = false;
    auto signalTree = [&] (int sig)
    {
        // The group outlives its leader while any member is alive; only fall
        // back to the pid itself while it is certainly still ours (not reaped).
        if (kill (-pid, sig) != 0 && !reaped)
            kill (pid, sig);
    };
    auto killTree = [&]
    {
        signalTree (SIGTERM);   // let Python run its finally/atexit handlers
        const double graceEnd = juce::Time::getMillisecondCounterHiRes () + std::max (0, options.killGraceMs);
        while (!reaped && juce::Time::getMillisecondCounterHiRes () < graceEnd)
        {
            pump (10);
            if (waitpid (pid, &status, WNOHANG) == pid) reaped = true;
        }
        signalTree (SIGKILL);   // stragglers and children that ignore SIGTERM
        if (!reaped)
        {
            while (waitpid (pid, &status, 0) < 0 && errno == EINTR) {}
            reaped = true;
        }
    };

    bool killed = false, lost = false;
    size_t linesFrom = 0;
    uint32_t lastPoll = juce::Time::getMillisecondCounter ();
    for (;;)
    {
        pump (20);
        deliverStdoutLines (outBytes, linesFrom, options.onStdoutLine, false);
        const pid_t r = waitpid (pid, &status, WNOHANG);
        if (r == pid) { reaped = true; break; }
        if (r < 0 && errno != EINTR) { reaped = true; lost = true; break; }   // ECHILD: reaped elsewhere
        if (options.timeoutMs > 0
            && juce::Time::getMillisecondCounterHiRes () - t0 >= (double) options.timeoutMs)
        {
            res.timedOut = true;
            killed = true;
            killTree ();
            break;
        }
        const uint32_t now = juce::Time::getMillisecondCounter ();
        if (now - lastPoll < 200) continue;
        lastPoll = now;
        if (shouldContinue != nullptr && !shouldContinue ())
        {
            res.cancelled = true;
            killed = true;
            killTree ();
            break;
        }
    }

    // Grandchildren left behind by a child that exited normally.
    if (!killed && options.killTreeOnExit)
        signalTree (SIGKILL);

    // Whatever the child wrote just before exiting is still in the pipes. Do
    // not wait for EOF: a grandchild may still hold the write ends open.
    while (pump (0)) {}
    close (outp[0]);
    close (errp[0]);
    deliverStdoutLines (outBytes, linesFrom, options.onStdoutLine, true);
    res.stdoutText = decodeProcessOutput (outBytes.data (), outBytes.size ());
    res.stderrText = decodeProcessOutput (errBytes.data (), errBytes.size ());

    res.exitCode = (!killed && !lost && WIFEXITED (status)) ? WEXITSTATUS (status) : -1;
    return finish ();
}

#endif   // JUCE_WINDOWS

bool runProcess (const std::string& cmd, const juce::StringArray& args,
                 const std::function<bool ()>& shouldContinue,
                 int* exitCodeOut, juce::String* outStdout, juce::String* outStderr)
{
    const ProcessResult r = runProcessEx (cmd, args, RunOptions {}, shouldContinue);
    if (outStdout != nullptr) *outStdout += r.stdoutText;
    if (outStderr != nullptr) *outStderr += r.stderrText;
    if (exitCodeOut != nullptr) *exitCodeOut = r.exitCode;
    return r.started && !r.cancelled && !r.timedOut;
}

// ============================================================================
// One script run
// ============================================================================

ScriptRunReport runScript (const std::string& pythonCmd, const juce::File& script,
                           const ScriptExchange& exchange, const RunOptions& options,
                           const std::function<bool ()>& shouldContinue)
{
    ScriptRunReport rep;
    rep.script = script;
    rep.interpreter = juce::String::fromUTF8 (pythonCmd.c_str ());
    rep.scriptSha256 = scriptSha256 (script);

    const juce::StringArray tokens = splitCommand (pythonCmd);
    if (tokens.isEmpty ())
        return rep;   // process.started = false

    juce::StringArray args;
    for (int i = 1; i < tokens.size (); ++i)   // extra flags, e.g. "-3"
        args.add (tokens[i]);
    args.add (script.getFullPathName ());
    args.add (exchange.inWav.getFullPathName ());
    args.add (exchange.outWav.getFullPathName ());
    args.add (exchange.metaJson.getFullPathName ());

    // Outputs of an earlier run must never pass for this one's.
    exchange.outWav.deleteFile ();
    exchange.resultJson.deleteFile ();

    rep.process = runProcessEx (tokens[0].toStdString (), args, options, shouldContinue);
    if (rep.process.started && !rep.process.cancelled && !rep.process.timedOut)
    {
        rep.outWavPresent = exchange.outWav.existsAsFile ();
        rep.result = readScriptResult (exchange.resultJson, exchange.inputStartSec);
    }
    return rep;
}

static thread_local ScopedScriptRunCapture* tlsScriptRunCapture = nullptr;

ScopedScriptRunCapture::ScopedScriptRunCapture (const RunOptions& options)
    : options_ (options), previous_ (tlsScriptRunCapture)
{
    tlsScriptRunCapture = this;
}

ScopedScriptRunCapture::~ScopedScriptRunCapture()
{
    tlsScriptRunCapture = previous_;
}

ScopedScriptRunCapture* ScopedScriptRunCapture::current()
{
    return tlsScriptRunCapture;
}

// ============================================================================
// Interpreter probing + install help
// ============================================================================

bool findPython (std::string* cmdOut, std::string* errOut)
{
    struct Cand { const char* exe; juce::StringArray extra; };
#if JUCE_WINDOWS
    static const Cand cands[] = {
        { "python", {} },
        { "py",     { "-3" } },
    };
#else
    static const Cand cands[] = {
        { "python3", {} },
        { "python",  {} },
    };
#endif

    for (const auto& c : cands)
    {
        juce::StringArray args = c.extra;
        args.add ("--version");
        int rc = -1;
        juce::String out, err;
        if (!runProcess (c.exe, args, [] { return true; }, &rc, &out, &err))
            continue;   // not found / failed to start

        const juce::String all = (out + " " + err).trim ();
        if (rc == 0 && all.containsIgnoreCase ("Python"))
        {
            std::string s = c.exe;
            for (const auto& e : c.extra) s += " " + e.toStdString ();
            if (cmdOut != nullptr) *cmdOut = s;
            return true;
        }
    }

    if (errOut != nullptr)
        *errOut = "No Python interpreter was found on the PATH.";
    return false;
}

const char* installHelpText()
{
    return R"TXT(No Python interpreter was found on your PATH.

The Python panel runs scripts with a minimal CPython installation - only the
standard library is needed, no pip packages are required.

To install:
 1. Download the latest stable Python 3.x for your operating system from
    https://www.python.org/downloads/
 2. Run the installer and keep the default options. On Windows make sure the
    "Add python.exe to PATH" option is checked (it is on by default).
 3. Verify in a terminal that this prints a version number:
        python --version      (or: py -3 --version)
 4. Press Run again in this panel.

The example scripts only use the Python standard library, so nothing else
needs to be installed.)TXT";
}

}}   // namespace sc::py
