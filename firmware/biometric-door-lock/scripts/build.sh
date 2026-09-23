#!/usr/bin/env bash
set -e
source "$(dirname "$0")/env.sh"
cd "$DOORLOCK_ROOT/firmware"
idf.py build "$@"
