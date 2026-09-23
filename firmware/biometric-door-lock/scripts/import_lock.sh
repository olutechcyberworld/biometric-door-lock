#!/usr/bin/env bash
# Copy the dependency lock that produced your last WORKING build from the old ESP-WHO checkout, so this project
# resolves exactly the same component versions.   Usage: scripts/import_lock.sh [old_example_dir]
set -e
OLD="${1:-$HOME/esp/esp-who/examples/human_face_recognition}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$OLD/dependencies.lock.esp32_s3_eye"
[ -f "$SRC" ] || { echo "not found: $SRC"; exit 1; }
cp "$SRC" "$ROOT/firmware/dependencies.lock"
echo "copied $SRC -> firmware/dependencies.lock (commit it)"
