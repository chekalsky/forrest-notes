#!/usr/bin/env bash
# Compile the Forrest Note firmware without flashing.
# Run from anywhere: ./scripts/compile.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FQBN="esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=custom,CDCOnBoot=cdc,FlashSize=8M"

if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "arduino-cli not found. Install it, then the esp32@3.2.0 core and" >&2
  echo "libraries — see README.md Installation." >&2
  exit 1
fi

if [[ -e "$ROOT/forrest_note/src/app/network.h" || -e "$ROOT/forrest_note/src/app/network.cpp" ]]; then
  echo "leftover network.cpp/.h — that file was split into transcribe + portal" >&2
  exit 1
fi

arduino-cli compile -b "$FQBN" "$ROOT/forrest_note"
