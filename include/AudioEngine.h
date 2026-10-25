#pragma once
// SpectroCraft - Audio Engine: streaming file access, playback, waveform mipmaps, edits
#include <JuceHeader.h>
#include "UndoPatch.h"
#include "SpectralSelection.h"
#include "SpectralMaskProcessor.h"
#include "FileMeta.h"
#include <atomic>
#include <functional>
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
    /** Creates a brand-new in-memory document with the given channel count
        and sample rate. With no data it is empty (zero samples) - "New
        File"; the caller can then fill it via edits/paste. With data (one
        vector per channel, all the same length) that content becomes the
        initial buffer ("Copy to New File"). The document has no file path
        until Save As and is marked unsaved when it carries content. */
    bool initNew (int numChans, double sampleRate,
                  const std::vector<std::vector<float>>* data = nullptr);

    /** Loads a file. When onProgress is non-null it is called with 0..1 as
        the load advances (useful for large files loaded off-thread). If cancel
        points at an atomic set to true, loading stops at the next checkpoint,
        the engine is left in its pre-load state and false is returned. */
    bool loadFile (const juce::File& file,
                   std::function<void(float)> onProgress = nullptr,
                   const std::atomic<bool>* cancel = nullptr);
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

    /** "Play Spectral Selection": when a non-null selection is installed,
        playback passes through an STFT mask that zeros every (t,f) bin outside
        it. nullptr restores normal playback. Thread-safe: the audio thread
        snapshots the shared pointer per block, and selection changes take
        effect from the next block without restarting transport. */
    void setSpectralPlaybackMask(std::shared_ptr<const SpectralSelection> sel);

    // ---- AudioSource interface (audio thread) ----
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill) override;

    // ---- Editing (destructive, recorded as undo patches) ----
    void setUndoManager (::UndoManager* um) { undo = um; }
    bool canEdit() const { return hasFile; }

    /** If the file is still being streamed from disk, materialise it into an
        in-memory buffer so destructive edits can be applied. Returns false
        (and sets *errOut when non-null) if that is impossible - e.g. out of
        memory. Safe to call while playback is running: playback is stopped
        and any in-flight audio block is waited for before the source is
        swapped. If cancel points at an atomic set to true, loading stops at
        the next chunk boundary, the engine state is unchanged and false is
        returned with *errOut = "Cancelled". */
    bool ensureEditable (std::string* errOut = nullptr,
                         std::function<void(float)> onProgress = nullptr,
                         const std::atomic<bool>* cancel = nullptr);

    /** Reads a range of all channels into out (one vector per channel,
        each of length count). Works on streamed engines too. */
    bool readRange (int64_t start, int64_t count,
                    std::vector<std::vector<float>>& out) const;

    // Destructive edits. `channels` optionally restricts the effect to a
    // subset of channels (the currently visible/selected ones); nullptr or an
    // empty list means "all channels" for structural ops, and "no-op" is NOT
    // implied - pass nullptr to affect everything.
    // All return false (and set *errOut when non-null) if the edit cannot be
    // performed - e.g. the file could not be loaded into memory.
    /** Inserts clip at samplePos (the clipboard lives in Application, shared
        across documents). Channels of the target beyond the clipboard's own
        channel count receive silence. */
    bool pasteAt (int64_t samplePos, const juce::AudioBuffer<float>& clip,
                  std::string* errOut = nullptr);
    /** Adds clip to the existing content at samplePos instead of replacing:
        the overlap [pos, pos+clipLen) is summed sample-wise; if the
        clipboard extends past the end of file, the remainder is appended.
        Recorded as one undoable patch. */
    bool pasteMixAt (int64_t samplePos, const juce::AudioBuffer<float>& clip,
                     std::string* errOut = nullptr);
    /** Appends clip.getNumChannels() brand-new channels to the file and
        pastes the clipboard into them at samplePos; the rest of each new
        channel is silence. Existing channels are untouched except for the
        insertion shift. Recorded as one undoable patch that also restores
        the old channel count on undo. */
    bool pasteToNewChannelsAt (int64_t samplePos, const juce::AudioBuffer<float>& clip,
                               std::string* errOut = nullptr);
    bool deleteSamples (juce::Range<int64_t> samples, std::string* errOut = nullptr);
    bool silenceSamples (juce::Range<int64_t> samples, const std::vector<int>* channels = nullptr,
                         std::string* errOut = nullptr);
    bool fadeInSamples (juce::Range<int64_t> samples, const std::vector<int>* channels = nullptr,
                        std::string* errOut = nullptr);
    bool fadeOutSamples (juce::Range<int64_t> samples, const std::vector<int>* channels = nullptr,
                         std::string* errOut = nullptr);
    bool normalizeSamples (juce::Range<int64_t> samples, float target = 1.0f,
                           const std::vector<int>* channels = nullptr,
                           std::string* errOut = nullptr);
    // Multiply the selection by 10^(gainDb/20) on the given channels.
    bool gainSamples (juce::Range<int64_t> samples, float gainDb,
                      const std::vector<int>* channels = nullptr,
                      std::string* errOut = nullptr);
    bool reverseSamples (juce::Range<int64_t> samples, const std::vector<int>* channels = nullptr,
                         std::string* errOut = nullptr);

    // Resample the whole file to a new sample rate using SoXR (the same
    // engine Audacity uses). Streaming files are materialised into memory
    // first (see ensureEditable).
    // remixWeights optionally changes the channel count at the same time:
    // weights[outCh][inCh] gives each input channel's contribution to each
    // output channel (rows are normalised by their sum; a zero row is
    // silence). nullptr keeps the current channels.
    // NOT undoable; clears the clipboard and mipmap pyramid (the caller is
    // expected to rebuild mipmaps on its worker thread afterwards).
    // If cancel points at an atomic set to true, resampling stops at a chunk
    // boundary and false is returned with the engine state unchanged.
    bool resampleTo (double newRate, std::function<void(float)> onProgress = nullptr,
                     const std::atomic<bool>* cancel = nullptr,
                     std::string* errOut = nullptr,
                     const std::vector<std::vector<double>>* remixWeights = nullptr);

    /** Stop playback and wait for any in-flight audio block to finish reading
        the buffer, so a long-running worker can modify it safely. */
    void quiesceForEdit ();

    // Replace the entire audio content with `data` (one row per channel, all
    // the same length) at sample rate `newRate`. Used by whole-file merges
    // that restructure the file (channel layout or a rate change). NOT
    // undoable, like resampleTo(): playback is stopped and in-flight blocks
    // are awaited before the swap; any streaming reader is dropped; the
    // mipmap pyramid is cleared (the caller rebuilds it) and unsaved is set.
    bool replaceWholeFile (const juce::AudioBuffer<float>& data, double newRate,
                           std::string* errOut = nullptr);

    // Append `append` after the current content. Channels beyond the appended
    // data's own count receive silence; when it has more channels than the
    // file currently does the channel count grows to match (existing channels
    // are untouched). Records an undoable insertion patch - because nothing
    // existing is modified, no old sample data is stored and undo only
    // truncates. The caller must have stopped playback first if this runs off
    // the message thread for a long time (see mergeFilesIntoActiveDoc).
    bool appendSamples (const juce::AudioBuffer<float>& append,
                        const juce::String& description);

    // Apply an undo/redo patch to the in-memory buffer
    // rebuild = false is used by batched undo/redo jumps so the mipmap
    // pyramid is rebuilt only once for the whole run of patches.
    bool applyPatch (const ::Patch& p, bool forward, bool rebuild = true);

    // ---- Saving ----
    bool saveFile (const juce::File& file) const;
    bool saveFileAs (const juce::File& file);

    // Markers embedded into WAV files as RIFF "cue" + "LIST"/"adtl" chunks.
    void setMarkersForSave (const std::vector<Marker>* m) { saveMarkers = m; }

    // Transcription segments embedded into WAV files as a RIFF "SCTT" chunk
    // (JSON, see include/ScttChunk.h). nullptr or empty string = no chunk.
    void setScttForSave (const std::string* json) { saveScttJson = json; }

    // Metadata mode for WAV saves: embedded writes cue/adtl/SCTT/SCMD chunks,
    // external writes a plain WAV (the sidecar is handled by the caller).
    // nullptr keeps the legacy behaviour (embedded).
    void setMetaModeForSave (const sc::MetaMode* m) { saveMetaMode = m; }

    // Builds the SCMD bundle JSON at save time (called on the save thread).
    void setScmdJsonForSave (std::function<std::string()> builder)
    {
        saveScmdBuilder = std::move (builder);
    }

    // Raw payload of the source file's "bkdk" calibration chunk, preserved
    // byte-for-byte in WAV saves. nullptr or empty string = no chunk.
    void setBkdkForSave (const std::string* raw) { saveBkdkPayload = raw; }

    // ---- Waveform overview mipmaps (min/max pyramid) ----
    struct MipLevel { int64_t factor; std::vector<std::vector<float>> min, max; };
    const std::vector<MipLevel>& mipmaps() const { return mips; }
    int64_t mipmapBaseFactor() const { return baseFactor; }

    /** Upper bound (in total sample-channels) for materialising a file into
        RAM. Files above this are streamed from disk. 0 = always stream.
        Default: 64 M sample-channels (~256 MB of float data). */
    void setMaxInMemorySampleChannels (int64_t v) { inMemoryLimit = v; }
    int64_t maxInMemorySampleChannels() const { return inMemoryLimit; }
    // Public so batched undo/redo jumps can rebuild once for a whole run.
    // Rebuilds the mipmap pyramid. Returns false (with the pyramid left empty)
    // if cancel points at an atomic set to true.
    bool rebuildMipmaps(std::function<void(float)> onProgress = nullptr,
                        const std::atomic<bool>* cancel = nullptr);

private:
    // One sequential pass over the file to build the finest mipmap level
    // (memory-mapped reader > in-memory buffer > chunked streaming reads).
    static bool buildFinestLevel (AudioEngine& eng, int64_t factor,
                                  MipLevel& lv,
                                  std::function<void(float)> onProgress,
                                  const std::atomic<bool>* cancel = nullptr);
public:

    /** Replace `samples` with newData (same length, one vector per channel)
        and record an undoable patch. The caller is responsible for rebuilding
        the mipmaps afterwards (see replaceSamples in AudioEngine.cpp).
        Returns false (and sets *errOut when non-null) if the file could not
        be made editable first. */
    bool replaceSamples(const juce::Range<int64_t>& samples,
                        const std::vector<std::vector<float>>& newData,
                        const juce::String& description,
                        std::string* errOut = nullptr);

    // ---- Zero-crossing snapping (nearest sign change of `channel` within
    //      +/- windowSecs around timeSecs; returns the crossing in seconds,
    //      quantised to a sample boundary). Falls back to timeSecs when no
    //      crossing exists in the window. ----
    double snapToZeroCrossing (double timeSecs, double windowSecs, int channel = 0) const;

private:
    mutable juce::File currentFile;
    std::unique_ptr<juce::AudioFormatManager> fm;
    mutable std::unique_ptr<juce::AudioFormatReader> reader; // streaming reader (mutex-protected)
    mutable std::mutex readerMutex;

    std::unique_ptr<juce::AudioBuffer<float>> buffer;
    std::vector<MipLevel> mips;
    int64_t baseFactor = 1;
    int64_t inMemoryLimit = 64ll * 1024 * 1024; // sample-channels

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
    std::atomic<int> audioInBufferSection{ 0 }; // >0 while the audio thread is reading `buffer`
    double playbackRate = 0.0;             // device rate set by prepareToPlay()
    std::atomic<float> gain_{ 1.0f };
    juce::Range<double> loopRange_{ 0, 0 };

    // Spectral playback mask (see setSpectralPlaybackMask). playSel is swapped
    // under maskMutex; the audio thread snapshots it per block and runs the
    // PlaybackMask state machine under the same lock so prepare/reset never
    // races an in-flight process().
    std::mutex maskMutex;
    std::shared_ptr<const SpectralSelection> playSel;
    std::shared_ptr<SpectralMask::PlaybackMask> playMask;

    ::UndoManager* undo = nullptr;
    const std::vector<Marker>* saveMarkers = nullptr;   // for WAV cue/adtl chunks
    const std::string* saveScttJson = nullptr;          // for the WAV SCTT chunk
    const sc::MetaMode* saveMetaMode = nullptr;         // for WAV metadata mode
    std::function<std::string()> saveScmdBuilder;       // SCMD bundle JSON builder
    const std::string* saveBkdkPayload = nullptr;       // raw "bkdk" chunk to preserve

    void clearMipmaps();
    bool saveWavWithMarkers (const juce::File& file) const;

    // friends: static edit helpers in AudioEngine.cpp
    friend void recordReplace (AudioEngine&, const juce::Range<int64_t>&,
                               const std::vector<std::vector<float>>&, const juce::String&, int);
    friend void applyPerSample (AudioEngine&, juce::Range<int64_t>,
                                std::function<float (float, int, int)>, const std::vector<int>*,
                                const juce::String&);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};

