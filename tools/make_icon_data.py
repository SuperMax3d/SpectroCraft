#!/usr/bin/env python3
"""Regenerate include/IconData.h from an .ico file.

The app embeds src/Icons/icon4.ico twice: as a PE resource (RT_ICON /
RT_GROUP_ICON, written into JUCE's generated resources.rc by
cmake/patch_generated.cmake) and as the byte array in include/IconData.h,
which MainWindow applies with WM_SETICON for the window title bar. Both must
come from the same file, so run this after changing the icon:

    python3 tools/make_icon_data.py                 # src/Icons/icon4.ico -> include/IconData.h
    python3 tools/make_icon_data.py other.ico out.h

The output format is fixed (16 bytes per line, 4-space indent) so a changed
icon shows as a readable diff.
"""
import sys

DEFAULT_SRC = "src/Icons/icon4.ico"
DEFAULT_DST = "include/IconData.h"


def main() -> int:
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SRC
    dst = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_DST

    with open(src, "rb") as f:
        data = f.read()
    if len(data) < 22 or data[0:4] != b"\x00\x00\x01\x00":
        print(f"{src}: not an .ico file", file=sys.stderr)
        return 1

    lines = [
        f"// Auto-generated from {src.replace(chr(92), '/')} - do not edit.",
        "#pragma once",
        "namespace scapp {",
        f"static const unsigned char kAppIconIcoData[{len(data)}] = {{",
    ]
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    lines += [
        "};",
        f"static const unsigned int kAppIconIcoSize = {len(data)};",
        "}",
        "",
    ]

    # CRLF, like the checked-in file: a changed icon must show as a content
    # diff, not a 27 000-line whitespace diff.
    with open(dst, "w", newline="\r\n") as f:
        f.write("\n".join(lines))
    print(f"{dst}: {len(data)} bytes from {src}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
