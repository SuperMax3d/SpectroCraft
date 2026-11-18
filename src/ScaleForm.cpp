/* The Scale form: three columns (Time / Amplitude / Frequency), one block per
   axis of that kind, its settings stacked under its name, a New button at the
   bottom of the column, and reordering by dragging a name.

   The model behind it is sc::axes (include/AxisScale.h); the form only ever
   writes it back through Application::setAxisEntries, which records one undo
   step and re-applies it to the tracks. */
#include "ScaleForm.h"
#include "Application.h"
#include "AxisScale.h"
#include "Panels.h"          // EyeButton: the same visibility icon as the tables

#include <algorithm>

namespace {

constexpr int kHeadH     = 26;    // column title + the rule under it
constexpr int kRowH      = 24;    // one field row inside a block
constexpr int kBlockPad  = 6;     // padding inside a block
constexpr int kBlockGap  = 10;    // between two blocks of a column
constexpr int kColGap    = 10;    // between two columns
constexpr int kMargin    = 8;     // around the whole form
constexpr int kTopPad    = 6;
constexpr int kNewBtnH   = 24;
constexpr int kBottomBarH = 36;   // Close / Cancel at the bottom of the form
constexpr int kBottomBtnW = 80;
constexpr int kLabelW    = 52;    // "Units", "Offset"... column inside a block
constexpr int kGripW     = 16;    // the drag grip left of a name
constexpr int kEyeW      = 24;    // the visibility eye (same icon as the tables)
constexpr int kDelW      = 24;    // the remove button: the same size as the eye
constexpr int kBtnGap    = 4;     // eye and remove sit together, not apart
constexpr int kRightPad  = 4;     // every row ends here: fields and buttons alike
constexpr int kPickW     = 24;    // a crosshair button next to a field
constexpr int kSameW     = 52;    // the "SAME" button next to a Scale field

constexpr int kDragThreshold = 6; // pixels of vertical movement before a drag

/* How many rows a block of that kind has: the name, then its settings. Only a
   time axis has an offset; amplitude and frequency are multipliers. */
int fieldRowsOf (sc::axes::Kind k)
{
    return k == sc::axes::Kind::Time ? 4 : 3;   // units, [offset], scale, type
}

int blockHeightFor (sc::axes::Kind k)
{
    return kBlockPad * 2 + (1 + fieldRowsOf (k)) * kRowH;
}

} // namespace

// ------------------------------------------------------------------ name field
/* A text field you can also drag. A press still puts the caret in the text; a
   drag only becomes a reorder when it clearly goes up/down (more vertically
   than horizontally, past a few pixels), so selecting text inside the field
   keeps working. */
struct ScaleFormWindow::NameField : public juce::TextEditor
{
    NameField() { setMultiLine (false); }

    std::function<void (const juce::MouseEvent&)> onDragMoved;
    std::function<void (const juce::MouseEvent&)> onDragEnded;

    void mouseDown (const juce::MouseEvent& e) override
    {
        TextEditor::mouseDown (e);
        startY = e.y;
        dragging = false;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging)
        {
            const int dy = e.y - startY, dx = e.x - startX;
            if (std::abs (dy) > kDragThreshold && std::abs (dy) > std::abs (dx))
            {
                dragging = true;
                if (onDragMoved) onDragMoved (e);
            }
            return;   // the base class only selects text; we suppress that here
        }
        if (onDragMoved) onDragMoved (e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragging)
        {
            dragging = false;
            if (onDragEnded) onDragEnded (e);
            return;
        }
        TextEditor::mouseUp (e);
    }

    bool isDragging() const noexcept { return dragging; }

private:
    int startY = 0, startX = 0;
    bool dragging = false;
};

/* The six dots left of a name: a real widget, so you can grab it and drag the
   axis up or down inside its column. */
struct ScaleFormWindow::Grip : public juce::Component
{
    explicit Grip (ScaleFormWindow& o) : owner (o) {}

    std::function<void (const juce::MouseEvent&)> onDragMoved;
    std::function<void (const juce::MouseEvent&)> onDragEnded;

    void paint (juce::Graphics& g) override
    {
        g.setColour (dragging ? juce::Colours::white
                              : juce::Colour::fromRGB (150, 150, 162));
        const float cx = getWidth() * 0.5f - 2.5f;
        const float cy = getHeight() * 0.5f;
        for (int row = -1; row <= 1; ++row)
            for (int col = 0; col < 2; ++col)
                g.fillEllipse (cx + col * 5.0f, cy + row * 5.0f - 1.5f, 3.0f, 3.0f);
    }

    juce::MouseCursor getMouseCursor() override
    {
        return juce::MouseCursor::UpDownResizeCursor;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = true;
        repaint();
        if (onDragMoved) onDragMoved (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging && onDragMoved) onDragMoved (e);
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! dragging) return;
        dragging = false;
        repaint();
        if (onDragEnded) onDragEnded (e);
    }

    bool dragging = false;
    ScaleFormWindow& owner;
};

// ----------------------------------------------------------------------- block
/* A crosshair button: it asks the app to take a position from the track (the
   playhead, a selection, a clicked reference) instead of typing a number. */
struct ScaleFormWindow::Crosshair : public juce::Button
{
    explicit Crosshair (const juce::String& nameIn) : Button (nameIn) {}

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto r = getLocalBounds().toFloat();
        g.setColour (down ? juce::Colour::fromRGB (70, 78, 96)
                          : over ? juce::Colour::fromRGB (52, 52, 58)
                                 : juce::Colour::fromRGB (40, 40, 45));
        g.fillRoundedRectangle (r.reduced (1.0f), 3.0f);

        const float cx = r.getCentre().x, cy = r.getCentre().y;
        const float rad = std::min (r.getWidth(), r.getHeight()) * 0.28f;
        g.setColour (over ? juce::Colours::white : juce::Colour::fromRGB (190, 190, 196));
        g.drawEllipse (juce::Rectangle<float> (cx - rad, cy - rad, rad * 2.0f, rad * 2.0f), 1.2f);
        // The cross reaches past the circle, like a sight.
        const float arm = rad + 3.0f;
        g.drawLine (cx - arm, cy, cx - rad + 1.0f, cy, 1.2f);
        g.drawLine (cx + rad - 1.0f, cy, cx + arm, cy, 1.2f);
        g.drawLine (cx, cy - arm, cx, cy - rad + 1.0f, 1.2f);
        g.drawLine (cx, cy + rad - 1.0f, cx, cy + arm, 1.2f);
    }
};

struct ScaleFormWindow::Block : public juce::Component
{
    Block (ScaleFormWindow& o, int entryIndexIn, sc::axes::Kind k)
        : owner (o), entryIndex (entryIndexIn), kind (k)
    {
        // Typing commits live without rebuilding the blocks: a rebuild
        // destroys the field being typed in, which lost the focus after
        // every character (issue #54).
        name.onTextChange = [this] { owner.commit (false); };
        name.onDragMoved = [this] (const juce::MouseEvent& e)
            { owner.nameDragMoved (this, e.getScreenPosition()); };
        name.onDragEnded = [this] (const juce::MouseEvent& e)
            { owner.nameDragEnded (this, e.getScreenPosition()); };

        units.onChange = [this]
        {
            // Renaming to the unit's built-in name keeps the name in step with
            // the unit, which is what the default names look like.
            const auto& list = sc::axes::unitNames (kind);
            const int u = std::max (0, units.getSelectedId() - 1);
            if (name.getText().trim() == sc::axes::defaultName (kind, previousUnits))
                name.setText (sc::axes::defaultName (kind, u), false);
            previousUnits = u;
            owner.commit();
        };
        offset.onTextChange = [this] { owner.commit (false); };
        scale.onTextChange  = [this] { owner.commit (false); };
        type.addItem (sc::axes::typeName (sc::axes::Type::AddAxis), 1);
        type.addItem (sc::axes::typeName (sc::axes::Type::FooterOnly), 2);
        type.onChange = [this] { owner.commit(); };

        eye.onClick = [this]
        {
            eye.shown = ! eye.shown;
            eye.setTooltip (eye.shown ? "Hide Axis" : "Show Axis");
            owner.commit();
        };
        eye.setTooltip ("Hide Axis");   // setFrom refines it per axis
        grip.onDragMoved = [this] (const juce::MouseEvent& e)
            { owner.nameDragMoved (this, e.getScreenPosition()); };
        grip.onDragEnded = [this] (const juce::MouseEvent& e)
            { owner.nameDragEnded (this, e.getScreenPosition()); };
        del.setButtonText ("x");
        del.setTooltip ("Remove this axis from the file");
        del.onClick = [this] { owner.removeAxis (entryIndex); };
        same.setButtonText ("SAME");
        same.setTooltip ("Take the units, offset and scale of this column's main axis");
        same.onClick = [this] { owner.copyFromMain (entryIndex); };

        /* Crosshair buttons: take the offset / scale from the track instead of
           typing it. The form asks MainWindow to arm the click, prompt for the
           value and do the arithmetic - it owns the views and the prompts. */
        if (kind == sc::axes::Kind::Time)
        {
            offsetPick.setTooltip ("Set offset based on Playhead");
            offsetPick.onClick = [this]
            { owner.onPickRequest (entryIndex, ScaleFormWindow::PickAction::OffsetFromPlayhead); };
            addAndMakeVisible (offsetPick);
        }
        scalePick.setTooltip (kind == sc::axes::Kind::Time
                                ? "Set Offset and Scale using selection"
                                : "Click a reference point in the track, then set its value");
        scalePick.onClick = [this]
        {
            owner.onPickRequest (entryIndex,
                                 kind == sc::axes::Kind::Time
                                     ? ScaleFormWindow::PickAction::TimeScaleFromSelection
                                     : ScaleFormWindow::PickAction::Reference);
        };
        addAndMakeVisible (scalePick);

        addAndMakeVisible (name);
        addAndMakeVisible (grip);
        addAndMakeVisible (eye);
        addAndMakeVisible (del);
        addAndMakeVisible (units);
        addAndMakeVisible (type);
        if (kind == sc::axes::Kind::Time) addAndMakeVisible (offset);
        addAndMakeVisible (scale);

        for (auto* lab : { &unitsLabel, &offsetLabel, &scaleLabel, &typeLabel })
        {
            lab->setFont (juce::Font { juce::FontOptions (10.5f) });
            lab->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
            lab->setInterceptsMouseClicks (false, false);
        }
        unitsLabel.setText ("Units", juce::dontSendNotification);
        offsetLabel.setText ("Offset", juce::dontSendNotification);
        scaleLabel.setText ("Scale", juce::dontSendNotification);
        typeLabel.setText ("Type", juce::dontSendNotification);
        addAndMakeVisible (unitsLabel);
        addAndMakeVisible (scaleLabel);
        addAndMakeVisible (typeLabel);
        if (kind == sc::axes::Kind::Time) addAndMakeVisible (offsetLabel);
    }

    void setFrom (const sc::axes::Entry& e)
    {
        const auto& list = sc::axes::unitNames (kind);
        units.clear();
        for (int i = 0; i < list.size(); ++i)
            units.addItem (list[i], i + 1);
        units.setSelectedId (std::clamp (e.units, 0, list.size() - 1) + 1, false);
        previousUnits = std::clamp (e.units, 0, list.size() - 1);

        name.setText (e.name, false);
        offset.setText (juce::String (e.offset, 6), false);
        scale.setText (juce::String (e.scale, 6), false);
        type.setSelectedId (e.type == sc::axes::Type::FooterOnly ? 2 : 1, false);
        eye.shown = e.visible;
        eye.setTooltip (e.visible ? "Hide Axis" : "Show Axis");
        eye.repaint();
        // The column's main axis has nothing to copy from: no SAME button. A
        // column with a single axis has nothing to remove either.
        same.setVisible (! isMain);
        del.setVisible (! onlyOneInColumn);
        del.setTooltip (e.type == sc::axes::Type::FooterOnly
                            ? "Remove this footer readout" : "Remove this axis");
        name.setTooltip ("Drag up or down to move this axis in the column");
    }

    sc::axes::Entry read (const sc::axes::Entry& previous) const
    {
        sc::axes::Entry e = previous;
        e.name    = name.getText().trim();
        e.units   = std::max (0, units.getSelectedId() - 1);
        e.offset  = offset.getText().trim().getDoubleValue();
        const double s = scale.getText().trim().getDoubleValue();
        e.scale   = (std::abs (s) < 1e-12 ? 1.0 : s);
        e.type    = (type.getSelectedId() == 2) ? sc::axes::Type::FooterOnly
                                                : sc::axes::Type::AddAxis;
        e.visible = eye.shown;
        if (e.name.isEmpty()) e.name = sc::axes::defaultName (e.kind, e.units);
        return e;
    }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colour::fromRGB (34, 34, 38));
        g.fillRoundedRectangle (r.reduced (1.0f), 3.0f);
        g.setColour (juce::Colour::fromRGB (62, 62, 70));
        g.drawRoundedRectangle (r.reduced (1.0f), 3.0f, 1.0f);

    }

    void resized() override
    {
        auto r = getLocalBounds();
        r.reduce (kBlockPad, kBlockPad);
        int y = r.getY();          // inside the padding, not at the top edge

        /* Name row: the grip, the name, then the eye and the remove button
           together at the right edge - same size, side by side, and ending at
           the very margin the text boxes below them end at. */
        const int rowRight = r.getRight() - kRightPad;
        const int delX = rowRight - kDelW;
        const int eyeX = delX - kBtnGap - kEyeW;
        const int nameW = std::max (50, eyeX - (kGripW + 2) - 6);
        grip.setBounds (0, y, kGripW, kRowH - 4);
        name.setBounds (kGripW + 2, y, nameW, kRowH - 4);
        eye.setBounds  (eyeX, y, kEyeW, kRowH - 4);
        del.setBounds  (delX, y, kDelW, kRowH - 4);
        y += kRowH;

        /* A field row reserves, on its right, the buttons that belong to it: the
           crosshair, plus SAME on a non-main Scale row. The field stops where
           its buttons start, and the last button stops at the same margin as a
           row without buttons - so nothing of a block hangs over the column. */
        auto fieldRow = [&] (juce::Label& lab, juce::Component& field, int fieldRight)
        {
            lab.setBounds (kGripW, y + 4, kLabelW - kGripW, kRowH - 8);
            const int x = kLabelW + 2;
            // fieldRight is the row's own right limit: the shared margin for a
            // plain row, the first button's edge (minus a gap) for the others.
            field.setBounds (x, y, std::max (40, fieldRight - x), kRowH - 4);
            y += kRowH;
        };

        fieldRow (unitsLabel, units, rowRight);
        if (kind == sc::axes::Kind::Time)
        {
            const int pickX = rowRight - kPickW;
            fieldRow (offsetLabel, offset, pickX - 4);   // a gap before the button
            offsetPick.setBounds (pickX, y - kRowH, kPickW, kRowH - 4);
        }
        {
            const int pickX = rowRight - kPickW;
            const int fieldRight = isMain ? pickX - 4
                                          : (pickX - kBtnGap - kSameW) - 4;
            fieldRow (scaleLabel, scale, fieldRight);
            scalePick.setBounds (pickX, y - kRowH, kPickW, kRowH - 4);
            if (! isMain)
                same.setBounds (pickX - kBtnGap - kSameW, y - kRowH, kSameW, kRowH - 4);
        }
        fieldRow (typeLabel, type, rowRight);
    }

    ScaleFormWindow& owner;
    int entryIndex;
    sc::axes::Kind kind;
    int previousUnits = 0;
    bool isMain = false;
    bool onlyOneInColumn = false;

    NameField name;
    Grip grip { owner };
    EyeButton eye { "Shown" };
    juce::TextButton del, same;
    Crosshair offsetPick { "OffsetFromPlayhead" }, scalePick { "ScaleFromTrack" };
    juce::ComboBox units, type;
    juce::TextEditor offset { "Offset" }, scale { "Scale" };
    juce::Label unitsLabel, offsetLabel, scaleLabel, typeLabel;
};

// ------------------------------------------------------------------- the body
/* The content component: it paints the three column titles and their rules,
   plus the line that shows where a dragged name would land. */
struct ScaleFormWindow::FormBody : public juce::Component
{
    explicit FormBody (ScaleFormWindow& f) : form (f)
    {
        for (int i = 0; i < 3; ++i)
        {
            newBtn[i].setButtonText ("New");
            newBtn[i].setTooltip (juce::String ("Add a ")
                                  + sc::axes::kindName ((sc::axes::Kind) i)
                                  + " axis to this file");
            newBtn[i].onClick = [this, i] { form.addAxis ((sc::axes::Kind) i); };
            addAndMakeVisible (newBtn[i]);
        }

        // Every change already applies live: Close only closes; Cancel puts
        // back the axes the file had when the form was opened (issue #54).
        closeBtn.setButtonText ("Close");
        closeBtn.setTooltip ("Close the form; the changes are already applied");
        closeBtn.onClick = [this] { form.closeForm(); };
        addAndMakeVisible (closeBtn);
        cancelBtn.setButtonText ("Cancel");
        cancelBtn.setTooltip ("Undo every change made since the form was opened, then close it");
        cancelBtn.onClick = [this] { form.cancelChanges(); };
        addAndMakeVisible (cancelBtn);
    }

    void paint (juce::Graphics& g) override
    {
        for (int col = 0; col < 3; ++col)
        {
            const auto c = form.columnRect (col);
            if (c.getWidth() <= 0) continue;

            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (12.5f, juce::Font::bold));
            g.drawText (sc::axes::kindName ((sc::axes::Kind) col),
                        c.getX() + 2, 2, c.getWidth() - 4, kHeadH - 8,
                        juce::Justification::centredLeft, false);

            g.setColour (juce::Colour::fromRGB (70, 70, 78));
            g.drawLine (c.getX(), (float) kHeadH - 2.0f,
                        (float) c.getRight(), (float) kHeadH - 2.0f);
        }

        // Where the dragged name would be inserted.
        if (form.dropIndex >= 0)
        {
            const auto c = form.columnRect ((int) form.dragKind);
            const int blocks = form.blocksInColumn (form.dragKind);
            const int h = blockHeightFor (form.dragKind);
            const int y = kTopPad + kHeadH + form.dropIndex * (h + kBlockGap) - kBlockGap / 2;
            if (blocks > 0 && y > kTopPad)
            {
                g.setColour (juce::Colour::fromRGB (0, 212, 170));
                g.drawLine ((float) c.getX(), (float) y, (float) c.getRight(), (float) y);
            }
        }
    }

    juce::TextButton newBtn[3];
    juce::TextButton closeBtn, cancelBtn;
    ScaleFormWindow& form;
};

// ---------------------------------------------------------------- construction
ScaleFormWindow::ScaleFormWindow (Application& appIn)
    : DocumentWindow ("Scale", juce::Colour (0xff232323),
                      DocumentWindow::allButtons),
      app (appIn)
{
    /* A normal OS window: a real title bar with a close button, and the app's
       dark window colour as background (the fields are ordinary JUCE widgets
       and take their colours from the shared LookAndFeel). */
    setUsingNativeTitleBar (true);
    body = new FormBody (*this);
    /* Owned by this window (deleted in the destructor), not by the peer: with
       "delete on exit" the peer frees it too and the window dies on close. */
    setContentNonOwned (body, false);
    setAlwaysOnTop (true);              // stays reachable while you click in the tracks
    setResizable (true, true);
    // 30 % narrower than it was (980): the three columns still show every
    // label and field in full; the narrowest field is a secondary axis' Scale,
    // next to its SAME and crosshair buttons. Minimise / maximise like any
    // window (issue #54).
    setResizeLimits (600, 260, 2400, 1600);

    centreWithSize (690, 400);
    rebuild();
}

ScaleFormWindow::~ScaleFormWindow()
{
    /* The body is a child component: take it out of the parent's child list
       before freeing it, or both the parent and this destructor delete it. */
    if (body != nullptr)
    {
        removeChildComponent (body);
        delete body;
        body = nullptr;
    }
}

// -------------------------------------------------------------------- rebuild
void ScaleFormWindow::rebuild()
{
    blocks.clear();

    const auto list = app.axisEntries;
    shown = list;
    // How many axes each column holds: the remove button only appears where
    // there is something to remove - a column's single axis is removed by
    // adding another one first.
    int perKind[3] = { 0, 0, 0 };
    for (const auto& e : list) ++perKind[(int) e.kind];

    for (int i = 0; i < (int) list.size(); ++i)
    {
        auto b = std::make_unique<Block> (*this, i, list[(size_t) i].kind);
        b->onlyOneInColumn = perKind[(int) list[(size_t) i].kind] <= 1;
        // The first "Add Axis" entry of a kind is that column's main axis.
        bool first = list[(size_t) i].isAddAxis();
        for (int j = 0; j < i; ++j)
            if (list[(size_t) j].kind == list[(size_t) i].kind
                && list[(size_t) j].isAddAxis()) { first = false; break; }
        b->isMain = first;
        b->setFrom (list[(size_t) i]);
        body->addAndMakeVisible (*b);
        blocks.push_back (std::move (b));
    }

    resized();

    // Grow the window so every column fits (never shrink it: the user may have
    // sized it on purpose).
    int need = kTopPad + kHeadH;
    for (int col = 0; col < 3; ++col)
    {
        int count = 0;
        for (const auto& e : list)
            if (e.kind == (sc::axes::Kind) col) ++count;
        need = std::max (need, kTopPad + kHeadH
                              + count * (blockHeightFor ((sc::axes::Kind) col) + kBlockGap)
                              + kNewBtnH + kMargin + kBottomBarH);
    }
    if (body->getHeight() < need)
        setSize (getWidth(), need + (getHeight() - body->getHeight()));
}

// --------------------------------------------------------------------- layout
void ScaleFormWindow::resized()
{
    /* The base class is what gives the content component its size; skipping it
       leaves the body at 0 x 0 and every block invisible. */
    juce::DocumentWindow::resized();

    // The base class can call resized() while the window is still being built,
    // before the body exists.
    if (body == nullptr) return;

    const auto b = body->getLocalBounds();
    const int colW = std::max (120, (b.getWidth() - 2 * kColGap - 2 * kMargin) / 3);

    layoutOrder.clear();
    for (int col = 0; col < 3; ++col)
    {
        int y = kTopPad + kHeadH;
        for (auto& blk : blocks)
        {
            if ((int) blk->kind != col) continue;
            blk->setBounds (kMargin + col * (colW + kColGap), y, colW,
                            blockHeightFor (blk->kind));
            y += blockHeightFor (blk->kind) + kBlockGap;
            layoutOrder.push_back (blk->entryIndex);
        }
        body->newBtn[col].setBounds (kMargin + col * (colW + kColGap) + 2, y,
                                     std::min (90, colW - 4), kNewBtnH);
    }

    // Close / Cancel, bottom right.
    const int by = b.getBottom() - kMargin - kNewBtnH;
    body->cancelBtn.setBounds (b.getRight() - kMargin - kBottomBtnW, by, kBottomBtnW, kNewBtnH);
    body->closeBtn.setBounds (body->cancelBtn.getX() - 8 - kBottomBtnW, by, kBottomBtnW, kNewBtnH);

    body->repaint();
}

void ScaleFormWindow::visibilityChanged()
{
    juce::DocumentWindow::visibilityChanged();
    // What Cancel goes back to: the file's axes when the form was opened.
    if (isVisible())
    {
        openedWith = app.axisEntries;
        openedDoc = (const void*) app.doc();
    }
}

void ScaleFormWindow::closeForm()
{
    setVisible (false);
}

void ScaleFormWindow::cancelChanges()
{
    // Only for the file the form was opened on (a document switch since then
    // leaves both files as they are), and never an empty list.
    if ((const void*) app.doc() == openedDoc && ! openedWith.empty()
        && app.axisEntries != openedWith)
    {
        app.setAxisEntries (openedWith, "Scale (cancelled)");
        shown = app.axisEntries;
        if (app.onStateChange) app.onStateChange ();
        rebuild();
    }
    setVisible (false);
}

juce::Rectangle<int> ScaleFormWindow::bottomButtonBounds (bool cancel) const
{
    if (body == nullptr) return {};
    return cancel ? body->cancelBtn.getBounds() : body->closeBtn.getBounds();
}

juce::Rectangle<int> ScaleFormWindow::columnRect (int columnIndex) const
{
    if (body == nullptr) return {};
    const auto b = body->getLocalBounds();
    const int colW = std::max (120, (b.getWidth() - 2 * kColGap - 2 * kMargin) / 3);
    int bottom = kTopPad + kHeadH;
    for (auto& blk : blocks)
        if ((int) blk->kind == columnIndex)
            bottom = std::max (bottom, blk->getBottom() + kBlockGap + kNewBtnH);
    return { kMargin + columnIndex * (colW + kColGap), kTopPad, colW,
             std::max (kHeadH, bottom - kTopPad) };
}

// ------------------------------------------------------------------ accessors
juce::Rectangle<int> ScaleFormWindow::contentBounds() const
{
    return body != nullptr ? body->getLocalBounds() : juce::Rectangle<int>();
}

int ScaleFormWindow::numRows() const
{
    return (int) blocks.size();
}

juce::Rectangle<int> ScaleFormWindow::rowBounds (int entryIndex) const
{
    for (auto& blk : blocks)
        if (blk->entryIndex == entryIndex) return blk->getBounds();
    return {};
}

int ScaleFormWindow::blocksInColumn (sc::axes::Kind kind) const
{
    int n = 0;
    for (auto& blk : blocks)
        if (blk->kind == kind) ++n;
    return n;
}

int ScaleFormWindow::columnIndexOf (int entryIndex) const
{
    for (auto& blk : blocks)
        if (blk->entryIndex == entryIndex) return (int) blk->kind;
    return -1;
}

// -------------------------------------------------------------------- commit
void ScaleFormWindow::commit (bool rebuildAfter)
{
    const auto before = app.axisEntries;
    sc::axes::List out = before;
    for (auto& blk : blocks)
    {
        if (blk->entryIndex < 0 || blk->entryIndex >= (int) out.size()) continue;
        out[(size_t) blk->entryIndex] = blk->read (out[(size_t) blk->entryIndex]);
    }
    if (out == before) return;

    // Live preview: the tracks and the footer update immediately, and the
    // change is one undoable step (History shows "Scale").
    app.setAxisEntries (out, "Scale");
    // The blocks already show what was just applied. Recording that before
    // the state callback runs keeps refreshFromApp() (called from it) from
    // rebuilding the form under the field being typed in.
    if (! rebuildAfter) shown = app.axisEntries;
    if (app.onStateChange) app.onStateChange ();
    if (rebuildAfter) rebuild();
}

void ScaleFormWindow::addAxis (sc::axes::Kind kind)
{
    auto list = app.axisEntries;
    const int n = sc::axes::countKind (list, kind) + 1;
    sc::axes::Entry e;
    e.kind  = kind;
    e.units = sc::axes::defaultUnits (kind);
    e.name  = sc::axes::defaultName (kind, e.units) + " " + juce::String (n);
    list.push_back (e);
    app.setAxisEntries (list, "Scale");
    if (app.onStateChange) app.onStateChange ();
    rebuild();
}

void ScaleFormWindow::removeAxis (int entryIndex)
{
    auto list = app.axisEntries;
    if (entryIndex < 0 || entryIndex >= (int) list.size()) return;
    // Removing the only axis of a kind is allowed: that kind then simply has
    // no axis (the tracks fall back to their uncalibrated plain).
    list.erase (list.begin() + entryIndex);
    app.setAxisEntries (list, "Scale");
    if (app.onStateChange) app.onStateChange ();
    rebuild();
}

/* Copy the column's main axis onto this one: same units, same offset, same
   scale. The button only appears on the blocks that are not the main one. */
void ScaleFormWindow::copyFromMain (int entryIndex)
{
    auto list = app.axisEntries;
    if (entryIndex < 0 || entryIndex >= (int) list.size()) return;
    const auto kind = list[(size_t) entryIndex].kind;
    const auto* main = sc::axes::primary (list, kind);
    if (main == nullptr) return;

    auto& e = list[(size_t) entryIndex];
    e.units  = main->units;
    e.offset = main->offset;
    e.scale  = main->scale;
    e.chanScale = main->chanScale;
    app.setAxisEntries (list, "Scale");
    if (app.onStateChange) app.onStateChange ();
    rebuild();
}

// ---------------------------------------------------------------- drag/reorder
void ScaleFormWindow::nameDragMoved (Block* block, const juce::Point<int>& screenPos)
{
    dragBlock = block;
    dragKind  = block->kind;
    const auto c = columnRect ((int) dragKind);
    dropIndex = dropIndexFor (dragKind, c.getX () == 0 ? 0
                        : body->getLocalPoint (nullptr, screenPos).getY());
    body->repaint();
}

void ScaleFormWindow::nameDragEnded (Block* block, const juce::Point<int>& screenPos)
{
    const auto c = columnRect ((int) block->kind);
    const int target = dropIndexFor (block->kind,
                                     body->getLocalPoint (nullptr, screenPos).getY());
    dragBlock = nullptr;
    dropIndex = -1;
    body->repaint();
    if (target >= 0) moveBlockInColumn (block->entryIndex, target);
}

/* Which position in the column the given y falls in. */
int ScaleFormWindow::dropIndexFor (sc::axes::Kind kind, int yInBody) const
{
    const int h = blockHeightFor (kind);
    const int first = kTopPad + kHeadH;
    if (yInBody < first) return 0;
    int pos = (yInBody - first) / (h + kBlockGap);
    return std::clamp (pos, 0, std::max (0, blocksInColumn (kind) - 1));
}

bool ScaleFormWindow::moveBlockInColumn (int entryIndex, int newPositionInColumn)
{
    auto list = app.axisEntries;
    if (entryIndex < 0 || entryIndex >= (int) list.size()) return false;
    const auto kind = list[(size_t) entryIndex].kind;

    // The entries of this column, in document order.
    std::vector<int> idx;
    for (int i = 0; i < (int) list.size(); ++i)
        if (list[(size_t) i].kind == kind) idx.push_back (i);

    const int from = (int) (std::find (idx.begin(), idx.end(), entryIndex) - idx.begin());
    if (from < 0 || from >= (int) idx.size()) return false;
    if (newPositionInColumn < 0 || newPositionInColumn >= (int) idx.size()) return false;
    if (from == newPositionInColumn) return false;

    sc::axes::List moved;
    for (int i : idx) moved.push_back (list[(size_t) i]);
    auto item = moved[(size_t) from];
    moved.erase (moved.begin() + from);
    moved.insert (moved.begin() + newPositionInColumn, item);
    for (size_t j = 0; j < idx.size(); ++j) list[(size_t) idx[j]] = moved[j];

    app.setAxisEntries (list, "Scale");
    if (app.onStateChange) app.onStateChange ();
    rebuild();
    return true;
}

/* Test hooks: press a block's own eye / remove button, exactly as a click
   does. Used to check that a control acts on its own axis. */
void ScaleFormWindow::clickEyeFor (int entryIndex)
{
    for (auto& blk : blocks)
        if (blk->entryIndex == entryIndex) { blk->eye.onClick(); return; }
}

ScaleFormWindow::Controls ScaleFormWindow::controlsFor (int entryIndex) const
{
    Controls c;
    for (auto& blk : blocks)
    {
        if (blk->entryIndex != entryIndex) continue;
        c.valid       = true;
        c.eye         = blk->eye.getBounds();
        c.del         = blk->del.getBounds();
        c.scaleField  = blk->scale.getBounds();
        c.scalePick   = blk->scalePick.getBounds();
        c.offsetField = blk->offset.getBounds();
        c.offsetPick  = blk->offsetPick.getBounds();
        c.unitsField  = blk->units.getBounds();
        c.typeField   = blk->type.getBounds();
        c.unitsField  = blk->units.getBounds();
        c.typeField   = blk->type.getBounds();
        break;
    }
    return c;
}

juce::String ScaleFormWindow::eyeTooltipFor (int entryIndex) const
{
    for (auto& blk : blocks)
        if (blk->entryIndex == entryIndex) return blk->eye.getTooltip();
    return {};
}

void ScaleFormWindow::clickRemoveFor (int entryIndex)
{
    for (auto& blk : blocks)
        if (blk->entryIndex == entryIndex && blk->del.isVisible()) { blk->del.onClick(); return; }
}

bool ScaleFormWindow::removeButtonVisibleFor (int entryIndex) const
{
    for (auto& blk : blocks)
        if (blk->entryIndex == entryIndex) return blk->del.isVisible();
    return false;
}

/* Re-read the document. A no-op when the blocks already mirror it: the form is
   refreshed from the shared-state callback, and refreshing while the user is
   typing would throw their edit away. */
void ScaleFormWindow::refreshFromApp()
{
    if (app.axisEntries == shown) return;
    rebuild();
}
