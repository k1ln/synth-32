#!/usr/bin/env python3
"""
upload_sounds.py — delta-upload sound kits from sounds/ to synth-32 SD card.

Requires: ffmpeg (PATH), requests
Usage:
  python3 tools/upload_sounds.py [options]

Options:
  --sounds-dir DIR   Source directory  (default: <repo>/sounds)
  --host IP          Device address    (default: 192.168.4.1)
  --wait             Poll until device responds, then upload (use after idf.py flash)
  --jobs N           Parallel upload workers (default: 4)
  --dry-run          Show what would be uploaded, don't transfer anything

Flow:
  1. Walk sounds-dir; for non-WAV files convert to 16-bit 44.1kHz mono/stereo WAV via ffmpeg.
  2. GET /sounds/manifest from device (current sha256 map).
  3. Compute sha256 of each local (converted) WAV; skip files already matching manifest.
  4. PUT /sounds/upload?path=<rel> for each new/changed file (parallel, --jobs workers).
  5. POST /sounds/commit to atomically update .manifest.json on SD.
"""

import argparse
import hashlib
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import requests

AUDIO_EXTENSIONS = {".wav", ".flac", ".aif", ".aiff", ".ogg"}
DEVICE_DEFAULT   = "192.168.4.1"
SCRIPT_DIR       = Path(__file__).resolve().parent
REPO_ROOT        = SCRIPT_DIR.parent
DEFAULT_SOUNDS   = REPO_ROOT / "sounds"


# ── helpers ───────────────────────────────────────────────────────────────────

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def to_wav(src: Path, tmp_dir: Path) -> Path | None:
    """Return 16-bit 44100 Hz LE PCM WAV path. WAV returned as-is; others converted."""
    if src.suffix.lower() == ".wav":
        return src
    dst = tmp_dir / (src.stem + ".wav")
    r = subprocess.run(
        ["ffmpeg", "-y", "-i", str(src),
         "-ar", "44100", "-sample_fmt", "s16", "-acodec", "pcm_s16le", str(dst)],
        capture_output=True,
    )
    if r.returncode != 0:
        last = r.stderr.decode(errors="replace").strip().splitlines()
        print(f"  [WARN] ffmpeg failed: {src.name}: {last[-1] if last else 'unknown'}")
        return None
    return dst


def fetch_manifest(base_url: str, timeout: int = 10) -> dict:
    print(f"[1/4] Fetching manifest from {base_url}/sounds/manifest ...", flush=True)
    t0 = time.monotonic()
    try:
        r = requests.get(f"{base_url}/sounds/manifest", timeout=timeout)
        r.raise_for_status()
        files = r.json().get("files", {})
        print(f"      → got {len(files)} entries in {time.monotonic()-t0:.1f}s", flush=True)
        return files
    except Exception as e:
        print(f"      → no manifest ({e}); treating all files as new "
              f"(took {time.monotonic()-t0:.1f}s)", flush=True)
        return {}


def sd_stat(base_url: str, sd_rel_path: str, timeout: int = 5):
    """GET /sd/stat?path=... → {"exists":bool, "size":int} or None on error.
    `sd_rel_path` is relative to SDCARD_ROOT, so include the "sounds/" prefix
    when checking files written by /sounds/upload."""
    try:
        r = requests.get(f"{base_url}/sd/stat",
                         params={"path": sd_rel_path}, timeout=timeout)
        if not r.ok:
            return None
        return r.json()
    except Exception:
        return None


def fetch_sd_listing(base_url: str, sd_rel_root: str,
                     timeout: int = 60) -> dict[str, int] | None:
    """GET /sd/ls/recursive?path=<sd_rel_root> → {rel_path: size_bytes}.
    Paths in the result are relative to `sd_rel_root` (i.e. the same form the
    caller uses for upload). Returns None on transport failure so the caller
    can decide between fail-fast and best-effort fallback paths."""
    url = f"{base_url}/sd/ls/recursive"
    params = {"path": sd_rel_root} if sd_rel_root else {}
    try:
        with requests.get(url, params=params, timeout=timeout,
                          stream=True) as r:
            if not r.ok:
                return None
            listing: dict[str, int] = {}
            for raw in r.iter_lines(decode_unicode=True):
                if not raw:
                    continue
                try:
                    obj = json.loads(raw)
                    p = obj.get("path")
                    s = obj.get("size")
                    if isinstance(p, str) and isinstance(s, int):
                        listing[p] = s
                except Exception:
                    # Skip malformed lines (partial chunk, truncated, etc.)
                    continue
            return listing
    except Exception:
        return None


def upload_file(session: requests.Session, base_url: str,
                 wav_path: Path, rel_path: str,
                 retries: int = 4) -> bool:
    url = f"{base_url}/sounds/upload?path={requests.utils.quote(rel_path, safe='/')}"
    last_err = None
    for attempt in range(retries):
        try:
            with open(wav_path, "rb") as f:
                r = session.put(url, data=f,
                                headers={"Content-Type": "application/octet-stream",
                                         "Connection": "keep-alive"},
                                timeout=180)
            r.raise_for_status()
            return True
        except Exception as e:
            last_err = e
            # On socket-exhaustion errors, give the device time to GC sockets
            # and rebuild a fresh connection on the next attempt.
            try:
                session.close()
            except Exception:
                pass
            if attempt < retries - 1:
                time.sleep(2.0 * (2 ** attempt))  # 2s, 4s, 8s
    print(f"  [ERROR] {rel_path}: {last_err}")
    return False


def commit_manifest(base_url: str, manifest: dict, retries: int = 3) -> bool:
    last_err = None
    for attempt in range(retries):
        try:
            r = requests.post(f"{base_url}/sounds/commit",
                              json={"version": 1, "files": manifest},
                              headers={"Content-Type": "application/json"},
                              timeout=30)
            r.raise_for_status()
            return True
        except Exception as e:
            last_err = e
            if attempt < retries - 1:
                time.sleep(2.0 * (2 ** attempt))
    print(f"[ERROR] commit failed: {last_err}")
    return False


def wait_for_device(base_url: str, timeout_s: int = 120) -> bool:
    """Poll GET /sounds/manifest until the device responds (post-flash boot)."""
    print(f"Waiting for device at {base_url} (up to {timeout_s}s) ...", flush=True)
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            requests.get(f"{base_url}/sounds/manifest", timeout=3)
            print("Device is up.")
            return True
        except Exception:
            time.sleep(2)
    return False


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Delta-upload sounds to synth-32 SD card")
    parser.add_argument("--sounds-dir", default=str(DEFAULT_SOUNDS),
                        help=f"Source directory (default: {DEFAULT_SOUNDS})")
    parser.add_argument("--host", default=DEVICE_DEFAULT,
                        help=f"Device IP or hostname (default: {DEVICE_DEFAULT})")
    parser.add_argument("--wait", action="store_true",
                        help="Wait for device to boot before uploading (use after idf.py flash)")
    parser.add_argument("--jobs", type=int, default=2,
                        help="Parallel upload workers (default: 2; device lwIP "
                             "has limited sockets, higher values cause ECONNRESET)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Show plan without transferring anything")
    args = parser.parse_args()

    sounds_dir = Path(args.sounds_dir).resolve()
    if not sounds_dir.is_dir():
        sys.exit(f"[ERROR] Sounds directory not found: {sounds_dir}")

    base_url = f"http://{args.host}"
    print(f"synth-32 upload  →  {base_url}")
    print(f"Source:             {sounds_dir}")

    if not args.dry_run:
        if shutil.which("ffmpeg") is None:
            sys.exit("[ERROR] ffmpeg not found in PATH — required for format conversion.")
        if args.wait and not wait_for_device(base_url):
            sys.exit("[ERROR] Device did not respond within timeout.")
        # Fast reachability probe: if the HTTP server doesn't answer in a few
        # seconds there's no point planning 1854 files or queueing uploads
        # that will each cost a 10s connect timeout. Bail out with a clear
        # message instead of marching silently into a multi-hour hang.
        if not args.wait:
            probe_t0 = time.monotonic()
            try:
                requests.get(f"{base_url}/sounds/manifest", timeout=3)
                print(f"[probe] device reachable in "
                      f"{time.monotonic()-probe_t0:.1f}s", flush=True)
            except Exception as e:
                sys.exit(
                    f"[ERROR] Device at {base_url} did not respond within 3s "
                    f"({e}).\n"
                    f"        Check that you are associated to the device's "
                    f"Wi-Fi and that the HTTP server is up.\n"
                    f"        Use --wait to poll until the device comes online "
                    f"(e.g. after idf.py flash)."
                )

    # Collect all audio files
    audio_files = sorted(
        p for p in sounds_dir.rglob("*")
        if p.is_file() and p.suffix.lower() in AUDIO_EXTENSIONS
        and not p.name.startswith(".")
    )
    print(f"Found {len(audio_files)} audio file(s).")

    device_manifest = fetch_manifest(base_url) if not args.dry_run else {}

    # Single-roundtrip SD listing keyed by path relative to /sdcard/sounds.
    # Doing this once is dramatically faster than one /sd/stat per file when
    # the device manifest is empty/stale (which would otherwise force a stat
    # roundtrip for every source file in the planning loop below).
    sd_listing: dict[str, int] = {}
    if not args.dry_run:
        print(f"[1b/4] Fetching SD listing under sounds/ ...", flush=True)
        ls_t0 = time.monotonic()
        result = fetch_sd_listing(base_url, "sounds")
        if result is None:
            print(f"      → /sd/ls/recursive failed; falling back to per-file "
                  f"/sd/stat lookups (slow). Reflash device if planning "
                  f"is too slow.", flush=True)
        else:
            sd_listing = result
            print(f"      → got {len(sd_listing)} file(s) in "
                  f"{time.monotonic()-ls_t0:.1f}s", flush=True)

    # ── Build work list (in temp dir so conversions persist across threads) ──
    work = []   # list of (wav_path, wav_rel, digest)
    stats = {"skip": 0, "convert": 0, "error": 0}

    print(f"[2/4] Planning: convert (if needed), hash, and check {len(audio_files)} "
          f"file(s) against the device ...", flush=True)
    plan_t0 = time.monotonic()

    with tempfile.TemporaryDirectory(prefix="synth32_") as tmp_s:
        tmp = Path(tmp_s)

        for idx, src in enumerate(audio_files, start=1):
            rel     = src.relative_to(sounds_dir)
            wav_rel = str(rel.with_suffix(".wav")).replace("\\", "/")

            # Heartbeat every 25 files so the user can see progress through a
            # long planning phase (each iteration may do an ffmpeg + sha256 +
            # /sd/stat roundtrip).
            if idx == 1 or idx % 25 == 0 or idx == len(audio_files):
                print(f"      [{idx}/{len(audio_files)}] planning … "
                      f"skip={stats['skip']} queued={len(work)} "
                      f"err={stats['error']} "
                      f"({time.monotonic()-plan_t0:.1f}s)", flush=True)

            wav = to_wav(src, tmp) if not args.dry_run else src
            if wav is None:
                stats["error"] += 1
                continue
            if wav != src:
                stats["convert"] += 1

            digest = sha256_file(wav) if not args.dry_run else ""

            if not args.dry_run and device_manifest.get(wav_rel) == digest:
                stats["skip"] += 1
                print(f"  [SKIP] {wav_rel}  (manifest sha256 match)", flush=True)
                continue

            # Fallback: even if the manifest doesn't list this file (or is
            # stale), the SD card itself may already hold an identical-sized
            # copy from a previous run. Prefer the prefetched listing; only
            # fall back to a per-file /sd/stat call if the bulk fetch failed.
            if not args.dry_run:
                local_size = wav.stat().st_size
                sd_size = sd_listing.get(wav_rel)
                if sd_size is None and not sd_listing:
                    sd_info = sd_stat(base_url, f"sounds/{wav_rel}")
                    if sd_info and sd_info.get("exists"):
                        sd_size = sd_info.get("size")
                if sd_size == local_size:
                    stats["skip"] += 1
                    print(f"  [SKIP] {wav_rel}  ({sd_size} B on SD)",
                          flush=True)
                    # Record the digest so the next commit reflects reality.
                    device_manifest[wav_rel] = digest
                    continue

            work.append((wav, wav_rel, digest))

        print(f"      → planning done in {time.monotonic()-plan_t0:.1f}s: "
              f"skip={stats['skip']} convert={stats['convert']} "
              f"upload={len(work)} error={stats['error']}", flush=True)

        if args.dry_run:
            for _, wav_rel, _ in work:
                print(f"  [WOULD UPLOAD] {wav_rel}")
            print(f"\nDry-run: {len(work)} file(s) would be uploaded.")
            return

        if not work:
            print("[3/4] Nothing to upload — SD card is already up to date.",
                  flush=True)
            return

        # ── Parallel upload ───────────────────────────────────────────────────
        upload_t0 = time.monotonic()
        print(f"[3/4] Uploading {len(work)} file(s) with "
              f"{min(args.jobs, len(work))} worker(s) ...", flush=True)

        q: queue.Queue = queue.Queue()
        for item in work:
            q.put(item)

        new_manifest = dict(device_manifest)
        lock         = threading.Lock()
        uploaded     = [0]
        failed       = [0]
        since_commit = [0]
        total        = len(work)
        COMMIT_EVERY = 25  # checkpoint manifest every N successful uploads

        def worker(wid: int):
            session = requests.Session()
            print(f"  [w{wid}] started", flush=True)
            try:
                while True:
                    try:
                        wav_path, wav_rel, digest = q.get_nowait()
                    except queue.Empty:
                        print(f"  [w{wid}] queue drained, exiting", flush=True)
                        return
                    kb = wav_path.stat().st_size // 1024
                    try:
                        ok = upload_file(session, base_url, wav_path, wav_rel)
                    except Exception as e:
                        # upload_file already catches per-attempt errors, but
                        # surface anything that escapes so the worker can't
                        # silently die mid-batch.
                        print(f"  [w{wid}] unexpected error on {wav_rel}: {e}",
                              flush=True)
                        ok = False
                    should_checkpoint = False
                    snapshot = None
                    with lock:
                        if ok:
                            new_manifest[wav_rel] = digest
                            uploaded[0] += 1
                            since_commit[0] += 1
                            n = uploaded[0] + failed[0]
                            print(f"  [{n}/{total}] ✓ {wav_rel}  ({kb} KB)",
                                  flush=True)
                            if since_commit[0] >= COMMIT_EVERY:
                                snapshot = dict(new_manifest)
                                should_checkpoint = True
                        else:
                            failed[0] += 1
                            n = uploaded[0] + failed[0]
                            print(f"  [{n}/{total}] ✗ {wav_rel}", flush=True)
                    if should_checkpoint:
                        if commit_manifest(base_url, snapshot):
                            with lock:
                                since_commit[0] = 0
                            print(f"  [checkpoint] manifest committed "
                                  f"({len(snapshot)} entries)", flush=True)
                        else:
                            print(f"  [checkpoint] manifest commit failed — "
                                  f"will retry on next upload", flush=True)
                    q.task_done()
            finally:
                session.close()

        n_workers = min(args.jobs, len(work))
        threads = [threading.Thread(target=worker, args=(i,), daemon=True)
                   for i in range(n_workers)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        print(f"      → upload phase done in "
              f"{time.monotonic()-upload_t0:.1f}s", flush=True)

    print(f"\nSummary  uploaded={uploaded[0]}  failed={failed[0]}  "
          f"skipped={stats['skip']}  converted={stats['convert']}", flush=True)

    if failed[0]:
        print(f"[WARN] {failed[0]} file(s) failed — run again to retry.",
              flush=True)

    if uploaded[0] > 0:
        print("[4/4] Committing manifest ...", end=" ", flush=True)
        if commit_manifest(base_url, new_manifest):
            print("✓", flush=True)
        else:
            print("✗  (files uploaded but manifest not updated — run again)",
                  flush=True)
            sys.exit(1)
    else:
        print("[4/4] Skipping manifest commit (no new uploads).", flush=True)

    if failed[0]:
        sys.exit(1)


if __name__ == "__main__":
    main()
