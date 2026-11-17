#!/usr/bin/env python3
"""Assemble the macOS / Linux release payload: the folder that becomes the archive (docs/updates.md).

    python3 tools/make_release_payload.py --platform macos \
        --src build/src/SpectroCraft_artefacts/Debug --out build/release-payload
    python3 tools/make_release_payload.py --platform linux \
        --src build/src/SpectroCraft_artefacts/Debug --out build/release-payload

macOS: <out>/SpectroCraft.app, a copy of the built bundle.
Linux: <out>/Debug, the executable, the updater and the shared libraries
(the folder name the Linux archives have always used).

Why a copy instead of archiving the build folder:

* **Symbolic links become files.** The build keeps the shared libraries as
  version chains (libggml.dylib -> libggml.0.dylib -> libggml.0.26.0.dylib,
  libggml.so.0 -> libggml.so.0.26.0). The executable loads the middle name
  (the install name / soname), and a zip made without symbolic-link support
  drops it, so the app cannot start. Here every link the program can load is
  replaced by a copy of the file it points to. Development links (the name
  with no version: libggml.dylib, libggml.so) are left out: only a linker
  uses them.
* **Only what a release needs.** Linux: SpectroCraft, spectrocraft-updater
  and lib*.so*; anything else in the build folder is listed and skipped.
  macOS: the whole bundle.
* **The executable bits are kept**, so a tar of this folder installs as is.
  A zip only keeps them when it is written with Unix permissions (zip -X on
  macOS/Linux); the app sets them again from the update manifest either way.

The update manifest (tools/make_update_manifest.py) is generated from this
folder, so it describes exactly what the archive contains.
"""

import argparse
import os
import re
import shutil
import stat
import sys

# libfoo.dylib, libfoo.so: no version anywhere in the name.
DEV_LINK = re.compile(r"^lib[^.]+(\.dylib|\.so)$")


def is_dev_link(name: str) -> bool:
    return DEV_LINK.match(name) is not None


def copy_file(src: str, dst: str) -> None:
    """Copy the content and the permission bits; src may be a symbolic link."""
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst, follow_symlinks=True)
    shutil.copymode(src, dst, follow_symlinks=True)


def resolve_inside(root: str, link: str) -> str:
    """The file a symbolic link points to, refused when it leaves root."""
    target = os.path.realpath(link)
    if os.path.commonpath([os.path.realpath(root), target]) != os.path.realpath(root):
        raise ValueError("%s points outside the build folder (%s)" % (link, target))
    if not os.path.isfile(target):
        raise ValueError("%s is a broken link (%s)" % (link, target))
    return target


def copy_tree(src_root: str, dst_root: str, accept) -> tuple:
    """Copy src_root to dst_root. accept(rel) decides which regular names belong."""
    copied, linked, skipped = [], [], []
    for dirpath, dirnames, filenames in os.walk(src_root, followlinks=False):
        # A linked folder (a .framework's Versions/Current) would have to be a
        # second copy of the folder: the bundle has none today, so refuse it.
        for d in list(dirnames):
            if os.path.islink(os.path.join(dirpath, d)):
                raise ValueError("linked folder not supported: %s" % os.path.join(dirpath, d))
        dirnames.sort()
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, src_root).replace("\\", "/")
            if not accept(rel):
                skipped.append(rel)
                continue
            if os.path.islink(full):
                if is_dev_link(name):
                    skipped.append(rel + " (development link)")
                    continue
                resolve_inside(src_root, full)
                copy_file(full, os.path.join(dst_root, rel))
                linked.append(rel)
            elif os.path.isfile(full):
                copy_file(full, os.path.join(dst_root, rel))
                copied.append(rel)
            else:
                skipped.append(rel + " (not a file)")
    return copied, linked, skipped


def require_exec(path: str, what: str) -> None:
    if not os.path.isfile(path):
        raise ValueError("%s is missing: %s" % (what, path))
    if not os.stat(path).st_mode & stat.S_IXUSR:
        raise ValueError("%s is not executable: %s" % (what, path))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--platform", required=True, choices=["macos", "linux"])
    ap.add_argument("--src", required=True,
                    help="the build output folder (holding SpectroCraft.app on macOS, "
                         "the SpectroCraft executable on Linux)")
    ap.add_argument("--out", required=True, help="payload folder to create (emptied first)")
    ap.add_argument("--name", help="top folder in the payload (default: SpectroCraft.app / Debug)")
    args = ap.parse_args()

    src = os.path.abspath(args.src)
    out = os.path.abspath(args.out)
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)

    try:
        if args.platform == "macos":
            name = args.name or "SpectroCraft.app"
            bundle = os.path.join(src, "SpectroCraft.app")
            if not os.path.isdir(bundle):
                raise ValueError("no SpectroCraft.app in %s" % src)
            top = os.path.join(out, name)
            copied, linked, skipped = copy_tree(bundle, top, lambda rel: True)
            require_exec(os.path.join(top, "Contents/MacOS/SpectroCraft"), "the application")
            require_exec(os.path.join(top, "Contents/MacOS/spectrocraft-updater"), "the updater")
        else:
            name = args.name or "Debug"
            top = os.path.join(out, name)

            def accept(rel: str) -> bool:
                if "/" in rel:
                    return False
                return rel in ("SpectroCraft", "spectrocraft-updater") or \
                    (rel.startswith("lib") and ".so" in rel)

            copied, linked, skipped = copy_tree(src, top, accept)
            require_exec(os.path.join(top, "SpectroCraft"), "the application")
            require_exec(os.path.join(top, "spectrocraft-updater"), "the updater")
    except (OSError, ValueError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1

    print("payload %s: %d files, %d of them copied from a link"
          % (os.path.join(out, name), len(copied) + len(linked), len(linked)))
    for rel in linked:
        print("  link -> file  %s" % rel)
    if skipped:
        print("skipped %d: %s" % (len(skipped), ", ".join(skipped)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
