#ifndef GW_ERROR_H
#define GW_ERROR_H

#include <stdint.h>

typedef int32_t gw_err_t;

#define GW_OK                    ((gw_err_t)0)
#define GW_ERR_TIMEOUT           ((gw_err_t)-1)
#define GW_ERR_BUSY              ((gw_err_t)-2)
#define GW_ERR_PARAM             ((gw_err_t)-3)
#define GW_ERR_NO_MEMORY         ((gw_err_t)-4)
#define GW_ERR_IO                ((gw_err_t)-5)
#define GW_ERR_CRC               ((gw_err_t)-6)
#define GW_ERR_STATE             ((gw_err_t)-7)
#define GW_ERR_FULL              ((gw_err_t)-8)
#define GW_ERR_NOT_FOUND         ((gw_err_t)-9)
#define GW_ERR_NOT_SUPPORTED     ((gw_err_t)-10)
#define GW_ERR_PROTOCOL          ((gw_err_t)-11)
#define GW_ERR_AUTH              ((gw_err_t)-12)

#endif
