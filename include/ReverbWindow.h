#pragma once
// SpectroCraft - Effects > Reverberation Analysis...: the reverberation of the
// place a recording was made in (sc::room, include/RoomAcoustics.h). The
// recording holds either a sweep played by a loudspeaker and the silence after
// it (the sweep's range and length are asked, or read from a sound map take's
// metadata) or an impulse (balloon, clapper). Per octave or 1/3 octave: EDT,
// T20, T30, C50, C80, D50, centre time and the response above the noise, with
// what is not reliable marked; the decay curve of the chosen band; a CSV.
// It reads the file and changes nothing. Behaviour: docs/reverberation.md.
#include <JuceHeader.h>
#include "RoomAcoustics.h"
#include <memory>
#include <vector>

class Application;

/** The window's content, on its own so the tests can drive it. */
class ReverbView : public juce::Component, private juce::TableListBoxModel
{
public:
    explicit ReverbView (Application& app);
    ~ReverbView() override;

    /** Re-reads the active document: channels, range, and the sweep a sound map take says it holds. */
    void refresh();
    /** Analyses the selection (or the whole file) of the chosen channel as set. False with the status saying why. */
    bool analyse();

    enum class Method { Sweep = 1, Impulse = 2 };
    void setMethod (Method m);
    Method method() const;
    void setSweep (const sc::room::SweepSpec&);
    sc::room::SweepSpec sweep() const;
    void setThirdOctaves (bool thirds);

    const std::vector<sc::room::BandResult>& results() const noexcept { return bands_; }
    const sc::room::ImpulseResponse& response() const noexcept { return response_; }
    juce::String statusText() const { return status_.getText(); }
    juce::String rangeText() const { return range_.getText(); }
    /** The table's text for a band and a column (1 Band, 2 EDT, 3 T20, 4 T30, 5 C50, 6 C80, 7 D50, 8 Ts, 9 INR, 10 Notes). */
    juce::String cellText (int row, int column) const;
    juce::String csv() const { return sc::room::toCsv (bands_); }

    std::function<void()> onClose;
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    class DecayPlot;
    int getNumRows() override { return (int) bands_.size(); }
    void paintRowBackground (juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int width, int height, bool selected) override;
    void selectedRowsChanged (int lastRow) override;
    void updateFields();
    void exportCsv();

    Application& app;
    juce::ComboBox method_, bandsBox_, channel_;
    juce::Label methodLabel_ { {}, "Recording" }, bandsLabel_ { {}, "Bands" }, channelLabel_ { {}, "Channel" },
                sweepLabel_ { {}, "Sweep: from (Hz), to (Hz), length (s)" };
    juce::TextEditor startHz_, endHz_, seconds_;
    juce::Label range_, status_;
    juce::TextButton analyse_ { "Analyse" }, export_ { "Export CSV..." }, close_ { "Close" };
    juce::TableListBox table_;
    std::unique_ptr<DecayPlot> plot_;
    std::unique_ptr<juce::FileChooser> chooser_;

    sc::room::ImpulseResponse response_;
    std::vector<sc::room::BandResult> bands_;
};

class ReverbWindow : public juce::DocumentWindow
{
public:
    explicit ReverbWindow (Application& app);
    ~ReverbWindow() override;
    void closeButtonPressed() override;
    /** Set by MainWindow to release the window (closed by X or Close). */
    std::function<void()> onCloseRequested;
    ReverbView& view() noexcept { return *view_; }

private:
    std::unique_ptr<ReverbView> view_;
};
