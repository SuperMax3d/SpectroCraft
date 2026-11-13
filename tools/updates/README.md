# Update signing key

Every automatic update is described by a `update-manifest.json` (file list + SHA-256 of each
file, `docs/updates.md`). SpectroCraft only unpacks it when its **Ed25519 signature** validates
against the public key built into the app — `sc::update::manifestPublicKeyHex()` in
`src/Updates.cpp`. This folder holds the key material CI signs with.

| File | |
|---|---|
| `ed25519.key` | The key CI uses: `seed=` (private, keep it secret) and `public=` (the value built into the app). |
| `run_tests_probe.cpp` | Optional Windows helper: runs `tests/regression/updates.inc` without building the whole app (see *Running the update tests without the app*). |

## The key file

```
spectrocraft-update-key/1
seed=7e3f…    ← 32 random bytes, hex. The secret.
public=3690…  ← the derived public key, the one the app trusts.
```

`public=` is never typed by hand: it is derived from the seed.

```sh
build/src/SpectroCraft_artefacts/Release/SpectroCraft/spectrocraft-updater.exe \
    --public tools/updates/ed25519.key          # print the public key of the seed
    --sign   tools/updates/ed25519.key build/update-manifest.json build/update-manifest.json.sig
    --verify tools/updates/ed25519.key build/update-manifest.json build/update-manifest.json.sig
```

(`--sign` / `--verify` / `--public` are modes of `spectrocraft-updater`, the same program the
installer runs; `--verify` exits non-zero and prints the reason, which is what makes it the CI
self-check.)

## Where it is used

`.github/workflows/build.yml` (Windows job): after the release payload is assembled,

```sh
python3 tools/make_update_manifest.py --dir build/release-payload \
        --out build/update-manifest.json --version-file build/generated/SC_VERSION.txt
spectrocraft-updater --sign   tools/updates/ed25519.key build/update-manifest.json build/update-manifest.json.sig
spectrocraft-updater --verify tools/updates/ed25519.key build/update-manifest.json build/update-manifest.json.sig
```

and both files are uploaded as artifacts; the release step publishes them next to the archive.

`tests/regression/updates.inc` reads this file and checks that it matches the key compiled into
the app: with `seed=` present it derives the public key and compares, with only `public=` present
it compares that line. A rotation that forgets `src/Updates.cpp` fails the test suite.

## What the signature is worth

The signature proves *who produced the manifest*, not that GitHub is honest. With the seed
committed here, anyone who can read the repository can also sign a manifest — so today it guards
against a mismatched or tampered manifest/archive pair (a wrong archive, a manifest from another
release, a corrupted download), which is the failure mode the flow actually has to catch.

To also guard against someone who can replace a release's assets, keep the seed out of the
repository:

1. Generate a new seed (see below), put its public key in `src/Updates.cpp`
   (`manifestPublicKeyHex`) and in `ed25519.key` as `public=` only — delete the `seed=` line.
2. Store the seed as a repository secret (e.g. `SC_UPDATE_SEED`) and have the CI job write it to a
   temporary key file before signing:
   ```yaml
   - name: Write signing key
     shell: bash
     run: |
       printf 'spectrocraft-update-key/1\nseed=%s\n' "$SC_UPDATE_SEED" > build/sign.key
     # with SC_UPDATE_SEED: ${{ secrets.SC_UPDATE_SEED }} on the step
   ```
   and sign with `build/sign.key`.
3. Old releases keep their own `public=` in `ed25519.key.<n>` (see *Rotating*) — the app only
   ever trusts the single key it was built with, so a rotated key means new releases only.

## Generating a seed

Any 32 random bytes will do; the public key is derived, never chosen:

```sh
python3 -c "import os; print('seed=' + os.urandom(32).hex())" > new.key
spectrocraft-updater --public new.key      # → the public= line to add
```

Nothing else about the key is configurable: Ed25519 (RFC 8032), the implementation is
`include/Ed25519.h` / `src/Ed25519.cpp` (no external crypto library, shared with the updater).

## Rotating

1. Generate a new seed and derive its public key.
2. Replace the string in `src/Updates.cpp` (`manifestPublicKeyHex`) — this is the trust decision,
   it ships in the app.
3. Rename the current file to `ed25519.key.1` (kept so an older release's manifest can still be
   verified by hand or in a test) and write the new `seed=`/`public=` into `ed25519.key`.
4. Build and run the update tests: `SC_TEST_FILTER=updates` must pass (it asserts the pairing).
5. Releases published with the old key are no longer installable by builds made from the new one —
   rotate at a release boundary, and keep the old key file in the repository.

## Running the update tests without the app

The suite is part of `--run-unit-tests` (`tests/regression/updates.inc`). For a quick check of the
manifest / staging / plan / result logic without a full CMake build, `run_tests_probe.cpp` compiles
just `src/Updates.cpp`, `src/Ed25519.cpp`, `src/AI/Sha256.cpp` and the JUCE objects already in the
build tree:

```bat
cl /std:c++latest /MDd /EHsc /DJUCE_MODAL_LOOPS_PERMITTED=1 ^
   /I extern\JUCE\modules /I . /I build\src\SpectroCraft_artefacts\JuceLibraryCode /I build\generated /I include ^
   /Feupd_tests.exe tools\updates\run_tests_probe.cpp src\Updates.cpp src\Ed25519.cpp src\AI\Sha256.cpp ^
   build\src\SpectroCraft.dir\Debug\juce_*.obj shell32.lib comdlg32.lib ole32.lib oleaut32.lib user32.lib gdi32.lib wldap32.lib winmm.lib
```

It is a developer probe (Windows, needs a built tree), not part of the CI test run.
