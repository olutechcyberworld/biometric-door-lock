#!/usr/bin/env bash
# Host-only tests: no ESP-IDF, no board. Run from anywhere:  tests/run_host_tests.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
AS="$ROOT/firmware/components/as608"
echo "== layout"; python3 "$ROOT/scripts/check_layout.py"
echo "== python syntax"; python3 -m py_compile "$ROOT"/tools/*.py && echo ok
echo "== AS608 driver vs simulated module"
gcc -std=c11 -Wall -Wextra -c "$AS/as608.c" -I"$AS/include" -o "$T/as608.o"
g++ -std=c++17 -Wall -Wextra "$AS/test/test_as608.cpp" "$T/as608.o" -I"$AS/include" -o "$T/as608_test"
"$T/as608_test"
echo "== auth FSM (Phase 3 core logic)"
AF="$ROOT/firmware/components/auth_fsm"
gcc -std=c11 -Wall -Wextra -c "$AF/auth_fsm.c" -I"$AF/include" -o "$T/auth_fsm.o"
g++ -std=c++17 -Wall -Wextra "$AF/test/test_auth_fsm.cpp" "$T/auth_fsm.o" -I"$AF/include" -o "$T/auth_fsm_test"
"$T/auth_fsm_test"
echo "== retry/lockout (Phase 4 core logic)"
RL="$ROOT/firmware/components/retry_lockout"
gcc -std=c11 -Wall -Wextra -c "$RL/retry_lockout.c" -I"$RL/include" -o "$T/retry_lockout.o"
g++ -std=c++17 -Wall -Wextra "$RL/test/test_retry_lockout.cpp" "$T/retry_lockout.o" -I"$RL/include" -o "$T/rl_test"
"$T/rl_test"
echo "== frame dump (firmware) -> dashboard parser"
FD="$ROOT/tests/host/frame_dump"; W="$ROOT/firmware/third_party/esp-who/components"
g++ -std=c++17 -Wall -Wextra "$FD/dump_frame_test.cpp" -I"$FD/stub" -I"$W/who_detect" -I"$W/who_frame_cap" -o "$T/dump_frame_test"
python3 "$FD/check_roundtrip.py" "$T/dump_frame_test"
echo "== dashboard smoke test (simulated board)"
python3 "$ROOT/tests/host/dashboard_smoke.py"
echo; echo "ALL HOST TESTS PASSED"
