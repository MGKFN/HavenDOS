#pragma once
#include "types.h"
#define HTTP_MAX_BODY (512*1024)
#define HTTP_ERR_URL      -1
#define HTTP_ERR_DNS      -2
#define HTTP_ERR_CONNECT  -3
#define HTTP_ERR_TLS      -4
#define HTTP_ERR_SEND     -5
#define HTTP_ERR_RECV     -6
typedef struct {
    char     *body;
    uint32_t  body_len;
    uint32_t  body_max;
    int       status;
    char      file_path[128];
    int       follow_redirect;
    void    (*progress)(int cur, int total);
} http_resp_t;
int http_get(const char *url, http_resp_t *resp);
