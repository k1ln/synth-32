#!/usr/bin/env bash
# flash_and_upload.sh — flash firmware then delta-upload sounds to SD card.
#
# Usage:
#   ./tools/flash_and_upload.sh [--port PORT] [--no-sounds] [extra idf.py args...]
#
# Options:
#   --port PORT    Serial port for flashing (default: auto-detect or IDF_PORT env var)
#   --no-sounds    Flash only; skip sound upload (useful for code-only changes)
#
# Examples:
#   ./tools/flash_and_upload.sh
#   ./tools/flash_and_upload.sh --port /dev/tty.usbserial-0001
#   ./tools/flash_and_upload.sh --no-sounds
#   SYNTH32_SKIP_MANIFEST=1 ./tools/flash_and_upload.sh   # skip manifest regen too

set -euo pipefail
cd "$(dirname "$0")/.."   # repo root

PORT="${IDF_PORT:-}"
NO_SOUNDS=0
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)   PORT="$2"; shift 2 ;;
        --no-sounds) NO_SOUNDS=1; shift ;;
        *)        EXTRA_ARGS+=("$1"); shift ;;
    esac
done

PORT_ARGS=()
[[ -n "$PORT" ]] && PORT_ARGS=(-p "$PORT")

# ── Flash ─────────────────────────────────────────────────────────────────────
echo "==> Flashing firmware..."
idf.py "${PORT_ARGS[@]}" flash "${EXTRA_ARGS[@]}"

[[ "$NO_SOUNDS" -eq 1 ]] && { echo "==> --no-sounds: skipping sound upload."; exit 0; }

# ── Upload sounds ─────────────────────────────────────────────────────────────
echo ""
echo "==> Delta-uploading sounds to SD card..."
echo "    (Connect to 'synth-32' WiFi AP if not already connected)"
echo ""

python3 tools/upload_sounds.py --wait --jobs 4

echo ""
echo "==> Done. Device SD card is in sync."
