// SpectroCraft - audio.cpp stem-separation support (external CLI tool).
#include "AudioCpp.h"
#include "Resampler.h"

#if JUCE_WINDOWS
#  include <windows.h>
#  include <wininet.h>
#endif

namespace sc { namespace acpp {

juce::File dir()
{
    const juce::File d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                             .getChildFile ("SpectroCraft")
                             .getChildFile ("models")
                             .getChildFile ("audiocpp");
    if (!d.exists())
        d.createDirectory();
    return d;
}

juce::File binaryPath(const juce::File& d)
{
    return (d.getFullPathName().isEmpty() ? dir() : d).getChildFile ("audiocpp_cli.exe");
}

bool hasBinary(const juce::File& d)
{
    const juce::File b = binaryPath (d);
    return b.existsAsFile () && b.getSize () > 0;
}

std::vector<juce::File> listModels(const juce::File& d)
{
    std::vector<juce::File> out;
    const juce::File root = (d.getFullPathName().isEmpty() ? dir() : d);
    if (!root.isDirectory()) return out;
    for (auto entry : juce::RangedDirectoryIterator (root, false))
    {
        const juce::String name = entry.getFile().getFileName();
        if (name.startsWithIgnoreCase ("sam-audio-") && name.endsWithIgnoreCase (".gguf"))
            out.push_back (entry.getFile());
    }
    std::sort (out.begin (), out.end (),
               [] (const juce::File& a, const juce::File& b) {
                   return a.getFileName().toLowerCase() < b.getFileName().toLowerCase(); });
    return out;
}

std::vector<BinarySpec> binaryChoices()
{
    // Windows x64 assets of the v0.9.2-drama3-base-small release.
    const char* base = "https://github.com/0xShug0/audio.cpp/releases/download/v0.9.2-drama3-base-small/";
    std::vector<BinarySpec> v;
    BinarySpec e1 { "cpu",      juce::String ("CPU (portable)"),
                    std::string (base) + "audio-v0.9.2-drama3-base-small-bin-windows-x64-cpu-portable.zip", 28786884 };
    BinarySpec e2 { "cuda12.4", juce::String ("CUDA 12.4"),
                    std::string (base) + "audio-v0.9.2-drama3-base-small-bin-windows-x64-cuda12.4.zip", 464268606 };
    BinarySpec e3 { "cuda13.3", juce::String ("CUDA 13.3"),
                    std::string (base) + "audio-v0.9.2-drama3-base-small-bin-windows-x64-cuda13.3.zip", 275837338 };
    BinarySpec e4 { "vulkan",   juce::String ("Vulkan (portable)"),
                    std::string (base) + "audio-v0.9.2-drama3-base-small-bin-windows-x64-vulkan-portable.zip", 63227361 };
    v.push_back (e1); v.push_back (e2); v.push_back (e3); v.push_back (e4);
    return v;
}

std::vector<ModelSpec> modelChoices()
{
    // audio-cpp/SAM-Audio-GGUF on Hugging Face.
    std::vector<ModelSpec> v;
    ModelSpec m1 { "sam-audio-small-q8_0.gguf", 2379290272 };
    ModelSpec m2 { "sam-audio-small-bf16.gguf", 2824105568 };
    ModelSpec m3 { "sam-audio-small-f32.gguf", 5640890208 };
    ModelSpec m4 { "sam-audio-base-q8_0.gguf", 3095951072 };
    ModelSpec m5 { "sam-audio-base-bf16.gguf", 4136791712 };
    ModelSpec m6 { "sam-audio-base-f32.gguf", 8265738720 };
    ModelSpec m7 { "sam-audio-large-q8_0.gguf", 5029984064 };
    ModelSpec m8 { "sam-audio-large-bf16.gguf", 7705276160 };
    ModelSpec m9 { "sam-audio-large-f32.gguf", 15401675328 };
    v.push_back (m1); v.push_back (m2); v.push_back (m3);
    v.push_back (m4); v.push_back (m5); v.push_back (m6);
    v.push_back (m7); v.push_back (m8); v.push_back (m9);
    return v;
}

juce::String modelUrl(const juce::String& fileName)
{
    return "https://huggingface.co/audio-cpp/SAM-Audio-GGUF/resolve/main/" + fileName;
}

static const juce::File variantMarkerFile(const juce::File& d)
{
    return (d.getFullPathName().isEmpty() ? dir() : d).getChildFile ("variant.txt");
}

juce::String installedVariant(const juce::File& d)
{
    const juce::File f = variantMarkerFile (d);
    if (!f.existsAsFile()) return {};
    return f.loadFileAsString().trim();
}

void setInstalledVariant(const juce::String& variantId, const juce::File& d)
{
    variantMarkerFile (d).replaceWithText (variantId);
}

// ---------------------------------------------------------------------------
// Downloads.
// ---------------------------------------------------------------------------

#if JUCE_WINDOWS

bool downloadFile(const std::string& url, const juce::File& dest,
                  const std::function<bool(float)>& onProgress,
                  std::string* errOut)
{
    HINTERNET hInternet = InternetOpenA ("SpectroCraft", INTERNET_OPEN_TYPE_PRECONFIG,
                                         nullptr, 0, 0);
    if (!hInternet)
    {
        if (errOut) *errOut = "Could not initialise the network stack.";
        return false;
    }

    const DWORD flags = (url.rfind ("https://", 0) == 0 ? INTERNET_FLAG_SECURE : 0);
    HINTERNET hRequest = InternetOpenUrlA (hInternet, url.c_str (), nullptr, 0, flags, 0);
    if (!hRequest)
    {
        const DWORD e = GetLastError ();
        if (errOut) *errOut = "HTTP request failed (error " + std::to_string ((int) e) + ").";
        InternetCloseHandle (hInternet);
        return false;
    }

    // Content-Length (may be 0 for chunked responses).
    DWORD contentLength = 0, lenSize = sizeof (contentLength);
    HttpQueryInfoA (hRequest, HTTP_QUERY_CONTENT_LENGTH, &contentLength, &lenSize, nullptr);
    const int64_t totalBytes = (int64_t) contentLength;

    const juce::File part = dest.getSiblingFile (dest.getFileName () + ".part");
    bool ok = false, cancelled = false, openFailed = false;
    int64_t got = 0;
    {
        juce::FileOutputStream out (part);
        if (!out.failedToOpen ())
        {
            char buf[1 << 18];
            while (!cancelled)
            {
                DWORD nRead = 0;
                // InternetReadFile returns FALSE with nRead == 0 at clean EOF.
                if (!InternetReadFile (hRequest, buf, sizeof (buf), &nRead) || nRead == 0)
                    break;
                out.write ((const unsigned char*) buf, nRead);
                got += (int64_t) nRead;
                if (onProgress && !onProgress (totalBytes > 0 ? (float) got / (float) totalBytes : 1.0f))
                    cancelled = true;
            }
        }
        else openFailed = true;
    }

    if (openFailed)
    {
        if (errOut) *errOut = "Could not write to the models folder.";
    }
    else if (!cancelled && got > 0)
    {
        // The stream is closed by now, so renaming cannot hit a sharing violation.
        dest.deleteFile ();
        ok = part.moveFileTo (dest);
        if (!ok && errOut) *errOut = "Could not move the downloaded file into place.";
    }
    else if (cancelled)
    {
        // Leave no partial file behind.
    }

    if (!ok) part.deleteFile ();
    InternetCloseHandle (hRequest);
    InternetCloseHandle (hInternet);
    return ok;
}

#else   // non-Windows: manual download only.

bool downloadFile(const std::string&, const juce::File&,
                  const std::function<bool(float)>&, std::string* errOut)
{
    if (errOut) *errOut = "Automatic downloads are only supported on Windows in v1.";
    return false;
}

#endif

bool installBinaryFromZip(const juce::File& zipPath, const juce::String& variantId,
                          const juce::File& d, std::string* errOut)
{
    const juce::File root = (d.getFullPathName().isEmpty() ? dir() : d);
    if (!zipPath.existsAsFile())
    {
        if (errOut) *errOut = "The downloaded zip file is missing.";
        return false;
    }

    juce::ZipFile zip (zipPath);
    const int idx = zip.getIndexOfFileName ("audiocpp_cli.exe");
    if (idx < 0)
    {
        if (errOut) *errOut = "audiocpp_cli.exe was not found inside the downloaded archive.";
        return false;
    }

    // The entry sits at the archive root, so it lands directly in root.
    const juce::Result r = zip.uncompressEntry (idx, root);
    if (!r.wasOk ())
    {
        juce::String msg ("Could not extract audiocpp_cli.exe: ");
        msg += r.getErrorMessage ();
        if (errOut) *errOut = msg.toStdString ();
        return false;
    }

    setInstalledVariant (variantId, root);
    zipPath.deleteFile ();   // the archive is only needed once
    return hasBinary (root);
}

// ---------------------------------------------------------------------------
// CLI invocation.
// ---------------------------------------------------------------------------

juce::String formatAnchorTime(double secs)
{
    if (!std::isfinite (secs)) secs = 0.0;
    juce::String s = juce::String (secs, 4);   // up to 4 decimals
    while (s.endsWith ("0")) s = s.dropLastCharacters (1);
    if (s.endsWith (".")) s = s.dropLastCharacters (1);
    return s;
}

juce::String buildAnchorsOption(const std::vector<Anchor>& anchors)
{
    juce::String s = "anchors=[";
    for (size_t i = 0; i < anchors.size (); ++i)
    {
        if (i > 0) s += ",";
        const Anchor& a = anchors[i];
        s += "[\"" + juce::String (a.include ? "+" : "-") + "\","
             + formatAnchorTime (a.startSecs) + ","
             + formatAnchorTime (a.stopSecs) + "]";
    }
    return s + "]";
}

juce::StringArray buildCliArgs(const juce::File& model, const juce::String& backend,
                               const juce::File& audioIn, const juce::String& text,
                               const std::vector<Anchor>& anchors,
                               const juce::File& outDir)
{
    juce::StringArray args;
    args.add ("--task");            args.add ("s2s");
    args.add ("--family");          args.add ("sam_audio");
    args.add ("--model");           args.add (model.getFullPathName());
    args.add ("--backend");         args.add (backend);
    args.add ("--audio");           args.add (audioIn.getFullPathName());
    args.add ("--text");            args.add (text);
    if (!anchors.empty ())
    {
        args.add ("--request-option");
        args.add (buildAnchorsOption (anchors));
    }
    args.add ("--out-dir");         args.add (outDir.getFullPathName());
    args.add ("--log");
    return args;
}

#if JUCE_WINDOWS

// Quote one argument for a CreateProcessW command line. Backslashes are only
// special immediately before a quote, so runs of backslashes preceding a
// quote must be doubled and the quote escaped with a single backslash - this
// is what makes "anchors=[[\"-\",0.1,0.5]]" reach the tool intact.
static std::wstring acppQuote(const juce::String& arg)
{
    const std::wstring w = arg.toWideCharPointer ();
    bool needsQuotes = false;
    for (wchar_t c : w)
        if (c == L' ' || c == L'\t' || c == L'"') { needsQuotes = true; break; }
    if (!w.empty () && !needsQuotes) return w;

    std::wstring q = L"\"";
    size_t i = 0;
    while (i < w.size ())
    {
        size_t nBack = 0;
        while (i + nBack < w.size () && w[i + nBack] == L'\\') ++nBack;
        if (i + nBack < w.size () && w[i + nBack] == L'"')
        {
            q.append (nBack * 2 + 1, L'\\');   // double each backslash...
            q += L'"';                        // ...and escape the quote
        }
        else
        {
            q.append (nBack, L'\\');          // literal backslashes pass through
            if (i + nBack < w.size ()) q += w[i + nBack];
        }
        i += nBack + 1;
    }
    return q + L"\"";
}

bool runStemSeparation(const juce::File& binary, const juce::File& model,
                       const juce::String& backend, const juce::File& audioIn,
                       const juce::String& text, const std::vector<Anchor>& anchors,
                       const juce::File& outDir,
                       const std::function<bool()>& shouldContinue,
                       const std::function<void(const juce::String&)>& onLogLine,
                       RunResult* resOut, std::string* errOut)
{
    if (!binary.existsAsFile ())
    {
        if (errOut) *errOut = "The audiocpp_cli.exe binary is missing.";
        return false;
    }

    outDir.createDirectory ();

    juce::String cmdLine (acppQuote (binary.getFullPathName()).c_str ());
    for (const auto& a : buildCliArgs (model, backend, audioIn, text, anchors, outDir))
        cmdLine << ' ' << acppQuote (a).c_str ();

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof (sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    HANDLE outR = INVALID_HANDLE_VALUE, outW = INVALID_HANDLE_VALUE;
    HANDLE errR = INVALID_HANDLE_VALUE, errW = INVALID_HANDLE_VALUE;
    if (!CreatePipe (&outR, &outW, &sa, 1 << 20) || !CreatePipe (&errR, &errW, &sa, 1 << 20))
    {
        if (errOut) *errOut = "Could not create the process pipes.";
        return false;
    }

    STARTUPINFOW si {};
    si.cb = sizeof (si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outW;
    si.hStdError  = errW;
    si.hStdInput  = GetStdHandle (STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi {};
    // CreateProcessW may modify the command line buffer, so it needs a mutable copy.
    std::wstring cmdW = cmdLine.toWideCharPointer ();
    const BOOL created = CreateProcessW (nullptr, cmdW.data (), nullptr, nullptr,
                                         TRUE, 0, nullptr, nullptr, &si, &pi);
    CloseHandle (outW);
    CloseHandle (errW);
    if (!created)
    {
        CloseHandle (outR);
        CloseHandle (errR);
        if (errOut) *errOut = "Could not start audiocpp_cli.exe.";
        return false;
    }

    // Non-blocking drain: PeekNamedPipe tells us how much is available without
    // stalling the poll loop, so log lines reach the UI while it runs.
    auto drainLines = [&] (HANDLE h, std::string& buf,
                           const std::function<void(const juce::String&)>& cb) {
        char tmp[1 << 16];
        for (;;)
        {
            DWORD avail = 0;
            if (!PeekNamedPipe (h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
            DWORD got = 0;
            if (!ReadFile (h, tmp, sizeof (tmp), &got, nullptr) || got == 0) break;
            buf.append (tmp, got);
            size_t pos;
            while ((pos = buf.find ('\n')) != std::string::npos)
            {
                const juce::String line = juce::String (buf.substr (0, pos)).trimCharactersAtEnd ("\r");
                if (!line.isEmpty () && cb) cb (line);
                buf.erase (0, pos + 1);
            }
        }
    };

    std::string outBuf, errBuf;
    bool killed = false;
    for (;;)
    {
        const DWORD w = WaitForSingleObject (pi.hProcess, 200);
        if (w == WAIT_OBJECT_0) break;
        drainLines (outR, outBuf, onLogLine);
        drainLines (errR, errBuf, nullptr);
        if (shouldContinue != nullptr && !shouldContinue ())
        {
            TerminateProcess (pi.hProcess, 1);
            killed = true;
            WaitForSingleObject (pi.hProcess, 5000);
            break;
        }
    }

    // Final drain after the write ends close.
    drainLines (outR, outBuf, onLogLine);
    drainLines (errR, errBuf, nullptr);

    int rc = -1;
    if (!killed) GetExitCodeProcess (pi.hProcess, (DWORD*) &rc);

    CloseHandle (outR);
    CloseHandle (errR);
    CloseHandle (pi.hProcess);
    CloseHandle (pi.hThread);

    if (resOut != nullptr)
    {
        resOut->exitCode = rc;
        resOut->stdoutText = juce::String (outBuf);
        resOut->stderrText = juce::String (errBuf);
    }
    if (killed)
    {
        if (errOut) *errOut = "Cancelled.";
        return false;
    }
    return rc == 0;
}

#else   // POSIX: not supported in v1.

bool runStemSeparation(const juce::File&, const juce::File&, const juce::String&,
                       const juce::File&, const juce::String&, const std::vector<Anchor>&,
                       const juce::File&, const std::function<bool()>&,
                       const std::function<void(const juce::String&)>&,
                       RunResult*, std::string* errOut)
{
    if (errOut) *errOut = "Stem Separation requires the Windows CLI binary.";
    return false;
}

#endif

}} // namespace sc::acpp
