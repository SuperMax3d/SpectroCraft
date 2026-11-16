#include "Session.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sc::session
{

int Session::totalChannels() const
{
    int n = 0;
    for (int c : laneChannels) n += std::max (0, c);
    return n;
}

int Session::laneFirstChannel (int lane) const
{
    if (lane < 0 || lane >= numLanes()) return -1;
    int first = 0;
    for (int i = 0; i < lane; ++i) first += std::max (0, laneChannels[(size_t) i]);
    return first;
}

int Session::laneOfChannel (int channel) const
{
    int first = 0;
    for (int i = 0; i < numLanes(); ++i)
    {
        const int n = std::max (0, laneChannels[(size_t) i]);
        if (channel >= first && channel < first + n) return i;
        first += n;
    }
    return -1;
}

bool Session::laneEmpty (int lane) const
{
    return std::none_of (clips.begin(), clips.end(), [lane] (const Clip& c) { return c.lane == lane; });
}

int64_t Session::extent() const
{
    int64_t e = 0;
    for (const auto& c : clips) e = std::max (e, c.end());
    return e;
}

int Session::clipAt (int channel, int64_t sample) const
{
    const int lane = laneOfChannel (channel);
    if (lane < 0) return -1;
    for (size_t i = 0; i < clips.size(); ++i)
        if (clips[i].lane == lane && sample >= clips[i].offset && sample < clips[i].end())
            return (int) i;
    return -1;
}

bool fits (const Session& s, int index, int lane, int64_t offset)
{
    if (index < 0 || index >= (int) s.clips.size() || lane < 0 || lane >= s.numLanes() || offset < 0)
        return false;
    const int64_t len = s.clips[(size_t) index].length;
    for (size_t j = 0; j < s.clips.size(); ++j)
    {
        if ((int) j == index || s.clips[j].lane != lane) continue;
        if (offset < s.clips[j].end() && offset + len > s.clips[j].offset)
            return false;
    }
    return true;
}

int64_t snapOffset (const Session& s, int index, int64_t proposed, int64_t tolerance,
                    const std::vector<int64_t>& extraTargets)
{
    if (index < 0 || index >= (int) s.clips.size()) return proposed;
    const int64_t len = s.clips[(size_t) index].length;
    std::vector<int64_t> targets { 0 };
    for (size_t j = 0; j < s.clips.size(); ++j)
        if ((int) j != index)
        {
            targets.push_back (s.clips[j].offset);
            targets.push_back (s.clips[j].end());
        }
    targets.insert (targets.end(), extraTargets.begin(), extraTargets.end());

    int64_t best = proposed;
    int64_t bestDist = std::numeric_limits<int64_t>::max();
    for (int64_t t : targets)
        for (int64_t cand : { t, t - len })   // start on t, or end on t
        {
            if (cand < 0) continue;
            const int64_t d = std::llabs (cand - proposed);
            if (d <= tolerance && d < bestDist) { best = cand; bestDist = d; }
        }
    return best;
}

int64_t firstFreeOffset (const Session& s, int index, int lane, int64_t proposed)
{
    if (index < 0 || index >= (int) s.clips.size() || lane < 0 || lane >= s.numLanes())
        return -1;
    int64_t off = std::max<int64_t> (0, proposed);
    for (int guard = 0; guard <= (int) s.clips.size() + 1; ++guard)
    {
        if (fits (s, index, lane, off)) return off;
        // Jump past the file in the way.
        const int64_t len = s.clips[(size_t) index].length;
        int64_t next = off;
        for (size_t j = 0; j < s.clips.size(); ++j)
            if ((int) j != index && s.clips[j].lane == lane
                && off < s.clips[j].end() && off + len > s.clips[j].offset)
                next = std::max (next, s.clips[j].end());
        if (next == off) break;
        off = next;
    }
    return fits (s, index, lane, off) ? off : -1;
}

namespace
{
    void dropEmptyClips (Session& s)
    {
        for (size_t i = s.clips.size(); i-- > 1;)
            if (s.clips[i].length <= 0)
                s.clips.erase (s.clips.begin() + (std::ptrdiff_t) i);
    }

    bool someClipEndsAt (const Session& s, int lane, int64_t t, size_t except)
    {
        for (size_t j = 0; j < s.clips.size(); ++j)
            if (j != except && s.clips[j].lane == lane && s.clips[j].end() == t)
                return true;
        return false;
    }
}

void warpForDeletion (Session& s, int64_t a, int64_t b)
{
    if (b <= a) return;
    auto w = [a, b] (int64_t t) { return t < a ? t : (t < b ? a : t - (b - a)); };
    for (auto& c : s.clips)
    {
        const int64_t start = w (c.offset), end = w (c.end());
        c.offset = start;
        c.length = std::max<int64_t> (0, end - start);
    }
    s.clips[0].offset = 0;
    dropEmptyClips (s);
}

void warpForInsertion (Session& s, int64_t a, int64_t len)
{
    if (len <= 0) return;
    std::vector<Clip> before = s.clips;
    Session ref = s;   // boundaries are judged on the layout before the edit
    for (size_t i = 0; i < s.clips.size(); ++i)
    {
        auto& c = s.clips[i];
        const Clip& o = before[i];
        // The insertion goes to the file it falls in. At a boundary between
        // two touching files of a lane it extends the left one; a file that
        // starts exactly there with nothing before it grows at its start.
        const bool shiftStart = o.offset > a
                                || (o.offset == a && i != 0 && someClipEndsAt (ref, o.lane, a, i));
        const bool shiftEnd = o.end() >= a;
        const int64_t start = shiftStart ? o.offset + len : o.offset;
        const int64_t end = shiftEnd ? o.end() + len : o.end();
        c.offset = start;
        c.length = std::max<int64_t> (0, end - start);
    }
    s.clips[0].offset = 0;
}

void warpForStretch (Session& s, int64_t a, int64_t bOld, int64_t bNew)
{
    if (bOld <= a || bNew < a) return;
    const double k = (double) (bNew - a) / (double) (bOld - a);
    auto w = [=] (int64_t t) -> int64_t
    {
        if (t < a) return t;
        if (t < bOld) return a + (int64_t) std::llround ((double) (t - a) * k);
        return t + (bNew - bOld);
    };
    for (auto& c : s.clips)
    {
        const int64_t start = w (c.offset), end = w (c.end());
        c.offset = start;
        c.length = std::max<int64_t> (0, end - start);
    }
    s.clips[0].offset = 0;
    dropEmptyClips (s);
}

std::vector<std::vector<float>> buildComposite (const Session& s,
                                                const std::vector<std::vector<std::vector<float>>>& data)
{
    const int64_t len = s.extent();
    std::vector<std::vector<float>> out ((size_t) s.totalChannels(),
                                         std::vector<float> ((size_t) std::max<int64_t> (0, len), 0.0f));
    for (size_t i = 0; i < s.clips.size() && i < data.size(); ++i)
    {
        const auto& c = s.clips[i];
        const int first = s.laneFirstChannel (c.lane);
        if (first < 0) continue;
        const int nch = s.laneChannels[(size_t) c.lane];
        for (int ch = 0; ch < nch && ch < (int) data[i].size(); ++ch)
        {
            const auto& src = data[i][(size_t) ch];
            const int64_t n = std::min<int64_t> ((int64_t) src.size(), std::min (c.length, len - c.offset));
            if (n > 0)
                std::copy (src.begin(), src.begin() + (std::ptrdiff_t) n,
                           out[(size_t) (first + ch)].begin() + (std::ptrdiff_t) c.offset);
        }
    }
    return out;
}

uint64_t regionHash (const Session& s, int index,
                     const std::function<bool (int, int64_t, int64_t, float*)>& read)
{
    uint64_t h = 1469598103934665603ull;   // FNV-1a offset basis
    if (index < 0 || index >= (int) s.clips.size() || read == nullptr) return h;
    const auto& c = s.clips[(size_t) index];
    const int first = s.laneFirstChannel (c.lane);
    if (first < 0) return h;
    const int nch = s.laneChannels[(size_t) c.lane];
    constexpr int64_t block = 1 << 16;
    std::vector<float> buf ((size_t) block);
    for (int ch = 0; ch < nch; ++ch)
        for (int64_t pos = 0; pos < c.length; pos += block)
        {
            const int64_t n = std::min (block, c.length - pos);
            std::fill (buf.begin(), buf.begin() + (std::ptrdiff_t) n, 0.0f);
            read (first + ch, c.offset + pos, n, buf.data());
            const auto* bytes = reinterpret_cast<const unsigned char*> (buf.data());
            for (size_t k = 0; k < (size_t) n * sizeof (float); ++k)
            {
                h ^= bytes[k];
                h *= 1099511628211ull;
            }
        }
    h ^= (uint64_t) c.length;
    return h;
}

juce::var toVar (const Session& s, const juce::File& baseDir)
{
    auto* root = new juce::DynamicObject();
    juce::Array<juce::var> lanes;
    for (int c : s.laneChannels) lanes.add (c);
    root->setProperty ("lanes", lanes);
    juce::Array<juce::var> files;
    for (size_t i = 1; i < s.clips.size(); ++i)
    {
        const auto& c = s.clips[i];
        auto* o = new juce::DynamicObject();
        o->setProperty ("path", c.file.getRelativePathFrom (baseDir).replaceCharacter ('\\', '/'));
        o->setProperty ("abs", c.file.getFullPathName());
        o->setProperty ("lane", c.lane);
        o->setProperty ("offset", (juce::int64) c.offset);
        o->setProperty ("length", (juce::int64) c.length);
        files.add (juce::var (o));
    }
    root->setProperty ("files", files);
    return juce::var (root);
}

Session fromVar (const juce::var& v, const juce::File& baseDir, const juce::File& mainFile,
                 int mainChannels, int64_t mainLength)
{
    Session s;
    Clip mainClip;
    mainClip.file = mainFile;
    mainClip.length = mainLength;
    s.clips.push_back (mainClip);
    s.laneChannels.push_back (std::max (1, mainChannels));

    if (!v.isObject()) return s;
    if (const auto* lanes = v.getProperty ("lanes", {}).getArray())
        for (int i = 1; i < lanes->size(); ++i)   // lane 0 is the main file's own layout
            s.laneChannels.push_back (std::clamp ((int) (*lanes)[i], 1, 64));

    if (const auto* files = v.getProperty ("files", {}).getArray())
        for (const auto& f : *files)
        {
            Clip c;
            const juce::String rel = f.getProperty ("path", "").toString();
            const juce::String abs = f.getProperty ("abs", "").toString();
            const juce::File byRel = rel.isEmpty() ? juce::File() : baseDir.getChildFile (rel);
            if (byRel != juce::File() && byRel.existsAsFile()) c.file = byRel;
            else if (juce::File::isAbsolutePath (abs) && juce::File (abs).existsAsFile()) c.file = juce::File (abs);
            else c.file = byRel != juce::File() ? byRel : juce::File (abs);
            c.lane = (int) f.getProperty ("lane", 0);
            c.offset = std::max<juce::int64> (0, (juce::int64) f.getProperty ("offset", 0));
            c.length = std::max<juce::int64> (0, (juce::int64) f.getProperty ("length", 0));
            if (c.lane < 0 || c.lane >= s.numLanes() || c.file == juce::File()) continue;
            s.clips.push_back (c);
        }
    return s;
}

} // namespace sc::session
