/* ================================================================
   shell/apps2.c  --  three new apps for v0.9.5.1.

     Stopwatch    a stopwatch with laps, and a countdown timer that beeps
     Converter    length, weight, temperature, data size, speed, time
     Notes        short notes, kept in your home folder

   Drawn in the 0.9.5.1 look (shell/ui.c). Laid out for 80x25; the
   desktop centres that on a bigger console.

   No floating point anywhere: the kernel never sets up the FPU for
   apps, and 32-bit code dividing 64-bit numbers pulls in libgcc, which
   the kernel does not link. The converter carries values in millionths
   and divides with its own 64-by-32 long division.
   ================================================================ */
#include "types.h"
#include "string.h"
#include "vga.h"
#include "keys.h"
#include "theme.h"
#include "task.h"      /* v0.9.6: TP_RUN while the stopwatch counts */

extern int      keyboard_getchar(void);
extern int      keyboard_waitchar(void);
extern void     sleep_ms(uint32_t);
extern uint32_t get_ticks(void);
extern void     speaker_beep(uint32_t freq, uint32_t ms);
extern void     speaker_tone(uint32_t freq);   /* v0.9.6 */
extern void     speaker_off(void);
extern void     kpoll_sleep_ms(uint32_t ms);
extern int      vfs_read(const char*, char*, uint32_t);
extern int      vfs_write(const char*, const char*, uint32_t);
extern int      vfs_mkdir(const char*);
extern const char *current_username(void);

static void ap_put(int x,int y,const char *s,vga_color_t fg,vga_color_t bg){
    for(;*s && x<vga_cols();s++,x++) vga_putchar_at(*s,x,y,fg,bg);
}
static void ap_fill(int x,int y,int w,int h,vga_color_t bg){
    for(int r=0;r<h;r++) for(int c=0;c<w;c++) vga_putchar_at(' ',x+c,y+r,VGA_WHITE,bg);
}

/* ── big digits, 3 cells wide and 5 tall, drawn as solid blocks ───── */
static const uint16_t font35[12] = {
    /* 15 bits, row-major, top-left first: 0-9, ':', '.' */
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF,
    0x0410, 0x0002
};
static int glyph(char c){ return c>='0'&&c<='9' ? c-'0' : c==':' ? 10 : c=='.' ? 11 : -1; }
/* Each font cell is two screen columns wide so the digits are square-ish. */
static int big_width(const char *s){ int w=0; for(;*s;s++) w += (glyph(*s)>=10 ? 3 : 7); return w; }
static void big_text(int x,int y,const char *s,vga_color_t c){
    for(;*s;s++){
        int g=glyph(*s);
        if(g<0){ x+=7; continue; }
        int cols = g>=10 ? 1 : 3;
        for(int r=0;r<5;r++) for(int k=0;k<3;k++){
            int bit = 14-(r*3+k);
            if(g>=10 && k!=1) continue;
            if(font35[g]>>bit & 1){
                int xx = x + (g>=10 ? 0 : k*2);
                vga_putchar_at(' ',xx,y+r,c,c);
                if(g<10) vga_putchar_at(' ',xx+1,y+r,c,c);
            }
        }
        x += (cols==1 ? 3 : 7);
    }
}

static void two(char *o, uint32_t v){ o[0]=(char)('0'+(v/10)%10); o[1]=(char)('0'+v%10); }

/* hh:mm:ss.cc from hundredths (one PIT tick each). */
static void fmt_time(uint32_t cs, char *o, int with_hours){
    uint32_t s=cs/100, h=s/3600, m=(s/60)%60; s%=60;
    int n=0;
    if(with_hours){ two(o+n,h); n+=2; o[n++]=':'; }
    two(o+n,m); n+=2; o[n++]=':'; two(o+n,s); n+=2;
    o[n++]='.'; two(o+n,cs%100); n+=2; o[n]=0;
}

/* ================================================================
   STOPWATCH AND TIMER
   ================================================================ */
void app_stopwatch(void){
    int mode=0;                         /* 0 stopwatch, 1 timer */
    int running=0;
    uint32_t base=0, acc=0;             /* stopwatch: ticks accumulated   */
    uint32_t laps[8]; int nlaps=0;
    uint32_t tset=5*60*100, tleft=tset; /* timer, hundredths              */
    int rang=0;
    vga_color_t sf,sb; ui_sel_colors(&sf,&sb); (void)sf;

    for(;;){
        uint32_t now=get_ticks();
        uint32_t elapsed = acc + (running ? now-base : 0);
        if(mode==1 && running){
            uint32_t gone = now-base;
            tleft = gone>=acc ? 0 : acc-gone;
            if(!tleft && !rang){
                running=0; rang=1;
                /* v0.9.6: timed by the clock and yielding, so a timer
                   that rings behind the front does not stall the app
                   in front for the length of the alarm. */
                for(int i=0;i<3;i++){
                    speaker_tone(880); kpoll_sleep_ms(150);
                    speaker_off();     kpoll_sleep_ms(100);
                }
            }
        }

        vga_begin_frame();
        ui_kit_begin(mode? "Timer" : "Stopwatch",
                     mode? "Space start/stop   Up/Down minutes   R reset   Tab stopwatch   Q quit"
                         : "Space start/stop   L lap   R reset   Tab timer   Q quit");
        /* mode switch */
        ap_put(28,2," Stopwatch ",mode?VGA_LIGHT_GREY:VGA_BLACK,mode?VGA_BLACK:sb);
        ap_put(41,2," Timer ",mode?VGA_BLACK:VGA_LIGHT_GREY,mode?sb:VGA_BLACK);

        char t[16];
        if(mode==0) fmt_time(elapsed,t,elapsed>=360000);
        else        fmt_time(tleft,t,tleft>=360000);
        vga_color_t dc = mode==1 && rang ? VGA_RED : running ? VGA_GREEN : sb;
        big_text((80-big_width(t))/2,6,t,dc);
        ap_put((80-12)/2,12, running?"   running  ":(mode==1&&rang?"  time's up ":"   stopped  "),
            running?VGA_LIGHT_GREEN:VGA_DARK_GREY,VGA_BLACK);

        if(mode==0){
            for(int i=0;i<nlaps;i++){
                char l[32]="Lap  "; l[4]=(char)('1'+i);
                char lt[16]; fmt_time(laps[i],lt,0);
                ap_put(30,14+i,l,VGA_DARK_GREY,VGA_BLACK);
                ap_put(38,14+i,lt,VGA_WHITE,VGA_BLACK);
                if(i){ char d[16]; fmt_time(laps[i]-laps[i-1],d,0);
                       ap_put(50,14+i,"+",VGA_DARK_GREY,VGA_BLACK); ap_put(52,14+i,d,VGA_DARK_GREY,VGA_BLACK); }
            }
        } else {
            char st[16]; fmt_time(tset,st,0);
            ap_put(30,14,"Set to",VGA_DARK_GREY,VGA_BLACK); ap_put(38,14,st,VGA_WHITE,VGA_BLACK);
        }
        vga_end_frame();

        /* v0.9.6: it runs behind the front only while it is counting.
           Stopped, it has nothing to do there, so it pauses like any
           app instead of redrawing an unchanging time in a console
           nobody can see. */
        task_set_policy(running ? TP_RUN : TP_PAUSE);
        int k=keyboard_getchar();
        /* v0.9.6: the Stopwatch keeps running behind the front (TK_RUN
           in the registry), so its idle wait must let the others run:
           kpoll_sleep_ms() yields, sleep_ms() would hold the app in
           front up for 30 ms every frame. */
        if(k==-1){ kpoll_sleep_ms(30); continue; }
        if(k=='q'||k=='Q'||k==27) return;
        if(k=='\t'){
            mode=!mode; running=0; rang=0;
            if(mode) tleft=tset; else { acc=0; nlaps=0; }
            continue;
        }
        if(k==' '){
            if(mode==0){
                if(running){ acc+=get_ticks()-base; running=0; }
                else { base=get_ticks(); running=1; }
            } else {
                if(running){ tleft = tleft; acc=tleft; running=0; }
                else if(tleft){ base=get_ticks(); acc=tleft; running=1; rang=0; }
            }
        }
        if(k=='r'||k=='R'){
            running=0; rang=0;
            if(mode==0){ acc=0; nlaps=0; } else tleft=tset;
        }
        if(mode==0 && (k=='l'||k=='L') && running && nlaps<8) laps[nlaps++]=elapsed;
        if(mode==1 && !running){
            if(k==KEY_UP   && tset < 99*60*100) { tset+=60*100; tleft=tset; rang=0; }
            if(k==KEY_DOWN && tset > 60*100)    { tset-=60*100; tleft=tset; rang=0; }
        }
    }
}

/* ================================================================
   CONVERTER
   ================================================================ */
/* n / d for a 64-bit n and a 32-bit d, without libgcc. */
static uint64_t udiv64(uint64_t n, uint32_t d, uint32_t *rem){
    uint64_t q=0, r=0;
    for(int i=63;i>=0;i--){
        r = (r<<1) | ((n>>i)&1);
        if(r>=d){ r-=d; q |= (uint64_t)1<<i; }
    }
    if(rem) *rem=(uint32_t)r;
    return q;
}
static int64_t sdiv64(int64_t n, uint32_t d){
    if(n<0) return -(int64_t)udiv64((uint64_t)(-n),d,0);
    return (int64_t)udiv64((uint64_t)n,d,0);
}

typedef struct { const char *name; uint32_t num, den; } unit_t;   /* value in base = v*num/den */
typedef struct { const char *name; const unit_t *u; int n; } cat_t;

static const unit_t u_len[] = { {"millimetres",1,1000},{"centimetres",1,100},{"metres",1,1},
    {"kilometres",1000,1},{"inches",254,10000},{"feet",3048,10000},{"yards",9144,10000},{"miles",1609344,1000} };
static const unit_t u_mass[] = { {"grams",1,1000},{"kilograms",1,1},{"tonnes",1000,1},
    {"ounces",28349523,1000000000},{"pounds",45359237,100000000},{"stone",635029318,100000000} };
static const unit_t u_data[] = { {"bytes",1,1},{"kilobytes (KB)",1024,1},{"megabytes (MB)",1048576,1},
    {"gigabytes (GB)",1073741824,1},{"bits",1,8} };
static const unit_t u_speed[] = { {"km per hour",1000,3600},{"metres per second",1,1},
    {"miles per hour",1609344,3600000},{"knots",1852,3600} };
static const unit_t u_time[] = { {"seconds",1,1},{"minutes",60,1},{"hours",3600,1},{"days",86400,1},{"weeks",604800,1} };
static const unit_t u_temp[] = { {"Celsius",0,0},{"Fahrenheit",0,0},{"Kelvin",0,0} };

static const cat_t cats[] = {
    {"Length",u_len,8},{"Weight",u_mass,6},{"Temperature",u_temp,3},
    {"Data size",u_data,5},{"Speed",u_speed,4},{"Time",u_time,5},
};
#define NCATS 6
#define MICRO 1000000

/* Parse "-12.5" into millionths. 0 on garbage. */
static int parse_micro(const char *s, int64_t *out){
    int neg=0; int64_t v=0, frac=0; int fd=0, any=0;
    if(*s=='-'){ neg=1; s++; }
    while(*s>='0'&&*s<='9'){ v=v*10+(*s-'0'); s++; any=1; if(v>2000000000LL) return 0; }
    if(*s=='.'){ s++; while(*s>='0'&&*s<='9'){ if(fd<6){ frac=frac*10+(*s-'0'); fd++; } s++; any=1; } }
    if(*s || !any) return 0;
    while(fd<6){ frac*=10; fd++; }
    *out = (v*MICRO+frac) * (neg?-1:1);
    return 1;
}
static void fmt_micro(int64_t v, char *o){
    int n=0;
    if(v<0){ o[n++]='-'; v=-v; }
    uint32_t fr; uint64_t ip=udiv64((uint64_t)v,MICRO,&fr);
    char t[24]; int k=0;
    do{ uint32_t r; ip=udiv64(ip,10,&r); t[k++]=(char)('0'+r); }while(ip);
    while(k) o[n++]=t[--k];
    /* up to six decimals, trailing zeros dropped */
    char f[7]; for(int i=5;i>=0;i--){ f[i]=(char)('0'+fr%10); fr/=10; } f[6]=0;
    int last=5; while(last>=0 && f[last]=='0') last--;
    if(last>=0){ o[n++]='.'; for(int i=0;i<=last;i++) o[n++]=f[i]; }
    o[n]=0;
}

static int64_t to_base(const cat_t *c, int u, int64_t v){
    if(c->u==u_temp){            /* to Celsius */
        if(u==1) return sdiv64((v-32*(int64_t)MICRO)*5,9);
        if(u==2) return v-273150000LL;
        return v;
    }
    return sdiv64(v*(int64_t)c->u[u].num, c->u[u].den);
}
static int64_t from_base(const cat_t *c, int u, int64_t v){
    if(c->u==u_temp){
        if(u==1) return sdiv64(v*9,5)+32*(int64_t)MICRO;
        if(u==2) return v+273150000LL;
        return v;
    }
    return sdiv64(v*(int64_t)c->u[u].den, c->u[u].num);
}

void app_convert(void){
    int cat=0, from=0, field=0;            /* field: 0 category 1 from 2 value */
    char in[20]="1"; int inl=1;
    vga_color_t sf,sb; ui_sel_colors(&sf,&sb);
    for(;;){
        const cat_t *c=&cats[cat];
        if(from>=c->n) from=0;
        vga_begin_frame();
        ui_kit_begin("Converter","Tab next field   Up/Down change   type a number   Q or ESC quit");
        ui_kit_heading(4,2,"Convert");
        /* category tabs */
        int x=4;
        for(int i=0;i<NCATS;i++){
            int on=(i==cat);
            vga_color_t bg = on ? (field==0?sb:VGA_DARK_GREY) : VGA_BLACK;
            vga_color_t fg = on ? (field==0?sf:VGA_WHITE) : VGA_LIGHT_GREY;
            ap_put(x,4," ",fg,bg); ap_put(x+1,4,cats[i].name,fg,bg);
            ap_put(x+1+(int)strlen(cats[i].name),4," ",fg,bg);
            x += (int)strlen(cats[i].name)+3;
        }
        ap_put(4,7,"From",VGA_DARK_GREY,VGA_BLACK);
        ap_fill(12,7,24,1,field==1?sb:VGA_BLACK);
        ap_put(13,7,c->u[from].name,field==1?sf:VGA_WHITE,field==1?sb:VGA_BLACK);
        ap_put(4,9,"Value",VGA_DARK_GREY,VGA_BLACK);
        ap_fill(12,9,24,1,field==2?sb:VGA_BLACK);
        ap_put(13,9,in,field==2?sf:VGA_WHITE,field==2?sb:VGA_BLACK);
        if(field==2) ap_put(13+inl,9,"_",sf,sb);

        int64_t v;
        if(!parse_micro(in,&v)){
            ap_put(4,12,"Type a number, like 12 or 3.75 or -40.",VGA_YELLOW,VGA_BLACK);
        } else {
            int64_t b=to_base(c,from,v);
            ui_kit_heading(4,12,"Is the same as");
            for(int i=0;i<c->n;i++){
                if(i==from) continue;
                char o[40]; fmt_micro(from_base(c,i,b),o);
                int y=14+(i>from?i-1:i);
                ap_put(6,y,o,VGA_WHITE,VGA_BLACK);
                ap_put(6+(int)strlen(o)+1,y,c->u[i].name,VGA_LIGHT_GREY,VGA_BLACK);
            }
        }
        vga_end_frame();

        int k=keyboard_waitchar();
        if(k==27||((k=='q'||k=='Q')&&field!=2)) return;
        if(k=='\t'||k=='\n'||k=='\r'){ field=(field+1)%3; continue; }
        if(field==0){
            if(k==KEY_RIGHT||k==KEY_DOWN) cat=(cat+1)%NCATS;
            if(k==KEY_LEFT ||k==KEY_UP)   cat=(cat+NCATS-1)%NCATS;
        } else if(field==1){
            if(k==KEY_DOWN||k==KEY_RIGHT) from=(from+1)%c->n;
            if(k==KEY_UP  ||k==KEY_LEFT)  from=(from+c->n-1)%c->n;
        } else {
            if(k=='\b'){ if(inl) in[--inl]=0; }
            else if(((k>='0'&&k<='9')||k=='.'||(k=='-'&&inl==0)) && inl<16){ in[inl++]=(char)k; in[inl]=0; }
            else if(k=='q'||k=='Q') return;
        }
    }
}

/* ================================================================
   NOTES
   ================================================================ */
#define NOTES_MAX 18
#define NOTE_LEN  64
static char notes[NOTES_MAX][NOTE_LEN];
static int  nnotes;

static void notes_path(char *p){
    const char *u=current_username(); int n=0;
    const char *h="HOME/"; while(*h) p[n++]=*h++;
    while(*u && n<40) p[n++]=*u++;
    p[n]=0;
    vfs_mkdir(p);
    h="/NOTES.TXT"; while(*h) p[n++]=*h++;
    p[n]=0;
}
static void notes_load(void){
    static char buf[NOTES_MAX*(NOTE_LEN+1)+1];
    char p[64]; notes_path(p);
    nnotes=0;
    int r=vfs_read(p,buf,sizeof buf-1);
    if(r<=0) return;
    buf[r]=0;
    for(char *s=buf; *s && nnotes<NOTES_MAX; ){
        char *e=s; while(*e && *e!='\n') e++;
        int l=(int)(e-s); if(l>NOTE_LEN-1) l=NOTE_LEN-1;
        if(l>0){ memcpy(notes[nnotes],s,(size_t)l); notes[nnotes][l]=0; nnotes++; }
        s = *e ? e+1 : e;
    }
}
static int notes_save(void){
    static char buf[NOTES_MAX*(NOTE_LEN+1)+1];
    int n=0;
    for(int i=0;i<nnotes;i++){
        for(const char *c=notes[i];*c;c++) buf[n++]=*c;
        buf[n++]='\n';
    }
    char p[64]; notes_path(p);
    return vfs_write(p,buf,(uint32_t)n) >= 0;
}

/* One line of input on row y; returns 1 on Enter. Starts from `buf`. */
static int notes_edit(int y, char *buf){
    int n=(int)strlen(buf);
    vga_color_t sf,sb; ui_sel_colors(&sf,&sb);
    for(;;){
        ap_fill(3,y,74,1,sb);
        ap_put(4,y,buf,sf,sb);
        ap_put(4+n,y,"_",sf,sb);
        ui_kit_footer("Type the note   Enter keeps it   ESC cancels");
        vga_flush();
        int k=keyboard_waitchar();
        if(k==27) return 0;
        if(k=='\n'||k=='\r') return n>0;
        if(k=='\b'){ if(n) buf[--n]=0; continue; }
        if(k>=32 && k<127 && n<NOTE_LEN-1 && n<70){ buf[n++]=(char)k; buf[n]=0; }
    }
}

void app_notes(void){
    notes_load();
    int sel=0;
    const char *msg=0;
    for(;;){
        if(sel>=nnotes) sel = nnotes ? nnotes-1 : 0;
        vga_begin_frame();
        ui_kit_begin("Notes","A add   Enter edit   D delete   Up/Down choose   Q quit");
        {
            char p[64]; notes_path(p);
            ap_put(3,2,"Saved in",VGA_DARK_GREY,VGA_BLACK);
            ap_put(12,2,p,VGA_LIGHT_GREY,VGA_BLACK);
        }
        if(!nnotes){
            ap_put(3,5,"No notes yet.",VGA_WHITE,VGA_BLACK);
            ap_put(3,6,"Press A to write one.",VGA_DARK_GREY,VGA_BLACK);
        }
        for(int i=0;i<nnotes;i++){
            char t[80]; int n=0;
            t[n++]=' ';
            for(const char *c=notes[i];*c&&n<72;c++) t[n++]=*c;
            t[n]=0;
            ui_kit_row(3,4+i,74,i==sel,t);
            if(i!=sel) vga_putchar_at(' ',3,4+i,VGA_BLACK,ui_theme()->accent&7?(vga_color_t)(ui_theme()->accent&7):VGA_LIGHT_GREY);
        }
        if(msg) ap_put(3,23,msg,VGA_YELLOW,VGA_BLACK);
        vga_end_frame();
        msg=0;

        int k=keyboard_waitchar();
        if(k=='q'||k=='Q'||k==27) return;
        if(k==KEY_UP && sel>0) sel--;
        if(k==KEY_DOWN && sel<nnotes-1) sel++;
        if(k=='a'||k=='A'){
            if(nnotes>=NOTES_MAX){ msg="That is eighteen notes -- delete one first."; continue; }
            char b[NOTE_LEN]="";
            if(notes_edit(4+nnotes,b)){
                memcpy(notes[nnotes],b,NOTE_LEN); nnotes++; sel=nnotes-1;
                msg = notes_save() ? 0 : "Could not save -- is this a live session?";
            }
        }
        if((k=='\n'||k=='\r') && nnotes){
            char b[NOTE_LEN]; memcpy(b,notes[sel],NOTE_LEN);
            if(notes_edit(4+sel,b)){ memcpy(notes[sel],b,NOTE_LEN); msg = notes_save()?0:"Could not save."; }
        }
        if((k=='d'||k=='D'||k==KEY_DEL) && nnotes){
            for(int i=sel;i<nnotes-1;i++) memcpy(notes[i],notes[i+1],NOTE_LEN);
            nnotes--;
            msg = notes_save()?0:"Could not save.";
        }
    }
}
