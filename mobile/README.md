# SpectroCraft Mobile

The iOS and Android companion app (issue #73): record, see and measure sound on a phone, with the
desktop's spectrogram, measurements and file format.

It is a JUCE app built from the desktop's own engine sources (`../src`: `AudioEngine`,
`SpectralEngine`, `Measures`, `FileDoc`, `FileMeta`, ...) plus a touch UI in `mobile/src`, so the
picture, the levels and the WAV metadata are the desktop's. The desktop build does not change.

## Features

| | |
|---|---|
| Views | Waveform, spectrogram, or both (top bar W / S / W+S). Pinch to zoom (horizontal = time, vertical = frequency on the spectrogram, amplitude on the waveform), two fingers to pan, double tap to show all. One-finger drag selects a time range; a tap sets the playhead. |
| Files | Open a WAV (the recordings list, or *Open...* for the Files app / the Android file picker), save, share (with its video), delete. Recordings go to the app's Documents folder (visible in the iOS Files app). |
| Recording | *Unlimited* (until the memory limit shown next to the time, then the take is kept) or *Loop N s* (keeps the last N seconds; the saved file starts with the oldest sample). Every take is saved when it stops. |
| Microphone | Built-in, USB or Bluetooth (*Input* tab), mono or stereo, sample rate. The input runs in measurement mode (no automatic gain or noise suppression). |
| Calibration | Per input (*Calibrate* tab): typed (the dB-SPL a full-scale signal stands for) or with an acoustic calibrator (default 1 kHz, 94 dB-SPL; any level and frequency can be typed). The calibration goes into every new recording, so the desktop shows it in dB-SPL. |
| Analysis | *Analyse* tab: FFT Linear, FFT Log and 1/3 octave of the selection (the visible range when nothing is selected, the last second while recording), with RMS and peak, through the desktop's `sc::measures`. *Store* keeps the curve (8 slots); stored plots are saved in the file. |
| Generator | *Generate* tab: manual tone, linear or log sweep (repeating or one-shot), white / pink / brown / custom noise (slope in dB/octave, band edges). It plays while recording. Level = RMS dBFS. |
| Editing | *Edit* tab: delete the selection, change its gain (or the whole file's), undo / redo; display options (log / linear frequency, dB range, channel). |
| Video | *Video* films with the recording (picture only: the WAV is the sound). The movie is saved next to the WAV with the same name and listed in the WAV's metadata with its offset, so the desktop opens it with the recording. |

## Crash reports and statistics

When the build has keys (see *CI and store builds*), the app sends:

- **crash reports** to Sentry: crashes in Java / Objective-C and in the C++ code (Android: `sentry-android`
  with its NDK part; iOS: the `SentryObjC` framework), plus a few non-fatal errors with generic text
  ("recording: the take could not be saved");
- **usage statistics** to PostHog: events such as `recording_started` (mode, loop length, channels,
  sample rate, calibrated, video), `recording_stopped` (duration), `file_opened`, `calibration` (method,
  result code), `analysis_settings`, `plot_stored`, `generator_toggled`, `input_changed` (built-in /
  USB / Bluetooth), `edit`, `video_enabled`. Each carries a random install id, the app version, the OS
  and its version, and the phone model. PostHog keeps no person profile (`$process_person_profile`
  false).

Never sent: audio, file names or paths, device names, location, account data.

Both are on by default when compiled in. A notice at first launch says so, and *Files > Send anonymous
crash reports and usage statistics* turns them off (and back on). A build without keys (local builds,
pull requests from forks) contains neither and shows no toggle.

What to declare to the stores:

- **Google Play, Data safety**: *App info and performance > Crash logs and Diagnostics*, and *App
  activity > App interactions*: collected, not shared, processed ephemerally: no, optional: yes
  (the user can turn it off), not linked to an identity.
- **App Store, App Privacy**: *Diagnostics > Crash Data*, *Other Diagnostic Data*; *Usage Data >
  Product Interaction*: not linked to the user, not used for tracking.

## Files and the desktop

- The WAV is the desktop's format: float32, with the `SCMD` metadata chunk written by the desktop's
  own `FileDoc` / `FileMeta` code.
- Calibration: the `scale` key (Pascal, full-scale value), the same as the desktop's Scale form.
- Video: a `videoTracks` entry with `relFile` (the movie's path relative to the WAV). The desktop
  resolves it next to the WAV (`sc::resolveVideoTracks`), so copying the two files to a computer is
  enough. It now also writes `relFile` itself, so a folder moved with its movies keeps them.
- Stored plots: the `storedPlots` key (the curves themselves). The desktop keeps the key through
  its saves; it does not show the plots yet.

## Building

```sh
# Unit tests and a phone-sized desktop window (macOS / Linux)
cmake -S mobile -B build-mobile -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-mobile --target SpectroCraftMobileTests SpectroCraftMobile
build-mobile/SpectroCraftMobileTests_artefacts/Debug/SpectroCraftMobileTests

# iOS (Xcode project)
cmake -S mobile -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
      -DCMAKE_XCODE_GENERATE_SCHEME=ON -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=<team id>
open build-ios/SpectroCraftMobile.xcodeproj      # run on a device or a simulator

# Android (Gradle 8.9+, JDK 17, Android SDK with an NDK)
gradle -p mobile/android assembleRelease -PndkVersion=<installed NDK>
# -> mobile/android/app/build/outputs/apk/release/app-release.apk
```

Options: `-DSC_MOBILE_BUNDLE_ID=` (iOS bundle id / Android application id, default
`com.spectrocraft.mobile`), `-DSC_MOBILE_BUILD_NUMBER=`, `-DSC_MOBILE_APP=OFF` (tests only),
`-DSC_SENTRY_DSN=`, `-DSC_POSTHOG_KEY=`, `-DSC_POSTHOG_HOST=` (Gradle: `-PsentryDsn=`, `-PposthogKey=`,
`-PposthogHost=`).

Desktop development hooks (macOS / Linux builds only): `SC_MOBILE_RECORDINGS=<folder>`,
`SC_MOBILE_NO_AUDIO=1`, `SC_MOBILE_OPEN=<wav>`, `SC_MOBILE_TAB=<0..5>`, `SC_MOBILE_SIZE=<w>x<h>`,
`SC_MOBILE_SNAPSHOT=<png>` (writes the window after 2.5 s and quits).

## CI and store builds

`.github/workflows/mobile.yml` runs the unit tests, builds a release APK (artifact
`SpectroCraftMobile-android`) and an iOS simulator build. Without secrets the APK is signed with
the debug key: it installs by hand ("install unknown apps"), not on Google Play.

Store builds need these repository secrets (Settings > Secrets and variables > Actions) and run on
*Run workflow* with *upload* ticked, on pushes to main, or on every push when the repository variable
`SC_MOBILE_UPLOAD` is `true` (GitHub offers *Run workflow* only once the workflow is on the default
branch, so a fork testing a branch uses the variable):

| Secret | |
|---|---|
| `SC_ANDROID_KEYSTORE_BASE64` | the upload keystore, PKCS12 (`base64 -i upload.p12`; set the variable `SC_KEYSTORE_TYPE=jks` in the workflow env for a JKS file) |
| `SC_ANDROID_KEYSTORE_PASSWORD`, `SC_ANDROID_KEY_ALIAS`, `SC_ANDROID_KEY_PASSWORD` | its passwords and alias |
| `SC_PLAY_SERVICE_ACCOUNT_JSON` | a Google Play service account allowed to release; the AAB goes to the *internal testing* track. The app must exist in the Play Console (the first AAB is uploaded by hand). |
| `SC_APPLE_TEAM_ID` | the Apple developer team |
| `SC_APPSTORE_KEY_ID`, `SC_APPSTORE_ISSUER_ID`, `SC_APPSTORE_KEY_P8` | an App Store Connect API key (role App Manager or Admin) and its `.p8` contents; Xcode signs automatically and uploads to TestFlight. The bundle id must be registered and the app created in App Store Connect. |

Repository variables (Settings > Secrets and variables > Actions > Variables):

| Variable | |
|---|---|
| `SC_MOBILE_BUNDLE_ID` | the bundle / application id |
| `SC_SENTRY_DSN` | the Sentry project's DSN: crash reports are compiled in |
| `SC_POSTHOG_KEY`, `SC_POSTHOG_HOST` | the PostHog project API key (`phc_...`) and host (default `https://us.i.posthog.com`): statistics are compiled in |
| `SC_SENTRY_ORG`, `SC_SENTRY_PROJECT` | with the secret `SC_SENTRY_AUTH_TOKEN` (a Sentry auth token allowed to upload debug files): the debug symbols are uploaded so native crashes show file and line |

The DSN and the PostHog key end up inside the app anyway (they are not secrets), but keeping them out of
the code means a fork or a pull request never reports to them.

## Limits

- Bluetooth headset microphones (HFP) record at 8 or 16 kHz: the spectrum stops at 4 or 8 kHz. Use
  the built-in or a USB microphone for measurements.
- *Unlimited* recording keeps the take in memory (a quarter of the phone's RAM, at most 1.5 GB: about
  2 hours mono at 48 kHz on a recent phone). The limit is shown while recording.
- The video's offset comes from the clocks of the first audio and video frames (a few tens of ms).
- Android 10 (API 29) or later: JUCE 9's Android font code needs it. iOS 16 or later.
