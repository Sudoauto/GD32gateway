#include "gw_lcd.h"

#include <string.h>

#include "gd32h7xx.h"
#include "gd32h7xx_tli.h"
#include "gd32h7xx_ipa.h"
#include "gw_time.h"
#include "gw_log.h"
#include "sdram/bsp_exmc_sdram.h"

/* 5-inch 800x480 RGB timing from the supplied official TLI/LVGL example. */
#define LCD_HSW  1U
#define LCD_HBP  46U
#define LCD_HFP  40U
#define LCD_VSW  3U
#define LCD_VBP  23U
#define LCD_VFP  13U

#define LCD_PAGEFLIP_TIMEOUT_MS  50U
#define LCD_IPA_TIMEOUT_MS       20U
#define LCD_SDRAM_TEST_ADDR      0xC03FF000UL
#define LCD_SDRAM_NOCACHE_END   0xC0400000UL
#define LCD_BOOT_RGB565          0x0842U

_Static_assert((GW_LCD_FB0_ADDR + GW_LCD_FRAME_BYTES) <= GW_LCD_FB1_ADDR,
               "LCD framebuffer A/B overlap");
_Static_assert((GW_LCD_FB1_ADDR + GW_LCD_FRAME_BYTES) <= GW_LVGL_HEAP_ADDR,
               "LCD framebuffer B overlaps LVGL SDRAM heap");
_Static_assert((GW_LVGL_HEAP_ADDR + GW_LVGL_HEAP_BYTES) <= LCD_SDRAM_TEST_ADDR,
               "LVGL SDRAM heap overlaps SDRAM self-test area");
_Static_assert((LCD_SDRAM_TEST_ADDR + 16U) <= LCD_SDRAM_NOCACHE_END,
               "LCD SDRAM allocations exceed non-cacheable MPU region");

static gw_lcd_stats_t s_stats;
static uint32_t s_active_fb;

static void lcd_pin(uint32_t port, uint32_t pin, uint32_t af)
{
    gpio_af_set(port, af, pin);
    gpio_mode_set(port, GPIO_MODE_AF, GPIO_PUPD_NONE, pin);
    gpio_output_options_set(port, GPIO_OTYPE_PP, GPIO_OSPEED_100_220MHZ, pin);
}

static void lcd_gpio_init(void)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_GPIOC);
    rcu_periph_clock_enable(RCU_GPIOD);
    rcu_periph_clock_enable(RCU_GPIOF);
    rcu_periph_clock_enable(RCU_GPIOG);
    rcu_periph_clock_enable(RCU_GPIOH);

    /* R3..R7 */
    lcd_pin(GPIOA, GPIO_PIN_15, GPIO_AF_9);
    lcd_pin(GPIOH, GPIO_PIN_10, GPIO_AF_14);
    lcd_pin(GPIOH, GPIO_PIN_11, GPIO_AF_14);
    lcd_pin(GPIOA, GPIO_PIN_8,  GPIO_AF_14); /* R6: conflicts with CKOUT0 */
    lcd_pin(GPIOG, GPIO_PIN_6,  GPIO_AF_14);

    /* G2..G7 */
    lcd_pin(GPIOC, GPIO_PIN_0, GPIO_AF_11);
    lcd_pin(GPIOG, GPIO_PIN_10, GPIO_AF_9);
    lcd_pin(GPIOH, GPIO_PIN_15, GPIO_AF_14);
    lcd_pin(GPIOH, GPIO_PIN_4, GPIO_AF_9);
    lcd_pin(GPIOC, GPIO_PIN_7, GPIO_AF_14);
    lcd_pin(GPIOD, GPIO_PIN_3, GPIO_AF_14);

    /* B3..B7 */
    lcd_pin(GPIOG, GPIO_PIN_11, GPIO_AF_14);
    lcd_pin(GPIOG, GPIO_PIN_12, GPIO_AF_9);
    lcd_pin(GPIOB, GPIO_PIN_5, GPIO_AF_3);
    lcd_pin(GPIOB, GPIO_PIN_8, GPIO_AF_14);
    lcd_pin(GPIOB, GPIO_PIN_9, GPIO_AF_14);

    /* PCLK / HSYNC / VSYNC / DE */
    lcd_pin(GPIOB, GPIO_PIN_3, GPIO_AF_2);
    lcd_pin(GPIOC, GPIO_PIN_6, GPIO_AF_14);
    lcd_pin(GPIOA, GPIO_PIN_4, GPIO_AF_14);
    lcd_pin(GPIOF, GPIO_PIN_10, GPIO_AF_14);

    gpio_mode_set(GPIOG, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLDOWN, GPIO_PIN_3);
    gpio_output_options_set(GPIOG, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, GPIO_PIN_3);
    gpio_bit_reset(GPIOG, GPIO_PIN_3);
}

static bool sdram_self_test(void)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)LCD_SDRAM_TEST_ADDR;
    static const uint32_t pattern[4] = {
        0x55AA55AAUL, 0xAA55AA55UL, 0x01234567UL, 0x89ABCDEFUL
    };

    for (uint32_t i = 0U; i < 4U; ++i) p[i] = pattern[i];
    __DSB();
    for (uint32_t i = 0U; i < 4U; ++i) {
        if (p[i] != pattern[i]) return false;
    }
    for (uint32_t i = 0U; i < 4U; ++i) p[i] = 0U;
    __DSB();
    return true;
}

static void lcd_fill_buffer(uint32_t addr, uint16_t rgb565)
{
    volatile uint16_t *fb = (volatile uint16_t *)(uintptr_t)addr;
    const uint32_t pixels = GW_LCD_HOR_RES * GW_LCD_VER_RES;
    for (uint32_t i = 0U; i < pixels; ++i) fb[i] = rgb565;
    __DSB();
}

static bool tli_init_800x480(void)
{
    tli_parameter_struct tli_cfg;
    tli_layer_parameter_struct layer;

    rcu_periph_clock_enable(RCU_TLI);

    /* PLL2R = 25MHz / 25 * 300 / 3 = 100MHz.
     * DIV4 gives 25MHz pixel clock, about 54.3Hz with the 887x519 timing.
     * The old DIV8 setting yielded only ~27.15Hz and was visibly flickery. */
    rcu_pll_input_output_clock_range_config(IDX_PLL2,
                                            RCU_PLL2RNG_1M_2M,
                                            RCU_PLL2VCO_150M_420M);
    if (ERROR == rcu_pll2_config(25U, 300U, 3U, 3U, 3U)) return false;
    rcu_pll_clock_output_enable(RCU_PLL2R);
    rcu_tli_clock_div_config(RCU_PLL2R_DIV4);
    rcu_osci_on(RCU_PLL2_CK);
    if (ERROR == rcu_osci_stab_wait(RCU_PLL2_CK)) return false;

    memset(&tli_cfg, 0, sizeof(tli_cfg));
    tli_cfg.signalpolarity_hs = TLI_HSYN_ACTLIVE_LOW;
    tli_cfg.signalpolarity_vs = TLI_VSYN_ACTLIVE_LOW;
    tli_cfg.signalpolarity_de = TLI_DE_ACTLIVE_LOW;
    tli_cfg.signalpolarity_pixelck = TLI_PIXEL_CLOCK_TLI;
    tli_cfg.synpsz_hpsz = LCD_HSW - 1U;
    tli_cfg.synpsz_vpsz = LCD_VSW - 1U;
    tli_cfg.backpsz_hbpsz = LCD_HSW + LCD_HBP - 1U;
    tli_cfg.backpsz_vbpsz = LCD_VSW + LCD_VBP - 1U;
    tli_cfg.activesz_hasz = LCD_HSW + LCD_HBP + GW_LCD_HOR_RES - 1U;
    tli_cfg.activesz_vasz = LCD_VSW + LCD_VBP + GW_LCD_VER_RES - 1U;
    tli_cfg.totalsz_htsz = LCD_HSW + LCD_HBP + GW_LCD_HOR_RES + LCD_HFP - 1U;
    tli_cfg.totalsz_vtsz = LCD_VSW + LCD_VBP + GW_LCD_VER_RES + LCD_VFP - 1U;
    tli_cfg.backcolor_red = 0x08U;
    tli_cfg.backcolor_green = 0x0CU;
    tli_cfg.backcolor_blue = 0x16U;
    tli_init(&tli_cfg);

    memset(&layer, 0, sizeof(layer));
    layer.layer_window_leftpos = LCD_HSW + LCD_HBP;
    layer.layer_window_rightpos = LCD_HSW + LCD_HBP + GW_LCD_HOR_RES - 1U;
    layer.layer_window_toppos = LCD_VSW + LCD_VBP;
    layer.layer_window_bottompos = LCD_VSW + LCD_VBP + GW_LCD_VER_RES - 1U;
    layer.layer_ppf = LAYER_PPF_RGB565;
    layer.layer_sa = 0xFFU;
    layer.layer_default_alpha = 0U;
    layer.layer_acf1 = LAYER_ACF1_SA;
    layer.layer_acf2 = LAYER_ACF1_SA;

    /* Start TLI on FB1 while LVGL initially renders into FB0. */
    layer.layer_frame_bufaddr = GW_LCD_FB1_ADDR;
    layer.layer_frame_line_length = (GW_LCD_HOR_RES * GW_LCD_BYTES_PER_PIXEL) + 3U;
    layer.layer_frame_buf_stride_offset = GW_LCD_HOR_RES * GW_LCD_BYTES_PER_PIXEL;
    layer.layer_frame_total_line_number = GW_LCD_VER_RES;
    tli_layer_init(LAYER0, &layer);
    tli_dither_config(TLI_DITHER_DISABLE);
    tli_layer_enable(LAYER0);
    tli_reload_config(TLI_REQUEST_RELOAD_EN);
    tli_enable();

    s_active_fb = GW_LCD_FB1_ADDR;
    s_stats.active_framebuffer = s_active_fb;
    return true;
}


static bool ipa_copy_rgb565(uint32_t source_address, uint16_t source_stride_pixels,
                            uint32_t destination_address, uint16_t destination_stride_pixels,
                            uint16_t width, uint16_t height, bool sync_copy)
{
    if ((source_address == 0U) || (destination_address == 0U) ||
        (width == 0U) || (height == 0U) ||
        (source_stride_pixels < width) || (destination_stride_pixels < width)) {
        ++s_stats.ipa_error_count;
        return false;
    }

    ipa_foreground_parameter_struct fg;
    ipa_destination_parameter_struct dst;
    ipa_foreground_struct_para_init(&fg);
    ipa_destination_struct_para_init(&dst);

    /* RGB565 -> RGB565 memory copy. Line offsets are expressed in pixels. */
    ipa_pixel_format_convert_mode_set(IPA_FGTODE);

    fg.foreground_memaddr = source_address;
    fg.foreground_lineoff = (uint32_t)(source_stride_pixels - width);
    fg.foreground_pf = FOREGROUND_PPF_RGB565;
    fg.foreground_alpha_algorithm = IPA_FG_ALPHA_MODE_0;
    ipa_foreground_init(&fg);

    dst.destination_memaddr = destination_address;
    dst.destination_lineoff = (uint32_t)(destination_stride_pixels - width);
    dst.destination_pf = IPA_DPF_RGB565;
    dst.image_width = width;
    dst.image_height = height;
    dst.image_scaling_width = width;
    dst.image_scaling_height = height;
    dst.image_rotate = DESTINATION_ROTATE_0;
    dst.image_hor_decimation = DESTINATION_HORDECIMATE_DISABLE;
    dst.image_ver_decimation = DESTINATION_VERDECIMATE_DISABLE;
    ipa_destination_init(&dst);

    ipa_flag_clear(IPA_FLAG_FTF | IPA_FLAG_TAE | IPA_FLAG_WCF);
    ipa_transfer_stop_disable();
    ipa_transfer_enable();

    uint64_t start = gw_time_ms();
    for (;;) {
        if (SET == ipa_flag_get(IPA_FLAG_FTF)) {
            ipa_flag_clear(IPA_FLAG_FTF);
            if (sync_copy) ++s_stats.ipa_sync_count;
            else ++s_stats.ipa_blit_count;
            s_stats.ipa_bytes += (uint32_t)width * (uint32_t)height * GW_LCD_BYTES_PER_PIXEL;
            return true;
        }
        if ((SET == ipa_flag_get(IPA_FLAG_TAE)) || (SET == ipa_flag_get(IPA_FLAG_WCF))) {
            ipa_flag_clear(IPA_FLAG_TAE | IPA_FLAG_WCF);
            ipa_transfer_stop_enable();
            ++s_stats.ipa_error_count;
            ++s_stats.flush_error_count;
            return false;
        }
        if ((gw_time_ms() - start) >= LCD_IPA_TIMEOUT_MS) {
            ipa_transfer_stop_enable();
            ++s_stats.ipa_timeout_count;
            ++s_stats.flush_error_count;
            return false;
        }
    }
}

static void collect_tli_errors(void)
{
    uint32_t flags = TLI_INTF;
    if ((flags & TLI_INTF_FEF) != 0U) {
        ++s_stats.tli_fifo_error_count;
        ++s_stats.flush_error_count;
        tli_interrupt_flag_clear(TLI_INT_FLAG_FE);
    }
    if ((flags & TLI_INTF_TEF) != 0U) {
        ++s_stats.tli_transfer_error_count;
        ++s_stats.flush_error_count;
        tli_interrupt_flag_clear(TLI_INT_FLAG_TE);
    }
}

bool gw_lcd_init(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    s_active_fb = 0U;

    GW_LOGI("LCD", "initializing EXMC SDRAM");
    exmc_synchronous_dynamic_ram_init(EXMC_SDRAM_DEVICE0);
    if (!sdram_self_test()) {
        GW_LOGE("LCD", "SDRAM self-test failed");
        return false;
    }
    GW_LOGI("LCD", "SDRAM self-test PASS");

    lcd_gpio_init();
    lcd_fill_buffer(GW_LCD_FB0_ADDR, LCD_BOOT_RGB565);
    lcd_fill_buffer(GW_LCD_FB1_ADDR, LCD_BOOT_RGB565);
    memset((void *)(uintptr_t)GW_LVGL_HEAP_ADDR, 0, GW_LVGL_HEAP_BYTES);

    if (!tli_init_800x480()) {
        GW_LOGE("LCD", "TLI 800x480 initialization failed");
        return false;
    }

    /* IPA is the pixel transport engine. LVGL renders in fast internal SRAM;
     * IPA moves only invalid RGB565 rectangles into the off-screen SDRAM FB. */
    rcu_periph_clock_enable(RCU_IPA);
    ipa_deinit();
    ipa_pixel_format_convert_mode_set(IPA_FGTODE);

    gw_lcd_backlight(true);
    GW_LOGI("LCD", "TLI ready 800x480 RGB565; PCLK=25.0MHz refresh=54.3Hz; IPA partial-blit + VSYNC double-FB; backlight ON");
    return true;
}

bool gw_lcd_present(const void *framebuffer_rgb565)
{
    uint32_t next = (uint32_t)(uintptr_t)framebuffer_rgb565;
    if ((next != GW_LCD_FB0_ADDR) && (next != GW_LCD_FB1_ADDR)) return false;

    collect_tli_errors();
    if (next == s_active_fb) {
        ++s_stats.flush_count;
        return true;
    }

    /* Program the shadow framebuffer address and apply it only at frame blank.
     * Waiting for FBR to clear guarantees LVGL will not start drawing into the
     * old scan-out buffer before TLI has actually switched away from it. */
    TLI_LXFBADDR(LAYER0) = next;
    tli_reload_config(TLI_FRAME_BLANK_RELOAD_EN);

    uint64_t start = gw_time_ms();
    while ((TLI_RL & TLI_RL_FBR) != 0U) {
        collect_tli_errors();
        if ((gw_time_ms() - start) >= LCD_PAGEFLIP_TIMEOUT_MS) {
            ++s_stats.flush_timeout_count;
            return false;
        }
    }
    __DSB();
    s_active_fb = next;
    s_stats.active_framebuffer = next;
    ++s_stats.flush_count;
    return true;
}

uint32_t gw_lcd_active_framebuffer(void)
{
    return s_active_fb;
}

uint32_t gw_lcd_inactive_framebuffer(void)
{
    return (s_active_fb == GW_LCD_FB0_ADDR) ? GW_LCD_FB1_ADDR : GW_LCD_FB0_ADDR;
}

bool gw_lcd_blit_rgb565(uint32_t destination_framebuffer,
                        uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                        const void *source_rgb565, uint16_t source_stride_pixels)
{
    if ((source_rgb565 == NULL) ||
        ((destination_framebuffer != GW_LCD_FB0_ADDR) &&
         (destination_framebuffer != GW_LCD_FB1_ADDR)) ||
        ((uint32_t)x + width > GW_LCD_HOR_RES) ||
        ((uint32_t)y + height > GW_LCD_VER_RES)) {
        ++s_stats.ipa_error_count;
        return false;
    }

    uint32_t destination = destination_framebuffer +
        ((((uint32_t)y * GW_LCD_HOR_RES) + x) * GW_LCD_BYTES_PER_PIXEL);
    return ipa_copy_rgb565((uint32_t)(uintptr_t)source_rgb565, source_stride_pixels,
                           destination, GW_LCD_HOR_RES, width, height, false);
}

bool gw_lcd_copy_rect(uint32_t source_framebuffer, uint32_t destination_framebuffer,
                      uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
    if (((source_framebuffer != GW_LCD_FB0_ADDR) && (source_framebuffer != GW_LCD_FB1_ADDR)) ||
        ((destination_framebuffer != GW_LCD_FB0_ADDR) && (destination_framebuffer != GW_LCD_FB1_ADDR)) ||
        ((uint32_t)x + width > GW_LCD_HOR_RES) ||
        ((uint32_t)y + height > GW_LCD_VER_RES)) {
        ++s_stats.ipa_error_count;
        return false;
    }
    uint32_t offset = ((((uint32_t)y * GW_LCD_HOR_RES) + x) * GW_LCD_BYTES_PER_PIXEL);
    return ipa_copy_rgb565(source_framebuffer + offset, GW_LCD_HOR_RES,
                           destination_framebuffer + offset, GW_LCD_HOR_RES,
                           width, height, true);
}

bool gw_lcd_copy_framebuffer(uint32_t source_framebuffer, uint32_t destination_framebuffer)
{
    bool ok = gw_lcd_copy_rect(source_framebuffer, destination_framebuffer, 0U, 0U,
                               GW_LCD_HOR_RES, GW_LCD_VER_RES);
    if (ok) ++s_stats.full_resync_count;
    return ok;
}

void gw_lcd_backlight(bool on)
{
    if (on) gpio_bit_set(GPIOG, GPIO_PIN_3);
    else gpio_bit_reset(GPIOG, GPIO_PIN_3);
}

void gw_lcd_get_stats(gw_lcd_stats_t *out)
{
    if (out != NULL) {
        collect_tli_errors();
        *out = s_stats;
    }
}
