# Check for Updates

SpectroCraft can fetch the latest GitHub release, verify it, download it, and replace its own
files with the help of a small separate console program. **Windows only for now**: the menu item
and the whole flow are guarded by `JUCE_WINDOWS` (the code compiles everywhere, and the manifest
format is platform-neutral — see *Platform scope*).

Code: `include/Updates.h`, `src/Updates.cpp` (lookup, download, verify, stage),
`include/Ed25519.h` + `src/Ed25519.cpp` (signature check), `updater/main.cpp`
(`spectrocraft-updater.exe`), `src/MainWindow.cpp` (menu, dialogs, hand-off).
Manifest tool: `tools/make_update_manifest.py`, keys: `tools/updates/`.
Tests: `tests/regression/updates.inc` (`SC_TEST_FILTER=updates`).

## What the user sees

| | |
|---|---|
| *Help > Check for Updates…* | Looks up the latest release. Up to date → a short confirmation. Newer → a dialog with the version, the release notes (first lines of the release body), the asset name and size, and **[Update Now] / [Later] / Open releases page**. |
| *Help > Check for Updates at Startup* | Toggle, saved (`check_updates_startup`, off by default). When on, the same lookup runs a few seconds after start, **at most once a day**, and stays silent unless a newer release exists. |
| Progress | A small dialog with a progress bar: manifest → signature → download → unpack and hash. **[Cancel]** at any point; nothing in the install folder was touched. |
| Install | After everything is verified the app asks to close (only if documents have unsaved changes, listing them), then quits. The updater replaces the files and starts the new version. |
| After the install | The new version reports the result: *Updated — SpectroCraft 1.0.2300 is installed, N files were replaced*, or *Update failed* with the reason and the note that the previous version is still in place. |

A startup check never nags: a failed lookup (offline, GitHub down) only goes to the diagnostic
log (`update / lookup_failed`), and a version the user already answered *[Later]* to is not
offered again (`update_skipped_version`).

## What a release must publish

Three assets on the GitHub release (`SuperMax3d/SpectroCraft`):

1. **The archive** — `SpectroCraft-<version>-<os>-<arch>.zip`, or the older
   `SpectroCraft-<os>-<arch>.zip` (e.g. `SpectroCraft-windows-x64.zip`); the app prefers the
   versioned name and falls back, then falls back to any `.zip` whose name contains both the
   platform and the architecture token. `pickAsset` in `src/Updates.cpp`.
2. **`update-manifest.json`** — the file list with sizes and SHA-256 hashes (*The manifest*).
3. **`update-manifest.json.sig`** — the Ed25519 signature of those bytes, hex
   (`spectrocraft-updater --sign`, whitespace tolerated).

The archive layout is the install layout, so the swap is a straight copy:

```
SpectroCraft.exe
SpectroCraft/spectrocraft-updater.exe     (+ the runtime DLLs: OpenVINO, whisper.cpp, FFmpeg, …)
recorders/*.dll
```

The workflow itself only publishes artifacts (`SpectroCraft-windows-x64…` = the payload folder,
`update-manifest-windows…` = the two manifest files); the GitHub release is created from them, and
the release must carry all three assets. A release without the manifest pair cannot be installed
automatically — the app says so and offers the releases page instead.

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

`path` is archive-relative with forward slashes. The generator only accepts the release layout
(`SpectroCraft.exe`, `SpectroCraft/*.dll`, `SpectroCraft/spectrocraft-updater.exe`,
`recorders/*.dll`) and fails on anything else, so a stray build file never ends up in a manifest.

`parseManifest` on the app side refuses: an unknown `format`, a `version` that is not a plain
three-part version, a path that is absolute or contains `..`, a duplicate path, a missing or
malformed `sha256`.

## What is trusted, and where

| Step | Check | Fails → |
|---|---|---|
| Release lookup | HTTPS only to `api.github.com/repos/SuperMax3d/SpectroCraft/releases/latest`, `User-Agent: SpectroCraft/<version>`, redirects followed | *The update check did not work: …* |
| Version | numeric parts of `tag_name` vs `SC_VERSION_STRING` (`compareVersions`; `1.0.2300-rc1` compares as `1.0.2300`) | *You are using the newest release* |
| Manifest | Ed25519 signature over the exact bytes, verified with the public key **built into the app** (`manifestPublicKeyHex()` in `src/Updates.cpp`) | *The update manifest could not be verified* |
| Manifest ↔ release | the manifest version equals the release tag, and is newer than the running version | *The manifest belongs to version …* |
| Archive | every manifest entry appears in the zip with the right **size** and **SHA-256**, and the zip contains nothing the manifest does not list | *The downloaded archive does not match the manifest* |

The signature is checked **before** the archive is downloaded (the manifest is a few KB, so a bad
release fails early), and the hashes are checked **while** unpacking, into a staging folder —
nothing in the install folder changes until every file has been verified.

The private key and how it is rotated: `tools/updates/README.md`.
`tests/regression/updates.inc` asserts that the key file's public key is the one built into the
app, so a key rotation that forgets `Updates.h` fails a test.

## The swap

Downloads and staging live in `<temp>/SpectroCraft/update` (`updateTempDir()`): never inside the
install folder, never inside `%APPDATA%/SpectroCraft` (settings, models, caches).

The app writes `<staging>/update-plan.txt` — the manifest reduced to what the updater needs, since
the updater has no JSON parser and the app has already checked everything:

```
spectrocraft-update-plan/1
version=1.0.2300
file=SpectroCraft.exe
file=SpectroCraft/spectrocraft-updater.exe
…
```

in manifest order. Then it launches the updater and quits:

```
<staging>/SpectroCraft/spectrocraft-updater.exe \
    --pid <app pid> --install-dir <install folder> --staging-dir <staging> \
    --relaunch <SpectroCraft.exe>
```

The updater (`updater/main.cpp`, plain C++20, no JUCE) waits for the app's PID to disappear
(bounded, `--wait-ms`), checks that the install folder is writable, then for every planned file:
move the old file to `<name>.old`, copy the new one in. Any failure (missing file, locked file,
disk full) restores every `.old` it already moved — the previous version comes back — writes
`<install>/update-result.json`, and still relaunches the app. On success it relaunches with
`--finish-update`.

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
removes the leftovers). `spectrocraft-updater --help` lists its modes (`--sign`, `--verify`,
`--public` besides the swap).

## Platform scope

v1 is Windows: `installDir()` is the folder holding `SpectroCraft.exe`, the swap uses
`CreateProcessW`, and the menu is only built under `JUCE_WINDOWS`. macOS/Linux would need the
`.app` bundle layout (or an AppImage) and a different staging plan; the manifest format, the
signature scheme and the updater's swap logic are not Windows-specific. No elevation is attempted:
if the install folder is not writable the update stops before anything is downloaded.

## Building and testing

```sh
cmake --build build --target spectrocraft-updater     # the console program
ctest --test-dir build -R updates                    # or:
SC_TEST_FILTER=updates build/src/SpectroCraft_artefacts/Debug/SpectroCraft --run-unit-tests
```

`tests/regression/updates.inc` covers: the RFC 8032 Ed25519 vectors and a sign/verify round trip
with the repository key, SHA-512/SHA-256 vectors, version comparison, asset picking, the release
JSON, manifest parsing and signature verification, staging with every failure mode (missing file,
extra file, wrong size, wrong hash, cancel), the plan file, reading a result file, and that the
update folders are never inside the install folder or the settings folder.
