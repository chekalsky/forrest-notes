#!/usr/bin/env bash
# Build Forrest Note firmware in BLE voice mode (mic → iPhone, no Wi-Fi notes UI).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FQBN="esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=custom,CDCOnBoot=cdc,FlashSize=8M"
FLAGS="-DFORREST_BLE_VOICE -I$ROOT/protocol"

arduino-cli compile -b "$FQBN" \
  --build-property "compiler.cpp.extra_flags=$FLAGS" \
  --build-property "compiler.c.extra_flags=$FLAGS" \
  "$ROOT/forrest_note"
