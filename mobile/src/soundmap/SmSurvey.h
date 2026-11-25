// SpectroCraft Mobile - sound map: the marker map from survey pictures.
//
// Every survey picture shows two tags or more. The solver
//   1. picks a starting focal length (the one whose start fits best among a
//      few candidates: a wrong focal length distorts every tag's pose),
//   2. links the tags through the pictures that show them together (a
//      maximum spanning tree over the best views; of each tag's two planar
//      solutions, the one whose printed-up direction agrees with the
//      picture's clearest tag: the boards all stand upright), places every
//      camera by resection and re-estimates every tag from all its views,
//   3. refines everything at once (bundle adjustment, Levenberg-Marquardt
//      with a Huber loss): the tags' poses, the cameras and the lens (focal
//      length, radial distortion, principal point, the last two with weak
//      priors); the tag size sets the scale,
//   4. drops the observations that do not fit and refines again,
//   5. moves the result into the bowl's frame (bowlFrame).
#pragma once
#include "SmPose.h"
#include <map>
#include <string>
#include <vector>

namespace scm::smap
{
struct SurveyPhoto
{
    int width = 0, height = 0;
    Hold hold = Hold::LandscapeTopLeft;   // turns the lens' principal point offset
    std::vector<TagObservation> tags;
};

struct SurveySettings
{
    double tagSize = 0.16;              // black square side, metres
    Lens lens;                          // starting lens
    bool solveLens = true;              // refine focal length, distortion and principal point
    bool searchFocal = true;            // try other starting focal lengths
    double tagHeightAboveDeck = 0.0;    // metres: the map's z = 0 is this far below the tag centres
};

/** The map's frame from the tags (see docs/sound-map.md): origin at the centre
    of the circle through the tag centres (or their centroid), z up, x toward
    the lowest tag id. */
struct Frame
{
    Pose mapFromRef;            // ref = the frame the tag poses were given in
    bool circle = false;        // origin from the circle fit (else the centroid, or a single tag)
    double radius = 0.0;        // circle radius (m)
    double circleRms = 0.0;     // tag centres' distance to the circle (m, RMS)
    std::string note;
};

Frame bowlFrame (const std::map<int, Pose>& refFromTag, double tagHeightAboveDeck);

struct SurveyResult
{
    bool ok = false;
    std::string problem;

    Lens lens;
    std::map<int, Pose> tags;            // mapFromTag
    std::vector<bool> photoUsed;         // per input picture
    std::vector<Pose> cameras;           // camFromMap, per input picture (valid where photoUsed)

    std::map<int, int> tagPhotos;        // pictures that place each tag
    std::map<int, double> tagRmsPx;      // per tag
    double rmsPx = 0.0;
    int observations = 0;
    int rejectedObservations = 0;
    std::vector<int> unlinkedTags;       // seen, but never with a linked tag
    std::vector<int> weakTags;           // in a single picture
    Frame frame;
    std::vector<std::string> warnings;
};

SurveyResult solveSurvey (const std::vector<SurveyPhoto>& photos, const SurveySettings& settings);
} // namespace scm::smap
