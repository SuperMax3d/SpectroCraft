#!/usr/bin/env python3
"""Build the update manifest for a SpectroCraft release (docs/updates.md).

Walks the built application folder and writes update-manifest.json: the version,
and for every file the update installs, its size and SHA-256. The app downloads
this manifest next to the archive, checks its Ed25519 signature against the key
built into the app, and then verifies every file it unpacks against these
hashes before a single installed file is touched.

    python3 tools/make_update_manifest.py \
        --dir build/release-payload \
        --out build/update-manifest.json

    spectrocraft-updater --sign tools/updates/ed25519.key \
        build/update-manifest.json build/update-manifest.json.sig

The manifest must describe exactly what the release archive contains.

Windows (--platform windows, the default; asset update-manifest.json): the
file set is an explicit allow-list matching the archive layout:

    SpectroCraft.exe                      the application
    SpectroCraft/*.dll                    OpenVINO / whisper.cpp / FFmpeg runtimes
    SpectroCraft/spectrocraft-updater.exe the updater (updater/CMakeLists.txt)
    recorders/*.dll                       recorder plugins

Anything else in the folder (a .pdb, a test log, a probe executable) is not
part of the update: it is left out, and an unexpected *other* file makes this
script fail rather than silently disagree with the archive. Add a rule here and
in the CI artifact paths together.

macOS / Linux (--platform macos|linux; assets update-manifest-macos-arm64.json,
update-manifest-linux-x64.json): --dir is the payload made by
tools/make_release_payload.py, one top folder (SpectroCraft.app, Debug) that
the manifest names as its "root". Every file under it belongs to the release;
the app installs <root>/<path> as <bundle or program folder>/<path>, whatever
the user renamed the folder to. Files with an executable bit are marked
"executable": true, and the app sets that bit again when it unpacks them (a
zip written without Unix permissions loses it).
"""

import argparse
import datetime
import hashlib
import json
import os
import stat
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


# The program and the updater each platform's archive must contain, relative to
# the root folder.
REQUIRED = {
    "macos": ["Contents/MacOS/SpectroCraft", "Contents/MacOS/spectrocraft-updater"],
    "linux": ["SpectroCraft", "spectrocraft-updater"],
}

DEFAULT_ARCH = {"windows": "x64", "macos": "arm64", "linux": "x64"}


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


def collect_windows(root: str):
    """The Windows allow-list (see the module docstring)."""
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
            files.append({"path": rel,
                          "size": os.path.getsize(full),
                          "sha256": sha256_of(full)})

    if unexpected:
        print("error: these files are in %s but no rule says whether they belong "
              "to an update:" % root, file=sys.stderr)
        for rel in unexpected:
            print("  %s" % rel, file=sys.stderr)
        print("Add a rule to tools/make_update_manifest.py (and to the CI "
              "artifact paths) or remove them.", file=sys.stderr)
        return files, 1

    if not any(f["path"].lower().endswith("spectrocraft.exe") for f in files):
        print("error: no SpectroCraft.exe under %s — is this the build output "
              "folder?" % root, file=sys.stderr)
        return files, 1

    if skipped:
        print("skipped %d file(s) (never part of an update): %s"
              % (len(skipped), ", ".join(sorted(skipped)[:6])))
    return files, 0


def collect_posix(root: str, platform: str):
    """macOS / Linux: one top folder, every regular file under it."""
    tops = sorted(n for n in os.listdir(root) if not n.startswith("."))
    if len(tops) != 1 or not os.path.isdir(os.path.join(root, tops[0])):
        print("error: %s must hold exactly one folder (SpectroCraft.app or Debug), "
              "not %s" % (root, tops), file=sys.stderr)
        return [], "", 1
    top = tops[0]
    if platform == "macos" and not top.endswith(".app"):
        print("error: the macOS payload must be an .app bundle, not %s" % top, file=sys.stderr)
        return [], "", 1

    files, links = [], []
    base = os.path.join(root, top)
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = sorted(dirnames)
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace("\\", "/")
            if any(matches_glob(name.lower(), p) for p in IGNORE) or name == ".DS_Store":
                continue
            if os.path.islink(full):
                links.append(rel)
                continue
            entry = {"path": rel, "size": os.path.getsize(full), "sha256": sha256_of(full)}
            if os.stat(full).st_mode & stat.S_IXUSR:
                entry["executable"] = True
            files.append(entry)

    if links:
        print("error: the payload still holds symbolic links (make it with "
              "tools/make_release_payload.py): %s" % ", ".join(links), file=sys.stderr)
        return files, top, 1

    paths = {f["path"] for f in files}
    for req in REQUIRED[platform]:
        if top + "/" + req not in paths:
            print("error: %s/%s is missing from %s" % (top, req, root), file=sys.stderr)
            return files, top, 1
    return files, top, 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dir", required=True, help="release payload folder to describe")
    ap.add_argument("--platform", default="windows", choices=["windows", "macos", "linux"],
                    help="archive layout (default: windows)")
    ap.add_argument("--arch", help="x64 / arm64 (default: arm64 on macOS, x64 elsewhere)")
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

    if args.platform == "windows":
        files, rc = collect_windows(root)
        manifest_root = ""
    else:
        files, manifest_root, rc = collect_posix(root, args.platform)
    if rc != 0:
        return rc

    files.sort(key=lambda f: f["path"].lower())

    generated = args.generated or datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ")

    arch = args.arch or DEFAULT_ARCH[args.platform]
    manifest = {"format": FORMAT, "version": version, "generated": generated,
                "platform": "%s-%s" % (args.platform, arch)}
    if manifest_root:
        manifest["root"] = manifest_root
    manifest["files"] = files

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
        print("  %10d  %s %s  %s" % (f["size"], f["sha256"][:16],
                                     "x" if f.get("executable") else " ", f["path"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
