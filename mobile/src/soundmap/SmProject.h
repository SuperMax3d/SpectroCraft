// SpectroCraft Mobile - sound map: one map's files (docs/sound-map.md).
//
//   <recordings>/SoundMaps/<name>/project.json   everything below
//                                 survey/S001.jpg  the survey pictures
//                                 points/P001.wav  the takes, P001a.jpg / P001b.jpg before / after
//
// Paths in project.json are relative to the map's folder. Positions are in
// metres in the map's frame (x toward tag 0, z up, origin at the centre of the
// tags' circle; z = 0 the set tag height below the tag centres). Every picture
// keeps its detected tags, so the map can be rebuilt (with the points'
// pictures too) and the points located again without the pictures.
#pragma once
#include <JuceHeader.h>
#include "SmLevels.h"
#include "SmSurvey.h"
#include <optional>

class FileDoc;

namespace scm::smap
{
struct MapSettings
{
    double tagSizeMm = 160.0;
    int tagCount = 24;               // ids 0 .. tagCount - 1 are used
    double takeSeconds = 10.0;
    double countdownSeconds = 3.0;   // after the tap: the operator steps away, the tap noise passes
    Vec3 micOffsetCm {};             // back camera -> microphone, phone body frame
    double tagCentreHeightCm = 0.0;    // the tag centres' height above the map's z = 0
    double lensFRel = 0.75;          // starting focal length / long side
};

struct SurveyPicture
{
    juce::String image;              // relative path
    int width = 0, height = 0;
    int hold = (int) Hold::LandscapeTopLeft;
    std::vector<TagObservation> tags;
};

struct MarkerMap
{
    bool built = false;
    Lens lens;
    std::map<int, Pose> tags;        // mapFromTag
    double rmsPx = 0.0;
    int picturesUsed = 0;
    double radius = 0.0;             // the tags' circle (0 if none)
    juce::StringArray warnings;
    juce::Time builtAt;
};

struct Fix                           // the phone's position from one picture
{
    bool ok = false;
    Vec3 mic;                        // microphone position (map frame)
    Pose camFromMap;
    int tags = 0;
    double rmsPx = 0.0;
    double sigmaM = 0.0;             // 1-sigma position uncertainty of the camera
    juce::String problem;
};

enum class PointKind
{
    Point = 0,        // a map point
    Background = 1,   // the source off: the background noise per band (SNR)
    Reference = 2,    // a fixed spot measured again and again: the source's drift
};

struct MapPoint
{
    int number = 0;
    PointKind kind = PointKind::Point;
    bool pending = true;                              // the take is not finished (or was lost)
    juce::String wav, pictureBefore, pictureAfter;    // relative paths
    juce::Time time;
    int hold = (int) Hold::LandscapeTopLeft;          // of both pictures
    int width = 0, height = 0;                        // picture size
    std::vector<TagObservation> tagsBefore, tagsAfter;
    Fix before, after;
    Vec3 position;                                    // the fixes' mean (weighted by their precision)
    double sigmaM = 0.0;                              // its uncertainty
    bool located = false;
    double movedM = 0.0;                              // distance between the two fixes
    juce::String input;                               // the input's name at the take
    BandLevels levels;
    juce::String note;

    static constexpr double kMovedLimitM = 0.05;
    static constexpr double kSigmaLimitM = 0.02;
    bool moved() const noexcept { return movedM > kMovedLimitM; }
    /** Both pictures gave a position, so the move check ran. */
    bool checked() const noexcept { return before.ok && after.ok; }
    /** Located, checked, not moved, precise enough. */
    bool good() const noexcept { return located && checked() && ! moved() && sigmaM <= kSigmaLimitM; }
    /** Combines the two fixes into position, sigma and moved. */
    void updatePosition();
};

class SoundMapProject
{
public:
    /** <recordings>/SoundMaps */
    static juce::File rootFolder();
    /** The maps' folders, newest first. */
    static juce::Array<juce::File> listMaps();
    /** A new map folder (name from the date and time, made unique). */
    static std::optional<SoundMapProject> create (juce::String& error);
    static std::optional<SoundMapProject> load (const juce::File& folder, juce::String& error);

    bool save (juce::String& error) const;

    juce::File folder;
    juce::String name;
    MapSettings settings;
    std::vector<SurveyPicture> survey;
    MarkerMap map;
    std::vector<MapPoint> points;

    juce::File file (const juce::String& relative) const { return folder.getChildFile (relative); }
    juce::String relative (const juce::File& f) const { return f.getRelativePathFrom (folder).replaceCharacter ('\\', '/'); }
    juce::File nextSurveyPictureFile() const;
    int nextPointNumber() const;
    juce::File pointFile (int number, const juce::String& suffix, const juce::String& ext) const;
    MapPoint* point (int number) noexcept;

    /** The survey's input from the stored detections; with the points'
        pictures too when `withPoints` (more views: a better map). */
    std::vector<SurveyPhoto> surveyPhotos (bool withPoints) const;
    SurveySettings surveySettings() const;
    /** Stores a solved survey as the map, then locates every point again. */
    void setMap (const SurveyResult& r);

    /** The phone's position from a picture's tags (needs a built map). */
    Fix locate (const std::vector<TagObservation>& tags, int width, int height, int hold) const;
    /** Locates every point again from its stored detections (after the map changed). */
    void relocatePoints();

    /** The points of a kind that hold a level for the band (nominal Hz; 0 = LZeq, -1 = LAeq). */
    static double levelOf (const MapPoint& p, double band) noexcept;
    /** The unit most of the map points' levels are in ("" when none): points
        in another unit (a route change) are left off the map. */
    juce::String mapUnit() const;
    /** Signal-to-background ratio of a point in a band (dB), from the
        background take in the same unit closest in time; +inf without one. */
    double snrDb (const MapPoint& p, double band) const noexcept;
    /** The reference takes' level change in a band since the first one (dB),
        in time order (takes in the first one's unit): the source's drift.
        band 0 = the 250 Hz-2 kHz bands summed. */
    std::vector<std::pair<juce::Time, double>> drift (double band) const;

    // ---- the position inside each take's WAV
    // Metadata key "soundMapPoint" (JSON: map, point, kind, x, y, z in metres,
    // sigma, checked, moved, frame) and one line of the WAV's INFO comment
    // ("Sound map ..."), so a WAV alone says where it was recorded.

    juce::var positionVar (const MapPoint& p) const;
    /** "Sound map <name>, P012 (point): x 1.234 m, y -0.456 m, z -1.800 m, +/- 6 mm" */
    juce::String positionText (const MapPoint& p) const;
    /** Puts the point's position into a document (before it is saved). */
    void stampDocument (FileDoc& d, const MapPoint& p) const;
    /** Rewrites the position in the point's WAV in place (metadata chunks
        only, the audio is not touched; into the .meta.json sidecar instead of
        the SCMD chunk when the file keeps its metadata there). Safe on a
        background thread; never on the WAV the app has open (see writeStamp). */
    bool stampWav (const MapPoint& p, juce::String& error) const;
    /** Writes an already stamped document's metadata into `wav` in place (the
        same rules as stampWav): for the open document, on the message thread. */
    static bool writeStamp (const FileDoc& d, const juce::File& wav, juce::String& error);

    /** One row per take: number, kind, x, y, z (m), checks, LZeq, LAeq, then every band. */
    juce::String toCsv() const;

    // ---- the map as one ZIP (Menu > Export map)

    /** A part of the ZIP: its files (path in the ZIP, the file) and their bytes. */
    struct ExportPart
    {
        std::vector<std::pair<juce::String, juce::File>> files;
        juce::int64 bytes = 0;
        int count() const noexcept { return (int) files.size(); }
    };

    /** What the ZIP holds, under one folder named after the map: the takes and
        the data always (README.txt and export.csv written from memory,
        project.json, every take's WAV and its .meta.json sidecar), the points'
        pictures (before and after each take) and the survey pictures as
        options. Files that are gone are left out; a take saved outside the map's
        folder goes into points/. */
    struct ExportPlan
    {
        juce::String root;                          // the folder inside the ZIP
        juce::String readme, csv;
        ExportPart takes, pointPictures, surveyPictures;
        int wavCount = 0;
        juce::int64 bytes (bool withPointPictures, bool withSurveyPictures) const noexcept
        {
            return takes.bytes + (withPointPictures ? pointPictures.bytes : 0) + (withSurveyPictures ? surveyPictures.bytes : 0);
        }
    };
    ExportPlan exportPlan() const;

    /** The largest ZIP written: JUCE's ZIP has 32-bit sizes and offsets (no ZIP64). */
    static constexpr juce::int64 kZipMaxBytes = (juce::int64) 0xF0000000;

    /** Writes the plan's ZIP (WAVs and pictures stored as they are, the text
        deflated). False with `error`, and no file, when it would pass maxBytes
        or a file cannot be read or written. Reads only the plan and the files:
        safe on a background thread. */
    static bool writeExportZip (const ExportPlan&, bool withPointPictures, bool withSurveyPictures,
                                const juce::File& zip, juce::String& error, juce::int64 maxBytes = kZipMaxBytes);

    juce::var toVar() const;
    static std::optional<SoundMapProject> fromVar (const juce::var& v, const juce::File& folder, juce::String& error);
};

/** A size for people: "340 KB", "4.2 MB", "92 MB", "1.3 GB" (decimal units, as the Files app). */
juce::String sizeText (juce::int64 bytes);

/** Hold from juce::Desktop's orientation (the picture is upright for it). */
Hold holdFromDisplay (juce::Desktop::DisplayOrientation o) noexcept;
} // namespace scm::smap
