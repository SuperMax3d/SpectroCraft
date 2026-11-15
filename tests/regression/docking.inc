// Tests of the Adobe-style docking workspace (include/Docking.h, the Editor and
// workspaces in MainWindow). Included by tests/UnitTests.cpp; uses its helpers
// (CHECK, progress, mkMouse, makeTempFile, dockGroupOf, scSkip).

// ---------------------------------------------------------------- helpers

/** Compact text form of a dock tree: a group is (a,b*) with * on its active
    tab, a split H[...] / V[...] with its children separated by |. */
static juce::String dockTreeText (const sc::dock::Node* n)
{
    if (n == nullptr)
        return "-";
    if (n->isGroup ())
    {
        juce::StringArray t;
        for (int i = 0; i < n->tabs.size (); ++i)
            t.add (n->tabs[i] + (i == n->active ? "*" : ""));
        return "(" + t.joinIntoString (",") + ")";
    }
    juce::StringArray k;
    for (const auto& c : n->children)
        k.add (dockTreeText (c.get ()));
    return juce::String (n->horizontal ? "H[" : "V[") + k.joinIntoString ("|") + "]";
}

/** A DockManager with six plain panels a..f, its main area laid out at
    1000 x 700 without being on screen. */
struct DockFixture
{
    DockFixture()
    {
        for (const char* id : { "a", "b", "c", "d", "e", "f" })
            dock.registerPanel ({ id, juce::String (id).toUpperCase (), &panels[(size_t) (id[0] - 'a')], true, 200, 150 });
        dock.mainArea ().setBounds (0, 0, 1000, 700);
    }
    ~DockFixture() { dock.flushRetired (); }

    bool load (const char* json) { return dock.loadLayout (juce::JSON::parse (json)); }
    juce::String tree() const { return dockTreeText (dock.mainArea ().root.get ()); }
    int groupOf (const char* id) const
    {
        const auto l = dock.locate (id);
        return l.group != nullptr ? l.group->uid : 0;
    }
    juce::Component& panel (char id) { return panels[(size_t) (id - 'a')]; }

    // Panels first: the manager (declared after them) is destroyed first and
    // detaches them from its groups.
    juce::Component panels[6];
    sc::dock::DockManager dock;
};

// ---------------------------------------------------------------- zones

static void dockTest_zoneGeometry()
{
    progress ("docking: drop zone geometry");
    using sc::dock::DropOverlay;
    using Z = sc::dock::Zone;

    const juce::Rectangle<int> r (100, 50, 400, 300);
    CHECK (DropOverlay::innerRect (r) == r.reduced (56, 56), "dock zones: 56 px edge bands on a large group");
    CHECK (DropOverlay::innerRect ({ 0, 0, 120, 80 }) == juce::Rectangle<int> (30, 20, 60, 40),
           "dock zones: a quarter of a small group");
    CHECK (DropOverlay::zoneAt (r, r.getCentre ()) == Z::centre, "dock zones: centre rectangle");
    CHECK (DropOverlay::zoneAt (r, { 105, 200 }) == Z::left, "dock zones: left trapezoid");
    CHECK (DropOverlay::zoneAt (r, { 495, 200 }) == Z::right, "dock zones: right trapezoid");
    CHECK (DropOverlay::zoneAt (r, { 300, 55 }) == Z::top, "dock zones: top trapezoid");
    CHECK (DropOverlay::zoneAt (r, { 300, 345 }) == Z::bottom, "dock zones: bottom trapezoid");
    // Near a corner the diagonal between two trapezoids decides.
    CHECK (DropOverlay::zoneAt (r, { 110, 90 }) == Z::left, "dock zones: below the top-left diagonal is left");
    CHECK (DropOverlay::zoneAt (r, { 140, 60 }) == Z::top, "dock zones: above the top-left diagonal is top");
    CHECK (DropOverlay::zoneAt (r, { 50, 50 }) == Z::none, "dock zones: outside the group");

    const auto left = DropOverlay::zonePath (r, Z::left);
    CHECK (left.contains (105.0f, 200.0f) && ! left.contains (300.0f, 200.0f) && ! left.contains (300.0f, 55.0f),
           "dock zones: the left trapezoid path covers only its zone");
}

// ---------------------------------------------------------------- tree

static void dockTest_treeOperations()
{
    progress ("docking: tree operations");
    using Z = sc::dock::Zone;
    DockFixture f;
    auto& main = f.dock.mainArea ();
    f.dock.setDefaultBuddy ("e", "c");

    CHECK (f.load (R"({"main":{"split":"h","children":[{"tabs":["a","b"],"active":"b"},{"tabs":["c"]}],"weights":[0.3,0.7]},"closed":["d","f"]})"),
           "dock tree: layout loaded");
    CHECK (f.tree () == "H[(a,b*)|(c,e*)]",
           "dock tree: a panel missing from a layout joins its default buddy: " + f.tree ());
    CHECK (! f.dock.isOpen ("d") && ! f.dock.isOpen ("f"), "dock tree: closed panels stay closed");
    CHECK (f.panel ('b').getParentComponent () == main.componentFor (f.groupOf ("b"))
               && f.panel ('a').getParentComponent () == nullptr,
           "dock tree: only the active tab's content is attached");

    // Docking beside a group whose parent split has the same direction adds
    // a sibling; otherwise a new split replaces the group.
    CHECK (f.dock.movePanel ("a", main, f.groupOf ("c"), Z::right), "dock tree: dock right accepted");
    CHECK (f.tree () == "H[(b*)|(c,e*)|(a*)]", "dock tree: right of a group: " + f.tree ());
    CHECK (f.dock.movePanel ("b", main, f.groupOf ("c"), Z::centre), "dock tree: centre accepted");
    CHECK (f.tree () == "H[(c,e,b*)|(a*)]", "dock tree: an emptied group disappears: " + f.tree ());
    CHECK (f.dock.movePanel ("a", main, f.groupOf ("c"), Z::top), "dock tree: dock top accepted");
    CHECK (f.tree () == "V[(a*)|(c,e,b*)]",
           "dock tree: a single-child split collapses, then a vertical split: " + f.tree ());
    CHECK (f.dock.movePanel ("a", main, f.groupOf ("c"), Z::tabStrip, 1), "dock tree: tab strip accepted");
    CHECK (f.tree () == "(c,a*,e,b)", "dock tree: inserted at the caret: " + f.tree ());
    CHECK (f.dock.movePanel ("c", main, f.groupOf ("c"), Z::tabStrip, 4), "dock tree: reorder accepted");
    CHECK (f.tree () == "(a,e,b,c*)", "dock tree: reordered inside the group: " + f.tree ());
    CHECK (! f.dock.movePanel ("c", main, f.groupOf ("c"), Z::centre), "dock tree: onto its own group is a no-op");
    CHECK (! f.dock.movePanels ({ "a", "e", "b", "c" }, main, f.groupOf ("c"), Z::left),
           "dock tree: a whole group against itself is a no-op");

    // Removing a split's last-but-one child flattens it into a parent of
    // the same direction, keeping the proportions.
    CHECK (f.load (R"({"main":{"split":"h","children":[{"split":"v","children":[{"split":"h","children":[{"tabs":["a"]},{"tabs":["b"]}],"weights":[0.5,0.5]},{"tabs":["c"]}],"weights":[0.6,0.4]},{"tabs":["d"]}],"weights":[0.5,0.5]},"closed":["e","f"]})"),
           "dock tree: nested layout loaded");
    CHECK (f.tree () == "H[V[H[(a*)|(b*)]|(c*)]|(d*)]", "dock tree: nested layout: " + f.tree ());
    f.dock.closePanel ("c");
    CHECK (f.tree () == "H[(a*)|(b*)|(d*)]", "dock tree: collapsed and flattened: " + f.tree ());
    {
        const auto& w = main.root->weights;
        CHECK (w.size () == 3 && std::abs (w[0] - 0.25) < 1e-6 && std::abs (w[1] - 0.25) < 1e-6
                   && std::abs (w[2] - 0.5) < 1e-6,
               "dock tree: flattened weights keep the proportions");
    }

    // Close + reopen: the relative tab order comes back (not the old index).
    CHECK (f.load (R"({"main":{"split":"h","children":[{"tabs":["a","b","c","d"],"active":"a"},{"tabs":["e"]}],"weights":[0.4,0.6]},"closed":["f"]})"),
           "dock tree: four-tab layout loaded");
    f.dock.closePanel ("c");
    f.dock.closePanel ("b");
    CHECK (f.tree () == "H[(a*,d)|(e*)]", "dock tree: two tabs closed: " + f.tree ());
    f.dock.openPanel ("c");
    CHECK (f.tree () == "H[(a,c*,d)|(e*)]", "dock tree: reopened after its predecessor: " + f.tree ());
    f.dock.openPanel ("b");
    CHECK (f.tree () == "H[(a,b*,c,d)|(e*)]", "dock tree: original order restored: " + f.tree ());

    // The last panel of a group comes back on the same side, same share.
    f.dock.closePanel ("e");
    CHECK (f.tree () == "(a,b*,c,d)", "dock tree: a lone panel closed: " + f.tree ());
    f.dock.openPanel ("e");
    CHECK (f.tree () == "H[(a,b*,c,d)|(e*)]", "dock tree: back on its side: " + f.tree ());
    CHECK (main.root != nullptr && main.root->weights.size () == 2 && std::abs (main.root->weights[1] - 0.6) < 0.02,
           "dock tree: with its share of the space");

    f.dock.closeOthersInGroup ("b");
    CHECK (f.tree () == "H[(b*)|(e*)]", "dock tree: Close Other Panels in Group: " + f.tree ());
    f.dock.closeGroup (f.groupOf ("e"));
    CHECK (f.tree () == "(b*)" && ! f.dock.isOpen ("e"), "dock tree: Close Panel Group: " + f.tree ());

    // Maximise: the group alone over the whole area; a move restores.
    CHECK (f.load (R"({"main":{"split":"h","children":[{"tabs":["a"]},{"tabs":["b"]},{"tabs":["c"]}]},"closed":["d","e","f"]})"),
           "dock tree: three groups loaded");
    f.dock.toggleMaximize (f.groupOf ("b"));
    CHECK (f.dock.isMaximized (f.groupOf ("b")) && ! main.componentFor (f.groupOf ("a"))->isVisible ()
               && main.componentFor (f.groupOf ("b"))->getBounds ()
                      == main.getLocalBounds ().reduced (sc::dock::style::kOuterMargin),
           "dock tree: maximised group fills the area");
    f.dock.movePanel ("c", main, f.groupOf ("a"), Z::centre);
    CHECK (! f.dock.isMaximized (f.groupOf ("b")) && main.componentFor (f.groupOf ("a"))->isVisible (),
           "dock tree: a move restores the maximised group");

    // The gap between two groups resizes them.
    CHECK (f.load (R"({"main":{"split":"h","children":[{"tabs":["a"]},{"tabs":["b"]}],"weights":[0.5,0.5]},"closed":["c","d","e","f"]})"),
           "dock tree: two groups loaded");
    {
        auto* ga = main.componentFor (f.groupOf ("a"));
        const int gapX = ga->getRight () + 2;
        main.mouseDown (mkMouse (main, gapX, 300, {}));
        main.mouseDrag (mkMouse (main, gapX + 100, 300, {}));
        main.mouseUp (mkMouse (main, gapX + 100, 300, {}));
        CHECK (std::abs (main.componentFor (f.groupOf ("a"))->getWidth () - 594) <= 1,
               "dock tree: dragging the gap widens the left group by 100 px ("
                   + juce::String (main.componentFor (f.groupOf ("a"))->getWidth ()) + ")");
        main.mouseDoubleClick (mkMouse (main, main.componentFor (f.groupOf ("a"))->getRight () + 2, 300, {}, 2));
        CHECK (std::abs (main.componentFor (f.groupOf ("a"))->getWidth ()
                         - main.componentFor (f.groupOf ("b"))->getWidth ()) <= 1,
               "dock tree: double-clicking the gap shares the space equally");
    }

    // Unusable layouts are refused without touching the arrangement.
    const auto before = f.tree ();
    CHECK (! f.dock.loadLayout (juce::var ()) && ! f.dock.loadLayout (juce::JSON::parse ("[1,2]")),
           "dock tree: non-layout values refused");
    CHECK (f.tree () == before, "dock tree: a refused layout changes nothing");
    CHECK (f.load (R"({"main":{"split":"v","children":[{"tabs":["zz","a","a"]},{"tabs":["a","b"]}]},"closed":["c","d","e","f"]})"),
           "dock tree: layout with unknown and repeated ids loaded");
    CHECK (f.tree () == "V[(a*)|(b*)]", "dock tree: unknown ids skipped, each panel placed once: " + f.tree ());
}

// ---------------------------------------------------------------- floating

static void dockTest_floatingWindows()
{
    progress ("docking: floating windows");
    using Z = sc::dock::Zone;
    DockFixture f;
    auto& main = f.dock.mainArea ();

    CHECK (f.load (R"({"main":{"split":"h","children":[{"tabs":["a","b","c"],"active":"a"},{"tabs":["d"]}],"weights":[0.5,0.5]},"closed":["e","f"]})"),
           "dock float: layout loaded");
    f.dock.floatPanel ("c", juce::Rectangle<int> (220, 240, 320, 220));
    CHECK (f.dock.isFloating ("c") && f.dock.numFloatingWindows () == 1, "dock float: Undock Panel opens a window");
    auto* fw = f.dock.floatingWindow (0);
    CHECK (fw != nullptr && fw->isVisible () && f.panel ('c').getTopLevelComponent () == fw,
           "dock float: the panel lives in that window");
    if (fw == nullptr)
        return;
    {
        const auto b = fw->area ().getScreenBounds ();
        CHECK (std::abs (b.getX () - 220) <= 2 && std::abs (b.getWidth () - 320) <= 2 && std::abs (b.getHeight () - 220) <= 2,
               "dock float: the window's dock area has the requested bounds");
    }

    // A floating window accepts other panels and splits like the main area.
    CHECK (f.dock.movePanel ("d", fw->area (), f.groupOf ("c"), Z::bottom), "dock float: dock into the window");
    CHECK (dockTreeText (fw->area ().root.get ()) == "V[(c*)|(d*)]" && f.tree () == "(a*,b)",
           "dock float: two groups in the floating window: " + dockTreeText (fw->area ().root.get ()));

    // Dock Panel: back to its former group in the main area, at its place.
    f.dock.dockPanelBack ("c");
    CHECK (f.tree () == "(a,b,c*)", "dock float: Dock Panel restores the former place: " + f.tree ());
    CHECK (f.dock.isFloating ("d") && f.dock.numFloatingWindows () == 1, "dock float: the window keeps its other panel");

    // The window's last panel moved out: the window goes away.
    f.dock.movePanel ("d", main, f.groupOf ("a"), Z::right);
    f.dock.flushRetired ();
    CHECK (f.dock.numFloatingWindows () == 0 && f.tree () == "H[(a,b,c*)|(d*)]",
           "dock float: an emptied floating window is removed: " + f.tree ());

    // Closing a floating window closes its panels; reopening one puts it
    // back in a window at the same place.
    f.dock.floatPanel ("b", juce::Rectangle<int> (300, 260, 260, 180));
    CHECK (f.dock.numFloatingWindows () == 1, "dock float: a second undock");
    const auto spot = f.dock.floatingWindow (0)->area ().getScreenBounds ();
    f.dock.closeFloatingWindow (*f.dock.floatingWindow (0));
    f.dock.flushRetired ();
    CHECK (! f.dock.isOpen ("b") && f.dock.numFloatingWindows () == 0, "dock float: closing the window closes its panels");
    f.dock.openPanel ("b");
    CHECK (f.dock.isFloating ("b") && f.dock.numFloatingWindows () == 1
               && f.dock.floatingWindow (0)->area ().getScreenBounds ().getPosition ().getDistanceFrom (spot.getPosition ()) <= 2,
           "dock float: reopened in a window at its last spot");

    // Round trip of the whole arrangement through its JSON text.
    const auto text = juce::JSON::toString (f.dock.saveLayout ());
    DockFixture g;
    CHECK (g.dock.loadLayout (juce::JSON::parse (text)), "dock float: saved layout loads");
    CHECK (g.tree () == f.tree () && g.dock.numFloatingWindows () == 1 && g.dock.isFloating ("b")
               && ! g.dock.isOpen ("e"),
           "dock float: layout round trip (" + g.tree () + ")");
    g.dock.closePanel ("b");
    g.dock.flushRetired ();
    f.dock.closePanel ("b");
    f.dock.flushRetired ();
    g.dock.dockPanelBack ("b");   // not floating: nothing
    CHECK (! g.dock.isOpen ("b") && g.dock.numFloatingWindows () == 0, "dock float: Dock Panel ignores closed panels");
}

// ---------------------------------------------------------------- mouse

/** Drags the tab of `id` to a screen point with real mouse events on its
    group; returns the drop target seen just before the release. */
static sc::dock::DockManager::DropTarget dockDragTab (sc::dock::DockManager& dock, const juce::String& id,
                                                    juce::Point<int> screenTarget, juce::ModifierKeys mods = {})
{
    const auto loc = dock.locate (id);
    if (loc.area == nullptr)
        return {};
    dock.setActiveTab (loc.group->uid, loc.index);   // shown, not in the >> menu
    auto* src = dockGroupOf (dock, id);
    if (src == nullptr)
        return {};
    const auto tab = src->tabBounds (dock.locate (id).index);
    const auto rel = screenTarget - src->getScreenPosition ();
    src->mouseDown (mkMouse (*src, tab.getCentreX (), tab.getCentreY (), mods));
    src->mouseDrag (mkMouse (*src, rel.x, rel.y, mods));
    const auto t = dock.currentDropTarget ();
    src->mouseUp (mkMouse (*src, rel.x, rel.y, mods));   // src may be retired now
    return t;
}

static void dockTest_mouseDrag()
{
    progress ("docking: mouse drags in the main window");
    using Z = sc::dock::Zone;
    const juce::File store = makeTempFile ("dock_drag.recent");
    store.deleteFile ();

    juce::String mainTree;
    {
        Application app (store);
        std::unique_ptr<MainWindow> win (new MainWindow (app, "dock drag test"));
        auto& dock = win->dockManager ();

        // 1) History onto the right trapezoid of the Editor's group.
        auto* ed = dockGroupOf (dock, "editor");
        CHECK (ed != nullptr && ed->isShowing (), "dock drag: the Editor's group is on screen");
        if (ed == nullptr || ! ed->isShowing ())
            return;
        const auto c = ed->contentBounds ();
        auto t = dockDragTab (dock, "history", ed->localPointToGlobal (juce::Point<int> (c.getRight () - 25, c.getCentreY ())));
        CHECK (t.zone == Z::right && t.groupUid == ed->getUid (), "dock drag: hovered the Editor's right zone");
        {
            auto* hist = dockGroupOf (dock, "history");
            auto* ed2 = dockGroupOf (dock, "editor");
            CHECK (dock.groupTabs ("history") == juce::StringArray ("history") && hist != nullptr && ed2 != nullptr
                       && hist->getX () >= ed2->getRight () && std::abs (hist->getY () - ed2->getY ()) <= 1,
                   "dock drag: History docked right of the Editor");
        }

        // 2) Ctrl (Cmd) held: never docks, undocks into a floating window.
        {
            auto* ed2 = dockGroupOf (dock, "editor");
            t = dockDragTab (dock, "history", ed2->localPointToGlobal (ed2->getLocalBounds ().getCentre ()),
                             juce::ModifierKeys::commandModifier);
            CHECK (t.zone == Z::floating && dock.isFloating ("history") && dock.numFloatingWindows () == 1,
                   "dock drag: Ctrl-drag undocks");
        }

        // 3) Back into the main window: the floating window goes away.
        {
            auto* mk = dockGroupOf (dock, "markers");
            t = dockDragTab (dock, "history", mk->localPointToGlobal (mk->contentBounds ().getCentre ()));
            dock.flushRetired ();
            CHECK (t.zone == Z::centre && dock.groupTabs ("markers").contains ("history")
                       && dock.numFloatingWindows () == 0,
                   "dock drag: docked back from the floating window");
        }

        // 4) Dropped outside every window: a floating window at the drop
        //    point, which then accepts another panel on its bottom zone.
        t = dockDragTab (dock, "tables", win->getScreenBounds ().getBottomRight () + juce::Point<int> (300, 300));
        CHECK (t.zone == Z::floating && dock.isFloating ("tables") && dock.numFloatingWindows () == 1,
               "dock drag: dropped outside, undocked");
        if (auto* tg = dockGroupOf (dock, "tables"))
        {
            const auto fc = tg->contentBounds ();
            t = dockDragTab (dock, "python", tg->localPointToGlobal (juce::Point<int> (fc.getCentreX (), fc.getBottom () - 25)));
            CHECK (t.zone == Z::bottom && dock.isFloating ("python")
                       && dockTreeText (dock.floatingWindow (0)->area ().root.get ()) == "V[(tables*)|(python*)]",
                   "dock drag: the floating window hosts a second panel");
        }

        // 5) The empty part of a tab strip drags the whole group: the
        //    Editor's group along the left edge of the main area.
        if (auto* eg = dockGroupOf (dock, "editor"))
        {
            const auto strip = eg->stripBounds ();
            const juce::Point<int> empty (strip.getRight () - 30, strip.getCentreY ());
            const auto edge = dock.mainArea ().localPointToGlobal (juce::Point<int> (3, dock.mainArea ().getHeight () / 2));
            const auto rel = edge - eg->getScreenPosition ();
            CHECK (eg->tabIndexAt (empty) < 0, "dock drag: an empty spot of the Editor's strip");
            eg->mouseDown (mkMouse (*eg, empty.x, empty.y, {}));
            eg->mouseDrag (mkMouse (*eg, rel.x, rel.y, {}));
            t = dock.currentDropTarget ();
            eg->mouseUp (mkMouse (*eg, rel.x, rel.y, {}));
            auto* eg2 = dockGroupOf (dock, "editor");
            CHECK (t.zone == Z::areaLeft && eg2 != nullptr && eg2->getX () <= sc::dock::style::kOuterMargin + 1
                       && dock.groupTabs ("editor") == juce::StringArray ("editor"),
                   "dock drag: the whole group docked along the area's left edge");
        }

        // 6) A cancelled drag (Esc) changes nothing.
        {
            const auto loc = dock.locate ("controls");
            dock.setActiveTab (loc.group->uid, loc.index);
            const auto before = dockTreeText (dock.mainArea ().root.get ());
            auto* g = dockGroupOf (dock, "controls");
            const auto tb = g->tabBounds (loc.index);
            g->mouseDown (mkMouse (*g, tb.getCentreX (), tb.getCentreY (), {}));
            g->mouseDrag (mkMouse (*g, tb.getCentreX () + 200, tb.getCentreY () + 200, {}));
            CHECK (dock.isDragging (), "dock drag: dragging");
            dock.cancelDrag ();
            g->mouseUp (mkMouse (*g, tb.getCentreX () + 200, tb.getCentreY () + 200, {}));
            CHECK (! dock.isDragging () && dockTreeText (dock.mainArea ().root.get ()) == before,
                   "dock drag: a cancelled drag changes nothing");
        }

        // 7) Right-click tab menu commands, through the same calls.
        dock.floatPanel ("selection");
        CHECK (dock.isFloating ("selection") && dock.numFloatingWindows () == 2, "dock drag: Undock Panel");
        dock.dockPanelBack ("selection");
        dock.flushRetired ();
        CHECK (! dock.isFloating ("selection") && win->panelGroupTitles ("controls").contains ("Selection"),
               "dock drag: Dock Panel");

        mainTree = dockTreeText (dock.mainArea ().root.get ());
        win.reset ();   // saves the arrangement
    }

    // 8) Restart: the main area and the floating window come back.
    {
        Application app (store);
        std::unique_ptr<MainWindow> win (new MainWindow (app, "dock drag test 2"));
        auto& dock = win->dockManager ();
        CHECK (dockTreeText (dock.mainArea ().root.get ()) == mainTree, "dock drag: main area restored");
        CHECK (dock.numFloatingWindows () == 1 && dock.isFloating ("tables") && dock.isFloating ("python"),
               "dock drag: floating window restored with its panels");
        win.reset ();
    }
    store.deleteFile ();
}

// ---------------------------------------------------------------- workspaces

static void dockTest_workspaces()
{
    progress ("docking: workspaces");
    using Z = sc::dock::Zone;
    const juce::File store = makeTempFile ("dock_workspaces.recent");
    store.deleteFile ();
    juce::String mine;

    {
        Application app (store);
        std::unique_ptr<MainWindow> win (new MainWindow (app, "workspace test"));
        auto& ws = win->workspaces ();
        auto& dock = win->dockManager ();

        CHECK (ws.activeId () == "default" && ws.entries ().size () == 4, "workspaces: four built-ins, Default active");
        auto* bar = win->workspaceBarComp ();
        CHECK (bar != nullptr && bar->isVisible () && bar->getRight () == win->getLocalBounds ().getRight ()
                   && bar->layoutSlots ().size () == 4,
               "workspaces: the bar shows them right of the menu bar");

        // Modify Default, visit Analysis, come back: Default kept its change.
        {
            const auto tl = dock.locate ("tables");
            dock.movePanel ("markers", *tl.area, tl.group->uid, Z::tabStrip, 0);
        }
        const auto modified = win->panelGroupTitles ("tables");
        win->switchWorkspace ("analysis");
        {
            auto* fftG = dockGroupOf (dock, "fft");
            auto* edG = dockGroupOf (dock, "editor");
            CHECK (ws.activeId () == "analysis" && win->panelGroupTitles ("fft") == juce::StringArray ("Frequency Analysis")
                       && fftG != nullptr && edG != nullptr && fftG->getX () >= edG->getRight (),
                   "workspaces: Analysis puts Frequency Analysis beside the Editor");
        }
        win->switchWorkspace ("default");
        CHECK (win->panelGroupTitles ("tables") == modified, "workspaces: a workspace keeps its modifications");
        win->resetWorkspace ();
        CHECK (win->panelGroupTitles ("controls").contains ("Markers")
                   && ! win->panelGroupTitles ("tables").contains ("Markers"),
               "workspaces: Reset to Saved Layout");

        // Save as New Workspace: a user workspace, active, with this layout.
        win->setPanelVisible ("python", false);
        mine = win->saveWorkspaceAs ("Mine");
        CHECK (mine.isNotEmpty () && ws.activeId () == mine && ws.entries ().size () == 5
                   && bar->layoutSlots ().size () >= 4,
               "workspaces: Save as New Workspace");
        win->switchWorkspace ("default");
        win->resetWorkspace ();
        CHECK (win->isPanelOpen ("python"), "workspaces: Default reset brings Python back");
        win->switchWorkspace (mine);
        CHECK (! win->isPanelOpen ("python"), "workspaces: the new workspace keeps Python closed");

        CHECK (win->renameWorkspace (mine, "Mine 2") && ws.find (mine) != nullptr && ws.find (mine)->name == "Mine 2",
               "workspaces: renamed");
        CHECK (! win->deleteWorkspace ("default") && ws.entries ().size () == 5,
               "workspaces: built-in workspaces cannot be deleted");

        // Save Changes to this Workspace: the modified layout becomes the saved one.
        win->setPanelVisible ("tables", false);
        win->saveWorkspaceChanges ();
        win->setPanelVisible ("tables", true);
        win->resetWorkspace ();
        CHECK (! win->isPanelOpen ("tables"), "workspaces: Save Changes, then Reset keeps the saved change");
        win.reset ();
    }

    // Restart: the active workspace and its layout are restored.
    {
        Application app (store);
        std::unique_ptr<MainWindow> win (new MainWindow (app, "workspace test 2"));
        auto& ws = win->workspaces ();
        CHECK (ws.activeId () == mine && ws.entries ().size () == 5 && ws.find (mine)->name == "Mine 2"
                   && ! win->isPanelOpen ("python") && ! win->isPanelOpen ("tables"),
               "workspaces: restored after a restart");

        CHECK (win->deleteWorkspace (mine) && ws.entries ().size () == 4 && ws.activeId () == "default",
               "workspaces: deleting the active one goes back to the first");
        CHECK (win->isPanelOpen ("python"), "workspaces: the Default layout applied");

        const int wIdx = win->getMenuBarNames ().indexOf ("Window");
        win->menuItemSelected (MainWindow::WindowWorkspaceBase + 2, wIdx);   // third entry: Analysis
        CHECK (ws.activeId () == "analysis", "workspaces: switched from the Window menu");
        win.reset ();
    }
    store.deleteFile ();
}

// ---------------------------------------------------------------- migration

static void dockTest_legacyLayout()
{
    progress ("docking: arrangement of the old left column");
    const juce::File store = makeTempFile ("dock_legacy.recent");
    store.deleteFile ();
    {
        juce::PropertiesFile pf (store, juce::PropertiesFile::Options ());
        // A layout saved by an older version: it still names the Files panel,
        // which no longer exists. The unknown id is dropped, the rest of the
        // order is kept.
        pf.setValue ("left_tab_layout", "fft;actions|controls;markers");
        pf.setValue ("left_panels_closed", "history;drawing");
        pf.setValue ("left_panel_w", 560);
        pf.setValue ("top_ratio_mil", 300);
        pf.save ();
    }

    Application app (store);
    std::unique_ptr<MainWindow> win (new MainWindow (app, "legacy layout test"));
    auto& dock = win->dockManager ();
    const auto top = win->panelGroupTitles ("fft");
    CHECK (top.size () >= 2 && top[0] == "Frequency Analysis" && top[1] == "Actions",
           "legacy: the top group keeps its tab order (History was closed there)");
    CHECK (win->panelGroupTitles ("controls").size () >= 2 && win->panelGroupTitles ("controls")[0] == "Spectral Controls"
               && win->panelGroupTitles ("controls")[1] == "Markers",
           "legacy: the bottom group keeps its tab order");
    CHECK (! win->isPanelOpen ("history") && ! win->isPanelOpen ("drawing"), "legacy: closed panels stay closed");
    CHECK (top.contains ("Python") && top.contains ("Video"), "legacy: panels missing from the list join their group");
    auto* tg = dockGroupOf (dock, "fft");
    auto* bg = dockGroupOf (dock, "controls");
    auto* eg = dockGroupOf (dock, "editor");
    CHECK (tg != nullptr && bg != nullptr && eg != nullptr
               && tg->getHeight () < bg->getHeight () && tg->getRight () <= eg->getX ()
               && tg->getWidth () > dock.mainArea ().getWidth () / 4,
           "legacy: column width and group heights follow the old sizes");
    win.reset ();
    store.deleteFile ();
}

// ---------------------------------------------------------------- Editor + video

static void dockTest_editorAndVideo()
{
    progress ("docking: floating Editor and video panel");
    const juce::File store = makeTempFile ("dock_editor.recent");
    store.deleteFile ();
    Application app (store);
    std::unique_ptr<MainWindow> win (new MainWindow (app, "dock editor test"));
    auto& dock = win->dockManager ();

    win->setPanelVisible ("editor", false);
    CHECK (win->isPanelOpen ("editor"), "dock editor: the Editor cannot be closed");

    dock.floatPanel ("editor");
    auto* fw = dock.floatingWindow (0);
    CHECK (dock.isFloating ("editor") && fw != nullptr
               && win->spectralViewComp ()->getTopLevelComponent () == fw,
           "dock editor: the views move into the floating window");
    CHECK (win->spectralViewComp ()->getWidth () > 0 && win->waveformViewComp ()->getWidth () > 0,
           "dock editor: and are laid out there");
    if (fw != nullptr)
    {
        // Shortcuts typed in a floating window act on the main window: F9
        // cycles the views (both -> waveform -> spectrogram -> both).
        CHECK (win->waveformVisible () && win->spectrogramVisible (), "dock editor: both views shown");
        fw->keyPressed (juce::KeyPress (juce::KeyPress::F9Key));
        CHECK (win->waveformVisible () && ! win->spectrogramVisible (), "dock editor: F9 in the floating window");
        fw->keyPressed (juce::KeyPress (juce::KeyPress::F9Key));
        fw->keyPressed (juce::KeyPress (juce::KeyPress::F9Key));
        CHECK (win->waveformVisible () && win->spectrogramVisible (), "dock editor: views back to both");

        // Closing that window docks the Editor back instead of closing it.
        dock.closeFloatingWindow (*fw);
        dock.flushRetired ();
        CHECK (! dock.isFloating ("editor") && dock.numFloatingWindows () == 0 && win->isPanelOpen ("editor")
                   && win->spectralViewComp ()->getTopLevelComponent () == win.get (),
               "dock editor: closing the floating window docks the Editor back");
    }

    // "Always on Top" lives in the floating panel-group menu: the manager
    // finds the window hosting a group through floaterForGroup.
    if (! sc::videoDecodingAvailable ())
        scSkip ("dock editor: no video decoder in this build (always-on-top)");
    else
    {
        win->setVideoDocked (true);
        auto* vw = win->getVideoWindow ();
        CHECK (vw != nullptr, "dock aot: the video window exists");

        auto* vg = dockGroupOf (dock, "video");
        const int uid = vg != nullptr ? vg->getUid() : 0;
        CHECK (dock.floaterForGroup (uid) == nullptr,
               "dock aot: no floater while the group is in the main window");

        dock.floatPanel ("video", juce::Rectangle<int> (100, 100, 420, 300));
        // Floating re-creates the group node with a fresh uid: locate it again.
        const auto loc = dock.locate ("video");
        auto* fw = loc.group != nullptr ? dock.floaterForGroup (loc.group->uid) : nullptr;
        CHECK (fw != nullptr && dock.isFloating ("video"),
               "dock aot: the floater hosts the floating group");

        if (fw != nullptr)
        {
            fw->setAlwaysOnTop (true);
            CHECK (fw->isAlwaysOnTop (), "dock aot: always-on-top turns on");
            fw->setAlwaysOnTop (false);
            CHECK (! fw->isAlwaysOnTop (), "dock aot: always-on-top turns off again");
        }

        dock.dockPanelBack ("video");
        dock.flushRetired ();
        CHECK (win->isPanelOpen ("video") && ! dock.isFloating ("video"),
               "dock aot: docking back removes the floater");
    }
    win.reset ();
    store.deleteFile ();
}

// Review findings: z-order of the drop target, a lone floating panel keeping
// its window size, Undock after Dock, reopening inside a floating window.
static void dockTest_floatingEdgeCases()
{
    progress ("docking: floating window edge cases");
    using Z = sc::dock::Zone;
    DockFixture f;
    CHECK (f.load (R"({"main":{"split":"h","children":[{"tabs":["a","b"]},{"tabs":["c","d"]}],"weights":[0.5,0.5]},"closed":["e","f"]})"),
           "dock edge: layout loaded");

    // A lone floating panel moved by its tab keeps its window's size.
    f.dock.floatPanel ("c", juce::Rectangle<int> (300, 300, 420, 300));
    auto* w0 = f.dock.floatingWindow (0);
    if (w0 == nullptr)
    {
        CHECK (false, "dock edge: floating window created");
        return;
    }
    const auto before = w0->area ().getScreenBounds ();
    f.dock.floatPanel ("c", juce::Rectangle<int> (before.getX () + 40, before.getY () + 30, 120, 90));
    const auto after = w0->area ().getScreenBounds ();
    CHECK (after.getWidth () == before.getWidth () && after.getHeight () == before.getHeight ()
               && after.getPosition () == before.getPosition () + juce::Point<int> (40, 30),
           "dock edge: a lone floating panel moves its window without resizing it ("
               + before.toString () + " -> " + after.toString () + ")");

    // Dock Panel, then Undock Panel: back at the remembered floating spot.
    f.dock.dockPanelBack ("c");
    f.dock.flushRetired ();
    CHECK (! f.dock.isFloating ("c") && f.dock.numFloatingWindows () == 0, "dock edge: docked back");
    f.dock.floatPanel ("c");
    CHECK (f.dock.isFloating ("c") && f.dock.floatingWindow (0) != nullptr
               && f.dock.floatingWindow (0)->area ().getScreenBounds ().getPosition ().getDistanceFrom (after.getPosition ()) <= 2,
           "dock edge: Undock Panel reuses the last floating spot");

    // A panel closed in a floating window with two groups reopens beside the
    // remaining group, in that window, instead of in a new window.
    w0 = f.dock.floatingWindow (0);
    f.dock.movePanel ("d", w0->area (), f.groupOf ("c"), Z::bottom);
    CHECK (dockTreeText (w0->area ().root.get ()) == "V[(c*)|(d*)]", "dock edge: two groups in the window");
    f.dock.closePanel ("c");
    CHECK (dockTreeText (w0->area ().root.get ()) == "(d*)", "dock edge: one left after closing c");
    f.dock.openPanel ("c");
    CHECK (f.dock.numFloatingWindows () == 1 && dockTreeText (w0->area ().root.get ()) == "V[(c*)|(d*)]",
           "dock edge: reopened above d in the same window ("
               + dockTreeText (w0->area ().root.get ()) + ", "
               + juce::String (f.dock.numFloatingWindows ()) + " windows)");

    // Drop target z-order: a second window over the first one's dock area.
    f.dock.floatPanel ("a", juce::Rectangle<int> (w0->area ().getScreenX () + 60, w0->area ().getScreenY () + 90, 260, 160));
    auto* w1 = f.dock.floatingWindow (1);
    if (w1 != nullptr && w0->isShowing () && w1->isShowing ())
    {
        const auto overlap = w0->area ().getScreenBounds ().getIntersection (w1->area ().getScreenBounds ());
        const auto strip = juce::Point<int> (w1->area ().getScreenBounds ().getCentreX (),
                                             w1->area ().getScreenY () - sc::dock::style::kTitleStripH / 2);
        CHECK (! overlap.isEmpty () && w0->area ().getScreenBounds ().contains (strip),
               "dock edge: the second window's title strip lies over the first's dock area");
        CHECK (f.dock.hitTest (strip, {}).zone == Z::floating,
               "dock edge: a drop on the front window's title strip undocks (not into the window behind)");
        CHECK (f.dock.hitTest (overlap.getCentre (), {}).area == &w1->area (),
               "dock edge: where both overlap, the front window takes the drop");
        w0->toFront (false);
        CHECK (f.dock.hitTest (overlap.getCentre (), {}).area == &w0->area (),
               "dock edge: after raising the other window, it takes the drop");
    }
    else
        scSkip ("dock edge: floating windows are not on screen (z-order checks)");
}

// ---------------------------------------------------------------- resize handles

/** Gap grab zones (the gap and 4 px into each neighbour, cursor = what a
    press does), the floating windows' wider resize edges and their
    minimise / maximise buttons, the menu fonts and the editor splitter's
    hover cursor. */
static void dockTest_resizeHandles()
{
    progress ("docking: resize handles, window buttons, menu fonts");
    namespace st = sc::dock::style;
    using Cur = juce::MouseCursor;
    DockFixture f;
    auto& main = f.dock.mainArea ();
    CHECK (f.load (R"({"main":{"split":"h","children":[{"tabs":["a"]},{"split":"v","children":[{"tabs":["b"]},{"tabs":["c"]}],"weights":[0.5,0.5]}],"weights":[0.5,0.5]},"closed":["d","e","f"]})"),
           "dock grab: layout loaded");
    auto* ga = dockGroupOf (f.dock, "a");
    auto* gb = dockGroupOf (f.dock, "b");
    if (ga == nullptr || gb == nullptr)
    {
        CHECK (false, "dock grab: groups found");
        return;
    }

    // The vertical gap right of a: [x0, x0 + kGap), grabbed kGapGrab px into
    // each neighbour, not further.
    const int x0 = ga->getRight ();
    const int y = gb->getY () + 40;
    CHECK (main.isOverGap ({ x0 + 1, y }), "dock grab: inside the gap");
    CHECK (main.isOverGap ({ x0 - st::kGapGrab, y }) && main.isOverGap ({ x0 + st::kGap + st::kGapGrab - 1, y }),
           "dock grab: 4 px into each neighbour");
    CHECK (! main.isOverGap ({ x0 - st::kGapGrab - 1, y }) && ! main.isOverGap ({ x0 + st::kGap + st::kGapGrab, y }),
           "dock grab: not further");

    // The band goes to the dock area: the group gives it up, the content
    // keeps the rest. (getComponentAt skips invisible components, and the
    // fixture's area is not on screen.)
    main.setVisible (true);
    const juce::Point<int> band (x0 - 2, y), inside (x0 - 8, y);
    CHECK (! ga->hitTest (band.x - ga->getX (), band.y - ga->getY ()) && ga->hitTest (inside.x - ga->getX (), inside.y - ga->getY ()),
           "dock grab: the group gives up only the band");
    CHECK (main.getComponentAt (band) == &main && main.getComponentAt (inside) != &main,
           "dock grab: a press in the band reaches the dock area");

    // The cursor is what a press there does, whatever event came first.
    CHECK (main.cursorTypeAt (band) == Cur::LeftRightResizeCursor && main.cursorTypeAt (inside) == Cur::NormalCursor,
           "dock grab: left-right arrow over the band only");
    const int yb = gb->getBottom ();   // horizontal gap between b and c
    CHECK (main.cursorTypeAt ({ gb->getX () + 30, yb - 3 }) == Cur::UpDownResizeCursor
               && main.cursorTypeAt ({ gb->getX () + 30, yb + st::kGap + 2 }) == Cur::UpDownResizeCursor,
           "dock grab: up-down arrow on both sides of a horizontal gap");

    // Dragging from the band (outside the visible gap) resizes, and the
    // arrow stays until the release.
    const int wA = ga->getWidth ();
    main.mouseDown (mkMouse (main, band.x, band.y, {}));
    main.mouseDrag (mkMouse (main, band.x + 60, band.y, {}));
    CHECK (std::abs (ga->getWidth () - (wA + 60)) <= 1, "dock grab: a drag from the band resizes ("
           + juce::String (wA) + " -> " + juce::String (ga->getWidth ()) + ")");
    CHECK (main.cursorTypeAt ({ 5, 5 }) == Cur::LeftRightResizeCursor, "dock grab: the arrow stays during the drag");
    main.mouseUp (mkMouse (main, band.x + 60, band.y, {}));
    CHECK (main.cursorTypeAt ({ 5, 5 }) == Cur::NormalCursor, "dock grab: normal cursor again after the release");

    // ---- floating window: 8 px resize edges, the look unchanged
    f.dock.floatPanel ("c", juce::Rectangle<int> (220, 240, 320, 220));
    auto* fw = f.dock.floatingWindow (0);
    CHECK (fw != nullptr, "dock window: floating window opened");
    if (fw == nullptr)
        return;
    CHECK (fw->getBorderThickness ().getLeft () == st::kWindowGrab && fw->getContentComponentBorder ().getLeft () == st::kWindowFrame,
           "dock window: 8 px resize zone, 4 px frame");
    {
        const int my = fw->getHeight () / 2;
        auto* edge = fw->getComponentAt (juce::Point<int> (st::kWindowGrab - 2, my));
        auto* content = fw->getComponentAt (juce::Point<int> (st::kWindowGrab + 2, my));
        CHECK (dynamic_cast<juce::ResizableBorderComponent*> (edge) != nullptr,
               "dock window: 6 px inside the edge is the resize border");
        CHECK (content != nullptr && dynamic_cast<juce::ResizableBorderComponent*> (content) == nullptr,
               "dock window: 10 px inside is the content");
        auto* bottom = fw->getComponentAt (juce::Point<int> (fw->getWidth () / 2, fw->getHeight () - 6));
        CHECK (dynamic_cast<juce::ResizableBorderComponent*> (bottom) != nullptr, "dock window: and along the bottom");
    }

    // The title strip's buttons: minimise, maximise, close (18 px each).
    auto* strip = fw->getContentComponent () != nullptr ? fw->getContentComponent ()->getChildComponent (0) : nullptr;
    CHECK (strip != nullptr && strip->getHeight () == st::kTitleStripH, "dock window: title strip found");
    if (strip == nullptr)
        return;
    auto click = [strip] (int fromRight)
    {
        const int x = strip->getWidth () - fromRight * 18 + 9;
        strip->mouseDown (mkMouse (*strip, x, 8, {}));
        strip->mouseUp (mkMouse (*strip, x, 8, {}));
    };
    const auto normal = fw->getBounds ();
    const auto* display = juce::Desktop::getInstance ().getDisplays ().getDisplayForRect (fw->getScreenBounds ());

    click (2);   // maximise
    CHECK (fw->isMaximised () && ! fw->isResizable () && display != nullptr
               && fw->getBounds ().getWidth () >= display->userBounds.toNearestInt ().getWidth () - 2,
           "dock window: Maximise fills the work area, without resize edges");
    {
        const auto saved = f.dock.saveLayout ()["floating"][0];
        const auto area = sc::dock::FloatingWindow::areaBoundsForWindow (normal);
        CHECK (std::abs ((int) saved["w"] - area.getWidth ()) <= 2 && std::abs ((int) saved["h"] - area.getHeight ()) <= 2,
               "dock window: the layout keeps the normal size of a maximised window");
    }
    click (2);   // restore
    CHECK (! fw->isMaximised () && fw->isResizable () && fw->getBounds () == normal, "dock window: Restore brings the bounds back");

    click (3);   // minimise
    CHECK (fw->isCollapsed () && fw->getHeight () == st::kTitleStripH + 2 * st::kWindowFrame && ! fw->area ().isVisible (),
           "dock window: Minimise collapses to the title strip");
    {
        const auto saved = f.dock.saveLayout ()["floating"][0];
        CHECK (std::abs ((int) saved["h"] - sc::dock::FloatingWindow::areaBoundsForWindow (normal).getHeight ()) <= 2,
               "dock window: the layout keeps the normal height of a collapsed window");
    }
    click (3);   // restore
    CHECK (! fw->isCollapsed () && fw->getHeight () == normal.getHeight () && fw->area ().isVisible (),
           "dock window: the same button brings the panels back");

    // Double-click the strip: maximise; dragging it then restores the size.
    strip->mouseDoubleClick (mkMouse (*strip, 30, 8, {}, 2));
    CHECK (fw->isMaximised (), "dock window: double-click on the strip maximises");
    strip->mouseDown (mkMouse (*strip, 40, 8, {}));
    strip->mouseDrag (mkMouse (*strip, 90, 40, {}));
    CHECK (! fw->isMaximised () && fw->getWidth () == normal.getWidth () && fw->getHeight () == normal.getHeight (),
           "dock window: dragging a maximised window restores its size");
    strip->mouseUp (mkMouse (*strip, 90, 40, {}));

    // ---- menu fonts and the editor's splitter
    sc::dock::DockLookAndFeel lf;
    juce::MenuBarComponent bar;
    bar.setSize (600, 24);
    CHECK (std::abs (lf.getPopupMenuFont ().getHeight () - 17.0f) < 0.01f
               && std::abs (lf.getMenuBarFont (bar, 0, "File").getHeight () - 16.8f) < 0.01f,
           "dock fonts: popup menus 17, menu bar 0.7 x its height");
    CHECK (st::kTabTextH == 16.0f && st::kTabStripH == 30, "dock fonts: tab titles 16 in a 30 px strip");

    ViewSplitter hs, vs;
    vs.vertical = true;
    CHECK (hs.getMouseCursor () == Cur (Cur::UpDownResizeCursor) && vs.getMouseCursor () == Cur (Cur::LeftRightResizeCursor),
           "editor splitter: the resize arrow on hover, not only once pressed");
}


// One Editor tab per open document, titled with its file name, in the
// Editor's group; the views follow the active document; a tab click (or a
// click in a tab moved elsewhere) activates its document.
static void dockTest_editorTabsPerDocument()
{
    progress ("docking: one Editor tab per open document, named after its file");
    const juce::File store = makeTempFile ("dock_editor_tabs.recent");
    store.deleteFile ();
    const juce::File fa = makeTempFile ("tabs_first.wav"), fb = makeTempFile ("tabs_second.wav"),
                     fc = makeTempFile ("tabs_third.wav"), fs = makeTempFile ("tabs_saved_as.wav");
    for (const auto& f : { fa, fb, fc })
        writeWav (f, 1, 8000.0, 8000, [] (int, int64_t i, double) { return (float) (0.3 * std::sin ((double) i * 0.1)); });
    fs.deleteFile ();
    {
        Application app (store);
        std::unique_ptr<MainWindow> win (new MainWindow (app, "editor tabs"));
        auto& dock = win->dockManager ();
        auto title = [&] (const juce::String& pid) { const auto* d = dock.panel (pid); return d != nullptr ? d->title : juce::String ("<none>"); };
        auto viewsIn = [&] (const juce::String& pid)
        {
            const auto* d = dock.panel (pid);
            return d != nullptr && win->editorPanelComp ()->getParentComponent () == d->content;
        };
        auto activeTabOfGroup = [&] (const juce::String& pid)
        {
            const auto loc = dock.locate (pid);
            return loc.group != nullptr ? loc.group->activeTab () : juce::String ();
        };
        auto indexOfDoc = [&] (int docId)
        {
            for (int i = 0; i < (int) app.docs.size (); ++i)
                if (app.docs[(size_t) i]->id == docId) return i;
            return -1;
        };

        CHECK (dock.groupTabs ("editor") == juce::StringArray ("editor") && title ("editor") == "Editor"
               && viewsIn ("editor"), "editor tabs: without a file, one tab called Editor");

        CHECK (app.openFile (fa), "editor tabs: first file opens");
        const int idA = app.doc ()->id;
        CHECK (title ("editor") == fa.getFileName () && win->editorTabOfDoc (idA) == "editor",
               "editor tabs: the Editor tab takes the file's name");

        CHECK (app.openFile (fb), "editor tabs: second file opens");
        const int idB = app.doc ()->id;
        const juce::String tabB = win->editorTabOfDoc (idB);
        CHECK (tabB.isNotEmpty () && tabB != "editor"
               && dock.groupTabs ("editor") == juce::StringArray (juce::String ("editor"), tabB),
               "editor tabs: a second file gets its own tab in the same group");
        CHECK (title (tabB) == fb.getFileName (), "editor tabs: ... named after it");
        CHECK (viewsIn (tabB) && activeTabOfGroup ("editor") == tabB,
               "editor tabs: the views show the new file, in its tab");

        // A click on the first tab activates the first file.
        {
            const auto loc = dock.locate ("editor");
            dock.setActiveTab (loc.group->uid, loc.group->tabs.indexOf ("editor"));
        }
        CHECK (app.doc () != nullptr && app.doc ()->id == idA && viewsIn ("editor"),
               "editor tabs: clicking a tab makes its file the active one");

        // Switching files another way (Tables panel, Ctrl+Tab) shows its tab.
        app.activate (indexOfDoc (idB));
        CHECK (activeTabOfGroup ("editor") == tabB && viewsIn (tabB), "editor tabs: switching files shows its tab");

        CHECK (app.openFile (fc), "editor tabs: third file opens");
        const int idC = app.doc ()->id;
        const juce::String tabC = win->editorTabOfDoc (idC);
        CHECK (dock.groupTabs ("editor") == juce::StringArray (juce::String ("editor"), tabB, tabC),
               "editor tabs: three files, three tabs, in opening order");

        // Save As renames the tab.
        if (FileDoc* dc = app.doc ())
        {
            CHECK (app.saveDocument (*dc, fs, true), "editor tabs: save as");
            if (app.onStateChange) app.onStateChange ();
            CHECK (title (tabC) == fs.getFileName (), "editor tabs: Save As renames the tab");
        }

        // Closing a file removes its tab.
        app.activate (indexOfDoc (idB));
        app.closeFile ();
        CHECK (dock.panel (tabB) == nullptr && dock.groupTabs ("editor") == juce::StringArray (juce::String ("editor"), tabC),
               "editor tabs: closing a file removes its tab");

        // Closing the file of the permanent tab: the active file takes it over.
        app.activate (indexOfDoc (idA));
        app.closeFile ();
        const juce::String nowC = win->editorTabOfDoc (idC);
        CHECK (dock.groupTabs ("editor") == juce::StringArray ("editor") && nowC == "editor"
               && title ("editor") == fs.getFileName () && dock.panel (tabC) == nullptr && viewsIn ("editor"),
               "editor tabs: the remaining file moves into the Editor tab");

        app.closeFile ();
        CHECK (app.doc () == nullptr && title ("editor") == "Editor" && viewsIn ("editor"),
               "editor tabs: no file left, the tab is the Editor again");

        // A maximised Editor group (View > Panels off) stays maximised.
        win->setLeftPanelsVisible (false);
        CHECK (app.openFile (fa) && app.openFile (fb), "editor tabs: two files reopen");
        CHECK (! win->areLeftPanelsVisible () && dock.isMaximized (dock.locate ("editor").group->uid)
               && dock.groupTabs ("editor").size () == 2,
               "editor tabs: opening files keeps the Editor maximised");
        win->setLeftPanelsVisible (true);

        // A tab moved to its own group keeps showing its file (issue #9: the
        // picture taken when it was left) until clicked.
        const int idB2 = app.doc ()->id;
        const juce::String tabB2 = win->editorTabOfDoc (idB2);
        app.activate (indexOfDoc (idB2) == 0 ? 1 : 0);   // the other file
        const juce::String tabOther = win->editorTabOfDoc (app.doc ()->id);
        {
            const auto loc = dock.locate ("editor");
            CHECK (dock.movePanel (tabB2, *loc.area, loc.group->uid, sc::dock::Zone::right),
                   "editor tabs: a file tab moves to a group of its own");
        }
        CHECK (dock.locate (tabB2).group != dock.locate ("editor").group, "editor tabs: ... beside the Editor");
        if (const auto* d = dock.panel (tabB2); d != nullptr && d->content != nullptr)
        {
            auto* slot = d->content;
            CHECK (slot->getNumChildComponents () == 0, "editor tabs: a tab whose file is not active holds no views");
            CHECK (win->editorTabHasPicture (tabB2) && ! win->editorTabHasPicture (tabOther),
                   "editor tabs: ... but a picture of them, taken when its file was left (issue #9); the active tab has the live views");
            slot->mouseDown (mkMouse (*slot, 10, 10, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier)));
            CHECK (app.doc () != nullptr && app.doc ()->id == idB2 && viewsIn (tabB2),
                   "editor tabs: a click in it makes its file active, the views move there");
            CHECK (! win->editorTabHasPicture (tabB2) && win->editorTabHasPicture (tabOther),
                   "editor tabs: ... its picture is dropped, and the file left gets one in turn");
        }

        // Switching workspaces keeps the active file's tab in front.
        win->switchWorkspace ("editing");
        CHECK (activeTabOfGroup (win->editorTabOfDoc (app.doc ()->id)) == win->editorTabOfDoc (app.doc ()->id)
               && viewsIn (win->editorTabOfDoc (app.doc ()->id)),
               "editor tabs: after a workspace switch the active file's tab shows the views");
        win->switchWorkspace ("default");
        win.reset ();
    }
    for (const auto& f : { fa, fb, fc, fs })
        f.deleteFile ();
    store.deleteFile ();
}

static void runRegression_docking()
{
    dockTest_zoneGeometry();
    dockTest_treeOperations();
    dockTest_floatingWindows();
    dockTest_floatingEdgeCases();
    dockTest_mouseDrag();
    dockTest_workspaces();
    dockTest_legacyLayout();
    dockTest_editorAndVideo();
    dockTest_resizeHandles();
    dockTest_editorTabsPerDocument();
}
