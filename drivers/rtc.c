/*
 * rtc.c — CMOS Real-Time Clock driver for BOOT OS v0.5.9.3
 */
#include "../include/types.h"
#include "../include/io.h"

#define CMOS_ADDR  0x70
#define CMOS_DATA  0x71
#define RTC_SEC    0x00
#define RTC_MIN    0x02
#define RTC_HOUR   0x04
#define RTC_DAY    0x07
#define RTC_MONTH  0x08
#define RTC_YEAR   0x09
#define RTC_REGA   0x0A
#define RTC_REGB   0x0B

static uint8_t cmos_read(uint8_t reg){
    outb(CMOS_ADDR, reg | 0x80);
    io_wait();
    return inb(CMOS_DATA);
}
static void rtc_wait_ready(void){
    int t=1000;
    while(t-- && (cmos_read(RTC_REGA) & 0x80)) io_wait();
}
static uint8_t bcd2bin(uint8_t v){ return (v&0x0F)+((v>>4)*10); }

typedef struct { uint8_t h,m,s,day,month; uint16_t year; } rtc_t;

static rtc_t rtc_read_raw(void){
    rtc_wait_ready();
    rtc_t t;
    t.s=cmos_read(RTC_SEC); t.m=cmos_read(RTC_MIN);
    t.h=cmos_read(RTC_HOUR); t.day=cmos_read(RTC_DAY);
    t.month=cmos_read(RTC_MONTH); t.year=cmos_read(RTC_YEAR);
    uint8_t rb=cmos_read(RTC_REGB);
    int bin=(rb&0x04), h24=(rb&0x02);
    if(!bin){
        uint8_t raw_h=t.h;
        int pm=(raw_h&0x80)!=0;
        t.s=bcd2bin(t.s); t.m=bcd2bin(t.m);
        t.h=bcd2bin(raw_h&0x7F);
        t.day=bcd2bin(t.day); t.month=bcd2bin(t.month);
        t.year=bcd2bin((uint8_t)t.year);
        if(!h24) t.h = (t.h%12)+(pm?12:0);
    } else {
        if(!h24){ int pm=(t.h&0x80)!=0; t.h=(t.h&0x7F)%12+(pm?12:0); }
    }
    if(t.year<100) t.year+=2000;
    return t;
}

void rtc_get_time(uint8_t *h, uint8_t *m, uint8_t *s){
    rtc_t a,b; int tries=5;
    do{ a=rtc_read_raw(); b=rtc_read_raw(); tries--; }
    while(tries>0&&(a.h!=b.h||a.m!=b.m||a.s!=b.s));
    *h=a.h; *m=a.m; *s=a.s;
}
void rtc_get_date(uint8_t *day, uint8_t *month, uint16_t *year){
    rtc_t t=rtc_read_raw(); *day=t.day; *month=t.month; *year=t.year;
}
void rtc_time_str(char *buf){
    uint8_t h,m,s; rtc_get_time(&h,&m,&s);
    buf[0]='0'+h/10; buf[1]='0'+h%10; buf[2]=':';
    buf[3]='0'+m/10; buf[4]='0'+m%10; buf[5]=':';
    buf[6]='0'+s/10; buf[7]='0'+s%10; buf[8]=0;
}

/* "DD/MM/YY\0" into buf[9] */
void rtc_date_str(char *buf){
    uint8_t d,m; uint16_t y;
    rtc_get_date(&d,&m,&y);
    buf[0]='0'+d/10;  buf[1]='0'+d%10;  buf[2]='/';
    buf[3]='0'+m/10;  buf[4]='0'+m%10;  buf[5]='/';
    /* 2-digit year */
    buf[6]='0'+(y/10)%10; buf[7]='0'+y%10; buf[8]=0;
}
