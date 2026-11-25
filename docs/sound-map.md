# Sound map (mobile): where each recording was made, and a map of the levels

Goal: record at many points of a space with the phone, know the position
(x, y, z) of each point to about a centimetre, then look at a map of the levels
per frequency band. Positions come from the phone's camera: printed markers
stand around the space; each take comes with a picture before and after it, in
which the app finds the markers and works out where the phone was.

First test: a concrete skate bowl 24 ft (7.32 m) across.

## 1. Feasibility

Feasible with the phone alone and printed markers, and implemented (Map tab).
The parts are standard computer vision (fiducial markers, camera resection,
bundle adjustment) and the app's recording and 1/3-octave analysis.

| Question | Answer |
|---|---|
| Position | About 0.5-1 cm typical, 1-2 cm worst (1 sigma), with 24 tags; measured on a synthetic model of the bowl (section 4). |
| Is that enough? | Yes. Wavelengths are 5.4 m at 63 Hz, 34 cm at 1 kHz, 8.6 cm at 4 kHz. The spacing between points (0.25-0.75 m) limits the detail long before the position does. |
| Field time | About 25 s per point (countdown, picture, 10 s take, picture, move): 60-80 points in 30-40 min, plus 10 min to set the tags and survey them. |
| The weak point | Acoustics, not geometry: the source's steadiness, the operator's body, the phone's microphone. Section 5 lists the checks the app and the procedure provide. |

## 2. Markers

### Pattern: AprilTag 36h11, not QR codes

| | AprilTag 36h11 | QR code | ArUco 4x4 |
|---|---|---|---|
| Designed for | Pose (corners to a fraction of a pixel) | Data | Pose |
| Cells across | 8 (6x6 data + black border) | 21+ (version 1) | 6 |
| Range at equal printed size | 1x | about 0.35x (cells 2.6x smaller) | 1.3x |
| False detections | Practically none (Hamming distance 11) | None | More, few ids |
| Ids | 587 | Any | 50-1000 |

A QR code carries data we do not need, and its modules are so small that it
stops being readable well before an AprilTag of the same size. The detector is
AprilRobotics' `apriltag` (BSD-2, vendored in `extern/apriltag`), small plain C.

### Size

| Black square | Pixels across at 3 m / 5 m / 7.5 m (1080p / 4K stills) | Paper |
|---|---|---|
| 160 mm (default) | 77 / 46 / 31 px (1080p), about 2x on 4K | Letter |
| 200 mm | 97 / 58 / 39 px (1080p) | Tabloid |

- iOS: the app takes the largest still the camera's 1080p video format offers
  (about 4K at the same field of view; a SPECTROCRAFT PATCH in the vendored
  JUCE, which by default took 1080p and re-encoded it at JPEG quality 0).
- Android: 1080p stills (the camera is shared with the 1080p video).

Menu > Tag sheets (PDF) prints one tag per page at the exact size, with its id,
the edge that goes up, and a 100 mm bar: print at 100 % ("actual size") and
check the bar. Keep the white border around the black square.

Mounting: glue each sheet flat on rigid board (foam board, corrugated plastic);
matte paper (gloss reflects the sun into the lens). A bowed board moves its
corners and the whole map.

### Number and layout for the 24 ft bowl

**24 tags around the rim** (16 at least), ids 0 to 23 counter-clockwise seen
from above, one every 15 deg (0.96 m apart along the coping):

```
                      6
                 8         4
              10              2
            12        (+)       0   ---> x
              14              22
                 16        20
                      18
```

- Stand each board on the deck just behind the coping, **upright, printed side
  toward the centre**, leaning the top 10-15 deg toward the bowl. The solver
  relies on the boards standing upright to choose between the two poses a
  single square allows.
- Raise each board so the **bottom of the black square is 15 cm or more above
  the top of the coping** (a block under it): from deep in the bowl the coping
  hides the bottom of anything close behind it.
- Weigh the boards down: a board that moves after the survey shifts every point
  measured after it (the localisation drops a tag that disagrees with the
  others, but not a slow drift of several).
- Nothing on the bowl's floor: the points need it, and tags seen at grazing
  angles are poor.

Why 24: the precision study (section 4) gives about 1.5x better positions than
16 tags, because every picture holds more tags at more different distances.

### The map's frame

Built from the tags, no tape measure needed:

- origin: the centre of the circle through the tag centres, at their height
  minus the "tag centres above the deck" setting (z = 0 at the deck once it is
  set);
- z up (normal to the plane of the tag centres, signed by the tags' upright
  direction), x toward tag 0, y counter-clockwise. Points in the bowl have
  negative z.

Measuring one distance (tag 0 to tag 12 across, centre to centre) with a tape
checks the scale: the map's scale comes only from the printed tag size.

## 3. Method

### Survey (about 5 minutes)

Map tab > Survey: the app switches to landscape and shows the camera. Take
30-40 pictures, each showing two tags or more:

- from the bowl's centre, turning around (a picture every 30 deg);
- from near the walls, looking **diagonally along the bowl** so that near and
  far tags share the picture;
- a few closer pictures of tags seen at 30-50 deg (they pin the lens).

Tilt the phone up: in landscape the picture spans only about +/-20 deg
vertically, and the rim is 20-28 deg above the phone from the bottom of the
bowl.

Build map solves every tag's position and orientation, the camera of every
picture and the lens (focal length, radial distortion, principal point) at once
(bundle adjustment). It reports the error in pixels, the tags seen in a single
picture and the tags not linked to the others; take more pictures of those. The
points' own pictures join the next Build map, so the map gets better as points
are measured; Build map locates every point again.

### Measuring a point

Map tab > Measure, choose the kind of take, tap Measure (takes are 3 s or
longer: the analysis needs one 1.4 s period of the source after the trimmed
ends):

1. a countdown (3 s by default: step away from the phone on its pole or
   tripod, let the tap noise pass);
2. a picture: the app finds the tags and the phone's position. It refuses a
   point seen with fewer than two mapped tags (one square has two mirror-image
   poses that a distant view cannot tell apart) or more uncertain than 5 cm
   ("aim at more tags");
3. the take (10 s by default), into the map's folder;
4. a second picture: if the two positions differ by more than 5 cm the point
   is flagged "moved"; if the second picture gives no position the point is
   "not checked". Both are left off the surface, as are points more uncertain
   than 2 cm;
5. the levels.

The pictures are taken before and after the take, never during it (the
shutter sound).

Kinds of take:

| Kind | Use |
|---|---|
| Point | A map point. |
| Background | The source off: the background per band. Bands of the points less than 10 dB above it are drawn hollow; less than 3 dB, left off the surface. |
| Reference | The same fixed spot again and again (a marked tripod spot): the source's drift. The View summary gives it for the 250 Hz-2 kHz bands. |

The microphone is not at the camera. Settings > Microphone offset (from the
back camera to the microphone in use, cm, phone seen from the screen: x right,
y up, z out of the screen) is applied with the way the phone was held. Measure
it once with a ruler on the phone used. iOS normally records from the bottom
microphone in measurement mode; the app does not pin the microphone.

### Map

View: the top view (x, y) with the points coloured by level over an
interpolated surface, the tags and their circle; or the side view (distance
from the axis, height). Choose the band (1/3 octave, LZeq, LAeq) and absolute
or relative levels. Tap a point for its position, uncertainty, level and
background margin, to open its take in the main views, or to delete it. Menu >
Export CSV writes every take for other tools.

The surface averages mean-square pressure (energy) with Gaussian weights, each
point with its own width (the distance to its third nearest neighbour), and
stops 1.5 widths from the nearest point: it never extends the data. The legend
says "trend only" for bands above c / (4 x spacing), whose interference
patterns the points cannot resolve. Relative levels are re the surface's
energy mean.

## 4. Precision (measured on a synthetic model)

`mobile/tests/SoundMapTests.cpp` builds the bowl (tags on the rim at 0.2 m
above the deck, leaning 15 deg), cameras 1.2 m above the bowl's surface, a lens
with distortion and an off-centre principal point. Two kinds of runs:

- **rendered pictures** (perspective, distortion, blur, noise) through the real
  detector, survey and localisation: corners within 0.02 px, tags 1.2 mm RMS,
  microphone 2.7 mm median, 6.7 mm worst over 20 points. Rendered pictures are
  cleaner than photographs: an optimistic bound;
- **corners with 0.3 px of noise** (a realistic value for photographs): the
  tables below.

Camera position error with the true map (60 points over the bowl; `SC_SOUNDMAP_STUDY=1`):

| Tags | Stills | 1 picture: median / 90 % | 2 pictures averaged |
|---|---|---|---|
| 16 | 1080p | 9 / 22 mm | 7 / 17 mm |
| 16 | 4K | 5 / 10 mm | 3 / 7 mm |
| 24 | 1080p | 6 / 13 mm | 4 / 8 mm |
| 24 | 4K | 3 / 6 mm | 2 / 4 mm |

The uncertainty the app reports for each position (from the fit) matches
these errors: over 400 simulated pictures, RMS error / RMS reported sigma =
0.97.

The aim direction changes little on average; it matters near the walls, where
the tags in view are all far and at the same distance (aim diagonally). The
200 mm tags change little at these distances.

The marker map from 28 pictures with 0.3 px noise: 6 mm RMS on the tag
centres, most of it a slight change of scale (the scale comes only from the
tag's printed size); more pictures, and the points' pictures, reduce it.

Not modelled: boards that are not flat (a 5 mm bow over 20 cm moves a tag's
pose by about 1 cm), autofocus changing the focal length by 0.1-0.5 % between
pictures, and the microphone offset's own error. Expect 1-2 cm in the field.

## 5. Acoustic protocol

- **Source signal**: Menu > Source signal writes a periodic pink noise (a
  multisine of period 2^16 samples at 48 kHz, 20 Hz-20 kHz, equal level per
  1/3 octave). Play it in a loop from another device into a loudspeaker that
  stays in place. Random noise varies from take to take by 4.34 / sqrt(B T) dB
  (0.4 dB at 50 Hz for 10 s); the periodic pink, analysed by frames of one
  period, gives the same band levels in every 10 s take (the test measures
  0.00 dB between two windows). The phone's own generator is switched off
  during takes: the source must not move with the phone.
- **Source placement, first test**: on the bowl's axis, facing up, low (0.3 m
  above the floor): the field is then nearly symmetric around the axis, the
  side view shows level against distance from the axis, and points at the same
  radius give a direct estimate of the measurement's spread (expect about
  1 dB). Then an off-axis source (r = 1.8 m), then a source on the coping (the
  wall's whispering gallery).
- **Heights**: a horizontal slice at one absolute height (floor + 1.2 m, for
  example), over the area where that plane is at least 0.5 m above the
  concrete. Walls and transitions are a separate set at a constant distance
  from the surface.
- **Points**: rings at r = 0, 0.5, 1.0 ... 3.0 m with 8-12 points each (about
  60), plus a radial line on the axis side in 0.1 m steps up to 0.6 m (on a
  tripod) for the focus, and a vertical profile on the axis in 0.2 m steps.
  The map resolves patterns down to twice the spacing: c / (4 x spacing) is
  114 Hz at 0.75 m, 340 Hz at 0.25 m, 860 Hz at 0.1 m.
- **The phone**: on a light pole or tripod, the operator 1.5-2 m away and not
  between the source and the phone (the countdown gives the time), same
  orientation rule at every point (camera toward the rim, the screen toward the
  source for example), not turned between the pictures.
- **Background and drift**: a Background take at the start, every 15-20
  points and at the end; a Reference take at a marked tripod spot at the same
  times. With the periodic pink, the reference should agree within 0.2-0.3 dB.
- **Calibration**: the 94 dB calibrator cannot seal over a phone's built-in
  microphone, and a one-frequency calibration leaves the phone's response in
  every other band. For dB-SPL use an external measurement microphone that the
  calibrator fits; with the built-in microphone treat the map as relative
  (patterns, differences between points), its LZeq and LAeq as band-limited.
- **Wind** outdoors: a foam windscreen; check the low bands against the
  background takes.

## 6. In the app

The Map tab (after Edit) takes the screen below the top bar:

- the map's list, View / Survey / Measure, and a menu: New map, Settings (tag
  size, number of tags, take length, countdown, microphone offset, tag height
  above the deck), Tag sheets (PDF), Source signal (WAV), Export CSV, Locate
  the points again, Delete this map;
- Survey and Measure show the camera preview (Android needs it on screen to
  take a picture), keep the screen on and hold the UI in landscape; the
  regular REC never films while the Map tab uses the camera.

### Files

```
<recordings>/SoundMaps/<map name>/
    project.json        settings, lens, tags (pose in the map's frame), survey
                        pictures and point pictures (their detected tags), points
                        (kind, position, uncertainty, checks, input, levels)
    survey/S001.jpg ... the survey pictures
    points/P001.wav     the takes (calibration in their metadata, like any take)
    points/P001a.jpg    picture before the take, P001b.jpg after
    export.csv          written by Export CSV
```

A point is written to project.json ("pending") before its take starts, so a
crash leaves a trace rather than an unknown file.

## 7. Code

Headless parts (no UI, unit tested) in `mobile/src/soundmap/`:

| File | Role |
|---|---|
| `SmMath.h` | vectors, rotations, poses, symmetric eigen, Cholesky, Horn's nearest rotation |
| `SmCamera.h` | lens (focal, distortion, principal point), projection, the phone's holds |
| `SmTagDetector` | AprilTag 36h11 detection (`extern/apriltag`), pixel centres on integers |
| `SmPose` | homography, single-tag pose (both planar solutions), multi-tag resection with its uncertainty |
| `SmSurvey` | the marker map: initialisation, bundle adjustment, the bowl's frame |
| `SmLevels` | band levels of a take, A weighting, the periodic pink source |
| `SmInterp` | energy-domain interpolation with per-point widths and a mask |
| `SmProject` | the map's files (JSON, CSV), localisation, background margin, drift |
| `SmMarkersPdf` | printable tag sheets (PDF, exact scale) |

UI: `mobile/src/SoundMapPane.*` (the tab), `mobile/src/SoundMapView.*` (the
drawing); MainScreen owns the camera and the takes. Tests:
`mobile/tests/SoundMapTests.cpp` (the study and a demo map behind
`SC_SOUNDMAP_STUDY=1` / `SC_SOUNDMAP_DEMO=<folder>`). Desktop development hooks:
`SC_MOBILE_SOUNDMAP=<map folder>` and `SC_MOBILE_SOUNDMAP_MODE`.

## 8. Not done yet

- Not tried on a phone: the iOS camera patch and the Android picture path are
  only compiled (CI). First check on a device: a picture of a tag at 7 m, the
  still's size, the position of one spot in both landscape holds (they must
  agree within about 1 cm).
- Focus lock for the map's pictures (autofocus breathing), a live tag overlay
  in the preview (not possible through JUCE on iOS without shutter sounds).
- Android's input preset: JUCE's Oboe device records with the VoiceRecognition
  preset, not Unprocessed; bands above 4 kHz are device-specific.
- A per-band comparison calibration against a sound level meter.
- Continuous scans with ARKit / ARCore anchored on the same tags; an echo map
  (the phone plays a sweep at every point); a desktop Sound Map panel with a 3D
  view; several phones recording at once.
