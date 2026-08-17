#ifndef GW_LV_PORT_H
#define GW_LV_PORT_H

#include <stdbool.h>
#include "lvgl.h"

bool gw_lv_port_init(void);
lv_display_t *gw_lv_display(void);
bool gw_lv_touch_available(void);

#endif /* GW_LV_PORT_H */
