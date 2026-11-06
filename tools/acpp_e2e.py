#!/usr/bin/env python3
"""End-to-end check of the audio.cpp integration with the real CLI.

Runs ``SpectroCraft --json`` (no window) to:

1. install this platform's audio.cpp CLI and the FlashSR model through the
   model catalogue (real downloads from GitHub, SHA-256 checked, archive
   extracted: .zip on Windows, .tar.gz with its libraries on macOS / Linux);
2. place the HTDemucs 6-stem q8_0 model (42 MB from Hugging Face, SHA-256
   checked; the catalogue lists it as a manual install);
3. run super resolution (FlashSR) and stem separation (htdemucs_6stems) on a
   synthetic 10 s stereo clip, save both results and check them.

About 80-110 MB is downloaded on the first run (the models folder can be
reused). Standard library only. Exit code 0 when every check passed.

    python3 tools/acpp_e2e.py --exe <SpectroCraft executable>
        [--backend cpu|metal|vulkan|cuda] [--variant cpu|metal|vulkan]
        [--models-dir DIR] [--work-dir DIR] [--timeout-min N]

macOS: the executable is SpectroCraft.app/Contents/MacOS/SpectroCraft.
Linux: run it under xvfb-run when there is no display.
"""

from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
import os
import platform
import random
import struct
import subprocess
import sys
import tempfile
import urllib.request
import wave
from pathlib import Path

HTDEMUCS_URL = ("https://huggingface.co/audio-cpp/audio.cpp-gguf/resolve/main/"
                "HTDemucs-6stems-GGUF/htdemucs-6stems-q8_0.gguf")
HTDEMUCS_SHA256 = "71f3d65681a25c28fbf6278fee579f810312eae47d3b26eb04bcd77c77934c0d"
HTDEMUCS_SIZE = 42477600
STEM_NAMES = ["bass", "drums", "guitar", "other", "piano", "vocals"]
RATE = 44100
SECONDS = 10

failures: list[str] = []


def check(ok: bool, what: str) -> None:
    print(("PASS  " if ok else "FAIL  ") + what, flush=True)
    if not ok:
        failures.append(what)


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch_htdemucs(models_dir: Path) -> Path:
    dest = models_dir / "audiocpp" / "HTDemucs-6stems-GGUF" / "htdemucs-6stems-q8_0.gguf"
    if dest.is_file() and dest.stat().st_size == HTDEMUCS_SIZE and sha256_of(dest) == HTDEMUCS_SHA256:
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    req = urllib.request.Request(HTDEMUCS_URL, headers={"User-Agent": "SpectroCraft-e2e"})
    with urllib.request.urlopen(req, timeout=120) as r, part.open("wb") as out:
        while True:
            block = r.read(1 << 20)
            if not block:
                break
            out.write(block)
    if part.stat().st_size != HTDEMUCS_SIZE or sha256_of(part) != HTDEMUCS_SHA256:
        part.unlink()
        raise RuntimeError("htdemucs download: size or SHA-256 mismatch")
    part.replace(dest)
    return dest


def write_clip(path: Path) -> None:
    """Alternating harmonic notes, four percussive hits per second, a little noise."""
    rnd = random.Random(1)
    frames = bytearray()
    for i in range(RATE * SECONDS):
        t = i / RATE
        f = 220.0 if int(t * 2) % 2 == 0 else 330.0
        tone = sum(0.25 / k * math.sin(2 * math.pi * f * k * t) for k in range(1, 6))
        beat = (t * 4) % 1.0
        click = 0.6 * math.exp(-beat * 40) * rnd.uniform(-1, 1)
        noise = 0.02 * rnd.uniform(-1, 1)
        left = max(-1.0, min(1.0, tone + click + noise))
        right = max(-1.0, min(1.0, 0.9 * tone + click + noise))
        frames += struct.pack("<hh", int(left * 30000), int(right * 30000))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))


def read_wav(path: Path) -> tuple[array.array, int, int]:
    """(interleaved samples as floats, channels, rate) for 16-bit PCM or 32-bit float WAV."""
    data = path.read_bytes()
    pos, fmt, body = 12, None, None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        n = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        chunk = data[pos + 8:pos + 8 + n]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", chunk[:16])
        elif cid == b"data":
            body = chunk
        pos += 8 + n + (n & 1)
    if fmt is None or body is None:
        raise RuntimeError(f"{path}: not a WAV file")
    _, channels, rate, _, _, bits = fmt
    if bits == 32:
        samples = array.array("f")
        samples.frombytes(body)
    else:
        ints = array.array("h")
        ints.frombytes(body)
        samples = array.array("f", (x / 32768.0 for x in ints))
    return samples, channels, rate


def dbfs(samples, start: int = 0, step: int = 1, count: int | None = None) -> float:
    total, n = 0.0, 0
    stop = len(samples) if count is None else start + count * step
    for i in range(start, stop, step):
        total += samples[i] * samples[i]
        n += 1
    return 10 * math.log10(total / max(n, 1) + 1e-24)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True, help="SpectroCraft executable")
    ap.add_argument("--backend", default="cpu", help="--backend handed to the CLI (default cpu)")
    ap.add_argument("--variant", default="metal" if platform.system() == "Darwin" else "cpu",
                    help="audio.cpp CLI variant to install (catalogue id audiocpp-cli-<variant>)")
    ap.add_argument("--models-dir", type=Path, help="SC_MODELS_DIR (default: <work dir>/models, reused)")
    # The app only opens files under the home, scripts or temporary folders;
    # on Windows the temporary folder may be spelled with 8.3 short names.
    default_work = (Path.home() if os.name == "nt" else Path(tempfile.gettempdir())) / "sc-acpp-e2e"
    ap.add_argument("--work-dir", type=Path, default=default_work)
    ap.add_argument("--timeout-min", type=int, default=40)
    args = ap.parse_args()

    work: Path = args.work_dir
    models: Path = args.models_dir or (work / "models")
    work.mkdir(parents=True, exist_ok=True)
    models.mkdir(parents=True, exist_ok=True)
    clip, sr_out, stems_out = work / "clip.wav", work / "superres.wav", work / "stems.wav"
    for f in (sr_out, stems_out):
        f.unlink(missing_ok=True)
    print(f"work folder {work}\nmodels      {models}\nvariant     {args.variant}, backend {args.backend}", flush=True)

    write_clip(clip)
    fetch_htdemucs(models)

    long_ms = args.timeout_min * 60 * 1000
    requests = [
        {"continueOnError": True},
        {"id": "install-cli", "cmd": "ai.models.install", "timeoutMs": long_ms,
         "params": {"modelId": f"audiocpp-cli-{args.variant}"}},
        {"id": "install-flashsr", "cmd": "ai.models.install", "timeoutMs": long_ms,
         "params": {"modelId": "flashsr"}},
        {"id": "devices", "cmd": "ai.devices"},
        {"id": "open", "cmd": "doc.open", "params": {"path": str(clip)}},
        {"id": "superres", "cmd": "ai.superRes.run", "doc": 1, "timeoutMs": long_ms,
         "params": {"mode": "flashsr", "backend": args.backend, "select": "all", "output": "newDoc"}},
        {"id": "save-superres", "cmd": "doc.saveAs", "doc": 2, "params": {"path": str(sr_out)}},
        {"id": "stems", "cmd": "ai.stems.run", "doc": 1, "timeoutMs": long_ms,
         "params": {"mode": "htdemucs6", "backend": args.backend, "select": "all", "output": "newDoc"}},
        {"id": "save-stems", "cmd": "doc.saveAs", "doc": 3, "params": {"path": str(stems_out)}},
    ]
    req_file, out_file = work / "requests.ndjson", work / "responses.ndjson"
    req_file.write_text("".join(json.dumps(r) + "\n" for r in requests), encoding="utf-8")
    out_file.unlink(missing_ok=True)

    env = dict(os.environ, SC_MODELS_DIR=str(models))
    proc = subprocess.run([args.exe, "--json", str(req_file), "--json-out", str(out_file)],
                          env=env, capture_output=True, text=True, errors="replace",
                          timeout=args.timeout_min * 60 + 120)
    responses = {}
    if out_file.is_file():
        for line in out_file.read_text(encoding="utf-8").splitlines():
            if line.strip():
                o = json.loads(line)
                responses[o.get("id")] = o

    def ok(rid: str) -> dict | None:
        r = responses.get(rid)
        good = r is not None and r.get("ok") is True
        detail = "" if good else f": {json.dumps(r)[:400] if r else 'no response'}"
        check(good, f"{rid} succeeded{detail}")
        if good and r.get("durationMs") is not None:
            print(f"      {rid}: {r['durationMs'] / 1000:.1f} s", flush=True)
        return r.get("result") if good else None

    cli = ok("install-cli")
    if cli:
        check(cli.get("installed") is True and cli.get("verified") is True,
              f"CLI {args.variant} installed and verified ({cli.get('downloadedBytes')} bytes downloaded)")
    ok("install-flashsr")
    dev = ok("devices")
    if dev:
        ac = dev.get("audiocpp", {})
        check(ac.get("available") is True and args.backend in (ac.get("backends") or []),
              f"ai.devices: audio.cpp available with backend {args.backend} ({ac})")
    ok("open")

    sr = ok("superres")
    if sr:
        check(sr.get("toolRate") == 48000.0, f"super resolution: tool rate 48 kHz ({sr.get('toolRate')})")
    if ok("save-superres") and sr_out.is_file():
        s, ch, rate = read_wav(sr_out)
        secs = len(s) / ch / rate
        level = dbfs(s)
        check(ch == 1 and rate == 48000 and abs(secs - SECONDS) < 0.05 and level > -40.0,
              f"super resolution output: {ch} ch, {rate} Hz, {secs:.2f} s, {level:.1f} dBFS")

    st = ok("stems")
    if st:
        names = [x.get("name") for x in st.get("stems", [])]
        frames = {x.get("frames") for x in st.get("stems", [])}
        check(sorted(names) == STEM_NAMES and frames == {RATE * SECONDS},
              f"stems: the 6 htdemucs stems, full length ({names})")
    if ok("save-stems") and stems_out.is_file():
        s, ch, rate = read_wav(stems_out)
        check(ch == 12 and rate == RATE, f"stems output: {ch} channels at {rate} Hz")
        if ch == 12:
            frames_n = len(s) // ch
            # Stems are in alphabetical order, two channels each (left channel measured).
            levels = {n: dbfs(s, start=2 * i, step=ch, count=frames_n) for i, n in enumerate(sorted(STEM_NAMES))}
            print("      stem levels: " + ", ".join(f"{n} {v:.1f}" for n, v in levels.items()) + " dBFS", flush=True)
            check(levels["drums"] > -45.0 and levels["other"] > -45.0,
                  "stems: the clicks land in drums and the notes in other (both above -45 dBFS)")

    if failures:
        print("\n--- SpectroCraft exit code", proc.returncode)
        print("--- stderr (tail)\n" + "\n".join(proc.stderr.splitlines()[-40:]))
        print(f"\n{len(failures)} check(s) failed")
        return 1
    print("\nall audio.cpp end-to-end checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
