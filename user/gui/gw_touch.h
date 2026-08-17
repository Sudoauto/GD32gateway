#ifndef GW_TOUCH_H
#define GW_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool initialized;
    uint32_t read_count;
    uint32_t read_error_count;
    uint32_t io_timeout_count;
    uint32_t bus_error_count;
    uint32_t recovery_count;
    uint32_t reprobe_count;
    uint32_t consecutive_error_count;
} gw_touch_stats_t;

bool gw_touch_init(void);
bool gw_touch_read(uint16_t *x, uint16_t *y, bool *pressed);
bool gw_touch_available(void);
const char *gw_touch_product_id(void);
uint8_t gw_touch_max_points(void);
void gw_touch_get_stats(gw_touch_stats_t *out);

#endif /* GW_TOUCH_H */
