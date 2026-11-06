// SpectroCraft - AI control: append-only session journal (see include/control/Journal.h).
#include "control/Journal.h"
#include "control/CommandRegistry.h"

namespace sc::control {

Journal::~Journal()
{
    std::lock_guard<std::mutex> l (lock);
    if (out != nullptr) out->flush();
}

juce::int64 Journal::append (const juce::String& kind, juce::var entry)
{
    if (!entry.isObject())
        entry = obj();
    juce::var copy;
    juce::int64 seq = 0;
    {
        std::lock_guard<std::mutex> l (lock);
        seq = nextSeq++;
        auto* o = entry.getDynamicObject();
        // seq / ts / kind first so the JSON line reads naturally.
        auto ordered = new juce::DynamicObject();
        ordered->setProperty ("seq", seq);
        ordered->setProperty ("ts", isoNow());
        ordered->setProperty ("kind", kind);
        for (const auto& p : o->getProperties())
            if (p.name != juce::Identifier ("seq") && p.name != juce::Identifier ("ts")
                && p.name != juce::Identifier ("kind"))
                ordered->setProperty (p.name, p.value);
        copy = juce::var (ordered);
        ring.push_back (copy);
        while (ring.size() > kCapacity)
            ring.pop_front();
        if (out != nullptr)
        {
            out->writeText (toLine (copy) + "\n", false, false, "\n");
            out->flush();
        }
    }
    if (onAppend)
        onAppend (copy);
    return seq;
}

juce::var Journal::read (juce::int64 sinceSeq, const juce::StringArray& kinds,
                         int limit, int docId) const
{
    juce::Array<juce::var> entries;
    juce::int64 last = 0;
    {
        std::lock_guard<std::mutex> l (lock);
        last = nextSeq - 1;
        for (const auto& e : ring)
        {
            if ((juce::int64) e.getProperty ("seq", 0) <= sinceSeq) continue;
            if (!kinds.isEmpty() && !kinds.contains (e.getProperty ("kind", {}).toString())) continue;
            if (docId >= 0 && (int) e.getProperty ("doc", -1) != docId) continue;
            entries.add (e);
            if (limit > 0 && entries.size() >= limit) break;
        }
    }
    const juce::int64 next = entries.isEmpty() ? std::max (sinceSeq, last) + 1
                                               : (juce::int64) entries.getLast().getProperty ("seq", 0) + 1;
    return obj ({ { "entries", entries },
                  { "nextSeq", next },
                  { "lastSeq", last },
                  { "file", jsonl == juce::File() ? juce::var() : juce::var (jsonl.getFullPathName()) } });
}

std::vector<juce::var> Journal::entriesSince (juce::int64 sinceSeq) const
{
    std::vector<juce::var> v;
    std::lock_guard<std::mutex> l (lock);
    for (const auto& e : ring)
        if ((juce::int64) e.getProperty ("seq", 0) > sinceSeq)
            v.push_back (e);
    return v;
}

juce::int64 Journal::lastSeq() const
{
    std::lock_guard<std::mutex> l (lock);
    return nextSeq - 1;
}

juce::int64 Journal::firstSeq() const
{
    std::lock_guard<std::mutex> l (lock);
    return ring.empty() ? 0 : (juce::int64) ring.front().getProperty ("seq", 0);
}

void Journal::setFile (const juce::File& f)
{
    std::lock_guard<std::mutex> l (lock);
    out.reset();
    jsonl = f;
    if (f == juce::File())
        return;
    f.getParentDirectory().createDirectory();
    auto s = std::make_unique<juce::FileOutputStream> (f);   // appends
    if (s->openedOk())
        out = std::move (s);
    else
        jsonl = juce::File();
}

juce::File Journal::file() const
{
    std::lock_guard<std::mutex> l (lock);
    return jsonl;
}

} // namespace sc::control
