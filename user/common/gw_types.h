#ifndef GW_TYPES_H
#define GW_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#define GW_MAX_POINTS        1000U
#define GW_MAX_DEVICES       32U
#define GW_POINT_NAME_LEN    32U
#define GW_DEVICE_NAME_LEN   32U

typedef enum {
    GW_IF_RS485_0 = 0,
    GW_IF_CANFD_0,
    GW_IF_ETH_0,
} gw_interface_id_t;

typedef enum {
    GW_PROTO_NONE = 0,
    GW_PROTO_MODBUS_RTU,
    GW_PROTO_MODBUS_TCP,
    GW_PROTO_CAN,
    GW_PROTO_RS485_RAW,
} gw_protocol_t;

typedef enum {
    GW_QUALITY_GOOD = 0,
    GW_QUALITY_STALE,
    GW_QUALITY_TIMEOUT,
    GW_QUALITY_BAD,
    GW_QUALITY_OFFLINE,
    GW_QUALITY_INVALID,
} gw_quality_t;

typedef enum {
    GW_VALUE_BOOL = 0,
    GW_VALUE_U16,
    GW_VALUE_I16,
    GW_VALUE_U32,
    GW_VALUE_I32,
    GW_VALUE_F32,
    GW_VALUE_F64,
} gw_value_type_t;

typedef union {
    bool b;
    uint16_t u16;
    int16_t i16;
    uint32_t u32;
    int32_t i32;
    float f32;
    double f64;
} gw_value_t;

typedef struct {
    uint32_t point_id;
    uint32_t device_id;
    gw_value_type_t type;
    gw_value_t value;
    gw_quality_t quality;
    uint64_t timestamp_ms;
} point_update_t;

typedef struct {
    uint32_t id;
    uint32_t device_id;
    char name[GW_POINT_NAME_LEN];
    gw_value_type_t type;
    gw_value_t value;
    float scale;
    float offset;
    gw_quality_t quality;
    uint64_t timestamp_ms;
    /* Monotonic content revision used by northbound ACK logic.  A publisher
     * snapshots (point, revision), sends it, then clears dirty only if the
     * revision is still unchanged. */
    uint32_t revision;
    bool valid;
    bool dirty;
} gw_point_t;

typedef enum {
    DEVICE_DISABLED = 0,
    DEVICE_INIT,
    DEVICE_ONLINE,
    DEVICE_OFFLINE,
    DEVICE_ERROR,
} device_state_t;

typedef struct {
    uint32_t id;
    char name[GW_DEVICE_NAME_LEN];
    gw_protocol_t protocol;
    gw_interface_id_t interface_id;
    uint16_t address;
    uint32_t timeout_ms;
    uint8_t retry;
    device_state_t state;
    uint64_t last_seen_ms;
    uint32_t success_count;
    uint32_t error_count;
    uint32_t consecutive_error;
    int32_t last_error;
    uint64_t last_error_ms;
    bool valid;
} gw_device_t;

#endif
