#pragma once
/**
 * SpectroCraft - Adobe-style docking workspace.
 *
 * Every panel (the Editor included) lives in a PANEL GROUP: a frame with a
 * strip of text tabs. Groups are arranged in a tree of horizontal / vertical
 * splits inside a DOCK AREA: the main window has one, and every floating panel
 * window has its own. Users rearrange the workspace with the mouse:
 *
 *  - drag a tab (or the empty part of a tab strip, to move the whole group)
 *    onto another group: a drop overlay splits it into a centre rectangle
 *    (add as a tab) and four trapezoids (dock left / right / top / bottom);
 *    the dragged panel follows the cursor as a translucent ghost that snaps
 *    ("magnet") onto the zone under the cursor;
 *  - drop it on the strip of tabs to insert it at that position, or on the
 *    thin band along the edge of a dock area to dock it along that edge;
 *  - drop it outside any dock area (or hold Ctrl/Cmd) to UNDOCK it into a
 *    floating window; floating windows accept other panels in turn;
 *  - Esc cancels a drag; the gaps between groups resize them;
 *  - right-click a tab or click the panel menu icon next to the active tab for
 *    Close Panel / Undock Panel / Close Other Panels in Group / panel group
 *    commands; double-click a tab (or press `) to maximise its group.
 *
 * The whole arrangement (main area, floating windows, closed panels) is a JSON
 * value (saveLayout / loadLayout). Workspaces stores named layouts with a
 * saved and a current version, like Adobe's Window > Workspaces.
 *
 * Panel content components are NOT owned here: the application owns them and
 * registers them with DockManager::registerPanel().
 */

#include <JuceHeader.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace sc::dock
{

//==============================================================================
// Look and feel

namespace style
{
    /** Source Sans 3 (Adobe's open-source UI family, SIL OFL 1.1; see
        third_party/licenses/SourceSans3-OFL.txt), embedded in the binary. */
    juce::Typeface::Ptr regularTypeface();
    juce::Typeface::Ptr semiboldTypeface();
    juce::Font uiFont (float height, bool semibold = false);

    constexpr int kTabStripH   = 30;   // tab strip at the top of every panel group
    /** Tab titles, the workspace bar and the dragged panel's title. A JUCE
        height (ascent + descent): a 12.1 px body in Source Sans 3, the size
        of the popup menus. The strip keeps 7 px above and below the text. */
    constexpr float kTabTextH  = 16.0f;
    constexpr int kGap         = 4;    // dark gap between groups (also a resizer)
    constexpr int kGapGrab     = 4;    // ... grabbed this far into each neighbour too (12 px in all)
    constexpr int kOuterMargin = 4;    // margin around a dock area
    constexpr int kMinGroupW   = 120;
    constexpr int kMinGroupH   = 80;
    constexpr int kAreaEdgeBand = 12;  // drop band along a dock area's left / right / bottom edge
    constexpr int kAreaTopBand  = 7;   // ... and its top edge (over the top groups' tab strips)
    constexpr int kTitleStripH = 16;   // drag strip at the top of a floating window
    constexpr int kWindowFrame = 4;    // frame of a floating window around its content
    constexpr int kWindowGrab  = 8;    // its resize zone: the frame plus the content's outer band
    constexpr float kCorner    = 4.0f;

    const juce::Colour appBackground   { 0xff141414 };
    const juce::Colour groupBackground { 0xff262626 };
    const juce::Colour contentBorder   { 0xff1d1d1d };
    const juce::Colour tabText         { 0xff9b9b9b };
    const juce::Colour tabTextHover    { 0xffd2d2d2 };
    const juce::Colour tabTextActive   { 0xfff0f0f0 };
    const juce::Colour focusOutline    { 0xff2d8ceb };
    const juce::Colour dropBase        { 0xf2c4c6c4 };
    const juce::Colour dropTrapezoid   { 0xf2aaaaaa };
    const juce::Colour dropHighlight   { 0xf53c3c96 };
    const juce::Colour dropLine        { 0xf0505050 };
    const juce::Colour titleStrip      { 0xff1b1b1b };
    const juce::Colour windowBorder    { 0xff3c3c3c };
}

/** LookAndFeel_V4 with neutral dark greys, dark popup menus and table
    headers, and the Adobe-like UI font (Source Sans 3) in the menu bar and
    popup menus. Installed as the application's default look and feel.
    The rest of the UI keeps the system font: small text (12 px labels) in
    Source Sans renders too thin on Windows and Linux, which do not darken
    glyphs the way macOS does. The docking chrome (tab strips, workspace bar,
    floating title strips) uses style::uiFont() directly. */
class DockLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DockLookAndFeel();
    juce::Font getPopupMenuFont() override;
    juce::Font getMenuBarFont (juce::MenuBarComponent&, int itemIndex, const juce::String& itemText) override;
};

//==============================================================================
// Model

/** A registered panel. The content component stays owned by the caller. */
struct PanelDesc
{
    juce::String id;
    juce::String title;
    juce::Component* content = nullptr;
    bool closable = true;
    int preferredWidth  = 320;   // size when docked on a side of a group
    int preferredHeight = 240;
};

/** Where a dragged panel lands. */
enum class Zone
{
    none,
    centre,                                     // add as a tab of the target group
    left, right, top, bottom,                   // split the target group
    tabStrip,                                   // insert among the target's tabs
    areaLeft, areaRight, areaTop, areaBottom,   // along the dock area's edge
    floating                                    // undock into a new window
};

/** A node of a dock area's tree: either a group of tabs or a split. */
struct Node
{
    enum class Kind { group, split };

    Kind kind = Kind::group;
    int uid = 0;
    Node* parent = nullptr;

    // group
    juce::StringArray tabs;      // panel ids, in tab order
    int active = 0;

    // split: children side by side (horizontal) or stacked (vertical)
    bool horizontal = true;
    std::vector<std::unique_ptr<Node>> children;
    std::vector<double> weights; // one per child, summing to 1

    bool isGroup() const noexcept { return kind == Kind::group; }
    juce::String activeTab() const { return juce::isPositiveAndBelow (active, tabs.size()) ? tabs[active] : juce::String(); }
};

class DockManager;
class DockArea;
class GroupComponent;
class FloatingWindow;

//==============================================================================
/** Draws the drop zones of the group (or area edge) under a dragged panel. */
class DropOverlay : public juce::Component, private juce::Timer
{
public:
    DropOverlay();
    /** zone is a group zone (target = the group's content rectangle in area
        coordinates; strip = its tab strip, caretX = tab insertion caret) or an
        area-edge zone (target = the highlighted band). */
    void show (Zone zone, juce::Rectangle<int> target, juce::Rectangle<int> strip = {}, int caretX = -1);
    void clear();

    Zone shownZone() const noexcept { return zone_; }
    juce::Rectangle<int> shownTarget() const noexcept { return target_; }

    /** The centre rectangle and the four trapezoids of a group's content
        rectangle (shared by hit testing and painting). */
    static juce::Rectangle<int> innerRect (juce::Rectangle<int> content);
    static Zone zoneAt (juce::Rectangle<int> content, juce::Point<int> p);
    static juce::Path zonePath (juce::Rectangle<int> content, Zone z);

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    Zone zone_ = Zone::none;
    juce::Rectangle<int> target_, strip_;
    int caretX_ = -1;
    float fade_ = 0.0f;
};

//==============================================================================
/** The tab strip + frame of one panel group; shows the active panel's content. */
class GroupComponent : public juce::Component,
                       public juce::TooltipClient,
                       private juce::Timer
{
public:
    GroupComponent (DockManager&, DockArea&, int uid);
    ~GroupComponent() override;

    int getUid() const noexcept { return uid_; }
    Node* node() const;
    DockArea& area() const noexcept { return area_; }

    /** Re-reads the node: attaches the active content, lays out the tabs. */
    void refresh();

    juce::Rectangle<int> stripBounds() const;
    juce::Rectangle<int> contentBounds() const;
    /** Tab under a point (local coordinates), -1 for none. */
    int tabIndexAt (juce::Point<int> p) const;
    /** Insertion index for a drop at local x on the strip (0..numTabs). */
    int insertIndexAt (int x) const;
    /** x of the insertion caret for an index. */
    int caretXFor (int insertIndex) const;
    /** Local bounds of tab i (empty when hidden in the overflow menu). */
    juce::Rectangle<int> tabBounds (int i) const;
    juce::Rectangle<int> menuIconBounds() const noexcept { return menuIcon_; }
    bool hasOverflow() const noexcept { return overflow_; }

    void showPanelMenu (int tabIndex, std::optional<juce::Point<int>> screenPos = {});

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    /** False along a gap beside the group: that band resizes the gap, so
        the dock area receives it (the panel content gives up a few pixels). */
    bool hitTest (int x, int y) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    /** Tooltip of the tab strip part under the mouse (tab, menu icon, overflow). */
    juce::String getTooltip() override;

private:
    struct TabSlot { int index; juce::Rectangle<int> r; };

    void layoutTabs();
    void showOverflowMenu();
    void timerCallback() override;   // click-and-hold: grab the panel
    void startDrag (const juce::MouseEvent&);

    DockManager& mgr_;
    DockArea& area_;
    const int uid_;
    std::vector<TabSlot> slots_;
    juce::Rectangle<int> menuIcon_, chevron_;
    bool overflow_ = false;
    int hoverTab_ = -1;

    // press / drag state
    bool pressed_ = false, dragging_ = false, wholeGroup_ = false;
    int pressedTab_ = -1;
    juce::Point<int> pressScreen_;
    juce::Point<int> pressLocal_;

    struct FocusTracker : public juce::MouseListener
    {
        explicit FocusTracker (GroupComponent& g) : owner (g) {}
        void mouseDown (const juce::MouseEvent&) override;
        GroupComponent& owner;
    } focusTracker_ { *this };

    JUCE_DECLARE_NON_COPYABLE (GroupComponent)
};

//==============================================================================
/** Lays out one tree of groups; the gaps between groups resize them. */
class DockArea : public juce::Component
{
public:
    DockArea (DockManager&, bool isMain);
    ~DockArea() override;

    bool isMain() const noexcept { return isMain_; }
    std::unique_ptr<Node> root;
    int maximizedUid = 0;   // group shown alone, 0 = none

    /** Creates / retires group components to match the tree, then lays out. */
    void sync();
    void layout();

    GroupComponent* componentFor (int uid) const;
    std::vector<GroupComponent*> groupComponents() const;
    DropOverlay& overlay() noexcept { return overlay_; }

    /** Group component under a point in local coordinates (visible ones only). */
    GroupComponent* groupAt (juce::Point<int> p) const;

    void paint (juce::Graphics&) override;
    void resized() override;
    /** Asked by JUCE at every mouse event: the cursor always matches what a
        press there would do (cursorTypeAt), even when the mouse entered the
        gap without moving inside it. */
    juce::MouseCursor getMouseCursor() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    /** A point (local coordinates) in a gap's grab zone: the gap plus
        style::kGapGrab px into each neighbour. */
    bool isOverGap (juce::Point<int>) const;
    /** Resize arrow over a gap's grab zone (and during a gap drag), the
        normal cursor elsewhere. */
    juce::MouseCursor::StandardCursorType cursorTypeAt (juce::Point<int>) const;

    /** Minimum size of a subtree along each axis (gap resizing, min window). */
    static juce::Point<int> minimumSize (const Node*);

private:
    struct Gap { int splitUid; int index; juce::Rectangle<int> r; bool horizontal; };
    void layoutNode (Node*, juce::Rectangle<int>);
    const Gap* gapAt (juce::Point<int>) const;

    DockManager& mgr_;
    const bool isMain_;
    std::map<int, std::unique_ptr<GroupComponent>> groups_;
    std::vector<Gap> gaps_;
    DropOverlay overlay_;

    // gap drag
    int dragSplit_ = 0, dragIndex_ = -1, dragStart_ = 0;
    bool dragHorizontal_ = false;
    int dragSizeA_ = 0, dragSizeB_ = 0;
    double dragWeightSum_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE (DockArea)
};

//==============================================================================
/** A floating panel window: a thin drag strip with minimise, maximise and
    close buttons above its own dock area. Moving it snaps to screen edges
    and to other windows. */
class FloatingWindow : public juce::ResizableWindow,
                       public juce::FileDragAndDropTarget
{
public:
    explicit FloatingWindow (DockManager&);
    ~FloatingWindow() override;

    DockArea& area() noexcept { return area_; }
    const DockArea& area() const noexcept { return area_; }

    /** Window bounds for a dock area of the given screen size / position. */
    static juce::Rectangle<int> windowBoundsForArea (juce::Rectangle<int> areaScreenBounds);

    void paint (juce::Graphics&) override;
    void resized() override;
    void moved() override;
    void activeWindowStatusChanged() override;
    bool keyPressed (const juce::KeyPress&) override;
    void userTriedToCloseWindow() override;
    int getDesktopWindowStyleFlags() const override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

    /** Moves the window so its top-left lands at `pos` (screen), snapping
        within a few pixels to screen edges and other windows ("magnet"). */
    void moveWithMagnet (juce::Point<int> pos);

    /** The resize zone (style::kWindowGrab) is wider than the frame around
        the content (style::kWindowFrame): the content lets the band
        between them through to the resize border. */
    juce::BorderSize<int> getBorderThickness() const override;
    juce::BorderSize<int> getContentComponentBorder() const override;

    /** Maximise: fill the work area of the window's screen (no resize
        edges, like a maximised OS window); off restores the former bounds.
        Moving the title strip of a maximised window restores it first. */
    void setMaximised (bool);
    bool isMaximised() const noexcept { return maximised_; }
    /** Minimise: collapse to the title strip (it stays on screen, the same
        button brings the panels back). A floating panel has no taskbar
        button, so an OS minimise could leave it out of reach on Windows. */
    void setCollapsed (bool);
    bool isCollapsed() const noexcept { return collapsed_; }

    /** Keep the window above all other windows (WS_EX_TOPMOST). */
    void setAlwaysOnTop (bool on);
    bool isAlwaysOnTop() const noexcept { return alwaysOnTop_; }
    /** The window's bounds when neither maximised nor collapsed: what the
        layout saves. */
    juce::Rectangle<int> normalBounds() const;
    /** Dock area screen bounds for a window of the given bounds (the
        inverse of windowBoundsForArea). */
    static juce::Rectangle<int> areaBoundsForWindow (juce::Rectangle<int> windowBounds);

private:
    class TitleStrip;
    class Holder;

    int collapsedHeight() const noexcept { return style::kTitleStripH + 2 * style::kWindowFrame; }

    DockManager& mgr_;
    DockArea area_;
    std::unique_ptr<TitleStrip> title_;
    std::unique_ptr<Holder> holder_;
    bool maximised_ = false, collapsed_ = false, alwaysOnTop_ = false;
    juce::Rectangle<int> restoreBounds_;   // before maximising
    int restoreHeight_ = 0;                // before collapsing

    JUCE_DECLARE_NON_COPYABLE (FloatingWindow)
};

//==============================================================================
/** Owns the main dock area, the floating windows and the panel registry, and
    runs every docking operation (open / close / move / undock / drag). */
class DockManager : private juce::AsyncUpdater
{
public:
    DockManager();
    ~DockManager() override;

    // ------------------------------------------------------------------ panels
    void registerPanel (PanelDesc);
    const PanelDesc* panel (const juce::String& id) const;
    juce::StringArray panelIds() const;   // registration order
    /** Changes a panel's tab title (its tab strip re-lays out). */
    void setPanelTitle (const juce::String& id, const juce::String& title);
    /** Forgets a panel (one per document, for instance): taken out of the
        layout first when open - even when not closable - its content
        detached and its remembered spot dropped. */
    void unregisterPanel (const juce::String& id);

    /* The owner may add its own entries to a panel's header menu (right-click
       on a tab, or on the ⋯ button): MainWindow adds "Open Containing Folder"
       and "Scale..." for the Editor. Ids handed out by the callback must be
       above 1000 so they cannot collide with the docking ids. */
    std::function<void (juce::PopupMenu& menu, const juce::String& panelId)> panelMenuExtender;

    DockArea& mainArea() noexcept { return *mainArea_; }
    const DockArea& mainArea() const noexcept { return *mainArea_; }

    // ----------------------------------------------------------------- queries
    struct Location { DockArea* area = nullptr; Node* group = nullptr; int index = -1; };
    Location locate (const juce::String& id) const;
    bool isOpen (const juce::String& id) const { return locate (id).area != nullptr; }
    bool isFloating (const juce::String& id) const;
    /** Tabs of the group holding a panel (empty when closed). */
    juce::StringArray groupTabs (const juce::String& id) const;
    /** Every group of every area, main area first. */
    std::vector<Node*> allGroups() const;
    Node* findGroup (int uid, DockArea** areaOut = nullptr) const;
    /** The floating window holding this group (nullptr when docked). */
    FloatingWindow* floaterForGroup (int uid) const;
    /** Screen bounds of the panel's content while open and showing. */
    juce::Rectangle<int> panelScreenBounds (const juce::String& id) const;

    // -------------------------------------------------------------- operations
    /** Opens a closed panel where it was last - its former group while one of
        its tab mates is open, else a new group beside the group it sat next
        to, else its last floating spot, else its default buddy's group - and
        makes it the active tab. No-op when already open. */
    void openPanel (const juce::String& id);
    /** Open + make the tab active + focus its group. */
    void showPanel (const juce::String& id);
    /** Opens a closed panel as a tab of the group holding `mateId` (at
        insertIndex, -1 = last) and makes it that group's active tab. Unlike
        openPanel(), a maximised group stays maximised. Falls back to
        openPanel() when mateId is not open. */
    void openPanelAsTabOf (const juce::String& id, const juce::String& mateId, int insertIndex = -1);
    void closePanel (const juce::String& id);
    void closeOthersInGroup (const juce::String& id);
    void closeGroup (int groupUid);
    /** Undocks a panel (or its whole group) into a new floating window. */
    void floatPanel (const juce::String& id, std::optional<juce::Rectangle<int>> areaScreenBounds = {});
    void floatGroup (int groupUid, std::optional<juce::Rectangle<int>> areaScreenBounds = {});
    /** A floating panel goes back to where it was last docked in the main area. */
    void dockPanelBack (const juce::String& id);
    /** Moves panels to a zone of a target group (or of the area for the
        area* zones; targetGroupUid is ignored then). Returns false when the
        move is impossible or a no-op. */
    bool movePanels (const juce::StringArray& ids, DockArea& area, int targetGroupUid,
                     Zone zone, int insertIndex = -1);
    bool movePanel (const juce::String& id, DockArea& area, int targetGroupUid,
                    Zone zone, int insertIndex = -1)
    { return movePanels (juce::StringArray (id), area, targetGroupUid, zone, insertIndex); }
    void setActiveTab (int groupUid, int index);
    void toggleMaximize (int groupUid);
    bool isMaximized (int groupUid) const;
    void restoreMaximized();

    void setFocusedGroup (int uid);
    int focusedGroup() const noexcept { return focusedUid_; }

    /** Default buddy: where a panel goes when it has no remembered spot
        (the group of this other panel). */
    void setDefaultBuddy (const juce::String& id, const juce::String& buddy) { buddies_[id] = buddy; }

    // ------------------------------------------------------------ layout state
    juce::var saveLayout() const;
    /** Replaces the whole arrangement. Unknown panel ids are skipped; panels
        missing from the layout are opened next to their default buddy unless
        listed as closed. Returns false for an unusable value. */
    bool loadLayout (const juce::var&);

    // --------------------------------------------------------------- callbacks
    std::function<void()> onLayoutChanged;                        // persist
    std::function<void (const juce::String&, bool)> onPanelOpenChanged;   // id, open
    /** setActiveTab() changed a group's active tab (tab click, overflow
        menu, showPanel...): the panel id now shown. Not while loading. */
    std::function<void (const juce::String&)> onActiveTabChanged;
    std::function<bool (const juce::KeyPress&)> onUnhandledKey;  // keys typed in floating windows
    std::function<bool (const juce::StringArray&)> isInterestedInFiles;
    std::function<void (const juce::StringArray&)> onFilesDropped;
    std::function<juce::Component*()> getMainWindow;             // floating windows stay near / above it

    // ------------------------------------------------------- floating windows
    int numFloatingWindows() const noexcept { return (int) floaters_.size(); }
    FloatingWindow* floatingWindow (int i) const;
    void bringFloatingWindowsToFront();
    void setFloatingWindowsVisible (bool);
    void closeFloatingWindow (FloatingWindow&);
    /** All dock areas: the main one, then one per floating window. */
    std::vector<DockArea*> areas() const;

    // ---------------------------------------------------- drag (from groups)
    struct DropTarget
    {
        Zone zone = Zone::none;
        DockArea* area = nullptr;
        int groupUid = 0;
        int insertIndex = -1;
    };
    void beginDrag (GroupComponent& source, const juce::StringArray& ids, bool wholeGroup,
                    juce::Point<int> screenPos);
    void updateDrag (juce::Point<int> screenPos, juce::ModifierKeys);
    /** Drops (or cancels when Esc was pressed). Callers must not touch
        themselves afterwards: the source group may have been retired. */
    void endDrag (juce::Point<int> screenPos, juce::ModifierKeys);
    void cancelDrag();
    bool isDragging() const noexcept { return drag_ != nullptr; }
    DropTarget currentDropTarget() const;
    /** The drop target under a screen position (public for the unit tests). */
    DropTarget hitTest (juce::Point<int> screenPos, juce::ModifierKeys) const;

    // ------------------------------------------------------------- internals
    int newUid() noexcept { return ++uidCounter_; }
    /** Hides a component now and deletes it later (it may be the source of
        the mouse event being handled). */
    void retire (std::unique_ptr<juce::Component>);
    void layoutChanged();             // sync every area + notify
    void geometryChanged();           // a floating window moved / resized: notify only
    void flushRetired();              // tests: delete retired components now

private:
    /** Where a panel was, so closing + reopening (or undock + dock) puts it back. */
    struct Memory
    {
        juce::StringArray mates;          // its group's tabs (itself included, in order)
        int index = -1;
        bool floating = false;
        juce::Rectangle<int> floatBounds; // dock area bounds of its floating window
        juce::StringArray neighbourTabs;  // when it was alone: the subtree next to it...
        Zone neighbourSide = Zone::none;  // ...and on which side of it the group was
        double neighbourShare = 0.3;      // its share of the space it had with that subtree
        // The same, for the last place in the MAIN area (Dock Panel).
        juce::StringArray dockedMates;
        int dockedIndex = -1;
        juce::StringArray dockedNeighbourTabs;
        Zone dockedNeighbourSide = Zone::none;
        double dockedNeighbourShare = 0.3;
    };
    struct DragSession;

    void handleAsyncUpdate() override;
    bool movePanelsImpl (const juce::StringArray& ids, DockArea& area, int targetGroupUid,
                         Zone zone, int insertIndex, int activeInIds);
    std::unique_ptr<Node> makeGroup (const juce::StringArray& tabs, int active = 0);
    /** Removes a panel from its group; empty groups and single-child splits
        collapse. Returns the area it was in. */
    DockArea* detach (const juce::String& id);
    std::unique_ptr<Node> detachGroup (int groupUid, DockArea** areaOut);
    void collapse (DockArea&, Node* split);
    /** Docks `node` on one side of `target`. The new node takes
        preferredPx (as a share of the target's size, 15..50 %) or exactly
        `share` when given. */
    void insertBeside (DockArea&, Node* target, std::unique_ptr<Node>, Zone, int preferredPx,
                       double share = -1.0);
    void insertAtAreaEdge (DockArea&, std::unique_ptr<Node>, Zone, int preferredPx);
    /** Reinserts a panel as a new group next to the remembered neighbour
        subtree; false when none of its tabs is open in a suitable area. */
    bool reinsertNextToNeighbour (const juce::String& id, const juce::StringArray& neighbourTabs,
                                  Zone side, double share, bool mainOnly, int& focusUid);
    void remember (const juce::String& id);
    void detachContent (const juce::String& id);
    void removeEmptyFloaters();
    FloatingWindow& createFloater (juce::Rectangle<int> areaScreenBounds);
    juce::Rectangle<int> defaultFloatBounds (const juce::String& id) const;
    void notifyOpen (const juce::String& id, bool open);
    juce::var nodeToVar (const Node*) const;
    std::unique_ptr<Node> nodeFromVar (const juce::var&, std::vector<juce::String>& placed);

    std::vector<PanelDesc> panels_;
    std::map<juce::String, Memory> memory_;
    std::map<juce::String, juce::String> buddies_;
    std::unique_ptr<DockArea> mainArea_;
    std::vector<std::unique_ptr<FloatingWindow>> floaters_;
    std::vector<std::unique_ptr<juce::Component>> retired_;
    std::unique_ptr<DragSession> drag_;
    int uidCounter_ = 0;
    int focusedUid_ = 0;
    bool loading_ = false;
    bool escapePressed_ = false;   // Esc seen during a drag: cancel asynchronously

    JUCE_DECLARE_NON_COPYABLE (DockManager)
};

//==============================================================================
/** Named workspaces (Window > Workspaces and the workspace bar): every one
    keeps a SAVED layout (built-in or "Save Changes") and a CURRENT layout
    (live modifications, restored at the next launch). Persistence goes
    through two callbacks so any settings store can back it. */
class Workspaces
{
public:
    struct Entry
    {
        juce::String id, name;
        bool builtIn = false;
        juce::var saved;     // layout JSON value
        juce::var current;   // modified layout (void = same as saved)
    };

    std::function<juce::String (const juce::String& key)> load;
    std::function<void (const juce::String& key, const juce::String& value)> store;

    /** Built-in workspaces in display order; the first is the default. */
    void setBuiltIns (std::vector<Entry>);
    void restore();                       // read the list + the active id
    void persist() const;

    const std::vector<Entry>& entries() const noexcept { return entries_; }
    const Entry* find (const juce::String& id) const;
    juce::String activeId() const { return active_; }
    void setActiveId (const juce::String& id);

    /** The layout to apply for a workspace (current if modified, else saved). */
    juce::var layoutFor (const juce::String& id) const;
    void setCurrent (const juce::String& id, const juce::var& layout);
    void resetCurrent (const juce::String& id);            // back to saved
    void saveCurrentAsSaved (const juce::String& id);
    /** Adds a user workspace (saved = current = layout); returns its id. */
    juce::String add (const juce::String& name, const juce::var& layout);
    bool remove (const juce::String& id);                  // user workspaces only
    bool rename (const juce::String& id, const juce::String& name);
    void moveTo (const juce::String& id, int newIndex);

private:
    std::vector<Entry> entries_;
    std::vector<Entry> builtIns_;
    juce::String active_;
};

//==============================================================================
/** Workspace switcher shown at the right of the menu bar: workspace names as
    text tabs (the active one in blue, underlined), with a >> overflow menu. */
class WorkspaceBar : public juce::Component, public juce::TooltipClient
{
public:
    WorkspaceBar() = default;

    std::function<std::vector<std::pair<juce::String, juce::String>>()> getItems;   // id, name
    std::function<juce::String()> getActive;
    std::function<void (const juce::String&)> onSelect;
    /** Context / overflow menu: the caller fills it (workspace commands). */
    std::function<void (juce::PopupMenu&, const juce::String& clickedId)> buildMenu;
    std::function<void (int)> onMenuResult;

    /** Width the bar wants for all its items. */
    int idealWidth() const;

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    juce::String getTooltip() override;

    struct Slot { juce::String id; juce::Rectangle<int> r; };
    std::vector<Slot> layoutSlots() const;
    juce::Rectangle<int> chevronBounds() const;

private:
    void showMenu (const juce::String& clickedId);
    juce::String hover_;
    bool hoverChevron_ = false;
};

} // namespace sc::dock
