#ifndef CAN_DECODER_H
#define CAN_DECODER_H

#include <stdbool.h>
#include <stdint.h>
#include "drv_canfd.h"
#include "gw_error.h"

typedef enum {
    CAN_SIGNAL_U8 = 0,
    CAN_SIGNAL_I8,
    CAN_SIGNAL_U16,
    CAN_SIGNAL_I16,
    CAN_SIGNAL_U32,
    CAN_SIGNAL_I32,
    CAN_SIGNAL_F32,
} can_signal_encoding_t;

typedef enum {
    CAN_ENDIAN_BIG = 0,
    CAN_ENDIAN_LITTLE,
} can_endian_t;

typedef struct {
    uint32_t id;
    uint32_t device_id;
    uint32_t point_id;
    uint32_t can_id;
    uint8_t byte_offset;
    can_signal_encoding_t encoding;
    can_endian_t endian;
    bool extended;
    bool require_fd;
    bool enabled;
} can_signal_map_t;

typedef struct {
    uint32_t frames_seen;
    uint32_t frames_matched;
    uint32_t signals_decoded;
    uint32_t point_updates_queued;
    uint32_t point_update_drop;
    uint32_t invalid_length;
    uint32_t schema_error;
    uint32_t stale_events;
    uint32_t offline_events;
} can_decoder_stats_t;

void can_decoder_init(void);
gw_err_t can_decoder_register(const can_signal_map_t *map);
gw_err_t can_decoder_upsert(const can_signal_map_t *map);
gw_err_t can_decoder_remove(uint32_t map_id);
void can_decoder_reset(void);
uint32_t can_decoder_snapshot(can_signal_map_t *out, uint32_t max_count);
void can_decoder_process(const canfd_frame_t *frame);
void can_decoder_maintenance(uint64_t now_ms);
void can_decoder_get_stats(can_decoder_stats_t *out);

#endif
