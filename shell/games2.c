/* ================================================================
   shell/games2.c  --  four new games for v0.9.5.1.

     2048         slide and merge tiles to reach 2048
     Breakout     a paddle, a ball, a wall of bricks
     Tic-Tac-Toe  against the computer, easy or perfect
     Memory       find the eight pairs

   Kyan, after 0.9.5: "add some new games and apps". All four are drawn
   in the 0.9.5.1 look (shell/ui.c): solid blocks rather than ASCII
   boxes, the app bar on top, the keys in a footer, the theme's
   selection colour for the cursor. They lay out for 80x25 -- the
   desktop centres that box on a bigger console -- and none of them
   needs the network, the disk or anything that can fail.
   ================================================================ */
#include "types.h"
#include "string.h"
#include "vga.h"
#include "keys.h"
#include "theme.h"
#include "rtc.h"

extern int      keyboard_getchar(void);
extern int      keyboard_waitchar(void);
extern void     sleep_ms(uint32_t);
extern uint32_t get_ticks(void);

/* ── small shared helpers ─────────────────────────────────────────── */
static uint32_t g_rng;
static void rng_seed(void){
    uint8_t h,m,s; rtc_get_time(&h,&m,&s);
    g_rng = get_ticks()*2654435761u ^ ((uint32_t)h<<16 | (uint32_t)m<<8 | s) ^ 0x9E3779B9u;
    if(!g_rng) g_rng = 1;
}
static uint32_t rng(void){            /* xorshift32 */
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}
static void utoa_s(uint32_t v, char *o){
    char t[12]; int n=0;
    do{ t[n++]=(char)('0'+v%10); v/=10; }while(v);
    for(int i=0;i<n;i++) o[i]=t[n-1-i];
    o[n]=0;
}
static void put(int x,int y,const char *s,vga_color_t fg,vga_color_t bg){
    for(;*s && x<vga_cols();s++,x++) vga_putchar_at(*s,x,y,fg,bg);
}
static void put_centre(int y,const char *s,vga_color_t fg){
    put((80-(int)strlen(s))/2,y,s,fg,VGA_BLACK);
}
static void fill(int x,int y,int w,int h,vga_color_t bg){
    for(int r=0;r<h;r++) for(int c=0;c<w;c++) vga_putchar_at(' ',x+c,y+r,VGA_WHITE,bg);
}
/* "Score 120" on the right of the app bar. */
static void bar_stat(int x,const char *label,uint32_t v){
    theme_t *th=ui_theme();
    char b[12]; utoa_s(v,b);
    put(x,0,label,th->bar_fg,th->bar_bg);
    put(x+(int)strlen(label)+1,0,b,th->accent,th->bar_bg);
    put(x+(int)strlen(label)+1+(int)strlen(b),0,"   ",th->bar_fg,th->bar_bg);
}

/* ================================================================
   2048
   ================================================================ */
#define G_N 4
static uint32_t g2[G_N][G_N];
static uint32_t g2_score, g2_best;

static void g2_spawn(void){
    int free_cells[G_N*G_N], n=0;
    for(int r=0;r<G_N;r++) for(int c=0;c<G_N;c++) if(!g2[r][c]) free_cells[n++]=r*G_N+c;
    if(!n) return;
    int k=free_cells[rng()%(uint32_t)n];
    g2[k/G_N][k%G_N] = (rng()%10==0) ? 4 : 2;
}

/* Slide one line toward index 0; returns 1 if anything moved. */
static int g2_line(uint32_t *v[G_N]){
    uint32_t out[G_N]={0}; int n=0, moved=0, merged=0;
    for(int i=0;i<G_N;i++){
        if(!*v[i]) continue;
        if(n && !merged && out[n-1]==*v[i]){ out[n-1]*=2; g2_score+=out[n-1]; merged=1; }
        else { out[n++]=*v[i]; merged=0; }
    }
    for(int i=0;i<G_N;i++){ if(*v[i]!=out[i]) moved=1; *v[i]=out[i]; }
    return moved;
}

static int g2_move(int dir){          /* 0 left 1 right 2 up 3 down */
    int moved=0;
    for(int k=0;k<G_N;k++){
        uint32_t *v[G_N];
        for(int i=0;i<G_N;i++){
            int j = (dir==1||dir==3) ? G_N-1-i : i;
            v[i] = (dir<2) ? &g2[k][j] : &g2[j][k];
        }
        moved |= g2_line(v);
    }
    return moved;
}

static int g2_can_move(void){
    for(int r=0;r<G_N;r++) for(int c=0;c<G_N;c++){
        if(!g2[r][c]) return 1;
        if(c+1<G_N && g2[r][c]==g2[r][c+1]) return 1;
        if(r+1<G_N && g2[r][c]==g2[r+1][c]) return 1;
    }
    return 0;
}

static void g2_draw(int over, int won){
    static const vga_color_t col[] = { VGA_BLUE, VGA_GREEN, VGA_CYAN, VGA_MAGENTA,
                                       VGA_RED, VGA_BROWN, VGA_LIGHT_GREY };
    vga_begin_frame();
    ui_kit_begin("2048", "Arrows slide the tiles   R new game   Q quit");
    bar_stat(28,"Score",g2_score);
    bar_stat(44,"Best",g2_best);
    const int TW=9, TH=4, X0=(80-(G_N*(TW+1)+1))/2, Y0=3;
    fill(X0,Y0,G_N*(TW+1)+1,G_N*(TH+1)+1,VGA_DARK_GREY);
    for(int r=0;r<G_N;r++) for(int c=0;c<G_N;c++){
        int x=X0+1+c*(TW+1), y=Y0+1+r*(TH+1);
        uint32_t v=g2[r][c];
        if(!v){ fill(x,y,TW,TH,VGA_BLACK); continue; }
        int lg=0; for(uint32_t t=v;t>2;t>>=1) lg++;
        vga_color_t bg=col[lg%7];
        vga_color_t fg=(bg==VGA_CYAN||bg==VGA_GREEN||bg==VGA_LIGHT_GREY||bg==VGA_BROWN)?VGA_BLACK:VGA_WHITE;
        fill(x,y,TW,TH,bg);
        char b[12]; utoa_s(v,b);
        put(x+(TW-(int)strlen(b))/2,y+TH/2-1+1,b,fg,bg);
    }
    if(won||over){
        int w=36, x=(80-w)/2, y=10;
        ui_kit_panel(x,y,w,5,won?"You made 2048!":"No moves left");
        put(x+2,y+2,won?"Keep going with the arrows,":"Final score:",VGA_LIGHT_GREY,VGA_BLACK);
        if(won) put(x+2,y+3,"or R for a new game.",VGA_LIGHT_GREY,VGA_BLACK);
        else { char b[12]; utoa_s(g2_score,b); put(x+15,y+2,b,VGA_WHITE,VGA_BLACK);
               put(x+2,y+3,"R plays again, Q quits.",VGA_LIGHT_GREY,VGA_BLACK); }
    }
    vga_end_frame();
}

void app_2048(void){
    rng_seed();
    for(;;){
        memset(g2,0,sizeof g2); g2_score=0;
        g2_spawn(); g2_spawn();
        int won_shown=0, over=0, show_win=0;
        for(;;){
            over = !g2_can_move();
            g2_draw(over, show_win);
            int k=keyboard_waitchar();
            show_win=0;
            if(k=='q'||k=='Q'||k==27) return;
            if(k=='r'||k=='R') break;
            if(over) continue;
            int d = k==KEY_LEFT?0 : k==KEY_RIGHT?1 : k==KEY_UP?2 : k==KEY_DOWN?3 : -1;
            if(d<0) continue;
            if(g2_move(d)){
                g2_spawn();
                if(g2_score>g2_best) g2_best=g2_score;
                if(!won_shown){
                    for(int r=0;r<G_N;r++) for(int c=0;c<G_N;c++) if(g2[r][c]>=2048) show_win=1;
                    if(show_win) won_shown=1;
                }
            }
        }
    }
}

/* ================================================================
   BREAKOUT
   ================================================================ */
#define BK_ROWS 5
#define BK_COLS 12
#define BK_W    5
#define BK_X0   ((80-BK_COLS*(BK_W+1))/2)
#define BK_Y0   3
#define BO_TOP  2
#define BO_BOT  23
#define PAD_W   11

static uint8_t bricks[BK_ROWS][BK_COLS];

void app_breakout(void){
    static const vga_color_t rowcol[BK_ROWS]={VGA_RED,VGA_BROWN,VGA_GREEN,VGA_CYAN,VGA_BLUE};
    rng_seed();
    uint32_t score=0, best=0;
    for(;;){                                       /* a game */
        for(int r=0;r<BK_ROWS;r++) for(int c=0;c<BK_COLS;c++) bricks[r][c]=1;
        int lives=3, left=BK_ROWS*BK_COLS, level=1;
        score=0;
        int px=(80-PAD_W)/2;
        for(;;){                                   /* a ball */
            /* fixed point, 1/16 of a cell */
            int bx=(px+PAD_W/2)*16, by=(BO_BOT-2)*16;
            int dx=(rng()&1)?10:-10, dy=-12-level*2;
            int launched=0, lost=0;
            uint32_t last=get_ticks();
            while(!lost){
                int k=keyboard_getchar();
                if(k=='q'||k=='Q'||k==27) return;
                if(k==KEY_LEFT)  { px-=3; if(px<1) px=1; }
                if(k==KEY_RIGHT) { px+=3; if(px>79-PAD_W) px=79-PAD_W; }
                if(k==' ') launched=1;
                if(get_ticks()-last<3){ sleep_ms(5); continue; }
                last=get_ticks();

                if(!launched){ bx=(px+PAD_W/2)*16; by=(BO_BOT-2)*16; }
                else{
                    bx+=dx; by+=dy;
                    int cx=bx/16, cy=by/16;
                    if(cx<=0){ bx=16; dx=-dx; }
                    if(cx>=79){ bx=78*16; dx=-dx; }
                    if(cy<=BO_TOP){ by=(BO_TOP+1)*16; dy=-dy; }
                    /* bricks */
                    if(cy>=BK_Y0 && cy<BK_Y0+BK_ROWS*2){
                        int r=(cy-BK_Y0)/2, c=(cx-BK_X0)/(BK_W+1);
                        if((cy-BK_Y0)%2==0 && cx>=BK_X0 && c>=0 && c<BK_COLS &&
                           (cx-BK_X0)%(BK_W+1)<BK_W && bricks[r][c]){
                            bricks[r][c]=0; left--; dy=-dy;
                            score+= (uint32_t)(BK_ROWS-r)*10;
                            if(score>best) best=score;
                        }
                    }
                    /* paddle */
                    if(dy>0 && cy==BO_BOT-1 && cx>=px-1 && cx<=px+PAD_W){
                        int off=cx-(px+PAD_W/2);
                        dx=off*4; if(dx==0) dx=(rng()&1)?3:-3;
                        dy=-dy;
                    }
                    if(cy>=BO_BOT){ lost=1; lives--; }
                }

                vga_begin_frame();
                ui_kit_begin("Breakout", launched?"Left/Right move the paddle   Q quit"
                                                 :"Space launches the ball   Left/Right move   Q quit");
                bar_stat(20,"Score",score);
                bar_stat(34,"Best",best);
                {   char lv[16]="Lives  "; lv[6]=(char)('0'+lives);
                    put(48,0,lv,ui_theme()->bar_fg,ui_theme()->bar_bg); }
                for(int r=0;r<BK_ROWS;r++) for(int c=0;c<BK_COLS;c++)
                    if(bricks[r][c]) fill(BK_X0+c*(BK_W+1),BK_Y0+r*2,BK_W,1,rowcol[r]);
                vga_color_t sf,sb; ui_sel_colors(&sf,&sb); (void)sf;
                fill(px,BO_BOT-1,PAD_W,1,sb);
                vga_putchar_at(' ',bx/16,by/16,VGA_LIGHT_GREY,VGA_LIGHT_GREY);
                vga_end_frame();

                if(!left){
                    level++;
                    for(int r=0;r<BK_ROWS;r++) for(int c=0;c<BK_COLS;c++) bricks[r][c]=1;
                    left=BK_ROWS*BK_COLS;
                    launched=0; dy=-12-level*2;
                }
            }
            if(lives<=0) break;
            sleep_ms(400);
        }
        ui_kit_panel(22,9,36,5,"Game over");
        { char b[12]; utoa_s(score,b);
          put(24,11,"Score:",VGA_LIGHT_GREY,VGA_BLACK); put(31,11,b,VGA_WHITE,VGA_BLACK);
          put(24,12,"Space plays again, Q quits.",VGA_LIGHT_GREY,VGA_BLACK); }
        vga_flush();
        while(keyboard_getchar()!=-1){}
        for(;;){ int k=keyboard_waitchar(); if(k=='q'||k=='Q'||k==27) return; if(k==' '||k=='\n') break; }
    }
}

/* ================================================================
   TIC-TAC-TOE
   ================================================================ */
static char t3[9];
static const uint8_t lines3[8][3]={{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};

static char t3_winner(void){
    for(int i=0;i<8;i++){
        char a=t3[lines3[i][0]];
        if(a!=' ' && a==t3[lines3[i][1]] && a==t3[lines3[i][2]]) return a;
    }
    for(int i=0;i<9;i++) if(t3[i]==' ') return 0;
    return 'D';
}

static int t3_minimax(int cpu_turn, int depth){
    char w=t3_winner();
    if(w=='O') return 10-depth;
    if(w=='X') return depth-10;
    if(w=='D') return 0;
    int best = cpu_turn ? -100 : 100;
    for(int i=0;i<9;i++){
        if(t3[i]!=' ') continue;
        t3[i]=cpu_turn?'O':'X';
        int v=t3_minimax(!cpu_turn,depth+1);
        t3[i]=' ';
        if(cpu_turn ? v>best : v<best) best=v;
    }
    return best;
}

static void t3_cpu(int hard){
    int free_cells[9], n=0;
    for(int i=0;i<9;i++) if(t3[i]==' ') free_cells[n++]=i;
    if(!n) return;
    if(!hard && rng()%2){ t3[free_cells[rng()%(uint32_t)n]]='O'; return; }
    int best=-1000, at=free_cells[0];
    for(int k=0;k<n;k++){
        int i=free_cells[k];
        t3[i]='O';
        int v=t3_minimax(0,1);
        t3[i]=' ';
        if(v>best){ best=v; at=i; }
    }
    t3[at]='O';
}

static void t3_draw(int cur, int hard, uint32_t you, uint32_t cpu, uint32_t draws, const char *msg){
    vga_begin_frame();
    ui_kit_begin("Tic-Tac-Toe", "Arrows choose   Enter places your X   D difficulty   R new round   Q quit");
    const int CW=11, CH=5, X0=(80-(3*CW+2))/2, Y0=3;
    fill(X0,Y0,3*CW+2,3*CH+2,VGA_DARK_GREY);
    vga_color_t sf,sb; ui_sel_colors(&sf,&sb); (void)sf;
    for(int i=0;i<9;i++){
        int r=i/3, c=i%3;
        int x=X0+c*(CW+1), y=Y0+r*(CH+1);
        vga_color_t bg = (i==cur) ? sb : VGA_BLACK;
        fill(x,y,CW,CH,bg);
        /* X and O as solid strokes, in a colour that shows on the cursor */
        if(t3[i]=='X'){
            vga_color_t f = (bg==VGA_CYAN) ? VGA_BLUE : VGA_CYAN;
            for(int k=0;k<3;k++){ fill(x+3+k*2,y+1+k,1,1,f); fill(x+7-k*2,y+1+k,1,1,f); }
        } else if(t3[i]=='O'){
            vga_color_t f = (bg==VGA_RED) ? VGA_BROWN : VGA_RED;
            fill(x+3,y+1,5,1,f); fill(x+3,y+3,5,1,f);
            fill(x+3,y+2,1,1,f); fill(x+7,y+2,1,1,f);
        }
    }
    int px=X0+3*CW+6;
    ui_kit_heading(px,4,"Score");
    char b[12];
    put(px,6,"You",VGA_DARK_GREY,VGA_BLACK);      utoa_s(you,b);   put(px+9,6,b,VGA_WHITE,VGA_BLACK);
    put(px,7,"Computer",VGA_DARK_GREY,VGA_BLACK); utoa_s(cpu,b);   put(px+9,7,b,VGA_WHITE,VGA_BLACK);
    put(px,8,"Draws",VGA_DARK_GREY,VGA_BLACK);    utoa_s(draws,b); put(px+9,8,b,VGA_WHITE,VGA_BLACK);
    put(px,10,"Computer",VGA_DARK_GREY,VGA_BLACK);
    put(px,11,hard?"perfect":"easy",VGA_WHITE,VGA_BLACK);
    if(msg) put_centre(Y0+3*CH+4,msg,VGA_YELLOW);
    vga_end_frame();
}

void app_tictactoe(void){
    rng_seed();
    uint32_t you=0,cpu=0,draws=0;
    int hard=0, first=0;
    for(;;){
        for(int i=0;i<9;i++) t3[i]=' ';
        if(first) t3_cpu(hard);
        first=!first;
        int cur=4;
        const char *msg="You are X. Three in a row wins.";
        for(;;){
            t3_draw(cur,hard,you,cpu,draws,msg);
            char w=t3_winner();
            int k=keyboard_waitchar();
            if(k=='q'||k=='Q'||k==27) return;
            if(k=='r'||k=='R') break;
            if(k=='d'||k=='D'){ hard=!hard; msg=hard?"The computer now plays perfectly.":"The computer now makes mistakes."; continue; }
            if(w) { if(k=='\n'||k=='\r'||k==' ') break; continue; }
            if(k==KEY_LEFT  && cur%3>0) cur--;
            if(k==KEY_RIGHT && cur%3<2) cur++;
            if(k==KEY_UP    && cur>2)   cur-=3;
            if(k==KEY_DOWN  && cur<6)   cur+=3;
            if((k=='\n'||k=='\r'||k==' ') && t3[cur]==' '){
                t3[cur]='X';
                if(!t3_winner()) t3_cpu(hard);
                w=t3_winner();
                if(w=='X'){ you++;   msg="You win! Enter for another round."; }
                if(w=='O'){ cpu++;   msg="The computer wins. Enter for another round."; }
                if(w=='D'){ draws++; msg="A draw. Enter for another round."; }
            }
        }
    }
}

/* ================================================================
   MEMORY
   ================================================================ */
static char mm_sym[16];
static uint8_t mm_state[16];          /* 0 hidden, 1 shown, 2 matched */

void app_memory(void){
    static const char syms[8]={'A','B','C','D','E','F','G','H'};
    static const vga_color_t symcol[8]={VGA_RED,VGA_GREEN,VGA_BLUE,VGA_MAGENTA,VGA_CYAN,VGA_BROWN,VGA_LIGHT_GREY,VGA_RED};
    rng_seed();
    uint32_t best=0;
    for(;;){
        for(int i=0;i<16;i++){ mm_sym[i]=syms[i/2]; mm_state[i]=0; }
        for(int i=15;i>0;i--){ int j=(int)(rng()%(uint32_t)(i+1)); char t=mm_sym[i]; mm_sym[i]=mm_sym[j]; mm_sym[j]=t; }
        int cur=0, first=-1, pairs=0, pend_a=-1, pend_b=-1; uint32_t moves=0;
        for(;;){
            vga_begin_frame();
            ui_kit_begin("Memory", "Arrows choose   Enter turns a card over   R new game   Q quit");
            bar_stat(28,"Moves",moves);
            if(best) bar_stat(44,"Best",best);
            const int CW=9, CH=4, X0=(80-4*(CW+2))/2, Y0=3;
            vga_color_t sf,sb; ui_sel_colors(&sf,&sb);
            for(int i=0;i<16;i++){
                int x=X0+(i%4)*(CW+2), y=Y0+(i/4)*(CH+1);
                int s=mm_state[i];
                int si=mm_sym[i]-'A';
                vga_color_t bg = s==0 ? VGA_DARK_GREY : s==2 ? VGA_BLACK : symcol[si];
                if(i==cur) bg = (s==0) ? sb : bg;
                fill(x,y,CW,CH,bg);
                if(s){
                    char t[2]={mm_sym[i],0};
                    put(x+CW/2,y+CH/2-1+0,t,s==2?VGA_DARK_GREY:VGA_WHITE,bg);
                    put(x+CW/2,y+CH/2,t,s==2?VGA_DARK_GREY:VGA_WHITE,bg);
                }
                if(i==cur && s){ vga_putchar_at('>',x,y+1,sf,sb); }
                else if(i==cur) vga_putchar_at(' ',x,y,sf,sb);
            }
            if(pairs==8){
                ui_kit_panel(22,9,36,5,"All pairs found");
                char b[12]; utoa_s(moves,b);
                put(24,11,"Moves:",VGA_LIGHT_GREY,VGA_BLACK); put(31,11,b,VGA_WHITE,VGA_BLACK);
                put(24,12,"R plays again, Q quits.",VGA_LIGHT_GREY,VGA_BLACK);
            }
            vga_end_frame();
            if(pend_a>=0){
                sleep_ms(900);
                mm_state[pend_a]=0; mm_state[pend_b]=0;
                pend_a=pend_b=-1;
                while(keyboard_getchar()!=-1){}
                continue;
            }

            int k=keyboard_waitchar();
            if(k=='q'||k=='Q'||k==27) return;
            if(k=='r'||k=='R') break;
            if(pairs==8) continue;
            if(k==KEY_LEFT  && cur%4>0) cur--;
            if(k==KEY_RIGHT && cur%4<3) cur++;
            if(k==KEY_UP    && cur>3)   cur-=4;
            if(k==KEY_DOWN  && cur<12)  cur+=4;
            if((k=='\n'||k=='\r'||k==' ') && mm_state[cur]==0){
                mm_state[cur]=1;
                if(first<0){ first=cur; continue; }
                moves++;
                if(mm_sym[first]==mm_sym[cur]){
                    mm_state[first]=mm_state[cur]=2; pairs++;
                    if(pairs==8 && (!best || moves<best)) best=moves;
                    first=-1;
                } else {
                    /* both stay face up for one frame, then turn back */
                    pend_a=first; pend_b=cur; first=-1;
                }
            }
        }
    }
}
