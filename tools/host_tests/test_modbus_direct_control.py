#!/usr/bin/env python3
"""Regression guard for v0.8.1 direct Modbus operator control."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CFG = (ROOT / "user/config/gateway_build_config.h").read_text()
CMD_H = (ROOT / "user/service/gateway/gw_command_router.h").read_text()
CMD_C = (ROOT / "user/service/gateway/gw_command_router.c").read_text()
BUS = (ROOT / "user/service/rs485/rs485_bus_manager.c").read_text()
DEV_H = (ROOT / "user/service/device/device_manager.h").read_text()
DEV_C = (ROOT / "user/service/device/device_manager.c").read_text()
GUI = (ROOT / "user/gui/gw_gui.c").read_text()
UP = (ROOT / "user/service/gateway/gw_uplink.c").read_text()
DATA = (ROOT / "user/task/task_data.c").read_text()
RTU_H = (ROOT / "user/protocol/modbus/modbus_rtu_master.h").read_text()

# Production demo remains off: manual Modbus must not depend on it.
assert "#define GW_DEMO_MODBUS_CONFIG_ENABLE      0U" in CFG
assert "gw_command_modbus_read_holding_slave" in CMD_H and "gw_command_modbus_read_holding_slave" in CMD_C
assert "gw_command_modbus_write_single_slave" in CMD_H and "gw_command_modbus_write_single_slave" in CMD_C
assert "device_manager_find_binding" in DEV_H and "device_manager_find_binding" in DEV_C
assert "device_id=0 is valid for an operator ad-hoc transaction" in CMD_C
assert "(txn->device_id == 0U)" not in BUS
assert '"Slave ID"' in GUI
assert "gw_command_modbus_read_holding_slave" in GUI
assert "gw_command_modbus_write_single_slave" in GUI
assert 'strcmp(cmd, "MBR")' in UP and "gw_command_modbus_read_holding_slave" in UP
assert 'strcmp(cmd, "MBW")' in UP and "gw_command_modbus_write_single_slave" in UP
assert 'strcmp(cmd, "MBRD")' in UP and 'strcmp(cmd, "MBWD")' in UP
assert "slave_address" in RTU_H and "r->slave_address" in DATA
assert "A Modbus exception response is always 5 bytes" in (ROOT / "user/task/task_rs485.c").read_text()
assert 'GW_LOGI("CMD", "Modbus %s queued' in CMD_C
assert 'FC06 reg=%u value=%u' in DATA
print("Modbus direct FC03/FC06 control regression: PASS")
