// SpectroCraft Mobile - sound map: camera pose from tags.
//
// A single square tag gives the camera pose up to the classic planar
// ambiguity (two poses whose tag normals mirror each other about the line of
// sight); both are returned, refined, best first. Several tags of a known map
// give one pose (resection), refined on every corner with a robust (Huber)
// loss; a tag that disagrees with the others is dropped.
#pragma once
#include "SmCamera.h"
#include <map>
#include <string>
#include <vector>

namespace scm::smap
{
/** Homography (row-major 3x3) mapping src -> dst (>= 4 points, normalised DLT).
    Returns false on degenerate input. */
bool homographyDlt (const std::vector<std::array<double, 2>>& src,
                    const std::vector<std::array<double, 2>>& dst,
                    std::array<double, 9>& H);

/** Refines camFromWorld on 3D-2D correspondences (Levenberg-Marquardt,
    Huber loss of `huberPx` pixels). Returns the plain RMS error in pixels. */
double refinePose (Pose& camFromWorld, const std::vector<Vec3>& world, const std::vector<Pixel>& px,
                   const Intrinsics& K, double huberPx = 2.0, int maxIterations = 40);

/** RMS reprojection error (pixels); points behind the camera count 1000 px. */
double reprojectionRms (const Pose& camFromWorld, const std::vector<Vec3>& world, const std::vector<Pixel>& px,
                        const Intrinsics& K);

struct PoseCandidate
{
    Pose camFromTag;
    double rmsPx = 0.0;
};

/** The pose of one tag in the camera: one or two candidates (planar
    ambiguity), lowest error first. Empty when the corners are degenerate. */
std::vector<PoseCandidate> tagPoseCandidates (const TagObservation& o, double tagSize, const Intrinsics& K);

/** The other solution of the planar ambiguity (before refinement). */
Pose mirroredTagPose (const Pose& camFromTag) noexcept;

struct Resection
{
    bool ok = false;
    std::string problem;
    Pose camFromWorld;
    double rmsPx = 0.0;
    int tagsUsed = 0;
    std::vector<int> tagIds;          // the tags used
    std::vector<int> rejectedTags;    // known tags dropped as inconsistent
    double positionSigma = 0.0;       // 1-sigma uncertainty of the camera centre (m), from the fit
    Vec3 cameraCentre() const noexcept { return camFromWorld.inverse().t; }
};

/** Camera pose from the tags of a known map (worldFromTag per id). ok needs
    two tags or more (one square's two planar poses are not told apart at a
    distance) and an RMS error up to maxRmsPx; with one tag the pose is still
    returned (tagsUsed = 1), for callers that only need a start. */
Resection resect (const std::vector<TagObservation>& obs, const std::map<int, Pose>& worldFromTag,
                  double tagSize, const Intrinsics& K, double maxRmsPx = 3.0);
} // namespace scm::smap
