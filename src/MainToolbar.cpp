// SpectroCraft - the toolbar in the menu bar row (issue #43). See MainToolbar.h.
#include "MainToolbar.h"

#include <cmath>

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
            // A waveform above, a spectrogram's bars below.
            stroke.addRectangle (1.5f, 1.5f, 13.0f, 5.5f);
            stroke.startNewSubPath (3.0f, 4.25f);
            for (int i = 0; i < 10; ++i)
                stroke.lineTo (3.0f + (float) (i + 1), 4.25f + ((i % 2 == 0) ? -1.6f : 1.6f));
            stroke.addRectangle (1.5f, 9.0f, 13.0f, 5.5f);
            for (int i = 0; i < 5; ++i)
                fill.addRectangle (3.0f + 2.2f * (float) i, 13.0f - (float) (1 + (i * 3) % 4), 1.4f, (float) (1 + (i * 3) % 4));
            break;
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
            addAndMakeVisible (*b);
            buttons_.push_back (std::move (b));
            groupOf_.push_back (gi);
        }
    for (int i = 0; i < kNumCustom; ++i)
    {
        const juce::String id = "custom." + juce::String (i + 1);
        auto b = std::make_unique<IconButton> (id);
        b->onClick = [this, id] { if (onCommand) onCommand (id); };
        b->onRightClick = [this, i] { if (onCustomMenu) onCustomMenu (i); };
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
            if (s.defined())
            {
                b->placeholder = false;
                b->icon = iconFromId (s.icon);
                b->label = s.label.isNotEmpty() ? s.label : juce::String (n);
                tip = "Shortcut " + juce::String (n) + ": " + s.describe()
                    + (s.key.isNotEmpty() ? " (" + displayText (s.key) + ")" : juce::String())
                    + ". Right-click to change it.";
            }
            else
            {
                b->placeholder = true;
                b->icon = Icon::none;
                b->label = juce::String (n);
                tip = "Shortcut " + juce::String (n) + ": not defined yet. Click or right-click to "
                      "define it: a menu command or effect, a recorded action, a Python script or keys to send.";
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
    return (int) buttons_.size() * kButtonW + (numGroups() - 1) * kGapW + 6;
}

void Toolbar::resized()
{
    // Groups left to right; the groups that do not fit are hidden, the
    // custom shortcuts first.
    separatorsX_.clear();
    int x = 3;
    for (int gi = 0; gi < numGroups(); ++gi)
    {
        int n = 0;
        for (int g : groupOf_) if (g == gi) ++n;
        const int need = n * kButtonW + (gi > 0 ? kGapW : 0);
        const bool fits = x + need <= getWidth() - 3;
        if (fits && gi > 0)
        {
            separatorsX_.push_back (x + kGapW / 2);
            x += kGapW;
        }
        for (size_t i = 0; i < buttons_.size(); ++i)
            if (groupOf_[i] == gi)
            {
                buttons_[i]->setVisible (fits);
                if (fits)
                {
                    buttons_[i]->setBounds (x, 0, kButtonW, getHeight());
                    x += kButtonW;
                }
            }
        if (! fits) x = getWidth();   // keep the order: nothing after a group that did not fit
    }
    repaint();
}

void Toolbar::paint (juce::Graphics& g)
{
    g.setColour (juce::Colour (0xff3a3a3a));
    for (int sx : separatorsX_)
        g.fillRect (sx, getHeight() / 2 - 7, 1, 14);
}
} // namespace sc::toolbar
