#!/usr/bin/env python3
"""
gen_manifest.py — generate main/sounds_manifest.json from sounds/.

Called automatically by CMake before each build (see main/CMakeLists.txt).
For WAV files: hashes the file directly.
For FLAC/OGG/AIFF: converts to 16-bit 44100 Hz LE PCM WAV in a temp dir,
hashes the converted WAV — so the hash matches what upload_sounds.py puts on SD.

Output (main/sounds_manifest.json):
  {"version":1,"files":{"KitName/file.wav":"<sha256>", ...}}

The JSON is embedded in firmware via COMPONENT_EMBED_TXTFILES so the device
can compare SD contents against it on every boot without any network calls.
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

AUDIO_EXTENSIONS = {".wav", ".flac", ".aif", ".aiff", ".ogg"}

SCRIPT_DIR  = Path(__file__).resolve().parent
REPO_ROOT   = SCRIPT_DIR.parent
SOUNDS_DIR  = REPO_ROOT / "sounds"
OUTPUT_FILE = REPO_ROOT / "main" / "sounds_manifest.json"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def to_wav(src: Path, tmp_dir: Path) -> Path | None:
    if src.suffix.lower() == ".wav":
        return src
    dst = tmp_dir / (src.stem + ".wav")
    r = subprocess.run(
        ["ffmpeg", "-y", "-i", str(src),
         "-ar", "44100", "-sample_fmt", "s16", "-acodec", "pcm_s16le", str(dst)],
        capture_output=True,
    )
    if r.returncode != 0:
        print(f"[WARN] ffmpeg failed: {src.name}", file=sys.stderr)
        return None
    return dst


def main():
    if not SOUNDS_DIR.is_dir():
        print(f"[WARN] sounds dir not found: {SOUNDS_DIR} — writing empty manifest",
              file=sys.stderr)
        OUTPUT_FILE.write_text('{"version":1,"files":{}}')
        return

    has_ffmpeg = shutil.which("ffmpeg") is not None
    need_ffmpeg = any(
        p.suffix.lower() in AUDIO_EXTENSIONS - {".wav"}
        for p in SOUNDS_DIR.rglob("*") if p.is_file()
    )
    if need_ffmpeg and not has_ffmpeg:
        print("[WARN] ffmpeg not found — non-WAV files will be skipped in manifest",
              file=sys.stderr)

    files: dict[str, str] = {}
    audio = sorted(p for p in SOUNDS_DIR.rglob("*")
                   if p.is_file() and p.suffix.lower() in AUDIO_EXTENSIONS)

    with tempfile.TemporaryDirectory(prefix="synth32_manifest_") as tmp_s:
        tmp = Path(tmp_s)
        for src in audio:
            rel = src.relative_to(SOUNDS_DIR)
            wav_rel = str(rel.with_suffix(".wav")).replace("\\", "/")

            if src.suffix.lower() != ".wav" and not has_ffmpeg:
                continue

            wav = to_wav(src, tmp) if has_ffmpeg else src
            if wav is None:
                continue

            files[wav_rel] = sha256_file(wav)

    manifest = {"version": 1, "files": files}
    OUTPUT_FILE.write_text(json.dumps(manifest, indent=None, separators=(",", ":")))
    print(f"[manifest] wrote {len(files)} entries → {OUTPUT_FILE.relative_to(REPO_ROOT)}")


if __name__ == "__main__":
    main()
