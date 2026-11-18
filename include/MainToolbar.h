#pragma once
// SpectroCraft - the toolbar in the menu bar row, right after the menus
// (issues #43, #50):
//
//   Undo, Redo | Time Range, Marquee, Lasso, Paintbrush, Move |
//   Waveform / Spectrogram (F9), Add Marker (F8) |
//   Free Drawing, Text, Arrow, Rectangle, Oval | custom buttons | calculator
//
// Issue #50: only the custom buttons that are defined show; a right-click on
// the toolbar offers Add Custom Button and Manage Visible Buttons (any
// predetermined button, and the calculator, can be hidden and restored). The
// one-line calculator evaluates as the user types (sc::toolbar::calc).
//
// The icons are drawn here with vector paths (original artwork, no
// third-party icon set). The nine shortcuts are the user's own: a right-click
// defines one - a menu command (effects included), a recorded action (Actions
// panel), a Python script or a keyboard shortcut to send (Ctrl+H shows the
// harmonics...) - shown with a
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
    "F9", "V", "Ctrl+="); "" when it is not one. The menus' "Ctrl" is the
    command modifier on every platform (Cmd on macOS): "Ctrl+Z" -> "cmd+z". */
juce::String canonicalFromText (const juce::String& text);
/** The key of a canonical form (invalid KeyPress for "" or garbage). */
juce::KeyPress keyFromCanonical (const juce::String& canonicalKey);
/** How a canonical key reads in tooltips and menus: "Ctrl+Shift+H". */
juce::String displayText (const juce::String& canonicalKey);

// ---------------------------------------------------------------- custom shortcuts

struct CustomShortcut
{
    enum class Kind { none = 0, menuCommand = 1, pythonScript = 2, keyPress = 3, action = 4 };
    Kind kind = Kind::none;
    /** menuCommand: the menu path ("Effects > Normalize"); pythonScript: the
        script's full path; keyPress: the canonical key to send ("cmd+h");
        action: the name of a recorded action (Actions panel). */
    juce::String target;
    juce::String label;   // short text on the button (used when there is no icon)
    juce::String icon;    // iconId() of a library icon, "" = the label
    juce::String key;     // the shortcut's own key (canonical), "" = none
    juce::String tooltip; // the user's tooltip text, "" = what it does (issue #50)

    bool defined() const noexcept { return kind != Kind::none && target.isNotEmpty(); }
    /** "Effects > Normalize", "Python script denoise.py", "keys Ctrl+H",
        "action Clean up". */
    juce::String describe() const;
    bool operator== (const CustomShortcut& o) const
    {
        return kind == o.kind && target == o.target && label == o.label && icon == o.icon && key == o.key
               && tooltip == o.tooltip;
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

/** The icon row of the custom button form (issue #50): "Text" (no icon)
    then every library icon, on one line like the toolbar. The selected one
    has a white border; icons other custom buttons use are drawn darker, and
    can still be picked. */
class IconPicker : public juce::Component, public juce::SettableTooltipClient
{
public:
    IconPicker();
    juce::String selected;          // iconId(), "" = the text
    juce::StringArray usedElsewhere; // iconId()s of the other custom buttons
    std::function<void()> onChange;
    /** The cells: "" (text) then the library's iconId()s. */
    static juce::StringArray cellIds();
    juce::Rectangle<int> cellBounds (int i) const;
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    static constexpr int kCell = 24;
};

// ---------------------------------------------------------------- calculator

namespace calc
{
/** What the one-line calculator shows for `text` (issue #50).
    - Operators ( ) + - * / ^ and log (base 10); '.' is the decimal point.
    - A comma makes a list, and the expression repeats for each value:
      "(1,2,3)+1" gives 2, 3, 4. Two lists combine element by element.
    - The input is completed rather than refused: a missing operand of * /
      or ^ is 1 ("1/" = 1), of + - is 0 ("1+" = 1); unmatched parentheses
      close at the end or open at the start ("(1+2/2" = 2); "2(3)" is 2*3.
    - Division by zero, log of a value <= 0, lists of different lengths or
      a character it does not know give an error.
    Empty input: `empty`, no value. */
struct Result
{
    bool empty = true;
    bool ok = false;
    std::vector<double> values;
    juce::String text;   // "2", "2, 3, 4", "Error", "" when empty
};
Result evaluate (const juce::String& text);
/** A value as the calculator shows it: up to 10 significant digits. */
juce::String format (double v);
} // namespace calc

/** The calculator field right of the toolbar: type, the result follows. Enter
    keeps the result as the new expression, Esc clears it. */
class Calculator : public juce::Component, public juce::SettableTooltipClient
{
public:
    Calculator();
    juce::TextEditor& field() noexcept { return field_; }
    /** The shown result ("= 2", "= Error", "" when empty). */
    juce::String resultText() const { return result_; }
    void setExpression (const juce::String& text);   // as if typed
    /** Enter: the result becomes the expression (when there is one). */
    void commit();
    /** A right-click beside the field: the toolbar menu. */
    std::function<void()> onContextMenu;
    void resized() override;
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    static constexpr int kWidth = 210;

private:
    juce::TextEditor field_;
    juce::String result_;
    bool error_ = false;
    void update();
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
    /** A right-click on a button (its id) or on the toolbar between them
        (""): MainWindow shows the toolbar menu (issue #50). */
    std::function<void (const juce::String& id)> onContextMenu;
    /** Asked by refresh(): is the button's tool on / is it usable / which
        keyboard shortcut does the same ("" = none). */
    std::function<bool (const juce::String& id)> isOn, isEnabled;
    std::function<juce::String (const juce::String& id)> shortcutFor;

    void setCustom (const CustomSet& set);
    const CustomSet& custom() const noexcept { return custom_; }
    /** The first custom slot with nothing in it (0-based), -1 when all nine
        are used: where Add Custom Button goes. */
    int firstFreeCustomSlot() const;
    /** The predetermined buttons the user hid (Manage Visible Buttons). */
    void setHiddenIds (const juce::StringArray& ids);
    const juce::StringArray& hiddenIds() const noexcept { return hidden_; }
    /** A button shows when it is not hidden and, for a custom slot, defined. */
    bool isShown (const juce::String& id) const;
    /** The name of a predetermined button ("Lasso"), "" for another id. */
    static juce::String nameOf (const juce::String& id);
    static Icon iconOf (const juce::String& id);
    /** Re-reads the state of every button (tools on, enabled, tooltips). */
    void refresh();

    /** Width that shows every group. */
    int idealWidth() const;
    void resized() override;
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;

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
    juce::StringArray hidden_;
    int numGroups() const;
};

/** The Manage Visible Buttons list (issue #50): a check box per
    predetermined button and for the calculator, then the custom buttons with
    Edit and Remove, and Add Custom Button / Show All at the bottom. The
    owner (MainWindow) applies each change at once; rebuild() re-reads. */
class ManageButtons : public juce::Component
{
public:
    explicit ManageButtons (const Toolbar& bar);
    std::function<bool (const juce::String& id)> isShown;            // "calculator" included
    std::function<void (const juce::String& id, bool shown)> onShow;
    std::function<void (int slot)> onEdit, onRemove;
    std::function<void()> onAdd, onShowAll;
    void rebuild();
    void resized() override;
    /** For the tests: the check box of a predetermined id, the Edit / Remove
        buttons of a slot, the bottom buttons. */
    juce::ToggleButton* toggleFor (const juce::String& id) const;
    juce::Button* editButton (int slot) const;
    juce::Button* removeButton (int slot) const;
    juce::Button* addButton() const { return add_.get(); }
    juce::Button* showAllButton() const { return showAll_.get(); }

private:
    const Toolbar& bar_;
    struct Row
    {
        juce::String id;   // a predetermined id, "calculator", or "custom.N"
        std::unique_ptr<juce::Component> icon;
        std::unique_ptr<juce::ToggleButton> toggle;
        std::unique_ptr<juce::Label> label;
        std::unique_ptr<juce::TextButton> edit, remove;
    };
    juce::Viewport view_;
    juce::Component content_;
    std::unique_ptr<juce::Label> customHeader_;
    std::vector<std::unique_ptr<Row>> rows_;
    std::unique_ptr<juce::TextButton> add_, showAll_;
    void layoutContent();
};
} // namespace sc::toolbar
