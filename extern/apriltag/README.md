# AprilTag 3 (vendored subset)

The AprilTag detector from AprilRobotics, used by the mobile app's sound map
(docs/sound-map.md) to find its printed markers.

- Upstream: https://github.com/AprilRobotics/apriltag
- Commit: b7c0ebe9aa20f82ec7a828579004f9e706bfecd9 (2026-08-07)
- Licence: BSD 2-Clause, `LICENSE.md` (Copyright (C) 2013-2016, The Regents of The University of Michigan)

Only the files the detector and the tag36h11 family need are kept, unmodified:
`apriltag.c/.h`, `apriltag_math.h`, `apriltag_quad_thresh.c`, `tag36h11.c/.h`
and `common/` without `getopt.*` and `pjpeg*`. The other families, the pose
estimation, the Python wrapper, the examples and the tests are left out.

Built by `mobile/CMakeLists.txt` as the static library `sc_apriltag` (always
optimised, warnings off).
