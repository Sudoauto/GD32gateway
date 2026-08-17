#!/usr/bin/env python3
"""Structural regression guard for the v0.9 edge-management baseline."""
from pathlib import Path
import re
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
CFG = (ROOT / "user/config/gateway_build_config.h").read_text()
UP = (ROOT / "user/service/gateway/gw_uplink.c").read_text()
GUI = (ROOT / "user/gui/gw_gui.c").read_text()
TOUCH = (ROOT / "user/gui/gw_touch.c").read_text()
CONF = (ROOT / "user/service/config/gw_config.c").read_text()
APP = (ROOT / "user/app/gateway_app.c").read_text()
MAIN = (ROOT / "user/main.c").read_text()
PROJ = ROOT / "project/gateway.uvprojx"


def macro(name: str) -> int:
    m = re.search(rf"^#define\s+{re.escape(name)}\s+(\d+)U?\s*$", CFG, re.M)
    assert m, name
    return int(m.group(1))

for feature in (
    "GW_RUNTIME_CONFIG_ENABLE", "GW_SNTP_ENABLE", "GW_ALARM_ENABLE",
    "GW_RULE_ENGINE_ENABLE", "GW_WATCHDOG_ENABLE", "GW_SYSLOG_ENABLE",
    "GW_SNMP_ENABLE", "GW_OFFLINE_SPOOL_ENABLE", "GW_AUTH_ENABLE",
    "GW_OTA_ENABLE", "GW_DIAGNOSTICS_ENABLE", "GW_SELFTEST_ENABLE",
):
    assert macro(feature) == 1, feature
assert macro("GW_PTP_ENABLE") == 0
assert macro("GW_CANFD_BRS_ENABLE") == 0
assert any(v in MAIN for v in ("v0.9.0-edge-management", "v0.9.1-hmi-screenfix", "v0.9.2-scheduler-runtimefix", "v0.9.3-hmi-mpu-faultdiag", "v0.9.4-hmi-vsync-doublebuffer", "v0.9.5-hmi-ipa-fastpath", "v0.9.1-hmi-screenfix"))
assert "gateway v0.9 services initialized" in APP

# Runtime config, remote management and transaction rollback.
for token in ("CFGSET", "CFGDEL", "CFGGET", "CFGSAVE", "FACTORY"):
    assert token in UP
assert "s_rollback_csv" in CONF and "apply_document" in CONF
assert "runtime_store" in CONF and "runtime_load" in CONF
assert "gw_config_external_factory_reset" in CONF
assert "delete_point_cascade" in CONF and "delete_device_cascade" in CONF

# Authentication and fail-closed OTA management surface.
for token in ("AUTH", "PASS", "OTA_BEGIN", "OTA_DATA", "OTA_END", "OTA_ABORT", "OTA_STATUS"):
    assert token in UP
assert "EVT_UPLINK_CLIENT_CONNECTED" in UP

# HMI lock, diagnostics, factory reset and touch recovery.
assert "OPERATOR ACCESS" in GUI
assert "Service & diagnostics" in GUI
assert "Factory reset ARMED" in GUI
assert "gw_security_hmi_authenticate" in GUI
assert "gw_diagnostics_run_selftest" in GUI
assert "GW_TOUCH_IO_TIMEOUT_MS" in TOUCH
assert "i2c_recover" in TOUCH and "maybe_reprobe" in TOUCH
assert "background re-probe enabled" in (ROOT / "user/gui/gw_lv_port.c").read_text()

# Single authoritative markdown, trimmed LVGL build, services in Keil target.
root = ET.parse(PROJ).getroot()
paths = [(e.text or "").replace("\\", "/") for e in root.findall(".//FilePath")]
all_c = [p for p in paths if p.lower().endswith(".c")]
lvgl = [p for p in all_c if "vendor/lvgl-9.2.2/lvgl/" in p.lower()]
assert len(all_c) <= 200, len(all_c)
assert 70 <= len(lvgl) <= 100, len(lvgl)
for required in (
    "../user/service/config/gw_config.c", "../user/service/storage/gw_flash_store.c",
    "../user/service/security/gw_security.c", "../user/service/time/gw_sntp.c",
    "../user/service/automation/gw_alarm_rules.c", "../user/service/watchdog/gw_watchdog.c",
    "../user/service/ops/gw_syslog.c", "../user/service/ops/gw_snmp.c",
    "../user/service/diagnostics/gw_diagnostics.c", "../user/service/ota/gw_ota.c",
):
    assert required in paths, required
markdown = list(ROOT.rglob("*.md"))
assert len(markdown) == 1 and markdown[0].name == "GATEWAY_PROJECT.md", markdown

print(f"v0.9 edge-management regression: PASS ({len(all_c)} C sources, {len(lvgl)} LVGL)")
