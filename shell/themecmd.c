/* ================================================================
   shell/themecmd.c  --  `theme`.  v0.9.5.

   It used to print ten names and "Themes 2,3,4,6 have desktop
   patterns. Others: plain black." -- which was the honest summary of
   a theme set where every desktop was black. Now every theme is
   listed with a strip that IS the theme: its taskbar colour, then its
   desktop colour with its wallpaper drawn in, so you can see what you
   are choosing before you choose it.
   ================================================================ */
#include "types.h"
#include "vga.h"
#include "string.h"
#include "theme.h"

extern int atoi(const char*);

static char lc(char c){ return (c>='A'&&c<='Z')?(char)(c+32):c; }

/* "ocean", "Ocean", "oce" -> Ocean. */
static int theme_by_name(const char *w){
    int n=(int)strlen(w), hit=-1;
    for(int i=0;i<ui_theme_count();i++){
        const char *t=ui_theme_name(i);
        int k=0; while(k<n && t[k] && lc(t[k])==lc(w[k])) k++;
        if(k==n){ if(!t[k]) return i; if(hit<0) hit=i; }
    }
    return hit;
}

static void strip(int n){
    const theme_t *t=ui_theme_get(n);
    for(int i=0;i<3;i++){ vga_set_color(t->bar_fg,t->bar_bg); vga_putchar(i==1?'=':' '); }
    for(int i=0;i<13;i++){
        char c=' '; vga_color_t fg=t->desk_bg;
        /* The first thing the wallpaper draws in this column, looking a few
           rows down -- one row alone misses most of the sparser shapes. */
        for(int y=1;y<=12;y++)
            if(ui_pattern_at_theme(n, i*2, y, &c, &fg)) break;
        vga_set_color(fg,t->desk_bg);
        vga_putchar(c);
    }
    vga_set_color(t->accent,t->win_bg); vga_putchar(' ');
}

void cmd_theme(const char *arg){
    if(arg && arg[0]){
        int n = (arg[0]>='0'&&arg[0]<='9') ? atoi(arg) : theme_by_name(arg);
        if(n<0 || n>=ui_theme_count()){
            vga_set_color(VGA_LIGHT_RED,VGA_BLACK);
            if(n>=ui_theme_count() && n<ui_theme_total())
                vga_puts("That theme comes with the themes-extra package: bpkg install themes-extra\n");
            else
                vga_printf("theme: no theme '%s' -- 'theme' lists them\n",arg);
            vga_set_color(VGA_WHITE,VGA_BLACK);
            return;
        }
        ui_set_theme(n);
        vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
        vga_printf("Theme: %s (saved). The desktop shows it when you go back.\n",ui_theme_name(n));
        vga_set_color(VGA_WHITE,VGA_BLACK);
        return;
    }
    int cur=ui_current_theme();
    vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts("  Themes   -- theme <number or name> switches\n");
    for(int i=0;i<ui_theme_total();i++){
        int locked = i>=ui_theme_count();
        if(i==ui_theme_base_count()){
            vga_set_color(VGA_DARK_GREY,VGA_BLACK);
            vga_puts(locked ? "  With the themes-extra package (bpkg install themes-extra):\n"
                            : "  From themes-extra:\n");
        }
        vga_set_color(i==cur?VGA_LIGHT_GREEN:VGA_DARK_GREY,VGA_BLACK);
        vga_puts(i==cur?"> ":"  ");
        vga_set_color(locked?VGA_DARK_GREY:VGA_WHITE,VGA_BLACK);
        vga_printf("%2d  ",i);
        strip(i);
        vga_set_color(locked?VGA_DARK_GREY:(i==cur?VGA_LIGHT_GREEN:VGA_LIGHT_GREY),VGA_BLACK);
        vga_puts("  ");
        vga_puts(ui_theme_get(i)->name);
        if(i==cur){ vga_set_color(VGA_DARK_GREY,VGA_BLACK); vga_puts("   current"); }
        vga_putchar('\n');
    }
    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    vga_printf("  Wallpaper is %s: Settings > Appearance turns it %s.\n",
               ui_desk_pattern()?"on":"off", ui_desk_pattern()?"off":"on");
    vga_set_color(VGA_WHITE,VGA_BLACK);
}
