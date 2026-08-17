#!/usr/bin/env python3
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
cache = (ROOT/'user/bsp/bsp_cache.c').read_text()
lv = (ROOT/'user/gui/gw_lv_port.c').read_text()
irq = (ROOT/'user/interrupt/gateway_irq.c').read_text()
conf = (ROOT/'vendor/lvgl-9.2.2/lv_conf.h').read_text()
assert 'cfg.region_base_address = 0xC0000000UL' in cache
assert 'cfg.access_bufferable = MPU_ACCESS_NON_BUFFERABLE' in cache
assert 'cfg.access_cacheable = MPU_ACCESS_NON_CACHEABLE' in cache
assert 'cfg.tex_type = MPU_TEX_TYPE1' in cache
assert '#define LV_MEM_ADR 0xC0200000UL' in conf
for marker in ['LVGL core init begin', 'LVGL core init PASS', 'LVGL display PASS', 'touch init begin']:
    assert marker in lv
for reg in ['CFSR', 'HFSR', 'MMFAR', 'BFAR', 'PC   =']:
    assert reg in irq
print('HMI MPU/fault diagnostics regression: PASS')
