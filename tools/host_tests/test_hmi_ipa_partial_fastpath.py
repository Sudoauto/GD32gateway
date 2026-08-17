from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
port = (ROOT/'user/gui/gw_lv_port.c').read_text()
lcd = (ROOT/'user/gui/gw_lcd.c').read_text()
hdr = (ROOT/'user/gui/gw_lcd.h').read_text()
cache = (ROOT/'user/bsp/bsp_cache.c').read_text()
gui = (ROOT/'user/gui/gw_gui.c').read_text()
conf = (ROOT/'user/config/gateway_build_config.h').read_text()
lvc = (ROOT/'vendor/lvgl-9.2.2/lv_conf.h').read_text()

assert '#define GW_LCD_DRAW_LINES       40U' in hdr
assert 's_draw_buf0[GW_LCD_DRAWBUF_BYTES]' in port
assert 's_draw_buf1[GW_LCD_DRAWBUF_BYTES]' in port
assert 'LV_DISPLAY_RENDER_MODE_PARTIAL' in port
assert 'LV_DISPLAY_RENDER_MODE_DIRECT' not in port
assert 'bsp_dcache_clean(px_map' in port
assert 'gw_lcd_blit_rgb565' in port
assert 'gw_lcd_copy_rect' in port
assert 'gw_lcd_copy_framebuffer' in port
assert 'ipa_transfer_enable();' in lcd
assert 'FOREGROUND_PPF_RGB565' in lcd
assert 'IPA_DPF_RGB565' in lcd
assert 'IPA partial-blit + VSYNC double-FB' in lcd
assert 'cfg.region_base_address = GW_LVGL_HEAP_ADDR' in cache
assert 'cfg.region_size = MPU_REGION_SIZE_512KB' in cache
assert 'cfg.access_cacheable = MPU_ACCESS_CACHEABLE' in cache
assert 'cfg.region_number = MPU_REGION_NUMBER2' in cache
assert 's_active_page = index;' in gui
assert 'strcmp(current, text) != 0' in gui
assert '#define GW_GUI_TASK_PRIORITY              2U' in conf
assert '#define LV_DEF_REFR_PERIOD  18' in lvc
print('HMI IPA/partial-render performance regression: PASS')
