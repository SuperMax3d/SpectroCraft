// SpectroCraft - basic Markdown (issue #67); see include/Markdown.h
#include "Markdown.h"

namespace sc::markdown
{
std::vector<Run> parseInline (const juce::String& text)
{
    std::vector<Run> runs;
    Run cur;
    bool bold = false, italic = false;
    auto flush = [&]
    {
        if (cur.text.isNotEmpty())
            runs.push_back (cur);
        cur = Run {};
        cur.bold = bold;
        cur.italic = italic;
    };
    // Whether `marker` occurs again from position `from` on (an unclosed
    // marker stays as typed).
    auto closes = [&text] (int from, const juce::String& marker)
    {
        return text.indexOf (from, marker) >= 0;
    };
    auto isSpace = [&text] (int at)
    {
        return at < 0 || at >= text.length() || juce::CharacterFunctions::isWhitespace (text[at]);
    };
    const int n = text.length();
    for (int i = 0; i < n;)
    {
        const auto c = text[i];
        if (c == '\\' && i + 1 < n)
        {
            cur.text += juce::String::charToString (text[i + 1]);
            i += 2;
            continue;
        }
        if (c == '`')
        {
            const int end = text.indexOf (i + 1, "`");
            if (end > i)
            {
                flush();
                Run code;
                code.code = true;
                code.text = text.substring (i + 1, end);
                runs.push_back (code);
                i = end + 1;
                continue;
            }
        }
        // A marker opens before a non-space and closes after one: "2 * 3"
        // stays as typed.
        const juce::String two = text.substring (i, i + 2);
        if ((two == "**" || two == "__")
            && (bold ? ! isSpace (i - 1) : (! isSpace (i + 2) && closes (i + 2, two))))
        {
            flush();
            bold = ! bold;
            cur.bold = bold;
            i += 2;
            continue;
        }
        if (two == "**" || two == "__")
        {
            cur.text += two;   // a double marker that opens nothing stays as typed
            i += 2;
            continue;
        }
        if ((c == '*' || c == '_')
            && (italic ? ! isSpace (i - 1) : (! isSpace (i + 1) && closes (i + 1, juce::String::charToString (c)))))
        {
            // An underscore inside a word (snake_case) is a character.
            const bool inWord = c == '_' && i > 0 && juce::CharacterFunctions::isLetterOrDigit (text[i - 1])
                                && i + 1 < n && juce::CharacterFunctions::isLetterOrDigit (text[i + 1]);
            if (! inWord)
            {
                flush();
                italic = ! italic;
                cur.italic = italic;
                ++i;
                continue;
            }
        }
        cur.text += juce::String::charToString (c);
        ++i;
    }
    flush();
    return runs;
}

std::vector<Line> parse (const juce::String& markdown)
{
    std::vector<Line> lines;
    for (const auto& raw : juce::StringArray::fromLines (markdown))
    {
        Line l;
        int lead = 0;
        while (lead < raw.length() && raw[lead] == ' ') ++lead;
        juce::String s = raw.substring (lead);
        l.indent = lead / 2;
        if (s.startsWith ("### "))      { l.kind = Line::Kind::heading3; s = s.substring (4); }
        else if (s.startsWith ("## "))  { l.kind = Line::Kind::heading2; s = s.substring (3); }
        else if (s.startsWith ("# "))   { l.kind = Line::Kind::heading1; s = s.substring (2); }
        else if (s.startsWith ("- ") || s.startsWith ("* ") || s.startsWith ("+ "))
        {
            l.kind = Line::Kind::bullet;
            s = s.substring (2);
        }
        else if (s.trim() == "---" || s.trim() == "***")
        {
            l.kind = Line::Kind::rule;
            s.clear();
        }
        else
        {
            int digits = 0;
            while (digits < s.length() && juce::CharacterFunctions::isDigit (s[digits])) ++digits;
            if (digits > 0 && digits < 10 && s.substring (digits, digits + 2) == ". ")
            {
                l.kind = Line::Kind::numbered;
                l.number = s.substring (0, digits).getIntValue();
                s = s.substring (digits + 2);
            }
        }
        l.runs = parseInline (s);
        lines.push_back (std::move (l));
    }
    return lines;
}

juce::AttributedString toAttributed (const juce::String& markdown, float fontSize, juce::Colour ink)
{
    juce::AttributedString out;
    out.setWordWrap (juce::AttributedString::byWord);
    const auto lines = parse (markdown);
    for (size_t li = 0; li < lines.size(); ++li)
    {
        const auto& l = lines[li];
        float size = fontSize;
        bool headingBold = false;
        switch (l.kind)
        {
            case Line::Kind::heading1: size = fontSize * 1.5f;  headingBold = true; break;
            case Line::Kind::heading2: size = fontSize * 1.3f;  headingBold = true; break;
            case Line::Kind::heading3: size = fontSize * 1.15f; headingBold = true; break;
            default: break;
        }
        const juce::String pad = juce::String::repeatedString ("    ", l.indent);
        if (l.kind == Line::Kind::bullet)
            out.append (pad + juce::String::charToString ((juce::juce_wchar) 0x2022) + "  ", juce::FontOptions (fontSize), ink);
        else if (l.kind == Line::Kind::numbered)
            out.append (pad + juce::String (l.number) + ".  ", juce::FontOptions (fontSize), ink);
        else if (l.kind == Line::Kind::rule)
            out.append (juce::String::repeatedString (juce::String::charToString ((juce::juce_wchar) 0x2500), 24),
                        juce::FontOptions (fontSize), ink.withMultipliedAlpha (0.5f));
        for (const auto& r : l.runs)
        {
            int style = juce::Font::plain;
            if (r.bold || headingBold) style |= juce::Font::bold;
            if (r.italic) style |= juce::Font::italic;
            if (r.code)
                out.append (r.text, juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size * 0.95f,
                                                       juce::Font::plain),
                            ink.interpolatedWith (juce::Colour (0xffe8b86a), 0.6f));
            else
                out.append (r.text, juce::FontOptions (size, style), ink);
        }
        if (li + 1 < lines.size())
            out.append ("\n", juce::FontOptions (fontSize), ink);
    }
    return out;
}

juce::String wrap (const juce::String& text, juce::Range<int> sel, const juce::String& marker,
                   juce::Range<int>& newSel)
{
    const int m = marker.length();
    sel = sel.getIntersectionWith ({ 0, text.length() });
    const juce::String before = text.substring (0, sel.getStart()), inner = text.substring (sel.getStart(), sel.getEnd()),
                       after = text.substring (sel.getEnd());
    // Already wrapped (markers just outside the selection): unwrap.
    if (before.endsWith (marker) && after.startsWith (marker))
    {
        newSel = sel - m;
        return before.dropLastCharacters (m) + inner + after.substring (m);
    }
    newSel = sel + m;
    return before + marker + inner + marker + after;
}

juce::String prefixLines (const juce::String& text, juce::Range<int> sel, const juce::String& prefix,
                          juce::Range<int>& newSel)
{
    sel = sel.getIntersectionWith ({ 0, text.length() });
    const int first = text.substring (0, sel.getStart()).lastIndexOfChar ('\n') + 1;
    int last = text.indexOf (sel.getEnd() > sel.getStart() ? sel.getEnd() - 1 : sel.getEnd(), "\n");
    if (last < 0) last = text.length();
    auto lines = juce::StringArray::fromTokens (text.substring (first, last), "\n", "");
    bool all = true;
    for (const auto& l : lines)
        all = all && l.startsWith (prefix);
    int delta = 0;
    for (auto& l : lines)
    {
        if (all) { l = l.substring (prefix.length()); delta -= prefix.length(); }
        else if (! l.startsWith (prefix)) { l = prefix + l; delta += prefix.length(); }
    }
    const juce::String joined = lines.joinIntoString ("\n");
    newSel = { first, first + joined.length() };
    juce::ignoreUnused (delta);
    return text.substring (0, first) + joined + text.substring (last);
}
} // namespace sc::markdown
