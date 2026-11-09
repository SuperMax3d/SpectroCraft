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
            const auto tl = dock.locate ("files");
            dock.movePanel ("markers", *tl.area, tl.group->uid, Z::tabStrip, 0);
        }
        const auto modified = win->panelGroupTitles ("files");
        win->switchWorkspace ("analysis");
        {
            auto* fftG = dockGroupOf (dock, "fft");
            auto* edG = dockGroupOf (dock, "editor");
            CHECK (ws.activeId () == "analysis" && win->panelGroupTitles ("fft") == juce::StringArray ("Frequency Analysis")
                       && fftG != nullptr && edG != nullptr && fftG->getX () >= edG->getRight (),
                   "workspaces: Analysis puts Frequency Analysis beside the Editor");
        }
        win->switchWorkspace ("default");
        CHECK (win->panelGroupTitles ("files") == modified, "workspaces: a workspace keeps its modifications");
        win->resetWorkspace ();
        CHECK (win->panelGroupTitles ("controls").contains ("Markers")
                   && ! win->panelGroupTitles ("files").contains ("Markers"),
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
        pf.setValue ("left_tab_layout", "fft;files|controls;markers");
        pf.setValue ("left_panels_closed", "history;drawing");
        pf.setValue ("left_panel_w", 560);
        pf.setValue ("top_ratio_mil", 300);
        pf.save ();
    }

    Application app (store);
    std::unique_ptr<MainWindow> win (new MainWindow (app, "legacy layout test"));
    auto& dock = win->dockManager ();
    const auto top = win->panelGroupTitles ("fft");
    CHECK (top.size () >= 2 && top[0] == "Frequency Analysis" && top[1] == "Files",
           "legacy: the top group keeps its tab order");
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

    // The video menus' Dock / Undock Panel item follows the picture.
    if (! sc::videoDecodingAvailable ())
        scSkip ("dock editor: no video decoder in this build (video panel labels)");
    else
    {
        win->setVideoDocked (true);
        auto* vw = win->getVideoWindow ();
        CHECK (vw != nullptr && vw->view ().docked, "dock video: in the main window the menu offers Undock Panel");
        if (vw != nullptr && vw->view ().onDockToggle)
        {
            vw->view ().onDockToggle ();   // Undock Panel
            CHECK (dock.isFloating ("video") && ! vw->view ().docked
                       && vw->view ().getParentComponent () == win->getVideoDockHost (),
                   "dock video: Undock Panel floats the panel, the menu then offers Dock Panel");
            vw->view ().onDockToggle ();   // Dock Panel
            dock.flushRetired ();
            CHECK (win->isPanelOpen ("video") && ! dock.isFloating ("video") && vw->view ().docked,
                   "dock video: Dock Panel brings it back to the main window");
        }
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
}
