#pragma once
#include "types.h"
void    rtc_get_time(uint8_t *h, uint8_t *m, uint8_t *s);
void    rtc_get_date(uint8_t *day, uint8_t *month, uint16_t *year);
void    rtc_time_str(char *buf);
void    rtc_date_str(char *buf);
uint32_t rtc_read_seconds_approx(void);
