/*
 * toast.c — BOOT OS v0.5.9.3 Notification Bar (fixed)
 * Shows a coloured single-line bar at row 23.
 * Does NOT try to restore the row — just blanks on expire.
 * Row 23 is semi-reserved for notifications in shell context.
 * Desktop redraws naturally cover it on next frame.
 */
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/types.h"

extern uint32_t get_ticks(void);

typedef enum {
    TOAST_INFO    = 0,
    TOAST_SUCCESS = 1,
    TOAST_WARNING = 2,
    TOAST_ERROR   = 3,
} toast_type_t;

#define TOAST_ROW    22          /* Row 22 — safe above status bar */
#define TOAST_EXPIRE 250         /* ~2.5s at 100Hz */

static int          toast_active = 0;
static uint32_t     toast_born   = 0;
static char         toast_msg[72];
static toast_type_t toast_type;

static vga_color_t tbg(toast_type_t t){
    switch(t){
        case TOAST_SUCCESS: return VGA_LIGHT_GREEN;
        case TOAST_WARNING: return VGA_YELLOW;
        case TOAST_ERROR:   return VGA_RED;
        default:            return VGA_CYAN;
    }
}
static vga_color_t tfg(toast_type_t t){
    switch(t){
        case TOAST_SUCCESS: return VGA_BLACK;
        case TOAST_WARNING: return VGA_BLACK;
        case TOAST_ERROR:   return VGA_WHITE;
        default:            return VGA_BLACK;
    }
}
static const char *tpfx(toast_type_t t){
    switch(t){
        case TOAST_SUCCESS: return " [ OK ]  ";
        case TOAST_WARNING: return " [WARN]  ";
        case TOAST_ERROR:   return " [ERR]   ";
        default:            return " [INFO]  ";
    }
}

static void toast_draw(void){
    vga_color_t fg=tfg(toast_type), bg=tbg(toast_type);
    const char *pre=tpfx(toast_type);
    /* Fill row */
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,TOAST_ROW,fg,bg);
    /* Write prefix */
    int x=0;
    for(;pre[x]&&x<79;x++) vga_putchar_at(pre[x],x,TOAST_ROW,fg,bg);
    /* Write message */
    for(int i=0;toast_msg[i]&&x<79;i++,x++)
        vga_putchar_at(toast_msg[i],x,TOAST_ROW,fg,bg);
}

static void toast_blank(void){
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,TOAST_ROW,VGA_BLACK,VGA_BLACK);
}

void toast_push(toast_type_t type, const char *msg){
    toast_active=1;
    toast_born=get_ticks();
    toast_type=type;
    strncpy(toast_msg,msg,71); toast_msg[71]=0;
    toast_draw();
}

void toast_info   (const char *m){ toast_push(TOAST_INFO,   m); }
void toast_success(const char *m){ toast_push(TOAST_SUCCESS,m); }
void toast_warning(const char *m){ toast_push(TOAST_WARNING,m); }
void toast_error  (const char *m){ toast_push(TOAST_ERROR,  m); }

void toast_tick(void){
    if(!toast_active) return;
    if((get_ticks()-toast_born)>=TOAST_EXPIRE){
        toast_active=0;
        toast_blank();
    }
}

void toast_clear(void){
    if(toast_active){ toast_active=0; }
    /* Don't blank — let the screen redraw handle it */
}
