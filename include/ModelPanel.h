/*
 * ModelPanel.h - the Model panel (issue #40): learns a sound model of the
 * active file - harmonic and noise components in an order, each one taken
 * out of the ones below - and renders it (Compare, Render Selection, Render
 * CSV) or exports it as JSON.
 *
 * The model lives in the file (FileDoc::soundModel); the actions are
 * Application's (src/ApplicationModel.cpp); the analysis and the synthesis
 * are in include/SoundModel.h. Behaviour: docs/model-panel.md.
 */
#pragma once

#include <JuceHeader.h>
#include "SoundModel.h"

class Application;

class ModelPanel : public juce::Component,
                   private juce::TableListBoxModel
{
public:
    explicit ModelPanel (Application& app);
    ~ModelPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Re-reads the active file's model (call after a file switch, a model
        change, new tables or stored selections). */
    void refresh();

    // Right-click "Close Panel": set by MainWindow.
    std::function<void()> onCloseRequested;
    void requestClose() { if (onCloseRequested) onCloseRequested(); }

    /** Set by MainWindow: Render Selection goes through its destructive-edit
        path (streamed files, view refresh). Unset (tests): the app directly. */
    std::function<void()> onRenderSelectionRequested;

    // ---- for MainWindow and the unit tests ----
    void addComponent (sc::model::Kind kind);
    int selectedIndex() const;
    void selectIndex (int index);
    void moveSelected (int delta);
    void deleteSelected();
    void analyseSelected();
    void analyseAll();
    void compare();
    void renderSelection();
    juce::String statusText() const { return status.getText(); }
    /** Applies the text of the detail editors (as Return / focus loss do). */
    void commitEdits();
    /** Adds a track made of these stored selections to the selected harmonic component. */
    void addTrack (const std::vector<juce::String>& selections);
    juce::TableListBox& list() { return table; }

private:
    // TableListBoxModel: the component list.
    int getNumRows() override;
    void paintRowBackground (juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell (juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    void cellClicked (int row, int col, const juce::MouseEvent&) override;
    void selectedRowsChanged (int lastRow) override;
    void backgroundClicked (const juce::MouseEvent&) override;

    sc::model::Model* model() const;
    sc::model::Component* current() const;
    void showStatus (const juce::String& text, bool error = false);
    void loadDetail();
    void layoutDetail();
    void changed (bool invalidates);   // a detail edit: invalidates = needs a new analysis
    void showRowMenu (int row);
    void showTrackMenu (bool remove);
    void togglePick (int mode);
    void chooseFile (bool save, const juce::String& title, const juce::String& patterns,
                     std::function<void (const juce::File&)> then);

    Application& app;
    juce::String selectedId_;
    bool loading_ = false;

    juce::TextButton addHarmBtn { "+ Harmonic" }, addNoiseBtn { "+ Noise" }, analyseAllBtn { "Analyse All" },
                     exportBtn { "Export..." }, importBtn { "Import..." },
                     compareBtn { "Compare" }, renderSelBtn { "Render Selection" }, renderCsvBtn { "Render CSV..." };
    juce::TableListBox table { "model-components", this };

    // Detail of the selected component, in a viewport.
    class Ranges;
    class Plot;
    juce::Viewport viewport;
    juce::Component content;
    juce::Label nameLab, paramLab, channelLab, rangesLab, f0Lab, f0AtLab, ordersLab, tracksLab, settingsLab;
    juce::TextEditor nameEd, f0Ed, f0AtEd, ordersEd, tolEd, steadyEd;
    juce::ComboBox paramCombo, channelCombo, fftCombo;
    std::unique_ptr<juce::ListBox> rangesList;
    std::unique_ptr<Ranges> rangesModel;
    juce::TextButton addRangeBtn { "Add Selection" }, removeRangeBtn { "Remove" },
                     pickF0Btn { "Pick" }, pickHarmBtn { "Pick Harmonics" },
                     addTrackBtn { "Add Track..." }, removeTrackBtn { "Remove Track..." },
                     analyseBtn { "Analyse" };
    juce::ToggleButton dynamicsTgl { "Rising / falling levels" }, variationTgl { "Reproduce variation" };
    juce::Label tracksText, report;
    std::unique_ptr<Plot> plot;
    juce::Label status;

    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ModelPanel)
};
