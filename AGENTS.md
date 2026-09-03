# Forrest Notes — agent notes

## Flash (BLE voice firmware)

**Prefer one `compile --upload` command** — builds and flashes in one step. Do not use `./scripts/compile_ble.sh` alone when the user asks to flash; that script compile-only.

### Port

```bash
ls /dev/cu.usbmodem* 2>/dev/null || ls /dev/tty.usbmodem* 2>/dev/null
```

Common macOS port: `/dev/cu.usbmodem2101`. If nothing appears, the board may be asleep — hold **REC/BOOT**, plug USB, keep holding through the write (see README flash quirk).

### Upload command

Run from repo root. Shell needs **`all` permissions** (Arduino cache + serial write).

```bash
ROOT="$(pwd)"
PORT="/dev/cu.usbmodem2101"   # replace with detected port
FQBN="esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=custom,CDCOnBoot=cdc,FlashSize=8M"
FLAGS="-DFORREST_BLE_VOICE -I$ROOT/protocol"

arduino-cli compile --upload -p "$PORT" -b "$FQBN" \
  --build-property "compiler.cpp.extra_flags=$FLAGS" \
  --build-property "compiler.c.extra_flags=$FLAGS" \
  "$ROOT/forrest_note"
```

**Done when:** output includes `Hash of data verified.` and `Hard resetting`.

### Compile only (no flash)

```bash
./scripts/compile_ble.sh
```

## Active firmware

- **BLE voice build** (`FORREST_BLE_VOICE`): mic → iPhone over BLE; no Wi‑Fi notes UI.
- Key UI: `forrest_note/src/app/ble_voice_ui.cpp`
- Key BLE/transfer: `forrest_note/src/app/ble_voice.cpp`
