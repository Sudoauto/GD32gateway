#!/usr/bin/env python3
"""Parallel host syntax check for the LVGL sources enabled in the Keil project."""
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import os
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
proj = ROOT / "project/gateway.uvprojx"
r = ET.parse(proj).getroot()
sources = []
for group in r.iter("Group"):
    if group.findtext("GroupName") == "LVGL 9.2.2":
        for e in group.iter("FilePath"):
            if e.text:
                sources.append((proj.parent / e.text.replace("\\", "/")).resolve())

inc_dirs = [
    "Firmware/GD32H7xx_standard_peripheral/Include", "Firmware/CMSIS/GD/GD32H7xx/Include",
    "user/config", "user/bsp", "user/driver", "user/interrupt", "user/rtos", "user/common",
    "user/service/rs485", "user/service/device", "user/service/point", "user/service/poll", "user/service/gateway",
    "user/service/config", "user/service/automation", "user/service/security", "user/service/storage", "user/service/time",
    "user/service/watchdog", "user/service/ops", "user/service/diagnostics", "user/service/ota",
    "user/protocol/modbus", "user/protocol/can", "user/task", "user/app", "user/net", "user/gui",
    "vendor/FreeRTOS-Kernel-10.3.1/include", "vendor/FreeRTOS-Kernel-10.3.1/portable/GCC/ARM_CM7/r0p1",
    "vendor/lwip-2.1.2/src/include", "vendor/lwip-2.1.2/port/GD32H7xx",
    "vendor/lwip-2.1.2/port/GD32H7xx/FreeRTOS",
    "vendor/lvgl-9.2.2", "vendor/lvgl-9.2.2/lvgl",
]
base = ["clang", "-fsyntax-only", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-Wno-unused-function",
        "-D__ARMCC_VERSION=6160000", "-DGD32H7XX",
        "-include", str(ROOT / "tools/host_tests/stubs/arm_host_intrinsics.h")]
for d in inc_dirs:
    base += ["-I", str(ROOT / d)]


def check(src: Path):
    cp = subprocess.run(base + [str(src)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                        text=True)
    return src, cp.returncode, cp.stdout, cp.stderr

workers = min(8, max(2, os.cpu_count() or 2))
errors = []
with ThreadPoolExecutor(max_workers=workers) as ex:
    futs = [ex.submit(check, s) for s in sources]
    for fut in as_completed(futs):
        src, rc, out, err = fut.result()
        if rc:
            errors.append((src, out, err))

if errors:
    for src, out, err in errors[:10]:
        print(f"FAILED: {src}", file=sys.stderr)
        if out:
            print(out, file=sys.stderr)
        if err:
            print(err, file=sys.stderr)
    raise SystemExit(1)

print(f"LVGL syntax: PASS ({len(sources)} files, {workers} workers)")
