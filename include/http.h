#pragma once
#include "types.h"

#define HTTP_MAX_BODY (512*1024)

#define HTTP_ERR_URL      -1
#define HTTP_ERR_DNS      -2
#define HTTP_ERR_CONNECT  -3
#define HTTP_ERR_TLS      -4
#define HTTP_ERR_SEND     -5
#define HTTP_ERR_RECV     -6
#define HTTP_ERR_REDIRECT -7
#define HTTP_ERR_NOMEM    -8   /* v0.7.18: no memory to stage a download */

/* -- v0.7.6 CRITICAL ABI FIX --------------------------------------------
 * Until v0.7.5 there were TWO different definitions of this struct:
 *
 *   include/http.h  (what net/http.c compiled against)
 *       body,body_len,body_max,status,file_path[128],follow_redirect,progress
 *       -> sizeof 152, follow_redirect at +144, progress at +148
 *
 *   shell/shell.c   (a local typedef inside cmd_wget / cmd_curl)
 *       body,body_max,file_path,follow_redirect,progress,status,body_len
 *       -> sizeof 28
 *
 * shell.c declared `extern int http_get(const char*, void*)` so the compiler
 * never saw the mismatch.  http.c therefore read follow_redirect and the
 * progress FUNCTION POINTER from 124 bytes past the end of a 28-byte stack
 * object and called whatever it found.  wget and curl were calling a garbage
 * address on every request.
 *
 * Second, independent bug: file_path was a char[128] ARRAY, so http.c's
 * `if(filepath)` test was always true (an array never decays to NULL).  Every
 * response body went to the file branch and resp->body was never filled --
 * that is why `bpkg update` and `bpkg install` could never see a downloaded
 * byte even though the transfer itself succeeded.  file_path is now a real
 * pointer and http.c tests both the pointer and its first character.
 *
 * There is now exactly ONE definition and every caller includes this header.
 * --------------------------------------------------------------------- */
typedef struct {
    uint8_t    *body;            /* NULL = do not keep the body in memory   */
    int         body_max;        /* capacity of body[]                      */
    const char *file_path;       /* NULL/"" = do not stream to disk         */
    int         follow_redirect; /* 1 = follow 301/302/303/307/308          */
    void      (*progress)(int cur, int total);
    int         status;          /* out: HTTP status code                   */
    int         body_len;        /* out: bytes actually stored              */
} http_resp_t;

int http_get(const char *url, http_resp_t *resp);

/* Human-readable text for a negative http_get() return value. */
const char *http_strerror(int err);
