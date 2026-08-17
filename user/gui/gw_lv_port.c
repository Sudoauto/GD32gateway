#include "gw_lv_port.h"

#include <stdint.h>
#include <string.h>

#include "bsp_cache.h"
#include "gw_lcd.h"
#include "gw_log.h"
#include "gw_time.h"
#include "gw_touch.h"
#include "gw_security.h"

#define GUI_DIRTY_RECTS_MAX 32U

/* v0.9.5 fast path:
 * LVGL rasterizes into AXI SRAM instead of non-cacheable external SDRAM.
 * 800 x 40 x RGB565 = 64 KiB per buffer. Keep 32-byte alignment so a cache
 * clean before IPA reads never touches an unrelated object. */
__attribute__((aligned(32))) static uint8_t s_draw_buf0[GW_LCD_DRAWBUF_BYTES];
__attribute__((aligned(32))) static uint8_t s_draw_buf1[GW_LCD_DRAWBUF_BYTES];

static lv_display_t *s_display;
static lv_indev_t *s_touch_indev;
static bool s_touch_ok;
static bool s_backlight_enabled;

static uint32_t s_frame_target;
static bool s_frame_failed;
static bool s_buffers_synced;
static lv_area_t s_dirty_rects[GUI_DIRTY_RECTS_MAX];
static uint8_t s_dirty_count;
static bool s_dirty_overflow;

static uint32_t lv_tick_ms(void)
{
    return (uint32_t)gw_time_ms();
}

static bool areas_touch(const lv_area_t *a, const lv_area_t *b)
{
    return !((a->x2 + 1) < b->x1 || (b->x2 + 1) < a->x1 ||
             (a->y2 + 1) < b->y1 || (b->y2 + 1) < a->y1);
}

static void dirty_record(const lv_area_t *area)
{
    if (area == NULL || s_dirty_overflow) return;

    for (uint8_t i = 0U; i < s_dirty_count; ++i) {
        if (areas_touch(&s_dirty_rects[i], area)) {
            lv_area_t *d = &s_dirty_rects[i];
            if (area->x1 < d->x1) d->x1 = area->x1;
            if (area->y1 < d->y1) d->y1 = area->y1;
            if (area->x2 > d->x2) d->x2 = area->x2;
            if (area->y2 > d->y2) d->y2 = area->y2;
            return;
        }
    }

    if (s_dirty_count < GUI_DIRTY_RECTS_MAX) {
        s_dirty_rects[s_dirty_count++] = *area;
    } else {
        s_dirty_overflow = true;
    }
}

static bool sync_old_framebuffer(uint32_t new_front, uint32_t old_front)
{
    if (s_dirty_overflow) {
        return gw_lcd_copy_framebuffer(new_front, old_front);
    }

    for (uint8_t i = 0U; i < s_dirty_count; ++i) {
        const lv_area_t *a = &s_dirty_rects[i];
        uint16_t x = (uint16_t)a->x1;
        uint16_t y = (uint16_t)a->y1;
        uint16_t w = (uint16_t)(a->x2 - a->x1 + 1);
        uint16_t h = (uint16_t)(a->y2 - a->y1 + 1);
        if (!gw_lcd_copy_rect(new_front, old_front, x, y, w, h)) {
            return false;
        }
    }
    return true;
}

static void frame_reset(void)
{
    s_frame_target = 0U;
    s_frame_failed = false;
    s_dirty_count = 0U;
    s_dirty_overflow = false;
}

static void display_flush_cb(lv_display_t *display,
                             const lv_area_t *area,
                             uint8_t *px_map)
{
    if ((area == NULL) || (px_map == NULL)) {
        lv_display_flush_ready(display);
        return;
    }

    if (s_frame_target == 0U) {
        uint32_t active = gw_lcd_active_framebuffer();
        s_frame_target = gw_lcd_inactive_framebuffer();
        s_frame_failed = false;
        s_dirty_count = 0U;
        s_dirty_overflow = false;

        /* A failed post-flip mirror makes the inactive FB stale. Repair it
         * once at the beginning of the next LVGL frame, never during TLI scan
         * of the destination. */
        if (!s_buffers_synced) {
            if (!gw_lcd_copy_framebuffer(active, s_frame_target)) {
                s_frame_failed = true;
                GW_LOGE("GUI", "LCD full framebuffer resync failed");
            } else {
                s_buffers_synced = true;
            }
        }
    }

    int32_t width_i = area->x2 - area->x1 + 1;
    int32_t height_i = area->y2 - area->y1 + 1;
    if ((width_i <= 0) || (height_i <= 0) || area->x1 < 0 || area->y1 < 0 ||
        area->x2 >= (int32_t)GW_LCD_HOR_RES || area->y2 >= (int32_t)GW_LCD_VER_RES) {
        s_frame_failed = true;
    } else if (!s_frame_failed) {
        uint16_t width = (uint16_t)width_i;
        uint16_t height = (uint16_t)height_i;
        uint32_t stride_bytes = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565);

        /* AXI SRAM is D-cacheable. IPA is a bus master and cannot see dirty
         * CPU cache lines until they are cleaned. Partial mode gives a packed
         * RGB565 area, so cleaning exactly stride*height is sufficient. */
        bsp_dcache_clean(px_map, (size_t)stride_bytes * height);

        if (!gw_lcd_blit_rgb565(s_frame_target,
                                (uint16_t)area->x1, (uint16_t)area->y1,
                                width, height, px_map,
                                (uint16_t)(stride_bytes / GW_LCD_BYTES_PER_PIXEL))) {
            s_frame_failed = true;
            GW_LOGE("GUI", "IPA LCD blit failed area=%ld,%ld-%ld,%ld",
                    (long)area->x1, (long)area->y1, (long)area->x2, (long)area->y2);
        } else {
            dirty_record(area);
        }
    }

    if (lv_display_flush_is_last(display)) {
        if (s_frame_failed) {
            /* Target contains a partial frame. Restore it from the currently
             * displayed framebuffer so the next page flip can start clean. */
            s_buffers_synced = gw_lcd_copy_framebuffer(gw_lcd_active_framebuffer(),
                                                        s_frame_target);
        } else {
            uint32_t old_front = gw_lcd_active_framebuffer();
            uint32_t new_front = s_frame_target;
            if (!gw_lcd_present((const void *)(uintptr_t)new_front)) {
                s_buffers_synced = false;
                GW_LOGE("GUI", "LCD/TLI page flip failed");
            } else {
                /* After VSYNC the old front is off-screen. Mirror only the
                 * areas changed in this LVGL frame so both FBs stay coherent. */
                s_buffers_synced = sync_old_framebuffer(new_front, old_front);
                if (!s_buffers_synced) {
                    GW_LOGE("GUI", "LCD back-buffer coherence copy failed");
                }
                if (!s_backlight_enabled) {
                    gw_lcd_backlight(true);
                    s_backlight_enabled = true;
                }
            }
        }
        frame_reset();
    }

    lv_display_flush_ready(display);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static uint16_t last_x;
    static uint16_t last_y;

    uint16_t x = last_x;
    uint16_t y = last_y;
    bool pressed = false;
    if (!gw_touch_read(&x, &y, &pressed)) {
        /* An I2C error must not stall LVGL. Keep the last coordinate and
         * report released; the touch driver will recover on later polls. */
        pressed = false;
    }

    last_x = x;
    last_y = y;
    data->point.x = (int32_t)x;
    data->point.y = (int32_t)y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (pressed) {
        gw_security_touch_session();
    }
}

bool gw_lv_port_init(void)
{
    s_display = NULL;
    s_touch_indev = NULL;
    s_touch_ok = false;
    s_backlight_enabled = false;
    s_buffers_synced = true;
    frame_reset();

    if (!gw_lcd_init()) {
        GW_LOGE("GUI", "LCD/SDRAM/TLI initialization failed");
        return false;
    }

    GW_LOGI("GUI", "LVGL core init begin heap=0x%08lX size=%lu",
            (unsigned long)GW_LVGL_HEAP_ADDR, (unsigned long)GW_LVGL_HEAP_BYTES);
    lv_init();
    GW_LOGI("GUI", "LVGL core init PASS");
    lv_tick_set_cb(lv_tick_ms);
    s_backlight_enabled = true;

    GW_LOGI("GUI", "LVGL display create begin");
    s_display = lv_display_create((int32_t)GW_LCD_HOR_RES,
                                  (int32_t)GW_LCD_VER_RES);
    if (s_display == NULL) {
        GW_LOGE("GUI", "lv_display_create failed");
        return false;
    }

    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, display_flush_cb);
    lv_display_set_buffers(s_display,
                           s_draw_buf0,
                           s_draw_buf1,
                           sizeof(s_draw_buf0),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    GW_LOGI("GUI", "LVGL display PASS partial-IPA draw0=0x%08lX draw1=0x%08lX %ux%u (%luB each) FB0=0x%08lX FB1=0x%08lX",
            (unsigned long)(uintptr_t)s_draw_buf0, (unsigned long)(uintptr_t)s_draw_buf1,
            (unsigned)GW_LCD_HOR_RES, (unsigned)GW_LCD_DRAW_LINES,
            (unsigned long)sizeof(s_draw_buf0),
            (unsigned long)GW_LCD_FB0_ADDR, (unsigned long)GW_LCD_FB1_ADDR);
    if (((uintptr_t)s_draw_buf0 < 0x24000000UL) ||
        ((uintptr_t)s_draw_buf1 + sizeof(s_draw_buf1) > 0x240D0000UL)) {
        GW_LOGW("GUI", "partial draw buffers are not fully inside AXI SRAM; performance may degrade");
    }

    GW_LOGI("GUI", "touch init begin");
    s_touch_ok = gw_touch_init();
    GW_LOGI("GUI", "touch init %s", s_touch_ok ? "PASS" : "DEGRADED");
    /* Always register the LVGL pointer device. If Goodix is temporarily absent
     * or the I2C bus is wedged at boot, gw_touch_read() can recover/re-probe
     * later. Permanently omitting the indev made an otherwise recoverable
     * touch failure look like a frozen HMI until reboot. */
    s_touch_indev = lv_indev_create();
    if (s_touch_indev != NULL) {
        lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_touch_indev, touch_read_cb);
        lv_indev_set_display(s_touch_indev, s_display);
        if (s_touch_ok) {
            GW_LOGI("GUI", "touch ready product=%s max=%u",
                    gw_touch_product_id(), (unsigned)gw_touch_max_points());
        } else {
            GW_LOGW("GUI", "Goodix touch unavailable at boot; background re-probe enabled");
        }
    } else {
        s_touch_ok = false;
        GW_LOGE("GUI", "lv_indev_create failed; touch disabled");
    }

    return true;
}

lv_display_t *gw_lv_display(void)
{
    return s_display;
}

bool gw_lv_touch_available(void)
{
    return (s_touch_indev != NULL) && gw_touch_available();
}
