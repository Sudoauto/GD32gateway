#!/usr/bin/env python3
"""Structural guard for Stage2A TX data-phase TDC follow-up."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
CFG = (ROOT / "user/config/gateway_build_config.h").read_text(encoding="utf-8")
DRV = (ROOT / "user/driver/drv_canfd.c").read_text(encoding="utf-8")
TASK = (ROOT / "user/task/task_can.c").read_text(encoding="utf-8")

assert re.search(r"#if \(GW_CANFD_BRINGUP_STAGE >= 2U\)\s*\n#define GW_CANFD_TDC_ACTIVE\s+1U", CFG), (
    "Stage2A must enable TDC after TX-only fdTEC failures"
)
assert "#define GW_CANFD_TDC_OFFSET               (1U + GW_CANFD_DATA_PROP_SEG + GW_CANFD_DATA_SEG1)" in CFG, (
    "TDCO must track the configured data-phase regular sample point"
)
# Stage2 low-rate: 150M / (6 * 25TQ) = 1M, TDCO = 1 + 11 + 8 = 20. Stage3 TDCO = 23.
assert re.search(r"#define GW_CANFD_DATA_PRESCALER\s+6U", CFG), "Stage2 data rate must be 1 Mbit/s at 150 MHz"
assert re.search(r"#define GW_CANFD_DATA_PROP_SEG\s+11U", CFG)
assert re.search(r"#define GW_CANFD_DATA_SEG1\s+8U", CFG)
assert "fd.tdc_enable = (uint32_t)ENABLE;" in DRV, "driver no longer applies configured TDC"
assert "s_stats.tdc_value = can_tdc_get(CAN_PERIPH);" in DRV, "TDCV diagnostic missing"
assert "data=1M BRS=on TDC=ON TDCO=%u" in TASK, "boot diagnostics must expose 1M data rate and computed TDCO"
print("CAN-FD Stage2A 500k/1M TDC regression: PASS")
