#pragma once
// SpectroCraft - WAV text metadata: the RIFF LIST/INFO fields and the DISP
// title, as Adobe Audition's File Info > Text Fields shows them (issue #67).
#include <JuceHeader.h>
#include <map>
#include <string>
#include <vector>

namespace sc::riffinfo
{
/** One field of the File Info panel: its chunk id ("IART"; "DISP" is the
    display title, a chunk of its own), its label (Audition's "Standard RIFF"
    names) and whether it is a multi-line, Markdown field. */
struct Field
{
    const char* id;
    const char* label;
    bool multiline = false;
};
/** Audition's order: Display Title, Original Artist, Name, Genre, Key Words,
    Digitization Source, Original Medium, Engineers, Digitizer, Source
    Supplier, Copyright, Software Package, Creation Date, then Comments and
    Subject (multi-line). */
const std::vector<Field>& fields();

/** id -> text. Ids the panel does not show (IPRD, ...) are kept too, so a
    save writes back every field the file had. Empty texts are not written. */
using Values = std::map<juce::String, juce::String>;

/** Payload of a "LIST" chunk of type "INFO" ("INFO" + one sub-chunk per
    non-empty field, UTF-8, zero-terminated, word-aligned); "" when there is
    nothing to write. DISP is not part of it. */
std::string buildInfoListPayload (const Values& v);
/** Payload of a "DISP" chunk (CF_TEXT = 1, then the zero-terminated title);
    "" when the title is empty. */
std::string buildDispPayload (const Values& v);
/** Reads a LIST payload of type INFO (from "INFO" on) into `out`; false
    when it is not one. Texts that are not valid UTF-8 read as Latin-1. */
bool parseInfoListPayload (const std::string& payload, Values& out);
/** Reads a DISP payload of type CF_TEXT into out["DISP"]. */
bool parseDispPayload (const std::string& payload, Values& out);
/** Every LIST/INFO field and the DISP title of a WAV file; an empty map for
    any other file or when it has none. */
Values readFromWav (const juce::File& wav);
/** True when both hold the same texts once empty fields are left out and
    trailing spaces trimmed (what a save would write). */
bool sameText (const Values& a, const Values& b);
} // namespace sc::riffinfo
