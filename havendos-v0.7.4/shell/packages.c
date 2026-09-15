/*
 * packages.c — HavenDOS v0.7.0 built-in package content
 *
 * Every package is self-contained here. When bpkg installs a package
 * and the network download fails (or TLS isn't available yet), it falls
 * back to pkg_install_builtin() which runs the package directly from ROM.
 *
 * Packages:
 *   ascii-art    — big ASCII banner text renderer
 *   fortune      — random quotes / jokes (50 built-in)
 *   paint        — keyboard ASCII art canvas, save to file
 *   music        — PC speaker melodies (Mario, Tetris, etc.)
 *   calendar     — monthly calendar, highlights today
 *   quiz         — multiple-choice trivia game with scoring
 *   weather      — ASCII art weather (placeholder until TLS lands)
 *   sysinfo-plus — deep system info panel
 *   hacker       — retro "hacking" terminal animation
 *   boot-lang    — enable BOOT language IDE (already compiled in)
 */

#include "../include/vga.h"
#include "../include/string.h"
#include "../include/types.h"
#include "../include/errors.h"

/* Forward declarations for RTC and PMM */
extern void     rtc_get_time(uint8_t*h,uint8_t*m,uint8_t*s);
extern void     rtc_get_date(uint8_t*day,uint8_t*month,uint16_t*year);
extern uint32_t pmm_total_blocks(void);
extern uint32_t pmm_free_blocks(void);

extern int  vfs_write(const char*, const char*, uint32_t);
extern int  vfs_read (const char*, char*, uint32_t);
extern void sleep_ms(uint32_t);
extern int  keyboard_getchar(void);
extern int  keyboard_waitchar(void);
extern void speaker_beep(uint32_t freq, uint32_t ms);
extern void utoa(uint32_t n, char *buf, int base);
extern void itoa(int32_t  n, char *buf, int base);
/* rtc seconds via rtc_get_time */

/* ─────────────────────────────────────────────────────────────── */
/* ASCII-ART                                                        */
/* ─────────────────────────────────────────────────────────────── */

/* 5-row tall ASCII font — uppercase + digits + space */
static const char *ascii_font[38][5] = {
    /* A */ {"  #  ","  ## "," #  #","#####","#   #"},
    /* B */ {"#### ","#   #","#### ","#   #","#### "},
    /* C */ {" ####","#    ","#    ","#    "," ####"},
    /* D */ {"#### ","#   #","#   #","#   #","#### "},
    /* E */ {"#####","#    ","###  ","#    ","#####"},
    /* F */ {"#####","#    ","###  ","#    ","#    "},
    /* G */ {" ####","#    ","#  ##","#   #"," ####"},
    /* H */ {"#   #","#   #","#####","#   #","#   #"},
    /* I */ {"#####","  #  ","  #  ","  #  ","#####"},
    /* J */ {"#####","   # ","   # ","#  # "," ##  "},
    /* K */ {"#  # ","# #  ","##   ","# #  ","#  # "},
    /* L */ {"#    ","#    ","#    ","#    ","#####"},
    /* M */ {"#   #","## ##","# # #","#   #","#   #"},
    /* N */ {"#   #","##  #","# # #","#  ##","#   #"},
    /* O */ {" ### ","#   #","#   #","#   #"," ### "},
    /* P */ {"#### ","#   #","#### ","#    ","#    "},
    /* Q */ {" ### ","#   #","# # #","#  ##"," ####"},
    /* R */ {"#### ","#   #","#### ","# #  ","#  # "},
    /* S */ {" ####","#    "," ### ","    #","#### "},
    /* T */ {"#####","  #  ","  #  ","  #  ","  #  "},
    /* U */ {"#   #","#   #","#   #","#   #"," ### "},
    /* V */ {"#   #","#   #","#   #"," # # ","  #  "},
    /* W */ {"#   #","#   #","# # #","## ##","#   #"},
    /* X */ {"#   #"," # # ","  #  "," # # ","#   #"},
    /* Y */ {"#   #"," # # ","  #  ","  #  ","  #  "},
    /* Z */ {"#####","   # ","  #  "," #   ","#####"},
    /* 0 */ {" ### ","#  ##","# # #","##  #"," ### "},
    /* 1 */ {"  #  "," ##  ","  #  ","  #  ","#####"},
    /* 2 */ {" ### ","#   #","  ## "," #   ","#####"},
    /* 3 */ {"#### ","    #","  ## ","    #","#### "},
    /* 4 */ {"#  # ","#  # ","#####","   # ","   # "},
    /* 5 */ {"#####","#    ","#### ","    #","#### "},
    /* 6 */ {" ### ","#    ","#### ","#   #"," ### "},
    /* 7 */ {"#####","   # ","  #  "," #   ","#    "},
    /* 8 */ {" ### ","#   #"," ### ","#   #"," ### "},
    /* 9 */ {" ### ","#   #"," ####","    #"," ### "},
    /* SP*/ {"     ","     ","     ","     ","     "},
    /* ! */ {"  #  ","  #  ","  #  ","     ","  #  "},
};

static int ascii_char_idx(char c) {
    if(c>='A'&&c<='Z') return c-'A';
    if(c>='a'&&c<='z') return c-'a';
    if(c>='0'&&c<='9') return 26+(c-'0');
    if(c==' ')          return 36;
    if(c=='!')          return 37;
    return 36;
}

void pkg_ascii_art(const char *text)
{
    /* Clear area */
    for(int y=4;y<12;y++)
        for(int x=0;x<80;x++)
            vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);

    int len = (int)strlen(text);
    if(len > 12) len = 12;   /* max 12 chars fit at 6px wide */

    /* Pick a colour cycle */
    vga_color_t cols[] = {VGA_LIGHT_CYAN, VGA_LIGHT_GREEN,
                          VGA_LIGHT_RED,  VGA_YELLOW,
                          VGA_LIGHT_MAGENTA};

    for(int row=0;row<5;row++) {
        int cx = (80 - len*7) / 2;
        if(cx < 0) cx = 0;
        for(int ci=0;ci<len;ci++) {
            int idx = ascii_char_idx(text[ci]);
            const char *r = ascii_font[idx][row];
            vga_color_t col = cols[ci % 5];
            for(int k=0;r[k];k++) {
                if(r[k]!=' ')
                    vga_putchar_at(r[k], cx+k, 5+row, col, VGA_BLACK);
            }
            cx += 6;
        }
    }

    vga_puts_at("ascii-art  --  HavenDOS v0.7.0",24,11,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("Usage: ascii-art <TEXT>  (max 12 chars)",20,12,VGA_DARK_GREY,VGA_BLACK);
}

/* ─────────────────────────────────────────────────────────────── */
/* FORTUNE                                                          */
/* ─────────────────────────────────────────────────────────────── */

static const char *fortune_lines[] = {
    "The best error message is the one that never shows up.",
    "It works on my machine.",
    "Have you tried turning it off and on again?",
    "There are only 10 types of people: those who understand binary and those who don't.",
    "Real programmers count from 0.",
    "A bug is just an undocumented feature.",
    "Debugging: being the detective in a crime where you are also the criminal.",
    "It's not a bug — it's a feature request from the future.",
    "The code you write today is the legacy code you debug tomorrow.",
    "Why do programmers prefer dark mode? Because light attracts bugs.",
    "I don't always test my code, but when I do, I do it in production.",
    "99 little bugs in the code... 99 little bugs... take one down, patch it around... 127 little bugs in the code.",
    "First, solve the problem. Then write the code.",
    "Simplicity is the soul of efficiency.",
    "Walking on water and developing software from a spec are easy — if both are frozen.",
    "The best thing about a boolean is even if you are wrong, you are only off by a bit.",
    "Documentation is like sex: when it is good, it is very, very good; and when it is bad, it is better than nothing.",
    "Programming is like writing a book... except if you miss a single comma on page 126 the whole thing explodes.",
    "Software is like entropy: it is difficult to grasp, weighs nothing, and obeys the second law of thermodynamics.",
    "Always code as if the guy who ends up maintaining your code will be a violent psychopath who knows where you live.",
    "HavenDOS: Where the bugs are features and the features are documented.",
    "TechHaven Studios: Building the future, one kernel panic at a time.",
    "The computer was born to solve problems that did not exist before.",
    "Any fool can write code that a computer can understand. Good programmers write code that humans can understand.",
    "Premature optimization is the root of all evil.",
    "The most disastrous thing you can ever learn is your first programming language.",
    "There is no place like 127.0.0.1.",
    "To understand recursion, you must first understand recursion.",
    "A computer lets you make more mistakes faster than any invention in human history.",
    "In theory, theory and practice are the same. In practice, they are not.",
    "One man's constant is another man's variable.",
    "The function of good software is to make the complex appear simple.",
    "Software testing proves the presence of bugs, not their absence.",
    "Every great developer you know got there by solving problems they were unqualified to solve.",
    "Copy-paste is a design pattern.",
    "It compiles. Ship it.",
    "undefined is not a function.",
    "Works on my machine — ship the machine.",
    "The best performance improvement is the transition from non-working to working.",
    "If debugging is the process of removing bugs, then programming must be the process of putting them in.",
    "HavenDOS runs on hopes, dreams, and carefully aligned stack frames.",
    "rm -rf / — the one command to rule them all.",
    "sudo make me a sandwich.",
    "Your code is perfect. The computer is wrong.",
    "BIOS: Basic Irreplaceable Obstruction to Software.",
    "A kernel panic a day keeps the users away.",
    "In HavenDOS, WE are the scheduler.",
    "FAT16: Flat And Trustworthy, mostly.",
    "TCP: Totally Correct Protocol. Except for the checksum byte order.",
    "HavenDOS v0.7.0 — Now with 43% fewer catastrophic failures.",
};
#define FORTUNE_COUNT 50

void pkg_fortune(void)
{
    uint8_t _fh=0,_fm=0,_fs=0;
    rtc_get_time(&_fh,&_fm,&_fs);
    uint32_t seed = (uint32_t)_fh*3600+(uint32_t)_fm*60+(uint32_t)_fs+1;
    int idx = (int)(seed % FORTUNE_COUNT);
    const char *f = fortune_lines[idx];

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("\n  \"");
    vga_puts(f);
    vga_puts("\"\n\n");
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_puts("     -- fortune, HavenDOS v0.7.0\n\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* ─────────────────────────────────────────────────────────────── */
/* PAINT                                                            */
/* ─────────────────────────────────────────────────────────────── */

static char  paint_canvas[22][79];
static int   paint_cx=0, paint_cy=0;
static char  paint_char='#';
static vga_color_t paint_col=VGA_LIGHT_GREEN;

static void paint_redraw_all(void)
{
    for(int y=0;y<22;y++)
        for(int x=0;x<78;x++) {
            char c=paint_canvas[y][x];
            vga_putchar_at(c?c:' ',x+1,y+2,
                           c?VGA_LIGHT_GREEN:VGA_DARK_GREY,VGA_BLACK);
        }
}

static void paint_status(void)
{
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,0,VGA_BLACK,VGA_DARK_GREY);
    vga_puts_at("PAINT  Arrows=Move  Space=Draw  C=Clear  S=Save  Q=Quit",1,0,VGA_WHITE,VGA_DARK_GREY);
    char pos[16]="(00,00)";
    pos[1]='0'+(paint_cx/10); pos[2]='0'+(paint_cx%10);
    pos[4]='0'+(paint_cy/10); pos[5]='0'+(paint_cy%10);
    vga_puts_at(pos,70,0,VGA_LIGHT_CYAN,VGA_DARK_GREY);
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,1,VGA_BLACK,VGA_DARK_GREY);
    vga_puts_at("Chars: # * . + - | @ % 0   1-9=Pick colour",1,1,VGA_DARK_GREY,VGA_DARK_GREY);
}

void pkg_paint(void)
{
    /* Clear canvas */
    for(int y=0;y<22;y++) for(int x=0;x<78;x++) paint_canvas[y][x]=0;
    paint_cx=0; paint_cy=0; paint_char='#';

    /* Draw border */
    for(int y=0;y<25;y++) for(int x=0;x<80;x++)
        vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);
    for(int x=0;x<80;x++) vga_putchar_at('-',x,24,VGA_DARK_GREY,VGA_BLACK);
    for(int y=2;y<24;y++) {
        vga_putchar_at('|',0, y,VGA_DARK_GREY,VGA_BLACK);
        vga_putchar_at('|',79,y,VGA_DARK_GREY,VGA_BLACK);
    }

    paint_status();
    paint_redraw_all();

    static const char char_cycle[]="# * . + - | @ % 0 ";
    int char_idx=0;

    while(1) {
        /* Draw cursor */
        char cc=paint_canvas[paint_cy][paint_cx];
        vga_putchar_at(cc?cc:'_',paint_cx+1,paint_cy+2,VGA_YELLOW,VGA_DARK_GREY);

        int k=keyboard_waitchar();

        /* Restore cell */
        cc=paint_canvas[paint_cy][paint_cx];
        vga_putchar_at(cc?cc:' ',paint_cx+1,paint_cy+2,
                       cc?VGA_LIGHT_GREEN:VGA_DARK_GREY,VGA_BLACK);

        if(k==0x148&&paint_cy>0)  paint_cy--;
        else if(k==0x150&&paint_cy<21) paint_cy++;
        else if(k==0x14B&&paint_cx>0)  paint_cx--;
        else if(k==0x14D&&paint_cx<77) paint_cx++;
        else if(k==' ') {
            paint_canvas[paint_cy][paint_cx]=paint_char;
            vga_putchar_at(paint_char,paint_cx+1,paint_cy+2,VGA_LIGHT_GREEN,VGA_BLACK);
        }
        else if(k=='c'||k=='C') {
            for(int y=0;y<22;y++) for(int x=0;x<78;x++) paint_canvas[y][x]=0;
            paint_redraw_all();
        }
        else if(k>='1'&&k<='9') {
            /* Switch character */
            static const char chars[]="#+*.-|@%0";
            paint_char=chars[k-'1'];
        }
        else if(k=='s'||k=='S') {
            /* Save to HOME/PAINT.TXT */
            char buf[22*80]; int bp=0;
            for(int y=0;y<22;y++){
                for(int x=0;x<78;x++) buf[bp++]=paint_canvas[y][x]?paint_canvas[y][x]:' ';
                buf[bp++]='\n';
            }
            vfs_write("HOME/PAINT.TXT",buf,(uint32_t)bp);
            vga_puts_at("Saved to HOME/PAINT.TXT",28,24,VGA_LIGHT_GREEN,VGA_BLACK);
            sleep_ms(800);
            for(int x=0;x<80;x++) vga_putchar_at('-',x,24,VGA_DARK_GREY,VGA_BLACK);
        }
        else if(k=='q'||k=='Q'||k==27) break;

        (void)char_idx; (void)char_cycle; (void)paint_col;
    }

    /* Clear screen on exit */
    for(int y=0;y<25;y++) for(int x=0;x<80;x++)
        vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);
}

/* ─────────────────────────────────────────────────────────────── */
/* MUSIC (PC speaker)                                               */
/* ─────────────────────────────────────────────────────────────── */

typedef struct { uint32_t freq; uint32_t ms; } note_t;

static const note_t melody_mario[] = {
    {660,100},{660,100},{0,100},{660,100},{0,100},{520,100},{660,100},
    {0,100},{784,100},{0,300},{392,100},{0,300},
    {524,100},{0,200},{392,100},{0,200},{330,100},{0,200},
    {440,100},{0,100},{494,100},{0,100},{466,100},{0,100},{440,100},{0,100},
    {392,130},{660,130},{784,130},{880,100},{0,100},{698,100},{784,100},
    {0,100},{660,100},{0,100},{524,100},{0,100},{330,100},{0,100},{440,100},
    {0,0}
};

static const note_t melody_tetris[] = {
    {659,150},{494,75},{523,75},{587,150},{523,75},{494,75},
    {440,150},{440,75},{523,75},{659,150},{587,75},{523,75},
    {494,150},{494,75},{523,75},{587,150},{659,150},
    {523,150},{440,150},{440,150},{0,150},
    {587,150},{698,75},{880,150},{784,75},{698,75},
    {659,150},{659,75},{523,75},{659,150},{587,75},{523,75},
    {494,150},{494,75},{523,75},{587,150},{659,150},
    {523,150},{440,150},{440,150},{0,0}
};

static const note_t melody_imperial[] = {
    {440,500},{440,500},{440,500},{349,350},{523,150},
    {440,500},{349,350},{523,150},{440,1000},
    {659,500},{659,500},{659,500},{698,350},{523,150},
    {415,500},{349,350},{523,150},{440,1000},
    {880,500},{440,350},{440,150},{880,500},{830,350},{784,150},
    {740,150},{698,150},{740,300},{0,150},{466,300},{622,500},
    {587,350},{554,150},{523,150},{494,150},{523,300},{0,0}
};

typedef struct { const char *name; const note_t *melody; } song_t;
static const song_t songs[] = {
    {"Super Mario Bros Theme", melody_mario},
    {"Tetris Theme (Korobeiniki)", melody_tetris},
    {"Imperial March", melody_imperial},
};
#define SONG_COUNT 3

void pkg_music(void)
{
    for(int y=0;y<25;y++) for(int x=0;x<80;x++)
        vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);

    for(int x=0;x<80;x++) vga_putchar_at(' ',x,0,VGA_BLACK,VGA_DARK_GREY);
    vga_puts_at(" MUSIC PLAYER  --  HavenDOS v0.7.0",0,0,VGA_WHITE,VGA_DARK_GREY);
    vga_puts_at("PC Speaker",69,0,VGA_LIGHT_CYAN,VGA_DARK_GREY);

    vga_puts_at("Select a song:",32,4,VGA_LIGHT_CYAN,VGA_BLACK);
    for(int i=0;i<SONG_COUNT;i++) {
        vga_putchar_at('0'+i+1,30,6+i,VGA_YELLOW,VGA_BLACK);
        vga_puts_at(". ",31,6+i,VGA_DARK_GREY,VGA_BLACK);
        vga_puts_at(songs[i].name,33,6+i,VGA_LIGHT_GREY,VGA_BLACK);
    }
    vga_puts_at("Q. Quit",30,6+SONG_COUNT+1,VGA_DARK_GREY,VGA_BLACK);

    vga_puts_at("Press a number to play  |  Any key to stop",18,22,VGA_DARK_GREY,VGA_BLACK);

    while(1) {
        int k=keyboard_waitchar();
        if(k=='q'||k=='Q'||k==27) break;
        int si=k-'1';
        if(si<0||si>=SONG_COUNT) continue;

        /* Clear status area */
        for(int x=0;x<80;x++) vga_putchar_at(' ',x,16,VGA_BLACK,VGA_BLACK);
        vga_puts_at("Now playing: ",10,16,VGA_LIGHT_GREY,VGA_BLACK);
        vga_puts_at(songs[si].name,23,16,VGA_LIGHT_CYAN,VGA_BLACK);
        vga_puts_at("Press any key to stop",29,18,VGA_DARK_GREY,VGA_BLACK);

        const note_t *m=songs[si].melody;
        for(int ni=0;m[ni].ms;ni++) {
            if(keyboard_getchar()!=0) break;
            if(m[ni].freq) speaker_beep(m[ni].freq,m[ni].ms);
            else           sleep_ms(m[ni].ms);
        }
        speaker_beep(0,0);

        for(int x=0;x<80;x++) vga_putchar_at(' ',x,16,VGA_BLACK,VGA_BLACK);
        for(int x=0;x<80;x++) vga_putchar_at(' ',x,18,VGA_BLACK,VGA_BLACK);
        vga_puts_at("Done.",37,16,VGA_DARK_GREY,VGA_BLACK);
    }

    for(int y=0;y<25;y++) for(int x=0;x<80;x++)
        vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);
}

/* ─────────────────────────────────────────────────────────────── */
/* CALENDAR                                                         */
/* ─────────────────────────────────────────────────────────────── */

extern void rtc_get_time(uint8_t*h,uint8_t*m,uint8_t*s);
extern void rtc_get_date(uint8_t*day,uint8_t*month,uint16_t*year);

static int days_in_month(int m, int y){
    static const int days[]={0,31,28,31,30,31,30,31,31,30,31,30,31};
    if(m==2&&((y%4==0&&y%100!=0)||y%400==0)) return 29;
    return days[m];
}
static int day_of_week(int y,int m,int d){ /* Zeller-ish, returns 0=Sun */
    if(m<3){m+=12;y--;}
    int k=y%100,j=y/100;
    int h2=( d + (13*(m+1))/5 + k + k/4 + j/4 - 2*j )%7;
    return ((h2+5)%7+1)%7;  /* 0=Sun */
}

void pkg_calendar(void)
{
    uint8_t _h=0,_mi=0,_se=0,_dy=1,_mo=1; uint16_t _yr=2024;
    rtc_get_time(&_h,&_mi,&_se);
    rtc_get_date(&_dy,&_mo,&_yr);
    int yr=(int)_yr,mo=(int)_mo,dy=(int)_dy,hr=(int)_h,mi=(int)_mi; (void)_se;

    for(int y=0;y<25;y++) for(int x=0;x<80;x++)
        vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);

    static const char *months[]={"","January","February","March","April","May",
                                  "June","July","August","September",
                                  "October","November","December"};
    static const char *dow="Su Mo Tu We Th Fr Sa";

    /* Header */
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,0,VGA_BLACK,VGA_DARK_GREY);
    char hdr[32]="   "; int hi=3;
    const char *mn=months[mo];
    while(*mn) hdr[hi++]=*mn++;
    hdr[hi++]=' ';
    char ys[8]; utoa((uint32_t)yr,ys,10);
    for(int i=0;ys[i];i++) hdr[hi++]=ys[i];
    hdr[hi]=0;
    vga_puts_at(" CALENDAR",0,0,VGA_WHITE,VGA_DARK_GREY);
    vga_puts_at(hdr,30,0,VGA_LIGHT_CYAN,VGA_DARK_GREY);

    /* Day-of-week row */
    vga_puts_at(dow, 28, 3, VGA_DARK_GREY, VGA_BLACK);

    /* Grid */
    int dow_start = day_of_week(yr,mo,1);
    int dim = days_in_month(mo,yr);
    int col=dow_start, row=0;

    for(int d=1;d<=dim;d++){
        int cx = 28 + col*3;
        int cy = 4 + row;
        char ds[4]; ds[0]=' ';
        utoa((uint32_t)d, ds+(d<10?1:0), 10);
        ds[2]=0;
        vga_color_t fc = (d==dy) ? VGA_BLACK : (col==0||col==6) ? VGA_LIGHT_RED : VGA_LIGHT_GREY;
        vga_color_t bc = (d==dy) ? VGA_LIGHT_CYAN : VGA_BLACK;
        vga_puts_at(ds, cx, cy, fc, bc);
        col++;
        if(col==7){col=0;row++;}
    }

    /* Time */
    char ts[16]; int thi=0;
    char tmp[8];
    utoa((uint32_t)hr,tmp,10); if(hr<10){ts[thi++]='0';} for(int i=0;tmp[i];i++) ts[thi++]=tmp[i];
    ts[thi++]=':';
    utoa((uint32_t)mi,tmp,10); if(mi<10){ts[thi++]='0';} for(int i=0;tmp[i];i++) ts[thi++]=tmp[i];
    ts[thi]=0;
    vga_puts_at(ts,55,0,VGA_YELLOW,VGA_DARK_GREY);

    vga_puts_at("Press any key to exit",29,22,VGA_DARK_GREY,VGA_BLACK);
    keyboard_waitchar();

    for(int y=0;y<25;y++) for(int x=0;x<80;x++)
        vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);
}

/* ─────────────────────────────────────────────────────────────── */
/* QUIZ                                                             */
/* ─────────────────────────────────────────────────────────────── */

typedef struct {
    const char *q;
    const char *a[4];
    int correct;  /* 0-3 */
} quiz_q_t;

static const quiz_q_t quiz_questions[] = {
    {"What does CPU stand for?",
     {"Central Processing Unit","Computer Personal Unit","Central Program Utility","Core Processing Unit"},0},
    {"How many bits in a byte?",
     {"4","16","8","32"},2},
    {"What is 0xFF in decimal?",
     {"128","255","256","127"},1},
    {"Which layer does TCP operate at?",
     {"Network","Physical","Transport","Application"},2},
    {"What does RAM stand for?",
     {"Random Access Memory","Read Access Module","Rapid Array Memory","Random Allocation Module"},0},
    {"What is the default HTTP port?",
     {"443","21","8080","80"},3},
    {"HavenDOS is written in which language?",
     {"Assembly only","Rust","C and Assembly","C++ and Python"},2},
    {"What does BIOS stand for?",
     {"Basic Input Output System","Binary Interface Operating System","Base Input Output Signal","Boot Interface OS"},0},
    {"Which filesystem does HavenDOS use on disk?",
     {"NTFS","FAT16","ext4","FAT32"},1},
    {"What is the loopback IP address?",
     {"192.168.0.1","10.0.0.1","0.0.0.0","127.0.0.1"},3},
    {"What does DNS stand for?",
     {"Domain Name System","Data Network Service","Dynamic Name Server","Digital Node System"},0},
    {"In binary, what is 1010?",
     {"8","12","10","6"},2},
};
#define QUIZ_COUNT 12

void pkg_quiz(void)
{
    for(int y=0;y<25;y++) for(int x=0;x<80;x++)
        vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);

    for(int x=0;x<80;x++) vga_putchar_at(' ',x,0,VGA_BLACK,VGA_DARK_GREY);
    vga_puts_at(" HAVENDOS QUIZ  --  12 Questions",0,0,VGA_WHITE,VGA_DARK_GREY);

    uint8_t _qh=0,_qm=0,_qs=0;
    rtc_get_time(&_qh,&_qm,&_qs);
    uint32_t seed=(uint32_t)_qh*3600+(uint32_t)_qm*60+(uint32_t)_qs+1;
    int score=0;
    /* Simple shuffle — pick 8 from 12 */
    int order[12]; for(int i=0;i<12;i++) order[i]=i;
    for(int i=11;i>0;i--){
        int j=(int)(seed%(uint32_t)(i+1)); seed=seed*6364136223846793005ull+1;
        int t=order[i];order[i]=order[j];order[j]=t;
    }
    int total=8;

    for(int qi=0;qi<total;qi++){
        const quiz_q_t *q=&quiz_questions[order[qi]];

        for(int y=2;y<24;y++) for(int x=0;x<80;x++)
            vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);

        /* Progress */
        char prog[24]="Question 0/0";
        prog[9]='1'+qi; prog[11]='0'+total;
        vga_puts_at(prog,60,0,VGA_DARK_GREY,VGA_DARK_GREY);

        /* Score */
        char sc[16]="Score: 0/0";
        sc[7]='0'+score; sc[9]='0'+qi;
        vga_puts_at(sc,2,0,VGA_LIGHT_GREEN,VGA_DARK_GREY);

        vga_puts_at(q->q,4,4,VGA_LIGHT_CYAN,VGA_BLACK);
        static const char *letters[]={"A","B","C","D"};
        for(int ai=0;ai<4;ai++){
            vga_puts_at(letters[ai],8,7+ai*2,VGA_YELLOW,VGA_BLACK);
            vga_puts_at(". ",9,7+ai*2,VGA_DARK_GREY,VGA_BLACK);
            vga_puts_at(q->a[ai],11,7+ai*2,VGA_LIGHT_GREY,VGA_BLACK);
        }
        vga_puts_at("Answer (A-D): ",8,16,VGA_WHITE,VGA_BLACK);

        int ans=-1;
        while(ans<0){
            int k=keyboard_waitchar();
            if(k=='a'||k=='A') ans=0;
            else if(k=='b'||k=='B') ans=1;
            else if(k=='c'||k=='C') ans=2;
            else if(k=='d'||k=='D') ans=3;
            else if(k=='q'||k==27){ goto quiz_done; }
        }

        if(ans==q->correct){
            score++;
            vga_puts_at("  Correct!  ",30,18,VGA_BLACK,VGA_LIGHT_GREEN);
        } else {
            char wrong[48]="  Wrong! Answer: ";
            int wl=(int)strlen(wrong);
            const char *ca=q->a[q->correct];
            while(*ca&&wl<47) wrong[wl++]=*ca++;
            wrong[wl]=0;
            vga_puts_at(wrong,15,18,VGA_BLACK,VGA_LIGHT_RED);
        }
        sleep_ms(1200);
    }

quiz_done:;
    for(int y=2;y<24;y++) for(int x=0;x<80;x++)
        vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);

    vga_puts_at("Quiz Complete!",33,6,VGA_LIGHT_CYAN,VGA_BLACK);

    char final[32]="Score:  / ";
    final[7]='0'+score; final[9]='0'+total;
    vga_puts_at(final,34,8,VGA_WHITE,VGA_BLACK);

    vga_color_t rc = score>=7?VGA_LIGHT_GREEN:score>=4?VGA_YELLOW:VGA_LIGHT_RED;
    const char *rank = score>=10?"Expert!":score>=7?"Great job!":score>=4?"Not bad.":"Keep studying!";
    vga_puts_at(rank,36,10,rc,VGA_BLACK);

    vga_puts_at("Press any key",33,14,VGA_DARK_GREY,VGA_BLACK);
    keyboard_waitchar();

    for(int y=0;y<25;y++) for(int x=0;x<80;x++)
        vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);
}

/* ─────────────────────────────────────────────────────────────── */
/* HACKER (retro terminal animation)                                */
/* ─────────────────────────────────────────────────────────────── */

void pkg_hacker(void)
{
    for(int y=0;y<25;y++) for(int x=0;x<80;x++)
        vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);

    /* Flush any keystrokes left over from the menu selection */
    while(keyboard_getchar() != -1) {}

    static const char *lines[] = {
        "Initializing TechHaven exploit framework v3.1.4...",
        "Loading payload modules................ [OK]",
        "Scanning network topology 10.0.2.0/24..",
        "  Host 10.0.2.2   [GATEWAY]  ports: 22,80,443",
        "  Host 10.0.2.15  [LOCAL]    ports: none",
        "Bypassing firewall rules............... [OK]",
        "Injecting polymorphic shellcode......... [OK]",
        "Escalating privileges.................. [OK]",
        "Accessing mainframe..................... [OK]",
        "Downloading secret files...............",
        "  /etc/shadow ............... [ENCRYPTED]",
        "  /root/launch_codes.txt .... [NOT FOUND]",
        "  /var/log/havendos.log ..... [OK]",
        "Covering tracks........................ [OK]",
        "Installing persistence backdoor........ [OK]",
        "Exfiltrating 1.2 TB of data............",
        "  Progress: ########## 100%",
        "Done. Connection closed.",
        "",
        "         [ THIS IS FAKE - HavenDOS v0.7.0 ]",
        "         [ Just a fun animation. Relax.   ]",
        "",
        "Press any key to exit...",
    };
    int n=(int)(sizeof(lines)/sizeof(lines[0]));

    for(int i=0;i<n;i++){
        sleep_ms(80+(i<8?120:i<16?80:40));
        vga_set_color(i==19||i==20?VGA_LIGHT_RED:
                      i==22?VGA_DARK_GREY:
                      (i%3==0)?VGA_LIGHT_GREEN:VGA_GREEN,VGA_BLACK);
        vga_puts(lines[i]); vga_puts("\n");
        if(keyboard_getchar()!=-1) break;
    }

    vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
    /* Flush again then wait — ensures we wait for a real new keypress */
    while(keyboard_getchar()!=-1) {}
    keyboard_waitchar();
    for(int y=0;y<25;y++) for(int x=0;x<80;x++)
        vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);
}

/* ─────────────────────────────────────────────────────────────── */
/* SYSINFO-PLUS                                                     */
/* ─────────────────────────────────────────────────────────────── */

/* pmm externs declared at file top */
extern uint32_t net_get_ip(void);
extern void     ip_to_str(uint32_t, char*);
extern void     heap_stats(uint32_t*,uint32_t*);

void pkg_sysinfo_plus(void)
{
    for(int y=0;y<25;y++) for(int x=0;x<80;x++)
        vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);

    for(int x=0;x<80;x++) vga_putchar_at(' ',x,0,VGA_BLACK,VGA_DARK_GREY);
    vga_puts_at(" SYSINFO+  --  HavenDOS v0.7.0  --  TechHaven Studios",0,0,VGA_WHITE,VGA_DARK_GREY);

    /* Box */
    for(int x=2;x<78;x++){
        vga_putchar_at('-',x,2,VGA_DARK_GREY,VGA_BLACK);
        vga_putchar_at('-',x,22,VGA_DARK_GREY,VGA_BLACK);
    }
    for(int y=2;y<23;y++){
        vga_putchar_at('|',2,y,VGA_DARK_GREY,VGA_BLACK);
        vga_putchar_at('|',77,y,VGA_DARK_GREY,VGA_BLACK);
    }

    int row=4;
    #define SI_ROW(label,val,col) \
        vga_puts_at(label,5,row,VGA_DARK_GREY,VGA_BLACK); \
        vga_puts_at(val,28,row,col,VGA_BLACK); row++;

    SI_ROW("OS:",             "HavenDOS v0.7.0",           VGA_LIGHT_CYAN)
    SI_ROW("Vendor:",         "TechHaven Studios",          VGA_LIGHT_GREY)
    SI_ROW("Architecture:",   "x86 32-bit (i386)",          VGA_LIGHT_GREY)
    SI_ROW("Boot:",           "GRUB2 / Multiboot1",         VGA_LIGHT_GREY)
    row++;

    /* RAM */
    uint32_t total_pages = pmm_total_blocks();
    uint32_t free_pages  = pmm_free_blocks();
    uint32_t total_mb    = (total_pages*4)/1024;  /* 4KB blocks */
    uint32_t free_mb     = (free_pages*4)/1024;
    uint32_t used_mb     = total_mb > free_mb ? total_mb - free_mb : 0;

    char tmb[16],fmb[16],umb[16];
    utoa(total_mb,tmb,10); strncat(tmb," MB",4);
    utoa(free_mb, fmb,10); strncat(fmb," MB",4);
    utoa(used_mb, umb,10); strncat(umb," MB",4);

    SI_ROW("Total RAM:",      tmb, VGA_LIGHT_GREEN)
    SI_ROW("Used RAM:",       umb, VGA_YELLOW)
    SI_ROW("Free RAM:",       fmb, VGA_LIGHT_GREEN)

    /* Heap */
    uint32_t hfree=0, hused=0;
    heap_stats(&hfree, &hused);
    char hfs[16],hus[16];
    utoa(hfree/1024,hfs,10); strncat(hfs," KB",4);
    utoa(hused/1024,hus,10); strncat(hus," KB",4);
    SI_ROW("Heap used:",      hus, VGA_YELLOW)
    SI_ROW("Heap free:",      hfs, VGA_LIGHT_GREEN)
    row++;

    /* Network */
    char ips[20]="Not configured";
    uint32_t ip=net_get_ip();
    if(ip) ip_to_str(ip,ips);
    SI_ROW("Network IP:",     ips, VGA_LIGHT_CYAN)
    SI_ROW("NIC:",            "RTL8139 / e1000",            VGA_LIGHT_GREY)
    SI_ROW("Stack:",          "TCP/IP IPv4 (HavenDOS)",     VGA_LIGHT_GREY)
    row++;

    /* Filesystem */
    SI_ROW("Filesystem:",     "FAT16 + RAMFS + FAT12",      VGA_LIGHT_GREY)
    SI_ROW("Disk I/O:",       "VirtIO block (polling)",      VGA_LIGHT_GREY)

    vga_puts_at("Press any key to exit",29,23,VGA_DARK_GREY,VGA_BLACK);
    keyboard_waitchar();

    for(int y=0;y<25;y++) for(int x=0;x<80;x++)
        vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);
}

/* ─────────────────────────────────────────────────────────────── */
/* ASCII-ART BANNER (shell command wrapper)                         */
/* ─────────────────────────────────────────────────────────────── */

/* already defined above as pkg_ascii_art() */

/* ─────────────────────────────────────────────────────────────── */
/* Public dispatch                                                   */
/* ─────────────────────────────────────────────────────────────── */

void run_package_app(const char *id, const char *args)
{
    if(strcmp(id,"ascii-art")==0)    { pkg_ascii_art(args[0]?args:"HAVENDOS"); return; }
    if(strcmp(id,"fortune")==0)      { pkg_fortune();    return; }
    if(strcmp(id,"paint")==0)        { pkg_paint();      return; }
    if(strcmp(id,"music")==0)        { pkg_music();      return; }
    if(strcmp(id,"calendar")==0)     { pkg_calendar();   return; }
    if(strcmp(id,"quiz")==0)         { pkg_quiz();       return; }
    if(strcmp(id,"hacker")==0)       { pkg_hacker();     return; }
    if(strcmp(id,"sysinfo-plus")==0) { pkg_sysinfo_plus();return;}
    vga_puts("Package app not found: "); vga_puts(id); vga_puts("\n");
}
