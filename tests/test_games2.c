/* ════════════════════════════════════════════════════════════════════
   test_games2.c -- the logic under the 0.9.5.1 games and apps.

   The screens are checked by looking at them in QEMU; what can be
   wrong without looking wrong is the arithmetic. So:

     2048        a line slides and merges the way the game's rules say
                 (each tile merges once per move, toward the move)
     Tic-Tac-Toe the "perfect" computer never loses -- every game X can
                 play against it, all of them, is walked
     Converter   known conversions come out exact, the parser and
                 printer round-trip, and the 64-by-32 long division the
                 kernel uses instead of libgcc agrees with the C library
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../include/types.h"
#include "../include/vga.h"
#include "../include/theme.h"

/* ── stubs: nothing here draws ─────────────────────────────────────── */
int  vga_cols(void){ return 80; }
int  vga_rows(void){ return 25; }
void vga_putchar_at(char c,int x,int y,vga_color_t f,vga_color_t b){ (void)c;(void)x;(void)y;(void)f;(void)b; }
void vga_begin_frame(void){}
void vga_end_frame(void){}
void vga_flush(void){}
static theme_t th;
theme_t *ui_theme(void){ return &th; }
void ui_sel_colors(vga_color_t *f, vga_color_t *b){ *f=VGA_BLACK; *b=VGA_CYAN; }
void ui_kit_begin(const char *t,const char *k){ (void)t;(void)k; }
void ui_kit_footer(const char *k){ (void)k; }
void ui_kit_panel(int x,int y,int w,int h,const char *t){ (void)x;(void)y;(void)w;(void)h;(void)t; }
void ui_kit_heading(int x,int y,const char *s){ (void)x;(void)y;(void)s; }
void ui_kit_row(int x,int y,int w,int s,const char *t){ (void)x;(void)y;(void)w;(void)s;(void)t; }
int  keyboard_getchar(void){ return -1; }
int  keyboard_waitchar(void){ return 'q'; }
void sleep_ms(uint32_t ms){ (void)ms; }
uint32_t get_ticks(void){ return 12345; }
void rtc_get_time(uint8_t *h,uint8_t *m,uint8_t *s){ *h=1;*m=2;*s=3; }
void speaker_beep(uint32_t f,uint32_t ms){ (void)f;(void)ms; }
void speaker_tone(uint32_t f){ (void)f; }          /* v0.9.6 */
void speaker_off(void){}
void kpoll_sleep_ms(uint32_t ms){ (void)ms; }
void task_set_policy(int p){ (void)p; }
int  vfs_read(const char *p,char *o,uint32_t n){ (void)p;(void)o;(void)n; return -1; }
int  vfs_write(const char *p,const char *d,uint32_t n){ (void)p;(void)d; return (int)n; }
int  vfs_mkdir(const char *p){ (void)p; return 0; }
const char *current_username(void){ return "kyan"; }

#include "../shell/games2.c"
#include "../shell/apps2.c"
/* v0.9.6.3: shell/uimouse.c is not linked here */
void um_set_mode(int m){ (void)m; }

static int total, fails;
static void ck(const char *w, int ok, const char *d){
    total++;
    if(ok) printf("    ok   %s\n", w);
    else { fails++; printf("    FAIL %s  [%s]\n", w, d ? d : ""); }
}

static int slide(uint32_t a,uint32_t b,uint32_t c,uint32_t d, uint32_t e0,uint32_t e1,uint32_t e2,uint32_t e3){
    uint32_t v[4]={a,b,c,d}; uint32_t *p[4]={&v[0],&v[1],&v[2],&v[3]};
    g2_line(p);
    return v[0]==e0&&v[1]==e1&&v[2]==e2&&v[3]==e3;
}

/* X tries every move at every turn against the perfect computer. */
static int t3_xwins;
static void t3_walk(void){
    char w=t3_winner();
    if(w){ if(w=='X') t3_xwins++; return; }
    for(int i=0;i<9;i++){
        if(t3[i]!=' ') continue;
        char saved[9]; memcpy(saved,t3,9);
        t3[i]='X';
        if(!t3_winner()) t3_cpu(1);
        t3_walk();
        memcpy(t3,saved,9);
    }
}

static int conv(int cat, int from, int to, const char *in, const char *want, char *got){
    int64_t v;
    if(!parse_micro(in,&v)){ strcpy(got,"(parse failed)"); return 0; }
    fmt_micro(from_base(&cats[cat],to,to_base(&cats[cat],from,v)),got);
    return !strcmp(got,want);
}

int main(void){
    printf("\n  2048\n");
    ck("2 2 . . slides to 4",                 slide(2,2,0,0, 4,0,0,0), 0);
    ck("2 2 2 2 makes 4 4, not 8",            slide(2,2,2,2, 4,4,0,0), 0);
    ck("4 4 8 . makes 8 8, the new 8 does not merge again", slide(4,4,8,0, 8,8,0,0), 0);
    ck(". 2 . 2 closes the gaps and merges",  slide(0,2,0,2, 4,0,0,0), 0);
    ck("2 4 2 4 does not move",               slide(2,4,2,4, 2,4,2,4), 0);
    { uint32_t v[4]={2,4,2,4}; uint32_t *p[4]={&v[0],&v[1],&v[2],&v[3]};
      ck("and says so (no new tile)", g2_line(p)==0, 0); }
    g2_score=0; slide(8,8,16,16, 16,32,0,0);
    ck("a merge scores the new tile's value", g2_score==48, 0);
    {
        uint32_t full[4][4]={{2,4,2,4},{4,2,4,2},{2,4,2,4},{4,2,4,2}};
        memcpy(g2,full,sizeof g2);
        ck("a full board with no pairs is game over", !g2_can_move(), 0);
        g2[3][3]=4;
        ck("one pair left is not", g2_can_move(), 0);
        memset(g2,0,sizeof g2); g2[0][3]=2; g2[0][0]=2;
        g2_move(1);
        ck("moving right merges toward the right edge", g2[0][3]==4 && g2[0][0]==0, 0);
        memset(g2,0,sizeof g2); g2[3][1]=2; g2[0][1]=2;
        g2_move(2);
        ck("moving up merges toward the top", g2[0][1]==4 && g2[3][1]==0, 0);
    }

    printf("\n  Tic-Tac-Toe\n");
    for(int i=0;i<9;i++) t3[i]=' ';
    t3_xwins=0; t3_walk();
    ck("the perfect computer never loses, X moving first", t3_xwins==0, 0);
    for(int i=0;i<9;i++) t3[i]=' ';
    t3_cpu(1); t3_xwins=0; t3_walk();
    ck("nor moving first itself", t3_xwins==0, 0);
    memcpy(t3,"XX OO    ",9);
    t3_cpu(1);
    ck("it takes a win when one is there", t3[5]=='O', 0);
    memcpy(t3,"XX  O    ",9);
    t3_cpu(1);
    ck("and blocks yours", t3[2]=='O', 0);

    printf("\n  Converter\n");
    char got[48];
    ck("100 Celsius is 212 Fahrenheit",  conv(2,0,1,"100","212",got), got);
    ck("-40 Celsius is -40 Fahrenheit",  conv(2,0,1,"-40","-40",got), got);
    ck("0 Celsius is 273.15 Kelvin",     conv(2,0,2,"0","273.15",got), got);
    ck("98.6 Fahrenheit is 37 Celsius",  conv(2,1,0,"98.6","37",got), got);
    ck("1 mile is 1609.344 metres",      conv(0,7,2,"1","1609.344",got), got);
    ck("1 inch is 2.54 centimetres",     conv(0,4,1,"1","2.54",got), got);
    ck("1 GB is 1073741824 bytes",       conv(3,3,0,"1","1073741824",got), got);
    ck("1 byte is 8 bits",               conv(3,0,4,"1","8",got), got);
    ck("1 kg is 2.204622 pounds",        conv(1,1,4,"1","2.204622",got), got);
    ck("36 km/h is 10 m/s",              conv(4,0,1,"36","10",got), got);
    ck("1 week is 168 hours",            conv(5,4,2,"1","168",got), got);
    {
        int64_t v;
        ck("garbage does not parse", !parse_micro("12x",&v) && !parse_micro("",&v) && !parse_micro("-",&v), 0);
        parse_micro("3.75",&v); fmt_micro(v,got);
        ck("3.75 round-trips", !strcmp(got,"3.75"), got);
        parse_micro("-0.000001",&v); fmt_micro(v,got);
        ck("a millionth round-trips", !strcmp(got,"-0.000001"), got);
    }
    {
        int ok=1;
        uint64_t ns[]={0,1,999999,1000000,0xFFFFFFFFull,0x123456789ABCDEFull,0xFFFFFFFFFFFFFFFFull};
        uint32_t ds[]={1,3,10,1000000,0xFFFFFFFFu,7};
        for(unsigned i=0;i<sizeof ns/sizeof ns[0];i++) for(unsigned j=0;j<sizeof ds/sizeof ds[0];j++){
            uint32_t r; uint64_t q=udiv64(ns[i],ds[j],&r);
            if(q!=ns[i]/ds[j] || r!=(uint32_t)(ns[i]%ds[j])) ok=0;
        }
        ck("the kernel's long division agrees with the C library's", ok, 0);
    }

    printf("\n  Stopwatch\n");
    fmt_time(123456,got,1);
    ck("123456 hundredths read 00:20:34.56", !strcmp(got,"00:20:34.56"), got);
    fmt_time(5999,got,0);
    ck("59.99 seconds read 00:59.99", !strcmp(got,"00:59.99"), got);
    ck("big digits: every digit has a glyph", glyph('0')==0 && glyph('9')==9 && glyph(':')==10 && glyph('x')<0, 0);

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
