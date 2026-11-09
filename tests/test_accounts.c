/* Can you log in to an account that first-run Setup just created?
 *
 * The v0.7.18 handoff left this open and called it blocking: Setup was walked
 * all the way through and the login screen was reached, but scripted keystrokes
 * never got in, so it was unclear whether the account was unusable or the
 * keystrokes were. Driving QEMU answers that badly — the setup wizard eats the
 * first character of a typed word often enough that a failure proves nothing.
 *
 * It is a pure-logic question, so it is answered here instead, against the real
 * shell/login.c: add_user() is exactly what Setup calls, try_login() is exactly
 * what the login screen calls, and both run below unmodified.
 *
 * The case that matters most is a 15-character username. add_user() does
 *
 *     strncpy(users[n].name, name, 15);
 *
 * into a char[16], and strncpy writes NO terminator when the source is already
 * 15 long. Whether that name is then findable by strcmp depends entirely on
 * what byte 15 of that slot happened to hold. Setup's own prompt says "3-15
 * characters", so it invites a person straight into that boundary.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* The OS headers come first, so every stub below matches the real prototype
   exactly. Guessing them produced eight conflicting-type errors: vga takes
   vga_color_t, vfs takes uint32_t lengths, rtc_get_date is (day, month, year)
   not (year, month, day), vfs_set_user takes a second argument, ui_load_theme
   takes none, and virtio_drive_sectors returns uint64_t. */
#include "../include/types.h"
#include "../include/vga.h"
#include "../include/theme.h"
#include "../include/rtc.h"

/* ── stubs: everything login.c reaches for that is not the account logic ── */
void vga_putchar_at(char c,int x,int y,vga_color_t f,vga_color_t b)
{ (void)c;(void)x;(void)y;(void)f;(void)b; }
void vga_puts_at(const char *s,int x,int y,vga_color_t f,vga_color_t b)
{ (void)s;(void)x;(void)y;(void)f;(void)b; }
void vga_set_cursor(int x,int y){ (void)x;(void)y; }
/* v0.7.19: the login tiles size themselves from the active mode, so the
   account logic now links against these too. */
int  vga_cols(void){ return 80; }
int  vga_rows(void){ return 25; }

int  keyboard_getchar(void){ return -1; }
int  keyboard_waitchar(void){ return 27; }
int  keyboard_haschar(void){ return 0; }
void sleep_ms(uint32_t m){ (void)m; }

/* login.c reboots the machine after three failed attempts. It used to
   do that by writing 0x64 itself; it now calls machine_reset(), which
   is the only thing in the tree that resets anything -- see
   include/machine.h and tools/check_machine_reset.py. Stubbed rather
   than linked, because kernel/machine.c would drag in the whole VGA
   layer and because what this suite tests is the account logic. The
   reset sequence has its own suite: tests/test_machine.c. */
void machine_reset(void){ }
void mouse_get(int *x,int *y,int *b){ (void)x;(void)y;(void)b; }
int  mouse_is_ready(void){ return 0; }

void rtc_get_time(uint8_t *h,uint8_t *m,uint8_t *s){ *h=12;*m=0;*s=0; }
void rtc_get_date(uint8_t *d,uint8_t *m,uint16_t *y){ *d=28;*m=9;*y=2026; }

/* The account file is held in memory, so accounts_save()/accounts_load()
   round-trip through this instead of a disk. That is the point: it lets the
   test prove a saved account can still be logged into after a reload. */
#define FAKEFS 4096
static char g_file[FAKEFS];
static int  g_filelen = -1;          /* -1 = the file does not exist */
int vfs_write(const char *p, const char *d, uint32_t n)
{
    (void)p;
    if(n > FAKEFS-1) n = FAKEFS-1;
    memcpy(g_file, d, (size_t)n);
    g_file[n] = 0;
    g_filelen = (int)n;
    return (int)n;
}
int vfs_read(const char *p, char *out, uint32_t max)
{
    (void)p;
    if(g_filelen < 0) return -1;
    uint32_t n = (uint32_t)g_filelen < max-1 ? (uint32_t)g_filelen : max-1;
    memcpy(out, g_file, (size_t)n);
    out[n] = 0;
    return (int)n;
}
void vfs_set_user(const char *u, int admin){ (void)u; (void)admin; }
void setup_user_home(const char *u){ (void)u; }
void shell_set_home(const char *u){ (void)u; }

static int  g_hidden[16];
int  cfg_acct_hidden(int i){ return (i>=0 && i<16) ? g_hidden[i] : 0; }
int  cfg_autologin(void){ return 0; }
int  cfg_autologin_timeout(void){ return 0; }
const char *cfg_autologin_user(void){ return ""; }

int  e1000_found(void){ return 0; }
int  rtl8139_found(void){ return 0; }
uint32_t pmm_total_blocks(void){ return 1024; }
int  virtio_drive_count(void){ return 0; }
uint64_t virtio_drive_sectors(int i){ (void)i; return 0; }

static theme_t g_theme;
theme_t *ui_theme(void){ return &g_theme; }
int  ui_current_theme(void){ return 0; }
void ui_load_theme(void){}
void ui_set_theme(int n){ (void)n; }
int  ui_theme_count(void){ return 1; }
const char *ui_theme_name(int i){ (void)i; return "Default"; }
void ui_draw_window(int x,int y,int w,int h,const char *t)
{ (void)x;(void)y;(void)w;(void)h;(void)t; }
void ui_draw_taskbar(const char *t){ (void)t; }

void utoa(uint32_t v, char *buf, int base)
{
    char tmp[12]; int n = 0;
    if(!v){ buf[0]='0'; buf[1]=0; return; }
    while(v && n < 11){ int d = (int)(v % (uint32_t)base);
                        tmp[n++] = (char)(d < 10 ? '0'+d : 'a'+d-10); v /= (uint32_t)base; }
    for(int i=0;i<n;i++) buf[i] = tmp[n-1-i];
    buf[n] = 0;
}

#include "../shell/login.c"

/* ── checks ──────────────────────────────────────────────────────── */
static int fails = 0, total = 0;
static void ck(const char *what, int cond, const char *detail)
{
    total++;
    if(cond) printf("  ok    %s\n", what);
    else { fails++; printf("  FAIL  %s\n          %s\n", what,
                           detail ? detail : ""); }
}
static void head(const char *t){ printf("\n%s\n", t); }

/* What Setup does, in the order it does it. */
static int setup_creates(const char *name, const char *pass, int admin)
{
    return add_user(name, pass, admin);
}

int main(void)
{
    printf("Setup-created accounts can be logged into\n");
    printf("========================================\n");

    accounts_load();

    head("the ordinary case");
    ck("Setup can create an account", setup_creates("kyan", "Secret1!", 1) == 1, "");
    ck("...and the login screen finds it",
       try_login("kyan", "Secret1!") >= 0, "try_login refused it");
    ck("a wrong password is refused",
       try_login("kyan", "Secret2!") < 0, "it accepted a wrong password");
    ck("an unknown account is refused",
       try_login("nobody", "Secret1!") < 0, "");
    ck("the administrator flag survives",
       users[user_count-1].admin == 1, "");

    head("the boundary Setup's own prompt invites: 15 characters");
    /* Setup says "Username (3-15 characters)". add_user copies 15 bytes into a
       char[16] with strncpy, which writes no terminator at exactly 15. */
    const char *n15 = "abcdefghijklmno";              /* exactly 15 */
    ck("the name really is 15 characters", strlen(n15) == 15, "");
    ck("Setup accepts a 15-character name",
       setup_creates(n15, "Secret1!", 0) == 1, "add_user refused it");
    ck("the stored name is NUL-terminated",
       users[user_count-1].name[15] == 0,
       "name[15] is not 0 - the name runs into the next field");
    ck("...and you can log in with it",
       try_login(n15, "Secret1!") >= 0,
       "a 15-character account cannot be logged into");

    head("a 15-character name after the slot has been reused");
    /* The slot a new account lands in may have held a previous one. If byte 15
       is not cleared, the SECOND 15-character name is the one that breaks. */
    ck("a 16th-character name is refused outright",
       setup_creates("abcdefghijklmnop", "Secret1!", 0) == 0, "");
    /* delete_user() is admin-gated, which is correct; become the admin
       account the way a real session would before removing anything. */
    logged_in_idx = try_login("kyan", "Secret1!");
    ck("the admin account is the one signed in", current_user_admin() == 1, "");
    ck("the account can be removed", delete_user(n15) == 0, "delete_user refused");
    ck("...and no longer logs in", try_login(n15, "Secret1!") < 0, "");
    const char *m15 = "ABCDEFGHIJKLMNO";
    ck("a second 15-character account can be created",
       setup_creates(m15, "Secret1!", 0) == 1, "");
    ck("it is terminated too", users[user_count-1].name[15] == 0,
       "byte 15 survived from the previous occupant");
    ck("and it can be logged into", try_login(m15, "Secret1!") >= 0,
       "the reused slot left the name unterminated");

    head("Setup offers a blank password");
    ck("an account with no password can be created",
       setup_creates("guestish", "", 0) == 1, "");
    ck("...and signed into with an empty password",
       try_login("guestish", "") >= 0, "");
    ck("a blank password does not open another account",
       try_login("kyan", "") < 0, "empty password matched a real one");

    head("passwords with the characters people actually use");
    ck("specials are accepted",
       setup_creates("specials", "p@ss W0rd!#$%", 0) == 1, "");
    ck("...and match exactly",
       try_login("specials", "p@ss W0rd!#$%") >= 0, "");
    ck("one character different is refused",
       try_login("specials", "p@ss W0rd!#$&") < 0, "");

    head("names are compared exactly");
    ck("a different case is a different account",
       try_login("KYAN", "Secret1!") < 0,
       "login is case-insensitive, which add_user's duplicate check is not");
    ck("a duplicate name is refused",
       setup_creates("kyan", "Other1!", 0) == 0, "");

    head("the account survives being written and read back");
    accounts_save();
    /* Wipe the in-memory table and reload from the fake file, which is what a
       reboot does. An account that only exists in RAM would pass every check
       above and still be unusable on the next boot. */
    user_count = 0;
    memset(users, 0, sizeof(users));
    accounts_load();
    ck("the account is still there after a reload",
       try_login("kyan", "Secret1!") >= 0,
       "the account did not survive accounts_save/accounts_load");
    ck("the 15-character one survived too",
       try_login(m15, "Secret1!") >= 0,
       "a 15-character name did not survive the round trip");
    ck("its password still does not match a wrong one",
       try_login("kyan", "nope") < 0, "");

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
