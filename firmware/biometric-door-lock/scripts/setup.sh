#!/usr/bin/env bash
# One-time (or after a clean): pick the chip and generate sdkconfig from firmware/sdkconfig.defaults.
set -e
source "$(dirname "$0")/env.sh"
cd "$DOORLOCK_ROOT/firmware"
rm -rf build sdkconfig sdkconfig.old
idf.py set-target esp32s3
echo "setup done. Next: scripts/build.sh"
