#pragma once
// SpectroCraft - Effects > Blind Source Separation... (issue #68): the method
// (with its description, pros and cons), the settings that method uses, the
// range and the output; the separation runs on a worker thread with a
// cancellable progress dialog (sc::bss::separate).
#include <JuceHeader.h>
#include "BlindSeparation.h"
#include <atomic>
#include <memory>

class Application;

/** The window's content, on its own so the tests can drive it. */
class BlindSeparationView : public juce::Component
{
public:
    explicit BlindSeparationView (Application& app);
    /** The settings as chosen (sources, FFT size, iterations...). */
    sc::bss::Params params() const;
    /** 1 = a new file, 2 = replace the current file, 3 = add as new tracks
        (Application::applyStemSeparationResult's modes). */
    int outputMode() const;
    /** Picks a method: its defaults and its own settings appear. */
    void setMethod (sc::bss::Method m);
    sc::bss::Method method() const;
    /** Whether a setting row is shown ("sources", "fft", "iterations",
        "bases", "ref", "noise", "extract", "block"). */
    bool isRowShown (const juce::String& id) const;
    juce::String descriptionText() const { return desc_.getText(); }
    juce::String rangeText() const { return range_.getText(); }
    void setStatus (const juce::String& s) { status_.setText (s, juce::dontSendNotification); }
    juce::String statusText() const { return status_.getText(); }
    /** Re-reads the active document (channels, selection, rate). */
    void refresh();
    /** Saves the settings (key "bss_params"). */
    void remember() const;

    std::function<void()> onSeparate, onClose;
    juce::Button& separateButton() noexcept { return separate_; }
    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    Application& app;
    juce::ComboBox method_, sources_, fft_, ref_, extract_, output_;
    juce::Slider iterations_, bases_, block_;
    juce::ToggleButton noise_ { "Noise class (diffuse noise is not output)" };
    juce::TextEditor desc_;
    juce::Label range_, status_;
    juce::TextButton separate_ { "Separate" }, close_ { "Close" };
    struct Row { juce::String id; std::unique_ptr<juce::Label> label; juce::Component* control; };
    std::vector<Row> rows_;
    void addRow (const juce::String& id, const juce::String& text, juce::Component& c, const juce::String& tip);
    void updateRows();
    void load();
};

class BlindSeparationWindow : public juce::DocumentWindow
{
public:
    explicit BlindSeparationWindow (Application& app);
    ~BlindSeparationWindow() override;
    void userTriedToCloseWindow() override;
    void closeButtonPressed() override;
    /** Set by MainWindow to release the window (closed by X or Close). */
    std::function<void()> onCloseRequested;
    BlindSeparationView& view() noexcept { return *view_; }
    /** Runs the separation as set in the view (the Separate button). */
    void start();
    bool isRunning() const noexcept { return running_; }

private:
    Application& app;
    std::unique_ptr<BlindSeparationView> view_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>> (true);
    bool running_ = false;
};
