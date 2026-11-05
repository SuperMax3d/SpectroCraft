/**
#include <cstdio>
 * SpectroCraft - Marker Manager Implementation
 */

#include "MarkerManager.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>

MarkerManager::MarkerManager() {}
MarkerManager::~MarkerManager() {}

void MarkerManager::sort()
{
    std::stable_sort (list.begin(), list.end(), [] (const Marker& a, const Marker& b)
    {
        return a.start < b.start;
    });
}

int MarkerManager::addPointMarker (double time, const juce::String& name)
{
    Marker m;
    m.id = nextId++;
    m.name = name.isNotEmpty() ? name : "Marker " + juce::String (m.id);
    m.start = time;
    m.end = -1.0;
    list.push_back (m);
    sort();
    if (onChange) onChange();
    return m.id;
}

int MarkerManager::addRegionMarker (double start, double end, const juce::String& name)
{
    if (end <= start) std::swap (start, end);
    Marker m;
    m.id = nextId++;
    m.name = name.isNotEmpty() ? name : "Region " + juce::String (m.id);
    m.start = start;
    m.end = end;
    list.push_back (m);
    sort();
    if (onChange) onChange();
    return m.id;
}

bool MarkerManager::removeMarker (int id)
{
    auto it = std::find_if (list.begin(), list.end(), [&] (const Marker& m) { return m.id == id; });
    if (it == list.end()) return false;
    list.erase (it);
    if (onChange) onChange();
    return true;
}

bool MarkerManager::renameMarker (int id, const juce::String& newName)
{
    for (auto& m : list)
        if (m.id == id) { m.name = newName; if (onChange) onChange(); return true; }
    return false;
}

bool MarkerManager::setComment (int id, const juce::String& newComment)
{
    for (auto& m : list)
        if (m.id == id) { m.comment = newComment; if (onChange) onChange(); return true; }
    return false;
}

bool MarkerManager::moveMarker (int id, double newTime)
{
    for (auto& m : list)
    {
        if (m.id == id)
        {
            const double delta = newTime - m.start;
            m.start = newTime;
            if (m.end >= 0) m.end += delta;
            sort();
            if (onChange) onChange();
            return true;
        }
    }
    return false;
}

bool MarkerManager::setRegion (int id, double start, double end)
{
    for (auto& m : list)
        if (m.id == id)
        {
            m.start = start;
            m.end = end;
            sort();
            if (onChange) onChange();
            return true;
        }
    return false;
}

bool MarkerManager::setPoint (int id)
{
    for (auto& m : list)
        if (m.id == id)
        {
            m.end = -1.0;
            if (onChange) onChange();
            return true;
        }
    return false;
}

bool MarkerManager::adjustForDeletion (double a, double b)
{
    if (!(b > a))
        return false;
    const double len = b - a;
    bool changed = false;

    for (size_t i = 0; i < list.size();)
    {
        Marker& m = list[i];
        if (m.end < 0.0)   // point marker at time m.start
        {
            const double t = m.start;
            if (t >= b)
            { m.start = t - len; changed = true; ++i; }
            else if (t >= a)
            { list.erase (list.begin() + (long long) i); changed = true; }   // inside [a, b): dropped
            else
                ++i;                                                          // before the deletion: unchanged
        }
        else                // region marker [m.start, m.end]
        {
            const double s = m.start, e = m.end;
            if (e <= a)
                ++i;                                                           // entirely before
            else if (s >= b)
            { m.start = s - len; m.end = e - len; changed = true; ++i; }       // entirely after: shift
            else if (s < a && e > b)
            { m.end = e - len; changed = true; ++i; }                          // straddles the whole deletion:
                                                                               // both halves stay contiguous -> [s, e-len]
            else if (s < a)
            { m.end = a; changed = true; ++i; }                                // right part deleted -> [s, a]
            else if (e <= b)
            { list.erase (list.begin() + (long long) i); changed = true; }     // fully contained: dropped
            else
            { m.start = a; m.end = e - len; changed = true; ++i; }             // left part deleted:
                                                                               // old [b, e] -> new [a, e-len]
        }
    }
    if (changed && onChange) onChange();
    return changed;
}

bool MarkerManager::adjustForStretch (double a, double bOld, double bNew)
{
    if (!(bOld > a))
        return false;
    const double k = (bNew - a) / (bOld - a);
    const double shiftAfter = bNew - bOld;
    auto W = [a, bOld, k, shiftAfter] (double t) -> double
    {
        if (t < a) return t;
        if (t <= bOld) return a + (t - a) * k;
        return t + shiftAfter;
    };

    bool changed = false;
    for (auto& m : list)
    {
        const double ns = W (m.start);
        const double ne = (m.end < 0.0) ? -1.0 : W (m.end);
        if (ns != m.start || ne != m.end)
        {
            m.start = ns;
            m.end = ne;
            changed = true;
        }
    }
    if (changed && onChange) onChange();
    return changed;
}

bool MarkerManager::adjustForInsertion (double t, double lenSecs)
{
    if (!(lenSecs > 0.0))
        return false;

    bool changed = false;
    for (auto& m : list)
    {
        if (m.end < 0.0)   // point marker at time m.start
        {
            // A marker sitting on the insertion position stays there: it marks
            // the start of the inserted data, not its end. The tiny tolerance
            // absorbs sample-boundary rounding when an edit position is derived
            // from a sample count (insertPos / rate) rather than the same
            // seconds double the marker was created with; such markers are
            // snapped exactly onto t so they stay aligned with the playhead.
            if (std::abs (m.start - t) <= 1e-4)
            {
                if (m.start != t) { m.start = t; changed = true; }
            }
            else if (m.start > t)
            { m.start += lenSecs; changed = true; }
        }
        else                // region marker [m.start, m.end]
        {
            const double s = m.start, e = m.end;
            if (e <= t)
                continue;                                                          // entirely before
            else if (s >= t)
            { m.start = s + lenSecs; m.end = e + lenSecs; changed = true; }        // shift right
            else
            { m.end = e + lenSecs; changed = true; }                               // straddles t:
                                                                                   // end follows its content
        }
    }
    if (changed && onChange) onChange();
    return changed;
}

void MarkerManager::restore (const std::vector<Marker>& m)
{
    list = m;
    sort();
    nextId = 1;
    for (const auto& mk : list)
        if (mk.id >= nextId)
            nextId = mk.id + 1;
    // Undo/redo and merges restore marker lists this way; FileDoc clears its
    // dirty flag at the end of load() so plain loading does not count.
    if (onChange) onChange();
}

const Marker* MarkerManager::findNear (double time, double tolSecs) const
{
    const Marker* best = nullptr;
    double bestDist = tolSecs;
    for (const auto& m : list)
    {
        const double d = m.end >= 0
            ? (time < m.start ? m.start - time : (time > m.end ? time - m.end : 0.0))
            : std::abs (m.start - time);
        if (d <= bestDist)
        {
            bestDist = d;
            best = &m;
        }
    }
    return best;
}

static juce::String jsonEscape (const juce::String& s)
{
    juce::String out;
    for (auto c : s)
    {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

bool MarkerManager::saveSidecar (const juce::File& audioFile) const
{
    // Brand-new documents have no file path yet: there is nowhere to put a
    // sidecar, so skip it rather than writing into the working directory.
    if (audioFile.getFullPathName().isEmpty())
        return false;

    const juce::File sidecar (audioFile.getFullPathName() + ".markers.json");
    juce::String s = "{\n  \"markers\": [\n";
    for (const auto& m : list)
    {
        s += "    { \"id\": " + juce::String (m.id)
           + ", \"name\": \"" + jsonEscape (m.name) + "\""
           + ", \"comment\": \"" + jsonEscape (m.comment) + "\""
           + ", \"start\": " + juce::String (m.start, 6)
           + ", \"end\": " + juce::String (m.end, 6)
           + ", \"colour\": " + juce::String ((int) m.colour.getARGB ())
           + " },\n";
    }
    if (!list.empty())
        s = s.dropLastCharacters (2);
    s += "\n ]\n}\n";
    sidecar.deleteFile(); // FileOutputStream appends to existing files!
    juce::FileOutputStream out (sidecar);
    if (out.failedToOpen())
        return false;
    return out.write (s.getCharPointer(), s.getNumBytesAsUTF8());
}

bool MarkerManager::loadSidecar (const juce::File& audioFile)
{
    // Always start from a clean slate: loading a file without a sidecar must
    // not leave the previous file's markers behind.
    list.clear();
    nextId = 1;

    const juce::File sidecar (audioFile.getFullPathName() + ".markers.json");
    const juce::String s = sidecar.loadFileAsString();
    if (s.isEmpty())
        return false;

    // Accept both the signed colour form this app writes (-2147483648 ..
    // 2147483647) and plain unsigned ARGB (0 .. 4294967295). The normalisation
    // keeps the value inside int range so the cast is exact instead of
    // saturating on x86-64.
    auto colourFrom = [] (double cv)
    {
        if (cv >= 2147483648.0)
            cv -= 4294967296.0;
        if (!(cv >= -2147483648.0 && cv <= 2147483647.0))
            return Marker().colour;   // out of range / NaN: keep the default
        return juce::Colour ((uint32) (int) cv);
    };

    // saveSidecar writes JSON with escaped strings: parse it with juce::JSON so
    // the escapes are decoded, braces inside names/comments are not taken for
    // the end of the object and times keep their full double precision.
    const juce::var root = juce::JSON::parse (s);
    if (const auto* obj = root.getDynamicObject())
    {
        const juce::var arrVar = obj->getProperties().getWithDefault ("markers", juce::var());
        if (const auto* arr = arrVar.isArray() ? arrVar.getArray() : nullptr)
        {
            for (int i = 0; i < arr->size(); ++i)
            {
                const auto* o = (*arr)[i].getDynamicObject();
                if (o == nullptr)
                    continue;
                const auto& p = o->getProperties();
                if (!p.contains ("id"))
                    continue;   // same rule as the tolerant scanner below

                Marker m;
                m.id      = (int) static_cast<double> (p.getWithDefault ("id", 0));
                m.name    = p.getWithDefault ("name", "").toString();
                m.comment = p.getWithDefault ("comment", "").toString();
                m.start   = static_cast<double> (p.getWithDefault ("start", 0.0));
                m.end     = static_cast<double> (p.getWithDefault ("end", -1.0));
                if (p.contains ("colour"))
                    m.colour = colourFrom (static_cast<double> (p.getWithDefault ("colour", 0)));

                if (m.id >= nextId) nextId = m.id + 1;
                list.push_back (m);
            }
            sort();
            return true;
        }
    }

    // Fallback for sidecars juce::JSON rejects (e.g. a non-finite time written
    // as "nan" by an old version): tolerant scan of the UTF-8 bytes, aware of
    // quoted strings and escapes. Succeeds only when a marker was recovered.
    const std::string u = s.toStdString();
    const size_t npos = std::string::npos;

    auto objectEnd = [&u, npos] (size_t open) -> size_t
    {
        bool inStr = false;
        for (size_t i = open + 1; i < u.size(); ++i)
        {
            const char c = u[i];
            if (inStr)
            {
                if (c == '\\') ++i;
                else if (c == '"') inStr = false;
            }
            else if (c == '"') inStr = true;
            else if (c == '}') return i;
        }
        return npos;
    };
    auto keyAt = [&u, npos] (const char* key, size_t a, size_t b) -> size_t
    {
        const size_t at = u.find (key, a);
        return (at == npos || at >= b) ? npos : at + std::strlen (key);
    };
    auto keyNum = [&] (const char* key, size_t a, size_t b, double& value) -> bool
    {
        size_t q = keyAt (key, a, b);
        if (q == npos) return false;
        while (q < b && (u[q] == ':' || u[q] == ' ')) ++q;
        const size_t st = q;
        while (q < b && (u[q] == '-' || u[q] == '+' || u[q] == '.' || u[q] == 'e' || u[q] == 'E'
                         || (u[q] >= '0' && u[q] <= '9')))
            ++q;
        value = juce::String::fromUTF8 (u.data() + st, (int) (q - st)).getDoubleValue();
        return true;
    };
    auto keyStr = [&] (const char* key, size_t a, size_t b, juce::String& value) -> bool
    {
        size_t q = keyAt (key, a, b);
        if (q == npos) return false;
        while (q < b && u[q] != '"') ++q;   // opening quote of the value
        std::string v;
        for (++q; q < b; ++q)
        {
            const char c = u[q];
            if (c == '"')
            {
                value = juce::String::fromUTF8 (v.data(), (int) v.size());
                return true;
            }
            if (c == '\\' && q + 1 < b)
            {
                const char e = u[++q];
                v += e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e;
            }
            else
                v += c;
        }
        return false;
    };

    size_t pos = 0;
    while (true)
    {
        const size_t open = u.find ('{', pos);
        if (open == npos) break;
        const size_t close = objectEnd (open);
        if (close == npos) break;

        Marker m;
        double v = 0;
        if (keyNum ("\"id\"", open, close, v))
        {
            m.id = (int) v;
            if (keyNum ("\"start\"", open, close, v)) m.start = v;
            if (keyNum ("\"end\"", open, close, v))   m.end = v;
            keyStr ("\"name\"", open, close, m.name);
            keyStr ("\"comment\"", open, close, m.comment);
            if (keyNum ("\"colour\"", open, close, v))
                m.colour = colourFrom (v);

            if (m.id >= nextId) nextId = m.id + 1;
            list.push_back (m);
        }
        pos = close + 1;
    }
    sort();
    return !list.empty();
}

// Reads markers embedded in a WAV file using the classic DAW RIFF marker layout:
//   "cue" chunk : u32 count + N x {u32 id, u32 position, "data", u32, u32,
//                 u32 sampleOffset} (24-byte cue points; 16-byte entries are
//                 also accepted)
//   LIST/"adtl" : one ltxt sub-chunk per marker (region length in samples),
//                 then labl (title) and note (comment) text entries keyed by
//                 cue id. Text entries declare size 5+len; even-length texts
//                 carry one alignment NUL beyond the declared size.
// The file is scanned chunk-by-chunk with seeks, so multi-GB files never get
// loaded into memory. Returns true if at least one marker was recovered.
bool MarkerManager::loadFromWavFile (const juce::File& wavFile, double sampleRate)
{
    list.clear();
    nextId = 1;

    auto in = std::make_unique<juce::FileInputStream> (wavFile);
    if (in->failedToOpen() || sampleRate <= 0.0)
        return false;

    char hdr[12] = {};
    if (in->read (hdr, 12) != 12
            || std::memcmp (hdr, "RIFF", 4) != 0
            || std::memcmp (hdr + 8, "WAVE", 4) != 0)
        return false;

    std::unordered_map<int, uint32_t> cueStarts;   // id -> start sample
    std::unordered_map<int, uint32_t> regionLens;  // id -> length in samples (0 = point)
    std::unordered_map<int, juce::String> titles;
    std::unordered_map<int, juce::String> comments;

    auto readU32 = [&in] () -> int64_t
    {
        uint32_t v = 0;
        return in->read (&v, 4) == 4 ? (int64_t) v : -1;
    };
    auto readFourCC = [&in] (char out[5]) -> bool
    {
        if (in->read (out, 4) != 4)
            return false;
        out[4] = 0;
        return true;
    };
    const int64_t total = in->getTotalLength();

    while (in->getPosition() + 8 <= total)
    {
        char cid[5];
        if (!readFourCC (cid))
            break;
        const int64_t csize = readU32 ();
        if (csize < 0)
            break;

        // Every chunk ends at a known offset (payload + RIFF word-alignment pad
        // byte for odd sizes). Jumping there after each chunk keeps the scan
        // aligned even when a chunk is odd-sized or only partly parsed. A size
        // larger than what is left in the file is never used to allocate.
        const int64_t payloadStart = in->getPosition();
        const int64_t avail = total - payloadStart;
        const int64_t next = payloadStart + csize + (csize & 1);

        if (std::strcmp (cid, "cue ") == 0 && csize >= 8 && csize <= avail)
        {
            const uint32_t n = (uint32_t) readU32 ();
            // Legacy DAW exports and standard RIFF use 24-byte cue points;
            // 16-byte entries (position at offset 12) are also accepted.
            if (n > 0 && n <= 100000
                    && (csize == 4 + 24LL * n || csize == 4 + 16LL * n))
            {
                const int64_t entrySize = (csize - 4) / (int64_t) n;
                std::vector<uint8_t> body ((size_t) (csize - 4));
                if (in->read (body.data(), (int) body.size()) == (int) body.size())
                    for (uint32_t i = 0; i < n; ++i)
                    {
                        const auto* e = body.data() + entrySize * (int64_t) i;
                        uint32_t id, start;
                        std::memcpy (&id, e, 4);
                        if (entrySize == 24)
                        {
                            // dwSampleOffset (+20) is the sample position inside
                            // the "data" chunk; dwPosition (+4) is the play-order
                            // position, which some writers leave at 0 (others
                            // fill only it).
                            uint32_t position, sampleOffset;
                            std::memcpy (&position, e + 4, 4);
                            std::memcpy (&sampleOffset, e + 20, 4);
                            const bool inData = std::memcmp (e + 8, "data", 4) == 0;
                            start = (inData && sampleOffset != 0) ? sampleOffset : position;
                        }
                        else
                            std::memcpy (&start, e + 12, 4);
                        cueStarts[(int) id] = start;
                    }
            }
        }
        else if (std::strcmp (cid, "LIST") == 0 && csize >= 8 && csize <= avail)
        {
            char listType[5];
            if (readFourCC (listType) && std::strcmp (listType, "adtl") == 0)
            {
                const int64_t remaining = csize - 4;
                int64_t pos = 0;
                while (pos + 8 <= remaining)
                {
                    char sid[5];
                    if (!readFourCC (sid))
                        break;
                    const int64_t ssize = readU32 ();
                    if (ssize < 0 || pos + 8 + ssize > remaining)
                        break; // truncated or malformed: stop parsing this list
                    const int64_t subStart = in->getPosition();

                    if (std::strcmp (sid, "ltxt") == 0 && ssize >= 12)
                    {
                        uint32_t id = 0, len = 0;
                        in->read (&id, 4);
                        in->read (&len, 4);
                        regionLens[(int) id] = len;
                    }
                    else if ((std::strcmp (sid, "labl") == 0 || std::strcmp (sid, "note") == 0)
                             && ssize >= 5 && ssize - 4 <= (1 << 20))
                    {
                        uint32_t id = 0;
                        in->read (&id, 4);
                        // Declared size covers cueID + text (+ NUL); the extra
                        // zero keeps fromUTF8 inside the buffer when a writer
                        // omits the terminator.
                        const int textBytes = (int) (ssize - 4);
                        std::vector<char> tb ((size_t) textBytes + 1, '\0');
                        if (in->read (tb.data(), textBytes) != textBytes)
                            break;
                        const juce::String text = juce::String::fromUTF8 (tb.data());
                        if (std::strcmp (sid, "labl") == 0)
                            titles[(int) id] = text;
                        else
                            comments[(int) id] = text;
                    }

                    pos += 8 + ssize;

                    // Odd-sized sub-chunks carry one alignment NUL beyond the
                    // declared size. Some writers omit it, so only consume a
                    // byte that really is a zero pad.
                    if ((ssize & 1) != 0 && pos < remaining)
                    {
                        char pad = 1;
                        if (in->setPosition (subStart + ssize) && in->read (&pad, 1) == 1 && pad == 0)
                            ++pos;
                    }
                    if (!in->setPosition (payloadStart + 4 + pos))
                        break;
                }
            }
        }

        if (!in->setPosition (next))
            break;
    }

    if (cueStarts.empty())
        return false;

    for (const auto& [id, startSample] : cueStarts)
    {
        Marker m;
        m.id = nextId++;
        m.start = (double) startSample / sampleRate;

        const auto lenIt = regionLens.find (id);
        if (lenIt != regionLens.end() && lenIt->second > 0)
            m.end = ((double) startSample + (double) lenIt->second) / sampleRate;
        else
            m.end = -1.0; // point marker

        const auto nameIt = titles.find (id);
        if (nameIt != titles.end())
            m.name = nameIt->second;
        if (m.name.isEmpty())
            m.name = "Marker " + juce::String (m.id);

        const auto comIt = comments.find (id);
        if (comIt != comments.end())
            m.comment = comIt->second;

        list.push_back (m);
    }

    sort();
    return true;
}

// ---------------------------------------------------------------- CSV export/import

namespace {

juce::String csvField (const juce::String& s)
{
    // Flatten newlines/tabs so each marker stays on one line, then quote per
    // RFC 4180 when the field contains a comma or a double quote.
    const auto flat = s.replace ("\r\n", " ").replace ("\n", " ")
                       .replace ("\r", " ").replace ("\t", " ");
    if (flat.containsChar (',') || flat.containsChar ('"'))
        return '"' + flat.replace ("\"", "\"\"") + '"';
    return flat;
}

std::vector<juce::String> csvLineFields (const juce::String& line, juce_wchar sep)
{
    std::vector<juce::String> out;
    juce::String cur;
    bool inQuotes = false;
    // Walk the characters with a pointer: String::operator[] is O(index) on
    // this build's UTF-8 strings.
    for (auto p = line.getCharPointer(); !p.isEmpty();)
    {
        const juce_wchar c = p.getAndAdvance();
        if (inQuotes)
        {
            if (c == '"')
            {
                if (*p == '"') { cur << '"'; ++p; }
                else inQuotes = false;
            }
            else
                cur << c;
        }
        else if (c == '"' && cur.isEmpty())
        {
            inQuotes = true;   // quotes only start a field at its beginning
        }
        else if (c == sep)
        {
            out.push_back (cur);
            cur.clear();
        }
        else
            cur << c;
    }
    out.push_back (cur);
    return out;
}

// Field separator of a CSV file, guessed from its first non-empty line:
// ';' (spreadsheets in decimal-comma locales such as fr-CA) when that line
// has at least as many unquoted ';' as ',', otherwise ','.
juce_wchar csvSeparator (const juce::StringArray& lines)
{
    for (const auto& line : lines)
    {
        if (line.trim().isEmpty())
            continue;
        int commas = 0, semis = 0;
        bool inQuotes = false;
        for (auto p = line.getCharPointer(); !p.isEmpty();)
        {
            const juce_wchar c = p.getAndAdvance();
            if (c == '"') inQuotes = !inQuotes;
            else if (!inQuotes && c == ',') ++commas;
            else if (!inQuotes && c == ';') ++semis;
        }
        return (semis > 0 && semis >= commas) ? (juce_wchar) ';' : (juce_wchar) ',';
    }
    return ',';
}

double csvTime (const juce::String& text)
{
    // Accepts plain seconds ("12.5", or "12,5" with a decimal comma) or a time
    // code: m:ss ("0:04.536", "1:02") or h:mm:ss ("1:02:03.5"). Anything that
    // is not a number (e.g. a header cell) gives NaN so the line is skipped.
    juce::StringArray parts;
    parts.addTokens (text.trim(), ":", "");
    if (parts.isEmpty() || parts.size() > 3)
        return std::numeric_limits<double>::quiet_NaN();

    double seconds = 0.0;
    for (auto part : parts)
    {
        part = part.trim();
        if (!part.containsChar ('.') && part.containsChar (','))
            part = part.replaceCharacter (',', '.');
        if (!part.containsAnyOf ("0123456789") || !part.containsOnly ("0123456789.+-eE"))
            return std::numeric_limits<double>::quiet_NaN();
        seconds = seconds * 60.0 + part.getDoubleValue();
    }
    return seconds;
}

} // namespace

bool MarkerManager::saveCsv (const juce::File& file) const
{
    juce::String out = "Title,Begin,End,Comment";
    for (const auto& m : list)
        out << "\r\n" << csvField (m.name)
            << ',' << juce::String (m.start, 6)
            << ',' << (m.end < 0 ? "-" : juce::String (m.end, 6))
            << ',' << csvField (m.comment);

    return file.replaceWithText (out);
}

int MarkerManager::loadCsv (const juce::File& file, double durationSec)
{
    juce::String text = file.loadFileAsString();
    if (text.length() > 0 && text[0] == 0xFEFF)   // strip a UTF-8 BOM if present
        text = text.substring (1);

    juce::StringArray lines;
    lines.addLines (text);
    const juce_wchar sep = csvSeparator (lines);

    int added = 0;
    for (int li = 0; li < lines.size(); ++li)
    {
        const auto line = lines[li].trim();
        if (line.isEmpty())
            continue;

        const auto fields = csvLineFields (line, sep);
        if (fields.size() < 2)   // need at least Title + Begin
            continue;

        if (li == 0 && fields[0].trim().equalsIgnoreCase ("Title"))
            continue;            // header row

        juce::String name = fields[0].trim();
        const juce::String bTok = fields[1].trim();
        if (bTok.isEmpty())
            continue;             // no time at all -> malformed line
        const double beginRaw = csvTime (bTok);
        if (!(beginRaw >= 0.0))   // NaN or negative -> malformed line
            continue;
        const double begin = std::min (beginRaw, durationSec);

        double end = -1.0;
        if (fields.size() > 2)
        {
            const juce::String eTok = fields[2].trim();
            if (!eTok.isEmpty() && eTok != "-")
            {
                const double e = csvTime (eTok);
                if (e > begin + 1e-9)
                    end = std::min (e, durationSec);
            }
        }

        juce::String comment;
        if (fields.size() > 3)
            comment = fields[3].trim();

        const juce::String label = name.isEmpty() ? "Marker" : name;
        const int id = (end < 0.0) ? addPointMarker (begin, label)
                                   : addRegionMarker (begin, end, label);
        if (!comment.isEmpty())
            setComment (id, comment);
        ++added;
    }

    if (added > 0)
    {
        sort();
        if (onChange) onChange();   // CSV import is a user-initiated change
    }
    return added;
}
