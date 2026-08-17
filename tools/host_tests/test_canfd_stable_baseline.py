#!/usr/bin/env python3
"""Static configuration/errata guard for v0.5.0 CAN-FD 500k BRS-OFF baseline."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
CFG = (ROOT / "user/config/gateway_build_config.h").read_text(encoding="utf-8")
DRV = (ROOT / "user/driver/drv_canfd.c").read_text(encoding="utf-8")
APP = (ROOT / "user/app/gateway_app.c").read_text(encoding="utf-8")
TASK = (ROOT / "user/task/task_can.c").read_text(encoding="utf-8")
MAIN = (ROOT / "user/main.c").read_text(encoding="utf-8")


def val(name: str) -> int:
    m = re.search(rf"^#define\s+{re.escape(name)}\s+(\d+)U?\s*$", CFG, re.M)
    assert m, f"missing numeric macro {name}"
    return int(m.group(1))


assert val("GW_CANFD_BRS_ENABLE") == 0
assert val("GW_CANFD_TDC_ACTIVE") == 0
assert val("GW_DEMO_MODBUS_CONFIG_ENABLE") == 0
assert val("GW_DEMO_CAN_CONFIG_ENABLE") == 0
assert val("GW_M3_BOARD_VALIDATION_ENABLE") == 0
assert val("GW_CANFD_RX_TRACE_ENABLE") == 0

nominal = [val("GW_CANFD_NOMINAL_PRESCALER"), val("GW_CANFD_NOMINAL_SJW"),
           val("GW_CANFD_NOMINAL_PROP_SEG"), val("GW_CANFD_NOMINAL_SEG1"),
           val("GW_CANFD_NOMINAL_SEG2")]
data = [val("GW_CANFD_DATA_PRESCALER"), val("GW_CANFD_DATA_SJW"),
        val("GW_CANFD_DATA_PROP_SEG"), val("GW_CANFD_DATA_SEG1"),
        val("GW_CANFD_DATA_SEG2")]
assert nominal == [60, 1, 2, 5, 2], nominal
assert data == nominal, "BRS-OFF defensive data timing must equal nominal timing"
assert 300_000_000 // (nominal[0] * (1 + nominal[2] + nominal[3] + nominal[4])) == 500_000

assert "RCU_CANSRC_APB2" in DRV and "RCU_CANSRC_APB2_DIV2" not in DRV
assert "fd.bitrate_switch_enable = (GW_CANFD_BRS_ENABLE != 0U)" in DRV
assert "if (frame->brs)" in DRV, "BRS TX requests must be rejected when baseline disables BRS"
assert "can_mailbox_receive_unlock(CAN_PERIPH);" in DRV, "RX BUSY error must explicitly unlock mailbox"
assert "CAN_ERR0(CAN_PERIPH) &= ~CAN_ERR0_TECNT;" in DRV, "bus-off recovery TEC workaround missing"
assert "xQueueSendFromISR(q_can_rx" in DRV, "RX mailbox must be copied to queue in ISR"

normal_pos = DRV.find("can_operation_mode_enter(CAN_PERIPH, CAN_NORMAL_MODE)")
tx_cfg_pos = DRV.find("can_mailbox_config(CAN_PERIPH, CAN_TX_MB, &txmd)")
assert 0 <= normal_pos < tx_cfg_pos, "TX mailbox must be initialized after entering normal mode"

assert "register_modbus_demo_objects" in APP
assert "register_can_demo_objects" in APP
assert any(v in APP for v in ("southbound stable + Ethernet + TCP baseline", "southbound + Ethernet + TCP + LVGL GUI baseline", "unified southbound + Ethernet + TCP + uplink + HMI", "gateway v0.9 services initialized"))
assert "CAN2 stable: CAN-FD 500k, BRS=OFF" in TASK
assert any(v in MAIN for v in ("v0.5.0-canfd500-stable", "v0.6.0-eth-lwip-baseline", "v0.6.1-tcp-baseline", "v0.7.0-gui-lvgl-baseline", "v0.8.0-unified-gateway-hmi", "v0.8.1-modbus-direct-control", "v0.9.0-edge-management", "v0.9.1-hmi-screenfix", "v0.9.2-scheduler-runtimefix", "v0.9.3-hmi-mpu-faultdiag", "v0.9.4-hmi-vsync-doublebuffer", "v0.9.5-hmi-ipa-fastpath", "v0.9.1-hmi-screenfix"))

print("CAN-FD 500k BRS-OFF stable baseline regression: PASS")
