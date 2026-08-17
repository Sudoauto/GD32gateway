#!/usr/bin/env python3
"""Structural guard for v0.9.1 HMI black-screen recovery."""
from pathlib import Path
import re
ROOT = Path(__file__).resolve().parents[2]
CONF = (ROOT / 'vendor/lvgl-9.2.2/lv_conf.h').read_text()
LCDH = (ROOT / 'user/gui/gw_lcd.h').read_text()
LCD = (ROOT / 'user/gui/gw_lcd.c').read_text()
GUI = (ROOT / 'user/gui/gw_gui.c').read_text()
MAIN = (ROOT / 'user/main.c').read_text()

def macro(text, name):
    m = re.search(rf'^#define\s+{name}\s+([^\s/]+)', text, re.M)
    assert m, name
    return m.group(1)
assert any(v in MAIN for v in ('v0.9.1-hmi-screenfix', 'v0.9.2-scheduler-runtimefix', 'v0.9.3-hmi-mpu-faultdiag', 'v0.9.4-hmi-vsync-doublebuffer', 'v0.9.5-hmi-ipa-fastpath'))
assert '512 * 1024U' in CONF
assert 'LV_MEM_ADR 0xC0200000UL' in CONF
assert 'GW_LVGL_HEAP_ADDR       0xC0200000UL' in LCDH
assert 'GW_LVGL_HEAP_BYTES      0x00080000UL' in LCDH
assert (('GW_LCD_FB1_ADDR + GW_LCD_FRAME_BYTES) <= GW_LVGL_HEAP_ADDR' in LCD) or ('GW_LVGL_DRAWBUF_ADDR + GW_LCD_FRAME_BYTES) <= GW_LVGL_HEAP_ADDR' in LCD))
assert 'GW_LVGL_HEAP_ADDR + GW_LVGL_HEAP_BYTES) <= LCD_SDRAM_TEST_ADDR' in LCD
assert ('lcd_fill_buffer(GW_LCD_FB0_ADDR, LCD_BOOT_RGB565)' in LCD or 'lcd_fill_framebuffer(LCD_BOOT_RGB565)' in LCD)
assert 'gw_lcd_backlight(true);' in LCD
assert 'SDRAM self-test PASS' in LCD and 'TLI ready 800x480 RGB565' in LCD and 'backlight ON' in LCD
assert 'lv_mem_monitor(&mem);' in GUI
assert 'lv_refr_now(NULL);' in GUI
assert 'creating HMI object tree' in GUI
print('HMI screenfix regression: PASS')
