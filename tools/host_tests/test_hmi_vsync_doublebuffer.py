from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
lcd = (ROOT/'user/gui/gw_lcd.c').read_text()
hdr = (ROOT/'user/gui/gw_lcd.h').read_text()
port = (ROOT/'user/gui/gw_lv_port.c').read_text()

assert '#define GW_LCD_FB0_ADDR' in hdr
assert '#define GW_LCD_FB1_ADDR' in hdr
assert 'RCU_PLL2R_DIV4' in lcd
assert 'RCU_PLL2R_DIV8' not in lcd
assert 'TLI_FRAME_BLANK_RELOAD_EN' in lcd
assert 'TLI_LXFBADDR(LAYER0) = next;' in lcd
assert '(TLI_RL & TLI_RL_FBR)' in lcd
assert 'lv_display_flush_is_last(display)' in port
assert 'gw_lcd_present' in port
assert 'PCLK=25.0MHz refresh=54.3Hz' in lcd
print('HMI VSYNC/double-framebuffer regression: PASS')
