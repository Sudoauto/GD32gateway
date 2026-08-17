#!/usr/bin/env python3
"""Structural guard for the v0.7.0 5-inch LVGL GUI baseline."""
from pathlib import Path
import re
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
CFG = (ROOT / "user/config/gateway_build_config.h").read_text()
ETH = (ROOT / "user/net/gw_eth_port.c").read_text()
CACHE = (ROOT / "user/bsp/bsp_cache.c").read_text()
LCD = (ROOT / "user/gui/gw_lcd.c").read_text()
GUI = (ROOT / "user/gui/gw_gui.c").read_text()
TOUCH = (ROOT / "user/gui/gw_touch.c").read_text()
POINT_H = (ROOT / "user/service/point/point_db.h").read_text()
DEV_H = (ROOT / "user/service/device/device_manager.h").read_text()


def val(name):
    m = re.search(rf"^#define\s+{name}\s+(\d+)U?\s*$", CFG, re.M)
    assert m, name
    return int(m.group(1))

assert val("GW_GUI_ENABLE") == 1
assert val("GW_ETH_RMII_REFCLK_PA8_MCO") == 0
assert val("GW_ETH_RMII_REFCLK_EXTERNAL_50M") == 1
assert "PA8 conflict" in CFG
assert "GW_ETH_RMII_REFCLK_PA8_MCO" in ETH
assert "0xC0000000UL" in CACHE and "MPU_REGION_SIZE_4MB" in CACHE
assert "GW_LCD_HOR_RES" in LCD and "GW_LCD_VER_RES" in LCD
assert (("ipa_interrupt_flag_clear(IPA_INT_FLAG_FTF)" in LCD) or
        ("TLI_FRAME_BLANK_RELOAD_EN" in LCD and "GW_LCD_FB1_ADDR" in LCD))
assert "GW_TOUCH_IO_TIMEOUT_MS" in TOUCH and "i2c_recover" in TOUCH and "maybe_reprobe" in TOUCH
assert "while(size)" not in TOUCH
assert "point_db_snapshot" in POINT_H
assert "device_manager_snapshot" in DEV_H
assert "lv_timer_handler" in GUI
assert "Device control" in GUI and "Recent traffic" in GUI and "Service & diagnostics" in GUI
assert "gw_command_modbus_read_holding_slave" in GUI and "gw_command_modbus_write_single_slave" in GUI

r = ET.parse(ROOT / "project/gateway.uvprojx").getroot()
paths = {e.text.replace('\\', '/').lower() for e in r.findall('.//FilePath') if e.text}
for required in (
    '../user/gui/gw_lcd.c', '../user/gui/gw_touch.c', '../user/gui/gw_lv_port.c',
    '../user/gui/gw_gui.c', '../user/gui/sdram/bsp_exmc_sdram.c',
    '../firmware/gd32h7xx_standard_peripheral/source/gd32h7xx_i2c.c',
    '../firmware/gd32h7xx_standard_peripheral/source/gd32h7xx_exmc.c',
    '../firmware/gd32h7xx_standard_peripheral/source/gd32h7xx_tli.c',
    '../firmware/gd32h7xx_standard_peripheral/source/gd32h7xx_ipa.c'):
    assert required in paths, required
lvgl = [p for p in paths if '../vendor/lvgl-9.2.2/lvgl/src/' in p and p.endswith('.c')]
assert 70 <= len(lvgl) <= 100, len(lvgl)
for forbidden in ("lv_table_create", "lv_tabview_create", "lv_chart_create", "lv_menu_create"):
    assert forbidden not in GUI, forbidden
print(f"GUI/LVGL optimized HMI regression: PASS ({len(lvgl)} LVGL sources)")
