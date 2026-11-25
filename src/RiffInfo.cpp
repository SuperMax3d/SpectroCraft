// SpectroCraft - WAV text metadata (issue #67); see include/RiffInfo.h
#include "RiffInfo.h"
#include <cstring>

namespace sc::riffinfo
{
const std::vector<Field>& fields()
{
    // The chunk ids follow the RIFF specification's meaning of each name:
    // ISRF "source form" is what was digitised, ISRC "source" who supplied
    // it, ITCH the technician who digitised it.
    static const std::vector<Field> f {
        { "DISP", "Display Title" },
        { "IART", "Original Artist" },
        { "INAM", "Name" },
        { "IGNR", "Genre" },
        { "IKEY", "Key Words" },
        { "ISRF", "Digitization Source" },
        { "IMED", "Original Medium" },
        { "IENG", "Engineers" },
        { "ITCH", "Digitizer" },
        { "ISRC", "Source Supplier" },
        { "ICOP", "Copyright" },
        { "ISFT", "Software Package" },
        { "ICRD", "Creation Date" },
        { "ICMT", "Comments", true },
        { "ISBJ", "Subject", true },
    };
    return f;
}

namespace
{
void putU32 (std::string& s, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        s.push_back ((char) ((v >> (8 * i)) & 0xff));
}

uint32_t getU32 (const std::string& s, size_t at)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= (uint32_t) (uint8_t) s[at + (size_t) i] << (8 * i);
    return v;
}

// A text field's bytes up to the first zero (fields are zero-terminated,
// some writers pad them with more zeros).
juce::String decodeText (const char* data, size_t n)
{
    size_t len = 0;
    while (len < n && data[len] != '\0') ++len;
    if (juce::CharPointer_UTF8::isValidString (data, (int) len))
        return juce::String::fromUTF8 (data, (int) len).trimEnd();
    juce::String latin;   // ISO-8859-1: each byte is its own code point
    for (size_t i = 0; i < len; ++i)
        latin += juce::String::charToString ((juce::juce_wchar) (uint8_t) data[i]);
    return latin.trimEnd();
}

bool isFourCC (const juce::String& id)
{
    if (id.length() != 4) return false;
    for (auto c : id)
        if (c < 0x20 || c > 0x7e) return false;
    return true;
}
} // namespace

std::string buildInfoListPayload (const Values& v)
{
    std::string p = "INFO";
    for (const auto& [id, text] : v)
    {
        if (id == "DISP" || ! isFourCC (id) || text.trim().isEmpty())
            continue;
        const std::string utf8 = text.toStdString();
        p += id.toStdString();
        putU32 (p, (uint32_t) utf8.size() + 1);
        p += utf8;
        p.push_back ('\0');
        if ((utf8.size() + 1) & 1)
            p.push_back ('\0');   // word alignment
    }
    return p.size() > 4 ? p : std::string();
}

std::string buildDispPayload (const Values& v)
{
    const auto it = v.find ("DISP");
    if (it == v.end() || it->second.trim().isEmpty())
        return {};
    std::string p;
    putU32 (p, 1);   // CF_TEXT
    p += it->second.toStdString();
    p.push_back ('\0');
    return p;
}

bool parseInfoListPayload (const std::string& payload, Values& out)
{
    if (payload.size() < 4 || payload.compare (0, 4, "INFO") != 0)
        return false;
    size_t at = 4;
    while (at + 8 <= payload.size())
    {
        const juce::String id = juce::String (payload.substr (at, 4));
        const uint32_t size = getU32 (payload, at + 4);
        at += 8;
        if (size > payload.size() - at)
            break;   // truncated: keep what was read
        if (isFourCC (id))
            out[id] = decodeText (payload.data() + at, size);
        at += size + (size & 1);
    }
    return true;
}

bool parseDispPayload (const std::string& payload, Values& out)
{
    if (payload.size() < 5 || getU32 (payload, 0) != 1)   // CF_TEXT only
        return false;
    out["DISP"] = decodeText (payload.data() + 4, payload.size() - 4);
    return true;
}

bool sameText (const Values& a, const Values& b)
{
    auto norm = [] (const Values& v)
    {
        Values n;
        for (const auto& [id, text] : v)
            if (text.trim().isNotEmpty())
                n[id] = text.trimEnd();
        return n;
    };
    return norm (a) == norm (b);
}

Values readFromWav (const juce::File& wav)
{
    Values v;
    if (! wav.getFileExtension().equalsIgnoreCase (".wav"))
        return v;
    juce::FileInputStream in (wav);
    char hdr[12];
    if (in.failedToOpen() || in.read (hdr, 12) != 12
        || (std::memcmp (hdr, "RIFF", 4) != 0 && std::memcmp (hdr, "RF64", 4) != 0)
        || std::memcmp (hdr + 8, "WAVE", 4) != 0)
        return v;
    const juce::int64 total = in.getTotalLength();
    juce::int64 ds64Data = -1;   // RF64: the data chunk's real size (its 32-bit field is 0xffffffff)
    while (in.getPosition() + 8 <= total)
    {
        char id[4];
        if (in.read (id, 4) != 4) break;
        const uint32_t size = (uint32_t) in.readInt();
        juce::int64 bytes = (juce::int64) size;
        if (std::memcmp (id, "ds64", 4) == 0 && size >= 16)
        {
            const juce::int64 at = in.getPosition();
            in.readInt64();                 // RIFF size
            ds64Data = in.readInt64();      // data size
            in.setPosition (at);
        }
        if (std::memcmp (id, "data", 4) == 0 && size == 0xffffffffu && ds64Data >= 0)
            bytes = ds64Data;
        const juce::int64 next = in.getPosition() + bytes + (bytes & 1);
        const bool infoList = std::memcmp (id, "LIST", 4) == 0;
        const bool disp = std::memcmp (id, "DISP", 4) == 0;
        if ((infoList || disp) && size < (1u << 24))   // text chunks are small
        {
            std::string payload ((size_t) size, '\0');
            if (in.read (payload.data(), (int) size) != (int) size) break;
            if (infoList) parseInfoListPayload (payload, v);   // other LIST types are skipped
            else          parseDispPayload (payload, v);
        }
        if (next > total || ! in.setPosition (next)) break;
    }
    return v;
}
} // namespace sc::riffinfo
