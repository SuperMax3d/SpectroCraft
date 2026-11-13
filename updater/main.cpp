// SpectroCraft — the updater: a small console program that finishes an update
// after the app has quit, and starts the app again.
//
// Why a separate program: SpectroCraft.exe is the file being replaced, and
// Windows will not overwrite a running executable. So the app stages and
// verifies everything, launches this program, and exits; the updater waits for
// the app's process to disappear, swaps the files, writes update-result.json
// next to SpectroCraft.exe, and relaunches the app with --finish-update so it
// can tell the user what happened (docs/updates.md).
//
// Plain C++20 + Win32, no JUCE: it must keep working when the rest of the
// install folder is in the middle of being replaced.
//
// Usage:
//   spectrocraft-updater --pid <n> --install-dir <dir> --staging-dir <dir>
//                        --relaunch <exe> [--no-relaunch]
//       Reads <staging-dir>/update-plan.txt (written by the app after it has
//       checked the manifest signature and every SHA-256), swaps the listed
//       files, and reports the result.
//
//   spectrocraft-updater --sign <keyfile> <input> <output.sig>
//       Ed25519-sign a file (used by CI to sign update-manifest.json).
//   spectrocraft-updater --verify <keyfile> <input> <sig>
//       Check such a signature (CI re-checks what it just signed; the app does
//       the same check with the public key built into it).
//   spectrocraft-updater --public <keyfile>
//       Print the public key that --sign produces signatures for.
//
// Exit code: 0 when the update was applied, 1 otherwise (update-result.json
// carries the reason either way).

#include "Ed25519.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <cerrno>
  #include <fcntl.h>
  #include <signal.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
#endif

namespace {

// ------------------------------------------------------------------ text bits

std::string trim (const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n')) --b;
    return s.substr (a, b - a);
}

bool isAbsolute (const std::string& p)
{
    if (p.empty()) return true;
    if (p[0] == '/') return true;
#ifdef _WIN32
    if (p.size() >= 2 && p[1] == ':') return true;
    if (p[0] == '\\' || p[0] == '?') return true;
#endif
    return false;
}

// A manifest-relative path must stay inside the install folder. The app checks
// this too; repeating it here keeps a damaged plan file from escaping.
bool safeRelative (const std::string& p)
{
    if (isAbsolute (p) || p.find ("..") != std::string::npos) return false;
    for (char c : p)
        if (c < 32 || c == '"' || c == '*' || c == '?') return false;
    return ! p.empty();
}

std::string jsonEscape (const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char) c < 0x20) out += " ";
                else out += c;
        }
    }
    return out;
}

std::string jsonArray (const std::vector<std::string>& items)
{
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i) out += ", ";
        out += "\"" + jsonEscape (items[i]) + "\"";
    }
    out += "]";
    return out;
}

// ------------------------------------------------------------------ paths

// root + manifest-relative path, in the separators this OS prefers.
std::string join (const std::string& root, const std::string& rel)
{
    std::string out = root;
    if (! out.empty() && out.back() != '/' && out.back() != '\\')
        out +=
#ifdef _WIN32
            "\\";
#else
            "/";
#endif
    for (char c : rel)
    {
#ifdef _WIN32
        out += (c == '/') ? '\\' : c;
#else
        out += (c == '\\') ? '/' : c;
#endif
    }
    return out;
}

#ifdef _WIN32

std::wstring toWide (const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar (CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring out ((size_t) n, L'\0');
    MultiByteToWideChar (CP_UTF8, 0, s.data(), (int) s.size(), out.data(), n);
    return out;
}

#endif

bool fileExists (const std::string& p)
{
#ifdef _WIN32
    const DWORD a = GetFileAttributesW (toWide (p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st {};
    return stat (p.c_str(), &st) == 0 && S_ISREG (st.st_mode);
#endif
}

bool makeOneDir (const std::string& p)
{
#ifdef _WIN32
    if (CreateDirectoryW (toWide (p).c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir (p.c_str(), 0777) == 0 || errno == EEXIST;
#endif
}

bool createParentDirs (const std::string& path)
{
    const size_t slash = path.find_last_of ("/\\");
    if (slash == std::string::npos) return true;
    const std::string dir = path.substr (0, slash);

    // Create every prefix in turn ("C:\\Program Files\\SpectroCraft\\x" needs
    // both C:\Program Files and …\SpectroCraft). A bare drive root is skipped:
    // it always exists.
    std::string built;
    for (size_t i = 0; i < dir.size(); ++i)
    {
        const char c = dir[i];
        if (c != '/' && c != '\\') { built += c; continue; }
        if (! built.empty() && ! (built.size() == 2 && built[1] == ':'))
            if (! makeOneDir (built)) return false;
        built += c;
    }
    if (! built.empty() && ! (built.size() == 2 && built[1] == ':'))
        if (! makeOneDir (built)) return false;
    return true;
}

bool removeFile (const std::string& p)
{
#ifdef _WIN32
    return DeleteFileW (toWide (p).c_str()) != 0;
#else
    return remove (p.c_str()) == 0;
#endif
}

bool moveFile (const std::string& from, const std::string& to)
{
#ifdef _WIN32
    return MoveFileExW (toWide (from).c_str(), toWide (to).c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
#else
    return rename (from.c_str(), to.c_str()) == 0;
#endif
}

bool writeFile (const std::string& path, const std::string& text)
{
    if (! createParentDirs (path)) return false;
    std::ofstream out (path, std::ios::binary | std::ios::trunc);
    if (! out) return false;
    out.write (text.data(), (std::streamsize) text.size());
    out.flush();
    return out.good();
}

// ------------------------------------------------------------------ processes

bool processAlive (unsigned long pid)
{
    if (pid == 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess (SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid);
    if (h == nullptr) return false;              // already gone (or not ours)
    DWORD code = 0;
    const bool alive = GetExitCodeProcess (h, &code) != 0 && code == STILL_ACTIVE;
    CloseHandle (h);
    return alive;
#else
    if (kill ((pid_t) pid, 0) == 0) return true;
    return errno == EPERM;                        // exists, just not ours
#endif
}

bool waitForExit (unsigned long pid, int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now()
                       + std::chrono::milliseconds (timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (! processAlive (pid)) return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (200));
    }
    return ! processAlive (pid);
}

// Quote one argument for a CreateProcessW command line (same rules as the app:
// backslashes only matter immediately before a quote).
std::string quoteForCmd (const std::string& s)
{
    bool needs = s.empty();
    for (char c : s)
        if (c == ' ' || c == '\t' || c == '"') { needs = true; break; }
    if (! needs) return s;

    std::string q = "\"";
    size_t i = 0;
    while (i < s.size())
    {
        size_t nBack = 0;
        while (i + nBack < s.size() && s[i + nBack] == '\\') ++nBack;
        if (i + nBack < s.size() && s[i + nBack] == '"')
        {
            q.append (nBack * 2 + 1, '\\');
            q += '"';
        }
        else
        {
            q.append (nBack, '\\');
            if (i + nBack < s.size()) q += s[i + nBack];
        }
        i += nBack + 1;
    }
    return q + "\"";
}

bool relaunch (const std::string& exe, const std::vector<std::string>& args)
{
#ifdef _WIN32
    std::wstring cmd = toWide (quoteForCmd (exe));
    for (const auto& a : args) cmd += L" " + toWide (quoteForCmd (a));

    std::wstring mutableCmd = cmd;
    STARTUPINFOW si = {};
    si.cb = sizeof (si);
    PROCESS_INFORMATION pi = {};
    // Its own console and process group: it must survive this program exiting.
    const BOOL ok = CreateProcessW (nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                                    CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP,
                                    nullptr, nullptr, &si, &pi);
    if (! ok) return false;
    CloseHandle (pi.hProcess);
    CloseHandle (pi.hThread);
    return true;
#else
    std::vector<std::string> argv { exe };
    for (const auto& a : args) argv.push_back (a);

    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0)
    {
        std::vector<char*> cargv;
        for (auto& s : argv) cargv.push_back (s.data());
        cargv.push_back (nullptr);
        execv (argv[0].c_str(), cargv.data());
        _exit (127);
    }
    return true;
#endif
}

// ------------------------------------------------------------------ key file

// tools/updates/ed25519.key: a text file with
//   spectrocraft-update-key/1
//   seed=<64 hex>       (the private seed; the only secret)
//   public=<64 hex>     (informational; checked against the seed)
struct KeyFile
{
    bool ok = false;
    uint8_t seed[32] = {};
    std::string publicHex, error;
};

KeyFile readKeyFile (const std::string& path)
{
    KeyFile k;
    std::ifstream in (path, std::ios::binary);
    if (! in) { k.error = "cannot read " + path; return k; }

    std::string line, seedHex, publicHex;
    bool formatSeen = false;
    while (std::getline (in, line))
    {
        line = trim (line);
        if (line.empty() || line[0] == '#') continue;
        if (! formatSeen)
        {
            if (line != "spectrocraft-update-key/1")
            { k.error = "not a SpectroCraft key file: " + line; return k; }
            formatSeen = true;
            continue;
        }
        const size_t eq = line.find ('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim (line.substr (0, eq));
        const std::string val = trim (line.substr (eq + 1));
        if (key == "seed") seedHex = val;
        else if (key == "public") publicHex = val;
    }

    if (! formatSeen) { k.error = "the key file has no format line."; return k; }
    if (seedHex.empty()) { k.error = "the key file has no seed= line."; return k; }
    if (! sc::crypto::keyFromHex (seedHex, k.seed))
    { k.error = "the seed is not 32 bytes of hex."; return k; }

    uint8_t pub[32];
    if (! sc::crypto::ed25519PublicFromSeed (k.seed, pub))
    { k.error = "could not derive the public key."; return k; }
    k.publicHex = sc::crypto::toHex (pub, 32);

    if (! publicHex.empty() && publicHex != k.publicHex)
    { k.error = "the public= line does not match the seed."; return k; }

    k.ok = true;
    return k;
}

// ------------------------------------------------------------------ plan file

struct Plan
{
    bool ok = false;
    std::string version, error;
    std::vector<std::string> files;
};

Plan readPlan (const std::string& path)
{
    Plan p;
    std::ifstream in (path, std::ios::binary);
    if (! in) { p.error = "cannot read " + path; return p; }

    std::string line;
    bool formatSeen = false;
    while (std::getline (in, line))
    {
        line = trim (line);
        if (line.empty() || line[0] == '#') continue;
        if (! formatSeen)
        {
            if (line != "spectrocraft-update-plan/1")
            { p.error = "unsupported update plan: " + line; return p; }
            formatSeen = true;
            continue;
        }
        const size_t eq = line.find ('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim (line.substr (0, eq));
        const std::string val = trim (line.substr (eq + 1));
        if (key == "version") p.version = val;
        else if (key == "file")
        {
            if (! safeRelative (val)) { p.error = "the plan lists an invalid path: " + val; return p; }
            p.files.push_back (val);
        }
    }

    if (! formatSeen) { p.error = "the plan file has no format line."; return p; }
    if (p.files.empty()) { p.error = "the plan lists no files."; return p; }
    if (p.version.empty()) { p.error = "the plan has no version."; return p; }
    p.ok = true;
    return p;
}

void logLine (const std::string& text)
{
    std::printf ("%s\n", text.c_str());
    std::fflush (stdout);
}

// ------------------------------------------------------------------ the update

struct Options
{
    bool havePid = false;
    unsigned long pid = 0;
    std::string installDir, stagingDir, relaunchExe;
    bool noRelaunch = false;
    int waitMs = 60000;
};

bool writeResult (const std::string& installDir, const std::string& version, bool success,
                  const std::string& reason, const std::string& failedFile,
                  const std::vector<std::string>& replaced,
                  const std::vector<std::string>& oldFiles,
                  const std::vector<std::string>& cleanupDirs)
{
    std::ostringstream js;
    js << "{\n"
       << "  \"format\": \"spectrocraft-update-result/1\",\n"
       << "  \"success\": " << (success ? "true" : "false") << ",\n"
       << "  \"version\": \"" << jsonEscape (version) << "\",\n"
       << "  \"reason\": \"" << jsonEscape (reason) << "\",\n"
       << "  \"failedFile\": \"" << jsonEscape (failedFile) << "\",\n"
       << "  \"replaced\": " << jsonArray (replaced) << ",\n"
       << "  \"oldFiles\": " << jsonArray (oldFiles) << ",\n"
       << "  \"cleanupDirs\": " << jsonArray (cleanupDirs) << "\n"
       << "}\n";
    return writeFile (join (installDir, "update-result.json"), js.str());
}

int runUpdate (const Options& opt)
{
    const std::string planPath = join (opt.stagingDir, "update-plan.txt");
    logLine ("SpectroCraft updater");
    logLine ("  install dir : " + opt.installDir);
    logLine ("  staging dir : " + opt.stagingDir);

    const Plan plan = readPlan (planPath);
    if (! plan.ok)
    {
        logLine ("error: " + plan.error);
        writeResult (opt.installDir, "", false, plan.error, planPath, {}, {}, { opt.stagingDir });
        return 1;
    }

    logLine ("  version     : " + plan.version);
    logLine ("  files       : " + std::to_string (plan.files.size()));

    // 1. Wait for the app to close. Until it is gone no file can be touched:
    //    SpectroCraft.exe is one of the files being replaced.
    if (opt.havePid)
    {
        logLine ("waiting for SpectroCraft (pid " + std::to_string (opt.pid) + ") to close...");
        if (! waitForExit (opt.pid, opt.waitMs))
        {
            logLine ("error: SpectroCraft is still running after "
                   + std::to_string (opt.waitMs / 1000) + " s; nothing was changed.");
            writeResult (opt.installDir, plan.version, false,
                         "SpectroCraft did not close in time, so no file was replaced.",
                         "", {}, {}, { opt.stagingDir });
            return 1;
        }
    }

    // 2. Preflight: every staged file must be there before anything is moved.
    for (const auto& rel : plan.files)
    {
        const std::string staged = join (opt.stagingDir, rel);
        if (! fileExists (staged))
        {
            logLine ("error: the staged file is missing: " + rel);
            writeResult (opt.installDir, plan.version, false,
                         "The staged file is missing: " + rel, rel, {}, {}, { opt.stagingDir });
            return 1;
        }
    }

    // 3. Swap. Each old file is kept as <name>.old; the app deletes those on
    //    the next start (this program cannot delete the copy of itself it is
    //    running from).
    std::vector<std::string> replaced, oldFiles;
    std::vector<std::pair<std::string, std::string>> done;   // (backup, target)
    std::string failedFile, why;

    for (const auto& rel : plan.files)
    {
        const std::string staged = join (opt.stagingDir, rel);
        const std::string target = join (opt.installDir, rel);
        const std::string backup = target + ".old";

        if (fileExists (target) && ! moveFile (target, backup))
        {
            why = "could not move the installed file out of the way: " + rel;
            failedFile = rel;
            break;
        }

        if (! createParentDirs (target) || ! moveFile (staged, target))
        {
            why = "could not install: " + rel;
            failedFile = rel;
            if (fileExists (backup) && ! moveFile (backup, target))
                logLine ("warning: could not put back " + rel);
            break;
        }

        done.push_back ({ backup, target });
        replaced.push_back (rel);
        if (fileExists (backup)) oldFiles.push_back (backup);
        logLine ("  replaced " + rel);
    }

    bool success = failedFile.empty();

    // 4. Roll the finished moves back if one of them failed.
    if (! success)
    {
        logLine ("error: " + why);
        for (auto it = done.rbegin(); it != done.rend(); ++it)
        {
            const std::string& backup = it->first;
            const std::string& target = it->second;
            if (fileExists (target)) removeFile (target);
            if (fileExists (backup) && moveFile (backup, target))
                logLine ("  restored " + target);
        }
        replaced.clear();
        oldFiles.clear();
    }

    // 5. Report, then hand the app back to the user.
    const std::vector<std::string> cleanup { opt.stagingDir };
    writeResult (opt.installDir, plan.version, success,
                 success ? "" : why, failedFile, replaced, oldFiles, cleanup);

    if (! opt.noRelaunch && ! opt.relaunchExe.empty())
    {
        logLine (success ? "starting SpectroCraft " + plan.version + "..."
                         : "starting SpectroCraft (the update failed)...");
        if (! relaunch (opt.relaunchExe, { "--finish-update" }))
            logLine ("warning: could not start " + opt.relaunchExe
                   + " - open SpectroCraft yourself.");
    }

    logLine (success ? "update applied." : "update failed.");
    return success ? 0 : 1;
}

// ------------------------------------------------------------------ --sign

int runSign (const std::string& keyPath, const std::string& inPath, const std::string& outPath)
{
    const KeyFile key = readKeyFile (keyPath);
    if (! key.ok) { std::fprintf (stderr, "error: %s\n", key.error.c_str()); return 1; }

    std::ifstream in (inPath, std::ios::binary);
    if (! in) { std::fprintf (stderr, "error: cannot read %s\n", inPath.c_str()); return 1; }
    const std::string data ((std::istreambuf_iterator<char> (in)),
                             std::istreambuf_iterator<char>());

    uint8_t sig[64];
    if (! sc::crypto::ed25519Sign (key.seed, reinterpret_cast<const uint8_t*> (data.data()),
                                   data.size(), sig))
    {
        std::fprintf (stderr, "error: signing failed.\n");
        return 1;
    }

    const std::string hex = sc::crypto::toHex (sig, 64) + "\n";
    if (! writeFile (outPath, hex)) { std::fprintf (stderr, "error: cannot write %s\n", outPath.c_str()); return 1; }

    std::printf ("signed %s (%zu bytes) -> %s\n", inPath.c_str(), data.size(), outPath.c_str());
    std::printf ("public key: %s\n", key.publicHex.c_str());
    return 0;
}

int runVerify (const std::string& keyPath, const std::string& inPath, const std::string& sigPath)
{
    const KeyFile key = readKeyFile (keyPath);
    if (! key.ok) { std::fprintf (stderr, "error: %s\n", key.error.c_str()); return 1; }

    std::ifstream in (inPath, std::ios::binary);
    if (! in) { std::fprintf (stderr, "error: cannot read %s\n", inPath.c_str()); return 1; }
    const std::string data ((std::istreambuf_iterator<char> (in)),
                             std::istreambuf_iterator<char>());

    std::ifstream sin (sigPath, std::ios::binary);
    if (! sin) { std::fprintf (stderr, "error: cannot read %s\n", sigPath.c_str()); return 1; }
    const std::string sigText ((std::istreambuf_iterator<char> (sin)),
                                std::istreambuf_iterator<char>());

    uint8_t sig[64];
    if (! sc::crypto::sigFromHex (sigText, sig))
    { std::fprintf (stderr, "error: %s is not a 64-byte Ed25519 signature in hex.\n", sigPath.c_str()); return 1; }

    uint8_t pub[32];
    sc::crypto::ed25519PublicFromSeed (key.seed, pub);

    if (! sc::crypto::ed25519Verify (pub, reinterpret_cast<const uint8_t*> (data.data()),
                                      data.size(), sig))
    {
        std::fprintf (stderr, "error: %s is NOT signed by %s.\n", inPath.c_str(), key.publicHex.c_str());
        return 1;
    }
    std::printf ("signature OK: %s was signed by %s\n", inPath.c_str(), key.publicHex.c_str());
    return 0;
}

void printUsage()
{
    std::printf (
        "SpectroCraft updater\n\n"
        "  spectrocraft-updater --pid <n> --install-dir <dir> --staging-dir <dir>\n"
        "                       --relaunch <exe> [--no-relaunch] [--wait-ms <n>]\n"
        "      Apply the update staged in <dir> (update-plan.txt), write\n"
        "      update-result.json next to the app, and start the app again.\n\n"
        "  spectrocraft-updater --sign <keyfile> <file> <out.sig>\n"
        "      Ed25519-sign a file (CI signs update-manifest.json this way).\n\n"
        "  spectrocraft-updater --verify <keyfile> <file> <out.sig>\n"
        "      Check that signature against the key file's public key.\n\n"
        "  spectrocraft-updater --public <keyfile>\n"
        "      Print the public key belonging to a key file.\n");
}

} // namespace

int main (int argc, char** argv)
{
    std::vector<std::string> args (argv + 1, argv + argc);

    for (int i = 0; i < (int) args.size(); ++i)
    {
        const std::string a = args[i];

        if (a == "--help" || a == "-h") { printUsage(); return 0; }

        if (a == "--public")
        {
            if (i + 1 >= (int) args.size()) { printUsage(); return 2; }
            const KeyFile k = readKeyFile (args[i + 1]);
            if (! k.ok) { std::fprintf (stderr, "error: %s\n", k.error.c_str()); return 1; }
            std::printf ("%s\n", k.publicHex.c_str());
            return 0;
        }

        if (a == "--verify")
        {
            if (i + 3 >= (int) args.size()) { printUsage(); return 2; }
            return runVerify (args[i + 1], args[i + 2], args[i + 3]);
        }

        if (a == "--sign")
        {
            if (i + 3 >= (int) args.size()) { printUsage(); return 2; }
            return runSign (args[i + 1], args[i + 2], args[i + 3]);
        }
    }

    Options opt;
    for (int i = 0; i < (int) args.size(); ++i)
    {
        const std::string& a = args[i];
        auto next = [&]() -> std::string {
            return i + 1 < (int) args.size() ? args[++i] : std::string();
        };
        if (a == "--pid") { opt.pid = std::strtoul (next().c_str(), nullptr, 10); opt.havePid = true; }
        else if (a == "--install-dir") opt.installDir = next();
        else if (a == "--staging-dir") opt.stagingDir = next();
        else if (a == "--relaunch") opt.relaunchExe = next();
        else if (a == "--no-relaunch") opt.noRelaunch = true;
        else if (a == "--wait-ms") opt.waitMs = std::atoi (next().c_str());
        else { std::fprintf (stderr, "error: unknown argument %s\n", a.c_str()); printUsage(); return 2; }
    }

    if (opt.installDir.empty() || opt.stagingDir.empty())
    {
        std::fprintf (stderr, "error: --install-dir and --staging-dir are required.\n");
        printUsage();
        return 2;
    }

    return runUpdate (opt);
}
