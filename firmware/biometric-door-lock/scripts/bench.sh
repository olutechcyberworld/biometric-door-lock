#!/usr/bin/env bash
# Face benchmark: 50 valid runs for one condition. Close the dashboard first (it holds the port).
# Usage: scripts/bench.sh <label> [port]        e.g. scripts/bench.sh dim_indoor
set -e
source "$(dirname "$0")/env.sh"
LABEL="${1:?usage: bench.sh <label> [port]}"; PORT="${2:-$DOORLOCK_PORT}"
mkdir -p "$DOORLOCK_ROOT/data/phase1"
python "$DOORLOCK_ROOT/tools/bench.py" --port "$PORT" --n 50 --label "$LABEL" --out "$DOORLOCK_ROOT/data/phase1/$LABEL.csv"
