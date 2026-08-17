#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
INC=(
  -I"$ROOT/Firmware/GD32H7xx_standard_peripheral/Include"
  -I"$ROOT/Firmware/CMSIS/GD/GD32H7xx/Include"
  -I"$ROOT/user/config" -I"$ROOT/user/bsp" -I"$ROOT/user/driver"
  -I"$ROOT/user/interrupt" -I"$ROOT/user/rtos" -I"$ROOT/user/common"
  -I"$ROOT/user/service/rs485" -I"$ROOT/user/service/device"
  -I"$ROOT/user/service/point" -I"$ROOT/user/service/poll"
  -I"$ROOT/user/service/gateway" -I"$ROOT/user/service/config" -I"$ROOT/user/service/automation"
  -I"$ROOT/user/service/security" -I"$ROOT/user/service/storage" -I"$ROOT/user/service/time"
  -I"$ROOT/user/service/watchdog" -I"$ROOT/user/service/ops" -I"$ROOT/user/service/diagnostics" -I"$ROOT/user/service/ota"
  -I"$ROOT/user/protocol/modbus" -I"$ROOT/user/protocol/can"
  -I"$ROOT/user/task" -I"$ROOT/user/app" -I"$ROOT/user/net" -I"$ROOT/user/gui"
  -I"$ROOT/vendor/lvgl-9.2.2" -I"$ROOT/vendor/lvgl-9.2.2/lvgl"
  -I"$ROOT/vendor/FreeRTOS-Kernel-10.3.1/include"
  -I"$ROOT/vendor/FreeRTOS-Kernel-10.3.1/portable/GCC/ARM_CM7/r0p1"
  -I"$ROOT/vendor/lwip-2.1.2/src/include"
  -I"$ROOT/vendor/lwip-2.1.2/port/GD32H7xx"
  -I"$ROOT/vendor/lwip-2.1.2/port/GD32H7xx/FreeRTOS"
)
COMMON=(-fsyntax-only -std=c11 -Wall -Wextra -Werror -D__ARMCC_VERSION=6160000 -DGD32H7XX
        -include "$ROOT/tools/host_tests/stubs/arm_host_intrinsics.h")

while IFS= read -r -d '' f; do
  clang "${COMMON[@]}" "${INC[@]}" "$f"
done < <(find "$ROOT/user" -type f -name '*.c' -print0 | sort -z)

python3 - "$ROOT" <<'PY' > "${TMPDIR:-/tmp}/gateway_lwip_sources.txt"
import pathlib, sys, xml.etree.ElementTree as ET
root = pathlib.Path(sys.argv[1])
p = root / 'project/gateway.uvprojx'
r = ET.parse(p).getroot()
for group in r.iter('Group'):
    name = group.find('GroupName')
    if name is not None and name.text == 'lwIP 2.1.2':
        for elem in group.iter('FilePath'):
            print((p.parent / elem.text.replace('\\', '/')).resolve())
PY

count=0
while IFS= read -r f; do
  clang "${COMMON[@]}" "${INC[@]}" "$f"
  count=$((count + 1))
done < "${TMPDIR:-/tmp}/gateway_lwip_sources.txt"

# Vendor MMIO macros intentionally cast 32-bit target addresses to pointers;
# suppress that host-64-bit-only warning while still checking C syntax.
clang "${COMMON[@]}" -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast \
  "${INC[@]}" "$ROOT/Firmware/GD32H7xx_standard_peripheral/Source/gd32h7xx_enet.c"

for spl in gd32h7xx_i2c.c gd32h7xx_exmc.c gd32h7xx_tli.c gd32h7xx_ipa.c gd32h7xx_fmc.c gd32h7xx_fwdgt.c; do
  clang "${COMMON[@]}" -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast \
    "${INC[@]}" "$ROOT/Firmware/GD32H7xx_standard_peripheral/Source/$spl"
done

python3 "$ROOT/tools/host_tests/check_lvgl_compile.py"

echo "embedded syntax: PASS (all user C + $count lwIP + LVGL + ENET/I2C/EXMC/TLI/IPA/FMC/FWDGT SPL)"
