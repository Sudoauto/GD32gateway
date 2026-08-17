#!/usr/bin/env python3
"""Project-level production-baseline guard before northbound development."""
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
APP = (ROOT / "user/app/gateway_app.c").read_text(encoding="utf-8")
POINT_H = (ROOT / "user/service/point/point_db.h").read_text(encoding="utf-8")
POINT_C = (ROOT / "user/service/point/point_db.c").read_text(encoding="utf-8")
TYPES = (ROOT / "user/common/gw_types.h").read_text(encoding="utf-8")

# Project XML must remain parseable and include the production gateway modules.
root = ET.parse(ROOT / "project/gateway.uvprojx").getroot()
paths = {e.text.replace('\\', '/').lower() for e in root.findall('.//FilePath') if e.text}
for required in (
    '../user/driver/drv_canfd.c', '../user/driver/drv_rs485.c',
    '../user/task/task_can.c', '../user/task/task_rs485.c', '../user/task/task_data.c',
    '../user/protocol/can/can_decoder.c', '../user/protocol/modbus/modbus_rtu_master.c',
    '../user/service/point/point_db.c', '../user/service/device/device_manager.c',
    '../user/service/poll/poll_scheduler.c'):
    assert required in paths, f"Keil project missing {required}"

# Demo registration must be compile-time gated; baseline config test verifies it is OFF.
assert "GW_DEMO_MODBUS_CONFIG_ENABLE" in APP
assert "GW_DEMO_CAN_CONFIG_ENABLE" in APP

# Northbound-safe dirty ACK contract.
assert "uint32_t revision;" in TYPES
assert "point_db_ack_dirty" in POINT_H and "point_db_ack_dirty" in POINT_C
assert "revision != expected_revision" in POINT_C
assert "point_db_snapshot" in POINT_H and "point_db_snapshot" in POINT_C

print("project stable-baseline regression: PASS")
