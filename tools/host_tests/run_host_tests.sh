#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/gateway_host_tests"
mkdir -p "$OUT"
COMMON=(-std=c11 -Wall -Wextra -Werror -I"$ROOT/tools/host_tests/stubs" -I"$ROOT/user/common")
clang "${COMMON[@]}" -I"$ROOT/user/protocol/modbus" \
  "$ROOT/tools/host_tests/test_modbus.c" "$ROOT/user/protocol/modbus/modbus_rtu_master.c" \
  -o "$OUT/test_modbus"
"$OUT/test_modbus"
clang "${COMMON[@]}" -I"$ROOT/user/protocol/can" -I"$ROOT/user/driver" \
  "$ROOT/tools/host_tests/test_can_decoder.c" "$ROOT/user/protocol/can/can_decoder.c" \
  -o "$OUT/test_can_decoder"
"$OUT/test_can_decoder"
clang -std=c11 -Wall -Wextra -Werror \
  -I"$ROOT/user/service/point" -I"$ROOT/user/service/device" -I"$ROOT/user/common" \
  -I"$ROOT/tools/host_tests/stubs" -I"$ROOT/user/rtos" \
  "$ROOT/tools/host_tests/test_point_device.c" "$ROOT/user/service/point/point_db.c" \
  "$ROOT/user/service/device/device_manager.c" -o "$OUT/test_point_device"
"$OUT/test_point_device"
python3 "$ROOT/tools/host_tests/test_canfd_irq_hold_regression.py"
python3 "$ROOT/tools/host_tests/test_canfd_stable_baseline.py"
python3 "$ROOT/tools/host_tests/test_project_baseline.py"
python3 "$ROOT/tools/host_tests/test_ethernet_baseline.py"
python3 "$ROOT/tools/host_tests/test_tcp_baseline.py"
python3 "$ROOT/tools/host_tests/test_gui_baseline.py"
python3 "$ROOT/tools/host_tests/test_v090_integrated.py"
python3 "$ROOT/tools/host_tests/test_modbus_direct_control.py"
"$ROOT/tools/host_tests/check_ethernet_compile.sh"
python3 "$ROOT/tools/host_tests/test_runtime_stats_isr_safety.py"
python3 "$ROOT/tools/host_tests/test_hmi_screenfix.py"
python3 "$ROOT/tools/host_tests/test_hmi_mpu_faultdiag.py"
python3 "$ROOT/tools/host_tests/test_hmi_vsync_doublebuffer.py"
python3 "$ROOT/tools/host_tests/test_hmi_ipa_partial_fastpath.py"
echo "host regressions: PASS"
