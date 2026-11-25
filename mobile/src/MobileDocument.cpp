#include "MobileDocument.h"
#include "AxisScale.h"
#include <cmath>

namespace scm
{
MobileDocument::MobileDocument()
{
    // A phone has far less memory than a desktop: a smaller tile cache.
    spectralEngine.setMaxCacheBytes ((size_t) 96 << 20);
    spectroSettings.fftAuto = true;
    spectroSettings.logFrequency = true;
    spectroSettings.minFrequency = 20.0f;
    spectroSettings.maxFrequency = 20000.0f;
}

MobileDocument::~MobileDocument() = default;

juce::File MobileDocument::recordingsFolder()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
   #if ! (JUCE_IOS || JUCE_ANDROID)
    // Desktop development builds: SC_MOBILE_RECORDINGS=<folder> keeps test
    // recordings out of the Documents folder.
    const auto custom = juce::SystemStats::getEnvironmentVariable ("SC_MOBILE_RECORDINGS", {});
    dir = custom.isNotEmpty() ? juce::File (custom) : dir.getChildFile ("SpectroCraft Mobile");
   #endif
    if (! dir.isDirectory()) dir.createDirectory();
    return dir;
}

juce::File MobileDocument::newRecordingFile()
{
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");
    return recordingsFolder().getNonexistentChildFile ("Recording " + stamp, ".wav", false);
}

bool MobileDocument::hasAudio() const noexcept
{
    return current != nullptr && current->engine.isFileLoaded();
}

double MobileDocument::duration() const noexcept
{
    return hasAudio() ? current->engine.duration() : 0.0;
}

double MobileDocument::sampleRate() const noexcept
{
    return hasAudio() ? current->engine.sampleRate() : 48000.0;
}

int MobileDocument::numChannels() const noexcept
{
    return hasAudio() ? current->engine.numChannels() : 0;
}

juce::File MobileDocument::file() const
{
    return current != nullptr ? current->file : juce::File();
}

juce::String MobileDocument::displayName() const
{
    if (current == nullptr) return "No file";
    if (current->file == juce::File()) return "New recording";
    return current->file.getFileNameWithoutExtension();
}

bool MobileDocument::hasUnsavedChanges() const
{
    return current != nullptr && (current->engine.hasUnsavedChanges() || (bool) current->metaDirty);
}

void MobileDocument::adoptDocument (std::unique_ptr<FileDoc> d)
{
    if (onAboutToReplace) onAboutToReplace();
    current = std::move (d);
    spectralEngine.clearCache();
    sel = {};
    head = 0.0;
    storedPlots.clear();
    if (current != nullptr)
        storedPlots.fromVar (current->storedPlots);
    if (onReplaced) onReplaced();
    changed();
}

bool MobileDocument::open (const juce::File& f, juce::String& error)
{
    auto d = std::make_unique<FileDoc>();
    // Recordings stay in memory so they can be edited; very long files stream.
    d->engine.setMaxInMemorySampleChannels ((int64_t) 48000 * 60 * 30 * 2);
    if (! d->load (f))
    {
        error = "Could not open " + f.getFileName() + ".";
        return false;
    }
    adoptDocument (std::move (d));
    return true;
}

bool MobileDocument::startNew (int channels, double rate)
{
    auto d = std::make_unique<FileDoc>();
    if (! d->engine.initNew (juce::jlimit (1, 8, channels), rate))
        return false;
    d->name = "New recording";
    adoptDocument (std::move (d));
    return true;
}

void MobileDocument::close()
{
    adoptDocument (nullptr);
}

bool MobileDocument::save (juce::String& error)
{
    if (! hasAudio()) { error = "Nothing to save."; return false; }
    const auto target = current->file != juce::File()
                         && current->file.getFileExtension().equalsIgnoreCase (".wav")
                            ? current->file : newRecordingFile();
    return saveAs (target, error);
}

bool MobileDocument::saveAs (const juce::File& f, juce::String& error)
{
    if (! hasAudio()) { error = "Nothing to save."; return false; }
    current->storedPlots = storedPlots.toVar();
    current->metaMode = sc::MetaMode::Embedded;
    if (! current->engine.saveFileAs (f))
    {
        error = "Could not write " + f.getFileName() + ".";
        return false;
    }
    current->file = f;
    current->name = f.getFileName();
    current->metaDirty = false;
    changed();
    return true;
}

void MobileDocument::setSelection (juce::Range<double> r)
{
    const double d = duration();
    r = juce::Range<double> (juce::jlimit (0.0, d, r.getStart()), juce::jlimit (0.0, d, r.getEnd()));
    if (r == sel) return;
    sel = r;
    changed();
}

void MobileDocument::setPlayhead (double t)
{
    head = juce::jlimit (0.0, duration(), t);
    if (hasAudio()) current->engine.setPosition (head);
    changed();
}

juce::Range<int64_t> MobileDocument::selectionSamples() const
{
    const double r = sampleRate();
    return { (int64_t) std::llround (sel.getStart() * r), (int64_t) std::llround (sel.getEnd() * r) };
}

bool MobileDocument::deleteSelection (juce::String& error)
{
    if (! hasAudio() || sel.isEmpty()) { error = "Select a time range first."; return false; }
    std::string err;
    auto& eng = current->engine;
    eng.stop();
    if (! eng.ensureEditable (&err) || ! eng.deleteSamples (selectionSamples(), &err))
    {
        error = err.empty() ? juce::String ("Delete failed.") : juce::String (err);
        return false;
    }
    current->remapForDeletion (sel.getStart(), sel.getEnd());
    sel = { sel.getStart(), sel.getStart() };
    head = juce::jlimit (0.0, duration(), sel.getStart());
    spectralEngine.clearCache();
    changed();
    return true;
}

bool MobileDocument::gainSelection (float db, juce::String& error)
{
    if (! hasAudio()) { error = "No audio."; return false; }
    auto& eng = current->engine;
    const auto range = sel.isEmpty() ? juce::Range<int64_t> (0, eng.numSamples()) : selectionSamples();
    std::string err;
    eng.stop();
    if (! eng.ensureEditable (&err) || ! eng.gainSamples (range, db))
    {
        error = err.empty() ? juce::String ("Gain failed.") : juce::String (err);
        return false;
    }
    changed();
    return true;
}

bool MobileDocument::canUndo() const { return current != nullptr && current->undo.canUndo(); }
bool MobileDocument::canRedo() const { return current != nullptr && current->undo.canRedo(); }

bool MobileDocument::undo()
{
    if (! canUndo()) return false;
    auto& eng = current->engine;
    eng.stop();
    auto p = current->undo.popUndo();
    if (! p || ! eng.applyPatch (*p, false)) return false;
    spectralEngine.clearCache();
    setSelection (sel);   // clamps to the new length
    changed();
    return true;
}

bool MobileDocument::redo()
{
    if (! canRedo()) return false;
    auto& eng = current->engine;
    eng.stop();
    auto p = current->undo.popRedo();
    if (! p || ! eng.applyPatch (*p, true)) return false;
    spectralEngine.clearCache();
    setSelection (sel);
    changed();
    return true;
}

ScaleCalibration MobileDocument::calibration() const
{
    if (current == nullptr) return {};
    if (const auto* e = sc::axes::primary (current->axes, sc::axes::Kind::Amplitude))
    {
        ScaleCalibration s;
        s.unit = sc::axes::calibrationUnit (e->units);
        if (s.unit == ScaleCalibration::Unit::None) return current->scale;
        s.fullScaleValue = e->scale;
        s.channelFullScale = e->chanScale;
        return s;
    }
    return current->scale;
}

void MobileDocument::setCalibration (const ScaleCalibration& c)
{
    if (current == nullptr) return;
    current->scale = c;
    // A file with Scale-form axes reads its calibration from the primary
    // Amplitude entry on the desktop: keep it in step.
    for (auto& e : current->axes)
        if (e.kind == sc::axes::Kind::Amplitude && e.visible && e.isAddAxis())
        {
            e.units = c.isCalibrated() ? sc::axes::unitsFromCalibration (c) : sc::axes::defaultUnits (sc::axes::Kind::Amplitude);
            e.scale = c.isCalibrated() ? c.fullScaleValue : 1.0;
            e.chanScale = c.channelFullScale;
            break;
        }
    current->metaDirty = true;
    changed();
}

void MobileDocument::linkVideo (const juce::File& movie, double offsetSecs)
{
    if (current == nullptr) return;
    sc::VideoTrackData t;
    t.file = movie;
    t.relPath = movie.getFileName();   // saved next to the WAV
    t.offsetSecs = offsetSecs;
    current->videoTracks = { t };
    current->metaDirty = true;
    changed();
}

juce::File MobileDocument::linkedVideo() const
{
    if (current == nullptr || current->videoTracks.empty()) return {};
    return current->videoTracks.front().file;
}

void MobileDocument::plotsChanged()
{
    if (current == nullptr) return;
    current->storedPlots = storedPlots.toVar();
    current->metaDirty = true;
    changed();
}
} // namespace scm
