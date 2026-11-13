#!/usr/bin/env python3
"""Build the update manifest for a SpectroCraft release (docs/updates.md).

Walks the built application folder and writes update-manifest.json: the version,
and for every file the update installs, its size and SHA-256. The app downloads
this manifest next to the archive, checks its Ed25519 signature against the key
built into the app, and then verifies every file it unpacks against these
hashes before a single installed file is touched.

    python3 tools/make_update_manifest.py \
        --dir build/src/SpectroCraft_artefacts/Release \
        --out build/update-manifest.json

    spectrocraft-updater --sign tools/updates/ed25519.key \
        build/update-manifest.json build/update-manifest.json.sig

The manifest must describe exactly what the release archive contains, so the
file set is an explicit allow-list matching the archive layout:

    SpectroCraft.exe                      the application
    SpectroCraft/*.dll                    OpenVINO / whisper.cpp / FFmpeg runtimes
    SpectroCraft/spectrocraft-updater.exe the updater (updater/CMakeLists.txt)
    recorders/*.dll                       recorder plugins

Anything else in the folder (a .pdb, a test log, a probe executable) is not
part of the update: it is left out, and an unexpected *other* file makes this
script fail rather than silently disagree with the archive. Add a rule here and
in the CI artifact paths together.
"""

import argparse
import datetime
import hashlib
import json
import os
import sys

FORMAT = "spectrocraft-update-manifest/1"

# (relative path or glob-ish rule) -> reason. Matched in order; the first
# rule that accepts a path decides. "exact" rules are compared case-insensitively.
ALLOW = [
    ("exact", "spectrocraft.exe", None, "the application"),
    ("exact", "spectrocraft/spectrocraft-updater.exe", None, "the updater"),
    ("prefix-ext", "spectrocraft/", ".dll", "runtime library"),
    ("prefix-ext", "recorders/", ".dll", "recorder plugin"),
]

# Never part of an update, whatever the folder happens to contain.
IGNORE = [
    "*.pdb", "*.ilk", "*.exp", "*.lib", "*.log", "*.obj", "*.tmp",
    "update-result.json", "update-plan.txt", "*.old",
]


def norm(rel: str) -> str:
    return rel.replace("\\", "/").lower()


def matches_glob(name: str, pattern: str) -> bool:
    import fnmatch
    return fnmatch.fnmatch(name, pattern)


def classify(rel: str):
    """Return (included, reason) for a normalised relative path."""
    for p in IGNORE:
        if matches_glob(rel, p):
            return False, "ignored (%s)" % p
    for kind, a, b, reason in ALLOW:
        if kind == "exact" and rel == a:
            return True, reason
        if kind == "prefix-ext" and rel.startswith(a) and rel.endswith(b):
            return True, reason
    return False, "unexpected"


def sha256_of(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dir", required=True, help="built application folder to describe")
    ap.add_argument("--out", required=True, help="manifest JSON to write")
    ap.add_argument("--version", help="release version (default: read --version-file)")
    ap.add_argument("--version-file", default="build/generated/SC_VERSION.txt",
                    help="file holding the version written by cmake/bump_version.cmake")
    ap.add_argument("--generated", help="override the 'generated' timestamp (for a reproducible manifest)")
    args = ap.parse_args()

    root = os.path.abspath(args.dir)
    if not os.path.isdir(root):
        print("error: --dir is not a folder: %s" % root, file=sys.stderr)
        return 2

    version = args.version
    if not version:
        try:
            with open(args.version_file, "r", encoding="utf-8") as f:
                version = f.readline().strip()
        except OSError as e:
            print("error: no --version and cannot read %s: %s" % (args.version_file, e), file=sys.stderr)
            return 2
    parts = version.split(".")
    if len(parts) < 3 or not all(p.isdigit() for p in parts[:3]):
        print("error: not a version: %r" % version, file=sys.stderr)
        return 2

    files, skipped, unexpected = [], [], []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace("\\", "/")
            ok, reason = classify(norm(rel))
            if not ok:
                if reason == "unexpected":
                    unexpected.append(rel)
                else:
                    skipped.append(rel)
                continue
            files.append({"path": rel.replace("\\", "/"),
                          "size": os.path.getsize(full),
                          "sha256": sha256_of(full)})

    if unexpected:
        print("error: these files are in %s but no rule says whether they belong "
              "to an update:" % root, file=sys.stderr)
        for rel in unexpected:
            print("  %s" % rel, file=sys.stderr)
        print("Add a rule to tools/make_update_manifest.py (and to the CI "
              "artifact paths) or remove them.", file=sys.stderr)
        return 1

    if not any(f["path"].lower().endswith("spectrocraft.exe") for f in files):
        print("error: no SpectroCraft.exe under %s — is this the build output "
              "folder?" % root, file=sys.stderr)
        return 1

    files.sort(key=lambda f: f["path"].lower())

    generated = args.generated or datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ")

    manifest = {"format": FORMAT, "version": version, "generated": generated, "files": files}

    outdir = os.path.dirname(os.path.abspath(args.out))
    if outdir:
        os.makedirs(outdir, exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    total = sum(f["size"] for f in files)
    print("wrote %s: version %s, %d files, %.1f MB"
          % (args.out, version, len(files), total / 1e6))
    for f in files:
        print("  %10d  %s  %s" % (f["size"], f["sha256"][:16], f["path"]))
    if skipped:
        print("skipped %d file(s) (never part of an update): %s"
              % (len(skipped), ", ".join(sorted(skipped)[:6])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
