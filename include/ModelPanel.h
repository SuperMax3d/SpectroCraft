#pragma once
// SpectroCraft - Model panel (left column, hidden by default).
//
// Builds parametric sound-source models from measurements of one or more
// files: named components (Harmonic / Noise) with time-varying inputs that
// are expressions over the Tables panel's parameters, and a table of cells
// laid out on X/Y breakpoints. The session is saved as its own versioned
// .json file (New / Open / Save / Save As), separate from per-file metadata.
//
// This step provides the UI: session toolbar, component list (add/remove/
// rename/reorder), per-component editor (type/comment/inputs with expression
// and curve editing) and table structure + structural cell dialogs. Picking
// mode, local spectrum analysis, candidate evaluation/subtraction graphs,
// render simulation and export come in later steps.

#include <JuceHeader.h>
#include "Application.h"
#include "ModelSession.h"

#include <functional>
#include <memory>
#include <vector>

/** Graphical editor for a RemapCurve: control points on an adjustable x/y
    range. Same interaction model as EnvCurveEditor: drag a point to move it,
    click empty space to add one, double-click a point to type its exact
    coordinates (onEditPoint), right-click a point to remove it. */
class ModelCurveEditor : public juce::Component
{
public:
    ModelCurveEditor();

    /** Sets the visible axis ranges; non-finite or degenerate (hi <= lo)
        values are ignored so an editor never ends up with a zero span. */
    void setRanges (double xLo, double xHi, double yLo, double yHi);
    double getRangeXLo() const { return xLo_; }
    double getRangeXHi() const { return xHi_; }
    double getRangeYLo() const { return yLo_; }
    double getRangeYHi() const { return yHi_; }

    /** Replaces the control points (kept sorted by ascending x). */
    void setPoints (std::vector<sc::model::CurvePoint> pts);

    /** The current control points, normalised: sorted by x with same-x
        duplicates merged. */
    std::vector<sc::model::CurvePoint> getPoints() const;

    /** Sets point `index`'s coordinates (clamped to the visible ranges). */
    void setPoint (int index, double x, double y);

    /** Called when a point is double-clicked, with its index in getPoints()
        order. If unset, double-clicking does nothing. */
    std::function<void(int)> onEditPoint;

    // Axis mapping and mouse entry points are public so tests can drive the
    // editor headlessly (compute a point's pixel position, click it).
    int valueToX (double v) const;
    double xToValue (int x) const;
    int valueToY (double v) const;
    double yToValue (int y) const;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    int findNearestPoint (const juce::Point<int>& p, int maxDistPx) const;
    void paint (juce::Graphics& g) override;

    std::vector<sc::model::CurvePoint> points_;   // sorted by x
    double xLo_ = 0.0, xHi_ = 1.0, yLo_ = 0.0, yHi_ = 1.0;
    int dragIndex_ = -1;

    static constexpr int kLeftMargin = 46, kBottomMargin = 20, kTopMargin = 8,
                        kRightMargin = 8;

    JUCE_DECLARE_NON_COPYABLE (ModelCurveEditor)
};

/** The cell table of the selected component: Y breakpoints descending
    top-to-bottom on the left, X breakpoints ascending left-to-right at the
    bottom. Storage order is axis order (row 0 = lowest Y band); the display
    flips rows so the highest values sit on top. Clicking a cell fires
    onEditCell with DISPLAY coordinates (row 0 = top). */
class ModelTableGrid : public juce::Component
{
public:
    sc::model::Component* comp = nullptr;   // non-owning pointer to the edited component

    /** Fired when the user clicks a cell, in display coordinates. */
    std::function<void(int dispRow, int dispCol)> onEditCell;

    void setComponent (sc::model::Component* c) { comp = c; repaint (); }
    sc::model::Component* getComponent() const { return comp; }

private:
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
};

/** The Model panel. Hidden by default: MainWindow only opens it when the
    saved left-column layout explicitly lists it, and the Window menu offers
    the same toggle as every other panel. */
class ModelPanel : public juce::Component
{
public:
    explicit ModelPanel (Application& app);

    std::function<void()> onCloseRequested;
    void requestClose() { if (onCloseRequested) onCloseRequested(); }

    /** The shared session owned by the Application (never null). */
    sc::model::ModelSession* session() const { return app.modelSession.get (); }

    // ---- state queries (MainWindow + tests) ----
    int numComponentsShown() const;
    juce::String componentShownName (int index) const;
    int selectedComponentIndex() const;

    /** Rebuilds the list, editor and status line from the current session. */
    void refresh();

    /** The component list box (exposed for unit tests). */
    juce::ListBox& componentList() { return compList; }

    // ---- session operations (the toolbar buttons wrap these with choosers) ----
    bool newSession (bool skipConfirm = false);
    bool openSessionFile (const juce::File& f, bool skipConfirm = false);
    bool saveSessionFile (const juce::File& target);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    // ---- component list model -------------------------------------------
    class ListModel : public juce::ListBoxModel
    {
    public:
        explicit ListModel (ModelPanel* p) : panel (p) {}
        int getNumRows() override
        { return panel->session () ? (int) panel->session ()->components.size () : 0; }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override;
        void listBoxItemClicked (int row, const juce::MouseEvent& e) override;

    private:
        ModelPanel* panel;
    };

    // ---- helpers ---------------------------------------------------------
    sc::model::ModelInput* inputForKind (sc::model::Component& c, int kind) const;
    int kindForEditor (juce::TextEditor* ed) const;
    void rebuildInputRows();
    void updateEditor();
    bool confirmDiscard();
    void syncSettingsFields();
    void onInputExprChanged (juce::TextEditor*);
    void onInputCurveClicked (juce::TextEditor*);
    void validateExprColour (juce::TextEditor*, const sc::model::Component&);

    // ---- widgets ----------------------------------------------------------
    Application& app;

    juce::TextButton btnNew, btnOpen, btnSave, btnSaveAs, btnAddComp;
    juce::Label sessionNameLbl;

    juce::Label matchPctLbl, inclDbLbl, overDbLbl;
    juce::TextEditor matchPctEd, inclDbEd, overDbEd;

    juce::Label listHdr, inputsHdr, tableHdr;

    ListModel listModel { this };
    juce::ListBox compList { "Components", &listModel };

    juce::Label nameLbl, typeLbl, commentLbl;
    juce::TextEditor nameEd, commentEd;
    juce::ComboBox typeCombo;

    std::vector<std::unique_ptr<juce::Label>> inputLabels_;
    std::vector<std::unique_ptr<juce::TextEditor>> inputExprs_;
    std::vector<std::unique_ptr<juce::TextButton>> inputCurveBtns_;
    juce::TextButton btnAddAmp, btnRemoveAmp;

    juce::Label xBreaksLbl, yBreaksLbl;
    juce::TextEditor xBreaksEd, yBreaksEd;
    juce::TextButton btnAddCol, btnDelCol, btnAddRow, btnDelRow;

    ModelTableGrid grid;

    int selectedComp_ = -1;

    JUCE_DECLARE_NON_COPYABLE (ModelPanel)
};
