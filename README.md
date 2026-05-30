# synth-32

ESP32-P4 sampler/synth workstation. Audio files live on SD card, never in flash.

## Hardware

| Part | Detail |
|------|--------|
| Board | Waveshare ESP32-P4 (dual-core 400 MHz, 32 MB PSRAM) |
| WiFi | ESP32-C6 companion via SDIO (esp_hosted) |
| Display | MIPI-DSI HX8394 1280×720 |
| Audio | ES8311 DAC via I2S, 48 kHz 16-bit stereo |
| Storage | SD card (SDMMC 4-bit), mounted at `/sdcard` |

---

## First-time setup

### 1. Prerequisites

```bash
# ESP-IDF v5.3+
. $IDF_PATH/export.sh

# Python deps for the upload tool
pip install requests

# ffmpeg — needed to convert FLAC/OGG/AIFF → WAV before upload
brew install ffmpeg        # macOS
sudo apt install ffmpeg    # Ubuntu/Debian
```

### 2. Flash the C6 WiFi coprocessor (once)

The ESP32-P4 has no built-in WiFi. The companion C6 chip acts as a transparent
WiFi coprocessor over SDIO. Flash it once via its USB port:

```bash
cd $IDF_PATH/examples/slave_hci   # or the esp_hosted slave example
idf.py -p /dev/tty.usbserial-C6 flash
```

After this the C6 never needs reflashing unless you update IDF.

### 3. Build, flash firmware, and upload sounds

Connect the **P4** USB port. Make sure your Mac is connected to the
`synth-32` WiFi AP (password: `synth1234`) — the upload happens over WiFi.

```bash
./tools/flash_and_upload.sh --port /dev/tty.usbmodem5ABA0540041
```

This does everything in one command:
1. Builds firmware (hashes `main/sounds/` → embeds manifest in binary)
2. Flashes the P4
3. Waits for the device to boot
4. Delta-uploads only the files missing or changed on the SD card (4 parallel workers)

The first run uploads ~1800 files (~370 MB of WAV). Subsequent runs skip files
already on the card — a code-only reflash typically uploads nothing.

---

## Daily workflow

### Code change only (no new sounds)

```bash
./tools/flash_and_upload.sh --port /dev/tty.usbmodem5ABA0540041 --no-sounds
```

Flashes new firmware, skips the WiFi upload entirely.

### New/changed sounds only (no firmware change)

```bash
python3 tools/upload_sounds.py
```

Diffs local `main/sounds/` against the SD card manifest and uploads only what changed.

### Preview what would be uploaded (no transfer)

```bash
python3 tools/upload_sounds.py --dry-run
```

---

## How the sound sync works

### Build time

Every `idf.py build` runs `tools/gen_manifest.py`, which:
- Walks `main/sounds/`
- Converts any FLAC / OGG / AIFF files to 16-bit 44100 Hz WAV via ffmpeg (in a temp dir)
- SHA-256s every resulting WAV
- Writes `main/sounds_manifest.json`

That JSON file (~200 KB) is baked into the firmware binary via
`EMBED_TXTFILES` — it costs no RAM at runtime (read directly from flash).

To skip manifest regeneration on fast incremental builds:

```bash
SYNTH32_SKIP_MANIFEST=1 idf.py build
```

### Boot time (device)

After mounting the SD card, the device calls `sounds_check_sd()`, which:
- Reads the embedded manifest from flash
- Reads `/sdcard/sounds/.manifest.json`
- Diffs the two and logs every missing or changed file to serial

```
W sounds_http: missing: Ian Paice/kik.wav
W sounds_http: missing: Boss_DR-110/dr110kik.wav
I sounds_http: SD check: 1851 ok, 2 missing, 0 changed
```

The device boots normally regardless — missing files produce silence when played.

### Upload time (host)

`upload_sounds.py`:
1. `GET /sounds/manifest` — fetches the SD's current sha256 map
2. Hashes each local WAV (converting non-WAV first)
3. Skips files whose hash matches
4. `PUT /sounds/upload?path=<kit/file.wav>` — streams each file in 4 KB chunks (never buffered in device RAM)
5. `POST /sounds/commit` — atomically renames `.manifest.tmp` → `.manifest.json` on SD

### SD card directory layout

```
/sdcard/sounds/<kit-name>/<file>.wav     ← always 16-bit LE PCM WAV
/sdcard/sounds/.manifest.json            ← sha256 per file, written by upload tool
```

---

## All sound formats are normalised to WAV

Source kits ship in mixed formats. Everything is stored on SD as
**16-bit little-endian PCM WAV at 44.1 kHz** — conversion happens on the
host (ffmpeg) before transfer, never on the device.

| Format | Files | Why converted |
|--------|-------|---------------|
| WAV 16-bit LE | 1347 | kept as-is |
| FLAC | 384 | decoding needs 40–80 KB RAM + significant CPU per lane |
| AIFF | 86 | identical to WAV but big-endian — byte-swap wasted work |
| OGG Vorbis | 37 | lossy decoder would saturate CPU across 16 lanes |

---

## Reference

### tools/flash_and_upload.sh

```
./tools/flash_and_upload.sh [--port PORT] [--no-sounds] [idf.py args...]

--port PORT     Serial port (or set IDF_PORT env var)
--no-sounds     Flash only, skip WiFi upload
```

### tools/upload_sounds.py

```
python3 tools/upload_sounds.py [options]

--sounds-dir DIR   Source directory   (default: main/sounds)
--host IP          Device address     (default: 192.168.4.1)
--wait             Poll until device responds (use right after flashing)
--jobs N           Parallel upload workers (default: 4)
--dry-run          Show plan, no transfer
```

### Device HTTP endpoints (while on synth-32 AP)

| Method | URL | What it does |
|--------|-----|--------------|
| GET | `/sounds/manifest` | Returns current SD manifest JSON |
| PUT | `/sounds/upload?path=kit/file.wav` | Stream-write a WAV to SD |
| POST | `/sounds/commit` | Atomically update `.manifest.json` |
