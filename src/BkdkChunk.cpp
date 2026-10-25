/**
 * SpectroCraft - "bkdk" RIFF chunk parsing (see include/BkdkChunk.h).
 */

#include "BkdkChunk.h"

namespace sc
{

double BkdkData::calibrationFor (int ch) const
{
    int g = -1;
    if (ch >= 0 && ch < static_cast<int> (channels.size()))
        g = ch;
    else if (channels.size() == 1)
        g = 0;   // a single defined group applies to every channel

    if (g < 0) return 1.0;   // no data for this channel: leave it unscaled

    const juce::String& s = channels[static_cast<size_t> (g)][7];   // field #8
    const double v = s.trim().getDoubleValue();
    return (std::isfinite (v) && v > 0.0) ? v : 1.0;
}

bool parseBkdkPayload (const std::string& payload, BkdkData& out)
{
    // Split on NUL bytes positionally: empty fields stay in place (writers
    // may emit "" for some elements) and the trailing padding zeros become a
    // run of empty tokens at the end.
    std::vector<std::string> tok;
    std::string cur;
    for (const char c : payload)
    {
        if (c == '\0')
        {
            tok.push_back (cur);
            cur.clear();
        }
        else
            cur += c;
    }
    tok.push_back (cur);   // text after the last NUL (usually "")

    out = BkdkData {};

    // Need at least two non-empty tokens: Version + Date.
    int nonEmpty = 0;
    for (const auto& t : tok)
        if (!t.empty()) ++nonEmpty;
    if (nonEmpty < 2) return false;

    out.version = juce::String::fromUTF8 (tok[0].c_str());
    out.date    = juce::String::fromUTF8 (tok[1].c_str());

    size_t i = 2;
    while (i + 11 <= tok.size())   // one complete channel group per 11 fields
    {
        std::array<juce::String, 11> g;
        for (int k = 0; k < 11; ++k)
            g[static_cast<size_t> (k)] = juce::String::fromUTF8 (tok[i + k].c_str());
        out.channels.push_back (g);
        i += 11;
    }

    // The first non-empty token after the last complete group is Foot; the
    // remaining empties are the writers' trailing padding.
    for (size_t j = i; j < tok.size(); ++j)
    {
        if (!tok[j].empty())
        {
            out.foot = juce::String::fromUTF8 (tok[j].c_str());
            break;
        }
    }

    return true;
}

} // namespace sc
