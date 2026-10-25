// SpectroCraft — SCTT RIFF chunk (see include/ScttChunk.h for the design notes).

#include "ScttChunk.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <juce_core/juce_core.h>

// ---------------------------------------------------------------------------
// JSON writer (fixed schema, compact output)
// ---------------------------------------------------------------------------

static void appendJsonString(std::string& out, const std::string& s)
{
    out += '"';
    for (const char c8 : s)
    {
        const unsigned char c = (unsigned char) c8;
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20)
                {
                    char buf [8];
                    std::snprintf (buf, sizeof (buf), "\\u%04x", c);
                    out += buf;
                }
                else
                    out += (char) c;   // raw UTF-8 passes through
        }
    }
    out += '"';
}

static void appendJsonNumber(std::string& out, double v)
{
    if (!std::isfinite (v)) { out += "0"; return; }
    char buf [32];
    // 3 decimal places is plenty for second-precision timestamps.
    std::snprintf (buf, sizeof (buf), "%.3f", v);
    out += buf;
}

std::string scttToJson(const ScttData& d)
{
    std::string out = "{\"version\":";
    appendJsonNumber (out, (double) d.version);
    out += ",\"segments\":[";
    for (size_t i = 0; i < d.segments.size(); ++i)
    {
        const ScttSegment& s = d.segments[i];
        if (i > 0) out += ',';
        out += "{\"start\":";
        appendJsonNumber (out, s.startSec);
        out += ",\"end\":";
        appendJsonNumber (out, s.endSec);
        out += ",\"text\":";
        appendJsonString (out, s.text);
        out += "}";
    }
    out += "]}";
    return out;
}

// ---------------------------------------------------------------------------
// JSON parser — small recursive-descent reader for the fixed schema. Tolerant
// of whitespace and unknown keys/values; strict about structure.
// ---------------------------------------------------------------------------

namespace {

struct JsonReader {
    const std::string& s;
    size_t pos = 0;

    explicit JsonReader(const std::string& str) : s (str) {}

    void skipWs()
    {
        while (pos < s.size() && std::isspace ((unsigned char) s[pos])) ++pos;
    }

    bool peek(char c)
    {
        skipWs ();
        return pos < s.size () && s[pos] == c;
    }

    bool eat(char c)
    {
        if (peek (c)) { ++pos; return true; }
        return false;
    }

    // Parse a JSON string value (caller positioned at the opening quote or ws).
    bool parseString(std::string& out)
    {
        skipWs ();
        if (!eat ('"')) return false;
        out.clear ();
        while (pos < s.size ())
        {
            const char c = s[pos++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }   // raw UTF-8 passes through

            if (pos >= s.size()) return false;
            const char e = s[pos++];
            switch (e)
            {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u':
                {
                    auto readCp = [&] (unsigned& cp) -> bool
                    {
                        if (pos + 4 > s.size ()) return false;
                        cp = 0;
                        for (int i = 0; i < 4; ++i)
                        {
                            const char h = s[pos++];
                            cp <<= 4;
                            if      (h >= '0' && h <= '9') cp |= (unsigned) (h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned) (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned) (h - 'A' + 10);
                            else return false;
                        }
                        return true;
                    };

                    unsigned hi = 0, lo = 0;
                    if (!readCp (hi)) return false;
                    if (hi >= 0xD800 && hi <= 0xDBFF)
                    {
                        // High surrogate: a low-surrogate \uXXXX must follow.
                        if (pos + 6 > s.size () || s[pos] != '\\' || s[pos + 1] != 'u')
                            return false;
                        pos += 2;
                        if (!readCp (lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return false;
                        appendUtf8 (out, 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00));
                    }
                    else
                        appendUtf8 (out, hi);
                    break;
                }
                default: return false;
            }
        }
        return false;   // unterminated string
    }

    bool parseNumber(double& out)
    {
        skipWs ();
        const char* p = s.c_str () + pos;
        char* end = nullptr;
        errno = 0;
        const double v = std::strtod (p, &end);
        if (end == p) return false;
        pos += (size_t) (end - p);
        out = v;
        return true;
    }

    // Skip any JSON value without interpreting it.
    bool skipValue()
    {
        skipWs ();
        if (pos >= s.size ()) return false;
        const char c = s[pos];
        if (c == '"')
        {
            std::string discard;
            return parseString (discard);
        }
        if (c == '{' || c == '[')
        {
            int depth = 0;
            bool inStr = false, esc = false;
            for (; pos < s.size (); ++pos)
            {
                const char ch = s[pos];
                if (inStr)
                {
                    if (esc) esc = false;
                    else if (ch == '\\') esc = true;
                    else if (ch == '"') inStr = false;
                }
                else if (ch == '"') inStr = true;
                else if (ch == '{' || ch == '[') ++depth;
                else if (ch == '}' || ch == ']')
                {
                    if (--depth == 0) return true;
                }
            }
            return false;
        }
        const size_t start = pos;
        while (pos < s.size () && s[pos] != ',' && s[pos] != '}' && s[pos] != ']'
               && !std::isspace ((unsigned char) s[pos]))
            ++pos;
        return pos > start;
    }

private:
    static void appendUtf8(std::string& out, unsigned cp)
    {
        if (cp < 0x80) out += (char) cp;
        else if (cp < 0x800)
        {
            out += (char) (0xC0 | (cp >> 6));
            out += (char) (0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += (char) (0xE0 | (cp >> 12));
            out += (char) (0x80 | ((cp >> 6) & 0x3F));
            out += (char) (0x80 | (cp & 0x3F));
        }
        else
        {
            out += (char) (0xF0 | (cp >> 18));
            out += (char) (0x80 | ((cp >> 12) & 0x3F));
            out += (char) (0x80 | ((cp >> 6) & 0x3F));
            out += (char) (0x80 | (cp & 0x3F));
        }
    }
};

} // namespace

bool parseScttJson(const std::string& json, ScttData& out, std::string* errOut)
{
    auto fail = [&] (const char* msg) -> bool
    {
        if (errOut) *errOut = msg;
        return false;
    };

    JsonReader r (json);
    if (!r.eat ('{')) return fail ("SCTT: expected JSON object");

    int version = 1;
    std::vector<ScttSegment> segments;
    bool haveSegments = false;

    while (true)
    {
        std::string key;
        if (!r.parseString (key)) return fail ("SCTT: bad member name");
        if (!r.eat (':')) return fail ("SCTT: expected ':'");

        if (key == "version")
        {
            double v = 1.0;
            if (!r.parseNumber (v)) return fail ("SCTT: bad version value");
            version = (int) v;
        }
        else if (key == "segments")
        {
            if (!r.eat ('[')) return fail ("SCTT: segments must be an array");
            while (true)
            {
                ScttSegment seg;
                if (r.peek (']')) { ++r.pos; break; }   // empty array / end
                if (!r.eat ('{')) return fail ("SCTT: segment must be an object");

                bool haveStart = false, haveEnd = false;
                while (true)
                {
                    std::string k2;
                    if (!r.parseString (k2)) return fail ("SCTT: bad segment key");
                    if (!r.eat (':')) return fail ("SCTT: expected ':' in segment");
                    if (k2 == "start")      { if (!r.parseNumber (seg.startSec)) return fail ("SCTT: bad start"); haveStart = true; }
                    else if (k2 == "end")   { if (!r.parseNumber (seg.endSec))   return fail ("SCTT: bad end");   haveEnd = true; }
                    else if (k2 == "text")  { if (!r.parseString (seg.text))     return fail ("SCTT: bad text"); }
                    else                    { if (!r.skipValue ())              return fail ("SCTT: bad value"); }

                    if (r.eat (',')) continue;
                    if (r.eat ('}')) break;
                    return fail ("SCTT: expected ',' or '}' in segment");
                }
                if (!haveStart || !haveEnd) return fail ("SCTT: segment missing start/end");
                segments.push_back (std::move (seg));

                if (r.eat (',')) continue;
                if (r.eat (']')) break;
                return fail ("SCTT: expected ',' or ']' in segments");
            }
            haveSegments = true;
        }
        else
        {
            if (!r.skipValue ()) return fail ("SCTT: bad value for unknown key");
        }

        if (r.eat (',')) continue;
        if (r.eat ('}')) break;
        return fail ("SCTT: expected ',' or '}' in object");
    }

    out.version = version;
    out.segments.clear ();
    if (haveSegments) out.segments = std::move (segments);
    return true;
}

// ---------------------------------------------------------------------------
// RIFF scanning
// ---------------------------------------------------------------------------

static bool readU32le(juce::InputStream& in, uint32_t* v)
{
    unsigned char b [4];
    if (in.read (b, 4) != 4) return false;
    *v = (uint32_t) b[0] | ((uint32_t) b[1] << 8) | ((uint32_t) b[2] << 16) | ((uint32_t) b[3] << 24);
    return true;
}

bool readRiffChunkPayload(const juce::File& wavFile, const char fourcc[4],
                          std::string& out, bool adtlList,
                          bool keepTrailingZeros)
{
    juce::FileInputStream in (wavFile);
    if (!wavFile.existsAsFile () || in.failedToOpen ()) return false;

    char hdr [12];
    if (in.read (hdr, 12) != 12
        || std::memcmp (hdr, "RIFF", 4) != 0
        || std::memcmp (hdr + 8, "WAVE", 4) != 0)
        return false;

    const int64_t total = in.getTotalLength ();
    while (in.getPosition () + 8 <= total)
    {
        char id [4];
        uint32_t size = 0;
        if (!in.read (id, 4) || !readU32le (in, &size)) break;   // truncated header

        bool match = std::memcmp (id, fourcc, 4) == 0;
        if (match && adtlList)
        {
            // LIST chunk: only manage sub-lists of type "adtl".
            char listType [4];
            const int64_t savePos = in.getPosition();   // at payload start
            match = size >= 8 && in.read (listType, 4) == 4
                    && std::memcmp (listType, "adtl", 4) == 0;
            if (!in.setPosition (savePos)) break;
        }

        if (match)
        {
            out.assign ((size_t) size, '\0');
            if (in.read (out.data (), size) != (int) size) return false;   // truncated payload
            if (!keepTrailingZeros)
                while (!out.empty () && out.back () == '\0') out.pop_back ();   // RIFF padding
            return true;
        }

        // Skip the payload plus its word-alignment pad byte.
        const int64_t skip = (int64_t) size + ((size & 1) ? 1 : 0);
        if (!in.setPosition (in.getPosition () + skip)) break;
    }

    return false;   // chunk not present
}

bool readScttChunk(const juce::File& wavFile, ScttData& out, std::string* errOut)
{
    std::string json;
    if (!readRiffChunkPayload (wavFile, "SCTT", json))
        return false;   // no SCTT chunk present

    ScttData d;
    if (!parseScttJson (json, d, errOut)) return false;
    out = std::move (d);
    return true;
}

bool replaceScttChunkInWav(const juce::File& wavFile, const std::string& json,
                           std::string* errOut)
{
    auto fail = [&] (const char* msg) -> bool
    {
        if (errOut != nullptr) *errOut = msg;
        return false;
    };

    juce::FileInputStream in (wavFile);
    if (!wavFile.existsAsFile () || in.failedToOpen ())
        return fail ("cannot open file for reading");

    char hdr [12];
    if (in.read (hdr, 12) != 12
        || std::memcmp (hdr, "RIFF", 4) != 0
        || std::memcmp (hdr + 8, "WAVE", 4) != 0)
        return fail ("not a RIFF/WAVE file");

    const int64_t total = in.getTotalLength ();

    // Temp sibling file: same folder so the final replace is atomic.
    const juce::File tmp = wavFile.getSiblingFile (wavFile.getFileName() + ".sctt-tmp");
    if (tmp.existsAsFile()) tmp.deleteFile();

    bool ok = true;
    {
    // Direct ctor: opens in append mode; the tmp file was deleted above, so
    // we start at offset 0. The stream is scoped so its handle closes BEFORE
    // the replace below — Windows refuses to move/replace a file that still
    // has an open handle without FILE_SHARE_DELETE (and JUCE's output handles
    // only allow sharing for read).
    juce::FileOutputStream out (tmp);
    if (!out.openedOk ())
        return fail ("cannot create temp file");

    // RIFF header with a placeholder size, fixed up at the end.
    out.write ("RIFF", 4);
    out.writeInt (0);
    out.write ("WAVE", 4);

    std::vector<uint8_t> buf;
    while (ok && in.getPosition () + 8 <= total)
    {
        char fourcc [4];
        uint32_t size = 0;
        if (!in.read (fourcc, 4) || !readU32le (in, &size)) { ok = false; break; }

        // Payload plus its word-alignment pad byte.
        const int64_t padded = (int64_t) size + ((size & 1) ? 1 : 0);
        if (std::memcmp (fourcc, "SCTT", 4) == 0)
        {
            // Drop the old SCTT chunk entirely; a fresh one is appended below.
            if (!in.setPosition (in.getPosition () + padded)) ok = false;
            continue;
        }

        out.write (fourcc, 4);
        out.writeInt ((int) size);
        if (padded > 0)
        {
            buf.resize ((size_t) padded);
            int64_t done = 0;
            while (done < padded && ok)
            {
                // Copy in 1 MiB slices so the buffer stays small even for huge chunks.
                int64_t n = padded - done;
                if (n > (int64_t) (1 << 20)) n = (int64_t) (1 << 20);
                const int got = in.read (buf.data() + (size_t) done, (int) n);
                if (got <= 0) { ok = false; break; }
                out.write (buf.data() + (size_t) done, (size_t) got);
                done += got;
            }
        }
    }

    // Append the new SCTT chunk (payload word-aligned with a zero pad byte).
    if (ok)
    {
        out.write ("SCTT", 4);
        out.writeInt ((int) json.size());
        // This JUCE build's OutputStream::write returns bool, not a byte count.
        if (!json.empty())
            ok = out.write (json.data(), json.size());
        if (ok && (json.size() & 1))
            ok = out.writeByte (0);
    }

    // Fix up the RIFF size field: total file length minus the 8-byte header.
    if (ok)
    {
        const int64_t fileSize = out.getPosition();
        ok = out.setPosition (4) && out.writeInt ((int) (fileSize - 8));
    }

    // Force the buffered data to disk before closing the handle.
    if (ok) out.flush ();
    }   // <- stream closed here; tmp is no longer open

    if (!ok)
    {
        tmp.deleteFile();
        return fail ("failed while rewriting the file");
    }

    // Atomic Win32 ReplaceFile: swaps tmp over wavFile without deleting the
    // destination first. (moveFileTo deletes it up front, which loses data if
    // the subsequent move fails — e.g. when the original is still open.)
    if (!tmp.replaceFileIn (wavFile))
    {
        tmp.deleteFile();
        return fail ("could not replace the original file");
    }

    return true;
}

// ---------------------------------------------------------------------------
// cue / LIST(adtl) payload builders - shared by the full WAV writer and the
// surgical patcher so both produce byte-identical legacy DAW layouts.
// ---------------------------------------------------------------------------

namespace {

void putU32le (std::string& out, uint32_t v)
{
    char b [4] = { static_cast<char> (v & 0xff),
                  static_cast<char> ((v >> 8) & 0xff),
                  static_cast<char> ((v >> 16) & 0xff),
                  static_cast<char> ((v >> 24) & 0xff) };
    out.append (b, 4);
}

void putU16le (std::string& out, uint16_t v)
{
    char b [2] = { static_cast<char> (v & 0xff), static_cast<char> ((v >> 8) & 0xff) };
    out.append (b, 2);
}

size_t adtlTextEntrySize (const std::string& utf8)
{
    const int len = static_cast<int> (utf8.size());
    return 8 + 4 + static_cast<size_t> (len) + ((len % 2 == 0) ? 1 : 0) + 1;
}

void appendAdtlTextEntry (std::string& out, const char* fourcc,
                          uint32_t cueId, const std::string& utf8)
{
    out.append (fourcc, 4);
    putU32le (out, static_cast<uint32_t> (5 + utf8.size()));
    putU32le (out, cueId);
    out += utf8;
    if (utf8.size() % 2 == 0) out += '\0';   // word-align the text
    out += '\0';                              // trailing NUL (legacy DAWs)
}

} // namespace

std::string buildCueChunkPayload(const std::vector<Marker>& markers,
                                 double sampleRate, int64_t totalSamples)
{
    if (markers.empty()) return {};

    auto startSampleOf = [&] (const Marker& m) -> uint32_t
    {
        return static_cast<uint32_t> (
            std::clamp (std::llround (m.start * sampleRate), int64_t(0), totalSamples));
    };

    std::string out;
    putU32le (out, static_cast<uint32_t> (markers.size()));
    for (const auto& m : markers)
    {
        const uint32_t pos = startSampleOf (m);
        // Legacy DAW cue entry (24 bytes): u32 id + u32 position + "data" +
        // u32 chunkStart(0) + u32 blockStart(0) + u32 sampleOffset (= position).
        putU32le (out, static_cast<uint32_t> (m.id));
        putU32le (out, pos);
        out.append ("data", 4);
        putU32le (out, 0);
        putU32le (out, 0);
        putU32le (out, pos);
    }
    return out;
}

std::string buildAdtlListPayload(const std::vector<Marker>& markers,
                                 double sampleRate, int64_t totalSamples)
{
    if (markers.empty()) return {};

    auto regionLengthOf = [&] (const Marker& m) -> uint32_t
    {
        if (m.end <= 0.0 || m.end <= m.start)
            return 0;   // point marker
        return static_cast<uint32_t> (
            std::clamp (std::llround ((m.end - m.start) * sampleRate), int64_t(0), totalSamples));
    };

    std::string out;
    out.append ("adtl", 4);

    // ltxt region-length entries first...
    for (const auto& m : markers)
    {
        out.append ("ltxt", 4);
        putU32le (out, 20);
        putU32le (out, static_cast<uint32_t> (m.id));
        putU32le (out, regionLengthOf (m));
        out.append ("rgn ", 4);
        putU16le (out, 0);   // country
        putU16le (out, 0);   // language
        putU16le (out, 0);   // dialect
        putU16le (out, 0);   // codepage
    }
    // ...then labl titles...
    for (const auto& m : markers)
        if (!m.name.isEmpty())
            appendAdtlTextEntry (out, "labl", static_cast<uint32_t> (m.id), m.name.toRawUTF8());
    // ...then note comments.
    for (const auto& m : markers)
        if (!m.comment.isEmpty())
            appendAdtlTextEntry (out, "note", static_cast<uint32_t> (m.id), m.comment.toRawUTF8());

    return out;
}

// ---------------------------------------------------------------------------
// Surgical multi-chunk RIFF rewrite.
// ---------------------------------------------------------------------------

bool patchRiffChunks(const juce::File& wavFile,
                     const std::vector<RiffPatchEntry>& entries,
                     std::string* errOut)
{
    auto fail = [&] (const char* msg) -> bool
    {
        if (errOut != nullptr) *errOut = msg;
        return false;
    };

    juce::FileInputStream in (wavFile);
    if (!wavFile.existsAsFile () || in.failedToOpen ())
        return fail ("cannot open file for reading");

    char hdr [12];
    if (in.read (hdr, 12) != 12
        || std::memcmp (hdr, "RIFF", 4) != 0
        || std::memcmp (hdr + 8, "WAVE", 4) != 0)
        return fail ("not a RIFF/WAVE file");

    const int64_t total = in.getTotalLength ();

    // Temp sibling file: same folder so the final replace is atomic.
    const juce::File tmp = wavFile.getSiblingFile (wavFile.getFileName() + ".scmeta-tmp");
    if (tmp.existsAsFile()) tmp.deleteFile();

    bool ok = true;
    {
    // Scoped stream: the handle must close before replaceFileIn below.
    juce::FileOutputStream out (tmp);
    if (!out.openedOk ())
        return fail ("cannot create temp file");

    out.write ("RIFF", 4);
    out.writeInt (0);   // placeholder, fixed up at the end
    out.write ("WAVE", 4);

    std::vector<uint8_t> buf;
    while (ok && in.getPosition () + 8 <= total)
    {
        char fourcc [4];
        uint32_t size = 0;
        if (!in.read (fourcc, 4) || !readU32le (in, &size)) { ok = false; break; }

        const int64_t padded = static_cast<int64_t> (size) + ((size & 1) ? 1 : 0);

        // Does this chunk match a managed entry? (LIST matches only "adtl" type.)
        bool managed = false;
        for (const auto& e : entries)
        {
            if (std::memcmp (e.fourcc, fourcc, 4) != 0) continue;
            if (!e.adtlList)
            {
                managed = true;
                break;
            }
            char listType [4];
            const int64_t savePos = in.getPosition();   // at payload start
            if (size >= 8 && in.read (listType, 4) == 4
                && std::memcmp (listType, "adtl", 4) == 0)
                managed = true;
            in.setPosition (savePos);   // always restore: the skip below uses it
        }

        if (managed)
        {
            // Drop the old chunk; a fresh one is appended below when wanted.
            if (!in.setPosition (in.getPosition () + padded)) ok = false;
            continue;
        }

        out.write (fourcc, 4);
        out.writeInt ((int) size);
        if (padded > 0)
        {
            buf.resize ((size_t) padded);
            int64_t done = 0;
            while (done < padded && ok)
            {
                int64_t n = padded - done;
                if (n > static_cast<int64_t> (1 << 20)) n = static_cast<int64_t> (1 << 20);
                const int got = in.read (buf.data() + (size_t) done, (int) n);
                if (got <= 0) { ok = false; break; }
                out.write (buf.data() + (size_t) done, (size_t) got);
                done += got;
            }
        }
    }

    // Append the fresh chunks (payload word-aligned with a zero pad byte).
    if (ok)
        for (const auto& e : entries)
        {
            if (!e.present || e.payload.empty()) continue;   // empty == remove
            out.write (e.fourcc, 4);
            out.writeInt ((int) e.payload.size());
            ok = out.write (e.payload.data(), e.payload.size());
            if (ok && (e.payload.size() & 1))
                ok = out.writeByte (0);
        }

    // Fix up the RIFF size field: total file length minus the 8-byte header.
    if (ok)
    {
        const int64_t fileSize = out.getPosition();
        ok = out.setPosition (4) && out.writeInt ((int) (fileSize - 8));
    }

    if (ok) out.flush ();
    }   // <- stream closed here; tmp is no longer open

    if (!ok)
    {
        tmp.deleteFile();
        return fail ("failed while rewriting the file");
    }

    if (!tmp.replaceFileIn (wavFile))
    {
        tmp.deleteFile();
        return fail ("could not replace the original file");
    }

    return true;
}
