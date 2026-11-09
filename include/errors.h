#pragma once
#include "types.h"
typedef int32_t hd_err_t;
#define HD_OK            0
#define HD_ERR_NOMEM    -1
#define HD_ERR_NODEV    -2
#define HD_ERR_IO       -3
#define HD_ERR_INVAL    -4
#define HD_ERR_NOENT    -5
void kernel_log(hd_err_t code, const char *msg);

/* Error codes */
#define ERR_MEM_OOM         -10
#define ERR_MEM_CORRUPT     -11
#define ERR_MEM_BAD_PTR     -12
#define ERR_MEM_DOUBLE_FREE -13
#define ERR_FS_NOT_FOUND    -20
#define ERR_FS_EXISTS       -21
#define ERR_FS_IO           -22
#define ERR_NET_NO_NIC      -30
#define ERR_NET_TIMEOUT     -31
#define ERR_NET_MALFORMED   -32
#define ERR_NET_DNS_FAILED  -33
