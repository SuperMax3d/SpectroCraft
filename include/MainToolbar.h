#pragma once
// SpectroCraft - the toolbar in the menu bar row, left of the workspace
// presets (issue #43):
//
//   Undo, Redo | Time Range, Marquee, Lasso, Paintbrush, Move |
//   Waveform / Spectrogram (F9), Add Marker (F8) |
//   Free Drawing, Text, Arrow, Rectangle, Oval | Shortcuts 1..9
//
// The icons are drawn here with vector paths (original artwork, no
// third-party icon set). The nine shortcuts are the user's own: a right-click
// defines one - a menu command (effects included), a Python script or a
// keyboard shortcut to send (Ctrl+H shows the harmonics...) - shown with a
// short text or an icon of a small library, with an optional keyboard
// shortcut of its own (checked against the shortcuts in use). MainWindow
// wires the commands and saves the shortcuts in the settings store.

#include <JuceHeader.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace sc::toolbar
{
// ---------------------------------------------------------------- icons

enum class Icon
{
    none,
    undo, redo, timeRange, marquee, lasso, paintbrush, move, toggleViews, addMarker,
    freehand, text, arrow, rectangle, oval,
    // The library offered for the custom shortcuts.
    star, bolt, gear, play, flag, heart, wave, note, check, cross, plus, eye, magnifier, scissors
};

/** Draws `icon` inside `area` with strokes / fills of colour `c`. */
void drawIcon (juce::Graphics& g, Icon icon, juce::Rectangle<float> area, juce::Colour c);
/** Stable name of an icon ("star"), saved in the settings; "" for none. */
juce::String iconId (Icon icon);
Icon iconFromId (const juce::String& id);
/** The icons a custom shortcut can use. */
const std::vector<Icon>& libraryIcons();

// ---------------------------------------------------------------- keys

/** Canonical form of a key: "cmd+shift+h", "f9", "space" (cmd = Ctrl on
    Windows / Linux, Cmd on macOS). Two keys are the same when their
    canonical forms are equal. */
juce::String canonical (const juce::KeyPress& k);
/** The canonical form of a shortcut as the menus show it ("Ctrl+Shift+H",
    "F9", "V", "Ctrl+="); "" when it is not one. */
juce::String canonicalFromText (const juce::String& text);
/** The key of a canonical form (invalid KeyPress for "" or garbage). */
juce::KeyPress keyFromCanonical (const juce::String& canonicalKey);
/** How a canonical key reads in tooltips and menus: "Ctrl+Shift+H". */
juce::String displayText (const juce::String& canonicalKey);

// ---------------------------------------------------------------- custom shortcuts

struct CustomShortcut
{
    enum class Kind { none = 0, menuCommand = 1, pythonScript = 2, keyPress = 3 };
    Kind kind = Kind::none;
    /** menuCommand: the menu path ("Effects > Normalize"); pythonScript: the
        script's full path; keyPress: the canonical key to send ("cmd+h"). */
    juce::String target;
    juce::String label;   // short text on the button (used when there is no icon)
    juce::String icon;    // iconId() of a library icon, "" = the label
    juce::String key;     // the shortcut's own key (canonical), "" = none

    bool defined() const noexcept { return kind != Kind::none && target.isNotEmpty(); }
    /** "Effects > Normalize", "Python: denoise.py", "Keys: Ctrl+H". */
    juce::String describe() const;
    bool operator== (const CustomShortcut& o) const
    {
        return kind == o.kind && target == o.target && label == o.label && icon == o.icon && key == o.key;
    }
};

constexpr int kNumCustom = 9;
using CustomSet = std::array<CustomShortcut, kNumCustom>;

/** JSON text of the nine shortcuts (settings store) and back. Unknown or
    broken entries come back empty. */
juce::String serialise (const CustomSet& set);
CustomSet parse (const juce::String& json);

// ---------------------------------------------------------------- widgets

/** One toolbar button: an icon (or a short text), a highlight while its
    tool is on, a dimmed number for an empty custom slot. */
class IconButton : public juce::Button
{
public:
    explicit IconButton (const juce::String& id);

    Icon icon = Icon::none;
    juce::String label;          // drawn when icon == none
    bool on = false;             // the tool it stands for is on
    bool placeholder = false;    // an empty custom slot: its number, dimmed
    /** Right-click (the custom slots' menu). */
    std::function<void()> onRightClick;

    void paintButton (juce::Graphics& g, bool over, bool down) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
};

/** A field that records the next key pressed while it has the focus (the
    custom shortcuts' dialog): "Ctrl+Shift+H". The x at its right end clears
    it. Plain Enter and Esc go to the dialog (OK / Cancel). */
class KeyCapture : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit KeyCapture (const juce::String& caption);
    juce::String key;   // canonical, "" = none
    std::function<void()> onChange;
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress& k) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
private:
    juce::String caption_;
    juce::Rectangle<int> clearBox() const { return getLocalBounds().removeFromRight (getHeight()); }
};

class Toolbar : public juce::Component
{
public:
    Toolbar();

    struct Spec { juce::String id; Icon icon; juce::String name; };
    /** The fixed groups, in order: "undo", "redo" | "tool.timeRange",
        "tool.marquee", "tool.lasso", "tool.paintbrush", "tool.move" |
        "view.toggle", "marker.add" | "draw.freehand", "draw.text",
        "draw.arrow", "draw.rectangle", "draw.oval". The custom slots follow
        as "custom.1" .. "custom.9". */
    static const std::vector<std::vector<Spec>>& groups();

    /** A click: the id of the button. */
    std::function<void (const juce::String& id)> onCommand;
    /** A right-click on custom slot `slot` (0-based). */
    std::function<void (int slot)> onCustomMenu;
    /** Asked by refresh(): is the button's tool on / is it usable / which
        keyboard shortcut does the same ("" = none). */
    std::function<bool (const juce::String& id)> isOn, isEnabled;
    std::function<juce::String (const juce::String& id)> shortcutFor;

    void setCustom (const CustomSet& set);
    const CustomSet& custom() const noexcept { return custom_; }
    /** Re-reads the state of every button (tools on, enabled, tooltips). */
    void refresh();

    /** Width that shows every group. */
    int idealWidth() const;
    void resized() override;
    void paint (juce::Graphics& g) override;

    IconButton* button (const juce::String& id) const;
    int numButtons() const noexcept { return (int) buttons_.size(); }
    IconButton* buttonAt (int i) const { return buttons_[(size_t) i].get(); }

    static constexpr int kButtonW = 24;
    static constexpr int kGapW = 9;

private:
    std::vector<std::unique_ptr<IconButton>> buttons_;
    std::vector<int> groupOf_;   // group index of each button (the custom slots: the last group)
    std::vector<int> separatorsX_;
    CustomSet custom_;
    int numGroups() const;
};
} // namespace sc::toolbar
