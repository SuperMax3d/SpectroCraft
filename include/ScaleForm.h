#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "AxisScale.h"

class Application;

/** The Scale form (track menu / panel header "Scale...").

    Three columns - Time, Amplitude, Frequency - each holding that kind's axes
    as a vertical stack: the name first, then its settings underneath (units,
    offset for time, scale, type), and a New button at the bottom of the
    column. Order inside a column is changed by dragging a name, not by
    buttons; the first block of a column is the main axis of that kind.

    Everything applies live: the tracks and the footer update as soon as a
    field commits, so the playhead and the clicked references keep reacting.
    Modeless and always-on-top on purpose - unlike the app's other dialogs it
    must stay open while you click in the tracks. */
class ScaleFormWindow : public juce::DocumentWindow
{
public:
    explicit ScaleFormWindow (Application& appIn);
    ~ScaleFormWindow() override;

    void closeButtonPressed() override { setVisible (false); }
    void resized() override;
    void visibilityChanged() override;

    /** The buttons at the bottom of the form. Close: changes apply live, so
        it only closes. Cancel: put back the axes the file had when the form
        was opened (one undoable step), then close. */
    void closeForm();
    void cancelChanges();
    /** Test hook: where Close (false) / Cancel (true) sit in the form body. */
    juce::Rectangle<int> bottomButtonBounds (bool cancel) const;

    /** Which kind a New button adds (the track the menu was opened from). */
    void setKindForNewAxis (sc::axes::Kind k) { newKind = k; }

    /** Re-read the document's entries (after a doc switch or an undo). */
    void refreshFromApp();

    /* What the tests check: the blocks really are laid out, in three columns.
       A DocumentWindow whose resized() skips the base class leaves its content
       component at 0 x 0 and the form shows an empty frame. */
    int numRows() const;                                   // blocks in total
    juce::Rectangle<int> contentBounds() const;             // the form's body area
    juce::Rectangle<int> rowBounds (int entryIndex) const; // bounds of one block
    int blocksInColumn (sc::axes::Kind kind) const;
    int columnIndexOf (int entryIndex) const;              // -1 when unknown
    /** Move a block inside its own column (what dragging a name does). */
    bool moveBlockInColumn (int entryIndex, int newPositionInColumn);

    /** What a crosshair button asks for. The form has no views and no prompts
        of its own: MainWindow arms the click in the track, prompts for the
        value, works out the offset / scale and refreshes the form. */
    enum class PickAction : int
    {
        OffsetFromPlayhead = 0,     // offset so the playhead shows a given time
        TimeScaleFromSelection = 1, // offset + scale from a selection's ends
        Reference = 2,              // click a reference, then its value -> scale
    };
    std::function<void (int entryIndex, PickAction)> onPickRequest;

    /** Test hooks: press one block's eye / remove button, and ask whether the
        remove button is shown for it. */
    void clickEyeFor (int entryIndex);
    void clickRemoveFor (int entryIndex);
    bool removeButtonVisibleFor (int entryIndex) const;

    /* Test hooks: where one block's controls ended up. The rules they have to
       satisfy: the eye and the remove button are the same size and sit together,
       neither reaches past the right edge of the fields below them, and the
       crosshair buttons sit inside the row they belong to. */
    struct Controls
    {
        juce::Rectangle<int> eye, del, scaleField, scalePick, offsetField, offsetPick;
        juce::Rectangle<int> unitsField, typeField;   // the rows without buttons
        bool valid = false;
    };
    Controls controlsFor (int entryIndex) const;
    juce::String eyeTooltipFor (int entryIndex) const;

private:
    struct FormBody;
    struct NameField;
    struct Grip;
    struct Crosshair;
    struct Block;

    void rebuild();
    /** Applies the blocks' values to the document (one undoable step).
        rebuildAfter = false for typing in a text field: the blocks are kept,
        so the field keeps the keyboard focus. */
    void commit (bool rebuildAfter = true);
    void addAxis (sc::axes::Kind kind);
    void removeAxis (int entryIndex);
    void copyFromMain (int entryIndex);

    // drag a name to reorder it inside its column
    void nameDragMoved (Block* block, const juce::Point<int>& posInBody);
    void nameDragEnded (Block* block, const juce::Point<int>& posInBody);
    int dropIndexFor (sc::axes::Kind kind, int yInBody) const;
    juce::Rectangle<int> columnRect (int columnIndex) const;

    Application& app;
    sc::axes::Kind newKind = sc::axes::Kind::Time;

    FormBody* body = nullptr;                 // paints the columns, holds the blocks
    std::vector<std::unique_ptr<Block>> blocks;   // one per entry, in entry order
    std::vector<int> columnOfKind;            // kind -> column index (0..2)
    std::vector<int> layoutOrder;             // entry indexes, in the order drawn
    sc::axes::List shown;                     // what the blocks currently mirror
    sc::axes::List openedWith;                // the file's axes when the form was opened (Cancel)
    const void* openedDoc = nullptr;          // the document they belong to

    // drag state
    Block* dragBlock = nullptr;
    sc::axes::Kind dragKind = sc::axes::Kind::Time;
    int dropIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScaleFormWindow)
};
