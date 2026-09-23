#!/usr/bin/env bash
# Debug dashboard in the browser (http://localhost:8000). Usage: scripts/dashboard.sh [port] [extra dashboard.py args]
set -e
source "$(dirname "$0")/env.sh"
PORT="${1:-$DOORLOCK_PORT}"; shift || true
python "$DOORLOCK_ROOT/tools/dashboard.py" --port "$PORT" "$@"
