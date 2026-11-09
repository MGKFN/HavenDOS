/* ════════════════════════════════════════════════════════════════════
   shell/rsacmd.c -- the `rsa` command.  v0.8.8.3.

   WHY THIS EXISTS

   net/rsa.c shipped in Helios beta 2 with Miller-Rabin, a stepping RSA
   key generator, 361 suite checks and an injection harness that
   catches all eighteen of its planted defects -- and NOTHING IN THE
   SHELL CALLED ANY OF IT. There was no way to type a command on the
   laptop and find out whether a key generates there at all.

   That is the same shape as `test_rsa` sitting in the tree without
   being wired into `make test`: the work existed and could not be
   reached.

   ── AND IT ANSWERS A QUESTION THE HANDOFF HAS BEEN GUESSING AT ──────

   "A full RSA-2048 keygen is 15 to 25 minutes" has been in HANDOFF.md
   since the bignum was scoped. That figure came from measuring the OLD
   byte-wise bn_powmod in net/tls_crypto.c at 0.073 s per exponent
   byte. net/bignum.c replaced the inner loop with Montgomery
   multiplication and nobody has measured anything since.

   So this prints the elapsed time and the number of candidates
   examined, and one run on real hardware replaces a year-old estimate
   of code that no longer exists. THE NUMBER IT PRINTS IS THE ANSWER,
   not the number in the handoff.

   ── IT STEPS, AND THAT IS THE WHOLE POINT OF THE TICK ───────────────

   rsa_keygen_step() does one bounded piece of work and returns. This
   loop calls it, polls, prints progress, and checks for a keypress
   between steps -- so a keygen that is going to take minutes looks
   like a machine working rather than a machine hung, and ESC stops
   it.

   That was the argument for doing Helios before stage 3 and it is the
   first thing to actually use it.
   ════════════════════════════════════════════════════════════════════ */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/vga.h"
#include "../include/bignum.h"
#include "../include/rsa.h"
#include "../include/kpoll.h"

extern uint32_t get_ticks(void);
extern int      keyboard_haschar(void);
extern char     keyboard_getchar(void);

static void up(const char *s){ vga_puts(s); }

static void putu(uint32_t v, int width){
    char b[12]; int n = 0;
    if(v == 0) b[n++] = '0';
    while(v){ b[n++] = (char)('0' + v % 10); v /= 10; }
    while(width-- > n) vga_putchar(' ');
    while(n) vga_putchar(b[--n]);
}

/* Seconds and hundredths from a tick count. The PIT runs at 100 Hz, so
   a tick IS a hundredth -- written out rather than assumed, because a
   timer that is reprogrammed later would silently rescale this. */
#define TICKS_PER_SEC 100u

static void put_elapsed(uint32_t ticks)
{
    uint32_t s = ticks / TICKS_PER_SEC;
    uint32_t h = ticks % TICKS_PER_SEC;
    if(s >= 60u){ putu(s / 60u, 0); up("m "); putu(s % 60u, 0); }
    else putu(s, 0);
    up(".");
    if(h < 10u) up("0");
    putu(h, 0);
    up("s");
}

/* A big integer as hex, truncated with a note. A 2048-bit modulus is
   512 hex digits and nobody reads that off a screen; the first and
   last few plus the bit count is what tells you it is the right
   shape. */
static void put_bn_brief(const bn_t *a)
{
    uint8_t buf[BN_BITS / 8];
    uint32_t bits = bn_bits(a);
    uint32_t nbytes = (bits + 7u) / 8u;
    uint32_t i;
    const char *hex = "0123456789abcdef";

    if(nbytes == 0){ up("0"); return; }
    if(!bn_to_bytes(a, buf, nbytes)){ up("<too wide to print>"); return; }
    for(i = 0; i < nbytes && i < 6u; i++){
        vga_putchar(hex[buf[i] >> 4]);
        vga_putchar(hex[buf[i] & 15]);
    }
    if(nbytes > 12u){
        up("...");
        for(i = nbytes - 6u; i < nbytes; i++){
            vga_putchar(hex[buf[i] >> 4]);
            vga_putchar(hex[buf[i] & 15]);
        }
    }
    up("  (");
    putu(bits, 0);
    up(" bits)");
}

/* ── the generated key, kept so `rsa show` can look at it again ──────
   One key at a time. A shell that could hold several would need a
   place to put them and a way to name them, and neither exists; a
   second `rsa keygen` replaces the first and says so. */
static rsa_keygen_t g_key;
static int          g_have_key = 0;
static uint32_t     g_key_ticks = 0;
static uint32_t     g_key_tried = 0;

static void show_key(void)
{
    up("  modulus n   "); put_bn_brief(&g_key.n); up("\n");
    up("  exponent e  "); put_bn_brief(&g_key.e); up("\n");
    up("  private d   "); put_bn_brief(&g_key.d); up("\n");
    up("  prime p     "); put_bn_brief(&g_key.p); up("\n");
    up("  prime q     "); put_bn_brief(&g_key.q); up("\n");
    up("  lambda      "); put_bn_brief(&g_key.lam); up("\n");
    up("\n  took        "); put_elapsed(g_key_ticks);
    up(", "); putu(g_key_tried, 0); up(" candidates examined\n");
}

/* ── the checks that are worth running on the key itself ─────────────
   Not "it returned DONE": the properties. p*q == n, d*e == 1 mod
   lambda, and a round trip through both exponents. A key that fails
   any of these is not a key, and finding that out here costs a
   second and finding it out during a pair request costs an
   afternoon. */
static void verify_key(void)
{
    static bn_t t, one, msg, ct, pt;
    int ok = 1;

    bn_set_u32(&one, 1);

    up("  p * q == n                    ");
    if(bn_mul(&t, &g_key.p, &g_key.q) && bn_cmp(&t, &g_key.n) == 0) up("yes\n");
    else { vga_set_color(VGA_LIGHT_RED, VGA_BLACK); up("NO\n");
           vga_set_color(VGA_WHITE, VGA_BLACK); ok = 0; }

    up("  d * e == 1 mod lambda         ");
    if(bn_mulmod(&t, &g_key.d, &g_key.e, &g_key.lam) && bn_cmp(&t, &one) == 0)
        up("yes\n");
    else { vga_set_color(VGA_LIGHT_RED, VGA_BLACK); up("NO\n");
           vga_set_color(VGA_WHITE, VGA_BLACK); ok = 0; }

    up("  p and q are prime (16 rounds) ");
    if(rsa_is_probable_prime(&g_key.p, 16) == 1 &&
       rsa_is_probable_prime(&g_key.q, 16) == 1) up("yes\n");
    else { vga_set_color(VGA_LIGHT_RED, VGA_BLACK); up("NO\n");
           vga_set_color(VGA_WHITE, VGA_BLACK); ok = 0; }

    /* THE ROUND TRIP, which is what the key is for. Encrypt with e,
       decrypt with d, and the message has to come back. */
    up("  m^e mod n then ^d mod n == m  ");
    bn_set_u32(&msg, 0x5A5A5A5Au);
    if(bn_modexp(&ct, &msg, &g_key.e, &g_key.n) &&
       bn_modexp(&pt, &ct, &g_key.d, &g_key.n) &&
       bn_cmp(&pt, &msg) == 0) up("yes\n");
    else { vga_set_color(VGA_LIGHT_RED, VGA_BLACK); up("NO\n");
           vga_set_color(VGA_WHITE, VGA_BLACK); ok = 0; }

    /* And the signing direction, which is the one stage 3 calls. */
    up("  m^d mod n then ^e mod n == m  ");
    if(bn_modexp(&ct, &msg, &g_key.d, &g_key.n) &&
       bn_modexp(&pt, &ct, &g_key.e, &g_key.n) &&
       bn_cmp(&pt, &msg) == 0) up("yes\n");
    else { vga_set_color(VGA_LIGHT_RED, VGA_BLACK); up("NO\n");
           vga_set_color(VGA_WHITE, VGA_BLACK); ok = 0; }

    up("\n");
    if(ok){
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        up("  Every property holds. This is a usable RSA key.\n");
    } else {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        up("  SOMETHING IS WRONG WITH THIS KEY. Report the lines\n");
        up("  above that say NO -- they are different bugs.\n");
    }
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

static uint32_t parse_bits(const char *s)
{
    uint32_t v = 0;
    if(!s || !*s) return 2048u;
    while(*s >= '0' && *s <= '9'){ v = v * 10u + (uint32_t)(*s - '0'); s++; }
    if(*s) return 0;                   /* trailing rubbish: refuse     */
    return v;
}

static void do_keygen(const char *arg)
{
    uint32_t bits = parse_bits(arg);
    uint32_t t0, last_print, tried_at_print = 0;
    int r, pct_last = -1;

    if(bits == 0){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        up("rsa keygen: that is not a number of bits.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }

    r = rsa_keygen_begin(&g_key, bits);
    if(r != RSA_OK){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        up("rsa keygen: "); up(rsa_why_name(r)); up("\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        up("  512 to 2048 bits, and an even number.\n");
        return;
    }
    g_have_key = 0;

    up("Generating a "); putu(bits, 0); up("-bit RSA key.\n");
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    up("  Press ESC to stop. The machine stays responsive -- this is\n");
    up("  what the v0.8.8 timer tick was built for.\n\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);

    t0 = get_ticks();
    last_print = t0;

    for(;;){
        r = rsa_keygen_step(&g_key);
        if(r != RSA_KG_WORKING) break;

        /* THE BOTTOM HALF RUNS. Without this the DHCP retries, the
           carrier poll and USB hot-plug all stop for the length of
           the keygen, and on a long one that is minutes of a machine
           that looks alive and is not listening to its network.

           include/kpoll.h restricts where this may be called, because
           the work it runs reaches the USB core, the block layer and
           the network stack, and each keeps a buffer that is not
           reentrant. This is a genuine idle point: rsa_keygen_step()
           has returned, the generator's whole state is in the
           rsa_keygen_t, and nothing in any of those three layers is
           part-way through anything. It is the same position the
           keyboard's idle loop is in, which is the other place on the
           list. */
        kernel_poll();   /* kpoll-site-ok: between keygen steps nothing
                            is in progress; the generator's state lives
                            entirely in the rsa_keygen_t */

        /* ESC stops it. A keygen is the longest thing this system
           does and the one most likely to be started by accident. */
        if(keyboard_haschar()){
            char c = keyboard_getchar();
            if(c == 27){
                up("\n");
                vga_set_color(VGA_YELLOW, VGA_BLACK);
                up("Stopped. No key was generated.\n");
                vga_set_color(VGA_WHITE, VGA_BLACK);
                return;
            }
        }

        /* Progress, at most five times a second, and only when it has
           actually moved -- a line that repeats is noise and a line
           that never changes reads as a hang. */
        {
            uint32_t now = get_ticks();
            if(now - last_print >= TICKS_PER_SEC / 5u){
                int pct = (int)rsa_keygen_progress(&g_key);
                if(pct != pct_last || g_key.tried != tried_at_print){
                    up("\r  ");
                    putu((uint32_t)pct, 3);
                    up("%   ");
                    putu(g_key.tried, 5);
                    up(" candidates   ");
                    put_elapsed(now - t0);
                    up("     ");
                    pct_last = pct;
                    tried_at_print = g_key.tried;
                }
                last_print = now;
            }
        }
    }

    g_key_ticks = get_ticks() - t0;
    g_key_tried = g_key.tried;
    up("\n\n");

    if(r != RSA_KG_DONE){
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        up("Failed: "); up(rsa_why_name(g_key.why)); up("\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        if(g_key.why == RSA_EGIVEUP){
            up("  The candidate budget ran out, which means the entropy\n");
            up("  source is not varying. That is a bug in entropy_u32(),\n");
            up("  not in the generator.\n");
        }
        return;
    }

    g_have_key = 1;
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    up("Key generated.\n\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    show_key();
    up("\n");
    verify_key();
    up("\n");
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    up("  That time is the real number for this machine. The 15-25\n");
    up("  minute figure in HANDOFF.md measured the OLD byte-wise\n");
    up("  exponentiation and has never been re-measured since\n");
    up("  net/bignum.c replaced it.\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

/* ── `rsa prime <n>`: the primality test, on one number ──────────────
   Small, and the only way to poke Miller-Rabin by hand. Worth having
   because v0.8.8.2 found that this function ignored its round count
   entirely -- it ran one witness whatever it was asked for -- and
   nothing outside a suite could have shown that. */
static void do_prime(const char *arg)
{
    bn_t n;
    uint32_t v = parse_bits(arg);          /* same digits-only parse */
    int r;

    if(!arg || !arg[0] || v == 0){
        up("rsa prime <number>   -- is it prime?\n");
        return;
    }
    bn_set_u32(&n, v);
    up("  "); putu(v, 0); up("  ");

    r = rsa_trial_divide(&n);
    if(!r){
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        up("composite -- a factor at or below 997\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }
    if(v < 994009u){
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        up("PRIME");
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        up("  -- proved by trial division, not estimated:\n");
        up("            below 997*997 a composite must have a factor\n");
        up("            at or under 997 and the table holds them all\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }
    r = rsa_is_probable_prime(&n, 64);
    if(r == 1){
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        up("probably prime");
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        up("  -- 64 Miller-Rabin witnesses\n");
    } else {
        vga_set_color(VGA_YELLOW, VGA_BLACK);
        up("composite");
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        up("  -- a witness proved it\n");
    }
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

void cmd_rsa(const char *arg1, const char *arg2)
{
    if(!arg1 || !arg1[0]){
        up("rsa -- RSA key generation, the first half of pairing stage 3\n\n");
        up("  rsa keygen [bits]   generate a key (default 2048)\n");
        up("  rsa show            the last key generated\n");
        up("  rsa verify          check it again\n");
        up("  rsa prime <n>       is n prime?\n\n");
        vga_set_color(VGA_DARK_GREY, VGA_BLACK);
        up("  The arithmetic is net/bignum.c and net/rsa.c, checked\n");
        up("  against CPython and `openssl genrsa` by 659 suite checks.\n");
        up("  What has never been measured is how long a key takes on\n");
        up("  THIS machine -- 'rsa keygen' is how that gets answered.\n");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        return;
    }
    if(!strcmp(arg1, "keygen")){ do_keygen(arg2); return; }
    if(!strcmp(arg1, "prime")){  do_prime(arg2);  return; }
    if(!strcmp(arg1, "show") || !strcmp(arg1, "verify")){
        if(!g_have_key){
            vga_set_color(VGA_YELLOW, VGA_BLACK);
            up("No key yet. 'rsa keygen' makes one.\n");
            vga_set_color(VGA_WHITE, VGA_BLACK);
            return;
        }
        if(!strcmp(arg1, "show")) show_key();
        else                      verify_key();
        return;
    }

    /* v0.8.6.6's lesson: a command that quietly does something else
       when it does not understand you is worse than one that refuses. */
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    up("rsa: no such option: "); up(arg1); up("\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    up("  rsa keygen [bits] | rsa show | rsa verify | rsa prime <n>\n");
}
