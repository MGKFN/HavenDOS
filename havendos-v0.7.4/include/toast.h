#pragma once
typedef enum { TOAST_INFO=0, TOAST_SUCCESS=1, TOAST_WARNING=2, TOAST_ERROR=3 } toast_type_t;
void toast_push(toast_type_t type, const char *msg);
void toast_info(const char *m);
void toast_success(const char *m);
void toast_warning(const char *m);
void toast_error(const char *m);
void toast_tick(void);
void toast_clear(void);
