#pragma once
// SpectroCraft - Audio Engine: streaming file access, playback, waveform mipmaps, edits
#include <JuceHeader.h>
#include "UndoPatch.h"
#include <atomic>
#include <memory>
#include <vector>
#include <mutex>

class UndoManager; // fwd
struct Marker;     // fwd (MarkerManager)

class AudioEngine : public juce::AudioSource
{
public:
    AudioEngine();
    ~AudioEngine();

    // ---- File loading (metadata always; full buffer only when small enough) ----
    bool loadFile (const juce::File& file);
    void closeFile();
    bool isFileLoaded() const { return hasFile; }
    bool fullyInMemory() const { return buffer != nullptr; }
    bool hasUnsavedChanges() const { return unsaved; }
    const juce::File& getFile() const { return currentFile; }
    int numChannels() const { return nchans; }
    int64_t numSamples() const { return totalSamples; }
    double sampleRate() const { return rate; }
    double duration() const { return durationSecs; }

    // Thread-safe read of a range of samples for one channel.
    // Uses the in-memory buffer if present, otherwise streams from the file.
    bool readSamples (int channel, int64_t start, int64_t count, float* dest) const;

    // ---- Transport ----
    void play(); void stop(); void pause();
    bool isPlaying() const { return playing.load(); }
    bool isPaused() const { return paused.load(); }
    // Position in seconds. The playhead is stored as a fractional file-sample
    // position so playback can be resampled when the output device's sample
    // rate differs from the file's.
    double position() const { return playheadFrac.load() / rate; }
    void setPosition (double secs);
    void setLooping (bool); bool isLooping() const { return looping.load(); }
    void setLoopRange (juce::Range<double>); juce::Range<double> loopRange() const { return loopRange_; }
    void setGain (float); float gain() const { return gain_.load(); }

    // ---- AudioSource interface (audio thread) ----
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill) override;

    // ---- Editing (destructive, recorded as undo patches) ----
    void setUndoManager (::UndoManager* um) { undo = um; }
    bool canEdit() const { return buffer != nullptr; }
    bool canPaste() const { return clipboard != nullptr; }
    // Destructive edits. `channels` optionally restricts the effect to a
    // subset of channels (the currently visible/selected ones); nullptr or an
    // empty list means "all channels" for structural ops, and "no-op" is NOT
    // implied - pass nullptr to affect everything.
    void copySelection (juce::Range<int64_t> samples);
    void pasteAt (int64_t samplePos);
    void deleteSamples (juce::Range<int64_t> samples);
    void silenceSamples (juce::Range<int64_t> samples, const std::vector<int>* channels = nullptr);
    void fadeInSamples (juce::Range<int64_t> samples, const std::vector<int>* channels = nullptr);
    void fadeOutSamples (juce::Range<int64_t> samples, const std::vector<int>* channels = nullptr);
    void normalizeSamples (juce::Range<int64_t> samples, float target = 1.0f,
                           const std::vector<int>* channels = nullptr);
    // Multiply the selection by 10^(gainDb/20) on the given channels.
    void gainSamples (juce::Range<int64_t> samples, float gainDb,
                      const std::vector<int>* channels = nullptr);
    void reverseSamples (juce::Range<int64_t> samples, const std::vector<int>* channels = nullptr);

    // Apply an undo/redo patch to the in-memory buffer
    bool applyPatch (const ::Patch& p, bool forward);

    // ---- Saving ----
    bool saveFile (const juce::File& file) const;
    bool saveFileAs (const juce::File& file);

    // Markers embedded into WAV files as RIFF "cue" + "LIST"/"adtl" chunks.
    void setMarkersForSave (const std::vector<Marker>* m) { saveMarkers = m; }

    // ---- Waveform overview mipmaps (min/max pyramid) ----
    struct MipLevel { int64_t factor; std::vector<std::vector<float>> min, max; };
    const std::vector<MipLevel>& mipmaps() const { return mips; }
    int64_t mipmapBaseFactor() const { return baseFactor; }

    // ---- Zero-crossing snapping (nearest sign change within windowSecs) ----
    double snapToZeroCrossing (double timeSecs, double windowSecs) const;

private:
    mutable juce::File currentFile;
    std::unique_ptr<juce::AudioFormatManager> fm;
    mutable std::unique_ptr<juce::AudioFormatReader> reader; // streaming reader (mutex-protected)
    mutable std::mutex readerMutex;

    std::unique_ptr<juce::AudioBuffer<float>> buffer;
    std::unique_ptr<juce::AudioBuffer<float>> clipboard;
    std::vector<MipLevel> mips;
    int64_t baseFactor = 1;

    int nchans = 0;
    int64_t totalSamples = 0;
    double rate = 44100.0;
    double durationSecs = 0.0;
    bool hasFile = false;
    mutable bool unsaved = false;

    std::atomic<bool> playing{ false };
    std::atomic<bool> paused{ false };
    std::atomic<bool> looping{ false };
    std::atomic<double> playheadFrac{ 0 }; // fractional position in file samples
    double playbackRate = 0.0;             // device rate set by prepareToPlay()
    std::atomic<float> gain_{ 1.0f };
    juce::Range<double> loopRange_{ 0, 0 };

    ::UndoManager* undo = nullptr;
    const std::vector<Marker>* saveMarkers = nullptr;   // for WAV cue/adtl chunks

    void rebuildMipmaps();
    void clearMipmaps();
    bool saveWavWithMarkers (const juce::File& file) const;

    // friends: static edit helpers in AudioEngine.cpp
    friend void recordReplace (AudioEngine&, const juce::Range<int64_t>&,
                               const std::vector<std::vector<float>>&);
    friend void applyPerSample (AudioEngine&, juce::Range<int64_t>,
                                std::function<float (float, int, int)>, const std::vector<int>*);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

