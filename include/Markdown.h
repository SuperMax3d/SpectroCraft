#pragma once
// SpectroCraft - basic Markdown for the File Info panel's Comments and
// Subject fields (issue #67): the text is stored as typed (Markdown source,
// readable anywhere); the panel previews it formatted.
#include <JuceHeader.h>
#include <vector>

namespace sc::markdown
{
/** A piece of a line with one style. */
struct Run
{
    juce::String text;
    bool bold = false, italic = false, code = false;
};

/** One line: "# " .. "### " headings, "- " / "* " / "+ " bullets, "1. "
    numbered items, "---" rules, else plain text; its runs carry **bold**
    (or __bold__), *italic* (or _italic_) and `code`. A backslash escapes the
    next character. An unclosed marker stays as typed. */
struct Line
{
    enum class Kind { text, heading1, heading2, heading3, bullet, numbered, rule };
    Kind kind = Kind::text;
    int number = 0;      // numbered items
    int indent = 0;      // leading spaces / 2, for nested lists
    std::vector<Run> runs;
};

std::vector<Line> parse (const juce::String& markdown);
/** Runs of one line's inline text (no block syntax). */
std::vector<Run> parseInline (const juce::String& text);

/** The formatted text, ready for juce::TextLayout. */
juce::AttributedString toAttributed (const juce::String& markdown, float fontSize, juce::Colour ink);

/** Editing helpers for the formatting buttons. `sel` is the selection (an
    empty range = the caret); `newSel` receives the selection to restore.
    wrap: puts `marker` on both sides of the selection (or removes it when it
    is already there). prefixLines: puts `prefix` before every line the
    selection touches (or removes it when every one of them has it). */
juce::String wrap (const juce::String& text, juce::Range<int> sel, const juce::String& marker,
                   juce::Range<int>& newSel);
juce::String prefixLines (const juce::String& text, juce::Range<int> sel, const juce::String& prefix,
                          juce::Range<int>& newSel);
} // namespace sc::markdown
