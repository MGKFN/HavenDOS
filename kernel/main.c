#include "../include/netdev.h"
#include "../include/io.h"
#include "version.h"
#include "../include/vga.h"
#include "../include/machine.h"
#include "../include/klog.h"
#include "../include/highfb.h"
#include "../include/xedisp.h"   /* v0.9.0: kernel_fb_phys() */
#include "../include/i2c.h"
#include "../include/net.h"      /* v0.7.6: net_ready() was implicit */
#include "../include/string.h"
#include "../include/types.h"
#include "../include/gfx.h"
#include "../include/virtio.h"

/* v0.7.6: forward decls — these were implicit, so GCC assumed int(...) */
extern int  cfg_setup_done(void);
extern void mode13_set_flip(int fx, int fy);
extern int  mode13_try_enter(void);

void idt_init(void);
void irq_enable_basic(void);
void irq_enable_mouse(void);
void pit_init(uint32_t hz);
#include "../include/kpoll.h"
#include "../include/pit.h"
void pmm_init(uint32_t mem_size);
void heap_init(void);
void keyboard_init(void);
void mouse_init(void);
void ramfs_init(void);
void shell_init(void);
void desktop_run(void);
void login_screen(void);
void boot_animation(int had_loading_screen);
void shell_run(void);
void mode13_desktop_run(void);
void vesa_desktop_run(void);
void vesa_set_framebuffer(uint32_t,uint32_t,uint32_t,uint32_t);
int  vga_fb_init(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
extern uint32_t get_ticks(void);

gfx_mode_t current_gfx = GFX_TEXT;

#define MB1_LOADER_MAGIC  0x2BADB002u
#define MB2_LOADER_MAGIC  0x36d76289u

/* ── Per-user home directory setup ───────────────────────────────
   Called from login.c after each successful login.
   Creates HOME/<username>/ and populates it with starter files
   the first time that user logs in.  Subsequent logins are no-ops. */
void setup_user_home(const char *username)
{
    extern int  ramfs_write(const char*,const char*,uint32_t);
    extern int  ramfs_mkdir(const char*);
    extern int  vfs_exists(const char*);

    /* Build HOME/<username> path */
    char homedir[48];
    strncpy(homedir, "HOME/", 5);
    strncat(homedir, username, sizeof(homedir)-6);
    homedir[sizeof(homedir)-1] = 0;

    /* Already exists — user has logged in before, leave files alone */
    if(vfs_exists(homedir)) return;

    ramfs_mkdir(homedir);

    /* NOTES.TXT — editable notes file */
    char notes_path[64];
    strncpy(notes_path, homedir, sizeof(notes_path)-16);
    strncat(notes_path, "/NOTES.TXT", sizeof(notes_path)-strlen(notes_path)-1);

    char notes[256];
    strncpy(notes, "HavenDOS Notes - ", sizeof(notes)-1);
    strncat(notes, username, sizeof(notes)-strlen(notes)-1);
    strncat(notes, "\r\n--------------\r\nEdit this file with: edit ~/NOTES.TXT\r\n", sizeof(notes)-strlen(notes)-1);
    ramfs_write(notes_path, notes, (uint32_t)strlen(notes));

    /* HELLO.BOOT — starter BOOT language script */
    char boot_path[64];
    strncpy(boot_path, homedir, sizeof(boot_path)-16);
    strncat(boot_path, "/HELLO.BOOT", sizeof(boot_path)-strlen(boot_path)-1);

    char boot_src[256];
    strncpy(boot_src, "rem Hello from BOOT Language\r\nvar name = \"", sizeof(boot_src)-1);
    strncat(boot_src, username, sizeof(boot_src)-strlen(boot_src)-1);
    strncat(boot_src, "\"\r\nprintln \"Hello, \" + name + \"!\"\r\nwait 1000\r\n", sizeof(boot_src)-strlen(boot_src)-1);
    ramfs_write(boot_path, boot_src, (uint32_t)strlen(boot_src));
}

/* The Multiboot 1 information structure, at the spec's offsets.
 *
 * This was wrong, and wrong in a way nothing could notice until now: the
 * four VBE interface fields at offsets 80-87 are u16 in the specification
 * and were declared u32 here, so everything from framebuffer_addr onward was
 * read EIGHT BYTES too far into the structure.  framebuffer_bpp and
 * framebuffer_type are single bytes as well, not words.
 *
 * Nothing depended on those fields -- the header never asked GRUB for a
 * framebuffer, so the whole block was always zero -- which is why the VESA
 * driver has been dead code rather than broken code.  Asking for a
 * framebuffer makes the offsets matter for the first time. */
typedef struct {
    uint32_t flags, mem_lower, mem_upper, boot_device, cmdline,
             mods_count, mods_addr, syms[4], mmap_length, mmap_addr,
             drives_length, drives_addr, config_table, boot_loader,
             apm_table, vbe_control, vbe_mode_info;
    uint16_t vbe_mode, vbe_seg, vbe_off, vbe_len;
    uint32_t fb_addr_lo, fb_addr_hi, fb_pitch, fb_width, fb_height;
    uint8_t  fb_bpp, fb_type;
    uint8_t  color_info[6];
} __attribute__((packed)) mb1_info_t;

typedef struct{uint32_t type,size;}__attribute__((packed))mb2_tag_t;
typedef struct{uint32_t type,size;char str[1];}__attribute__((packed))mb2_cmd_t;

typedef enum{MODE_DESKTOP=0,MODE_SHELL,MODE_SAFE,MODE_LOWRES}boot_mode_t;
static boot_mode_t boot_mode=MODE_DESKTOP;
static uint32_t    ram_bytes=32u*1024u*1024u;
static int         mouse_enabled=1; /* default ON; mode=safe disables it */
static int         force_install=0;  /* set by install=1 on cmdline */
static int         g_usb_hid_off=0;  /* set by nousbkbd on cmdline      */
int usb_hid_wanted(void){ return !g_usb_hid_off; }
static int         gfx_flip_x=0, gfx_flip_y=0;

static void busy_wait(volatile uint32_t n){while(n--)__asm__ volatile("nop");}

/* v0.7.6: this was written as tail recursion — one stack frame per token on
   the kernel command line, with no depth limit, running on the boot stack
   before pmm_init().  While strncmp() was broken it never mattered because no
   token except the last was ever honoured; once v0.7.6 fixed strncmp the
   recursion became live.  Rewritten as a loop, and each token is now compared
   as a WHOLE token rather than as a prefix, so "mode=shellish" no longer
   silently selects shell mode. */
static int tok_eq(const char *tok, const char *want){
    while(*want){ if(*tok!=*want) return 0; tok++; want++; }
    return (*tok==0 || *tok==' ');
}

static int g_want_log = 0;   /* `verbose`: the log, not the splash */
static int g_fb_diag = 0;   /* `fbdiag` on the kernel command line */
static uint32_t g_fb_lo, g_fb_w, g_fb_h, g_fb_pitch;   /* forward, see below */
static uint32_t g_fb_bpp_early;
/* Writes a colour at whatever depth the firmware chose.  The console itself
   only handles 32bpp - that is the point of the magenta case. */
static void fill_any(uint32_t colour){
    for(uint32_t y=0;y<g_fb_h;y++){
        volatile uint8_t *row = (volatile uint8_t*)(g_fb_lo + y*g_fb_pitch);
        for(uint32_t x=0;x<g_fb_w;x++){
            if(g_fb_bpp_early==32){
                ((volatile uint32_t*)row)[x] = colour;
            } else if(g_fb_bpp_early==24){
                row[x*3+0]=(uint8_t)(colour);
                row[x*3+1]=(uint8_t)(colour>>8);
                row[x*3+2]=(uint8_t)(colour>>16);
            } else if(g_fb_bpp_early==16){
                uint16_t c = (uint16_t)(((colour>>19)&0x1F)<<11 |
                                        ((colour>>10)&0x3F)<<5  |
                                        ((colour>>3)&0x1F));
                ((volatile uint16_t*)row)[x] = c;
            }
        }
    }
}

/* ---------------------------------------------------------------------
   Signals for a machine with no usable screen.
   ---------------------------------------------------------------------
   The first version of fbdiag answered by resetting the machine, and that
   cannot work: fbdiag runs BEFORE idt_init(), so any fault between kernel
   entry and the end of this block has no handler, double-faults, and
   TRIPLE-FAULTS -- which resets the CPU.  The diagnostic's own exit is also
   a reset.  "Black screen, then reboot" is therefore what you see whether
   the kernel ran the diagnostic and found nothing, or died on its first
   instruction.  Kyan's HP showed exactly that, and the old table read it as
   the former when it is equally the latter.

   So the signals below do not use reset to mean anything.  They use two
   channels that need no video, no IDT, no interrupts and no PIT init, and
   they fire BEFORE the framebuffer is touched -- because writing pixels to
   an address the firmware did not really give us is itself a candidate for
   the hang.

   Neither channel is guaranteed on a 2023 laptop: the PC speaker may be
   absent and the 8042 may be emulated or missing.  Hearing or seeing
   nothing proves nothing.  Hearing or seeing SOMETHING proves the kernel
   ran, which is the question that was unanswerable before.
   --------------------------------------------------------------------- */

static void diag_spin(uint32_t n){ for(volatile uint32_t d=0;d<n;d++){} }

/* PIT channel 2 straight to the speaker gate.  pit_init() has not run and
   does not need to -- channel 2 is programmed here directly. */
static void diag_tone_hz(uint32_t hz, uint32_t ms_spin){
    uint32_t div = 1193182u / hz;
    outb(0x43,0xB6);
    outb(0x42,(uint8_t)(div&0xFF));
    outb(0x42,(uint8_t)(div>>8));
    outb(0x61,(uint8_t)(inb(0x61)|0x03));       /* gate + data on */
    diag_spin(ms_spin);
    outb(0x61,(uint8_t)(inb(0x61)&~0x03));
}
/* 262 Hz, not 880.  The first version used 880 Hz for a fifth of a second,
   seven times over, and Kyan's verdict was "obnoxious loud" -- which is fair,
   and a diagnostic nobody will run twice is no diagnostic.  A PC speaker has
   no volume control, so the levers are pitch and duration: low C is far less
   piercing than an A above the treble staff, and the whole sequence is now
   about two seconds, once, instead of forty seconds of repeats. */
static void diag_tone(uint32_t ms_spin){ diag_tone_hz(262u, ms_spin); }

/* Keyboard LEDs via the 8042.  Every wait is bounded: a UEFI laptop may
   have no 8042 at all, and a diagnostic that hangs where it used to reset
   would be worse than the bug it is chasing. */
static void diag_8042_wait_in(void){
    for(uint32_t i=0;i<100000u;i++) if(!(inb(0x64)&0x02)) return;
}
static void diag_leds(uint8_t mask){        /* bit0 scroll, bit1 num, bit2 caps */
    diag_8042_wait_in(); outb(0x60,0xED);
    diag_8042_wait_in(); outb(0x60,mask);
    diag_8042_wait_in();
}

/* How the answer is signalled, and why it is so quiet now.
   --------------------------------------------------------
   The screen already carries most of it: answers 3, 4 and 5 all PAINT the
   framebuffer (magenta, blue-then-green, blue-then-red).  Only 2 and 6 leave
   it black, because in those two cases there is nothing to paint on.  So
   sound is not the primary channel at all -- it only has to separate "no
   framebuffer supplied" from "framebuffer above 4 GB", and a handful of soft
   blips does that.

   Three mistakes are corrected here.  The first version beeped 880 Hz for a
   fifth of a second, seven times; the second repeated that twelve times
   over.  Both are unusable on a machine someone actually owns.  And the
   answer was also left in the keyboard LEDs -- which assumed a caps/num/
   scroll lock LED that a 2023 laptop simply may not have: Kyan's HP has
   lights on the power button, the mic mute and the volume mute, none of
   which is reachable from a bare-metal kernel (they are wired to the
   embedded controller, not the 8042).

   So: one long low tone, a clear gap, then N short low blips.  Once.  About
   two seconds in total.  `fbsilent` on the kernel command line drops even
   that and leaves only the screen.  The LED flashes are still emitted
   because they cost nothing and work on a desktop keyboard, but nothing
   depends on them. */

#define DIAG_SHORT   90000000u     /* ~0.11 s on a modern machine */
#define DIAG_LONG    320000000u    /* ~0.40 s -- clearly a marker  */

static int g_fb_silent = 0;        /* `fbsilent`: screen only, no sound */
static int g_fb_map    = 0;        /* `fbmap`: force it even under fbdiag */
static int g_fb_nomap  = 0;        /* `nohighfb`: never page one in       */
static int g_fb_mapped = 0;        /* 0 none, 1 PSE-36, 2 PAE             */

/* ------------------------------------------------------------------
   `live` -- touch no disk, ever.
   ------------------------------------------------------------------
   Booting removable media on a machine with another operating system on
   it must not be able to damage that system, and "it probably cannot see
   an NVMe disk anyway" is not a guarantee -- it is an accident of which
   drivers exist this week.  The realistic way HavenDOS could hurt a
   Windows install is not the installer, which asks first: it is the VFS
   quietly mounting the first FAT partition it finds and writing
   .accounts, DESKTOP.CFG, the theme and the shell history into it.  An
   EFI System Partition is FAT, sits first on the disk, and is exactly
   what a careless probe would land on.

   In live mode vfs_init() never mounts anything, so every write goes to
   the RAM filesystem and is gone at power-off, and the installer refuses
   outright rather than relying on the user not to pick it.

   The ISO's own menu entries pass `live`.  The grub.cfg the INSTALLER
   writes does not, so an installed system still owns its disk.
   ------------------------------------------------------------------ */
static int g_live = 0;
int boot_is_live(void){ return g_live; }

static void diag_beep_short(void){
    diag_leds(0x07);
    if(!g_fb_silent) diag_tone(DIAG_SHORT); else diag_spin(DIAG_SHORT);
    diag_leds(0x00);
    diag_spin(DIAG_SHORT);
}
static void diag_beep_long(void){
    diag_leds(0x07);
    if(!g_fb_silent) diag_tone(DIAG_LONG); else diag_spin(DIAG_LONG);
    diag_leds(0x00);
    diag_spin(DIAG_SHORT*2u);
}

/* Still set at the end, for a keyboard that does have the lights:
   1 = 001  2 = 010  3 = 011  4 = 100  5 = 101  6 = 110                */
static void diag_leds_binary(uint32_t answer){
    uint8_t m = 0;
    if(answer & 1u) m |= 0x01;
    if(answer & 2u) m |= 0x02;
    if(answer & 4u) m |= 0x04;
    diag_leds(m);
}

static void apply_cmdline(const char *s){
    if(!s||!s[0]) return;
    const char *p = s;
    int guard = 0;
    while(*p && ++guard < 64){
        while(*p==' ') p++;
        if(!*p) break;

        if     (tok_eq(p,"mode=shell"))   boot_mode=MODE_SHELL;
        else if(tok_eq(p,"mode=safe"))    boot_mode=MODE_SAFE;
        else if(tok_eq(p,"mode=lowres"))  boot_mode=MODE_LOWRES;
        else if(tok_eq(p,"mode=desktop")) boot_mode=MODE_DESKTOP;
        else if(tok_eq(p,"mouse=on"))     mouse_enabled=1;
        else if(tok_eq(p,"mouse=off"))    mouse_enabled=0;
        else if(tok_eq(p,"install=1"))    force_install=1;
        /* Linux spells it this way and so does this: the log is on unless
           asked otherwise, because a kernel that dies during init has to
           have said how far it got BEFORE it died. */
        else if(tok_eq(p,"quiet"))        klog_set_echo(0);
        /* v0.8.8.4: the loading screen is up by default now, so this
           is how a person asks for the scrolling log instead. Named
           for what it does rather than "nosplash", because the thing
           being chosen is the log, not the absence of a picture. */
        else if(tok_eq(p,"verbose"))      g_want_log=1;
        else if(tok_eq(p,"fbdiag"))       g_fb_diag=1;
        else if(tok_eq(p,"fbsilent"))     g_fb_silent=1;
        else if(tok_eq(p,"fbmap"))        g_fb_map=1;
        else if(tok_eq(p,"nohighfb"))     g_fb_nomap=1;
        /* v0.8.9.6: the way back from write-combining, if a machine
           turns up whose framebuffer does not like it. */
        else if(tok_eq(p,"nowc"))         highfb_nowc=1;
        else if(tok_eq(p,"live"))         g_live=1;
        /* v0.8.5.2 -- the escape hatch for a firmware that will not hand
           the xHCI over. If SMM keeps its USB legacy emulation running,
           every keystroke arrives twice: once at port 0x60 and once
           through our own HID driver. This turns OUR half off, leaving
           the firmware's emulation to do what it was doing before any of
           the USB work existed. A machine whose only keyboard is USB
           stays usable either way, which is the point. */
        else if(tok_eq(p,"nousbkbd"))     g_usb_hid_off=1;
        /* v0.8.5.3 -- print every byte read out of the 8042 and say
           whether the interrupt or the poll read it. For working out
           whether a doubled keystroke is one scancode read twice or two
           scancodes sent once. */
        else if(tok_eq(p,"kbddiag")){ extern int kbd_diag; kbd_diag=1; }
        else if(tok_eq(p,"gfx=mode13"))   current_gfx=GFX_MODE13;
        else if(tok_eq(p,"gfx=vesa640"))  current_gfx=GFX_VESA640;
        else if(tok_eq(p,"gfx=vesa800"))  current_gfx=GFX_VESA800;
        else if(tok_eq(p,"gfx=text"))     current_gfx=GFX_TEXT;
        else if(tok_eq(p,"flip=xy")){ gfx_flip_x=1; gfx_flip_y=1; }
        else if(tok_eq(p,"flip=x"))       gfx_flip_x=1;
        else if(tok_eq(p,"flip=y"))       gfx_flip_y=1;

        while(*p && *p!=' ') p++;          /* skip to end of this token */
    }
}

/* What the loader said about the display, kept so the boot log can report it
   once the console exists.  On a machine where the framebuffer turns out to be
   unusable this is the only evidence of why the screen is blank. */
static int      g_fb_unreachable = 0;
static uint32_t g_fb_hi_orig = 0, g_fb_lo_orig = 0;  /* before remapping */
static uint32_t g_fb_hi = 0;
static uint32_t g_fb_bpp = 0;
static int      g_fb_kind = -1;   /* -1 = a linear framebuffer was supplied */

/* v0.9.0: the framebuffer's PHYSICAL address and geometry, for `gfx`,
   which compares it with what the display engine's plane registers say
   is being scanned out. g_fb_lo is rewritten to the virtual window when
   the framebuffer is above 4 GB, so the originals are what is handed
   back; below 4 GB nothing was remapped and g_fb_lo is still physical. */
void kernel_fb_phys(uint32_t *hi, uint32_t *lo, uint32_t *w, uint32_t *h,
                    uint32_t *pitch)
{
    *hi    = g_fb_hi_orig;
    *lo    = g_fb_hi_orig ? g_fb_lo_orig : g_fb_lo;
    *w     = g_fb_w;
    *h     = g_fb_h;
    *pitch = g_fb_pitch;
}

static void parse_mb1(uint32_t addr){
    if(addr<0x500||addr>0x8000000)return;
    mb1_info_t *mb=(mb1_info_t*)addr;
    if(mb->flags&1){
        /* mem_upper is KB above 1MB — allow up to 4GB (0xFFFFF KB) */
        uint32_t kb=mb->mem_upper+1024;
        if(kb>1024) ram_bytes=(uint64_t)kb*1024 > 0xFFFFFFFFULL
                              ? 0xFFFFFFFF : (uint32_t)((uint64_t)kb*1024);
    }
    /* Use memory map if available — more accurate */
    if((mb->flags&(1<<6))&&mb->mmap_addr&&mb->mmap_length){
        uint32_t highest=0;
        uint8_t *p=(uint8_t*)mb->mmap_addr;
        uint8_t *end=p+mb->mmap_length;
        while(p<end){
            /* v0.7.18 FIX — a Multiboot1 mmap entry is
                   size@0 (u32), base_addr@4 (u64), length@12 (u64), type@20 (u32)
               and p points at `size` because the walk below advances by
               size+4.  `type` was being read from p+16, which is the UPPER
               dword of `length` — zero for every region below 4GB.  So
               `type==1` never matched, `highest` never moved, and this entire
               "more accurate" mmap path was dead code: RAM size always fell
               back to mem_upper, which some BIOSes cap well below the truth. */
            uint32_t entry_sz=*(uint32_t*)p;
            uint32_t type=*(uint32_t*)(p+20);
            uint64_t base=*(uint64_t*)(p+4);
            uint64_t len =*(uint64_t*)(p+12);
            if(entry_sz<20) break;            /* malformed — stop, don't spin */
            if(type==1&&base<0x100000000ULL){ /* type 1 = usable RAM */
                uint64_t top=base+len;
                if(top>0x100000000ULL) top=0x100000000ULL;
                if((uint32_t)top>highest) highest=(uint32_t)top;
            }
            p+=entry_sz+4;
        }
        if(highest>1024*1024) ram_bytes=highest;
    }
    if((mb->flags&4)&&mb->cmdline&&mb->cmdline<0x8000000)
        apply_cmdline((const char*)mb->cmdline);
    /* Framebuffer (bit 12).  fb_type 1 is a linear RGB framebuffer, which
       is what the console wants; type 2 is EGA text, which means GRUB could
       not give us graphics and the 0xB8000 path still applies. */
    if((mb->flags&(1<<12))&&mb->fb_type==1){
        /* The address is 64 bits wide and this kernel is 32-bit with no
           paging, so anything above 4 GB is unreachable.  Firmware on recent
           machines does put the GOP framebuffer up there when Above-4G
           decoding is on, and taking the low half would send every pixel to
           an unrelated physical address: the kernel runs perfectly and the
           screen stays black, which is indistinguishable from a kernel that
           never started.  Refuse it and record why. */
        if(mb->fb_addr_hi){
            g_fb_unreachable = 1;
            g_fb_hi = mb->fb_addr_hi;
            g_fb_lo = mb->fb_addr_lo;
            g_fb_hi_orig = mb->fb_addr_hi;
            g_fb_lo_orig = mb->fb_addr_lo;
            /* The geometry is kept even though the address is out of
               reach: highfb_map() needs the size to know how much to
               map, and the boot log is far more useful saying
               "1920x1080 32 bpp at 4_00000000" than just an address. */
            g_fb_w     = mb->fb_width;
            g_fb_h     = mb->fb_height;
            g_fb_bpp   = mb->fb_bpp;
            g_fb_bpp_early = mb->fb_bpp;
            g_fb_pitch = mb->fb_pitch;
        } else if(mb->fb_addr_lo && mb->fb_width){
            g_fb_lo    = mb->fb_addr_lo;
            g_fb_w     = mb->fb_width;
            g_fb_h     = mb->fb_height;
            g_fb_bpp   = mb->fb_bpp;
            g_fb_bpp_early = mb->fb_bpp;
            g_fb_pitch = mb->fb_pitch;
            vga_fb_init(mb->fb_addr_lo,mb->fb_width,mb->fb_height,
                        mb->fb_pitch,mb->fb_bpp);
            vesa_set_framebuffer(mb->fb_addr_lo,mb->fb_width,mb->fb_height,mb->fb_pitch);
        }
    } else if(mb->flags&(1<<12)){
        g_fb_kind = mb->fb_type;      /* 0 = indexed, 2 = EGA text */
    } else {
        g_fb_kind = 0xFF;             /* the loader supplied nothing at all */
    }
}

static void parse_mb2(uint32_t addr){
    if(!addr||(addr&7)||addr>0x8000000)return;
    uint32_t total=*(uint32_t*)addr; if(total<8||total>65536)return;
    mb2_tag_t *tag=(mb2_tag_t*)(addr+8);
    while((uint32_t)tag<addr+total&&tag->type!=0){
        if(tag->type==1&&tag->size>8) apply_cmdline(((mb2_cmd_t*)tag)->str);
        /* Tag 8 is the framebuffer.  Multiboot 2 was being parsed for the
           command line and the memory map but not for this, so a kernel
           loaded by an MB2 loader had no console at all once text mode went
           away. */
        if(tag->type==8&&tag->size>=32){
            /* addr at +8 (u64), pitch +16, width +20, height +24,
               bpp +28 (u8), type +29 (u8). */
            uint32_t *fb  = (uint32_t*)((uint8_t*)tag+8);
            uint8_t   bpp = *((uint8_t*)tag+28);
            uint8_t   typ = *((uint8_t*)tag+29);
            /* fb[1] is the high half of the 64-bit address - see parse_mb1 */
            if(typ==1&&fb[1]){
                g_fb_unreachable=1; g_fb_hi=fb[1]; g_fb_lo=fb[0];
                g_fb_hi_orig=fb[1]; g_fb_lo_orig=fb[0];
                g_fb_w=fb[3]; g_fb_h=fb[4]; g_fb_bpp=bpp;
                g_fb_bpp_early=bpp; g_fb_pitch=fb[2];
            } else if(typ==1&&fb[0]&&fb[3]&&fb[4]){
                g_fb_lo=fb[0]; g_fb_w=fb[3]; g_fb_h=fb[4]; g_fb_bpp=bpp;
                g_fb_bpp_early=bpp; g_fb_pitch=fb[2];
                vga_fb_init(fb[0],fb[3],fb[4],fb[2],(uint32_t)bpp);
            }
        }
        /* v0.7.18 FIX — tag 6 IS the memory map, but it was never parsed:
           RAM was simply pinned to 64MB on any Multiboot2 boot regardless of
           how much the machine actually had.  Layout is a 16-byte tag header
           (type, size, entry_size, entry_version) followed by entries of
           entry_size bytes: addr@0 (u64), len@8 (u64), type@16 (u32). */
        if(tag->type==6&&tag->size>16){
            uint8_t *base=(uint8_t*)tag;
            uint32_t esz=*(uint32_t*)(base+8);
            uint32_t highest=0;
            if(esz>=24&&esz<=256){
                for(uint32_t off=16;off+esz<=tag->size;off+=esz){
                    uint8_t  *e=base+off;
                    uint64_t  a=*(uint64_t*)e;
                    uint64_t  l=*(uint64_t*)(e+8);
                    uint32_t  t=*(uint32_t*)(e+16);
                    if(t!=1||a>=0x100000000ULL) continue;   /* 1 = usable RAM */
                    uint64_t top=a+l;
                    if(top>0x100000000ULL) top=0x100000000ULL;
                    if((uint32_t)top>highest) highest=(uint32_t)top;
                }
            }
            if(highest>1024*1024) ram_bytes=highest;
        }
        uint32_t n=(tag->size+7)&~7u; if(!n)break;
        tag=(mb2_tag_t*)((uint8_t*)tag+n);
    }
}

static void create_sysfiles(void){
    extern int ramfs_write(const char*,const char*,uint32_t);
    extern int ramfs_mkdir(const char*);
    extern int ramfs_chmod(const char*,int);

    /* ── Root files ─────────────────────────────────────── */
    const char *readme =
        "HavenDOS " HAVENDOS_VER "\r\n"
        "TechHaven Studios\r\n"
        "Type 'help' in the shell for a command list.\r\n"
        "Type 'guide' for the built-in user guide.\r\n";
    ramfs_write("README.TXT", readme, strlen(readme));

    /* v0.7.17: a starter hosts file, so `chat kyan@bootchat` works out of the
       box instead of forcing the user to memorise 10.0.2.2.  Written to the
       RAM filesystem on every boot; a disk install keeps its own edited copy
       because vfs_write() prefers FAT16 when a volume is mounted. */
    {
        extern int vfs_exists(const char*);
        extern int vfs_write(const char*, const char*, uint32_t);
        if(!vfs_exists("hosts.txt")){
            const char *hosts =
                "# HavenDOS hosts file - name then address, one per line.\r\n"
                "# Edit with:  edit hosts.txt      Reload with:  nslookup -flush\r\n"
                "#\r\n"
                "# 10.0.2.2 is ALWAYS the machine running QEMU, from inside the guest.\r\n"
                "bootchat        10.0.2.2\r\n"
                "host            10.0.2.2\r\n"
                "#\r\n"
                "# Add your own, e.g. a laptop on the LAN:\r\n"
                "# laptop        192.168.2.20\r\n";
            vfs_write("hosts.txt", hosts, strlen(hosts));
        }
    }

    const char *changelog =
        HAVENDOS_VER " - http_resp_t ABI fix (wget/curl/bpkg), TLS NewSessionTicket, selftest, netstat -s\r\n"
        HAVENDOS_VER " - Net stack overhaul: HTTPS actually works, strncmp fix, TCP window/MSS fixes\r\n"
        HAVENDOS_VER " - Anti-flicker VGA shadow buffer, SATA/LBA48 ATA, faster PS/2 mouse (200sps), PC optimisations\r\n"
        HAVENDOS_VER " - Final stabilization: TCP checksum fix, heap magic, DNS/FAT loop guards\r\n"
        HAVENDOS_VER " - Security: IPv4 checksum verify, UDP length fix, TCP ACK/window, heap double-free\r\n"
        HAVENDOS_VER " - Major: ISO install-only boot, arrow-key installer, shell history, autologin timeout\r\n"
        HAVENDOS_VER " - BOOT language IDE + boot shell cmd + 10 extra themes via bpkg\r\n"
        HAVENDOS_VER " - Bug fixes: verbose boot, reminder alerts, notif count, theme preview\r\n"
        HAVENDOS_VER " - bpkg package manager + per-theme wallpapers + notification center\r\n"
        HAVENDOS_VER " - Desktop polish: live clock, reminders, calculator, real POST screen\r\n"
        HAVENDOS_VER " - Win11-style login (theme bg, user tiles, live clock)\r\n"
        HAVENDOS_VER " - ISO-based update command + rollback\r\n"
        HAVENDOS_VER " - FAT16 disk subsystem (installable HavenDOS)\r\n"
        HAVENDOS_VER " - VirtIO block driver (UTM SE disk I/O)\r\n"
        HAVENDOS_VER " - File Manager mouse + start menu hover\r\n"
        HAVENDOS_VER " - Desktop: per-theme bg, pipes screensaver, dashboard, 2GB RAM\r\n"
        HAVENDOS_VER " - Shell: PS1 prompt, fortune, banner, tab-complete overhaul\r\n"
        HAVENDOS_VER "   - Polish: RTC date on taskbar, sysinfo, shutdown\r\n"
        HAVENDOS_VER "   - Networking: RTL8139, ARP, ICMP ping, UDP send\r\n"
        HAVENDOS_VER "   - Shell: touch, wc, head, tail, alias, tab-complete\r\n";
    ramfs_write("CHANGELOG.TXT", changelog, strlen(changelog));

    const char *welcome =
        "Welcome to HavenDOS!\r\n"
        "Press ESC for the Start Menu.\r\n"
        "Open a terminal from the Start Menu to use the shell.\r\n";
    ramfs_write("WELCOME.TXT", welcome, strlen(welcome));

    /* ── ETC — system configuration ─────────────────────── */
    ramfs_mkdir("ETC");

    const char *motd =
        "HavenDOS " HAVENDOS_VER " - TechHaven Studios\r\n"
        "Type 'help' for commands. Type 'guide' for the user guide.\r\n"
        HAVENDOS_VER ": http_resp_t ABI unified, TLS NewSessionTicket, chunked decoder, line editing, selftest\r\n";
    ramfs_write("ETC/MOTD.TXT", motd, strlen(motd));

    const char *hosts =
        "127.0.0.1  localhost\r\n"
        "10.0.2.2   gateway\r\n"
        "10.0.2.15  bootos\r\n";
    ramfs_write("ETC/HOSTS.TXT", hosts, strlen(hosts));

    const char *sysconf =
        "OS=HavenDOS\r\n"
        "VERSION=0.6.1\r\n"
        "ARCH=x86-32\r\n"
        "BOOT=GRUB2-Multiboot1\r\n"
        "FS=RAMFS\r\n"
        "DISPLAY=VGA-80x25\r\n";
    ramfs_write("ETC/SYSCONF.TXT", sysconf, strlen(sysconf));

    /* SECURITY FIX: do not store plaintext credentials in any file.
       Passwords are stored as djb2 hashes in login.c (login_init).
       This file exists only as a user reference; no actual secrets here. */
    const char *passwd =
        "# HavenDOS user database\r\n"
        "# Passwords are stored as hashes in the login subsystem.\r\n"
        "# Use 'passwd' in the shell to change your password.\r\n"
        "admin::1\r\n"
        "bootos::0\r\n"
        "guest::0\r\n";
    ramfs_write("ETC/PASSWD.TXT", passwd, strlen(passwd));
    ramfs_chmod("ETC/PASSWD.TXT", 1);  /* admin only */

    /* ── BIN — built-in command reference ───────────────── */
    ramfs_mkdir("BIN");

    const char *cmdlist =
        "Shell built-in commands (" HAVENDOS_VER "):\r\n"
        "  Files:   ls tree cat write touch mkdir cd pwd del cp mv find grep wc\r\n"
        "  Shell:   echo set env alias history run which calc repeat seq\r\n"
        "  System:  sysinfo mem uptime disk dmesg ps ver date time\r\n"
        "  Users:   whoami users passwd adduser deluser chmod chown log\r\n"
        "  Network: ifconfig ping netstat udpsend\r\n"
        "  Fun:     matrix stars pipes snake guide man help theme fortune\r\n"
        "Type 'man <cmd>' for details. Type 'guide' for the full guide.\r\n";
    ramfs_write("BIN/COMMANDS.TXT", cmdlist, strlen(cmdlist));

    /* ── HOME — per-user home directories ───────────────── */
    /* Root HOME dir. Each user gets HOME/<username>/ created at first login.
       setup_user_home() is called from login.c after a successful login. */
    ramfs_mkdir("HOME");

    /* ── VAR — runtime logs ──────────────────────────────── */
    ramfs_mkdir("VAR");
    { static const char sl[] = "HavenDOS " HAVENDOS_VER " boot OK\r\n";
      ramfs_write("VAR/SYSLOG.TXT", sl, strlen(sl)); }
    ramfs_chmod("VAR/SYSLOG.TXT", 1);  /* admin only */

    /* Hidden audit log */
    ramfs_write(".loginlog","",0);
    ramfs_chmod(".loginlog",1);
}

static __attribute__((unused)) void boot_screen(const char *lbl, const char *gfxlbl, uint32_t magic){
    vga_clear();
    vga_puts_at("  ____   ___   ___ _____    ___  ____  ", 20,4,VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts_at(" | __ ) / _ \\ / _ \\_   _|  / _ \\/ ___| ",20,5,VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts_at(" |  _ \\| | | | | | || |   | | | \\___ \\ ",20,6,VGA_CYAN,VGA_BLACK);
    vga_puts_at(" | |_) | |_| | |_| || |   | |_| |___) |",20,7,VGA_CYAN,VGA_BLACK);
    vga_puts_at(" |____/ \\___/ \\___/ |_|    \\___/|____/ ", 20,8,VGA_BLUE,VGA_BLACK);
    vga_puts_at(HAVENDOS_VER,               34,10,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("Mode: ",   28,12,VGA_LIGHT_GREY, VGA_BLACK);
    vga_puts_at(lbl,        34,12,VGA_LIGHT_GREEN,VGA_BLACK);
    vga_puts_at("Graphics: ",28,13,VGA_LIGHT_GREY, VGA_BLACK);
    vga_puts_at(gfxlbl,     38,13,VGA_YELLOW,     VGA_BLACK);
    char mb[12]; utoa(magic,mb,16);
    vga_puts_at("Magic: 0x",28,14,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at(mb,        37,14,VGA_DARK_GREY,VGA_BLACK);
    vga_puts_at("[",19,16,VGA_WHITE,VGA_BLACK);
    vga_puts_at("]",60,16,VGA_WHITE,VGA_BLACK);
    for(int i=0;i<41;i++){
        vga_putchar_at('=',20+i,16,VGA_LIGHT_GREEN,VGA_BLACK);
        busy_wait(300000);
    }
    vga_puts_at("Starting...",20,18,VGA_LIGHT_GREEN,VGA_BLACK);
    busy_wait(800000);
}

/* Kernel location — used by installer to copy the running ELF to disk */
uint32_t g_kernel_load_addr = 0x00100000;
uint32_t g_kernel_size      = 0; /* set at runtime from linker symbol */

void kernel_main(uint32_t magic, uint32_t mb_info){
    /* Calculate kernel size from linker symbol */
    extern uint32_t _kernel_end;
    g_kernel_size = (uint32_t)&_kernel_end - g_kernel_load_addr;

    /* The multiboot info is read FIRST, before anything touches the screen:
       it carries the framebuffer GRUB set up, and vga_init() has to know
       whether it is driving text memory or pixels before it programs
       anything.  It also carries the command line, so `quiet` is in force
       before the first log line rather than three lines after it. */
    if(magic==MB1_LOADER_MAGIC)       parse_mb1(mb_info);
    else if(magic==MB2_LOADER_MAGIC)  parse_mb2(mb_info);
    else{ parse_mb1(mb_info); parse_mb2(mb_info); }

    /* A framebuffer above 4 GB, paged in, on the ordinary boot path.
       ------------------------------------------------------------------
       Confirmed on Kyan's HP 15-fd0xxx (12th-gen i5, Iris Xe, 16 GB, UEFI):
       the firmware puts the GOP framebuffer above the top of RAM once
       Above-4G decoding is on, GRUB draws on it from 64-bit mode, and the
       32-bit kernel it hands over to cannot form the address at all.  That
       is the whole "boots and shows nothing" story.

       highfb_map() identity-maps the low 4 GB -- so physical == virtual
       stays true for every driver, every DMA buffer and every MMIO
       register in the system -- and redirects one window to the high
       framebuffer.  Only the framebuffer pointer changes meaning.

       `nohighfb` on the kernel command line skips it, which is the way
       back if this ever misbehaves on a machine nobody has tried yet. */
    if(g_fb_unreachable && !g_fb_nomap && g_fb_h && g_fb_pitch){
        uint32_t v, bytes = g_fb_h * g_fb_pitch;
        if(highfb_map(g_fb_hi, g_fb_lo, bytes, &v)){
            g_fb_mapped      = highfb_mechanism();
            g_fb_lo          = v;
            g_fb_unreachable = 0;
            vga_fb_init(v, g_fb_w, g_fb_h, g_fb_pitch, g_fb_bpp);
        }
    }
    /* v0.8.9.6: a framebuffer below 4 GB gets write-combining as well.
       It needs no window, but with paging off its memory type is the
       firmware's MTRR type -- UC for a graphics aperture -- and that is
       the same slow drawing the high one had. `nohighfb` still means
       paging stays off, so it skips this too. */
    else if(!g_fb_unreachable && !g_fb_nomap && g_fb_lo && g_fb_w
            && g_fb_h && g_fb_pitch){
        highfb_wc_low(g_fb_lo, g_fb_h * g_fb_pitch);
    }

    /* `fbdiag` -- see the signal helpers above for why this no longer
       answers by resetting the machine.

       Order is deliberate.  The entry pulse fires FIRST, before a single
       pixel is written, because writing to a framebuffer address the
       firmware did not really hand over is itself one of the things being
       diagnosed.  Then the screen colours, for a machine that does have a
       working display.  Then the counted pulses, which carry the same answer
       with no display at all.  Then a long, countable hold -- so that
       "rebooted after a quarter of a minute" and "rebooted instantly" are
       different observations, where before they were the same one.

       Each round is ONE LONG tone then N short beeps; the LEDs flash with
       the tones, and the final static LED pattern is N in binary
       (caps=4 num=2 scroll=1):

         N=1  001   fbdiag entered, then something after it killed the machine
         N=2  010   no usable framebuffer was supplied
         N=3  011   framebuffer supplied but not 32 bpp
         N=4  100   framebuffer good, and the PIT counts
         N=5  101   framebuffer good, PIT never ticks
         N=6  110   framebuffer is above 4 GB and unreachable

       And on a machine that can show colour:
         blue then green   framebuffer good, PIT counts
         blue then red     framebuffer good, PIT dead
         magenta           framebuffer supplied, wrong depth
         stays black       no usable framebuffer -- listen/watch for pulses

       This block HALTS when it is done.  So a reboot is never its doing:
       an immediate reboot with no tone is a triple fault before or during
       it, because idt_init() has not run yet.  That is the distinction the
       reset-based version could not draw at all. */
    if(g_fb_diag){
        diag_beep_long();      /* "I am here", before a single pixel is written */

        /* The mapping has already happened above, on the ordinary path.
           `fbmap` remains only as a way to force it when something else has
           turned it off. */
        if(g_fb_unreachable && g_fb_map && g_fb_h && g_fb_pitch){
            uint32_t v, bytes = g_fb_h * g_fb_pitch;
            if(highfb_map(g_fb_hi, g_fb_lo, bytes, &v)){
                g_fb_mapped = highfb_mechanism();
                g_fb_lo = v; g_fb_unreachable = 0;
            }
        }

        uint32_t have = (!g_fb_unreachable) && g_fb_lo && g_fb_w && g_fb_h
                        && g_fb_pitch;
        uint32_t answer;
        if(g_fb_unreachable){
            answer = 6;
        } else if(have && g_fb_bpp==32){
            fill_any(0x000040FFu);                    /* blue: I am running */

            /* Does the PIT advance?  Latch channel 0 twice with a busy loop
               between and see whether the count moved.  sleep_ms() polls this
               counter, so a PIT that never ticks hangs every later wait. */
            outb(0x43,0x34); outb(0x40,0xFF); outb(0x40,0xFF);
            outb(0x43,0x00);
            uint32_t c1 = inb(0x40); c1 |= (uint32_t)inb(0x40)<<8;
            diag_spin(20000000u);
            outb(0x43,0x00);
            uint32_t c2 = inb(0x40); c2 |= (uint32_t)inb(0x40)<<8;
            int pit_ok = (c1!=c2);

            diag_spin(300000000u);                     /* hold blue */
            fill_any(pit_ok ? 0x0000C000u : 0x00FF0000u);
            answer = pit_ok ? 4 : 5;
        } else if(have){
            fill_any(0x00FF00FFu);                     /* magenta */
            answer = 3;
        } else {
            answer = 2;                                /* screen stays black */
        }

        /* One long marker, a clear gap, then `answer` short blips.  Once.
           The screen has already answered for 3, 4 and 5; this only has to
           separate 2 from 6, and two blips against six is not a hard count
           even heard once. */
        diag_spin(DIAG_LONG*2u);
        diag_beep_long();
        diag_spin(DIAG_LONG*2u);
        for(uint32_t i=0;i<answer;i++) diag_beep_short();

        /* Then leave the answer in the keyboard LEDs and stop.  No counting,
           no stopwatch: photograph the keyboard.  And because this HALTS
           rather than resets, a machine still sitting here is proof the
           diagnostic finished -- where a reboot now means a fault, which is
           exactly the distinction the first version could not draw. */
        diag_leds_binary(answer);
        for(;;) __asm__ volatile("cli; hlt");
    }

    vga_init();

    /* Order matters here.  The clock comes up first because the splash and
       every later timeout need sleep_ms.  Then the command line is parsed,
       so `quiet` is in force BEFORE the first log line rather than three
       lines after it.  Then the splash, which is the only thing drawn ahead
       of the log and makes no claims about hardware -- the log does that,
       from the drivers. */
    idt_init();
    pit_init(100);
    /* ── v0.8.8: the bottom half, before interrupts are let in ────────
       kpoll_init() has to run BEFORE irq_enable_basic(), because the
       first IRQ0 after that sti calls kpoll_tick().  It is safe either
       way -- the tick checks g_ready -- but a tick arriving before
       init would be a tick that counted into a structure about to be
       zeroed, and "safe because the callee checks" is how a guard
       comes to be the only thing holding an ordering nobody wrote
       down.  The order is written down.  Clients register later, as
       each subsystem comes up. */
    kpoll_init();
    irq_enable_basic();

    /* ── one brand moment, not two ────────────────────────────────────
       shell/bootanim.c's boot_splash() draws the name and version and
       holds for up to 600 ms. The loading screen draws the same three
       lines and then keeps reporting, so showing both means drawing
       the wordmark twice with a blink between -- and paying 600 ms of
       boot for the privilege. The splash is now only for the path
       where there is no loading screen. */
    if(g_want_log || boot_mode != MODE_DESKTOP){
        extern void boot_splash(void);
        boot_splash();
    }

    /* ── THE LOADING SCREEN ───────────────────────────────────────────
       Up from here to "System ready", unless the command line asked
       for the log. It replaces what `quiet` used to give, which was a
       blank screen for the whole of init -- so this is strictly more
       information than before, not less: the ring turns once per
       recorded line and the caption is the line itself.

       kernel/bootsplash.c explains why it is driven by klog() rather
       than by a timer, and include/klog.h's promise that "a kernel
       that dies halfway through init has already said how far it got"
       is kept by the caption.

       `verbose` on the command line skips it. The saved setting
       cannot be consulted yet -- reading the config needs most of the
       drivers this log is about to describe -- so boot_animation(),
       which runs once the filesystem is up, drops the splash and
       replays the whole log if the saved preference turns out to be
       verbose. Nothing is lost either way. */
    /* Desktop boots get the loading screen. SHELL AND SAFE MODE DO
       NOT, and that is deliberate rather than an omission: those are
       the diagnostic entries, the person who picked one is looking for
       what the machine is doing, and a shell that opens on a cleared
       screen after a picture would have thrown away the one thing they
       came for. `verbose` turns it off everywhere for one boot, and
       the saved setting turns it off for good. */
    if(!g_want_log && boot_mode == MODE_DESKTOP){
        extern void bootsplash_begin(void);
        bootsplash_begin();
    }

    /* Read the descriptor tables back out of the CPU rather than describing
       them from memory.  The old boot log said "GDT: 256 descriptors" for
       years; boot.S defines three. */
    {
        struct { uint16_t limit; uint32_t base; } __attribute__((packed)) g, i;
        __asm__ volatile("sgdt %0" : "=m"(g));
        __asm__ volatile("sidt %0" : "=m"(i));
        klog(KLOG_OK, "GDT: %u descriptors at 0x%x, flat 32-bit segments",
             (uint32_t)((g.limit + 1) / 8), g.base);
        klog(KLOG_OK, "IDT: %u gates at 0x%x, PIC remapped 0x20-0x2F",
             (uint32_t)((i.limit + 1) / 8), i.base);
    }
    {
        extern uint32_t pit_hz_actual(void);
        extern uint32_t pit_divisor_actual(void);
        klog(KLOG_OK, "PIT 8253: divisor %u, %u Hz",
             pit_divisor_actual(), pit_hz_actual());
    }

    klog(KLOG_OK, "Multiboot: magic 0x%x, kernel at 0x%x, %u KB",
         magic, g_kernel_load_addr, g_kernel_size / 1024);

    pmm_init(ram_bytes);
    {
        extern uint32_t pmm_total_blocks(void);
        extern uint32_t pmm_free_blocks(void);
        extern uint32_t pmm_reserved_blocks(void);
        uint32_t mb = pmm_total_blocks() * 4 / 1024;
        klog(KLOG_OK, "PMM: %u MB usable, %u KB reserved below the kernel end",
             mb, pmm_reserved_blocks() * 4);
        /* A 32-bit flat map reaches 4 GB and no further, and
           ram_bytes is already clamped to that when the memory map is
           walked. A machine at the ceiling has more memory than this
           kernel can address, and should be told so rather than left
           to wonder where it went. */
        if(mb >= 4088)
            klog(KLOG_WARN, "PMM: at the 4 GB ceiling of a 32-bit flat "
                            "map; any memory above that is not reachable "
                            "without PAE or a 64-bit kernel");
    }
    heap_init();
    {
        /* v0.8.8.5: this said "static pool" unconditionally, which was
           true when the pool was always the 256 KB array and became a
           string literal describing something that might not be the
           case -- the shape tools/check_boot_log.py exists to refuse.
           The heap says where its memory came from and the log repeats
           it. */
        extern uint32_t heap_pool_bytes(void);
        extern int      heap_pool_is_pmm(void);
        uint32_t kb = heap_pool_bytes() / 1024u;
        if(heap_pool_is_pmm())
            klog(KLOG_OK, "Heap: %u KB from the physical allocator", kb);
        else
            klog(KLOG_WARN, "Heap: %u KB static pool -- the physical "
                            "allocator had nothing to give", kb);
    }
    /* What the loader handed over, before saying what was made of it.  On a
       machine that comes up blank this is the whole diagnosis, and `dmesg`
       from a serial or a later boot is the only place it can be read. */
    if(g_fb_mapped)
        klog(KLOG_OK, "Framebuffer %ux%u %u bpp at %x%4x, above 4 GB, paged "
                      "in by %s at %x",
             g_fb_w, g_fb_h, g_fb_bpp, g_fb_hi_orig, g_fb_lo_orig,
             g_fb_mapped==1 ? "PSE-36" : "PAE", g_fb_lo);
    else if(g_fb_unreachable)
        klog(KLOG_ERR, "Framebuffer at %x%4x is above 4 GB and could not be "
                       "paged in (no PSE-36, no PAE)", g_fb_hi, g_fb_lo);
    else if(g_fb_kind==0xFF)
        klog(KLOG_WARN, "Loader supplied no framebuffer; using VGA text memory "
                        "(a UEFI machine has none)");
    else if(g_fb_kind>=0)
        klog(KLOG_WARN, "Loader supplied framebuffer type %u, not linear RGB; "
                        "using VGA text memory", (uint32_t)g_fb_kind);
    else
        klog(KLOG_OK, "Framebuffer %ux%u %u bpp at %x",
             g_fb_w, g_fb_h, g_fb_bpp, g_fb_lo);

    /* v0.8.9.6: and what KIND of memory the framebuffer is, because on
       real hardware that decides whether the desktop is usable. Said only
       when there is a framebuffer: VGA text memory is not this question. */
    if(g_fb_w && !g_fb_unreachable && g_fb_kind < 0){
        if(highfb_wc())
            klog(KLOG_OK, "Framebuffer memory: write-combining (PAT)");
        else
            klog(KLOG_WARN, "Framebuffer memory: not write-combining (%s) "
                            "-- drawing will be slow on real hardware",
                 highfb_nowc ? "`nowc`" : g_fb_nomap ? "`nohighfb`"
                                        : "no PAT on this CPU");
    }

    klog(KLOG_OK, "VGA: text %ux%u, shadow buffer, 16 colours",
         (uint32_t)vga_cols(), (uint32_t)vga_rows());

    keyboard_init();
    if(mouse_enabled&&boot_mode!=MODE_SAFE){
        extern int mouse_ps2_ready(void);
        mouse_init(); irq_enable_mouse();
        /* ── v0.8.8.1: AND IT NOW REPORTS WHAT HAPPENED ──────────────
           v0.8.8 corrected this line's IRQ claim and left the bigger
           one in it. mouse_init() returns silently when the aux device
           does not acknowledge its reset -- `if(mouse_rd()!=0xFA)
           return;` -- and this printed "ok ... mouse polled" either
           way. On the HP, whose trackpad does not answer, the log
           reported a mouse that is not there and nothing anywhere said
           otherwise, which is why the boot log could not help diagnose
           a dead pointer.

           A log line nobody measured is a line that does not get
           written: tools/check_boot_log.py's whole subject, and this
           one had survived the release that added the check's newest
           rule. The earlier note about IRQ12 stands and is on
           irq_enable_mouse() in kernel/idt.c. */
        if(mouse_ps2_ready())
            klog(KLOG_OK, "PS/2: keyboard on IRQ1, mouse polled "
                          "(8042 IRQ12 off)");
        else
            klog(KLOG_WARN, "PS/2: keyboard on IRQ1, no mouse answered "
                            "the aux port (a USB mouse still works)");
    } else {
        klog(KLOG_WARN, "PS/2: keyboard on IRQ1, mouse disabled by boot mode");
    }
    {
        extern void rtc_get_time(uint8_t*, uint8_t*, uint8_t*);
        uint8_t hh = 0, mm = 0, ss = 0;
        rtc_get_time(&hh, &mm, &ss);
        klog(KLOG_OK, "RTC: CMOS clock reads %u:%u:%u",
             (uint32_t)hh, (uint32_t)mm, (uint32_t)ss);
    }
    ramfs_init();
    klog(KLOG_OK, "RAMFS: root mounted");
    /* Disk init — PCI scan, then VirtIO (UTM SE), then ATA fallback */
    extern void pci_init(void);
    extern void ata_init(void);
    extern int  fat12_init(void);
    extern int  fat16_init(void);
    extern void vfs_init(void);
    extern int  virtio_blk_init(void);
    extern int  virtio_scan_drives(void);
    pci_init();
    { extern int pci_count(void);
      klog(KLOG_OK, "PCI: %u devices on bus 0", (uint32_t)pci_count()); }
    /* USB, after the PCI table exists and before anything that might want a
       device off it.

       v0.8.5: ONE call, where there used to be two naming a controller.
       usb_init() registers every back-end -- UHCI and xHCI -- and then
       walks every port of every one of them through a single enumeration.
       It is safe on a machine with no USB hardware at all: it reports what
       it found and returns 0 rather than touching anything.

       Nothing above usb/ names a controller any more, which is the point
       of the layer. See include/usb.h for why it was built with two
       implementations from the first day rather than one. */
    extern int usb_init(void);
    extern int usb_hid_init(void);
    /* ── I2C, v0.8.8.3 ───────────────────────────────────────────
       After PCI, because it walks the device table, and before the
       shell, because `i2c scan` is the one command that answers what
       the trackpad is. It touches no device of its own: i2c_init()
       maps the controllers and resets them, and nothing is addressed
       until somebody asks. */
    i2c_init();
    /* ── the trackpad, v0.8.8.11 ─────────────────────────────────
       i2c_init() maps the controllers; this is the step that finds
       out whether anything on them is a HID device. It probes every
       address and tries the handful of registers an HID descriptor
       is ever at, accepting only one whose first four bytes ARE an
       HID descriptor -- see include/i2c_hid.h on why that is an
       identification and not a guess.

       It is quiet on a machine with no I2C bus, and on a machine
       whose bus has nothing HID on it the log says which addresses
       answered and what was at them, which is the measurement that
       would settle the next step. */
    {
        extern int i2c_hid_init(void);
        if(i2c_bus_count() > 0) i2c_hid_init();
    }

    usb_init();
    /* Keyboards and mice, after the bus has been walked -- usb_hid_init()
       reads the device table usb_init() filled in. On a machine with no
       PS/2 port this is the step that makes it typeable. */
    if(usb_hid_wanted()) usb_hid_init();
    else klog(KLOG_WARN, "USB HID input disabled by nousbkbd; "
                         "the firmware's keyboard emulation is in charge");
    virtio_blk_init();   /* VirtIO block — primary disk on UTM SE (QEMU TCG) */
    /* v0.7.12: enumerate every VirtIO drive during boot, not just when the
       shell or the installer asks.  virtio_drive_count() was zero until one of
       those ran, and the POST screen reads exactly that — so it printed
       "VirtIO BLK .... Not detected" on EVERY boot, including machines whose
       disk was working perfectly two lines later.  Purely a reporting bug, but
       it points anyone debugging a disk problem at the wrong layer. */
    virtio_scan_drives();
    {
        extern int      virtio_drive_count(void);
        extern uint64_t virtio_drive_sectors(int);
        int vd = virtio_drive_count();
        if(vd > 0) klog(KLOG_OK, "VirtIO blk: %u drive(s), drive0 %u MB",
                        (uint32_t)vd, (uint32_t)(virtio_drive_sectors(0)/2048));
        else       klog(KLOG_WARN, "VirtIO blk: not present");
    }
    /* AHCI before legacy ATA: on a machine where both answer, the legacy
       ports are a compatibility shim over the same controller and reach
       at most two of its ports.  See include/ahci.h.  ahci_init() logs
       what it actually found through klog(); nothing is printed here,
       because tools/check_boot_log.py fails the build on boot output that
       is not reporting a real result. */
    { extern int ahci_init(void); ahci_init(); }
    ata_init();          /* ATA PIO fallback — no-ops where 0x1F0 reads 0xFF */
    {
        extern int ata_detected(void);
        extern int ata_lba48(void);
        if(ata_detected())
            klog(KLOG_OK, "ATA: present on the legacy ports, PIO %s",
                 ata_lba48() ? "LBA48" : "LBA28");
        else
            klog(KLOG_WARN, "ATA: nothing on 0x1F0/0x170 (no legacy IDE here)");
    }
    /* A live session does not probe a disk at all.
       vfs_init() already refuses to ROUTE anywhere but the RAM filesystem,
       but these two calls sit above it and ran regardless -- so the
       partition was still being opened and `disk` still reported it
       "mounted (persistent)", which is exactly the sentence someone
       checking whether their other operating system is safe does not want
       to read.  Probing is only a read, but the honest state for live media
       is that nothing was touched, and that has to be true at every layer
       rather than just the one that happens to route writes. */
    /* NVMe before the filesystem probe, so a machine whose only disk is
       NVMe can at least SEE it.  Read-only: see include/nvme.h. */
    { extern int nvme_init(void); nvme_init(); }

    /* USB mass storage, after every FIXED bus and before the filesystem
       probe. v0.8.6.

       After the fixed buses because fs/blkdev.c registers removable
       disks LAST and a stick that renumbered somebody's Windows NVMe
       between the installer's picker and its confirmation screen would
       make the four-sectors guarantee worth nothing. Before the
       filesystem probe because a machine can boot HavenDOS from a USB
       disk and then has to find it.

       ── AND IT RUNS IN A LIVE SESSION, WHICH NEEDS ARGUING ──────────
       Every other disk probe below sits behind g_live, because a live
       session must leave the machine exactly as it found it: "the
       honest state for live media is that nothing was touched."

       A USB stick is not the machine. The rule exists to protect the
       disks that were in the laptop before HavenDOS was booted --
       somebody's Windows install, somebody's Linux Mint -- and a stick
       the person has just plugged into the side is the reason they
       booted the ISO. The first version of this release put msc_probe()
       behind g_live and the result was a live shell in which `usb -vv`
       listed a mass-storage device and `disk` said there were none,
       which is the kind of contradiction v0.8.5.3's welcome screen
       shipped.

       What live mode still guarantees is unchanged, because it is about
       the FIXED disks: fat16_init() and fat12_init() do not run, the VFS
       routes every write to RAM, and nothing mounts a partition that was
       on the machine. `disk` says exactly that rather than "no disk is
       probed", which was the old wording and is no longer true. */
    { extern int msc_probe(void);
      msc_probe(); }

    if(!g_live){
        fat16_init();        /* FAT16 on the first usable disk */
        fat12_init();        /* FAT12 fallback */
    }
    vfs_init();
    {
        extern int vfs_using_fat16(void);
        /* FAT32 has to be reported separately or a system running on one
           gets told "no HavenDOS partition mounted", which is a line that
           sends somebody looking for a fault that is not there.  The two
           numbers come off the volume that was just mounted -- a boot log
           describes what a driver read, never what the code expected. */
        extern int      vfs_using_fat32(void);
        extern uint32_t fat32_total_clusters(void);
        extern uint32_t fat32_cluster_bytes(void);
        /* v0.8.6: "no disk probed" stopped being true when USB mass
           storage started being probed on live media. What live mode
           guarantees is about the disks that were in the machine
           already; a stick is not the machine, and it is the reason
           somebody boots the ISO. */
        if(g_live)                 klog(KLOG_OK, "Live session: this machine's "
                                                 "own disks untouched, RAM "
                                                 "only; USB disks listed");
        else if(vfs_using_fat16()) klog(KLOG_OK, "FAT16: mounted, VFS ready");
        else if(vfs_using_fat32()) klog(KLOG_OK, "FAT32: mounted, %u clusters of "
                                                 "%u bytes, VFS ready",
                                        fat32_total_clusters(),
                                        fat32_cluster_bytes());
        else                       klog(KLOG_WARN, "no HavenDOS partition mounted "
                                                   "(no FAT16 and no FAT32)");
    }
    extern void ui_load_theme(void);
    extern void cfg_load(void);
    extern int  cfg_console_cols(void);
    extern int  cfg_console_rows(void);
    extern void run_setup_wizard(void);
    cfg_load();          /* load config BEFORE boot_animation so verbose/POST flags are respected */

    /* ── THE GRID FITS THE PANEL ON A MACHINE THAT HAS NEVER CHOSEN ───
       v0.8.8.5. shell/shell_v076.c's header has carried this as a
       known gap since the grid became persistent:

         "The auto-detect-at-boot that RM-1003 asks for still needs
          the viewport layer; this does not."

       THE VIEWPORT LAYER EXISTS NOW (v0.8.8.4), so the reason not to
       do this is gone. An app that lays out for 80x25 is boxed and
       centred rather than stranded in the corner, which is the thing
       that made a big grid unsafe.

       WHY IT MATTERS, MEASURED: 80x25 of an 8x16 cell is 640x400,
       which is 16:10. On a 16:9 panel the console can only ever
       occupy a letterboxed window in the middle -- 1280x800 of
       1920x1080 at the largest whole scale that fits. Kyan reported
       it as "some apps still go off the screen and aren't show fully
       maybe bc of 16:10", and the 16:10 was exactly right: `layout`
       opened all 43 auditable apps at 80x25 and NOT ONE of them drew
       outside its console. The apps were never the problem.

       The decision is vga_fb_boot_grid(), not an `if` here, because
       an `if` here cannot be tested -- see its own header.

       ONLY when nothing has been chosen. A zero in the config means
       "never chosen", so a saved grid, including a deliberate 80x25,
       is left alone. */
    /* ── A SAVED GRID TOO SMALL FOR ANY APP IS NOT A PREFERENCE ───────
       FOUND ON THE HP. v0.8.8.5's `resolution fit` offered 64x24 on
       his 1024x768 panel -- the readable cell, and smaller than the
       80x25 every app lays out for. He took it, it was saved, and
       every later boot came up with a console no app fits in. The
       viewport could not help: there is nowhere to centre an 80x25
       box inside a 64x24 screen. `layout` reported the Dashboard
       3855 cells outside it.

       A saved grid is normally left alone and still is -- but this
       one could only have been chosen because THIS OS OFFERED IT, and
       a console that cannot display a single app is a broken state
       rather than a choice anybody expressed. So it is replaced, once
       and loudly. `resolution` sets it back at any time, and
       `resolution fit` no longer offers a grid that small. */
    {
        extern void cfg_set_console_grid(int c, int r);
        extern void cfg_save(void);
        extern int  vga_fb_app_grid(int *cols, int *rows);
        int sc = cfg_console_cols(), sr = cfg_console_rows();

        if(sc > 0 && sr > 0 && (sc < 80 || sr < 25)){
            int fc = 0, fr = 0;
            klog(KLOG_WARN, "Console: the saved %dx%d grid is smaller "
                            "than the 80x25 every app lays out for",
                 sc, sr);
            if(vga_fb_app_grid(&fc, &fr) && vga_set_mode(fc, fr)){
                cfg_set_console_grid(fc, fr);
                cfg_save();
                klog(KLOG_OK, "Console: %dx%d instead, and saved "
                              "('resolution' changes it)", fc, fr);
            } else {
                klog(KLOG_WARN, "Console: this panel cannot show 80x25 "
                                "at any cell size, so apps will be "
                                "clipped -- 'layout' lists them");
            }
        }
    }

    if(cfg_console_cols() == 0 && cfg_console_rows() == 0){
        int fc = 0, fr = 0;
        if(vga_fb_boot_grid(&fc, &fr)){
            int was_c = vga_cols(), was_r = vga_rows();
            if(vga_set_mode(fc, fr))
                klog(KLOG_OK, "Console: %dx%d fits this panel, up from "
                              "%dx%d (nothing was saved; 'resolution' "
                              "changes it)", fc, fr, was_c, was_r);
            else
                klog(KLOG_WARN, "Console: %dx%d would fit this panel but "
                                "was refused; staying at %dx%d",
                     fc, fr, was_c, was_r);
        }
    }

    extern void ui_load_theme(void);
    extern void cfg_load(void);
    extern int  cfg_console_cols(void);
    extern int  cfg_console_rows(void);
    extern void run_setup_wizard(void);
    cfg_load();          /* load config BEFORE boot_animation so verbose/POST flags are respected */

    /* ── THE GRID FITS THE PANEL ON A MACHINE THAT HAS NEVER CHOSEN ───
       v0.8.8.5. shell/shell_v076.c's header has carried this as a
       known gap since the grid became persistent:

         "The auto-detect-at-boot that RM-1003 asks for still needs
          the viewport layer; this does not."

       THE VIEWPORT LAYER EXISTS NOW (v0.8.8.4), so the reason not to
       do this is gone. An app that lays out for 80x25 is boxed and
       centred rather than stranded in the corner, which is the thing
       that made a big grid unsafe.

       WHY IT MATTERS, MEASURED: 80x25 of an 8x16 cell is 640x400,
       which is 16:10. On the HP's 16:9 panel the console can only
       ever occupy a letterboxed window in the middle -- 1280x800 of
       1920x1080 at the largest whole scale that fits. Kyan reported
       it as "some apps still go off the screen and aren't show fully
       maybe bc of 16:10", and the 16:10 was exactly right: the
       `layout` audit opened all 43 auditable apps at 80x25 and NOT
       ONE of them drew outside its console. The apps were never the
       problem; the console was.

       READABLE and not MAX: vga_fb_readable_grid() keeps the glyph at
       least VGA_READABLE_PX wide, so the text stays the size it is
       now and there is simply more of it. `resolution max` is the
       8-pixel cell people complained about.

       ONLY when nothing has been chosen. A zero in the config means
       "never chosen", so a saved grid -- including a deliberate
       80x25 -- is left alone. And only on the framebuffer console:
       the text console's grid is a CRTC register table, and changing
       one at boot with nobody watching is how a machine comes up with
       a display that will not sync. */
    if(vga_fb_active() && cfg_console_cols() == 0 && cfg_console_rows() == 0){
        int fc = 0, fr = 0;
        /* ── ONLY UPWARDS ─────────────────────────────────────────────
           FOUND BY BOOTING IT, on a 640x384 framebuffer. 80x25 of an
           8x16 cell needs 640x400, so that panel cannot show the
           stock grid at all -- and vga_fb_readable_grid() dutifully
           returned 40x12, which is the biggest grid whose glyphs stay
           16 pixels wide.

           Taking it would have been the complaint this change exists
           to fix, backwards: a 40x12 console boxes nothing, because
           it is SMALLER than 80x25, so every app would draw past the
           right-hand edge and off the bottom with nothing to clip it
           but the grid itself.

           So this only ever makes the grid bigger. A panel that
           cannot do 80x25 keeps whatever the console came up with,
           which is the behaviour it had before this release and is
           the one the apps were written for. */
        if(vga_fb_readable_grid(VGA_READABLE_PX, &fc, &fr) &&
           fc >= 80 && fr >= 25 &&
           (fc > vga_cols() || fr > vga_rows())){
            int was_c = vga_cols(), was_r = vga_rows();
            if(vga_set_mode(fc, fr)){
                klog(KLOG_OK, "Console: %dx%d fits this panel, up from "
                              "%dx%d (nothing was saved; 'resolution' "
                              "changes it)", fc, fr, was_c, was_r);
            } else {
                klog(KLOG_WARN, "Console: %dx%d would fit this panel but "
                                "was refused; staying at %dx%d",
                     fc, fr, was_c, was_r);
            }
        }
    }
    ui_load_theme();

    /* v0.7.12: net_init() used to run AFTER boot_animation(), so the POST
       screen asked rtl8139_found()/e1000_found() before either driver had been
       probed.  The answer was structurally guaranteed to be "no": every boot
       reported "NIC ..... Not detected" regardless of the hardware present.
       The NIC is brought up first now; nothing in boot_animation() feeds
       net_init(), so the order is free to change. */
    extern void net_init(void);
    net_init();
    /* ── v0.8.7: a USB NIC is found here, not in net_init() ─────────
       net_init() scans PCI. A USB NIC appears when the USB bus is
       walked, which happened above -- so the probe goes after the NIC
       table exists and before anything asks whether there is a
       network.

       And the report asks the NIC LAYER rather than two drivers. The
       line it replaces called rtl8139_found() and e1000_found()
       directly, which is the shape net/netdev.c was written to delete:
       a third NIC meant a third branch, and forgetting it meant a
       machine with a working adapter reporting none. */
    {
        extern int usbnet_probe(void);
        int nu = usbnet_probe();
        int act = netdev_active();
        (void)nu;
        if (act >= 0)
            klog(KLOG_OK, "NIC: %s, %d adapter(s)",
                 netdev_bus_name(act), netdev_count());
        else
            klog(KLOG_WARN, "NIC: no adapter, networking off");
    }

    /* The saved verbose setting, applied as late as it can be read and
       as early as it can be applied. The argument says whether a
       loading screen was ever in play: shell and safe mode never have
       one, and must keep their log either way -- see bootanim.c. */
    boot_animation(!g_want_log && boot_mode == MODE_DESKTOP);
    /* DHCP — auto-configure IP/mask/gw/dns. Falls back to static
       10.0.2.15/24 gw 10.0.2.2 if DHCP server doesn't respond.      */
    if(net_ready()){
        extern int dhcp_request(void);
        extern uint32_t net_get_ip(void);
        int nic = netdev_active();
        /* v0.8.7.5: ask the link before spending four seconds asking a
           network that is not there. A USB Ethernet adapter that is
           really a phone reports no carrier until the phone is actually
           sharing its connection, and "no carrier" is a different thing
           to say than "no DHCP answer": the first names the cause, the
           second only describes a symptom of it.

           This check is itself a control transfer, so until the event
           ring had a single owner its answer could not be trusted
           either -- which is why 0.8.7.4 could not have used it. */
        int carrier = (nic >= 0) ? netdev_link_up(nic) : 0;
        uint32_t ip;

        if (!carrier)
            klog(KLOG_WARN, "NET: %s reports no carrier; DHCP skipped",
                 (nic >= 0) ? netdev_bus_name(nic) : "eth0");
        else
            dhcp_request();   /* ~4s timeout max; non-blocking on failure */

        ip = net_get_ip();
        /* net.c keeps the first octet in the LOW byte (see DEFAULT_IP), so
           this reads up, not down.  The first version of this line printed
           15.2.0.10 -- which is the sort of thing a log made of real values
           shows you immediately and a log made of literals never can. */
        /* v0.8.7.2: an address is not a configuration. The default is
           QEMU's, and announcing it with an "ok" on a machine whose
           network has never heard of it is how a boot log reports
           success for a link that cannot route. */
        if (net_config_source() != NET_CFG_FALLBACK)
            klog(KLOG_OK, "NET: eth0 %u.%u.%u.%u",
                 ip & 0xFF, (ip >> 8) & 0xFF,
                 (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
        else if (!carrier)
            klog(KLOG_WARN, "NET: eth0 on the fallback %u.%u.%u.%u, and "
                 "nothing is on the other end of the cable",
                 ip & 0xFF, (ip >> 8) & 0xFF,
                 (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
        else
            klog(KLOG_WARN, "NET: no DHCP answer; eth0 on the fallback "
                 "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF,
                 (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    }

    /* ── v0.8.8 "Helios": the periodic work, registered last ──────────
     *
     * After the one-shot DHCP attempt above and after the bus walk, so
     * that neither client's first run re-does something that has just
     * been done: usb_kpoll_register() takes a BASELINE of the root
     * ports, and a baseline taken before enumeration would read every
     * occupied port as a new arrival and rescan a bus nobody had
     * touched.
     *
     * The clients and what each one is for are in net/net_tick.c and
     * usb/hotplug.c.  Registration is here, in one place, rather than
     * inside each subsystem's init -- so the list of everything this
     * machine does on a timer is four lines in one file, and `kpoll`
     * can be checked against it by eye. */
    {
        extern void net_kpoll_register(void);
        extern int  usb_hc_count(void);
        extern void usb_kpoll_register(void);
        net_kpoll_register();
        if (usb_hc_count() > 0) usb_kpoll_register();
        klog(KLOG_OK, "kpoll: %d periodic task(s) at %d Hz",
             kpoll_client_count(), KPOLL_HZ);
    }

    create_sysfiles();
    shell_init();

    extern int  vfs_exists(const char*);
    extern void login_welcome(const char*, int);
    extern void reminders_load(void);

    /* ── Disk probe + boot decision ────────────────────────────────
       v0.7.4: bus-agnostic disk detection.
       Probes VirtIO then ATA/SATA.  Three outcomes:

       A) Disk found AND has a valid HavenDOS FAT16 partition
          → boot straight to desktop (normal case after install).

       B) Disk(s) found but unformatted / blank
          → go to installer (new install).

       C) No disk at all
          → go to installer which shows the "no disk" error screen.

       If force_install flag is set (GRUB menu "Reinstall" entry)
       always go to installer regardless.

       Disk picker: if multiple disks found and none is already
       formatted, show a simple list and let the user pick.
       If exactly one disk found, auto-select it silently.
    ─────────────────────────────────────────────────────────────── */
    extern int  vfs_using_fat16(void);
    extern void iso_install_wizard(void);

    /* Shell and safe mode are a deliberate choice from the boot menu, so they
       are honoured even on install media.  They used to be swallowed by the
       line below: the menu offered "Shell Mode", the user picked it, and the
       installer came up anyway.  A live shell on the ISO is also the only way
       to look at a machine before committing to it - lspci, usb -vv, dmesg
       and ifconfig all work with nothing installed - and it is where the
       installer can be run by hand on a system that has an image to install. */
    int live_shell = (boot_mode==MODE_SHELL || boot_mode==MODE_SAFE);

    /* In live mode nothing is mounted by design, so "no FAT16 partition"
       is the expected state and must NOT be read as "this machine needs
       installing".  Without this the desktop entry would drop straight
       into the installer on the one machine we are trying to protect. */
    if(g_live) live_shell = 1;

    if((!vfs_using_fat16() && !live_shell) || force_install){
        /* fat16_init() (called earlier in vfs_init) already tried
           both buses.  If it failed, we're running from ISO with no
           formatted HavenDOS partition — go to installer.            */
        iso_install_wizard();
        vga_clear();
        for(int y=0;y<25;y++) for(int x=0;x<80;x++)
            vga_putchar_at(' ',x,y,VGA_BLACK,VGA_BLUE);
        vga_puts_at("Installation cancelled or failed.",23,11,VGA_LIGHT_GREY,VGA_BLUE);
        vga_puts_at("Remove the ISO and reboot to try again.",21,13,VGA_DARK_GREY,VGA_BLUE);
        for(;;) __asm__ volatile("hlt");
    }

    reminders_load();

    /* First-boot after fresh install: run post-install setup wizard */
    int first_boot = !cfg_setup_done();

    klog(KLOG_OK, "System ready");

    /* ── v0.8.8: THE LIVE ECHO STOPS HERE ─────────────────────────────
     *
     * FOUND BY BOOTING IT, in harness/helios_boot.py, on the first run
     * after the periodic clients were wired up.
     *
     * klog's echo exists so that "a kernel that dies halfway through
     * init has already said how far it got" -- include/klog.h says so,
     * and it is the entire reason the log is not an animation that
     * plays after everything succeeded. That argument is about INIT,
     * and it stops applying the moment init is over.
     *
     * It was safe to leave on because every klog call came from a
     * driver bringing itself up, in a fixed order, before anything was
     * on the screen. THAT IS NO LONGER TRUE. net/net_tick.c's DHCP
     * retry logs a line at 10 seconds, 40 seconds, 100 seconds; a
     * carrier transition logs one whenever the cable moves; a hot-plug
     * rescan logs several. Those arrive from kernel_poll(), which runs
     * from the keyboard's idle loop -- which is to say, at the exact
     * moments the screen belongs to somebody else.
     *
     * The first boot after wiring it up showed `dmesg`'s pager prompt
     * with a DHCP line printed over the top of it:
     *
     *     -- more -- (any key, Q to stop)[  28.874]      dhcp: no addre
     *     ss yet, asking again (try 1 of 5)
     *
     * and the harness, reading the last line of the screen, concluded
     * the pager had closed. In a text editor or a form field it would
     * be worse than confusing.
     *
     * So the ring keeps every line and `dmesg` is the view of it, which
     * is what a printk ring is for. Shell mode still starts underneath
     * the log it has already printed; what it does not get is a line
     * arriving on top of its prompt a minute later. */
    klog_set_echo(0);

    /* The loading screen comes down here, at the same moment and for
       the same reason the echo stops: init is over, so the thing the
       splash was reporting on has finished. No replay -- the log was
       not what was on screen, and dumping it now would paint
       twenty-four lines the person did not ask for over the desktop
       that is about to draw. `dmesg` has all of it. */
    {
        extern int  bootsplash_active(void);
        extern void bootsplash_end(int replay);
        if(bootsplash_active()) bootsplash_end(0);
    }

    /* Shell mode keeps the log on screen and starts underneath it, the way a
       Linux console does -- on a machine this fast the log would otherwise
       be visible for about a second and then gone.  The desktop and the
       login screen need the whole screen, so they still get a clean one;
       `dmesg` replays the log in either case. */
    if(boot_mode!=MODE_SHELL && boot_mode!=MODE_SAFE) vga_clear();

#if HAVENDOS_PRERELEASE
    /* ── THE EARLY-ACCESS NOTICE ──────────────────────────────────────
       v0.8.8.8, asked for directly: "the beta phases shows warning
       that this is early access and beta software".

       After the splash and before anything that looks like a finished
       operating system, because once the desktop is up HavenDOS looks
       far more finished than it is -- and the person in front of it
       may not be the person who built it.

       Five seconds, any key skips it, and it is positioned off
       vga_cols()/vga_rows() rather than 80x25: v0.8.8.7 spent a
       release finding every literal that assumed that grid and this
       is not going to be a new one. */
    if(boot_mode != MODE_SAFE){
        extern int  keyboard_haschar(void);
        extern char keyboard_getchar(void);
        const int W = vga_cols(), H = vga_rows();
        const int top = H / 2 - 4;
        int x, y, i;

        /* Drain first. The key that picked the GRUB entry, or that
           dismissed the splash, is still in the 8042 buffer -- the
           same keystroke-meant-for-something-else that dismissed the
           loading screen in v0.8.8.4 and the crash window before it.
           A notice nobody sees is not a notice. */
        while(keyboard_haschar()) (void)keyboard_getchar();

        /* ── REDRAWN EVERY SLICE, ON PURPOSE ───────────────────────
           Drawn once and then waited on, this notice was measured --
           by screenshotting the boot every 180 ms and counting the
           frames carrying its colour -- as visible for about ONE
           second out of the five it waits. Two rewrites of the WAIT
           produced exactly the same duration, and so did compiling
           the keypress exit out entirely, which means nothing was
           cutting the wait short: something inside the loop was
           painting over the notice while it ran. sleep_ms() pushes
           the shadow buffer with vga_present() and kernel_poll()
           services every driver in the tree, any of which may log.

           Finding WHICH is a different afternoon. Redrawing is four
           hundred cells per slice on a screen nobody is using yet,
           it is correct whichever of them it was, and it cannot
           regress if a future driver starts logging here. */
        for(i = 0; i < 50; i++){
            for(y = 0; y < H; y++)
                for(x = 0; x < W; x++)
                    vga_putchar_at(' ', x, y, VGA_BLACK, VGA_BLACK);
            for(x = 0; x < W; x++){
                vga_putchar_at(' ', x, top - 1, VGA_BLACK, VGA_BROWN);
                vga_putchar_at(' ', x, top + 7, VGA_BLACK, VGA_BROWN);
            }
            {
                const char *l1 = HAVENDOS_EA_LINE1;
                const char *l2 = HAVENDOS_EA_LINE2;
                const char *l3 = HAVENDOS_FULL;
                const char *l4 = "press any key";
                vga_puts_at(l3, (W - (int)strlen(l3)) / 2, top + 1,
                            VGA_LIGHT_CYAN, VGA_BLACK);
                vga_puts_at(l1, (W - (int)strlen(l1)) / 2, top + 3,
                            VGA_YELLOW, VGA_BLACK);
                vga_puts_at(l2, (W - (int)strlen(l2)) / 2, top + 5,
                            VGA_LIGHT_GREY, VGA_BLACK);
                vga_puts_at(l4, (W - (int)strlen(l4)) / 2, top + 9,
                            VGA_DARK_GREY, VGA_BLACK);
            }
            vga_flush();

            sleep_ms(100);
            kernel_poll();   /* kpoll-site-ok: this notice holds the boot path for five seconds with no other loop running, so the timer tick, the NIC and the USB stack have nowhere else to be serviced from */
            if(i >= 4 && keyboard_haschar()){   /* 400 ms of settle */
                (void)keyboard_getchar();
                break;
            }
        }
        vga_clear();
    }
#endif

    /* Apply flip settings for Mode13h */
    mode13_set_flip(gfx_flip_x, gfx_flip_y);

    /* Helper: run desktop in chosen graphics mode with fallback */
    #define RUN_DESKTOP() do { \
        if(current_gfx==GFX_MODE13){ \
            if(mode13_try_enter()) mode13_desktop_run(); \
            else { vga_init(); vga_puts_at("Mode13h failed - text fallback",0,0,VGA_YELLOW,VGA_BLACK); busy_wait(2000000); desktop_run(); } \
        } else if(current_gfx==GFX_VESA640||current_gfx==GFX_VESA800){ \
            vesa_desktop_run(); \
        } else { \
            desktop_run(); \
        } \
    } while(0)

    switch(boot_mode){
        case MODE_DESKTOP:
            if(first_boot) run_setup_wizard();
            login_screen();
            RUN_DESKTOP();
            break;
        case MODE_SHELL:
            vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
            vga_puts("HavenDOS " HAVENDOS_VER " Shell\nType 'exit' for desktop.\n\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
            shell_run();
            login_screen();
            RUN_DESKTOP();
            break;
        case MODE_SAFE:
            vga_set_color(VGA_YELLOW,VGA_BLACK);
            vga_puts("Safe Mode\n\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
            shell_run();
            break;
        case MODE_LOWRES:
            if(mode13_try_enter()) mode13_desktop_run();
            else { vga_init(); desktop_run(); }
            break;
    }

    vga_puts("\nSession ended. Rebooting...\n");
    busy_wait(20000000);
    machine_reset();
}
