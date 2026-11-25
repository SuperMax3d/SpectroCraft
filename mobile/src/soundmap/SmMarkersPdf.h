// SpectroCraft Mobile - sound map: printable tag sheets.
//
// One tag per page, vector shapes at the exact size (print at 100 % / actual
// size), with its id, which edge goes up, and a 100 mm bar to check the
// printer's scale. A plain PDF (Helvetica, no embedded font).
#pragma once
#include <JuceHeader.h>
#include <vector>

namespace scm::smap
{
struct Paper
{
    juce::String name;
    double widthMm = 215.9, heightMm = 279.4;
};

/** The smallest of Letter, Tabloid (11 x 17 in) and 12 x 16 in that holds the
    tag, its white border and the printer's margins across. */
Paper paperFor (double tagSizeMm);

/** The PDF bytes. False on a bad id or size. */
bool markersPdf (const std::vector<int>& ids, double tagSizeMm, const Paper& paper, juce::MemoryBlock& out,
                 const juce::String& title = "SpectroCraft sound map");

bool writeMarkersPdf (const juce::File& file, const std::vector<int>& ids, double tagSizeMm);
} // namespace scm::smap
