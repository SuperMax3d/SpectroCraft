# Check for Updates

SpectroCraft can fetch the latest GitHub release, verify it, download it, and replace its own
files with the help of a small separate console program. **Windows, macOS and Linux** (issue
#37; Windows since 1.0.40): what differs between them is listed in *Platforms*.

Code: `include/Updates.h`, `src/Updates.cpp` (lookup, download, verify, stage),
`include/Ed25519.h` + `src/Ed25519.cpp` (signature check), `updater/main.cpp`
(`spectrocraft-updater`), `src/MainWindow.cpp` (menu, dialogs, hand-off).
Release tools: `tools/make_release_payload.py` (macOS / Linux archive payload),
`tools/make_update_manifest.py` (manifest), keys: `tools/updates/`.
Tests: `tests/regression/updates.inc` (`SC_TEST_FILTER=updates`).

## What the user sees

| | |
|---|---|
| *Help > Check for Updates…* | Looks up the latest release. Up to date → a short confirmation. Newer → a dialog with the version, the asset name and size, the release notes in a read-only scrollable field of fixed size (so long notes never push the buttons off the window), and **[Update Now] / [Later]**. |
| *Help > Check for Updates at Startup* | Toggle, saved (`check_updates_startup`, off by default). When on, the same lookup runs a few seconds after start, **at most once a day**, and stays silent unless a newer release exists. |
| Progress | A small dialog with a progress bar: manifest → signature → download → unpack and hash. **[Cancel]** at any point; nothing in the install folder was touched. |
| Install | After everything is verified the app asks to close (only if documents have unsaved changes, listing them), then quits. The updater replaces the files and starts the new version. |
| After the install | The new version reports the result: *Updated — SpectroCraft 1.0.2300 is installed, N files were replaced*, or *Update failed* with the reason and the note that the previous version is still in place. |

A startup check never nags: a failed lookup (offline, GitHub down) only goes to the diagnostic
log (`update / lookup_failed`), and a version the user already answered *[Later]* to is not
offered again (`update_skipped_version`).

## What a release must publish

Three assets per platform on the GitHub release (`SuperMax3d/SpectroCraft`):

| | Archive | Manifest + signature |
|---|---|---|
| Windows | `SpectroCraft-<version>-windows-x64.zip` | `update-manifest.json`, `update-manifest.json.sig` |
| macOS | `SpectroCraft-<version>-macos-arm64.zip` | `update-manifest-macos-arm64.json`, `….json.sig` |
| Linux | `SpectroCraft-<version>-linux-x64.zip` | `update-manifest-linux-x64.json`, `….json.sig` |

1. **The archive** — `SpectroCraft-<version>-<os>-<arch>.zip`, or the older
   `SpectroCraft-<os>-<arch>.zip`; the app prefers the versioned name and falls back, then falls
   back to any `.zip` whose name contains both the platform and the architecture token.
   `pickAsset` in `src/Updates.cpp`.
2. **The manifest** — the file list with sizes and SHA-256 hashes (*The manifest*). Windows keeps
   the name `update-manifest.json` (every Windows version since 1.0.40 looks for it); macOS and
   Linux have their own names (`manifestAssetName()`), so the three sit in one release.
3. **Its `.sig`** — the Ed25519 signature of those bytes, hex (`spectrocraft-updater --sign`,
   whitespace tolerated).

The workflow publishes artifacts, and the GitHub release is created from them:

| Artifact | Holds | Becomes |
|---|---|---|
| `SpectroCraft-windows-x64…` | the payload folder | the Windows zip |
| `SpectroCraft-macos-arm64…` | `SpectroCraft-macos-arm64.tar.gz` (`SpectroCraft.app`) | the macOS zip |
| `SpectroCraft-linux-x64…` | `SpectroCraft-linux-x64.tar.gz` (`Debug/`) | the Linux zip |
| `update-manifest-windows…` / `-macos…` / `-linux…` | the manifest and its `.sig` | uploaded as they are |

The manifest is made from the same payload as the archive, so the zip must hold exactly the
files of the payload, unchanged (the hashes are checked). Two things matter when the macOS /
Linux zip is made from the tar:

- **No symbolic links.** The payload has none (`tools/make_release_payload.py` turns the
  library links the program loads into files), and the app refuses a link in the zip.
- **The Unix permissions** should be kept (`zip -X -r`, or Python's `zipfile` with
  `ZipInfo.external_attr = mode << 16`), so a first install from the zip starts. A zip without
  them still updates: the app sets the executable bits from the manifest. But someone who
  downloads it by hand gets a program that will not start.

A release without the manifest pair for a platform cannot be installed automatically there — the
app says so and offers the releases page instead.

## The manifest

Generated from the *release payload* (the folder that becomes the archive), never from the raw
build folder, so the list always matches the archive exactly:

```sh
python3 tools/make_update_manifest.py --dir build/release-payload \
        --out build/update-manifest.json --version-file build/generated/SC_VERSION.txt
spectrocraft-updater --sign tools/updates/ed25519.key build/update-manifest.json \
                     build/update-manifest.json.sig
spectrocraft-updater --verify tools/updates/ed25519.key build/update-manifest.json \
                     build/update-manifest.json.sig      # the CI self-check
```

```json
{
  "format": "spectrocraft-update-manifest/1",
  "version": "1.0.2300",
  "generated": "2025-01-01T12:00:00Z",
  "files": [
    { "path": "SpectroCraft.exe", "size": 4194304, "sha256": "…" },
    { "path": "SpectroCraft/spectrocraft-updater.exe", "size": 245760, "sha256": "…" }
  ]
}
```

`path` is archive-relative with forward slashes. On Windows the generator only accepts the
release layout (`SpectroCraft.exe`, `SpectroCraft/*.dll`, `SpectroCraft/spectrocraft-updater.exe`,
`recorders/*.dll`) and fails on anything else, so a stray build file never ends up in a manifest.

macOS / Linux manifests (`--platform macos|linux`) describe the payload made by
`tools/make_release_payload.py`, and carry three more fields:

```json
{
  "format": "spectrocraft-update-manifest/1",
  "version": "1.0.2300",
  "generated": "2025-01-01T12:00:00Z",
  "platform": "macos-arm64",
  "root": "SpectroCraft.app",
  "files": [
    { "path": "SpectroCraft.app/Contents/MacOS/SpectroCraft", "size": 118599936,
      "sha256": "…", "executable": true },
    { "path": "SpectroCraft.app/Contents/Frameworks/libggml.0.dylib", "size": 60288, "sha256": "…" }
  ]
}
```

- `platform` — `<os>-<arch>`; the app refuses a manifest for another platform (Windows manifests
  have it too, `windows-x64`; older manifests without it are checked by their layout only).
- `root` — the archive's one top folder. Every path starts with it, and the app installs
  `<root>/<rest>` as `<install folder>/<rest>`: a bundle the user renamed to
  `SpectroCraft 2.app` is updated in place.
- `executable` — the file had its executable bit in the payload. The app sets it again after
  unpacking (a zip written without Unix permissions loses it).

`parseManifest` on the app side refuses: an unknown `format`, a `version` that is not a plain
three-part version, a path that is absolute or contains `..`, a duplicate path, a missing or
malformed `sha256`, a `root` that is not a single folder name, a path outside the `root`.
`checkManifestPlatform` then refuses a manifest for another platform, or one without the
application or the updater where this platform has them.

## What is trusted, and where

| Step | Check | Fails → |
|---|---|---|
| Release lookup | HTTPS only to `api.github.com/repos/SuperMax3d/SpectroCraft/releases/latest`, `User-Agent: SpectroCraft/<version>`, redirects followed | *The update check did not work: …* |
| Version | numeric parts of `tag_name` vs `SC_VERSION_STRING` (`compareVersions`; `1.0.2300-rc1` compares as `1.0.2300`) | *You are using the newest release* |
| Manifest | Ed25519 signature over the exact bytes, verified with the public key **built into the app** (`manifestPublicKeyHex()` in `src/Updates.cpp`) | *The update manifest could not be verified* |
| Manifest ↔ release | the manifest version equals the release tag, and is newer than the running version | *The manifest belongs to version …* |
| Manifest ↔ computer | `platform` is this `<os>-<arch>`, the application and the updater are where this platform has them (`checkManifestPlatform`) | *The update manifest does not fit this computer: …* |
| Archive | every manifest entry appears in the zip with the right **size** and **SHA-256**, and the zip contains nothing the manifest does not list (folder entries and `__MACOSX/` metadata aside); a symbolic link is refused | *The downloaded archive does not match the manifest* |

The signature is checked **before** the archive is downloaded (the manifest is a few KB, so a bad
release fails early), and the hashes are checked **while** unpacking, into a staging folder —
nothing in the install folder changes until every file has been verified.

The private key and how it is rotated: `tools/updates/README.md`.
`tests/regression/updates.inc` asserts that the key file's public key is the one built into the
app, so a key rotation that forgets `Updates.h` fails a test.

## The swap

Downloads and staging live in `<temp>/SpectroCraft/update` (`updateTempDir()`): never inside the
install folder, never inside `%APPDATA%/SpectroCraft` (settings, models, caches).

The staging folder has the layout of the install folder: a macOS file
`SpectroCraft.app/Contents/MacOS/SpectroCraft` is staged as `<staging>/Contents/MacOS/SpectroCraft`
(the manifest's `root` is dropped).

The app writes `<staging>/update-plan.txt` — the manifest reduced to what the updater needs, since
the updater has no JSON parser and the app has already checked everything:

```
spectrocraft-update-plan/1
version=1.0.2300
file=SpectroCraft.exe
file=SpectroCraft/spectrocraft-updater.exe
…
```

in manifest order, install-relative. Then it launches the updater and quits:

```
<staging>/<updaterPath()> \
    --pid <app pid> --install-dir <install folder> --staging-dir <staging> \
    --relaunch <SpectroCraft.exe | SpectroCraft.app | SpectroCraft>
```

The updater (`updater/main.cpp`, plain C++20, no JUCE) waits for the app's PID to disappear
(bounded, `--wait-ms`), checks that the install folder is writable, then for every planned file:
move the old file to `<name>.old`, copy the new one in. Any failure (missing file, locked file,
disk full) restores every `.old` it already moved — the previous version comes back — writes
`<install>/update-result.json`, and still relaunches the app. On success it relaunches with
`--finish-update`. A failure before the swap (the plan or a staged file missing) also relaunches
the previous version once the app has gone, so the user is told instead of left without a
program; only a timeout waiting for the app does not (the app is still running).

On the next start the app reads and deletes `update-result.json` (`readUpdateResult`), reports it
(*After the install* above), and removes what the updater could not: the `.old` backups (one of
them is the updater's own running image) and the staging folder. A result file is only read once.

`update-result.json`:

```json
{ "success": true, "version": "1.0.2300", "replaced": ["SpectroCraft.exe", …],
  "oldFiles": ["C:\\…\\SpectroCraft.exe.old", …],
  "cleanupDirs": ["C:\\…\\SpectroCraft\\update"],
  "reason": "", "failedFile": "" }
```

## Failure cases

| Situation | Result |
|---|---|
| No network / GitHub error | *The update check did not work: …* (manual) or nothing at all (startup, diagnostic log only) |
| Release without a matching `.zip` | *This release does not publish a … archive*, with the releases page |
| Release without manifest or `.sig` | *This release does not publish update-manifest.json*, nothing is downloaded |
| Bad signature / manifest from another version | *The update manifest could not be verified / belongs to version …* |
| Damaged or unexpected archive | *The downloaded archive does not match the manifest: …* |
| Cancel at any stage | *The update was cancelled. Nothing was changed.* |
| Install folder not writable (Program Files without permission) | *… this folder is not writable*, nothing is downloaded |
| Updater fails mid-swap | *Update failed* + the reason; the old files are back in place |
| Updater cannot be launched | *The update could not be started*, the app stays open |

In every case the install folder is either untouched or restored; the app never runs a half-installed
mix of two versions.

## Settings and files

| Key (`recentStore`) | Meaning |
|---|---|
| `check_updates_startup` | *Check for Updates at Startup* (bool, default off) |
| `update_last_check_ms` | When the last lookup was **attempted** — an offline start does not retry on the next launch |
| `update_skipped_version` | The version the user answered *[Later]* to |

Command-line: `SpectroCraft --finish-update` (used by the updater; the app reports the result and
removes the leftovers). Such a start never hands itself to another running instance, even with
*Allow multiple instances* off (#92): the updater relaunched it precisely so that it becomes the
running app. `spectrocraft-updater --help` lists its modes (`--sign`, `--verify`,
`--public` besides the swap).

## Platforms

| | Windows | macOS | Linux |
|---|---|---|---|
| Install folder (`installDir()`) | the folder holding `SpectroCraft.exe` | the `SpectroCraft.app` bundle (any name) | the folder holding `SpectroCraft` |
| Archive top folder (`root`) | none | `SpectroCraft.app` | `Debug` |
| Updater (`updaterPath()`) | `SpectroCraft/spectrocraft-updater.exe` | `Contents/MacOS/spectrocraft-updater` | `spectrocraft-updater` |
| Started by | `CreateProcessW`, own console | `posix_spawn`, output in `<temp>/SpectroCraft/update-log.txt` | same as macOS |
| Relaunch | the exe, without a console (`DETACHED_PROCESS`); the app also leaves the console JUCE attaches it to, when it sees `--finish-update` (#49: closing the updater's console window used to end the updated app) | `/usr/bin/open -n <bundle> --args --finish-update` | the executable |
| `.old` backups | removed by the app on the next start | removed by the updater once every file is in place | same as macOS |

macOS and Linux details:

- **The updater detaches** from the app: its own session (closing the terminal the app was
  started from does not stop it), SIGPIPE and SIGHUP ignored, no inherited file descriptors
  (`POSIX_SPAWN_CLOEXEC_DEFAULT` on macOS; the updater closes them on Linux), so the app's control
  socket does not keep its port busy for the relaunched app.
- **Another file system.** The staging folder is in the temporary folder: on Linux `/tmp` is
  often a tmpfs, and the app may live on an external disk. When `rename()` cannot cross, the
  updater copies to `<target>.new` in the same folder (same permission bits), then renames it
  over the target: a file is never half written. On macOS this also gives the new executable a
  new inode, which matters because the kernel caches code signatures per inode.
- **Unwritable install** (`firstUnwritableFolder`, checked before anything is downloaded):
  - on macOS, App Translocation (an app opened straight from the download runs from a read-only
    copy) gets a message saying to move `SpectroCraft.app` to Applications;
  - a disk image gets its own message;
  - otherwise the message says to move the program to a folder the user owns.
  
  No elevation is attempted anywhere.
- **Code signature (macOS).** Today's bundle is ad hoc signed by the linker, file by file, and
  is not notarised: replacing the files with the ones of the next build keeps every binary
  valid, and a file the app downloads and unpacks itself carries no quarantine attribute. A
  Developer ID signed and notarised bundle would rather be swapped as a whole (sealed resources);
  that is for when releases are signed.

## Building and testing

```sh
cmake --build build --target SpectroCraft             # builds spectrocraft-updater with it
ctest --test-dir build -R updates                    # or:
SC_TEST_FILTER=updates build/src/SpectroCraft_artefacts/Debug/SpectroCraft --run-unit-tests

# a macOS / Linux release payload and its signed manifest, as CI makes them
python3 tools/make_release_payload.py --platform macos --src build/src/SpectroCraft_artefacts/Debug \
        --out build/release-payload
python3 tools/make_update_manifest.py --platform macos --dir build/release-payload \
        --out build/update-manifest-macos-arm64.json --version-file build/generated/SC_VERSION.txt
```

`tests/regression/updates.inc` covers:

- the crypto: the RFC 8032 Ed25519 vectors, a sign/verify round trip with the repository key,
  SHA-512/SHA-256 vectors;
- version comparison, asset picking, the release JSON;
- manifest parsing and signature verification;
- staging with every failure mode (missing file, extra file, wrong size, wrong hash, cancel);
- the plan file and reading a result file;
- that the update folders are never inside the install folder or the settings folder;
- the macOS / Linux layouts:
  - manifest asset names, `root`, `platform`, `executable`;
  - a zip with folder entries and `__MACOSX/` metadata staged without its root, with its
    executable bits back;
  - the platform check, unwritable folders, the advice texts;
- the real `spectrocraft-updater` on a fake install:
  - files swapped, permissions kept, backups removed;
  - a failure halfway rolled back;
  - a missing staged file changing nothing.
