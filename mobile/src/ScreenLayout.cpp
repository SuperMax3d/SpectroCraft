#include "ScreenLayout.h"
#include "MobileStyle.h"

namespace scm::screen
{
Form formFor (juce::Rectangle<int> safeArea)
{
    if (std::min (safeArea.getWidth(), safeArea.getHeight()) < kTabletMinSide)
        return Form::Phone;
    return safeArea.getWidth() > safeArea.getHeight() ? Form::TabletLandscape : Form::TabletPortrait;
}

int groupOf (int tab)
{
    return tab == Files || tab == Analysis ? 0 : 1;
}

const std::vector<int>& tabsOfGroup (int group)
{
    static const std::vector<int> upper { Files, Analysis };
    static const std::vector<int> lower { Edit, Generator, Calibrate, Input };
    return group == 0 ? upper : lower;
}

void tapTab (TabletState& s, int tab)
{
    if (! juce::isPositiveAndBelow (tab, (int) NumTabs)) return;
    s.tab[groupOf (tab)] = tab;
    s.panelsVisible = true;
}

int transportHeight (bool singleRow)
{
    return singleRow ? style::kTouch + 8 : style::kTouch + 34;
}

namespace
{
/** One row holds REC, Play, Stop, the mode, Video, the time and the meter. */
constexpr int kSingleRowWidth = 880;

/** `desired`, kept within the room `avail` shared with a neighbour: at least
    `minThis`, and at least `minOther` left for the neighbour (both minimums
    shrink alike when they cannot both fit). */
int fit (double desired, int avail, int minThis, int minOther)
{
    if (minThis + minOther > avail)
    {
        const double k = (double) std::max (0, avail) / (double) std::max (1, minThis + minOther);
        minThis = (int) (minThis * k);
        minOther = (int) (minOther * k);
    }
    return juce::jlimit (minThis, std::max (minThis, avail - minOther), juce::roundToInt (desired));
}
} // namespace

TabletFrame tabletFrame (juce::Rectangle<int> safeArea, Form form, const TabletState& s)
{
    TabletFrame f;
    auto r = safeArea;
    f.topBar = r.removeFromTop (style::kTouch);
    f.transportSingleRow = r.getWidth() >= kSingleRowWidth;
    const int transportH = transportHeight (f.transportSingleRow);

    if (form == Form::TabletLandscape)
    {
        // transport along the bottom, as the desktop's status bar
        f.transport = r.removeFromBottom (transportH);
        if (s.panelsVisible)
        {
            const int avail = r.getWidth() - kSplitter;
            const int col = fit (s.landscapeColumn * avail, avail, kMinPanelWidth, kMinViewsWidth);
            f.panels = r.removeFromLeft (col);
            f.panelsSplitter = r.removeFromLeft (kSplitter);
            auto p = f.panels;
            const int availH = p.getHeight() - 2 * style::kTouch - kSplitter;
            const int h0 = fit (s.landscapeSplit * availH, availH, kMinPanelHeight, kMinPanelHeight);
            f.tabs[0] = p.removeFromTop (style::kTouch);
            f.panel[0] = p.removeFromTop (h0);
            f.groupSplitter = p.removeFromTop (kSplitter);
            f.tabs[1] = p.removeFromTop (style::kTouch);
            f.panel[1] = p;
        }
        f.views = r;
    }
    else
    {
        if (s.panelsVisible)
        {
            const int avail = r.getHeight() - transportH - kSplitter;
            const int panelsH = fit (s.portraitPanels * r.getHeight(), avail,
                                     style::kTouch + kMinPanelHeight, kMinViewsHeight);
            f.panels = r.removeFromBottom (panelsH);
            f.panelsSplitter = r.removeFromBottom (kSplitter);
            auto p = f.panels;
            const int availW = p.getWidth() - kSplitter;
            const int w0 = fit (s.portraitSplit * availW, availW, kMinPanelWidth, kMinPanelWidth);
            auto left = p.removeFromLeft (w0);
            f.groupSplitter = p.removeFromLeft (kSplitter);
            auto right = p;
            f.tabs[0] = left.removeFromTop (style::kTouch);
            f.panel[0] = left;
            f.tabs[1] = right.removeFromTop (style::kTouch);
            f.panel[1] = right;
        }
        f.transport = r.removeFromBottom (transportH);
        f.views = r;
    }
    return f;
}

void dragPanelsSplitter (TabletState& s, juce::Rectangle<int> safeArea, Form form, int pos)
{
    if (! isTablet (form) || ! s.panelsVisible) return;
    const auto now = tabletFrame (safeArea, form, s);
    const int half = kSplitter / 2;
    if (form == Form::TabletLandscape)
    {
        const int avail = now.panels.getWidth() + now.panelsSplitter.getWidth() + now.views.getWidth() - kSplitter;
        s.landscapeColumn = (double) (pos - half - now.panels.getX()) / std::max (1, avail);
        s.landscapeColumn = (double) tabletFrame (safeArea, form, s).panels.getWidth() / std::max (1, avail);
    }
    else
    {
        const int below = safeArea.getHeight() - style::kTouch;   // under the top bar
        s.portraitPanels = (double) (now.panels.getBottom() - (pos + half)) / std::max (1, below);
        s.portraitPanels = (double) tabletFrame (safeArea, form, s).panels.getHeight() / std::max (1, below);
    }
}

void dragGroupSplitter (TabletState& s, juce::Rectangle<int> safeArea, Form form, int pos)
{
    if (! isTablet (form) || ! s.panelsVisible) return;
    const auto now = tabletFrame (safeArea, form, s);
    const int half = kSplitter / 2;
    if (form == Form::TabletLandscape)
    {
        const int avail = now.panels.getHeight() - 2 * style::kTouch - kSplitter;
        s.landscapeSplit = (double) (pos - half - now.panel[0].getY()) / std::max (1, avail);
        s.landscapeSplit = (double) tabletFrame (safeArea, form, s).panel[0].getHeight() / std::max (1, avail);
    }
    else
    {
        const int avail = now.panels.getWidth() - kSplitter;
        s.portraitSplit = (double) (pos - half - now.panels.getX()) / std::max (1, avail);
        s.portraitSplit = (double) tabletFrame (safeArea, form, s).panel[0].getWidth() / std::max (1, avail);
    }
}

Command commandForKey (const juce::KeyPress& key)
{
    using MK = juce::ModifierKeys;
    const int mods = key.getModifiers().getRawFlags()
                   & (MK::shiftModifier | MK::ctrlModifier | MK::altModifier | MK::commandModifier);
    int code = key.getKeyCode();
    if (code >= 'A' && code <= 'Z') code += 'a' - 'A';
    const int cmd = MK::commandModifier, shift = MK::shiftModifier;

    if (mods == 0)
    {
        if (code == juce::KeyPress::spaceKey)     return Command::PlayPause;
        if (code == juce::KeyPress::homeKey)      return Command::GoToStart;
        if (code == juce::KeyPress::endKey)       return Command::GoToEnd;
        if (code == juce::KeyPress::escapeKey)    return Command::ClearSelection;
        if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) return Command::Delete;
    }
    if (mods == shift && code == juce::KeyPress::F10Key) return Command::Record;
    if (mods == cmd)
    {
        if (code == '.') return Command::Stop;
        if (code == 'a') return Command::SelectAll;
        if (code == 'z') return Command::Undo;
        if (code == 'y') return Command::Redo;
        if (code == 's') return Command::Save;
        if (code == '=') return Command::ZoomIn;
        if (code == '-') return Command::ZoomOut;
        if (code == '0') return Command::ZoomAll;
    }
    if (mods == (cmd | shift))
    {
        if (code == 'z') return Command::Redo;
        if (code == juce::KeyPress::returnKey) return Command::TogglePanels;
    }
    return Command::None;
}
} // namespace scm::screen
