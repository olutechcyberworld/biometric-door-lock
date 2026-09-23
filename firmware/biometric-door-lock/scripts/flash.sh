#!/usr/bin/env bash
# Flash without opening a monitor (the dashboard owns the serial port). Usage: scripts/flash.sh [port]
set -e
source "$(dirname "$0")/env.sh"
PORT="${1:-$DOORLOCK_PORT}"
cd "$DOORLOCK_ROOT/firmware"
idf.py -p "$PORT" flash
