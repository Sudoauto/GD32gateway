#include "gw_gui.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"
#include "gateway_build_config.h"
#include "gw_lv_port.h"
#include "gw_lcd.h"
#include "gw_touch.h"
#include "gw_log.h"
#include "gw_time.h"
#include "point_db.h"
#include "device_manager.h"
#include "drv_canfd.h"
#include "drv_rs485.h"
#include "gw_net_manager.h"
#include "gw_ethernetif.h"
#include "gw_tcp_server.h"
#include "gw_uplink.h"
#include "gw_command_router.h"
#include "gw_watchdog.h"
#include "rtos_objects.h"
#include "gw_diagnostics.h"
#include "gw_security.h"
#include "gw_config.h"
#include "gw_flash_store.h"

#if (GW_GUI_ENABLE != 0U)

#define GUI_PAGE_COUNT          6U
#define GUI_DEVICE_ROWS         7U
#define GUI_POINT_ROWS          4U
#define GUI_TRAFFIC_ROWS        3U
#define GUI_DIAG_SAMPLES        12U
#define GUI_HEADER_H            58
#define GUI_SIDEBAR_W           126
#define GUI_CONTENT_X           GUI_SIDEBAR_W
#define GUI_CONTENT_Y           GUI_HEADER_H
#define GUI_CONTENT_W           (GW_LCD_HOR_RES - GUI_SIDEBAR_W)
#define GUI_CONTENT_H           (GW_LCD_VER_RES - GUI_HEADER_H)

#define C_BG                    0x101316U
#define C_SIDEBAR               0x15191DU
#define C_PANEL                 0x1A1F24U
#define C_PANEL_ALT             0x20262CU
#define C_BORDER                0x303840U
#define C_TEXT                  0xE7ECEFU
#define C_MUTED                 0x8E9AA5U
#define C_ACCENT                0x2E9F8FU
#define C_ACCENT_SOFT           0x173C39U
#define C_CYAN                  0x46B7C8U
#define C_SUCCESS               0x3FAE73U
#define C_WARNING               0xD6A54AU
#define C_ERROR                 0xD96464U
#define C_WHITE                 0xF7F9FAU

static StaticTask_t s_gui_task_cb;
static StackType_t s_gui_stack[GW_GUI_TASK_STACK_WORDS];
static gw_gui_stats_t s_stats;

static lv_obj_t *s_pages[GUI_PAGE_COUNT];
static lv_obj_t *s_nav_buttons[GUI_PAGE_COUNT];
static uint8_t s_nav_index[GUI_PAGE_COUNT] = {0U, 1U, 2U, 3U, 4U, 5U};
static uint8_t s_active_page;
static lv_obj_t *s_clock_label;
static lv_obj_t *s_header_state;

static lv_obj_t *s_metric_devices;
static lv_obj_t *s_metric_points;
static lv_obj_t *s_metric_can;
static lv_obj_t *s_metric_net;
static lv_obj_t *s_health_label;
static lv_obj_t *s_traffic_label;

static lv_obj_t *s_device_rows[GUI_DEVICE_ROWS];
static lv_obj_t *s_device_cells[GUI_DEVICE_ROWS][4];
static lv_obj_t *s_point_rows[GUI_POINT_ROWS];
static lv_obj_t *s_point_cells[GUI_POINT_ROWS][4];
static lv_obj_t *s_traffic_rows[GUI_TRAFFIC_ROWS];
static lv_obj_t *s_traffic_cells[GUI_TRAFFIC_ROWS][5];

static lv_obj_t *s_network_summary;
static lv_obj_t *s_network_counters;
static lv_obj_t *s_uplink_summary;

static lv_obj_t *s_diag_summary;
static lv_obj_t *s_diag_bars[4][GUI_DIAG_SAMPLES];
static lv_obj_t *s_diag_trend_value[4];
static lv_obj_t *s_diag_selftest;
static lv_obj_t *s_diag_touch;
static lv_obj_t *s_lock_overlay;
static lv_obj_t *s_login_user;
static lv_obj_t *s_login_password;
static lv_obj_t *s_login_status;
static uint64_t s_factory_arm_until_ms;

static lv_obj_t *s_can_id_input;
static lv_obj_t *s_can_data_input;
static lv_obj_t *s_modbus_dev_input;
static lv_obj_t *s_modbus_reg_input;
static lv_obj_t *s_modbus_value_input;
static lv_obj_t *s_modbus_qty_input;
static lv_obj_t *s_command_status;
static lv_obj_t *s_keyboard;

static lv_style_t s_panel_style;
static lv_style_t s_panel_alt_style;
static lv_style_t s_nav_button_style;
static lv_style_t s_nav_checked_style;
static lv_style_t s_input_style;
static lv_style_t s_primary_button_style;
static lv_style_t s_secondary_button_style;

static void gui_label_set_text(lv_obj_t *label, const char *text)
{
    if ((label == NULL) || (text == NULL)) return;
    const char *current = lv_label_get_text(label);
    if ((current == NULL) || (strcmp(current, text) != 0)) {
        lv_label_set_text(label, text);
    }
}

static void gui_label_set_text_fmt(lv_obj_t *label, const char *fmt, ...)
{
    if ((label == NULL) || (fmt == NULL)) return;
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    text[sizeof(text) - 1U] = '\0';
    gui_label_set_text(label, text);
}

static const char *device_state_text(device_state_t state)
{
    switch (state) {
    case DEVICE_DISABLED: return "DISABLED";
    case DEVICE_INIT: return "INIT";
    case DEVICE_ONLINE: return "ONLINE";
    case DEVICE_OFFLINE: return "OFFLINE";
    case DEVICE_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}

static const char *protocol_text(gw_protocol_t protocol)
{
    switch (protocol) {
    case GW_PROTO_MODBUS_RTU: return "MODBUS RTU";
    case GW_PROTO_MODBUS_TCP: return "MODBUS TCP";
    case GW_PROTO_CAN: return "CAN-FD";
    case GW_PROTO_RS485_RAW: return "RS485 RAW";
    default: return "-";
    }
}

static const char *interface_text(gw_interface_id_t interface_id)
{
    switch (interface_id) {
    case GW_IF_RS485_0: return "RS485";
    case GW_IF_CANFD_0: return "CAN";
    case GW_IF_ETH_0: return "ETH";
    default: return "-";
    }
}

static const char *quality_text(gw_quality_t quality)
{
    switch (quality) {
    case GW_QUALITY_GOOD: return "GOOD";
    case GW_QUALITY_STALE: return "STALE";
    case GW_QUALITY_TIMEOUT: return "TIMEOUT";
    case GW_QUALITY_BAD: return "BAD";
    case GW_QUALITY_OFFLINE: return "OFFLINE";
    case GW_QUALITY_INVALID: return "INVALID";
    default: return "?";
    }
}

static lv_color_t state_color(device_state_t state)
{
    if (state == DEVICE_ONLINE) return lv_color_hex(C_SUCCESS);
    if ((state == DEVICE_ERROR) || (state == DEVICE_OFFLINE)) return lv_color_hex(C_ERROR);
    if (state == DEVICE_INIT) return lv_color_hex(C_WARNING);
    return lv_color_hex(C_MUTED);
}

static lv_color_t quality_color(gw_quality_t quality)
{
    if (quality == GW_QUALITY_GOOD) return lv_color_hex(C_SUCCESS);
    if ((quality == GW_QUALITY_BAD) || (quality == GW_QUALITY_OFFLINE) ||
        (quality == GW_QUALITY_INVALID)) return lv_color_hex(C_ERROR);
    return lv_color_hex(C_WARNING);
}

static void point_value_text(const gw_point_t *p, char *buf, size_t size)
{
    if ((p == NULL) || (buf == NULL) || (size == 0U)) return;
    switch (p->type) {
    case GW_VALUE_BOOL:
        (void)snprintf(buf, size, "%s", p->value.b ? "ON" : "OFF");
        break;
    case GW_VALUE_U16:
        (void)snprintf(buf, size, "%u", (unsigned)p->value.u16);
        break;
    case GW_VALUE_I16:
        (void)snprintf(buf, size, "%d", (int)p->value.i16);
        break;
    case GW_VALUE_U32:
        (void)snprintf(buf, size, "%lu", (unsigned long)p->value.u32);
        break;
    case GW_VALUE_I32:
        (void)snprintf(buf, size, "%ld", (long)p->value.i32);
        break;
    case GW_VALUE_F32: {
        float v = p->value.f32;
        int32_t scaled = (int32_t)(v * 100.0f);
        int32_t whole = scaled / 100;
        int32_t frac = scaled % 100;
        if (frac < 0) frac = -frac;
        (void)snprintf(buf, size, "%ld.%02ld", (long)whole, (long)frac);
        break;
    }
    case GW_VALUE_F64: {
        double v = p->value.f64;
        if ((v > 2000000000.0) || (v < -2000000000.0)) {
            (void)snprintf(buf, size, "OUT-OF-RANGE");
            break;
        }
        int64_t scaled = (int64_t)(v * 100.0);
        int64_t whole = scaled / 100;
        int64_t frac = scaled % 100;
        if (frac < 0) frac = -frac;
        (void)snprintf(buf, size, "%lld.%02lld", (long long)whole, (long long)frac);
        break;
    }
    default:
        (void)snprintf(buf, size, "-");
        break;
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    gui_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static lv_obj_t *make_panel(lv_obj_t *parent, int32_t x, int32_t y,
                            int32_t w, int32_t h, bool alternate)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_add_style(panel, alternate ? &s_panel_alt_style : &s_panel_style, 0);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
    return panel;
}

static void style_init(void)
{
    lv_style_init(&s_panel_style);
    lv_style_set_bg_color(&s_panel_style, lv_color_hex(C_PANEL));
    lv_style_set_bg_opa(&s_panel_style, LV_OPA_COVER);
    lv_style_set_border_color(&s_panel_style, lv_color_hex(C_BORDER));
    lv_style_set_border_width(&s_panel_style, 1);
    lv_style_set_radius(&s_panel_style, 8);
    lv_style_set_pad_all(&s_panel_style, 14);

    lv_style_init(&s_panel_alt_style);
    lv_style_set_bg_color(&s_panel_alt_style, lv_color_hex(C_PANEL_ALT));
    lv_style_set_bg_opa(&s_panel_alt_style, LV_OPA_COVER);
    lv_style_set_border_color(&s_panel_alt_style, lv_color_hex(C_BORDER));
    lv_style_set_border_width(&s_panel_alt_style, 1);
    lv_style_set_radius(&s_panel_alt_style, 8);
    lv_style_set_pad_all(&s_panel_alt_style, 12);

    lv_style_init(&s_nav_button_style);
    lv_style_set_bg_opa(&s_nav_button_style, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_nav_button_style, 0);
    lv_style_set_radius(&s_nav_button_style, 6);
    lv_style_set_text_color(&s_nav_button_style, lv_color_hex(C_MUTED));
    lv_style_set_pad_left(&s_nav_button_style, 14);

    lv_style_init(&s_nav_checked_style);
    lv_style_set_bg_color(&s_nav_checked_style, lv_color_hex(C_ACCENT_SOFT));
    lv_style_set_bg_opa(&s_nav_checked_style, LV_OPA_COVER);
    lv_style_set_text_color(&s_nav_checked_style, lv_color_hex(C_WHITE));

    lv_style_init(&s_input_style);
    lv_style_set_bg_color(&s_input_style, lv_color_hex(C_BG));
    lv_style_set_bg_opa(&s_input_style, LV_OPA_COVER);
    lv_style_set_border_color(&s_input_style, lv_color_hex(C_BORDER));
    lv_style_set_border_width(&s_input_style, 1);
    lv_style_set_radius(&s_input_style, 6);
    lv_style_set_text_color(&s_input_style, lv_color_hex(C_TEXT));
    lv_style_set_text_font(&s_input_style, &lv_font_montserrat_16);
    lv_style_set_pad_left(&s_input_style, 10);
    lv_style_set_pad_right(&s_input_style, 10);

    lv_style_init(&s_primary_button_style);
    lv_style_set_bg_color(&s_primary_button_style, lv_color_hex(C_ACCENT));
    lv_style_set_bg_opa(&s_primary_button_style, LV_OPA_COVER);
    lv_style_set_border_width(&s_primary_button_style, 0);
    lv_style_set_radius(&s_primary_button_style, 6);
    lv_style_set_text_color(&s_primary_button_style, lv_color_hex(C_WHITE));

    lv_style_init(&s_secondary_button_style);
    lv_style_set_bg_color(&s_secondary_button_style, lv_color_hex(C_PANEL_ALT));
    lv_style_set_bg_opa(&s_secondary_button_style, LV_OPA_COVER);
    lv_style_set_border_color(&s_secondary_button_style, lv_color_hex(C_BORDER));
    lv_style_set_border_width(&s_secondary_button_style, 1);
    lv_style_set_radius(&s_secondary_button_style, 6);
    lv_style_set_text_color(&s_secondary_button_style, lv_color_hex(C_TEXT));
}

static void show_page(uint8_t index)
{
    if (index >= GUI_PAGE_COUNT) return;
    s_active_page = index;
    for (uint8_t i = 0U; i < GUI_PAGE_COUNT; ++i) {
        if (i == index) {
            lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_state(s_nav_buttons[i], LV_STATE_CHECKED);
        } else {
            lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_state(s_nav_buttons[i], LV_STATE_CHECKED);
        }
    }
}

static void nav_event(lv_event_t *e)
{
    uint8_t *index = (uint8_t *)lv_event_get_user_data(e);
    if ((index != NULL) && (lv_event_get_code(e) == LV_EVENT_CLICKED)) {
        gw_security_touch_session();
        show_page(*index);
    }
}

static lv_obj_t *create_nav_button(lv_obj_t *sidebar, uint8_t index,
                                   const char *symbol, const char *text,
                                   int32_t y)
{
    lv_obj_t *btn = lv_button_create(sidebar);
    lv_obj_set_pos(btn, 8, y);
    lv_obj_set_size(btn, GUI_SIDEBAR_W - 16, 52);
    lv_obj_add_style(btn, &s_nav_button_style, 0);
    lv_obj_add_style(btn, &s_nav_checked_style, LV_STATE_CHECKED);
    lv_obj_add_event_cb(btn, nav_event, LV_EVENT_CLICKED, &s_nav_index[index]);

    lv_obj_t *icon = make_label(btn, symbol, &lv_font_montserrat_20, C_MUTED);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *label = make_label(btn, text, &lv_font_montserrat_14, C_TEXT);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 30, 0);
    return btn;
}

static void create_header(lv_obj_t *screen)
{
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, GW_LCD_HOR_RES, GUI_HEADER_H);
    lv_obj_set_style_bg_color(header, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_scrollbar_mode(header, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *brand = make_label(header, "GW-H759", &lv_font_montserrat_20, C_WHITE);
    lv_obj_set_pos(brand, 20, 17);
    lv_obj_t *sub = make_label(header, "INDUSTRIAL EDGE", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(sub, 118, 20);

    s_header_state = make_label(header, "CAN  RS485  NET", &lv_font_montserrat_14, C_SUCCESS);
    lv_obj_align(s_header_state, LV_ALIGN_CENTER, 80, 0);
    s_clock_label = make_label(header, "00:00:00", &lv_font_montserrat_16, C_MUTED);
    lv_obj_align(s_clock_label, LV_ALIGN_RIGHT_MID, -18, 0);

    lv_obj_t *line = lv_obj_create(header);
    lv_obj_set_pos(line, 0, GUI_HEADER_H - 1);
    lv_obj_set_size(line, GW_LCD_HOR_RES, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
}

static void create_sidebar(lv_obj_t *screen)
{
    lv_obj_t *sidebar = lv_obj_create(screen);
    lv_obj_set_pos(sidebar, 0, GUI_HEADER_H);
    lv_obj_set_size(sidebar, GUI_SIDEBAR_W, GUI_CONTENT_H);
    lv_obj_set_style_bg_color(sidebar, lv_color_hex(C_SIDEBAR), 0);
    lv_obj_set_style_bg_opa(sidebar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sidebar, 0, 0);
    lv_obj_set_style_pad_all(sidebar, 0, 0);
    lv_obj_set_scrollbar_mode(sidebar, LV_SCROLLBAR_MODE_OFF);

    s_nav_buttons[0] = create_nav_button(sidebar, 0U, LV_SYMBOL_HOME, "Overview", 8);
    s_nav_buttons[1] = create_nav_button(sidebar, 1U, LV_SYMBOL_LIST, "Devices", 62);
    s_nav_buttons[2] = create_nav_button(sidebar, 2U, LV_SYMBOL_EYE_OPEN, "Data", 116);
    s_nav_buttons[3] = create_nav_button(sidebar, 3U, LV_SYMBOL_EDIT, "Control", 170);
    s_nav_buttons[4] = create_nav_button(sidebar, 4U, LV_SYMBOL_WIFI, "Network", 224);
    s_nav_buttons[5] = create_nav_button(sidebar, 5U, LV_SYMBOL_SETTINGS, "Service", 278);

    lv_obj_t *footer = make_label(sidebar, "v0.9.0\nFD 500K / BRS OFF", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(footer, 14, GUI_CONTENT_H - 58);
}

static lv_obj_t *create_metric(lv_obj_t *page, int32_t x, const char *caption,
                               const char *initial, uint32_t accent)
{
    lv_obj_t *panel = make_panel(page, x, 54, 154, 98, false);
    lv_obj_t *stripe = lv_obj_create(panel);
    lv_obj_set_pos(stripe, -14, -14);
    lv_obj_set_size(stripe, 4, 98);
    lv_obj_set_style_bg_color(stripe, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(stripe, 0, 0);
    make_label(panel, caption, &lv_font_montserrat_14, C_MUTED);
    lv_obj_t *value = make_label(panel, initial, &lv_font_montserrat_24, C_WHITE);
    lv_obj_set_pos(value, 0, 34);
    return value;
}

static void create_overview_page(lv_obj_t *page)
{
    make_label(page, "Gateway overview", &lv_font_montserrat_24, C_WHITE);
    lv_obj_t *hint = make_label(page, "Runtime status and recent southbound traffic",
                                &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(hint, 0, 30);

    s_metric_devices = create_metric(page, 0, "DEVICES", "0", C_ACCENT);
    s_metric_points = create_metric(page, 166, "POINTS", "0", C_SUCCESS);
    s_metric_can = create_metric(page, 332, "CAN-FD", "READY", C_WARNING);
    s_metric_net = create_metric(page, 498, "NETWORK", "DOWN", C_ACCENT);

    lv_obj_t *health = make_panel(page, 0, 166, 322, 222, false);
    make_label(health, "System health", &lv_font_montserrat_16, C_WHITE);
    lv_obj_t *rule = lv_obj_create(health);
    lv_obj_set_pos(rule, 0, 30);
    lv_obj_set_size(rule, 294, 1);
    lv_obj_set_style_bg_color(rule, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    s_health_label = make_label(health, "Collecting diagnostics...", &lv_font_montserrat_16, C_TEXT);
    lv_obj_set_pos(s_health_label, 0, 48);
    lv_obj_set_width(s_health_label, 292);

    lv_obj_t *traffic = make_panel(page, 334, 166, 318, 222, true);
    make_label(traffic, "Recent traffic", &lv_font_montserrat_16, C_WHITE);
    s_traffic_label = make_label(traffic, "No southbound frames yet", &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_pos(s_traffic_label, 0, 40);
    lv_obj_set_width(s_traffic_label, 288);
}

static void create_device_page(lv_obj_t *page)
{
    make_label(page, "Device inventory", &lv_font_montserrat_24, C_WHITE);
    lv_obj_t *sub = make_label(page, "Configured southbound endpoints and runtime state",
                               &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(sub, 0, 30);

    lv_obj_t *header = make_panel(page, 0, 58, 650, 42, true);
    make_label(header, "ID / DEVICE", &lv_font_montserrat_14, C_MUTED);
    lv_obj_t *p = make_label(header, "PROTOCOL", &lv_font_montserrat_14, C_MUTED); lv_obj_set_pos(p, 242, 0);
    p = make_label(header, "STATE", &lv_font_montserrat_14, C_MUTED); lv_obj_set_pos(p, 398, 0);
    p = make_label(header, "OK / ERR", &lv_font_montserrat_14, C_MUTED); lv_obj_set_pos(p, 516, 0);

    for (uint32_t i = 0U; i < GUI_DEVICE_ROWS; ++i) {
        int32_t y = 106 + (int32_t)i * 43;
        s_device_rows[i] = make_panel(page, 0, y, 650, 38, false);
        s_device_cells[i][0] = make_label(s_device_rows[i], "-", &lv_font_montserrat_14, C_TEXT);
        s_device_cells[i][1] = make_label(s_device_rows[i], "-", &lv_font_montserrat_14, C_TEXT); lv_obj_set_pos(s_device_cells[i][1], 242, 0);
        s_device_cells[i][2] = make_label(s_device_rows[i], "-", &lv_font_montserrat_14, C_MUTED); lv_obj_set_pos(s_device_cells[i][2], 398, 0);
        s_device_cells[i][3] = make_label(s_device_rows[i], "-", &lv_font_montserrat_14, C_MUTED); lv_obj_set_pos(s_device_cells[i][3], 516, 0);
    }
}

static void create_data_page(lv_obj_t *page)
{
    make_label(page, "Unified data", &lv_font_montserrat_24, C_WHITE);
    lv_obj_t *sub = make_label(page, "Normalized points above, recent southbound frames below",
                               &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(sub, 0, 30);

    lv_obj_t *header = make_panel(page, 0, 58, 650, 36, true);
    make_label(header, "POINT", &lv_font_montserrat_14, C_MUTED);
    lv_obj_t *p = make_label(header, "VALUE", &lv_font_montserrat_14, C_MUTED); lv_obj_set_pos(p, 266, 0);
    p = make_label(header, "QUALITY", &lv_font_montserrat_14, C_MUTED); lv_obj_set_pos(p, 426, 0);
    p = make_label(header, "REV", &lv_font_montserrat_14, C_MUTED); lv_obj_set_pos(p, 558, 0);

    for (uint32_t i = 0U; i < GUI_POINT_ROWS; ++i) {
        int32_t y = 98 + (int32_t)i * 36;
        s_point_rows[i] = make_panel(page, 0, y, 650, 32, false);
        s_point_cells[i][0] = make_label(s_point_rows[i], "-", &lv_font_montserrat_14, C_TEXT);
        s_point_cells[i][1] = make_label(s_point_rows[i], "-", &lv_font_montserrat_14, C_TEXT); lv_obj_set_pos(s_point_cells[i][1], 266, 0);
        s_point_cells[i][2] = make_label(s_point_rows[i], "-", &lv_font_montserrat_14, C_MUTED); lv_obj_set_pos(s_point_cells[i][2], 426, 0);
        s_point_cells[i][3] = make_label(s_point_rows[i], "-", &lv_font_montserrat_14, C_MUTED); lv_obj_set_pos(s_point_cells[i][3], 558, 0);
    }

    lv_obj_t *traffic_header = make_panel(page, 0, 250, 650, 32, true);
    make_label(traffic_header, "RECENT BUS FRAMES", &lv_font_montserrat_14, C_MUTED);
    p = make_label(traffic_header, "SOURCE", &lv_font_montserrat_14, C_MUTED); lv_obj_set_pos(p, 166, 0);
    p = make_label(traffic_header, "ADDRESS", &lv_font_montserrat_14, C_MUTED); lv_obj_set_pos(p, 286, 0);
    p = make_label(traffic_header, "LEN", &lv_font_montserrat_14, C_MUTED); lv_obj_set_pos(p, 400, 0);
    p = make_label(traffic_header, "DATA", &lv_font_montserrat_14, C_MUTED); lv_obj_set_pos(p, 452, 0);

    for (uint32_t i = 0U; i < GUI_TRAFFIC_ROWS; ++i) {
        int32_t y = 286 + (int32_t)i * 34;
        s_traffic_rows[i] = make_panel(page, 0, y, 650, 30, false);
        s_traffic_cells[i][0] = make_label(s_traffic_rows[i], "-", &lv_font_montserrat_14, C_TEXT);
        s_traffic_cells[i][1] = make_label(s_traffic_rows[i], "-", &lv_font_montserrat_14, C_MUTED); lv_obj_set_pos(s_traffic_cells[i][1], 166, 0);
        s_traffic_cells[i][2] = make_label(s_traffic_rows[i], "-", &lv_font_montserrat_14, C_TEXT); lv_obj_set_pos(s_traffic_cells[i][2], 286, 0);
        s_traffic_cells[i][3] = make_label(s_traffic_rows[i], "-", &lv_font_montserrat_14, C_MUTED); lv_obj_set_pos(s_traffic_cells[i][3], 400, 0);
        s_traffic_cells[i][4] = make_label(s_traffic_rows[i], "-", &lv_font_montserrat_14, C_TEXT); lv_obj_set_pos(s_traffic_cells[i][4], 452, 0);
        lv_obj_set_width(s_traffic_cells[i][4], 172);
    }
}

static lv_obj_t *create_input_at(lv_obj_t *parent, const char *caption,
                                 const char *initial, int32_t x, int32_t y,
                                 int32_t width)
{
    lv_obj_t *caption_label = make_label(parent, caption, &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(caption_label, x, y);
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_pos(ta, x, y + 20);
    lv_obj_set_size(ta, width, 42);
    lv_obj_add_style(ta, &s_input_style, 0);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, initial);
    return ta;
}

static lv_obj_t *create_input(lv_obj_t *parent, const char *caption,
                              const char *initial, int32_t y, int32_t width)
{
    return create_input_at(parent, caption, initial, 0, y, width);
}

static void keyboard_hide(void)
{
    if (s_keyboard != NULL) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_keyboard, NULL);
    }
}

static void input_focus_event(lv_event_t *e)
{
    if ((lv_event_get_code(e) != LV_EVENT_FOCUSED) || (s_keyboard == NULL)) return;
    lv_obj_t *ta = lv_event_get_target(e);
    lv_keyboard_set_textarea(s_keyboard, ta);
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_keyboard);
}

static void keyboard_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if ((code == LV_EVENT_READY) || (code == LV_EVENT_CANCEL)) {
        keyboard_hide();
    }
}

static bool parse_uint_text(const char *text, uint32_t max_value, uint32_t *out)
{
    if ((text == NULL) || (out == NULL)) return false;
    while ((*text == ' ') || (*text == '\t')) ++text;
    uint32_t base = 10U;
    if ((text[0] == '0') && ((text[1] == 'x') || (text[1] == 'X'))) {
        base = 16U;
        text += 2;
    }
    if (*text == '\0') return false;
    uint32_t value = 0U;
    bool have = false;
    while (*text != '\0') {
        char c = *text++;
        if ((c == ' ') || (c == '\t')) continue;
        uint32_t digit;
        if ((c >= '0') && (c <= '9')) digit = (uint32_t)(c - '0');
        else if ((base == 16U) && (c >= 'a') && (c <= 'f')) digit = 10U + (uint32_t)(c - 'a');
        else if ((base == 16U) && (c >= 'A') && (c <= 'F')) digit = 10U + (uint32_t)(c - 'A');
        else return false;
        if ((digit >= base) || (digit > max_value)) return false;
        if (value > ((max_value - digit) / base)) return false;
        value = value * base + digit;
        have = true;
    }
    if (!have) return false;
    *out = value;
    return true;
}

static int hex_nibble(char c)
{
    if ((c >= '0') && (c <= '9')) return c - '0';
    if ((c >= 'a') && (c <= 'f')) return 10 + c - 'a';
    if ((c >= 'A') && (c <= 'F')) return 10 + c - 'A';
    return -1;
}

static bool parse_hex_bytes(const char *text, uint8_t *out, uint8_t *length)
{
    if ((text == NULL) || (out == NULL) || (length == NULL)) return false;
    uint8_t count = 0U;
    int high = -1;
    for (; *text != '\0'; ++text) {
        char c = *text;
        if ((c == ' ') || (c == '\t') || (c == ',') || (c == ':') || (c == '-')) continue;
        int nibble = hex_nibble(c);
        if (nibble < 0) return false;
        if (high < 0) {
            high = nibble;
        } else {
            if (count >= CANFD_MAX_DATA_BYTES) return false;
            out[count++] = (uint8_t)((high << 4) | nibble);
            high = -1;
        }
    }
    if (high >= 0) return false;
    *length = count;
    return true;
}

static void set_command_text(const char *text, uint32_t color)
{
    gui_label_set_text(s_command_status, text);
    lv_obj_set_style_text_color(s_command_status, lv_color_hex(color), 0);
}

static void show_lock_overlay(const char *message)
{
#if (GW_AUTH_ENABLE != 0U)
    if (s_lock_overlay == NULL) return;
    if (message != NULL && s_login_status != NULL) gui_label_set_text(s_login_status, message);
    lv_obj_remove_flag(s_lock_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_lock_overlay);
#else
    (void)message;
#endif
}

static bool operator_unlocked(void)
{
#if (GW_AUTH_ENABLE != 0U)
    if (!gw_security_hmi_unlocked()) {
        show_lock_overlay("Operator authentication required");
        return false;
    }
#endif
    gw_security_touch_session();
    return true;
}

static void can_send_event(lv_event_t *e)
{
    (void)e;
    keyboard_hide();
    if (!operator_unlocked()) return;
    uint32_t can_id;
    uint8_t data[CANFD_MAX_DATA_BYTES];
    uint8_t len;
    const char *id_text = lv_textarea_get_text(s_can_id_input);
    const char *data_text = lv_textarea_get_text(s_can_data_input);
    if (!parse_uint_text(id_text, 0x1FFFFFFFU, &can_id) ||
        !parse_hex_bytes(data_text, data, &len)) {
        set_command_text("CAN: invalid ID or hex payload", C_ERROR);
        return;
    }
    bool extended = can_id > 0x7FFU;
    gw_err_t err = gw_command_send_can(can_id, extended, true, data, len);
    if (err == GW_OK) set_command_text("CAN: frame queued (BRS forced OFF)", C_SUCCESS);
    else set_command_text("CAN: submit rejected", C_ERROR);
}

static void modbus_read_event(lv_event_t *e)
{
    (void)e;
    keyboard_hide();
    if (!operator_unlocked()) return;
    uint32_t slave, reg, qty;
    if (!parse_uint_text(lv_textarea_get_text(s_modbus_dev_input), 247U, &slave) ||
        (slave == 0U) ||
        !parse_uint_text(lv_textarea_get_text(s_modbus_reg_input), 65535U, &reg) ||
        !parse_uint_text(lv_textarea_get_text(s_modbus_qty_input), 125U, &qty) ||
        (qty == 0U)) {
        set_command_text("Modbus FC03: slave must be 1..247", C_ERROR);
        return;
    }
    gw_err_t err = gw_command_modbus_read_holding_slave((uint8_t)slave,
                                                          (uint16_t)reg,
                                                          (uint16_t)qty);
    if (err == GW_OK) set_command_text("Modbus FC03: request queued", C_SUCCESS);
    else if (err == GW_ERR_FULL || err == GW_ERR_BUSY)
        set_command_text("Modbus FC03: RS485 queue busy", C_WARNING);
    else set_command_text("Modbus FC03: request rejected", C_ERROR);
}

static void modbus_send_event(lv_event_t *e)
{
    (void)e;
    keyboard_hide();
    if (!operator_unlocked()) return;
    uint32_t slave, reg, value;
    if (!parse_uint_text(lv_textarea_get_text(s_modbus_dev_input), 247U, &slave) ||
        (slave == 0U) ||
        !parse_uint_text(lv_textarea_get_text(s_modbus_reg_input), 65535U, &reg) ||
        !parse_uint_text(lv_textarea_get_text(s_modbus_value_input), 65535U, &value)) {
        set_command_text("Modbus FC06: slave must be 1..247", C_ERROR);
        return;
    }
    gw_err_t err = gw_command_modbus_write_single_slave((uint8_t)slave,
                                                          (uint16_t)reg,
                                                          (uint16_t)value);
    if (err == GW_OK) set_command_text("Modbus FC06: request queued", C_SUCCESS);
    else if (err == GW_ERR_FULL || err == GW_ERR_BUSY)
        set_command_text("Modbus FC06: RS485 queue busy", C_WARNING);
    else set_command_text("Modbus FC06: request rejected", C_ERROR);
}

static lv_obj_t *create_action_button(lv_obj_t *parent, const char *text,
                                      int32_t x, int32_t y, int32_t w,
                                      lv_event_cb_t cb, bool primary)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, 44);
    lv_obj_add_style(btn, primary ? &s_primary_button_style : &s_secondary_button_style, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = make_label(btn, text, &lv_font_montserrat_16, C_WHITE);
    lv_obj_center(label);
    return btn;
}

static void create_control_page(lv_obj_t *page)
{
    make_label(page, "Device control", &lv_font_montserrat_24, C_WHITE);
    lv_obj_t *sub = make_label(page, "Operator commands use the same validated CAN / Modbus drivers",
                               &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(sub, 0, 30);

    lv_obj_t *can = make_panel(page, 0, 58, 319, 284, false);
    make_label(can, "CAN-FD transmit", &lv_font_montserrat_20, C_WHITE);
    lv_obj_t *note = make_label(can, "500 kbit/s  /  FD  /  BRS OFF", &lv_font_montserrat_14, C_SUCCESS);
    lv_obj_set_pos(note, 0, 28);
    s_can_id_input = create_input(can, "CAN ID  (0x... or decimal)", "0x302", 58, 291);
    s_can_data_input = create_input(can, "Payload hex", "47 57 00 00 00 01", 132, 291);
    lv_textarea_set_max_length(s_can_data_input, 192U);
    create_action_button(can, "SEND CAN-FD", 0, 222, 291, can_send_event, true);

    lv_obj_t *modbus = make_panel(page, 331, 58, 319, 284, false);
    make_label(modbus, "Modbus RTU", &lv_font_montserrat_20, C_WHITE);
    note = make_label(modbus, "Direct slave access  /  FC03 + FC06", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(note, 0, 28);

    s_modbus_dev_input = create_input_at(modbus, "Slave ID", "1", 0, 58, 88);
    s_modbus_reg_input = create_input_at(modbus, "Register", "0", 100, 58, 192);
    s_modbus_qty_input = create_input_at(modbus, "Quantity", "1", 0, 132, 88);
    s_modbus_value_input = create_input_at(modbus, "Write value", "1", 100, 132, 192);
    create_action_button(modbus, "READ FC03", 0, 222, 140, modbus_read_event, false);
    create_action_button(modbus, "WRITE FC06", 152, 222, 140, modbus_send_event, true);

    /* All textareas summon one shared touch keyboard. */
    lv_obj_add_event_cb(s_can_id_input, input_focus_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_can_data_input, input_focus_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_modbus_dev_input, input_focus_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_modbus_reg_input, input_focus_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_modbus_qty_input, input_focus_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_modbus_value_input, input_focus_event, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *status = make_panel(page, 0, 354, 650, 50, true);
    make_label(status, "LAST COMMAND", &lv_font_montserrat_14, C_MUTED);
    s_command_status = make_label(status, "Ready", &lv_font_montserrat_16, C_TEXT);
    lv_obj_set_pos(s_command_status, 128, 0);
}

static void create_network_page(lv_obj_t *page)
{
    make_label(page, "Network & uplink", &lv_font_montserrat_24, C_WHITE);
    lv_obj_t *sub = make_label(page, "Ethernet services and unified GW-JSONL stream",
                               &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(sub, 0, 30);

    lv_obj_t *net = make_panel(page, 0, 58, 319, 156, false);
    make_label(net, "Ethernet / TCP", &lv_font_montserrat_16, C_WHITE);
    s_network_summary = make_label(net, "Waiting for network...", &lv_font_montserrat_16, C_TEXT);
    lv_obj_set_pos(s_network_summary, 0, 36);

    lv_obj_t *up = make_panel(page, 331, 58, 319, 156, false);
    make_label(up, "Unified uplink", &lv_font_montserrat_16, C_WHITE);
    s_uplink_summary = make_label(up, "Port 5001 / GW-JSONL", &lv_font_montserrat_16, C_TEXT);
    lv_obj_set_pos(s_uplink_summary, 0, 36);

    lv_obj_t *cnt = make_panel(page, 0, 226, 650, 178, true);
    make_label(cnt, "Counters", &lv_font_montserrat_16, C_WHITE);
    s_network_counters = make_label(cnt, "Collecting counters...", &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_pos(s_network_counters, 0, 34);
    lv_obj_set_width(s_network_counters, 620);
}

static const char *selftest_text(gw_selftest_state_t state)
{
    switch (state) {
    case GW_SELFTEST_PASS: return "PASS";
    case GW_SELFTEST_FAIL: return "FAIL";
    case GW_SELFTEST_NOT_SUPPORTED: return "FIXTURE";
    default: return "IDLE";
    }
}

static void selftest_event(lv_event_t *e)
{
    (void)e;
    if (!operator_unlocked()) return;
    gw_diagnostics_run_selftest();
    if (s_diag_selftest != NULL) gui_label_set_text(s_diag_selftest, "Self-test completed; refreshing status...");
}

static void factory_reset_event(lv_event_t *e)
{
    (void)e;
    if (!operator_unlocked()) return;
    uint64_t now = gw_time_ms();
    if ((s_factory_arm_until_ms == 0U) || (now > s_factory_arm_until_ms)) {
        s_factory_arm_until_ms = now + 5000U;
        if (s_diag_selftest != NULL)
            gui_label_set_text(s_diag_selftest, "Factory reset ARMED: press again within 5 s");
        return;
    }
    s_factory_arm_until_ms = 0U;
    if (s_diag_selftest != NULL) gui_label_set_text(s_diag_selftest, "Factory reset requested; rebooting...");
    gw_config_request_factory_reset(true);
}

static void lock_hmi_event(lv_event_t *e)
{
    (void)e;
    gw_security_hmi_lock();
    show_lock_overlay("HMI locked");
}

static void create_diagnostics_page(lv_obj_t *page)
{
    make_label(page, "Service & diagnostics", &lv_font_montserrat_24, C_WHITE);
    lv_obj_t *sub = make_label(page, "Health trends, watchdog, storage and maintenance controls",
                               &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(sub, 0, 30);

    lv_obj_t *health = make_panel(page, 0, 58, 319, 146, false);
    make_label(health, "Runtime health", &lv_font_montserrat_16, C_WHITE);
    s_diag_summary = make_label(health, "Collecting diagnostics...", &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_pos(s_diag_summary, 0, 32);
    lv_obj_set_width(s_diag_summary, 290);

    lv_obj_t *touch = make_panel(page, 331, 58, 319, 146, false);
    make_label(touch, "Touch / storage", &lv_font_montserrat_16, C_WHITE);
    s_diag_touch = make_label(touch, "Collecting I2C status...", &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_pos(s_diag_touch, 0, 32);
    lv_obj_set_width(s_diag_touch, 290);

    lv_obj_t *trend = make_panel(page, 0, 216, 650, 122, true);
    make_label(trend, "120 s trend  /  newest at right", &lv_font_montserrat_14, C_MUTED);
    static const char *trend_name[4] = {"CPU", "CAN", "485", "LOSS"};
    static const uint32_t trend_color[4] = {C_CYAN, C_ACCENT, C_WARNING, C_ERROR};
    for (uint32_t row = 0U; row < 4U; ++row) {
        lv_obj_t *name = make_label(trend, trend_name[row], &lv_font_montserrat_14, C_MUTED);
        lv_obj_set_pos(name, 0, 24 + (int32_t)row * 20);
        for (uint32_t i = 0U; i < GUI_DIAG_SAMPLES; ++i) {
            lv_obj_t *bar = lv_obj_create(trend);
            s_diag_bars[row][i] = bar;
            lv_obj_set_pos(bar, 48 + (int32_t)i * 42, 37 + (int32_t)row * 20);
            lv_obj_set_size(bar, 30, 2);
            lv_obj_set_style_bg_color(bar, lv_color_hex(trend_color[row]), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(bar, 0, 0);
            lv_obj_set_style_radius(bar, 1, 0);
            lv_obj_set_style_pad_all(bar, 0, 0);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        }
        s_diag_trend_value[row] = make_label(trend, "--.-%", &lv_font_montserrat_14, C_TEXT);
        lv_obj_set_pos(s_diag_trend_value[row], 558, 24 + (int32_t)row * 20);
        lv_obj_set_width(s_diag_trend_value[row], 62);
        lv_obj_set_style_text_align(s_diag_trend_value[row], LV_TEXT_ALIGN_RIGHT, 0);
    }

    lv_obj_t *actions = make_panel(page, 0, 350, 650, 54, false);
    create_action_button(actions, "RUN SELF-TEST", 0, -8, 170, selftest_event, false);
    create_action_button(actions, "LOCK HMI", 184, -8, 140, lock_hmi_event, false);
    create_action_button(actions, "FACTORY RESET", 338, -8, 170, factory_reset_event, false);
    s_diag_selftest = make_label(actions, "Double-confirm reset within 5 seconds", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(s_diag_selftest, 518, 1);
    lv_obj_set_width(s_diag_selftest, 116);
}

static void create_pages(lv_obj_t *screen)
{
    for (uint8_t i = 0U; i < GUI_PAGE_COUNT; ++i) {
        s_pages[i] = lv_obj_create(screen);
        lv_obj_set_pos(s_pages[i], GUI_CONTENT_X + 12, GUI_CONTENT_Y + 10);
        lv_obj_set_size(s_pages[i], GUI_CONTENT_W - 24, GUI_CONTENT_H - 20);
        lv_obj_set_style_bg_opa(s_pages[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_pages[i], 0, 0);
        lv_obj_set_style_pad_all(s_pages[i], 0, 0);
        lv_obj_set_scrollbar_mode(s_pages[i], LV_SCROLLBAR_MODE_OFF);
        if (i != 0U) lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    create_overview_page(s_pages[0]);
    create_device_page(s_pages[1]);
    create_data_page(s_pages[2]);
    create_control_page(s_pages[3]);
    create_network_page(s_pages[4]);
    create_diagnostics_page(s_pages[5]);
}

static void create_keyboard(lv_obj_t *screen)
{
    s_keyboard = lv_keyboard_create(screen);
    lv_obj_set_pos(s_keyboard, 126, 250);
    lv_obj_set_size(s_keyboard, 674, 230);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(C_PANEL_ALT), 0);
    lv_obj_set_style_bg_opa(s_keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_keyboard, lv_color_hex(C_BORDER), 0);
    lv_obj_set_style_border_width(s_keyboard, 1, 0);
    lv_obj_add_event_cb(s_keyboard, keyboard_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_keyboard, keyboard_event, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void login_event(lv_event_t *e)
{
    (void)e;
    keyboard_hide();
#if (GW_AUTH_ENABLE != 0U)
    const char *user = lv_textarea_get_text(s_login_user);
    const char *password = lv_textarea_get_text(s_login_password);
    if (gw_security_hmi_authenticate(user, password)) {
        lv_textarea_set_text(s_login_password, "");
        gui_label_set_text(s_login_status, "Authenticated");
        lv_obj_add_flag(s_lock_overlay, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_textarea_set_text(s_login_password, "");
        gui_label_set_text(s_login_status, "Login failed / temporarily locked");
    }
#endif
}

static void create_lock_overlay(lv_obj_t *screen)
{
#if (GW_AUTH_ENABLE != 0U)
    s_lock_overlay = lv_obj_create(screen);
    lv_obj_set_pos(s_lock_overlay, 0, 0);
    lv_obj_set_size(s_lock_overlay, GW_LCD_HOR_RES, GW_LCD_VER_RES);
    lv_obj_set_style_bg_color(s_lock_overlay, lv_color_hex(0x0B0E10U), 0);
    lv_obj_set_style_bg_opa(s_lock_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_lock_overlay, 0, 0);
    lv_obj_set_scrollbar_mode(s_lock_overlay, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *card = make_panel(s_lock_overlay, 180, 78, 440, 300, false);
    make_label(card, "OPERATOR ACCESS", &lv_font_montserrat_24, C_WHITE);
    lv_obj_t *hint = make_label(card, "Authenticate to control field devices or change configuration.",
                                &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(hint, 0, 36);
    lv_obj_set_width(hint, 408);
    s_login_user = create_input(card, "Username", GW_AUTH_DEFAULT_USER, 82, 408);
    s_login_password = create_input(card, "Password", "", 154, 408);
    lv_textarea_set_password_mode(s_login_password, true);
    lv_textarea_set_max_length(s_login_password, 63U);
    lv_obj_add_event_cb(s_login_user, input_focus_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_login_password, input_focus_event, LV_EVENT_FOCUSED, NULL);
    create_action_button(card, "UNLOCK", 0, 226, 158, login_event, true);
    s_login_status = make_label(card, "Default credential must be changed before deployment",
                                &lv_font_montserrat_14, C_WARNING);
    lv_obj_set_pos(s_login_status, 176, 238);
    lv_obj_set_width(s_login_status, 225);
    lv_obj_move_foreground(s_lock_overlay);
#else
    (void)screen;
#endif
}

static void create_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    style_init();
    create_header(screen);
    create_sidebar(screen);
    create_pages(screen);
    create_keyboard(screen);
    create_lock_overlay(screen);
    show_page(0U);
}

static void refresh_devices(void)
{
    gw_device_t rows[GUI_DEVICE_ROWS];
    memset(rows, 0, sizeof(rows));
    uint32_t count = device_manager_snapshot(rows, GUI_DEVICE_ROWS);
    for (uint32_t i = 0U; i < GUI_DEVICE_ROWS; ++i) {
        if (i >= count) {
            gui_label_set_text(s_device_cells[i][0], (i == 0U) ? "No devices configured" : "");
            gui_label_set_text(s_device_cells[i][1], "");
            gui_label_set_text(s_device_cells[i][2], "");
            gui_label_set_text(s_device_cells[i][3], "");
            continue;
        }
        gui_label_set_text_fmt(s_device_cells[i][0], "%lu  %s",
                              (unsigned long)rows[i].id, rows[i].name);
        gui_label_set_text(s_device_cells[i][1], protocol_text(rows[i].protocol));
        gui_label_set_text(s_device_cells[i][2], device_state_text(rows[i].state));
        lv_obj_set_style_text_color(s_device_cells[i][2], state_color(rows[i].state), 0);
        gui_label_set_text_fmt(s_device_cells[i][3], "%lu / %lu",
                              (unsigned long)rows[i].success_count,
                              (unsigned long)rows[i].error_count);
    }
}

static void refresh_points(void)
{
    gw_point_t rows[GUI_POINT_ROWS];
    memset(rows, 0, sizeof(rows));
    uint32_t count = point_db_snapshot(rows, GUI_POINT_ROWS);
    for (uint32_t i = 0U; i < GUI_POINT_ROWS; ++i) {
        if (i >= count) {
            gui_label_set_text(s_point_cells[i][0], (i == 0U) ? "No points configured" : "");
            gui_label_set_text(s_point_cells[i][1], "");
            gui_label_set_text(s_point_cells[i][2], "");
            gui_label_set_text(s_point_cells[i][3], "");
            continue;
        }
        char value[32];
        point_value_text(&rows[i], value, sizeof(value));
        gui_label_set_text_fmt(s_point_cells[i][0], "%lu  %s",
                              (unsigned long)rows[i].id, rows[i].name);
        gui_label_set_text(s_point_cells[i][1], value);
        gui_label_set_text(s_point_cells[i][2], quality_text(rows[i].quality));
        lv_obj_set_style_text_color(s_point_cells[i][2], quality_color(rows[i].quality), 0);
        gui_label_set_text_fmt(s_point_cells[i][3], "%lu", (unsigned long)rows[i].revision);
    }
}

static void refresh_recent_frames(void)
{
    gw_uplink_event_t events[GUI_TRAFFIC_ROWS];
    memset(events, 0, sizeof(events));
    uint32_t count = gw_uplink_history_snapshot(events, GUI_TRAFFIC_ROWS);
    uint32_t shown = (count > GUI_TRAFFIC_ROWS) ? GUI_TRAFFIC_ROWS : count;
    uint32_t first = (count > shown) ? (count - shown) : 0U;

    for (uint32_t i = 0U; i < GUI_TRAFFIC_ROWS; ++i) {
        if (i >= shown) {
            gui_label_set_text(s_traffic_cells[i][0], (i == 0U) ? "No bus traffic yet" : "");
            for (uint32_t c = 1U; c < 5U; ++c) gui_label_set_text(s_traffic_cells[i][c], "");
            continue;
        }
        const gw_uplink_event_t *event = &events[first + i];
        char data_text[40];
        size_t pos = 0U;
        uint16_t bytes = (event->length > 8U) ? 8U : event->length;
        for (uint16_t b = 0U; b < bytes; ++b) {
            int n = snprintf(&data_text[pos], sizeof(data_text) - pos,
                             (b + 1U < bytes) ? "%02X " : "%02X",
                             event->data[b]);
            if ((n <= 0) || ((size_t)n >= (sizeof(data_text) - pos))) break;
            pos += (size_t)n;
        }
        if (event->length > bytes && (pos + 4U) < sizeof(data_text)) {
            memcpy(&data_text[pos], " ...", 5U);
        } else {
            data_text[pos] = '\0';
        }
        gui_label_set_text_fmt(s_traffic_cells[i][0], "#%lu  %s",
                              (unsigned long)event->sequence,
                              event->tx_direction ? "TX" : "RX");
        gui_label_set_text_fmt(s_traffic_cells[i][1], "%s / %s",
                              interface_text(event->interface_id),
                              protocol_text(event->protocol));
        if (event->protocol == GW_PROTO_CAN) {
            gui_label_set_text_fmt(s_traffic_cells[i][2], "0x%lX",
                                  (unsigned long)event->address);
        } else {
            gui_label_set_text_fmt(s_traffic_cells[i][2], "%lu / FC%02X",
                                  (unsigned long)event->address, (unsigned)event->code);
        }
        gui_label_set_text_fmt(s_traffic_cells[i][3], "%u", (unsigned)event->length);
        gui_label_set_text(s_traffic_cells[i][4], data_text);
        lv_obj_set_style_text_color(s_traffic_cells[i][0],
            lv_color_hex(event->result == GW_OK ? C_TEXT : C_ERROR), 0);
    }
}

static uint32_t online_device_count(void)
{
    gw_device_t rows[GW_MAX_DEVICES];
    uint32_t count = device_manager_snapshot(rows, GW_MAX_DEVICES);
    uint32_t online = 0U;
    for (uint32_t i = 0U; i < count; ++i) if (rows[i].state == DEVICE_ONLINE) ++online;
    return online;
}

static const char *command_state_text(gw_command_state_t state)
{
    switch (state) {
    case GW_COMMAND_IDLE: return "IDLE";
    case GW_COMMAND_QUEUED: return "QUEUED";
    case GW_COMMAND_OK: return "OK";
    case GW_COMMAND_ERROR: return "ERROR";
    default: return "?";
    }
}

static const char *uplink_if_short(gw_interface_id_t interface_id)
{
    switch (interface_id) {
    case GW_IF_CANFD_0: return "CAN";
    case GW_IF_RS485_0: return "485";
    case GW_IF_ETH_0: return "ETH";
    default: return "?";
    }
}

static void refresh_recent_traffic(void)
{
    gw_uplink_event_t events[5];
    uint32_t count = gw_uplink_history_snapshot(events, 5U);
    if (count == 0U) {
        gui_label_set_text(s_traffic_label, "No southbound frames yet");
        return;
    }

    char text[480];
    size_t pos = 0U;
    text[0] = '\0';
    for (uint32_t i = 0U; (i < count) && (pos + 32U < sizeof(text)); ++i) {
        const gw_uplink_event_t *ev = &events[i];
        char hex[19];
        size_t hp = 0U;
        uint16_t shown = (ev->length < 6U) ? ev->length : 6U;
        for (uint16_t b = 0U; (b < shown) && (hp + 3U < sizeof(hex)); ++b) {
            int n = snprintf(&hex[hp], sizeof(hex) - hp, "%02X", ev->data[b]);
            if (n <= 0) break;
            hp += (size_t)n;
        }
        if (ev->length > shown && (hp + 3U < sizeof(hex))) {
            hex[hp++] = '.';
            hex[hp++] = '.';
        }
        hex[hp] = '\0';

        int n = snprintf(&text[pos], sizeof(text) - pos,
                         "#%lu  %-3s %-2s  A:%lX  %uB  %s%s",
                         (unsigned long)ev->sequence,
                         uplink_if_short(ev->interface_id),
                         ev->tx_direction ? "TX" : "RX",
                         (unsigned long)ev->address,
                         (unsigned)ev->length,
                         hex,
                         (i + 1U < count) ? "\n" : "");
        if ((n <= 0) || ((size_t)n >= (sizeof(text) - pos))) break;
        pos += (size_t)n;
    }
    gui_label_set_text(s_traffic_label, text);
}

static void refresh_ui(void)
{
    canfd_stats_t can;
    rs485_dma_stats_t rs485;
    gw_net_status_t net;
    gw_ethernetif_stats_t eth;
    gw_tcp_server_stats_t tcp;
    gw_uplink_stats_t uplink;
    gw_command_status_t command;
    gw_lcd_stats_t lcd;
    gw_diag_status_t diag;
    gw_watchdog_stats_t wd;
    gw_touch_stats_t touch;
    gw_flash_store_stats_t flash;
    gw_security_stats_t security;
    memset(&can, 0, sizeof(can));
    memset(&rs485, 0, sizeof(rs485));
    memset(&net, 0, sizeof(net));
    memset(&eth, 0, sizeof(eth));
    memset(&tcp, 0, sizeof(tcp));
    memset(&uplink, 0, sizeof(uplink));
    memset(&command, 0, sizeof(command));
    memset(&lcd, 0, sizeof(lcd));
    memset(&diag, 0, sizeof(diag));
    memset(&wd, 0, sizeof(wd));
    memset(&touch, 0, sizeof(touch));
    memset(&flash, 0, sizeof(flash));
    memset(&security, 0, sizeof(security));

    drv_canfd_get_stats(&can);
    drv_rs485_get_stats(&rs485);
    gw_net_get_status(&net);
    gw_ethernetif_get_stats(&eth);
    gw_tcp_server_get_stats(&tcp);
    gw_uplink_get_stats(&uplink);
    gw_command_router_get_status(&command);
    gw_lcd_get_stats(&lcd);
    gw_diagnostics_get_status(&diag);
    gw_watchdog_get_stats(&wd);
    gw_touch_get_stats(&touch);
    gw_flash_store_get_stats(&flash);
    gw_security_get_stats(&security);

    uint64_t clock_s = gw_time_is_synchronized() ? (gw_time_utc_ms() / 1000ULL)
                                                  : (gw_time_ms() / 1000ULL);
    gui_label_set_text_fmt(s_clock_label, "%02lu:%02lu:%02lu%s",
                          (unsigned long)((clock_s / 3600ULL) % 24ULL),
                          (unsigned long)((clock_s / 60ULL) % 60ULL),
                          (unsigned long)(clock_s % 60ULL),
                          gw_time_is_synchronized() ? " UTC" : "");

    bool rs485_ok = (rs485.dma_error_count == 0U) && (rs485.uart_error_count == 0U);
    bool can_ok = !drv_canfd_tx_is_held() && (can.busoff_count == 0U);
    gui_label_set_text_fmt(s_header_state, "CAN %s   485 %s   NET %s",
                          can_ok ? "OK" : "ERR",
                          rs485_ok ? "OK" : "ERR",
                          net.ip_ready ? "UP" : "DOWN");
    lv_obj_set_style_text_color(s_header_state,
        lv_color_hex((can_ok && rs485_ok && net.ip_ready) ? C_SUCCESS : C_WARNING), 0);

    if (s_active_page == 0U) {
    uint32_t dev_count = device_manager_count();
    uint32_t online = online_device_count();
    uint32_t points = point_db_count();
    gui_label_set_text_fmt(s_metric_devices, "%lu / %lu",
                          (unsigned long)online, (unsigned long)dev_count);
    gui_label_set_text_fmt(s_metric_points, "%lu", (unsigned long)points);
    gui_label_set_text_fmt(s_metric_can, "%s  %lu/%lu",
                          drv_canfd_tx_is_held() ? "HOLD" : "OK",
                          (unsigned long)can.rx_frames,
                          (unsigned long)can.tx_success);
    gui_label_set_text(s_metric_net, net.ip_ready ? "ONLINE" : (net.link_up ? "LINK" : "DOWN"));

    gui_label_set_text_fmt(s_health_label,
        "CAN-FD     %s\n"
        "RS485      %s\n"
        "Ethernet   %s\n"
        "TCP :5000  %s\n"
        "Uplink     %s\n"
        "Touch      %s",
        can_ok ? "HEALTHY" : "CHECK",
        rs485_ok ? "HEALTHY" : "CHECK",
        net.ip_ready ? "READY" : "DOWN",
        tcp.client_connected ? "CLIENT" : (tcp.listening ? "LISTEN" : "DOWN"),
        uplink.client_connected ? "STREAMING" : (uplink.listening ? "LISTEN" : "DOWN"),
        gw_lv_touch_available() ? "READY" : "NOT FOUND");

    refresh_recent_traffic();
    }

    if (s_active_page == 4U) {
    gui_label_set_text_fmt(s_network_summary,
        "Link      %s\n"
        "IPv4      192.168.103.213\n"
        "TCP echo  :5000  %s",
        net.link_up ? "100M / UP" : "DOWN",
        tcp.client_connected ? "CLIENT" : (tcp.listening ? "LISTEN" : "DOWN"));

    gui_label_set_text_fmt(s_uplink_summary,
        "GW-JSONL  :%u\n"
        "State     %s\n"
        "Format    GW-JSONL v2",
        (unsigned)GW_UPLINK_PORT,
        uplink.client_connected ? "STREAMING" : (uplink.listening ? "LISTEN" : "DOWN"));

    gui_label_set_text_fmt(s_network_counters,
        "ETH  rx=%lu irq=%lu drop=%lu alloc=%lu   tx=%lu fail=%lu wait=%lu\n"
        "TCP  acc=%lu disc=%lu rx=%luB tx=%luB err=%lu/%lu\n"
        "UP   acc=%lu disc=%lu point=%lu frame=%lu drop=%lu cmd=%lu/%lu tx=%luB\n"
        "CAN  TEC/REC=%lu/%lu  overrun=%lu drop=%lu busoff=%lu\n"
        "LCD  flip=%lu timeout=%lu err=%lu  IPA=%lu/%lu %luKB",
        (unsigned long)eth.rx_frames, (unsigned long)eth.rx_irq,
        (unsigned long)eth.rx_input_fail, (unsigned long)eth.rx_alloc_fail,
        (unsigned long)eth.tx_frames, (unsigned long)eth.tx_fail,
        (unsigned long)eth.tx_busy_timeout,
        (unsigned long)tcp.accept_count, (unsigned long)tcp.disconnect_count,
        (unsigned long)tcp.rx_bytes, (unsigned long)tcp.tx_bytes,
        (unsigned long)tcp.recv_error_count, (unsigned long)tcp.send_error_count,
        (unsigned long)uplink.accept_count, (unsigned long)uplink.disconnect_count,
        (unsigned long)uplink.point_sent, (unsigned long)uplink.frame_sent,
        (unsigned long)uplink.frame_dropped,
        (unsigned long)uplink.rx_command_count,
        (unsigned long)uplink.command_error_count,
        (unsigned long)uplink.tx_bytes,
        (unsigned long)can.tx_error_count, (unsigned long)can.rx_error_count,
        (unsigned long)can.rx_overrun, (unsigned long)can.rx_queue_drop,
        (unsigned long)can.busoff_count,
        (unsigned long)lcd.flush_count, (unsigned long)lcd.flush_timeout_count,
        (unsigned long)lcd.flush_error_count,
        (unsigned long)lcd.ipa_blit_count, (unsigned long)lcd.ipa_sync_count,
        (unsigned long)(lcd.ipa_bytes / 1024U));
    }

    if (s_active_page == 5U) {
    gui_label_set_text_fmt(s_diag_summary,
        "CPU      %u.%u %%\nHeap     %lu B\nCAN load %u.%u %%\nRS485    %u.%u %%\nLoss     %u.%u %%\nWD stale 0x%02lX",
        (unsigned)(diag.cpu_load_permille / 10U), (unsigned)(diag.cpu_load_permille % 10U),
        (unsigned long)diag.free_heap_bytes,
        (unsigned)(diag.can_load_permille / 10U), (unsigned)(diag.can_load_permille % 10U),
        (unsigned)(diag.rs485_load_permille / 10U), (unsigned)(diag.rs485_load_permille % 10U),
        (unsigned)(diag.rs485_loss_permille / 10U), (unsigned)(diag.rs485_loss_permille % 10U),
        (unsigned long)wd.stale_mask);
    gui_label_set_text_fmt(s_diag_touch,
        "Touch     %s\nread/err  %lu / %lu\ntimeout    %lu\nrecover    %lu  reprobe %lu\nspool      %lu records\nauth fail  %lu",
        gw_lv_touch_available() ? "ONLINE" : "RECOVERING",
        (unsigned long)touch.read_count, (unsigned long)touch.read_error_count,
        (unsigned long)touch.io_timeout_count, (unsigned long)touch.recovery_count,
        (unsigned long)touch.reprobe_count, (unsigned long)flash.spool_valid_count,
        (unsigned long)security.auth_fail);

    gw_diag_sample_t hist[GUI_DIAG_SAMPLES];
    uint32_t hn = gw_diagnostics_history(hist, GUI_DIAG_SAMPLES);
    uint32_t lead = GUI_DIAG_SAMPLES - hn;
    for (uint32_t row = 0U; row < 4U; ++row) {
        uint16_t latest = 0U;
        for (uint32_t i = 0U; i < GUI_DIAG_SAMPLES; ++i) {
            uint16_t v = 0U;
            if (i >= lead) {
                const gw_diag_sample_t *sample = &hist[i - lead];
                v = (row == 0U) ? sample->cpu_load_permille :
                    (row == 1U) ? sample->can_load_permille :
                    (row == 2U) ? sample->rs485_load_permille : sample->rs485_loss_permille;
                latest = v;
            }
            uint16_t h = (uint16_t)(2U + ((uint32_t)v * 12U) / 1000U);
            if (h > 14U) h = 14U;
            lv_obj_set_y(s_diag_bars[row][i], 37 + (int32_t)row * 20 - ((int32_t)h - 2));
            lv_obj_set_height(s_diag_bars[row][i], h);
        }
        if (hn != 0U)
            gui_label_set_text_fmt(s_diag_trend_value[row], "%u.%u%%", (unsigned)(latest / 10U), (unsigned)(latest % 10U));
        else
            gui_label_set_text(s_diag_trend_value[row], "--.-%");
    }
    gw_selftest_status_t selftest; memset(&selftest, 0, sizeof(selftest));
    gw_diagnostics_get_selftest(&selftest);
    if ((selftest.can_state != GW_SELFTEST_IDLE) || (selftest.rs485_state != GW_SELFTEST_IDLE)) {
        gui_label_set_text_fmt(s_diag_selftest, "C %s/%s\nR %s/%s",
                              selftest_text(selftest.can_state), selftest_text(selftest.can_fixture_state),
                              selftest_text(selftest.rs485_state), selftest_text(selftest.rs485_fixture_state));
    } else if ((s_factory_arm_until_ms != 0U) && (gw_time_ms() > s_factory_arm_until_ms)) {
        s_factory_arm_until_ms = 0U;
        gui_label_set_text(s_diag_selftest, "Double-confirm reset within 5 seconds");
    }
    }

#if (GW_AUTH_ENABLE != 0U)
    if (!gw_security_hmi_unlocked()) show_lock_overlay("HMI locked - authenticate to continue");
#endif

    if ((s_active_page == 3U) &&
        ((command.state == GW_COMMAND_OK) || (command.state == GW_COMMAND_ERROR))) {
        if ((command.kind == GW_COMMAND_MODBUS_READ_HOLDING) ||
            (command.kind == GW_COMMAND_MODBUS_WRITE_SINGLE)) {
            gui_label_set_text_fmt(s_command_status,
                                  "%s  S%u R%u  #%lu  result=%ld",
                                  command_state_text(command.state),
                                  (unsigned)command.modbus_slave,
                                  (unsigned)command.modbus_register,
                                  (unsigned long)command.sequence,
                                  (long)command.result);
        } else {
            gui_label_set_text_fmt(s_command_status, "%s  #%lu  result=%ld",
                                  command_state_text(command.state),
                                  (unsigned long)command.sequence,
                                  (long)command.result);
        }
        lv_obj_set_style_text_color(s_command_status,
            lv_color_hex(command.state == GW_COMMAND_OK ? C_SUCCESS : C_ERROR), 0);
    }

    if (s_active_page == 1U) {
        refresh_devices();
    } else if (s_active_page == 2U) {
        refresh_points();
        refresh_recent_frames();
    }
    ++s_stats.refresh_count;
}

static void gui_task(void *argument)
{
    (void)argument;
    s_stats.task_started = true;
    vTaskDelay(pdMS_TO_TICKS(100U));

    if (!gw_lv_port_init()) {
        ++s_stats.init_fail_count;
        GW_LOGE("GUI", "GUI initialization failed; communications continue in degraded mode");
        if (g_system_events != NULL) (void)xEventGroupSetBits(g_system_events, EVT_SYSTEM_DEGRADED);
        for (;;) {
            gw_watchdog_beat(GW_WD_GUI);
            vTaskDelay(pdMS_TO_TICKS(1000U));
        }
    }

    s_stats.display_ready = true;
    s_stats.touch_ready = gw_lv_touch_available();
    GW_LOGI("GUI", "creating HMI object tree");
    create_ui();

    lv_mem_monitor_t mem;
    memset(&mem, 0, sizeof(mem));
    lv_mem_monitor(&mem);
    GW_LOGI("GUI", "LVGL heap=%luB used=%u%% free=%luB largest=%luB frag=%u%%",
            (unsigned long)mem.total_size, (unsigned)mem.used_pct,
            (unsigned long)mem.free_size, (unsigned long)mem.free_biggest_size,
            (unsigned)mem.frag_pct);

    refresh_ui();
    /* Force the first refresh now so the boot surface is replaced immediately
     * instead of waiting for the first periodic refresh cycle. */
    lv_refr_now(NULL);
    GW_LOGI("GUI", "HMI ready 800x480 LVGL 9.2.2 partial-IPA 2x40-line touch=%s",
            s_stats.touch_ready ? "YES" : "NO");

    uint64_t next_refresh = gw_time_ms() + GW_GUI_REFRESH_MS;
    for (;;) {
        gw_watchdog_beat(GW_WD_GUI);
        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms < 5U) wait_ms = 5U;
        if (wait_ms > 20U) wait_ms = 20U;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        uint64_t now = gw_time_ms();
        if (now >= next_refresh) {
            refresh_ui();
            next_refresh = now + GW_GUI_REFRESH_MS;
        }
    }
}

void gw_gui_task_create(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    TaskHandle_t handle = xTaskCreateStatic(gui_task, "gui",
                                            GW_GUI_TASK_STACK_WORDS, NULL,
                                            GW_GUI_TASK_PRIORITY,
                                            s_gui_stack, &s_gui_task_cb);
    configASSERT(handle != NULL);
}

void gw_gui_get_stats(gw_gui_stats_t *out)
{
    if (out != NULL) *out = s_stats;
}

#else

void gw_gui_task_create(void) {}
void gw_gui_get_stats(gw_gui_stats_t *out)
{
    if (out != NULL) memset(out, 0, sizeof(*out));
}

#endif
