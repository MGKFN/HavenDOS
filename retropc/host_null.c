/* ════════════════════════════════════════════════════════════════════
   retropc/host_null.c -- Retro PC with no window.  v0.9.7.5.

   For `make test`, and for a machine with no display. The program is
   the same; this host types a script instead of a person, keeps a
   virtual clock, and prints the old PC's screen as text at the end.

     retropc-headless [--keys <script>] [--after <ms>] [--speed <M>]
                      [retropc's own arguments]

   THE SCRIPT is typed one key every 60 virtual ms, so each key
   arrives as its own interrupt the way a person's would. Escapes:
     \n Enter   \t Tab   \b Backspace   \e Esc   \\ a backslash
     {F1}..{F12} {UP} {DOWN} {LEFT} {RIGHT} {HOME} {END} {PGUP} {PGDN}
     {W<ms>}    wait that many virtual milliseconds
   After the last key it runs --after more ms (default 2000), then
   closes the window, and the program prints the screen and exits.

   THE CLOCK is virtual: one frame is 16 ms, and the program is told
   to run at most --speed million instructions per frame (default 2,
   so 125 million a second). So a run does exactly the same thing
   every time, on any machine, however fast it is.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "retropc.h"
#include "keys.h"

static uint32_t    T;                  /* virtual milliseconds        */
static const char *g_keys = "";
static uint32_t    g_next_key = 500;   /* the first key, after the boot starts */
static uint32_t    g_after = 2000, g_end;
static int         g_closed;

int  host_open(const char *t){ (void)t; return 0; }
void host_present(const uint32_t *px){ (void)px; }
void host_title(const char *t){ if(getenv("RETROPC_TITLES")) fprintf(stderr, "[title] %s\n", t); }
void host_close(void){}
uint32_t host_ms(void){ return T; }
void host_sleep(uint32_t ms){ (void)ms; T += 16; }
int  host_pick_file(char *o, int m){ (void)o; (void)m; return -1; }
const char *host_dropped(void){ return 0; }

static const struct { const char *name; int key; } g_named[] = {
    {"F1",KEY_F1},{"F2",KEY_F2},{"F3",KEY_F3},{"F4",KEY_F4},{"F5",0x13F},{"F6",0x140},
    {"F7",0x141},{"F8",0x142},{"F9",0x143},{"F10",0x144},{"F11",0x157},{"F12",0x158},
    {"UP",KEY_UP},{"DOWN",KEY_DOWN},{"LEFT",KEY_LEFT},{"RIGHT",KEY_RIGHT},
    {"HOME",KEY_HOME},{"END",KEY_END},{"PGUP",KEY_PGUP},{"PGDN",KEY_PGDN},
};

int host_key(void){
    if(g_closed) return RP_KEY_QUIT;
    while(*g_keys && T >= g_next_key){
        int k;
        if(*g_keys == '{'){
            const char *e = strchr(g_keys, '}');
            char name[16];
            size_t n;
            if(!e || (n = (size_t)(e - g_keys - 1)) >= sizeof name){ fprintf(stderr, "bad script at %s\n", g_keys); exit(2); }
            memcpy(name, g_keys + 1, n); name[n] = 0;
            g_keys = e + 1;
            if(name[0] == 'W'){ g_next_key = T + (uint32_t)atoi(name + 1); continue; }
            k = 0;
            for(unsigned i = 0; i < sizeof g_named / sizeof g_named[0]; i++)
                if(!strcmp(g_named[i].name, name)) k = g_named[i].key;
            if(!k){ fprintf(stderr, "no key {%s}\n", name); exit(2); }
        } else if(*g_keys == '\\' && g_keys[1]){
            char c = g_keys[1];
            g_keys += 2;
            k = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'b' ? '\b' : c == 'e' ? 27 : c;
        } else k = (unsigned char)*g_keys++;
        g_next_key = T + 60;
        if(!*g_keys) g_end = T + g_after;
        return k;
    }
    if(!*g_keys){
        /* a script that ends in {W..} waits that long first */
        if(!g_end) g_end = (g_next_key > T ? g_next_key : T) + g_after;
        if(T >= g_end){ g_closed = 1; return RP_KEY_QUIT; }
    }
    return RP_KEY_NONE;
}

int main(int argc, char **argv){
    char **rest = calloc((size_t)argc + 1, sizeof *rest);
    int n = 0;
    rest[n++] = argv[0];
    rp_dump = 1;
    rp_frame_cap = 2000000u;
    for(int i = 1; i < argc; i++){
        if(!strcmp(argv[i], "--keys") && i + 1 < argc) g_keys = argv[++i];
        else if(!strcmp(argv[i], "--after") && i + 1 < argc) g_after = (uint32_t)atoi(argv[++i]);
        else if(!strcmp(argv[i], "--speed") && i + 1 < argc) rp_frame_cap = (uint32_t)atoi(argv[++i]) * 1000000u;
        else rest[n++] = argv[i];
    }
    return rp_main(n, rest);
}
