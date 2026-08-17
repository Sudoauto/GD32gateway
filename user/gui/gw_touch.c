#include "gw_touch.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "gd32h7xx.h"
#include "gd32h7xx_i2c.h"
#include "gateway_build_config.h"
#include "gw_lcd.h"
#include "gw_time.h"

#define TP_I2C                 I2C2
#define TP_I2C_RCU             RCU_I2C2
#define TP_SCL_PORT            GPIOH
#define TP_SDA_PORT            GPIOH
#define TP_SCL_PIN             GPIO_PIN_7
#define TP_SDA_PIN             GPIO_PIN_8
#define TP_I2C_AF              GPIO_AF_4
#define TP_RST_PORT            GPIOB
#define TP_RST_PIN             GPIO_PIN_1

#define GT_REG_PRODUCT_ID      0x8140U
#define GT_REG_STATUS          0x814EU
#define GT_REG_POINTS          0x814FU
#define GT_STATUS_READY        0x80U
#define GT_STATUS_POINTS_MASK  0x0FU
#define GT_MAX_POINTS          10U

/* The supplied board example probes these two 8-bit I2C address forms. */
static const uint8_t s_addresses[] = {0x28U, 0xBAU};
static uint8_t s_address;
static uint8_t s_max_points;
static char s_product_id[5];
static gw_touch_stats_t s_stats;
static uint64_t s_next_reprobe_ms;

typedef struct {
    uint8_t track_id;
    uint8_t x_l;
    uint8_t x_h;
    uint8_t y_l;
    uint8_t y_h;
    uint8_t size_l;
    uint8_t size_h;
    uint8_t reserved;
} gt_point_reg_t;

static bool i2c_error_pending(void)
{
    return (SET == i2c_flag_get(TP_I2C, I2C_FLAG_NACK)) ||
           (SET == i2c_flag_get(TP_I2C, I2C_FLAG_BERR)) ||
           (SET == i2c_flag_get(TP_I2C, I2C_FLAG_LOSTARB)) ||
           (SET == i2c_flag_get(TP_I2C, I2C_FLAG_OUERR)) ||
           (SET == i2c_flag_get(TP_I2C, I2C_FLAG_PECERR)) ||
           (SET == i2c_flag_get(TP_I2C, I2C_FLAG_TIMEOUT));
}

static void clear_i2c_status(void)
{
    if (SET == i2c_flag_get(TP_I2C, I2C_FLAG_NACK)) i2c_flag_clear(TP_I2C, I2C_FLAG_NACK);
    if (SET == i2c_flag_get(TP_I2C, I2C_FLAG_STPDET)) i2c_flag_clear(TP_I2C, I2C_FLAG_STPDET);
    if (SET == i2c_flag_get(TP_I2C, I2C_FLAG_BERR)) i2c_flag_clear(TP_I2C, I2C_FLAG_BERR);
    if (SET == i2c_flag_get(TP_I2C, I2C_FLAG_LOSTARB)) i2c_flag_clear(TP_I2C, I2C_FLAG_LOSTARB);
    if (SET == i2c_flag_get(TP_I2C, I2C_FLAG_OUERR)) i2c_flag_clear(TP_I2C, I2C_FLAG_OUERR);
    if (SET == i2c_flag_get(TP_I2C, I2C_FLAG_PECERR)) i2c_flag_clear(TP_I2C, I2C_FLAG_PECERR);
    if (SET == i2c_flag_get(TP_I2C, I2C_FLAG_TIMEOUT)) i2c_flag_clear(TP_I2C, I2C_FLAG_TIMEOUT);
}

static bool wait_flag_set(uint32_t flag)
{
    uint64_t start = gw_time_ms();
    for (;;) {
        if (SET == i2c_flag_get(TP_I2C, flag)) return true;
        if (i2c_error_pending()) {
            ++s_stats.bus_error_count;
            return false;
        }
        if ((gw_time_ms() - start) >= GW_TOUCH_IO_TIMEOUT_MS) {
            ++s_stats.io_timeout_count;
            return false;
        }
        taskYIELD();
    }
}

static bool wait_bus_idle(void)
{
    uint64_t start = gw_time_ms();
    while (SET == i2c_flag_get(TP_I2C, I2C_FLAG_I2CBSY)) {
        if ((gw_time_ms() - start) >= GW_TOUCH_IO_TIMEOUT_MS) {
            ++s_stats.io_timeout_count;
            return false;
        }
        taskYIELD();
    }
    return true;
}

static void i2c_hw_init(void)
{
    rcu_periph_clock_enable(RCU_GPIOH);
    rcu_periph_clock_enable(TP_I2C_RCU);

    gpio_af_set(TP_SCL_PORT, TP_I2C_AF, TP_SCL_PIN);
    gpio_af_set(TP_SDA_PORT, TP_I2C_AF, TP_SDA_PIN);
    gpio_mode_set(TP_SCL_PORT, GPIO_MODE_AF, GPIO_PUPD_PULLUP, TP_SCL_PIN);
    gpio_output_options_set(TP_SCL_PORT, GPIO_OTYPE_OD, GPIO_OSPEED_60MHZ, TP_SCL_PIN);
    gpio_mode_set(TP_SDA_PORT, GPIO_MODE_AF, GPIO_PUPD_PULLUP, TP_SDA_PIN);
    gpio_output_options_set(TP_SDA_PORT, GPIO_OTYPE_OD, GPIO_OSPEED_60MHZ, TP_SDA_PIN);

    rcu_i2c_clock_config(IDX_I2C2, RCU_I2CSRC_IRC64MDIV);
    i2c_disable(TP_I2C);
    i2c_timing_config(TP_I2C, 0x0U, 0x6U, 0U);
    i2c_master_clock_config(TP_I2C, 0x26U, 0x73U);
    clear_i2c_status();
    i2c_enable(TP_I2C);
}

static void i2c_recover(void)
{
    ++s_stats.recovery_count;
    i2c_stop_on_bus(TP_I2C);
    clear_i2c_status();
    i2c_disable(TP_I2C);
    i2c_deinit(TP_I2C);

    gpio_mode_set(TP_SCL_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLUP, TP_SCL_PIN);
    gpio_output_options_set(TP_SCL_PORT, GPIO_OTYPE_OD, GPIO_OSPEED_60MHZ, TP_SCL_PIN);
    gpio_mode_set(TP_SDA_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLUP, TP_SDA_PIN);
    gpio_output_options_set(TP_SDA_PORT, GPIO_OTYPE_OD, GPIO_OSPEED_60MHZ, TP_SDA_PIN);

    gpio_bit_set(TP_SDA_PORT, TP_SDA_PIN);
    for (uint32_t pulse = 0U; pulse < 9U; ++pulse) {
        gpio_bit_set(TP_SCL_PORT, TP_SCL_PIN);
        for (volatile uint32_t d = 0U; d < 200U; ++d) { }
        gpio_bit_reset(TP_SCL_PORT, TP_SCL_PIN);
        for (volatile uint32_t d = 0U; d < 200U; ++d) { }
    }
    /* Generate a passive STOP-like release: SCL high, then SDA high. */
    gpio_bit_reset(TP_SDA_PORT, TP_SDA_PIN);
    gpio_bit_set(TP_SCL_PORT, TP_SCL_PIN);
    for (volatile uint32_t d = 0U; d < 200U; ++d) { }
    gpio_bit_set(TP_SDA_PORT, TP_SDA_PIN);
    i2c_hw_init();
}

static bool transaction_failed(void)
{
    ++s_stats.read_error_count;
    ++s_stats.consecutive_error_count;
    i2c_recover();
    return false;
}

static bool reg_write(uint8_t addr, uint16_t reg, const uint8_t *data, uint8_t len)
{
    if ((data == NULL) || (len == 0U)) return false;
    clear_i2c_status();
    if (!wait_bus_idle()) {
        i2c_recover();
        if (!wait_bus_idle()) return transaction_failed();
    }

    i2c_master_addressing(TP_I2C, addr, I2C_MASTER_TRANSMIT);
    i2c_transfer_byte_number_config(TP_I2C, (uint32_t)len + 2U);
    i2c_automatic_end_enable(TP_I2C);
    i2c_start_on_bus(TP_I2C);

    if (!wait_flag_set(I2C_FLAG_TBE)) return transaction_failed();
    i2c_data_transmit(TP_I2C, (uint8_t)(reg >> 8));
    if (!wait_flag_set(I2C_FLAG_TBE)) return transaction_failed();
    i2c_data_transmit(TP_I2C, (uint8_t)(reg & 0xFFU));
    for (uint8_t i = 0U; i < len; ++i) {
        if (!wait_flag_set(I2C_FLAG_TI)) return transaction_failed();
        i2c_data_transmit(TP_I2C, data[i]);
    }
    if (!wait_flag_set(I2C_FLAG_STPDET)) return transaction_failed();
    i2c_flag_clear(TP_I2C, I2C_FLAG_STPDET);
    s_stats.consecutive_error_count = 0U;
    return true;
}

static bool reg_read(uint8_t addr, uint16_t reg, uint8_t *data, uint8_t len)
{
    if ((data == NULL) || (len == 0U)) return false;
    clear_i2c_status();
    if (!wait_bus_idle()) {
        i2c_recover();
        if (!wait_bus_idle()) return transaction_failed();
    }

    i2c_master_addressing(TP_I2C, addr, I2C_MASTER_TRANSMIT);
    i2c_transfer_byte_number_config(TP_I2C, 2U);
    i2c_automatic_end_disable(TP_I2C);
    i2c_start_on_bus(TP_I2C);
    if (!wait_flag_set(I2C_FLAG_TBE)) return transaction_failed();
    i2c_data_transmit(TP_I2C, (uint8_t)(reg >> 8));
    if (!wait_flag_set(I2C_FLAG_TBE)) return transaction_failed();
    i2c_data_transmit(TP_I2C, (uint8_t)(reg & 0xFFU));
    if (!wait_flag_set(I2C_FLAG_TC)) return transaction_failed();

    i2c_master_addressing(TP_I2C, addr, I2C_MASTER_RECEIVE);
    i2c_transfer_byte_number_config(TP_I2C, len);
    i2c_automatic_end_enable(TP_I2C);
    i2c_start_on_bus(TP_I2C);
    for (uint8_t i = 0U; i < len; ++i) {
        if (!wait_flag_set(I2C_FLAG_RBNE)) return transaction_failed();
        data[i] = i2c_data_receive(TP_I2C);
    }
    if (!wait_flag_set(I2C_FLAG_STPDET)) return transaction_failed();
    i2c_flag_clear(TP_I2C, I2C_FLAG_STPDET);
    s_stats.consecutive_error_count = 0U;
    return true;
}

static void touch_reset(void)
{
    rcu_periph_clock_enable(RCU_GPIOB);
    gpio_mode_set(TP_RST_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLDOWN, TP_RST_PIN);
    gpio_output_options_set(TP_RST_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, TP_RST_PIN);
    gpio_bit_reset(TP_RST_PORT, TP_RST_PIN);
    vTaskDelay(pdMS_TO_TICKS(2U));
    gpio_bit_set(TP_RST_PORT, TP_RST_PIN);
    vTaskDelay(pdMS_TO_TICKS(10U));
}

static bool probe_controller(void)
{
    s_address = 0U;
    memset(s_product_id, 0, sizeof(s_product_id));
    for (uint32_t i = 0U; i < (sizeof(s_addresses) / sizeof(s_addresses[0])); ++i) {
        uint8_t id[4] = {0U};
        if (reg_read(s_addresses[i], GT_REG_PRODUCT_ID, id, sizeof(id))) {
            memcpy(s_product_id, id, sizeof(id));
            s_product_id[4] = '\0';
            s_address = s_addresses[i];
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2U));
    }
    if (s_address == 0U) return false;
    s_max_points = (strstr(s_product_id, "115") != NULL) ? 10U : 5U;
    s_stats.initialized = true;
    s_stats.consecutive_error_count = 0U;
    return true;
}

static void maybe_reprobe(void)
{
    uint64_t now = gw_time_ms();
    if ((s_stats.consecutive_error_count < GW_TOUCH_REPROBE_THRESHOLD) ||
        (now < s_next_reprobe_ms)) return;
    ++s_stats.reprobe_count;
    s_next_reprobe_ms = now + GW_TOUCH_REPROBE_BACKOFF_MS;
    s_stats.initialized = false;
    s_address = 0U;
    touch_reset();
    i2c_recover();
    (void)probe_controller();
}

bool gw_touch_init(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_product_id, 0, sizeof(s_product_id));
    s_address = 0U;
    s_max_points = 0U;
    s_next_reprobe_ms = 0U;
    i2c_hw_init();
    touch_reset();
    if (!probe_controller()) {
        /* Keep the LVGL input device alive even if the controller is absent at
         * boot. Mark the error threshold reached so periodic reads will retry
         * probe after the backoff instead of permanently disabling touch. */
        s_stats.consecutive_error_count = GW_TOUCH_REPROBE_THRESHOLD;
        s_next_reprobe_ms = gw_time_ms() + GW_TOUCH_REPROBE_BACKOFF_MS;
        return false;
    }
    return true;
}

bool gw_touch_read(uint16_t *x, uint16_t *y, bool *pressed)
{
    if ((x == NULL) || (y == NULL) || (pressed == NULL)) return false;
    *pressed = false;
    if ((s_address == 0U) || !s_stats.initialized) {
        maybe_reprobe();
        return false;
    }

    ++s_stats.read_count;
    uint8_t status = 0U;
    if (!reg_read(s_address, GT_REG_STATUS, &status, 1U)) {
        maybe_reprobe();
        return false;
    }
    if ((status & GT_STATUS_READY) == 0U) return true;

    uint8_t points = status & GT_STATUS_POINTS_MASK;
    if ((points == 0U) || (points > GT_MAX_POINTS) ||
        ((s_max_points != 0U) && (points > s_max_points))) {
        uint8_t clear = 0U;
        (void)reg_write(s_address, GT_REG_STATUS, &clear, 1U);
        return true;
    }

    gt_point_reg_t point;
    if (!reg_read(s_address, GT_REG_POINTS, (uint8_t *)&point, sizeof(point))) {
        maybe_reprobe();
        return false;
    }
    uint8_t clear = 0U;
    if (!reg_write(s_address, GT_REG_STATUS, &clear, 1U)) {
        maybe_reprobe();
        return false;
    }

    uint16_t tx = (uint16_t)(((uint16_t)point.x_h << 8U) | point.x_l);
    uint16_t ty = (uint16_t)(((uint16_t)point.y_h << 8U) | point.y_l);
    if (tx >= GW_LCD_HOR_RES) tx = GW_LCD_HOR_RES - 1U;
    if (ty >= GW_LCD_VER_RES) ty = GW_LCD_VER_RES - 1U;
    *x = tx;
    *y = ty;
    *pressed = true;
    return true;
}

bool gw_touch_available(void) { return s_stats.initialized && (s_address != 0U); }
const char *gw_touch_product_id(void) { return s_product_id; }
uint8_t gw_touch_max_points(void) { return s_max_points; }
void gw_touch_get_stats(gw_touch_stats_t *out) { if (out != NULL) *out = s_stats; }
