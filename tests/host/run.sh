#!/usr/bin/env bash
# Compila ed esegue gli unit test host (g++). Offline-friendly: usa il g++ di
# sistema (Strawberry/MinGW), niente piattaforma native PlatformIO richiesta.
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CXX="${CXX:-g++}"
FAIL=0

run_one() {
  local name="$1"; shift
  echo "=== $name ==="
  "$CXX" -std=c++17 -Wall -Wextra -I "$DIR/stubs" -I "$DIR/../../firmware/1_esp32-hub/Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard/include" "$DIR/$name.cpp" -o "$DIR/build_$name" || { echo "[BUILD FAILED] $name"; FAIL=1; return; }
  "$DIR/build_$name" || FAIL=1
  echo
}

run_one test_crc16
run_one test_crc_utils
run_one test_command_policy
run_one test_sensor_policy
run_one test_persistence

if [ "$FAIL" -ne 0 ]; then echo "SUITE: FAILED"; exit 1; fi
echo "SUITE: ALL PASSED"
