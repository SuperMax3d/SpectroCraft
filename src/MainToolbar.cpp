// SpectroCraft - the toolbar in the menu bar row (issue #43). See MainToolbar.h.
#include "MainToolbar.h"

#include <cmath>
#include <cstdio>
#include <optional>

namespace sc::toolbar
{
// ============================================================== icons
//
// Every icon is drawn on a 16 x 16 grid, then scaled into the button. All of
// them are original drawings made for SpectroCraft (no icon font or set).

namespace
{
juce::Path star (float cx, float cy, float r1, float r2, int points)
{
    juce::Path p;
    for (int i = 0; i < points * 2; ++i)
    {
        const float a = juce::MathConstants<float>::pi * (float) i / (float) points - juce::MathConstants<float>::halfPi;
        const float r = (i % 2 == 0) ? r1 : r2;
        const float x = cx + r * std::cos (a), y = cy + r * std::sin (a);
        if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
    }
    p.closeSubPath();
    return p;
}

void arrowHead (juce::Path& p, float tipX, float tipY, float fromX, float fromY, float len, float half)
{
    const float dx = tipX - fromX, dy = tipY - fromY;
    const float d = std::max (1.0e-3f, std::sqrt (dx * dx + dy * dy));
    const float ux = dx / d, uy = dy / d;
    const float bx = tipX - ux * len, by = tipY - uy * len;
    p.addTriangle (tipX, tipY, bx - uy * half, by + ux * half, bx + uy * half, by - ux * half);
}
} // namespace

void drawIcon (juce::Graphics& g, Icon icon, juce::Rectangle<float> area, juce::Colour c)
{
    if (icon == Icon::none || area.isEmpty()) return;
    const float s = std::min (area.getWidth(), area.getHeight()) / 16.0f;
    const auto t = juce::AffineTransform::scale (s).translated (area.getCentreX() - 8.0f * s,
                                                                area.getCentreY() - 8.0f * s);
    juce::Path stroke, fill;
    float width = 1.5f;
    switch (icon)
    {
        case Icon::undo:
        case Icon::redo:
        {
            // A U-turn: along the top, around the right, back along the bottom.
            stroke.startNewSubPath (5.0f, 5.5f);
            stroke.lineTo (10.0f, 5.5f);
            stroke.quadraticTo (14.0f, 5.5f, 14.0f, 9.0f);
            stroke.quadraticTo (14.0f, 12.5f, 10.0f, 12.5f);
            stroke.lineTo (6.0f, 12.5f);
            fill.addTriangle (1.5f, 5.5f, 6.0f, 2.0f, 6.0f, 9.0f);
            if (icon == Icon::redo)
            {
                const auto flip = juce::AffineTransform::scale (-1.0f, 1.0f).translated (16.0f, 0.0f);
                stroke.applyTransform (flip);
                fill.applyTransform (flip);
            }
            break;
        }
        case Icon::timeRange:
            fill.addRectangle (4.0f, 3.0f, 8.0f, 10.0f);
            stroke.startNewSubPath (3.5f, 2.0f); stroke.lineTo (3.5f, 14.0f);
            stroke.startNewSubPath (12.5f, 2.0f); stroke.lineTo (12.5f, 14.0f);
            break;
        case Icon::marquee:
        {
            juce::Path r;
            r.addRectangle (2.5f, 3.5f, 11.0f, 9.0f);
            const float dashes[] = { 2.0f, 1.6f };
            juce::PathStrokeType (1.4f).createDashedStroke (fill, r, dashes, 2);
            break;
        }
        case Icon::lasso:
            stroke.addEllipse (2.5f, 2.0f, 11.0f, 7.5f);
            stroke.startNewSubPath (5.0f, 9.0f);
            stroke.quadraticTo (4.0f, 12.0f, 6.5f, 12.0f);
            stroke.quadraticTo (8.5f, 12.0f, 6.0f, 14.5f);
            break;
        case Icon::paintbrush:
            stroke.startNewSubPath (13.5f, 2.5f);
            stroke.lineTo (8.0f, 8.0f);
            width = 2.0f;
            // The bristles: a wide drop ending in a point at the bottom left.
            fill.startNewSubPath (8.8f, 7.0f);
            fill.quadraticTo (10.5f, 9.5f, 8.0f, 12.0f);
            fill.quadraticTo (5.0f, 15.0f, 1.0f, 15.0f);
            fill.quadraticTo (3.0f, 13.0f, 3.0f, 10.5f);
            fill.quadraticTo (4.0f, 6.5f, 8.8f, 7.0f);
            fill.closeSubPath();
            break;
        case Icon::move:
            stroke.startNewSubPath (8.0f, 3.5f); stroke.lineTo (8.0f, 12.5f);
            stroke.startNewSubPath (3.5f, 8.0f); stroke.lineTo (12.5f, 8.0f);
            arrowHead (fill, 8.0f, 1.0f, 8.0f, 8.0f, 3.0f, 2.5f);
            arrowHead (fill, 8.0f, 15.0f, 8.0f, 8.0f, 3.0f, 2.5f);
            arrowHead (fill, 1.0f, 8.0f, 8.0f, 8.0f, 3.0f, 2.5f);
            arrowHead (fill, 15.0f, 8.0f, 8.0f, 8.0f, 3.0f, 2.5f);
            break;
        case Icon::toggleViews:
        {
            // A plain waveform (issue #50): a thin oscillation whose
            // amplitude swells and fades, like a sound's envelope.
            width = 1.1f;
            stroke.startNewSubPath (1.0f, 8.0f);
            for (int i = 1; i <= 56; ++i)
            {
                const float x = 1.0f + 14.0f * (float) i / 56.0f;
                const float u = (x - 1.0f) / 14.0f;
                const float env = 6.0f * std::sin (juce::MathConstants<float>::pi * u) * (0.55f + 0.45f * std::sin (9.0f * u));
                stroke.lineTo (x, 8.0f - env * std::sin (juce::MathConstants<float>::twoPi * 4.5f * u));
            }
            break;
        }
        case Icon::addMarker:
            fill.startNewSubPath (4.5f, 1.5f);
            fill.lineTo (11.5f, 1.5f);
            fill.lineTo (11.5f, 6.0f);
            fill.lineTo (8.0f, 9.0f);
            fill.lineTo (4.5f, 6.0f);
            fill.closeSubPath();
            stroke.startNewSubPath (8.0f, 9.0f); stroke.lineTo (8.0f, 15.0f);
            break;
        case Icon::freehand:
            stroke.startNewSubPath (1.5f, 11.0f);
            stroke.cubicTo (4.0f, 2.0f, 6.0f, 2.0f, 7.5f, 8.0f);
            stroke.cubicTo (9.0f, 14.0f, 12.0f, 13.0f, 14.5f, 4.0f);
            break;
        case Icon::text:
            width = 2.0f;
            stroke.startNewSubPath (3.0f, 3.0f); stroke.lineTo (13.0f, 3.0f);
            stroke.startNewSubPath (8.0f, 3.0f); stroke.lineTo (8.0f, 14.0f);
            stroke.startNewSubPath (6.0f, 14.0f); stroke.lineTo (10.0f, 14.0f);
            break;
        case Icon::arrow:
            stroke.startNewSubPath (2.5f, 13.5f); stroke.lineTo (11.0f, 5.0f);
            arrowHead (fill, 14.0f, 2.0f, 2.5f, 13.5f, 5.0f, 3.0f);
            break;
        case Icon::rectangle:
            stroke.addRectangle (2.0f, 3.5f, 12.0f, 9.0f);
            break;
        case Icon::oval:
            stroke.addEllipse (1.5f, 3.5f, 13.0f, 9.0f);
            break;
        case Icon::star:
            fill = star (8.0f, 8.5f, 7.0f, 3.0f, 5);
            break;
        case Icon::bolt:
            fill.startNewSubPath (9.5f, 1.0f);
            fill.lineTo (3.0f, 9.0f);
            fill.lineTo (7.5f, 9.0f);
            fill.lineTo (6.0f, 15.0f);
            fill.lineTo (13.0f, 6.5f);
            fill.lineTo (8.5f, 6.5f);
            fill.closeSubPath();
            break;
        case Icon::gear:
            fill = star (8.0f, 8.0f, 7.0f, 5.0f, 8);
            fill.setUsingNonZeroWinding (false);
            fill.addEllipse (5.5f, 5.5f, 5.0f, 5.0f);
            break;
        case Icon::play:
            fill.addTriangle (4.0f, 2.0f, 4.0f, 14.0f, 14.0f, 8.0f);
            break;
        case Icon::flag:
            stroke.startNewSubPath (3.5f, 1.5f); stroke.lineTo (3.5f, 15.0f);
            fill.startNewSubPath (4.0f, 2.0f);
            fill.lineTo (13.5f, 2.0f);
            fill.lineTo (11.0f, 5.0f);
            fill.lineTo (13.5f, 8.0f);
            fill.lineTo (4.0f, 8.0f);
            fill.closeSubPath();
            break;
        case Icon::heart:
            fill.startNewSubPath (8.0f, 14.0f);
            fill.cubicTo (-2.0f, 7.0f, 3.0f, -1.0f, 8.0f, 4.5f);
            fill.cubicTo (13.0f, -1.0f, 18.0f, 7.0f, 8.0f, 14.0f);
            fill.closeSubPath();
            break;
        case Icon::wave:
            stroke.startNewSubPath (1.0f, 8.0f);
            for (int i = 1; i <= 28; ++i)
            {
                const float x = 1.0f + 14.0f * (float) i / 28.0f;
                stroke.lineTo (x, 8.0f - 5.0f * std::sin ((x - 1.0f) / 14.0f * juce::MathConstants<float>::twoPi));
            }
            break;
        case Icon::note:
            fill.addEllipse (2.5f, 10.5f, 5.5f, 4.0f);
            stroke.startNewSubPath (7.5f, 12.0f); stroke.lineTo (7.5f, 2.0f);
            stroke.quadraticTo (12.0f, 3.0f, 12.5f, 7.0f);
            break;
        case Icon::check:
            width = 2.2f;
            stroke.startNewSubPath (2.5f, 8.5f); stroke.lineTo (6.5f, 12.5f); stroke.lineTo (13.5f, 3.5f);
            break;
        case Icon::cross:
            width = 2.2f;
            stroke.startNewSubPath (3.0f, 3.0f); stroke.lineTo (13.0f, 13.0f);
            stroke.startNewSubPath (13.0f, 3.0f); stroke.lineTo (3.0f, 13.0f);
            break;
        case Icon::plus:
            width = 2.2f;
            stroke.startNewSubPath (8.0f, 2.0f); stroke.lineTo (8.0f, 14.0f);
            stroke.startNewSubPath (2.0f, 8.0f); stroke.lineTo (14.0f, 8.0f);
            break;
        case Icon::eye:
            stroke.startNewSubPath (1.0f, 8.0f);
            stroke.quadraticTo (8.0f, 0.5f, 15.0f, 8.0f);
            stroke.quadraticTo (8.0f, 15.5f, 1.0f, 8.0f);
            fill.addEllipse (5.5f, 5.5f, 5.0f, 5.0f);
            break;
        case Icon::magnifier:
            stroke.addEllipse (2.0f, 2.0f, 8.5f, 8.5f);
            width = 1.7f;
            stroke.startNewSubPath (9.5f, 9.5f); stroke.lineTo (14.5f, 14.5f);
            break;
        case Icon::scissors:
            stroke.addEllipse (1.5f, 10.0f, 4.5f, 4.5f);
            stroke.addEllipse (10.0f, 10.0f, 4.5f, 4.5f);
            stroke.startNewSubPath (5.0f, 10.5f); stroke.lineTo (12.5f, 1.5f);
            stroke.startNewSubPath (11.0f, 10.5f); stroke.lineTo (3.5f, 1.5f);
            break;
        case Icon::none:
        default:
            break;
    }

    g.setColour (c);
    if (! fill.isEmpty())
    {
        fill.applyTransform (t);
        if (icon == Icon::timeRange)
            g.setColour (c.withMultipliedAlpha (0.35f));
        g.fillPath (fill);
        g.setColour (c);
    }
    if (! stroke.isEmpty())
    {
        stroke.applyTransform (t);
        g.strokePath (stroke, juce::PathStrokeType (width * std::max (0.75f, s),
                                                    juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

namespace
{
const std::vector<std::pair<Icon, const char*>>& iconNames()
{
    static const std::vector<std::pair<Icon, const char*>> n {
        { Icon::undo, "undo" }, { Icon::redo, "redo" }, { Icon::timeRange, "timeRange" },
        { Icon::marquee, "marquee" }, { Icon::lasso, "lasso" }, { Icon::paintbrush, "paintbrush" },
        { Icon::move, "move" }, { Icon::toggleViews, "toggleViews" }, { Icon::addMarker, "addMarker" },
        { Icon::freehand, "freehand" }, { Icon::text, "text" }, { Icon::arrow, "arrow" },
        { Icon::rectangle, "rectangle" }, { Icon::oval, "oval" },
        { Icon::star, "star" }, { Icon::bolt, "bolt" }, { Icon::gear, "gear" }, { Icon::play, "play" },
        { Icon::flag, "flag" }, { Icon::heart, "heart" }, { Icon::wave, "wave" }, { Icon::note, "note" },
        { Icon::check, "check" }, { Icon::cross, "cross" }, { Icon::plus, "plus" }, { Icon::eye, "eye" },
        { Icon::magnifier, "magnifier" }, { Icon::scissors, "scissors" } };
    return n;
}
} // namespace

juce::String iconId (Icon icon)
{
    for (const auto& [i, n] : iconNames())
        if (i == icon) return n;
    return {};
}

Icon iconFromId (const juce::String& id)
{
    for (const auto& [i, n] : iconNames())
        if (id == n) return i;
    return Icon::none;
}

const std::vector<Icon>& libraryIcons()
{
    static const std::vector<Icon> v { Icon::star, Icon::bolt, Icon::gear, Icon::play, Icon::flag, Icon::heart,
                                       Icon::wave, Icon::note, Icon::check, Icon::cross, Icon::plus, Icon::eye,
                                       Icon::magnifier, Icon::scissors };
    return v;
}

// ============================================================== keys

namespace
{
struct NamedKey { int code; const char* name; };
const std::vector<NamedKey>& namedKeys()
{
    static const std::vector<NamedKey> k {
        { juce::KeyPress::spaceKey, "space" }, { juce::KeyPress::returnKey, "return" },
        { juce::KeyPress::escapeKey, "escape" }, { juce::KeyPress::deleteKey, "delete" },
        { juce::KeyPress::backspaceKey, "backspace" }, { juce::KeyPress::tabKey, "tab" },
        { juce::KeyPress::leftKey, "left" }, { juce::KeyPress::rightKey, "right" },
        { juce::KeyPress::upKey, "up" }, { juce::KeyPress::downKey, "down" },
        { juce::KeyPress::homeKey, "home" }, { juce::KeyPress::endKey, "end" },
        { juce::KeyPress::pageUpKey, "pageup" }, { juce::KeyPress::pageDownKey, "pagedown" },
        { juce::KeyPress::insertKey, "insert" } };
    return k;
}

juce::String keyName (int code)
{
    for (const auto& k : namedKeys())
        if (k.code == code) return k.name;
    for (int i = 0; i < 12; ++i)
        if (code == juce::KeyPress::F1Key + i) return "f" + juce::String (i + 1);
    if (code >= 'A' && code <= 'Z') code += 'a' - 'A';
    if (code <= 32 || code > 0xffff) return {};
    return juce::String::charToString ((juce::juce_wchar) code).toLowerCase();
}

int keyCodeOf (const juce::String& name)
{
    for (const auto& k : namedKeys())
        if (name == k.name) return k.code;
    if (name.length() >= 2 && name[0] == 'f' && name.substring (1).containsOnly ("0123456789"))
    {
        const int n = name.substring (1).getIntValue();
        if (n >= 1 && n <= 12) return juce::KeyPress::F1Key + n - 1;
    }
    if (name.length() == 1) return (int) name[0];
    return 0;
}

juce::String join (bool cmd, bool ctrl, bool alt, bool shift, const juce::String& key)
{
    juce::String s;
    if (cmd)   s << "cmd+";
    if (ctrl)  s << "ctrl+";
    if (alt)   s << "alt+";
    if (shift) s << "shift+";
    return s + key;
}
} // namespace

juce::String canonical (const juce::KeyPress& k)
{
    if (! k.isValid()) return {};
    const auto key = keyName (k.getKeyCode());
    if (key.isEmpty()) return {};
    const auto m = k.getModifiers();
   #if JUCE_MAC
    const bool ctrl = m.isCtrlDown();
   #else
    const bool ctrl = false;   // Ctrl is the command key here
   #endif
    return join (m.isCommandDown(), ctrl, m.isAltDown(), m.isShiftDown(), key);
}

juce::String canonicalFromText (const juce::String& textIn)
{
    const auto text = textIn.trim().toLowerCase();
    if (text.isEmpty()) return {};
    // "Ctrl++" ends with the '+' key itself.
    juce::StringArray parts;
    juce::String rest = text;
    juce::String key;
    if (rest.endsWith ("++")) { key = "+"; rest = rest.dropLastCharacters (2); }
    parts.addTokens (rest, "+", "");
    parts.trim();
    if (key.isEmpty())
    {
        key = parts.size() > 0 ? parts[parts.size() - 1] : juce::String();
        parts.remove (parts.size() - 1);
    }
    bool cmd = false, ctrl = false, alt = false, shift = false;
    for (const auto& p : parts)
    {
        // The menus write "Ctrl+" for the command modifier on every
        // platform (MainWindow::keyPressed tests commandModifier, which is
        // Cmd on macOS), so "Ctrl" is cmd here, macOS included.
        if (p == "ctrl" || p == "control" || p == "cmd" || p == "command") cmd = true;
        else if (p == "alt" || p == "option") alt = true;
        else if (p == "shift") shift = true;
        else if (p.isNotEmpty()) return {};   // not a shortcut ("Paintbrush (25%")
    }
    if (key == "enter") key = "return";
    if (key == "esc")   key = "escape";
    if (key == "del")   key = "delete";
    if (key == "spacebar") key = "space";
    if (key.isEmpty() || keyCodeOf (key) == 0) return {};
    return join (cmd, ctrl, alt, shift, key);
}

juce::KeyPress keyFromCanonical (const juce::String& c)
{
    if (c.isEmpty()) return {};
    juce::String rest = c;
    juce::String key;
    if (rest.endsWith ("++")) { key = "+"; rest = rest.dropLastCharacters (2); }
    juce::StringArray parts;
    parts.addTokens (rest, "+", "");
    if (key.isEmpty())
    {
        key = parts.size() > 0 ? parts[parts.size() - 1] : juce::String();
        parts.remove (parts.size() - 1);
    }
    int mods = 0;
    for (const auto& p : parts)
    {
        if (p == "cmd")   mods |= juce::ModifierKeys::commandModifier;
        if (p == "ctrl")  mods |= juce::ModifierKeys::ctrlModifier;
        if (p == "alt")   mods |= juce::ModifierKeys::altModifier;
        if (p == "shift") mods |= juce::ModifierKeys::shiftModifier;
    }
    const int code = keyCodeOf (key);
    if (code == 0) return {};
    return juce::KeyPress (code, juce::ModifierKeys (mods), 0);
}

juce::String displayText (const juce::String& c)
{
    if (c.isEmpty()) return {};
    juce::String rest = c;
    juce::String key;
    if (rest.endsWith ("++")) { key = "+"; rest = rest.dropLastCharacters (2); }
    juce::StringArray parts;
    parts.addTokens (rest, "+", "");
    if (key.isEmpty())
    {
        key = parts.size() > 0 ? parts[parts.size() - 1] : juce::String();
        parts.remove (parts.size() - 1);
    }
    juce::String out;
    for (const auto& p : parts)
    {
       #if JUCE_MAC
        if (p == "cmd")  out << "Cmd+";
        if (p == "ctrl") out << "Ctrl+";
       #else
        if (p == "cmd")  out << "Ctrl+";
       #endif
        if (p == "alt")   out << "Alt+";
        if (p == "shift") out << "Shift+";
    }
    if (key.length() == 1) return out + key.toUpperCase();
    if (key.startsWith ("f") && key.substring (1).containsOnly ("0123456789")) return out + key.toUpperCase();
    if (key == "pageup")   return out + "Page Up";
    if (key == "pagedown") return out + "Page Down";
    if (key == "escape")   return out + "Esc";
    if (key == "return")   return out + "Enter";
    return out + key.substring (0, 1).toUpperCase() + key.substring (1);
}

// ============================================================== custom shortcuts

juce::String CustomShortcut::describe() const
{
    switch (kind)
    {
        case Kind::menuCommand:  return target;
        case Kind::pythonScript: return "Python script " + juce::File (target).getFileName();
        case Kind::keyPress:     return "keys " + displayText (target);
        case Kind::action:       return "action " + target;
        case Kind::none:
        default:                 return {};
    }
}

juce::String serialise (const CustomSet& set)
{
    juce::Array<juce::var> arr;
    for (const auto& s : set)
    {
        auto* o = new juce::DynamicObject();
        if (s.defined())
        {
            o->setProperty ("kind", (int) s.kind);
            o->setProperty ("target", s.target);
            if (s.label.isNotEmpty()) o->setProperty ("label", s.label);
            if (s.icon.isNotEmpty())  o->setProperty ("icon", s.icon);
            if (s.key.isNotEmpty())   o->setProperty ("key", s.key);
            if (s.tooltip.isNotEmpty()) o->setProperty ("tooltip", s.tooltip);
        }
        arr.add (juce::var (o));
    }
    return juce::JSON::toString (juce::var (arr), true);
}

CustomSet parse (const juce::String& json)
{
    CustomSet set;
    const auto v = juce::JSON::parse (json);
    if (const auto* arr = v.getArray())
        for (int i = 0; i < std::min ((int) set.size(), arr->size()); ++i)
            if (const auto* o = (*arr)[i].getDynamicObject())
            {
                const auto& p = o->getProperties();
                CustomShortcut s;
                const int k = (int) p.getWithDefault ("kind", 0);
                if (k < 1 || k > 4) continue;
                s.kind   = (CustomShortcut::Kind) k;
                s.target = p.getWithDefault ("target", {}).toString();
                s.label  = p.getWithDefault ("label", {}).toString().substring (0, 4);
                s.icon   = iconFromId (p.getWithDefault ("icon", {}).toString()) != Icon::none
                               ? p.getWithDefault ("icon", {}).toString() : juce::String();
                s.key    = p.getWithDefault ("key", {}).toString();
                s.tooltip = p.getWithDefault ("tooltip", {}).toString();
                if (s.kind == CustomShortcut::Kind::keyPress && ! keyFromCanonical (s.target).isValid()) continue;
                if (s.key.isNotEmpty() && ! keyFromCanonical (s.key).isValid()) s.key.clear();
                if (s.defined()) set[(size_t) i] = s;
            }
    return set;
}

// ============================================================== widgets

IconButton::IconButton (const juce::String& id) : juce::Button (id)
{
    setComponentID (id);
    // Clicking a tool must not take the keyboard focus from the views (the
    // single-letter shortcuts keep working right after).
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

void IconButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f, 2.0f);
    if (on)
    {
        g.setColour (juce::Colour (0xff3d6fa8));
        g.fillRoundedRectangle (r, 3.0f);
    }
    else if (isEnabled() && (over || down))
    {
        g.setColour (juce::Colours::white.withAlpha (down ? 0.22f : 0.12f));
        g.fillRoundedRectangle (r, 3.0f);
    }
    const auto ink = juce::Colour (0xffd8d8d8).withMultipliedAlpha (isEnabled() ? 1.0f : 0.35f);
    if (icon != Icon::none)
        drawIcon (g, icon, r.reduced (3.0f, 2.0f), ink);
    else if (label.isNotEmpty())
    {
        g.setColour (placeholder ? ink.withMultipliedAlpha (0.45f) : ink);
        g.setFont (juce::FontOptions (label.length() > 2 ? 9.5f : 11.5f, placeholder ? juce::Font::plain : juce::Font::bold));
        g.drawFittedText (label, r.toNearestInt(), juce::Justification::centred, 1, 0.7f);
    }
}

void IconButton::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        if (onRightClick) onRightClick();
        return;
    }
    juce::Button::mouseDown (e);
}

void IconButton::mouseUp (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    juce::Button::mouseUp (e);
}

KeyCapture::KeyCapture (const juce::String& caption) : caption_ (caption)
{
    setWantsKeyboardFocus (true);
    setSize (300, 24);
    setTooltip ("Click here, then press the keys (Ctrl, Shift, Alt and a key). The x clears it.");
}

void KeyCapture::paint (juce::Graphics& g)
{
    auto r = getLocalBounds();
    g.setColour (juce::Colour (0xff232327));
    g.fillRoundedRectangle (r.toFloat(), 3.0f);
    g.setColour (hasKeyboardFocus (false) ? juce::Colour (0xff4ea1f5) : juce::Colour (0xff555555));
    g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 1.0f);
    const auto box = clearBox();
    r.removeFromRight (box.getWidth());
    g.setFont (juce::FontOptions (12.0f));
    g.setColour (juce::Colours::lightgrey);
    const juce::String shown = key.isNotEmpty() ? displayText (key)
                             : (hasKeyboardFocus (false) ? juce::String ("press the keys...") : juce::String ("none"));
    g.drawText (caption_ + ": " + shown, r.reduced (6, 0), juce::Justification::centredLeft, true);
    if (key.isNotEmpty())
        drawIcon (g, Icon::cross, box.toFloat().reduced (7.0f), juce::Colours::grey);
}

void KeyCapture::mouseDown (const juce::MouseEvent& e)
{
    if (key.isNotEmpty() && clearBox().contains (e.getPosition()))
    {
        key.clear();
        repaint();
        if (onChange) onChange();
        return;
    }
    grabKeyboardFocus();
}

bool KeyCapture::keyPressed (const juce::KeyPress& k)
{
    const auto m = k.getModifiers();
    const bool plain = ! m.isCommandDown() && ! m.isCtrlDown() && ! m.isAltDown() && ! m.isShiftDown();
    if (plain && (k.getKeyCode() == juce::KeyPress::returnKey || k.getKeyCode() == juce::KeyPress::escapeKey))
        return false;   // the dialog's OK / Cancel
    const auto c = canonical (k);
    if (c.isEmpty()) return true;
    key = c;
    repaint();
    if (onChange) onChange();
    return true;
}

const std::vector<std::vector<Toolbar::Spec>>& Toolbar::groups()
{
    static const std::vector<std::vector<Spec>> g {
        { { "undo", Icon::undo, "Undo" }, { "redo", Icon::redo, "Redo" } },
        { { "tool.timeRange", Icon::timeRange, "Time Range" }, { "tool.marquee", Icon::marquee, "Marquee" },
          { "tool.lasso", Icon::lasso, "Lasso" }, { "tool.paintbrush", Icon::paintbrush, "Paintbrush" },
          { "tool.move", Icon::move, "Move (drawings, session files)" } },
        { { "view.toggle", Icon::toggleViews, "Waveform / Spectrogram" }, { "marker.add", Icon::addMarker, "Add Marker" } },
        { { "draw.freehand", Icon::freehand, "Free Drawing" }, { "draw.text", Icon::text, "Text" },
          { "draw.arrow", Icon::arrow, "Arrow" }, { "draw.rectangle", Icon::rectangle, "Rectangle" },
          { "draw.oval", Icon::oval, "Oval" } } };
    return g;
}

Toolbar::Toolbar()
{
    setComponentID ("main-toolbar");
    const auto& gs = groups();
    for (int gi = 0; gi < (int) gs.size(); ++gi)
        for (const auto& spec : gs[(size_t) gi])
        {
            auto b = std::make_unique<IconButton> (spec.id);
            b->icon = spec.icon;
            b->setTooltip (spec.name);
            const juce::String id = spec.id;
            b->onClick = [this, id] { if (onCommand) onCommand (id); };
            b->onRightClick = [this, id] { if (onContextMenu) onContextMenu (id); };
            addAndMakeVisible (*b);
            buttons_.push_back (std::move (b));
            groupOf_.push_back (gi);
        }
    for (int i = 0; i < kNumCustom; ++i)
    {
        const juce::String id = "custom." + juce::String (i + 1);
        auto b = std::make_unique<IconButton> (id);
        b->onClick = [this, id] { if (onCommand) onCommand (id); };
        b->onRightClick = [this, id] { if (onContextMenu) onContextMenu (id); };
        addAndMakeVisible (*b);
        buttons_.push_back (std::move (b));
        groupOf_.push_back ((int) gs.size());
    }
    setCustom ({});
}

int Toolbar::numGroups() const
{
    return (int) groups().size() + 1;
}

IconButton* Toolbar::button (const juce::String& id) const
{
    for (const auto& b : buttons_)
        if (b->getComponentID() == id) return b.get();
    return nullptr;
}

void Toolbar::setCustom (const CustomSet& set)
{
    custom_ = set;
    refresh();
    resized();   // the defined slots show, the others do not
}

int Toolbar::firstFreeCustomSlot() const
{
    for (int i = 0; i < kNumCustom; ++i)
        if (! custom_[(size_t) i].defined()) return i;
    return -1;
}

void Toolbar::setHiddenIds (const juce::StringArray& ids)
{
    hidden_ = ids;
    resized();
}

bool Toolbar::isShown (const juce::String& id) const
{
    if (hidden_.contains (id)) return false;
    if (id.startsWith ("custom."))
    {
        const int n = id.fromFirstOccurrenceOf (".", false, false).getIntValue();
        return n >= 1 && n <= kNumCustom && custom_[(size_t) n - 1].defined();
    }
    return button (id) != nullptr;
}

juce::String Toolbar::nameOf (const juce::String& id)
{
    for (const auto& g : groups())
        for (const auto& spec : g)
            if (spec.id == id) return spec.name;
    return {};
}

Icon Toolbar::iconOf (const juce::String& id)
{
    for (const auto& g : groups())
        for (const auto& spec : g)
            if (spec.id == id) return spec.icon;
    return Icon::none;
}

void Toolbar::refresh()
{
    for (const auto& b : buttons_)
    {
        const auto id = b->getComponentID();
        if (id.startsWith ("custom."))
        {
            const int n = id.fromFirstOccurrenceOf (".", false, false).getIntValue();
            const auto& s = custom_[(size_t) juce::jlimit (1, kNumCustom, n) - 1];
            const bool wasPlaceholder = b->placeholder;
            const auto oldIcon = b->icon;
            const auto oldLabel = b->label;
            juce::String tip;
            const auto keyText = s.key.isNotEmpty() ? " (" + displayText (s.key) + ")" : juce::String();
            if (s.defined())
            {
                b->placeholder = false;
                b->icon = iconFromId (s.icon);
                b->label = s.label.isNotEmpty() ? s.label : juce::String (n);
                // The user's own text when there is one (issue #50).
                tip = s.tooltip.isNotEmpty() ? s.tooltip + keyText
                                             : "Custom button " + juce::String (n) + ": " + s.describe() + keyText
                                                   + ". Right-click to change it.";
            }
            else
            {
                // Not shown (issue #50): Add Custom Button fills it.
                b->placeholder = true;
                b->icon = Icon::none;
                b->label = juce::String (n);
                tip = "Custom button " + juce::String (n) + ": not defined yet.";
            }
            if (b->getTooltip() != tip) b->setTooltip (tip);
            if (wasPlaceholder != b->placeholder || oldIcon != b->icon || oldLabel != b->label)
                b->repaint();
            continue;
        }
        const bool on = isOn ? isOn (id) : false;
        const bool enabled = isEnabled ? isEnabled (id) : true;
        if (on != b->on) { b->on = on; b->repaint(); }
        if (enabled != b->isEnabled()) b->setEnabled (enabled);
        juce::String name;
        for (const auto& g : groups())
            for (const auto& spec : g)
                if (spec.id == id) name = spec.name;
        const auto key = shortcutFor ? shortcutFor (id) : juce::String();
        const auto tip = name + (key.isNotEmpty() ? " (" + key + ")" : juce::String());
        if (b->getTooltip() != tip) b->setTooltip (tip);
    }
}

int Toolbar::idealWidth() const
{
    int w = kGripW + 3, groupsShown = 0;
    for (int gi = 0; gi < numGroups(); ++gi)
    {
        int n = 0;
        for (size_t i = 0; i < buttons_.size(); ++i)
            if (groupOf_[i] == gi && isShown (buttons_[i]->getComponentID())) ++n;
        if (n == 0) continue;
        w += n * kButtonW + (groupsShown > 0 ? kGapW : 0);
        ++groupsShown;
    }
    return groupsShown > 0 ? w : 0;
}

void Toolbar::setVertical (bool v)
{
    if (v == vertical_) return;
    vertical_ = v;
    resized();
}

juce::Rectangle<int> Toolbar::gripBounds() const
{
    return vertical_ ? getLocalBounds().withHeight (kGripW) : getLocalBounds().withWidth (kGripW);
}

void Toolbar::resized()
{
    // Groups along the bar after the grip (left to right, or top to bottom
    // in a column); the groups that do not fit are hidden, the custom
    // shortcuts first.
    // Hidden buttons and empty custom slots take no room (issue #50).
    separatorsX_.clear();
    const int length = vertical_ ? getHeight() : getWidth();
    int x = kGripW, groupsShown = 0;
    for (int gi = 0; gi < numGroups(); ++gi)
    {
        int n = 0;
        for (size_t i = 0; i < buttons_.size(); ++i)
            if (groupOf_[i] == gi && isShown (buttons_[i]->getComponentID())) ++n;
        const int need = n * kButtonW + (groupsShown > 0 ? kGapW : 0);
        const bool fits = n > 0 && x + need <= length - 3;
        if (fits && groupsShown > 0)
        {
            separatorsX_.push_back (x + kGapW / 2);
            x += kGapW;
        }
        for (size_t i = 0; i < buttons_.size(); ++i)
            if (groupOf_[i] == gi)
            {
                const bool show = fits && isShown (buttons_[i]->getComponentID());
                buttons_[i]->setVisible (show);
                if (show)
                {
                    buttons_[i]->setBounds (vertical_ ? juce::Rectangle<int> (0, x, getWidth(), kButtonW)
                                                      : juce::Rectangle<int> (x, 0, kButtonW, getHeight()));
                    x += kButtonW;
                }
            }
        if (fits) ++groupsShown;
        else if (n > 0) x = length;   // keep the order: nothing after a group that did not fit
    }
    repaint();
}

void Toolbar::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        if (onContextMenu)
            onContextMenu ({});   // between the buttons, or the grip
        return;
    }
    gripDown_ = gripBounds().contains (e.getPosition());
    gripDragging_ = false;
}

void Toolbar::mouseDrag (const juce::MouseEvent& e)
{
    if (! gripDown_) return;
    if (! gripDragging_ && e.getDistanceFromDragStart() < 4) return;
    gripDragging_ = true;
    if (onDragMove) onDragMove (e.getScreenPosition());
}

void Toolbar::mouseUp (const juce::MouseEvent& e)
{
    const bool dropped = gripDown_ && gripDragging_;
    gripDown_ = gripDragging_ = false;
    if (dropped && onDragEnd) onDragEnd (e.getScreenPosition());
}

void Toolbar::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (gripBounds().contains (e.getPosition()) ? juce::MouseCursor::DraggingHandCursor
                                                            : juce::MouseCursor::NormalCursor);
}

void Toolbar::paint (juce::Graphics& g)
{
    // The grip: two lines of dots across the bar's start.
    g.setColour (juce::Colour (0xff6a6a6a));
    const auto c = gripBounds().getCentre().toFloat();
    for (int i = -2; i <= 2; ++i)
        for (float along : { -1.5f, 1.5f })
        {
            const float across = (float) i * 3.0f;
            const auto dot = vertical_ ? c + juce::Point<float> (across, along)
                                       : c + juce::Point<float> (along, across);
            g.fillRect (juce::Rectangle<float> (1.5f, 1.5f).withCentre (dot));
        }
    g.setColour (juce::Colour (0xff3a3a3a));
    for (int sx : separatorsX_)
    {
        if (vertical_)
            g.fillRect (getWidth() / 2 - 7, sx, 14, 1);
        else
            g.fillRect (sx, getHeight() / 2 - 7, 1, 14);
    }
}

// ---------------------------------------------------------------- placement

juce::String Placement::toString() const
{
    static const char* const docks[] = { "menuRow", "top", "bottom", "left", "right" };
    static const char* const aligns[] = { "start", "centre", "end" };
    return juce::String (docks[(int) dock]) + ":" + aligns[(int) align];
}

Placement Placement::fromString (const juce::String& s)
{
    static const juce::StringArray docks { "menuRow", "top", "bottom", "left", "right" };
    static const juce::StringArray aligns { "start", "centre", "end" };
    const int d = docks.indexOf (s.upToFirstOccurrenceOf (":", false, false));
    const int a = aligns.indexOf (s.fromFirstOccurrenceOf (":", false, false));
    if (d < 0 || a < 0)
        return {};
    return { (Dock) d, (Align) a };
}

std::optional<Placement> placementAt (juce::Point<int> p, juce::Rectangle<int> menuRow,
                                      juce::Rectangle<int> content)
{
    using Dock = Placement::Dock;
    using Align = Placement::Align;
    auto third = [] (int v, int lo, int len)
    {
        if (len <= 0) return Align::start;
        const int t = juce::jlimit (0, 2, (v - lo) * 3 / len);
        return t == 0 ? Align::start : (t == 1 ? Align::centre : Align::end);
    };
    // The menu row (a few pixels of slack above and below).
    if (p.y < menuRow.getBottom() + 4 && p.x >= menuRow.getX() && p.x < menuRow.getRight())
        return Placement { Dock::menuRow, third (p.x, menuRow.getX(), menuRow.getWidth()) };
    if (! content.contains (p))
        return std::nullopt;
    const int zone = juce::jmin (kDropZone, content.getWidth() / 4, content.getHeight() / 4);
    if (p.x < content.getX() + zone)
        return Placement { Dock::left, third (p.y, content.getY(), content.getHeight()) };
    if (p.x >= content.getRight() - zone)
        return Placement { Dock::right, third (p.y, content.getY(), content.getHeight()) };
    if (p.y < content.getY() + zone)
        return Placement { Dock::top, third (p.x, content.getX(), content.getWidth()) };
    if (p.y >= content.getBottom() - zone)
        return Placement { Dock::bottom, third (p.x, content.getX(), content.getWidth()) };
    return std::nullopt;
}

int alignedStart (Placement::Align a, int lo, int hi, int length, int centre)
{
    const int maxStart = juce::jmax (lo, hi - length);
    switch (a)
    {
        case Placement::Align::start:  return lo;
        case Placement::Align::end:    return maxStart;
        case Placement::Align::centre: return juce::jlimit (lo, maxStart, centre - length / 2);
    }
    return lo;
}

// ============================================================== icon picker

IconPicker::IconPicker()
{
    setSize (kCell * cellIds().size() + 2, kCell + 2);
    setTooltip ("The button's icon. Text shows the text instead. A darker icon is already on another "
                "custom button; it can still be used.");
}

juce::StringArray IconPicker::cellIds()
{
    juce::StringArray ids { "" };
    for (auto i : libraryIcons())
        ids.add (iconId (i));
    return ids;
}

juce::Rectangle<int> IconPicker::cellBounds (int i) const
{
    return { 1 + i * kCell, 1, kCell, kCell };
}

void IconPicker::paint (juce::Graphics& g)
{
    const auto ids = cellIds();
    for (int i = 0; i < ids.size(); ++i)
    {
        const auto r = cellBounds (i).toFloat();
        const bool sel = ids[i] == selected;
        const bool used = ids[i].isNotEmpty() && usedElsewhere.contains (ids[i]);
        const auto ink = juce::Colour (0xffd8d8d8).withMultipliedAlpha (used && ! sel ? 0.35f : 1.0f);
        if (ids[i].isEmpty())
        {
            g.setColour (ink);
            g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
            g.drawText ("Text", r, juce::Justification::centred, false);
        }
        else
            drawIcon (g, iconFromId (ids[i]), r.reduced (4.0f), ink);
        if (sel)
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (r.reduced (1.0f), 3.0f, 1.5f);
        }
    }
}

void IconPicker::mouseDown (const juce::MouseEvent& e)
{
    const auto ids = cellIds();
    for (int i = 0; i < ids.size(); ++i)
        if (cellBounds (i).contains (e.getPosition()))
        {
            if (selected != ids[i])
            {
                selected = ids[i];
                repaint();
                if (onChange) onChange();
            }
            return;
        }
}

// ============================================================== calculator

namespace calc
{
namespace
{
using Vec = std::vector<double>;

struct Tok
{
    enum Type { num, op, open, close, comma, log, end };
    Type type = end;
    double value = 0.0;
    char c = 0;
};

// Reads the text into tokens; false on a character the calculator does not know.
bool tokenise (const juce::String& text, std::vector<Tok>& out)
{
    const auto t = text.toLowerCase();
    const int n = t.length();
    for (int i = 0; i < n;)
    {
        const auto ch = t[i];
        if (juce::CharacterFunctions::isWhitespace (ch)) { ++i; continue; }
        if (juce::CharacterFunctions::isDigit (ch) || ch == '.')
        {
            int j = i, dots = 0, digits = 0;
            while (j < n && (juce::CharacterFunctions::isDigit (t[j]) || t[j] == '.'))
            {
                if (t[j] == '.') ++dots; else ++digits;
                ++j;
            }
            if (dots > 1 || digits == 0) return false;
            // An exponent only when digits follow ("2e3", "1.5e-4").
            if (j < n && t[j] == 'e')
            {
                int k = j + 1;
                if (k < n && (t[k] == '+' || t[k] == '-')) ++k;
                if (k < n && juce::CharacterFunctions::isDigit (t[k]))
                {
                    while (k < n && juce::CharacterFunctions::isDigit (t[k])) ++k;
                    j = k;
                }
            }
            Tok tok;
            tok.type = Tok::num;
            tok.value = t.substring (i, j).getDoubleValue();
            out.push_back (tok);
            i = j;
            continue;
        }
        if (juce::CharacterFunctions::isLetter (ch))
        {
            int j = i;
            while (j < n && juce::CharacterFunctions::isLetter (t[j])) ++j;
            if (t.substring (i, j) != "log") return false;
            Tok tok;
            tok.type = Tok::log;
            out.push_back (tok);
            i = j;
            continue;
        }
        Tok tok;
        switch (ch)
        {
            case '+': case '-': case '*': case '/': case '^': tok.type = Tok::op; tok.c = (char) ch; break;
            case '(': tok.type = Tok::open; break;
            case ')': tok.type = Tok::close; break;
            case ',': tok.type = Tok::comma; break;
            default: return false;
        }
        out.push_back (tok);
        ++i;
    }
    return true;
}

// Unmatched ')' open at the start, unmatched '(' close at the end.
void balance (std::vector<Tok>& toks)
{
    int depth = 0, missingOpen = 0;
    for (const auto& t : toks)
    {
        if (t.type == Tok::open) ++depth;
        else if (t.type == Tok::close) { if (depth > 0) --depth; else ++missingOpen; }
    }
    Tok o; o.type = Tok::open;
    Tok c; c.type = Tok::close;
    toks.insert (toks.begin(), (size_t) missingOpen, o);
    toks.insert (toks.end(), (size_t) depth, c);
}

class Parser
{
public:
    explicit Parser (std::vector<Tok> t) : toks (std::move (t)) { toks.push_back ({}); }
    bool failed = false;

    std::optional<Vec> all()
    {
        auto v = list();
        if (peek().type != Tok::end) failed = true;
        return v;
    }

private:
    std::vector<Tok> toks;
    size_t at = 0;

    const Tok& peek() const { return toks[std::min (at, toks.size() - 1)]; }
    bool isOp (char c) const { return peek().type == Tok::op && peek().c == c; }
    static bool startsOperand (const Tok& t) { return t.type == Tok::num || t.type == Tok::open || t.type == Tok::log; }

    Vec combine (const Vec& a, const Vec& b, char op)
    {
        if (a.size() != b.size() && a.size() != 1 && b.size() != 1) { failed = true; return { 0.0 }; }
        const size_t n = std::max (a.size(), b.size());
        Vec r (n);
        for (size_t i = 0; i < n; ++i)
        {
            const double x = a[a.size() == 1 ? 0 : i], y = b[b.size() == 1 ? 0 : i];
            switch (op)
            {
                case '+': r[i] = x + y; break;
                case '-': r[i] = x - y; break;
                case '*': r[i] = x * y; break;
                case '/': if (y == 0.0) failed = true; r[i] = y != 0.0 ? x / y : 0.0; break;
                case '^': r[i] = std::pow (x, y); break;
                default: failed = true; break;
            }
            if (! std::isfinite (r[i])) failed = true;
        }
        return r;
    }

    // list := expr (',' expr)*
    std::optional<Vec> list()
    {
        auto a = expr();
        if (peek().type != Tok::comma) return a;
        Vec out = a.value_or (Vec { 0.0 });
        while (peek().type == Tok::comma)
        {
            ++at;
            const auto b = expr().value_or (Vec { 0.0 });
            out.insert (out.end(), b.begin(), b.end());
        }
        return out;
    }

    // expr := term (('+' | '-') term)*; a missing term is 0.
    std::optional<Vec> expr()
    {
        auto a = term();
        while (isOp ('+') || isOp ('-'))
        {
            const char c = peek().c;
            ++at;
            const auto b = term();
            a = combine (a.value_or (Vec { 0.0 }), b.value_or (Vec { 0.0 }), c);
        }
        return a;
    }

    // term := unary (('*' | '/' | nothing) unary)*; a missing factor is 1.
    std::optional<Vec> term()
    {
        auto a = unary();
        for (;;)
        {
            char c = 0;
            if (isOp ('*') || isOp ('/')) { c = peek().c; ++at; }
            else if (a.has_value() && startsOperand (peek())) c = '*';   // "2(3)", "2log(100)"
            else break;
            const auto b = unary();
            a = combine (a.value_or (Vec { 1.0 }), b.value_or (Vec { 1.0 }), c);
        }
        return a;
    }

    // unary := ('-' | '+') unary | power; a sign with nothing after it is dropped.
    std::optional<Vec> unary()
    {
        if (isOp ('-') || isOp ('+'))
        {
            const bool neg = peek().c == '-';
            ++at;
            auto a = unary();
            if (a.has_value() && neg)
                for (auto& x : *a) x = -x;
            return a;
        }
        return power();
    }

    // power := primary ('^' unary)?  (right-associative); a missing exponent is 1.
    std::optional<Vec> power()
    {
        auto base = primary();
        if (! isOp ('^')) return base;
        ++at;
        const auto e = unary();
        return combine (base.value_or (Vec { 1.0 }), e.value_or (Vec { 1.0 }), '^');
    }

    std::optional<Vec> primary()
    {
        const auto t = peek();
        if (t.type == Tok::num) { ++at; return Vec { t.value }; }
        if (t.type == Tok::open)
        {
            ++at;
            auto v = list();
            if (peek().type == Tok::close) ++at;
            return v;
        }
        if (t.type == Tok::log)
        {
            ++at;
            auto x = power().value_or (Vec { 1.0 });
            for (auto& v : x)
            {
                if (v <= 0.0) { failed = true; v = 0.0; }
                else v = std::log10 (v);
            }
            return x;
        }
        return std::nullopt;
    }
};
} // namespace

juce::String format (double v)
{
    if (v == 0.0) return "0";   // and -0
    char buf[64];
    std::snprintf (buf, sizeof (buf), "%.10g", v);
    return juce::String (buf);
}

Result evaluate (const juce::String& text)
{
    Result r;
    std::vector<Tok> toks;
    if (! tokenise (text, toks))
    {
        r.empty = false;
        r.text = "Error";
        return r;
    }
    if (toks.empty()) return r;
    balance (toks);
    Parser p (toks);
    const auto v = p.all();
    if (p.failed)
    {
        r.empty = false;
        r.text = "Error";
        return r;
    }
    if (! v.has_value()) return r;   // only signs or "()": nothing to show yet
    r.empty = false;
    r.ok = true;
    r.values = *v;
    juce::StringArray parts;
    for (double x : r.values) parts.add (format (x));
    r.text = parts.joinIntoString (", ");
    return r;
}
} // namespace calc

Calculator::Calculator()
{
    setComponentID ("calculator");
    const juce::String tip ("Calculator: + - * / ^ ( ) log, '.' for decimals, a comma for a list: "
                            "(1,2,3)+1 = 2, 3, 4. The result follows as you type; Enter keeps it, Esc clears.");
    setTooltip (tip);
    field_.setTooltip (tip);
    field_.setComponentID ("calculator-field");
    field_.setMultiLine (false);
    field_.setFont (juce::FontOptions (12.5f));
    field_.setIndents (4, 1);
    field_.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff232327));
    field_.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff3a3a3a));
    field_.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (0xff4ea1f5));
    field_.setColour (juce::TextEditor::textColourId, juce::Colour (0xffe0e0e0));
    field_.setTextToShowWhenEmpty ("calc", juce::Colour (0xff6a6a6a));
    field_.onTextChange = [this] { update(); };
    field_.onReturnKey = [this] { commit(); };
    field_.onEscapeKey = [this]
    {
        field_.clear();
        update();
        field_.giveAwayKeyboardFocus();
    };
    addAndMakeVisible (field_);
}

void Calculator::setExpression (const juce::String& text)
{
    field_.setText (text, false);
    update();
}

void Calculator::commit()
{
    const auto r = calc::evaluate (field_.getText());
    if (r.ok)
        setExpression (r.text.removeCharacters (" "));
}

void Calculator::update()
{
    const auto r = calc::evaluate (field_.getText());
    error_ = ! r.empty && ! r.ok;
    const auto shown = r.empty ? juce::String() : "= " + r.text;
    if (shown != result_)
    {
        result_ = shown;
        repaint();
    }
}

void Calculator::resized()
{
    field_.setBounds (getLocalBounds().withWidth (getWidth() * 11 / 20).reduced (0, 3));
}

juce::String Calculator::answerValue() const
{
    if (error_ || ! result_.startsWith ("= "))
        return {};
    return result_.substring (2);
}

juce::Rectangle<int> Calculator::answerBounds() const
{
    return getLocalBounds().withTrimmedLeft (field_.getRight());
}

juce::PopupMenu Calculator::answerMenu() const
{
    juce::PopupMenu m;
    m.addItem (kCopyAnswerId, "Copy", answerValue().isNotEmpty());
    return m;
}

void Calculator::copyAnswer() const
{
    if (const auto v = answerValue(); v.isNotEmpty())
        juce::SystemClipboard::copyTextToClipboard (v);
}

void Calculator::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        return;
    // The answer's own menu (issue #66); elsewhere, the toolbar menu.
    if (answerValue().isNotEmpty() && answerBounds().contains (e.getPosition()))
    {
        juce::Component::SafePointer<Calculator> safe (this);
        answerMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                                                               .withMousePosition(),
                                    [safe] (int id)
                                    {
                                        if (safe != nullptr && id == kCopyAnswerId)
                                            safe->copyAnswer();
                                    });
        return;
    }
    if (onContextMenu)
        onContextMenu();
}

void Calculator::paint (juce::Graphics& g)
{
    if (result_.isEmpty()) return;
    const auto r = getLocalBounds().withTrimmedLeft (field_.getRight() + 5);
    g.setColour (error_ ? juce::Colour (0xffe8806a) : juce::Colour (0xffd8d8d8));
    g.setFont (juce::FontOptions (12.5f));
    g.drawFittedText (result_, r, juce::Justification::centredLeft, 1, 0.8f);
}

// ============================================================== manage buttons

namespace
{
// The icon (or the text) of a row of the Manage Visible Buttons list.
class IconCell : public juce::Component
{
public:
    IconCell (Icon i, juce::String t) : icon (i), text (std::move (t)) { setInterceptsMouseClicks (false, false); }
    void paint (juce::Graphics& g) override
    {
        const auto ink = juce::Colour (0xffd8d8d8);
        if (icon != Icon::none)
            drawIcon (g, icon, getLocalBounds().toFloat().reduced (4.0f), ink);
        else
        {
            g.setColour (ink);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawFittedText (text, getLocalBounds(), juce::Justification::centred, 1, 0.7f);
        }
    }
private:
    Icon icon;
    juce::String text;
};
} // namespace

ManageButtons::ManageButtons (const Toolbar& bar) : bar_ (bar)
{
    setComponentID ("manage-buttons");
    view_.setViewedComponent (&content_, false);
    view_.setScrollBarsShown (true, false);
    addAndMakeVisible (view_);
    add_ = std::make_unique<juce::TextButton> ("Add Custom Button...");
    add_->onClick = [this] { if (onAdd) onAdd(); };
    addAndMakeVisible (*add_);
    showAll_ = std::make_unique<juce::TextButton> ("Show All Buttons");
    showAll_->setTooltip ("Shows every predetermined button and the calculator again.");
    showAll_->onClick = [this] { if (onShowAll) onShowAll(); };
    addAndMakeVisible (*showAll_);
    setSize (460, 480);
}

void ManageButtons::rebuild()
{
    rows_.clear();
    customHeader_.reset();
    auto addPredetermined = [this] (const juce::String& id, Icon icon, const juce::String& name, const juce::String& text)
    {
        auto row = std::make_unique<Row>();
        row->id = id;
        row->icon = std::make_unique<IconCell> (icon, text);
        row->toggle = std::make_unique<juce::ToggleButton> (name);
        row->toggle->setComponentID ("show-" + id);
        row->toggle->setToggleState (isShown ? isShown (id) : true, juce::dontSendNotification);
        row->toggle->onClick = [this, id, t = row->toggle.get()] { if (onShow) onShow (id, t->getToggleState()); };
        content_.addAndMakeVisible (*row->icon);
        content_.addAndMakeVisible (*row->toggle);
        rows_.push_back (std::move (row));
    };
    for (const auto& g : Toolbar::groups())
        for (const auto& spec : g)
            addPredetermined (spec.id, spec.icon, spec.name, {});
    addPredetermined ("calculator", Icon::none, "Calculator", "=");

    customHeader_ = std::make_unique<juce::Label> (juce::String(), "Custom buttons");
    customHeader_->setFont (juce::FontOptions (13.0f, juce::Font::bold));
    content_.addAndMakeVisible (*customHeader_);
    bool any = false;
    for (int slot = 0; slot < kNumCustom; ++slot)
    {
        const auto& s = bar_.custom()[(size_t) slot];
        if (! s.defined()) continue;
        any = true;
        auto row = std::make_unique<Row>();
        row->id = "custom." + juce::String (slot + 1);
        row->icon = std::make_unique<IconCell> (iconFromId (s.icon), s.label.isNotEmpty() ? s.label : juce::String (slot + 1));
        row->label = std::make_unique<juce::Label> (juce::String(), juce::String (slot + 1) + ": "
                                                        + (s.tooltip.isNotEmpty() ? s.tooltip : s.describe()));
        row->edit = std::make_unique<juce::TextButton> ("Edit...");
        row->edit->setComponentID ("edit-" + row->id);
        row->edit->onClick = [this, slot] { if (onEdit) onEdit (slot); };
        row->remove = std::make_unique<juce::TextButton> ("Remove");
        row->remove->setComponentID ("remove-" + row->id);
        row->remove->onClick = [this, slot] { if (onRemove) onRemove (slot); };
        for (juce::Component* c : { (juce::Component*) row->icon.get(), (juce::Component*) row->label.get(),
                                    (juce::Component*) row->edit.get(), (juce::Component*) row->remove.get() })
            content_.addAndMakeVisible (*c);
        rows_.push_back (std::move (row));
    }
    if (! any)
        customHeader_->setText ("Custom buttons: none yet (Add Custom Button)", juce::dontSendNotification);
    add_->setEnabled (bar_.firstFreeCustomSlot() >= 0);
    add_->setTooltip (add_->isEnabled() ? "A new button: the form opens for custom button "
                                              + juce::String (bar_.firstFreeCustomSlot() + 1) + "."
                                        : "All nine custom buttons are in use.");
    layoutContent();
}

void ManageButtons::layoutContent()
{
    const int w = std::max (300, view_.getWidth() - view_.getScrollBarThickness());
    constexpr int rowH = 26;
    int y = 4;
    bool headerPlaced = false;
    for (auto& row : rows_)
    {
        const bool custom = row->id.startsWith ("custom.");
        if (custom && ! headerPlaced)
        {
            customHeader_->setBounds (4, y + 6, w - 8, 20);
            y += 30;
            headerPlaced = true;
        }
        auto r = juce::Rectangle<int> (4, y, w - 8, rowH);
        row->icon->setBounds (r.removeFromLeft (rowH));
        r.removeFromLeft (6);
        if (custom)
        {
            row->remove->setBounds (r.removeFromRight (72).reduced (0, 2));
            r.removeFromRight (4);
            row->edit->setBounds (r.removeFromRight (64).reduced (0, 2));
            row->label->setBounds (r);
        }
        else
            row->toggle->setBounds (r);
        y += rowH;
    }
    if (! headerPlaced && customHeader_ != nullptr)
    {
        customHeader_->setBounds (4, y + 6, w - 8, 20);
        y += 30;
    }
    content_.setSize (w, y + 4);
}

void ManageButtons::resized()
{
    auto r = getLocalBounds().reduced (6);
    auto bottom = r.removeFromBottom (28);
    showAll_->setBounds (bottom.removeFromRight (150));
    bottom.removeFromRight (8);
    add_->setBounds (bottom.removeFromRight (170));
    r.removeFromBottom (6);
    view_.setBounds (r);
    layoutContent();
}

juce::ToggleButton* ManageButtons::toggleFor (const juce::String& id) const
{
    for (const auto& row : rows_)
        if (row->id == id && row->toggle != nullptr) return row->toggle.get();
    return nullptr;
}

juce::Button* ManageButtons::editButton (int slot) const
{
    const auto id = "custom." + juce::String (slot + 1);
    for (const auto& row : rows_)
        if (row->id == id) return row->edit.get();
    return nullptr;
}

juce::Button* ManageButtons::removeButton (int slot) const
{
    const auto id = "custom." + juce::String (slot + 1);
    for (const auto& row : rows_)
        if (row->id == id) return row->remove.get();
    return nullptr;
}
} // namespace sc::toolbar
