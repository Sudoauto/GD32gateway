#ifndef GW_GUI_H
#define GW_GUI_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool task_started;
    bool display_ready;
    bool touch_ready;
    uint32_t refresh_count;
    uint32_t init_fail_count;
} gw_gui_stats_t;

void gw_gui_task_create(void);
void gw_gui_get_stats(gw_gui_stats_t *out);

#endif /* GW_GUI_H */
