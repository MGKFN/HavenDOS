#include "../include/vga.h"
#include "../include/string.h"
#include "../include/types.h"

extern int      keyboard_haschar(void);
extern int      keyboard_getchar(void);
extern uint32_t get_ticks(void);

static void busy_wait(volatile uint32_t n){ while(n--) __asm__ volatile("nop"); }

/* ---- Simple LCG random number generator ---- */
static uint32_t rng_state = 0xDEADBEEF;
static uint32_t rng(void){
    rng_state = rng_state * 1664525 + 1013904223;
    return rng_state;
}

/* ---- Matrix rain screensaver ---- */
static void screensaver_matrix(void){
    /* Each column has a drop position and speed */
    #define COLS 80
    #define ROWS 25
    int drop[COLS];
    int speed[COLS];
    int active[COLS];

    for(int x=0;x<COLS;x++){
        drop[x]   = (int)(rng()%ROWS);
        speed[x]  = 1+(int)(rng()%4);
        active[x] = (rng()%2==0);
    }

    /* Matrix chars - katakana-ish using ASCII approximations */
    static const char mchars[]="01234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ@#$%&*+-=<>";
    int mlen=strlen(mchars);

    /* Clear to black */
    for(int y=0;y<ROWS;y++)
        for(int x=0;x<COLS;x++)
            vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);

    uint32_t frame=0;
    while(1){
        if(keyboard_haschar()){ keyboard_getchar(); return; }

        for(int x=0;x<COLS;x++){
            if(!active[x]){
                if((int)(rng()%30)==0) active[x]=1;
                continue;
            }
            if(frame%(uint32_t)speed[x]!=0) continue;

            int y=drop[x];
            /* Draw bright head */
            if(y>=0&&y<ROWS)
                vga_putchar_at(mchars[rng()%mlen],x,y,VGA_WHITE,VGA_BLACK);
            /* Draw green trail */
            if(y-1>=0&&y-1<ROWS)
                vga_putchar_at(mchars[rng()%mlen],x,y-1,VGA_LIGHT_GREEN,VGA_BLACK);
            if(y-2>=0&&y-2<ROWS)
                vga_putchar_at(mchars[rng()%mlen],x,y-2,VGA_GREEN,VGA_BLACK);
            if(y-3>=0&&y-3<ROWS)
                vga_putchar_at(mchars[rng()%mlen],x,y-3,VGA_DARK_GREY,VGA_BLACK);
            /* Erase tail */
            if(y-4>=0&&y-4<ROWS)
                vga_putchar_at(' ',x,y-4,VGA_BLACK,VGA_BLACK);

            drop[x]++;
            if(drop[x]>ROWS+4){
                drop[x]=0;
                speed[x]=1+(int)(rng()%4);
                if((int)(rng()%4)==0) active[x]=0;
            }
        }

        /* Overlay screensaver hint */
        vga_puts_at("Press any key to continue...",26,24,VGA_DARK_GREY,VGA_BLACK);

        frame++;
        busy_wait(800000);
    }
}

/* ---- Starfield screensaver ---- */
#define MAX_STARS 60
typedef struct { int x,y,z; char ch; vga_color_t col; } star_t;

static void screensaver_stars(void){
    star_t stars[MAX_STARS];
    for(int i=0;i<MAX_STARS;i++){
        stars[i].x=(int)(rng()%78)+1;
        stars[i].y=(int)(rng()%23)+1;
        stars[i].z=(int)(rng()%4);
        static const char chs[]=".*+o";
        static const vga_color_t cols[]={VGA_DARK_GREY,VGA_LIGHT_GREY,VGA_WHITE,VGA_LIGHT_CYAN};
        stars[i].ch=chs[stars[i].z];
        stars[i].col=cols[stars[i].z];
    }
    for(int y=0;y<25;y++)
        for(int x=0;x<80;x++)
            vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);

    /* Centered title */
    vga_puts_at("* BOOT OS *",34,12,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("Press any key",33,13,VGA_DARK_GREY,VGA_BLACK);

    uint32_t frame=0;
    while(1){
        if(keyboard_haschar()){ keyboard_getchar(); return; }
        for(int i=0;i<MAX_STARS;i++){
            /* Erase old */
            vga_putchar_at(' ',stars[i].x,stars[i].y,VGA_BLACK,VGA_BLACK);
            /* Move - slower stars move less */
            if(frame%(uint32_t)(4-stars[i].z+1)==0){
                stars[i].x--;
                if(stars[i].x<1){
                    stars[i].x=78;
                    stars[i].y=(int)(rng()%23)+1;
                }
            }
            /* Twinkle */
            if((int)(rng()%20)==0){
                static const char chs[]=".*+o";
                stars[i].ch=chs[rng()%4];
            }
            vga_putchar_at(stars[i].ch,stars[i].x,stars[i].y,stars[i].col,VGA_BLACK);
        }
        vga_puts_at("Press any key to continue",27,24,VGA_DARK_GREY,VGA_BLACK);
        frame++;
        busy_wait(500000);
    }
}

/* ---- Public API ---- */
/* idle_ticks = how many ticks of inactivity before screensaver
   Call this regularly from desktop loop */
static uint32_t last_activity = 0;
static int ss_type = 0; /* 0=matrix 1=stars 2=pipes, cycles */

static void screensaver_pipes(void); /* forward decl */
void screensaver_reset(void){ last_activity=get_ticks(); }

void screensaver_check(void){
    /* Trigger after 30 seconds of inactivity (3000 ticks at 100Hz) */
    if(get_ticks()-last_activity < 3000) return;
    /* Pick screensaver */
    rng_state = get_ticks() ^ 0xABCD1234;
    if(ss_type==0)      screensaver_matrix();
    else if(ss_type==1) screensaver_stars();
    else                screensaver_pipes();
    ss_type=(ss_type+1)%3;
    last_activity = get_ticks();
}

void screensaver_run_matrix(void){ rng_state=get_ticks(); screensaver_matrix(); }
void screensaver_run_stars(void) { rng_state=get_ticks(); screensaver_stars();  }

/* ================================================================
   PIPES SCREENSAVER
   Uses only plain ASCII pipe chars: | - + (UTM SE safe)
   ================================================================ */
static void screensaver_pipes(void){
    static const vga_color_t pipe_cols[]={
        VGA_LIGHT_GREEN,VGA_CYAN,VGA_LIGHT_CYAN,VGA_YELLOW,
        VGA_MAGENTA,VGA_WHITE,VGA_LIGHT_BLUE,VGA_LIGHT_RED
    };
    /* 4 simultaneous pipes */
    #define NPIPES 4
    int px[NPIPES],py[NPIPES];
    int pdx[NPIPES],pdy[NPIPES];
    vga_color_t pcol[NPIPES];
    int plen[NPIPES];

    /* Clear screen */
    for(int y=0;y<25;y++)
        for(int x=0;x<80;x++)
            vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLACK);

    /* Init pipes */
    for(int i=0;i<NPIPES;i++){
        px[i]=(int)(rng()%78)+1;
        py[i]=(int)(rng()%23)+1;
        int dir=(int)(rng()%4);
        int dxs[]={1,-1,0,0};
        int dys[]={0,0,1,-1};
        pdx[i]=dxs[dir]; pdy[i]=dys[dir];
        pcol[i]=pipe_cols[rng()%8];
        plen[i]=0;
    }

    uint32_t frame=0;
    while(1){
        if(keyboard_haschar()){ keyboard_getchar(); return; }

        for(int i=0;i<NPIPES;i++){
            /* Draw current char */
            char c;
            if(pdx[i]!=0) c='-';
            else           c='|';
            vga_putchar_at(c,px[i],py[i],pcol[i],VGA_BLACK);

            /* Move */
            int nx=px[i]+pdx[i];
            int ny=py[i]+pdy[i];

            /* Bounce or turn */
            int must_turn=0;
            if(nx<0||nx>=80||ny<0||ny>=25) must_turn=1;
            /* Random turn chance */
            else if((int)(rng()%12)==0) must_turn=1;

            if(must_turn){
                /* Draw corner '+' at turn point */
                vga_putchar_at('+',px[i],py[i],pcol[i],VGA_BLACK);

                /* Pick new perpendicular direction */
                int newdir;
                if(pdx[i]!=0){
                    /* was horizontal, go vertical */
                    newdir=(rng()%2)?1:-1;
                    pdx[i]=0; pdy[i]=newdir;
                } else {
                    /* was vertical, go horizontal */
                    newdir=(rng()%2)?1:-1;
                    pdx[i]=newdir; pdy[i]=0;
                }
                nx=px[i]+pdx[i];
                ny=py[i]+pdy[i];

                plen[i]++;
                /* Reset pipe after long run */
                if(plen[i]>40||(nx<0||nx>=80||ny<0||ny>=25)){
                    px[i]=(int)(rng()%78)+1;
                    py[i]=(int)(rng()%23)+1;
                    int dir2=(int)(rng()%4);
                    int dxs[]={1,-1,0,0};
                    int dys[]={0,0,1,-1};
                    pdx[i]=dxs[dir2]; pdy[i]=dys[dir2];
                    pcol[i]=pipe_cols[rng()%8];
                    plen[i]=0;
                    continue;
                }
            }

            px[i]=nx; py[i]=ny;
        }

        vga_puts_at("Press any key...",32,24,VGA_DARK_GREY,VGA_BLACK);
        frame++;
        busy_wait(200000);
    }
}

void screensaver_run_pipes(void){ rng_state=get_ticks(); screensaver_pipes(); }
