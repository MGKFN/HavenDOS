#include "../include/vga.h"
#include "../include/string.h"
#include "../include/types.h"

extern int      keyboard_getchar(void);
extern int      keyboard_haschar(void);
extern uint32_t get_ticks(void);
extern void     sleep_ms(uint32_t);
extern void     music_levelup(void);
extern void     music_gameover(void);
extern void     ui_draw_taskbar(const char*);

#define TW 10
#define TH 20
#define BX 30
#define BY 2

static uint8_t board[TH][TW];

/* Tetrominoes: 7 pieces, 4 rotations, 4 cells (row,col) from pivot */
static const int8_t pieces[7][4][4][2]={
    /* I */
    {{{0,-1},{0,0},{0,1},{0,2}},
     {{-1,0},{0,0},{1,0},{2,0}},
     {{0,-1},{0,0},{0,1},{0,2}},
     {{-1,0},{0,0},{1,0},{2,0}}},
    /* O */
    {{{0,0},{0,1},{1,0},{1,1}},
     {{0,0},{0,1},{1,0},{1,1}},
     {{0,0},{0,1},{1,0},{1,1}},
     {{0,0},{0,1},{1,0},{1,1}}},
    /* T */
    {{{0,-1},{0,0},{0,1},{1,0}},
     {{-1,0},{0,0},{1,0},{0,1}},
     {{0,-1},{0,0},{0,1},{-1,0}},
     {{-1,0},{0,0},{1,0},{0,-1}}},
    /* S */
    {{{0,0},{0,1},{1,-1},{1,0}},
     {{-1,0},{0,0},{0,1},{1,1}},
     {{0,0},{0,1},{1,-1},{1,0}},
     {{-1,0},{0,0},{0,1},{1,1}}},
    /* Z */
    {{{0,-1},{0,0},{1,0},{1,1}},
     {{0,0},{0,1},{1,-1},{1,0}},
     {{0,-1},{0,0},{1,0},{1,1}},
     {{0,0},{0,1},{1,-1},{1,0}}},
    /* J */
    {{{0,-1},{0,0},{0,1},{1,-1}},
     {{-1,0},{0,0},{1,0},{-1,1}},
     {{0,-1},{0,0},{0,1},{-1,1}},
     {{-1,0},{0,0},{1,0},{1,-1}}},
    /* L */
    {{{0,-1},{0,0},{0,1},{1,1}},
     {{-1,0},{0,0},{1,0},{1,1}},
     {{0,-1},{0,0},{0,1},{-1,-1}},
     {{-1,0},{0,0},{1,0},{-1,-1}}},
};

static const vga_color_t piece_colors[7]={
    VGA_LIGHT_CYAN, VGA_YELLOW, VGA_MAGENTA,
    VGA_LIGHT_GREEN, VGA_RED, VGA_LIGHT_BLUE, VGA_LIGHT_RED
};

static int cur_type, cur_rot, cur_row, cur_col;
static int next_type;
static int score, lines, level;
static int game_over;

/* Simple LCG rng seeded from ticks */
static uint32_t rng_s=12345;
static int rng_next(void){
    rng_s=rng_s*1664525+1013904223;
    return (int)(rng_s%7);
}

static int valid(int type, int rot, int row, int col){
    for(int i=0;i<4;i++){
        int r=row+pieces[type][rot][i][0];
        int c=col+pieces[type][rot][i][1];
        if(r<0||r>=TH||c<0||c>=TW) return 0;
        if(board[r][c]) return 0;
    }
    return 1;
}

static void place(void){
    for(int i=0;i<4;i++){
        int r=cur_row+pieces[cur_type][cur_rot][i][0];
        int c=cur_col+pieces[cur_type][cur_rot][i][1];
        if(r>=0&&r<TH&&c>=0&&c<TW)
            board[r][c]=(uint8_t)(cur_type+1);
    }
}

static int clear_lines(void){
    int cleared=0;
    for(int r=TH-1;r>=0;){
        int full=1;
        for(int c=0;c<TW;c++) if(!board[r][c]){full=0;break;}
        if(full){
            cleared++;
            for(int rr=r;rr>0;rr--)
                for(int c=0;c<TW;c++) board[rr][c]=board[rr-1][c];
            for(int c=0;c<TW;c++) board[0][c]=0;
            /* don't decrement r — recheck same row */
        } else r--;
    }
    return cleared;
}

static void draw_cell(int r, int c, uint8_t val){
    int sx=BX+c*2, sy=BY+r;
    if(val==0){
        vga_putchar_at('.',sx,  sy,VGA_DARK_GREY,VGA_BLACK);
        vga_putchar_at('.',sx+1,sy,VGA_DARK_GREY,VGA_BLACK);
    } else {
        vga_color_t col=piece_colors[val-1];
        vga_putchar_at('[',sx,  sy,col,VGA_BLACK);
        vga_putchar_at(']',sx+1,sy,col,VGA_BLACK);
    }
}

static void draw_board(void){
    for(int r=0;r<TH;r++)
        for(int c=0;c<TW;c++)
            draw_cell(r,c,board[r][c]);
}

static void draw_piece(int type,int rot,int row,int col,int show){
    for(int i=0;i<4;i++){
        int r=row+pieces[type][rot][i][0];
        int c=col+pieces[type][rot][i][1];
        if(r>=0&&r<TH&&c>=0&&c<TW)
            draw_cell(r,c,show?(uint8_t)(type+1):board[r][c]);
    }
}

/* Draw ghost piece (where piece will land) */
static void draw_ghost(int show){
    int gr=cur_row;
    while(valid(cur_type,cur_rot,gr+1,cur_col)) gr++;
    if(gr==cur_row) return; /* already landed */
    for(int i=0;i<4;i++){
        int r=gr+pieces[cur_type][cur_rot][i][0];
        int c=cur_col+pieces[cur_type][cur_rot][i][1];
        if(r>=0&&r<TH&&c>=0&&c<TW){
            if(show){
                vga_putchar_at('#',BX+c*2,  BY+r,VGA_DARK_GREY,VGA_BLACK);
                vga_putchar_at('#',BX+c*2+1,BY+r,VGA_DARK_GREY,VGA_BLACK);
            } else {
                draw_cell(r,c,board[r][c]);
            }
        }
    }
}

static void draw_sidebar(void){
    int ix=BX+TW*2+2;

    /* Left border */
    for(int r=0;r<TH;r++){
        vga_putchar_at('|',BX-1,BY+r,VGA_WHITE,VGA_BLACK);
        vga_putchar_at('|',BX+TW*2,BY+r,VGA_WHITE,VGA_BLACK);
    }
    /* Top/bottom border */
    vga_putchar_at('.',BX-1,BY-1,VGA_WHITE,VGA_BLACK);
    for(int i=0;i<TW*2;i++) vga_putchar_at('-',BX+i,BY-1,VGA_WHITE,VGA_BLACK);
    vga_putchar_at('.',BX+TW*2,BY-1,VGA_WHITE,VGA_BLACK);
    vga_putchar_at('.',BX-1,BY+TH,VGA_WHITE,VGA_BLACK);
    for(int i=0;i<TW*2;i++) vga_putchar_at('-',BX+i,BY+TH,VGA_WHITE,VGA_BLACK);
    vga_putchar_at('.',BX+TW*2,BY+TH,VGA_WHITE,VGA_BLACK);

    /* Sidebar panel */
    vga_puts_at(".----------.",ix-1,BY,VGA_WHITE,VGA_BLACK);
    for(int r=1;r<TH;r++){
        vga_putchar_at('|',ix-1,BY+r,VGA_WHITE,VGA_BLACK);
        vga_putchar_at('|',ix+11,BY+r,VGA_WHITE,VGA_BLACK);
        for(int i=0;i<11;i++) vga_putchar_at(' ',ix+i,BY+r,VGA_BLACK,VGA_BLACK);
    }
    vga_puts_at(".----------.",ix-1,BY+TH,VGA_WHITE,VGA_BLACK);

    /* Title */
    vga_puts_at(" TETRIS ",ix+1,BY,VGA_BLACK,VGA_LIGHT_CYAN);
    vga_puts_at("v0.5.4",ix+2,BY+1,VGA_DARK_GREY,VGA_BLACK);

    /* Score */
    vga_puts_at("Score:",ix+1,BY+3,VGA_LIGHT_GREY,VGA_BLACK);
    char sb[12]; itoa(score,sb,10);
    vga_puts_at("          ",ix+1,BY+4,VGA_BLACK,VGA_BLACK);
    vga_puts_at(sb,ix+1,BY+4,VGA_YELLOW,VGA_BLACK);

    /* Lines */
    vga_puts_at("Lines:",ix+1,BY+6,VGA_LIGHT_GREY,VGA_BLACK);
    char lb[12]; itoa(lines,lb,10);
    vga_puts_at("     ",ix+1,BY+7,VGA_BLACK,VGA_BLACK);
    vga_puts_at(lb,ix+1,BY+7,VGA_LIGHT_GREEN,VGA_BLACK);

    /* Level */
    vga_puts_at("Level:",ix+1,BY+9,VGA_LIGHT_GREY,VGA_BLACK);
    char lv[4]; itoa(level+1,lv,10);
    vga_puts_at("     ",ix+1,BY+10,VGA_BLACK,VGA_BLACK);
    vga_puts_at(lv,ix+1,BY+10,VGA_LIGHT_CYAN,VGA_BLACK);

    /* Next piece preview */
    vga_puts_at("Next:",ix+1,BY+12,VGA_LIGHT_GREY,VGA_BLACK);
    for(int r=0;r<4;r++)
        for(int c=0;c<5;c++)
            vga_putchar_at(' ',ix+1+c*2,BY+13+r,VGA_BLACK,VGA_BLACK);
    for(int i=0;i<4;i++){
        int pr=pieces[next_type][0][i][0]+1;
        int pc=pieces[next_type][0][i][1]+2;
        if(pr>=0&&pr<5&&pc>=0&&pc<6){
            vga_color_t col=piece_colors[next_type];
            vga_putchar_at('[',ix+1+pc*2,  BY+13+pr,col,VGA_BLACK);
            vga_putchar_at(']',ix+1+pc*2+1,BY+13+pr,col,VGA_BLACK);
        }
    }

    /* Controls */
    vga_puts_at("Keys:",    ix+1,BY+17,VGA_LIGHT_GREY,VGA_BLACK);
    vga_puts_at("L-R  Move",  ix+1,BY+18,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("Up   Rotate",ix+1,BY+19,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("Dn   Soft",  ix+1,BY+20,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("Spc Hard", ix+1,BY+21,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("Q Quit",   ix+1,BY+22,VGA_DARK_GREY,VGA_BLACK);
}

static void spawn_piece(void){
    cur_type=next_type;
    next_type=rng_next();
    cur_rot=0;
    cur_row=0;   /* spawn at row 0 so pieces enter from top */
    cur_col=TW/2-1;
    if(!valid(cur_type,cur_rot,cur_row,cur_col))
        game_over=1;
}

/* Drop speed in ms per row */
static uint32_t drop_ms(void){
    static const uint32_t speeds[]={800,650,500,400,300,220,160,120,80,60,40};
    int l=level; if(l>10)l=10;
    return speeds[l];
}

/* Flash cleared rows */
static void flash_lines(int *rows, int count){
    for(int flash=0;flash<3;flash++){
        for(int f=0;f<count;f++){
            vga_color_t col=(flash%2)?VGA_WHITE:VGA_YELLOW;
            for(int c=0;c<TW*2;c++)
                vga_putchar_at('#',BX+c,BY+rows[f],col,VGA_BLACK);
        }
        sleep_ms(60);
    }
}

void app_tetris(void){
    vga_clear();
    ui_draw_taskbar("Tetris");
    memset(board,0,sizeof(board));
    score=0; lines=0; level=0; game_over=0;
    rng_s=get_ticks()^0xFACE;
    next_type=rng_next();
    spawn_piece();
    draw_board();
    draw_sidebar();
    draw_piece(cur_type,cur_rot,cur_row,cur_col,1);
    draw_ghost(1);

    /* tick_ms = how many ms per game tick (one call through the loop)
       We poll input at ~30ms per tick, and count ticks to auto-drop */
    #define TICK_MS 30
    int drop_counter=0;  /* counts ticks until next drop */

    while(!game_over){
        sleep_ms(TICK_MS);

        /* Input */
        int c=keyboard_getchar();
        if(c=='q'||c=='Q'||c==27) break;

        if(c==0x14B){ /* left */
            draw_ghost(0);
            draw_piece(cur_type,cur_rot,cur_row,cur_col,0);
            if(valid(cur_type,cur_rot,cur_row,cur_col-1)) cur_col--;
            draw_ghost(1);
            draw_piece(cur_type,cur_rot,cur_row,cur_col,1);
        }
        if(c==0x14D){ /* right */
            draw_ghost(0);
            draw_piece(cur_type,cur_rot,cur_row,cur_col,0);
            if(valid(cur_type,cur_rot,cur_row,cur_col+1)) cur_col++;
            draw_ghost(1);
            draw_piece(cur_type,cur_rot,cur_row,cur_col,1);
        }
        if(c==0x148){ /* rotate */
            draw_ghost(0);
            draw_piece(cur_type,cur_rot,cur_row,cur_col,0);
            int nr=(cur_rot+1)%4;
            /* Wall kick: try shifting if rotation fails */
            if(valid(cur_type,nr,cur_row,cur_col)) cur_rot=nr;
            else if(valid(cur_type,nr,cur_row,cur_col+1)){cur_rot=nr;cur_col++;}
            else if(valid(cur_type,nr,cur_row,cur_col-1)){cur_rot=nr;cur_col--;}
            draw_ghost(1);
            draw_piece(cur_type,cur_rot,cur_row,cur_col,1);
        }
        if(c==0x150){ /* soft drop */
            draw_ghost(0);
            draw_piece(cur_type,cur_rot,cur_row,cur_col,0);
            if(valid(cur_type,cur_rot,cur_row+1,cur_col)){cur_row++;score++;}
            draw_ghost(1);
            draw_piece(cur_type,cur_rot,cur_row,cur_col,1);
            drop_counter=0;
        }
        if(c==' '){ /* hard drop */
            draw_ghost(0);
            draw_piece(cur_type,cur_rot,cur_row,cur_col,0);
            while(valid(cur_type,cur_rot,cur_row+1,cur_col)){cur_row++;score+=2;}
            draw_piece(cur_type,cur_rot,cur_row,cur_col,1);
            drop_counter=9999; /* force lock next tick */
        }

        /* Auto drop - count ticks */
        drop_counter++;
        if(drop_counter*TICK_MS >= drop_ms()){
            drop_counter=0;
            draw_ghost(0);
            draw_piece(cur_type,cur_rot,cur_row,cur_col,0);
            if(valid(cur_type,cur_rot,cur_row+1,cur_col)){
                cur_row++;
                draw_ghost(1);
                draw_piece(cur_type,cur_rot,cur_row,cur_col,1);
            } else {
                /* Lock */
                place();
                /* Find cleared rows for flash */
                int crow[4]; int cc=0;
                for(int r=0;r<TH;r++){
                    int full=1;
                    for(int col=0;col<TW;col++) if(!board[r][col]){full=0;break;}
                    if(full&&cc<4) crow[cc++]=r;
                }
                if(cc>0) flash_lines(crow,cc);
                int cl=clear_lines();
                if(cl>0){
                    static const int pts[]={0,100,300,500,800};
                    score+=pts[cl>4?4:cl]*(level+1);
                    lines+=cl;
                    int new_level=lines/10;
                    if(new_level>level){level=new_level;music_levelup();}
                }
                draw_board();
                spawn_piece();
                draw_sidebar();
                if(!game_over){
                    draw_ghost(1);
                    draw_piece(cur_type,cur_rot,cur_row,cur_col,1);
                }
            }
        }
    }

    if(game_over) music_gameover();
    draw_board();
    vga_puts_at(" GAME OVER! ",BX+2,BY+8, VGA_WHITE,VGA_RED);
    char fs[20]; itoa(score,fs,10);
    vga_puts_at("Score:",BX+2,BY+10,VGA_LIGHT_GREY,VGA_BLACK);
    vga_puts_at(fs,BX+9,BY+10,VGA_YELLOW,VGA_BLACK);
    vga_puts_at("Any key...",BX+2,BY+12,VGA_DARK_GREY,VGA_BLACK);
    /* Drain keyboard buffer */
    while(keyboard_getchar()!=-1);
    /* Wait for real keypress */
    while(keyboard_getchar()==-1) __asm__ volatile("pause");
}
