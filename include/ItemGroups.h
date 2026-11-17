// SpectroCraft - groups of list items (Drawing panel, Selection panel).
#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <vector>

/** One group of the Drawing panel or the Selection panel (issues #39, #41):
    its name (renamable; empty = the default "Group N") and whether its
    members are folded away under its row in the panel's list. The members
    themselves carry the group id (DrawingItem::group,
    SavedSpectralSelection::group); this only keeps what belongs to the
    group itself. Saved with the file's metadata next to the items. */
struct ItemGroupInfo
{
    int id = 0;
    juce::String name;        // empty = "Group <id>"
    bool collapsed = false;   // members hidden in the panel list (not in the views)

    bool operator== (const ItemGroupInfo& o) const
    {
        return id == o.id && name == o.name && collapsed == o.collapsed;
    }
};

/** The groups of one list (a document's drawings, or its stored selections). */
struct ItemGroupList
{
    std::vector<ItemGroupInfo> list;

    bool empty() const noexcept { return list.empty(); }

    const ItemGroupInfo* find (int id) const
    {
        for (const auto& g : list)
            if (g.id == id) return &g;
        return nullptr;
    }

    static juce::String defaultName (int id) { return "Group " + juce::String (id); }

    /** The group's name: its own, or "Group N". */
    juce::String nameOf (int id) const
    {
        const auto* g = find (id);
        return g != nullptr && g->name.trim().isNotEmpty() ? g->name : defaultName (id);
    }

    bool isCollapsed (int id) const
    {
        const auto* g = find (id);
        return g != nullptr && g->collapsed;
    }

    ItemGroupInfo& entry (int id)
    {
        for (auto& g : list)
            if (g.id == id) return g;
        list.push_back ({ id, {}, false });
        return list.back();
    }

    /** Renames group `id`; an empty name (or the default one) goes back to
        "Group N". */
    void setName (int id, const juce::String& name)
    {
        const auto n = name.trim();
        entry (id).name = (n == defaultName (id)) ? juce::String() : n;
    }

    void setCollapsed (int id, bool c) { entry (id).collapsed = c; }

    /** Keeps only the groups in `liveIds` that hold something worth saving
        (a name or a folded state). */
    ItemGroupList liveSubset (const std::vector<int>& liveIds) const
    {
        ItemGroupList out;
        for (const auto& g : list)
            if (std::find (liveIds.begin(), liveIds.end(), g.id) != liveIds.end()
                && (g.name.isNotEmpty() || g.collapsed))
                out.list.push_back (g);
        return out;
    }

    /** JSON for the file's metadata: [{"id", "name", "collapsed"}, ...]. */
    juce::var toVar() const
    {
        juce::Array<juce::var> arr;
        for (const auto& g : list)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("id", g.id);
            if (g.name.isNotEmpty()) o->setProperty ("name", g.name);
            if (g.collapsed)         o->setProperty ("collapsed", 1);
            arr.add (juce::var (o));
        }
        return arr;
    }

    static ItemGroupList fromVar (const juce::var& v)
    {
        ItemGroupList out;
        if (const auto* arr = v.getArray())
            for (const auto& e : *arr)
                if (const auto* o = e.getDynamicObject())
                {
                    const auto& p = o->getProperties();
                    const int id = static_cast<int> (p.getWithDefault ("id", 0));
                    if (id <= 0 || out.find (id) != nullptr) continue;
                    ItemGroupInfo g;
                    g.id = id;
                    g.name = p.getWithDefault ("name", juce::var()).toString().trim();
                    g.collapsed = static_cast<int> (p.getWithDefault ("collapsed", 0)) != 0;
                    out.list.push_back (g);
                }
        return out;
    }
};

namespace sc::groups
{
/** One row of a grouped list: an item (item >= 0, group = its group or 0), or
    a group's own row (item = -1) followed by its members unless the group is
    collapsed. */
struct Row { int item = -1; int group = 0; };

/** The rows of a list whose items belong to the groups `groupOfItem` (0 = in
    no group), in item order: a group shows up where its first member is. */
inline std::vector<Row> buildRows (const std::vector<int>& groupOfItem, const ItemGroupList& groups)
{
    std::vector<Row> rows;
    std::vector<int> shown;
    for (int i = 0; i < (int) groupOfItem.size(); ++i)
    {
        const int g = groupOfItem[(size_t) i];
        if (g == 0) { rows.push_back ({ i, 0 }); continue; }
        if (std::find (shown.begin(), shown.end(), g) != shown.end()) continue;
        shown.push_back (g);
        rows.push_back ({ -1, g });
        if (groups.isCollapsed (g)) continue;
        for (int j = i; j < (int) groupOfItem.size(); ++j)
            if (groupOfItem[(size_t) j] == g) rows.push_back ({ j, g });
    }
    return rows;
}

/** Items of group g, in item order. */
inline std::vector<int> membersOf (const std::vector<int>& groupOfItem, int g)
{
    std::vector<int> out;
    if (g == 0) return out;
    for (int i = 0; i < (int) groupOfItem.size(); ++i)
        if (groupOfItem[(size_t) i] == g) out.push_back (i);
    return out;
}

/** The distinct group ids in use. */
inline std::vector<int> liveIds (const std::vector<int>& groupOfItem)
{
    std::vector<int> out;
    for (int g : groupOfItem)
        if (g != 0 && std::find (out.begin(), out.end(), g) == out.end()) out.push_back (g);
    return out;
}

/** The items the rows stand for (a group row = all its members, folded or
    not), sorted, without duplicates. */
inline std::vector<int> itemsOfRows (const std::vector<Row>& rows, const std::vector<int>& groupOfItem,
                                     const std::vector<int>& rowIndices)
{
    std::vector<int> out;
    for (int r : rowIndices)
    {
        if (r < 0 || r >= (int) rows.size()) continue;
        const auto& row = rows[(size_t) r];
        if (row.item >= 0) out.push_back (row.item);
        else
            for (int m : membersOf (groupOfItem, row.group)) out.push_back (m);
    }
    std::sort (out.begin(), out.end());
    out.erase (std::unique (out.begin(), out.end()), out.end());
    return out;
}

/** The rows showing `items`: an item's row, and a group's row when every
    member of the group is in `items` (the only row left for a folded group). */
inline juce::SparseSet<int> rowsForItems (const std::vector<Row>& rows, const std::vector<int>& groupOfItem,
                                          const std::vector<int>& items)
{
    juce::SparseSet<int> sel;
    auto has = [&items] (int i) { return std::find (items.begin(), items.end(), i) != items.end(); };
    for (int r = 0; r < (int) rows.size(); ++r)
    {
        const auto& row = rows[(size_t) r];
        bool on = false;
        if (row.item >= 0) on = has (row.item);
        else
        {
            const auto members = membersOf (groupOfItem, row.group);
            on = ! members.empty() && std::all_of (members.begin(), members.end(), has);
        }
        if (on) sel.addRange ({ r, r + 1 });
    }
    return sel;
}

/** Width of the fold arrow at the left of a group's name cell. */
constexpr int kDisclosureW = 16;

/** The fold arrow: pointing right when the group is collapsed, down when
    its members show. */
inline void drawDisclosure (juce::Graphics& g, juce::Rectangle<float> r, bool collapsed)
{
    const auto c = r.getCentre();
    const float s = std::min (r.getWidth(), r.getHeight()) * 0.28f;
    juce::Path p;
    if (collapsed)
        p.addTriangle (c.x - s * 0.6f, c.y - s, c.x - s * 0.6f, c.y + s, c.x + s, c.y);
    else
        p.addTriangle (c.x - s, c.y - s * 0.6f, c.x + s, c.y - s * 0.6f, c.x, c.y + s);
    g.setColour (juce::Colours::lightgrey);
    g.fillPath (p);
}
} // namespace sc::groups
