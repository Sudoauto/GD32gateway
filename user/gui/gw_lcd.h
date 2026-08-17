#ifndef GW_LCD_H
#define GW_LCD_H

#include <stdbool.h>
#include <stdint.h>

#define GW_LCD_HOR_RES          800U
#define GW_LCD_VER_RES          480U
#define GW_LCD_BYTES_PER_PIXEL  2U
#define GW_LCD_FRAME_BYTES      (GW_LCD_HOR_RES * GW_LCD_VER_RES * GW_LCD_BYTES_PER_PIXEL)

/* Tear-free RGB display memory map.
 *
 * FB0/FB1 are two complete RGB565 scan-out buffers in external SDRAM.
 * v0.9.5 no longer lets LVGL render pixel-by-pixel into non-cacheable SDRAM.
 * LVGL renders changed areas into small internal SRAM buffers and IPA copies
 * those areas into the off-screen framebuffer. TLI changes framebuffer only
 * during frame blank, then IPA mirrors the changed areas into the old buffer so
 * both full framebuffers remain coherent for the next page flip. */
#define GW_LCD_FB0_ADDR         0xC0000000UL
#define GW_LCD_FB1_ADDR         0xC0100000UL

/* Compatibility alias used by older diagnostics/documentation. */
#define GW_LCD_FRAMEBUFFER_ADDR GW_LCD_FB0_ADDR

/* Partial RGB565 draw buffers live in internal AXI SRAM. 40 lines is ~8.3% of
 * the panel and keeps each buffer at 64 KiB. Two buffers are reserved so a
 * future interrupt-driven IPA path can overlap rendering and DMA without
 * changing the display API. */
#define GW_LCD_DRAW_LINES       40U
#define GW_LCD_DRAWBUF_BYTES    (GW_LCD_HOR_RES * GW_LCD_DRAW_LINES * GW_LCD_BYTES_PER_PIXEL)

/* LVGL dynamic heap lives after the two full-screen framebuffers. The MPU
 * overlays this 512 KiB range as cacheable normal memory; the scan-out buffers
 * remain non-cacheable for TLI/IPA coherency. */
#define GW_LVGL_HEAP_ADDR       0xC0200000UL
#define GW_LVGL_HEAP_BYTES      0x00080000UL

/* With 25 MHz HXTAL: 25 / 25 * 300 / 3 / 4 = 25 MHz pixel clock.
 * Total timing is 887 x 519, giving about 54.3 Hz panel refresh. */
#define GW_LCD_PIXEL_CLOCK_HZ   25000000UL
#define GW_LCD_REFRESH_MILLIHZ  54306UL

typedef struct {
    uint32_t flush_count;          /* successful frame-blank page flips */
    uint32_t flush_timeout_count;  /* page flip did not complete in time */
    uint32_t flush_error_count;    /* TLI FIFO/transfer error observed */
    uint32_t tli_fifo_error_count;
    uint32_t tli_transfer_error_count;
    uint32_t active_framebuffer;
    uint32_t ipa_blit_count;       /* partial/internal-buffer -> SDRAM copies */
    uint32_t ipa_sync_count;       /* front -> back coherence copies */
    uint32_t ipa_bytes;
    uint32_t ipa_timeout_count;
    uint32_t ipa_error_count;
    uint32_t full_resync_count;
} gw_lcd_stats_t;

bool gw_lcd_init(void);
bool gw_lcd_present(const void *framebuffer_rgb565);
uint32_t gw_lcd_active_framebuffer(void);
uint32_t gw_lcd_inactive_framebuffer(void);
bool gw_lcd_blit_rgb565(uint32_t destination_framebuffer,
                        uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                        const void *source_rgb565, uint16_t source_stride_pixels);
bool gw_lcd_copy_rect(uint32_t source_framebuffer, uint32_t destination_framebuffer,
                      uint16_t x, uint16_t y, uint16_t width, uint16_t height);
bool gw_lcd_copy_framebuffer(uint32_t source_framebuffer, uint32_t destination_framebuffer);
void gw_lcd_backlight(bool on);
void gw_lcd_get_stats(gw_lcd_stats_t *out);

#endif /* GW_LCD_H */
