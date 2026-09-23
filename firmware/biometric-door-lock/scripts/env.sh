# Source this in every new terminal:   source scripts/env.sh
# Loads ESP-IDF and sets the defaults the other scripts use. Override with environment variables:
#   IDF_PATH (default ~/esp/esp-idf)   DOORLOCK_PORT (default /dev/ttyACM0)
export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
if [ ! -f "$IDF_PATH/export.sh" ]; then echo "ESP-IDF not found at $IDF_PATH (set IDF_PATH)"; return 1 2>/dev/null || exit 1; fi
. "$IDF_PATH/export.sh" > /dev/null 2>&1 || { echo "failed to load ESP-IDF from $IDF_PATH"; return 1 2>/dev/null || exit 1; }
export DOORLOCK_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export DOORLOCK_PORT="${DOORLOCK_PORT:-/dev/ttyACM0}"
echo "door-lock env ready: root=$DOORLOCK_ROOT port=$DOORLOCK_PORT idf=$(idf.py --version 2>/dev/null | head -1)"
