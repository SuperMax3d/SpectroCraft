/**
#include <cstdio>
 * SpectroCraft - Marker Manager Implementation
 */

#include "MarkerManager.h"
#include <algorithm>
#include <cmath>
#include <cstring>
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
    return m.id;
}

bool MarkerManager::removeMarker (int id)
{
    auto it = std::find_if (list.begin(), list.end(), [&] (const Marker& m) { return m.id == id; });
    if (it == list.end()) return false;
    list.erase (it);
    return true;
}

bool MarkerManager::renameMarker (int id, const juce::String& newName)
{
    for (auto& m : list)
        if (m.id == id) { m.name = newName; return true; }
    return false;
}

bool MarkerManager::setComment (int id, const juce::String& newComment)
{
    for (auto& m : list)
        if (m.id == id) { m.comment = newComment; return true; }
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

    auto findKeyNum = [&s] (const juce::String& key, size_t objStart, size_t objEnd, double& value) -> bool
    {
        const auto at = s.indexOf ((int) objStart, key);
        if (at < 0 || (size_t) at >= objEnd)
            return false;
        size_t pos = (size_t) at + (size_t) key.length();
        while (pos < objEnd && (s[(int) pos] == ':' || s[(int) pos] == ' '))
            ++pos;
        const auto start = pos;
        while (pos < objEnd && (s[(int) pos] == '-' || s[(int) pos] == '.' || s[(int) pos] == 'e'
               || s[(int) pos] == 'E'
               || (s[(int) pos] >= '0' && s[(int) pos] <= '9')))
            ++pos;
        value = s.substring ((int) start, (int) pos).getFloatValue();
        return true;
    };

    size_t pos = 0;
    while (true)
    {
        const auto open = s.indexOfChar ((int) pos, '{');
        if (open < 0) break;
        const auto close = s.indexOfChar ((int) open + 1, '}');
        if (close < 0) break;
        const size_t objStart = (size_t) open, objEnd = (size_t) close;

        Marker m;
        double v = 0;
        if (!findKeyNum ("\"id\"", objStart, objEnd, v))
        {
            pos = objEnd + 1;
            continue;
        }
        m.id = (int) v;
        findKeyNum ("\"start\"", objStart, objEnd, v); m.start = v;
        findKeyNum ("\"end\"", objStart, objEnd, v); m.end = v;

        const auto nameAt = s.indexOf ((int) objStart, "\"name\"");
        if (nameAt >= 0 && (size_t) nameAt < objEnd)
        {
            size_t q = (size_t) nameAt + 6;
            while (q < objEnd && s[(int) q] != '"') ++q;
            ++q;
            const auto qEnd = s.indexOfChar ((int) q, '"');
            if (qEnd >= 0 && (size_t) qEnd < objEnd)
                m.name = s.substring ((int) q, (int) qEnd);
        }

        const auto colAt = s.indexOf ((int) objStart, "\"colour\"");
        if (colAt >= 0 && (size_t) colAt < objEnd)
        {
            double cv = 0;
            if (findKeyNum ("\"colour\"", objStart, objEnd, cv))
                m.colour = juce::Colour ((uint32) (int) cv);
        }

        const auto comAt = s.indexOf ((int) objStart, "\"comment\"");
        if (comAt >= 0 && (size_t) comAt < objEnd)
        {
            size_t q = (size_t) comAt + 9;
            while (q < objEnd && s[(int) q] != '"') ++q;
            ++q;
            const auto qEnd = s.indexOfChar ((int) q, '"');
            if (qEnd >= 0 && (size_t) qEnd < objEnd)
                m.comment = s.substring ((int) q, (int) qEnd);
        }

        if (m.id >= nextId) nextId = m.id + 1;
        list.push_back (m);
        pos = objEnd + 1;
    }
    sort();
    return true;
}

// Reads markers embedded in a WAV file using the classic DAW RIFF marker layout:
//   "cue" chunk : u32 count + N x {u32 id, u32 startSample, "data", u32, u32,
//                 u32} (standard RIFF's 16-byte entries are also accepted)
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
    auto skip = [&in] (int64_t n) -> bool
    {
        return in->setPosition (in->getPosition() + n);
    };

    while (!in->isExhausted())
    {
        char cid[5];
        if (!readFourCC (cid))
            break;
        const int64_t csize = readU32 ();
        if (csize < 0)
            break;

        if (std::strcmp (cid, "cue ") == 0 && csize >= 8)
        {
            const uint32_t n = (uint32_t) readU32 ();
            // Legacy DAW exports use 24-byte entries; standard RIFF uses 16-byte ones.
            if ((n <= 100000 && csize == 4 + 24LL * n)
                    || (n <= 100000 && csize == 4 + 16LL * n))
            {
                const int64_t entrySize = (csize - 4) / (int64_t) n;
                std::vector<uint8_t> body ((size_t) (csize - 4));
                if (in->read (body.data(), body.size()) == (int64_t) body.size())
                    for (uint32_t i = 0; i < n; ++i)
                    {
                        const auto* e = body.data() + entrySize * (int64_t) i;
                        uint32_t id, start;
                        std::memcpy (&id, e, 4);
                        std::memcpy (&start, entrySize == 24 ? e + 4 : e + 12, 4);
                        cueStarts[(int) id] = start;
                    }
            }
            else
                skip (csize - 4); // malformed or unknown layout: skip
        }
        else if (std::strcmp (cid, "LIST") == 0 && csize >= 8)
        {
            char listType[5];
            if (!readFourCC (listType))
                break;
            const int64_t remaining = csize - 4;

            if (std::strcmp (listType, "adtl") != 0)
            {
                skip (remaining); // some other list type: skip
                continue;
            }

            int64_t pos = 0;
            while (pos + 8 <= remaining && !in->isExhausted())
            {
                char sid[5];
                if (!readFourCC (sid))
                    break;
                const int64_t ssize = readU32 ();
                if (ssize < 0 || pos + 8 + ssize > remaining)
                    break; // truncated or malformed: stop parsing this list

                if (std::strcmp (sid, "ltxt") == 0 && ssize >= 12)
                {
                    uint32_t id = 0, len = 0;
                    in->read (&id, 4);
                    in->read (&len, 4);
                    regionLens[(int) id] = len;
                    skip (ssize - 8);
                }
                else if ((std::strcmp (sid, "labl") == 0 || std::strcmp (sid, "note") == 0)
                         && ssize >= 9)
                {
                    uint32_t id = 0;
                    in->read (&id, 4);
                    const int64_t textBytes = ssize - 4; // declared covers cueID + text
                    std::vector<uint8_t> tb ((size_t) textBytes);
                    if (in->read (tb.data(), tb.size()) != (int64_t) tb.size())
                        break;
                    const juce::String text = juce::String::fromUTF8 ((const char*) tb.data());
                    if (std::strcmp (sid, "labl") == 0)
                        titles[(int) id] = text;
                    else
                        comments[(int) id] = text;

                    // Even-length texts carry one alignment NUL beyond the
                    // declared size; consume it when present.
                    if (((ssize - 5) % 2) == 0 && pos + 8 + ssize < remaining)
                    {
                        const auto savePos = in->getPosition();
                        char pad = 1;
                        if (in->read (&pad, 1) != 1 || pad != 0)
                            in->setPosition (savePos);
                        else
                            ++pos;
                    }
                }
                else
                {
                    // Unknown sub-chunk: skip its declared payload.
                    skip (ssize);
                }

                pos += 8 + ssize;
            }
        }
        else
        {
            // Skip the chunk body without reading it (the audio data can be
            // many GB).
            if (!skip (csize))
                break;
        }
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
