# Forrest Voice iOS MVP

Receives WAV recordings from the ESP32 over BLE **in the background** and fires a **local notification** when a transfer completes.

## Requirements

- Xcode 15+ (iOS 17 SDK)
- Physical iPhone (CoreBluetooth background modes do not work in Simulator)
- ESP32-S3 flashed with `firmware/ble_poc/ble_poc.ino`

## Generate Xcode project

```bash
brew install xcodegen   # if needed
cd ios && xcodegen generate
open ForrestVoice.xcodeproj
```

Set your **Development Team** in Signing & Capabilities, then run on device.

## Capabilities (already in project.yml)

- **Background Modes → Uses Bluetooth LE accessories** (`bluetooth-central`)
- Bluetooth permission strings in Info.plist

## Test flow

1. Flash `firmware/ble_poc/ble_poc.ino` to ESP32-S3 (Arduino, esp32 @ 3.2.0).
2. Install and launch Forrest Voice on iPhone; allow **Bluetooth** and **Notifications**.
3. App scans for service `6E4000F0-…`, auto-connects to `Forrest-Voice`.
4. **Press BOOT button** on ESP32 → ~16 KB WAV transfers.
5. iPhone shows banner: **"New voice recording"** (works if app is backgrounded after initial pairing).
6. Pull-to-refresh or reopen app → file listed under Recordings (`Documents/Recordings/`).

## Background behavior (honest)

| Scenario | Expected |
|----------|----------|
| App foreground | Transfer + notification |
| App backgrounded, BLE connected | iOS wakes app briefly; transfer + notification usually works |
| App force-quit | **No** — user must reopen app; ESP32 retains data (future: pending queue) |
| Phone locked | Same as background if session alive |

For MVP, **open the app once** after install so it connects and subscribes. Then background/lock the phone and press BOOT on the device.

## Architecture

```
ESP32 (peripheral)                    iPhone (central)
  RecordingMeta notify  ───────────▶  AudioTransferManager.begin
  AudioData notify      ───────────▶  chunk ingest → Documents/Recordings/
  (complete)            ───────────▶  UNUserNotificationCenter (local push)
```

## Next steps

- Wire `ble_poc` into full Forrest Note `record.cpp` (real mic WAV)
- STT + LLM pipeline on transfer complete
- Send `ResultText` back to ESP32 display
