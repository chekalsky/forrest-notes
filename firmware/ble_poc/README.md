# BLE PoC firmware

Minimal ESP32-S3 sketch that advertises `Forrest-Voice` and sends a synthetic WAV when **BOOT (GPIO0)** is pressed.

## Flash

```bash
arduino-cli compile --upload -p /dev/cu.usbmodemXXXX \
  -b esp32:esp32:esp32s3 \
  firmware/ble_poc/ble_poc.ino
```

Or open `ble_poc.ino` in Arduino IDE (ESP32 3.2.x).

## Test with iPhone

See [`ios/README.md`](../../ios/README.md).
