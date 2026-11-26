// SpectroCraft Mobile - the main screen's geometry and keyboard map, without
// any component, so the unit tests can check them.
//
// The form follows the space the app gets, not the device: a screen whose
// smaller side is at least 600 points (every iPad, Android's "sw600dp"
// tablets) gets the tablet layout, in either orientation. Phones keep the
// phone layout (MainScreen::layoutPhone).
//
// The tablet layout follows the desktop's default workspace: the panels sit in
// two groups with their own tabs, as in the desktop's left column (Files |
// Analyse above, the others below), so the analysis stays in view while the
// generator, the calibration or the edit tools are open.
//
//   landscape                               portrait
//   +----------------------------------+    +---------------------------+
//   | top bar                          |    | top bar                   |
//   +-----------+-+--------------------+    | waveform                  |
//   | tabs 0    | | waveform           |    | spectrogram               |
//   | panel 0   | |                    |    |                           |
//   +-----------+ | spectrogram        |    +---------------------------+
//   | tabs 1    | |                    |    | transport                 |
//   | panel 1   | |                    |    +=========== splitter ======+
//   +-----------+-+--------------------+    | tabs 0      |s| tabs 1    |
//   | transport                        |    | panel 0     |s| panel 1   |
//   +----------------------------------+    +---------------------------+
#pragma once
#include <JuceHeader.h>

namespace scm::screen
{
/** The panels. The numbers are saved in the settings: keep them. */
enum Tab { Files = 0, Analysis, Generator, Calibrate, Input, Edit, Map, NumTabs };
/** Phone: the tab bar, left to right. */
inline constexpr int kPhoneTabOrder[NumTabs] = { Files, Edit, Analysis, Generator, Calibrate, Input, Map };

enum class Form { Phone, TabletLandscape, TabletPortrait };

/** The smaller side of the tablet form (points). */
constexpr int kTabletMinSide = 600;

/** The form for the safe area the screen lays out in. */
Form formFor (juce::Rectangle<int> safeArea);
inline bool isTablet (Form f) noexcept { return f != Form::Phone; }

/** Tablet: the group a panel belongs to (0: Files | Analyse | Map, 1: Edit |
    Generate | Calibrate | Input) and the tabs of a group, in strip order. The
    Map tab, when shown, takes the screen below the top bar (MainScreen). */
int groupOf (int tab);
const std::vector<int>& tabsOfGroup (int group);

/** What the tablet layout remembers (saved in the app's settings). */
struct TabletState
{
    int tab[2] { Analysis, Edit };   // the panel each group shows
    bool panelsVisible = true;       // the panel groups (top bar toggle)
    // Landscape: the column's share of the width and group 0's share of the
    // column. Portrait: the panels' share of the height and group 0's share
    // of the width. Both kept within limits by tabletFrame.
    double landscapeColumn = kDefaultLandscapeColumn, landscapeSplit = kDefaultLandscapeSplit;
    double portraitPanels = kDefaultPortraitPanels, portraitSplit = kDefaultPortraitSplit;

    static constexpr double kDefaultLandscapeColumn = 0.34, kDefaultLandscapeSplit = 0.55;
    static constexpr double kDefaultPortraitPanels = 0.40, kDefaultPortraitSplit = 0.5;
};

/** A tap on a tab: its group shows it, and hidden panels come back. */
void tapTab (TabletState&, int tab);

/** The tablet's areas, in the screen's coordinates. Empty rectangles for what
    is not shown (the panel areas and splitters when the panels are hidden). */
struct TabletFrame
{
    juce::Rectangle<int> topBar, views, transport;
    juce::Rectangle<int> panels;              // both groups and the splitter between them
    juce::Rectangle<int> tabs[2], panel[2];
    juce::Rectangle<int> panelsSplitter;      // between the panels and the views (or transport)
    juce::Rectangle<int> groupSplitter;       // between the two groups
    bool transportSingleRow = false;          // buttons, time and meter on one row
};

constexpr int kSplitter = 14;                 // splitter thickness (a finger can take it)
constexpr int kMinPanelWidth = 300;           // a group (the phone panels are this wide)
constexpr int kMinPanelHeight = 150;          // a group's panel, under its tabs
constexpr int kMinViewsWidth = 400;
constexpr int kMinViewsHeight = 260;

/** Transport row heights: one row (buttons, time, meter) or the phone's two. */
int transportHeight (bool singleRow);

TabletFrame tabletFrame (juce::Rectangle<int> safeArea, Form form, const TabletState&);

/** Turns a splitter drag into the state's fraction. `pos` is the splitter's
    new centre (x for a vertical bar, y for a horizontal one) in the screen's
    coordinates; the result is limited the way tabletFrame limits it. */
void dragPanelsSplitter (TabletState&, juce::Rectangle<int> safeArea, Form form, int pos);
void dragGroupSplitter (TabletState&, juce::Rectangle<int> safeArea, Form form, int pos);

// -------------------------------------------------------------------- keyboard
// A hardware keyboard (iPad keyboard, Android tablet keyboard) gets the
// desktop's shortcuts for what the app can do (src/MainWindow.cpp,
// mainWindowKeys). Cmd on iPadOS, Ctrl on Android (juce::ModifierKeys::commandModifier).
enum class Command
{
    None, PlayPause, Stop, Record, GoToStart, GoToEnd, SelectAll, ClearSelection,
    Delete, Undo, Redo, Save, ZoomIn, ZoomOut, ZoomAll, TogglePanels
};
Command commandForKey (const juce::KeyPress&);
} // namespace scm::screen
