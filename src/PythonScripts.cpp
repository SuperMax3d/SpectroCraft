// SpectroCraft - Python script panel support.
#include "PythonScripts.h"
#include "MarkerManager.h"

#if JUCE_WINDOWS
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/wait.h>
#  include <signal.h>
#  include <cstring>
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

// Template written by the panel's Add button.
static juce::String newScriptTemplateText ()
{
    const std::string s = std::string (R"PY(
# SpectroCraft Python script
#
# Called as:  python <this file> <input.wav> <output.wav> <meta.json>
#   input.wav  - float32 WAV of the selected range (or whole file), all channels
#   meta.json  - sample rate, channel count, selection times, markers, ...
# You must write output.wav with the SAME sample rate, channel count and length.

import sys

)PY") + kWavIo + R"PY(


def main():
    in_path, out_path, meta_path = sys.argv[1:4]
    rate, nch, x = read_wav(in_path)

    # TODO: process the samples here (x is a channel-interleaved float32 array)

    write_wav(out_path, rate, nch, x)


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

void ensureExampleScripts (const juce::File& dir)
{
    if (!dir.exists())
        dir.createDirectory ();

    struct Ex { const char* name; juce::String body; };
    const Ex examples[] = {
        { "Dynamic processing.py", dynamicProcessingScript () },
        { "Threshold.py",          thresholdScript () },
        { "Modulation.py",         modulationScript () },
    };

    for (const auto& e : examples)
    {
        const juce::File f = dir.getChildFile (e.name);
        if (!f.existsAsFile())
            f.replaceWithText (e.body);   // never overwrite an existing script
    }
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

// ============================================================================
// Metadata JSON
// ============================================================================

juce::String buildMetaJson (double sampleRate, int numChannels, double fileDurationSec,
                            juce::Range<double> selectionSecs, bool wholeFile,
                            const juce::String& fileName,
                            const std::vector<Marker>& markers)
{
    auto root = new juce::DynamicObject ();
    root->setProperty ("sampleRate",        (int64_t) sampleRate);
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
        mj->setProperty ("end",     (m.end < 0.0) ? juce::var ((int64_t) -1)
                                                   : juce::var ((int64_t) std::llround (m.end)));
        markerArray.add (juce::var (mj));
    }
    root->setProperty ("markers", juce::var (markerArray));

    return juce::JSON::toString (juce::var (root), true);
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

#if JUCE_WINDOWS

static std::wstring toWide (const juce::String& s)
{
    return std::wstring (s.toWideCharPointer ());
}

// MSVCRT-style quoting: wrap in quotes when the argument contains whitespace,
// and double any embedded quotes.
static std::wstring quoteArg (const juce::String& a)
{
    const std::wstring w = toWide (a);
    bool needsQuotes = false;
    for (wchar_t c : w)
        if (c == L' ' || c == L'\t' || c == L'"') { needsQuotes = true; break; }
    if (!w.empty () && !needsQuotes) return w;
    std::wstring q = L"\"";
    for (wchar_t c : w)
    {
        if (c == L'"') q += L"\"\"";
        else           q += c;
    }
    q += L'\"';
    return q;
}

bool runProcess (const std::string& cmd, const juce::StringArray& args,
                 const std::function<bool ()>& shouldContinue,
                 int* exitCodeOut, juce::String* outStdout, juce::String* outStderr)
{
    // Build the command line: "exe" arg1 arg2 ...
    juce::String cmdLine (quoteArg (juce::String (cmd)).c_str ());
    for (const auto& a : args)
        cmdLine << ' ' << juce::String (quoteArg (a).c_str ());

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof (sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    HANDLE outR = INVALID_HANDLE_VALUE, outW = INVALID_HANDLE_VALUE;
    HANDLE errR = INVALID_HANDLE_VALUE, errW = INVALID_HANDLE_VALUE;
    if (!CreatePipe (&outR, &outW, &sa, 0) || !CreatePipe (&errR, &errW, &sa, 0))
        return false;

    STARTUPINFOW si {};
    si.cb = sizeof (si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outW;
    si.hStdError  = errW;
    si.hStdInput  = GetStdHandle (STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi {};
    const BOOL created = CreateProcessW (nullptr, toWide (cmdLine).data (), nullptr, nullptr,
                                         TRUE, 0, nullptr, nullptr, &si, &pi);
    CloseHandle (outW);
    CloseHandle (errW);
    if (!created)
    {
        CloseHandle (outR);
        CloseHandle (errR);
        return false;   // executable not found / failed to start
    }

    int rc = -1;
    bool killed = false;
    for (;;)
    {
        const DWORD w = WaitForSingleObject (pi.hProcess, 200);
        if (w == WAIT_OBJECT_0) break;
        if (shouldContinue != nullptr && !shouldContinue ())
        {
            TerminateProcess (pi.hProcess, 1);
            killed = true;
            WaitForSingleObject (pi.hProcess, 5000);
            break;
        }
    }

    if (!killed)
        GetExitCodeProcess (pi.hProcess, (DWORD*) &rc);

    auto drain = [] (HANDLE h, juce::String* dest)
    {
        if (dest == nullptr || h == INVALID_HANDLE_VALUE) return;
        char tmp[8192];
        DWORD got = 0;
        while (ReadFile (h, tmp, sizeof (tmp), &got, nullptr) && got > 0)
            *dest += juce::String (tmp);   // UTF-8
    };
    drain (outR, outStdout);
    drain (errR, outStderr);

    CloseHandle (outR);
    CloseHandle (errR);
    CloseHandle (pi.hProcess);
    CloseHandle (pi.hThread);

    if (exitCodeOut != nullptr) *exitCodeOut = rc;
    return !killed;
}

#else   // POSIX

bool runProcess (const std::string& cmd, const juce::StringArray& args,
                 const std::function<bool ()>& shouldContinue,
                 int* exitCodeOut, juce::String* outStdout, juce::String* outStderr)
{
    int outp[2] = { -1, -1 };
    int errp[2] = { -1, -1 };
    if (pipe (outp) != 0 || pipe (errp) != 0) return false;

    const pid_t pid = fork ();
    if (pid < 0)
        return false;

    if (pid == 0)   // child
    {
        dup2 (outp[1], STDOUT_FILENO);
        dup2 (errp[1], STDERR_FILENO);
        close (outp[0]); close (outp[1]);
        close (errp[0]); close (errp[1]);

        // Build a proper argv with heap-allocated strings that outlive execvp.
        std::vector<char*> argv;
        argv.push_back (strdup (cmd.c_str ()));
        for (const auto& a : args)
            argv.push_back (strdup (a.toRawUTF8 ()));
        argv.push_back (nullptr);
        execvp (cmd.c_str (), argv.data ());
        _exit (127);
    }

    close (outp[1]);
    close (errp[1]);

    int rc = -1;
    bool killed = false;
    for (;;)
    {
        pid_t r = waitpid (pid, &rc, WNOHANG);
        if (r == pid) break;
        if (shouldContinue != nullptr && !shouldContinue ())
        {
            kill (pid, SIGTERM);
            killed = true;
            waitpid (pid, &rc, 0);
            break;
        }
        juce::Thread::sleep (200);
    }

    auto drain = [] (int fd, juce::String* dest)
    {
        if (dest == nullptr || fd < 0) return;
        char tmp[8192];
        ssize_t got = 0;
        while ((got = read (fd, tmp, sizeof (tmp))) > 0)
            *dest += juce::String (tmp);   // UTF-8
    };
    drain (outp[0], outStdout);
    drain (errp[0], outStderr);
    close (outp[0]);
    close (errp[0]);

    if (WIFEXITED (rc)) rc = WEXITSTATUS (rc); else rc = -1;
    if (exitCodeOut != nullptr) *exitCodeOut = rc;
    return !killed;
}

#endif   // JUCE_WINDOWS

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
