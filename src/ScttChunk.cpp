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

bool readScttChunk(const juce::File& wavFile, ScttData& out, std::string* errOut)
{
    juce::FileInputStream in (wavFile);
    if (in.failedToOpen ()) return false;

    char hdr [12];
    if (in.read (hdr, 12) != 12
        || std::memcmp (hdr, "RIFF", 4) != 0
        || std::memcmp (hdr + 8, "WAVE", 4) != 0)
        return false;

    const int64_t total = in.getTotalLength ();
    while (in.getPosition () + 8 <= total)
    {
        char fourcc [4];
        uint32_t size = 0;
        if (!in.read (fourcc, 4) || !readU32le (in, &size)) break;

        if (std::memcmp (fourcc, "SCTT", 4) == 0)
        {
            std::string json ((size_t) size, '\0');
            if (in.read (json.data (), size) != (int) size) return false;
            while (!json.empty () && json.back () == '\0') json.pop_back ();   // RIFF padding

            ScttData d;
            if (!parseScttJson (json, d, errOut)) return false;
            out = std::move (d);
            return true;
        }

        // Skip the payload plus its word-alignment pad byte.
        const int64_t skip = (int64_t) size + ((size & 1) ? 1 : 0);
        if (!in.setPosition (in.getPosition () + skip)) break;
    }

    return false;   // no SCTT chunk present
}
