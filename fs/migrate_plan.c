/* fs/migrate_plan.c -- v0.9.6: the pure half of `migrate`.
 *
 * Names, menus, and what a partition's first sectors say it holds. No
 * disk, no driver, no kernel: tests/test_migrate.c links this file as it
 * is, which is the point of keeping it apart from fs/migrate.c.
 */
#include "../include/types.h"
#include "../include/string.h"
#include "../include/version.h"
#include "../include/migrate.h"

/* ── names FAT16 cannot hold ─────────────────────────────────────────
   Every one is a dotfile in the root. On FAT16 each is stored under a
   mangled 8.3 name ("_ACCOUNT" since 0.9.3, blank-name ".ACC" before) and
   found by the driver from its real name; on FAT32 it is stored as itself.
   Copied entry by entry, they would arrive on FAT32 as "_account" and the
   new system would ask for ".accounts", find nothing, and start from a
   blank -- no users, no password, which is to say the setup screen.

   tools/check_migrate_names.py keeps this list honest: it fails the build
   when the source asks the filesystem for a name that is not 8.3-legal
   and is not here. */
const char *const MIG_LONG_NAMES[] = {
    ".accounts",          /* shell/login.c         -- the users, hashed   */
    ".theme",             /* shell/ui.c                                    */
    ".shell_history",     /* shell/shell.c                                 */
    ".loginlog",          /* shell/login.c                                 */
    ".bpkg_db",           /* shell/bpkg.c          -- installed packages  */
    ".calc_history",      /* shell/desktop.c                               */
    ".reminders",         /* shell/desktop.c                               */
    ".havendos_config",   /* kernel/config.c                               */
    ".pairrec",           /* usb/pair.c            -- the iPhone's pairing */
    ".desktop",           /* older desktops, read once and moved           */
    0
};

static char up(char c){ return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static char lo(char c){ return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* The same 11 bytes fs/fat16.c's name_to_83() makes, lower-cased the way
   its name_from_83() prints them. Written out again rather than shared:
   fs/fat16.c's is static, and test_migrate.c checks the two against each
   other through a FAT16 image made by the real driver's rules (mtools'). */
static void from83(const char *n8, const char *e3, char *out){
    int j = 0, i, has = 0;
    for(i = 0; i < 8 && n8[i] != ' '; i++) out[j++] = lo(n8[i]);
    for(i = 0; i < 3; i++) if(e3[i] != ' ') has = 1;
    if(has){
        out[j++] = '.';
        for(i = 0; i < 3 && e3[i] != ' '; i++) out[j++] = lo(e3[i]);
    }
    out[j] = 0;
}

void mig_fat16_name(const char *real, char *out){
    char n8[8], e3[3];
    int i = 0;
    const char *p = real;
    memset(n8, ' ', 8); memset(e3, ' ', 3);
    if(p[0] == '.' && p[1]){ n8[i++] = '_'; p++; }
    while(*p && *p != '.' && i < 8) n8[i++] = up(*p++);
    while(*p && *p != '.') p++;
    if(*p == '.'){
        p++; i = 0;
        while(*p && i < 3) e3[i++] = up(*p++);
    }
    from83(n8, e3, out);
}

void mig_fat16_legacy_name(const char *real, char *out){
    /* Before 0.9.3 the basename loop stopped at the leading dot at once:
       a blank name, and the first three letters after it as extension. */
    char n8[8], e3[3];
    int i = 0;
    const char *p = real;
    memset(n8, ' ', 8); memset(e3, ' ', 3);
    while(*p && *p != '.' && i < 8) n8[i++] = up(*p++);
    while(*p && *p != '.') p++;
    if(*p == '.'){
        p++; i = 0;
        while(*p && i < 3) e3[i++] = up(*p++);
    }
    from83(n8, e3, out);
}

static int streq_ci(const char *a, const char *b){
    while(*a && *b){ if(lo(*a) != lo(*b)) return 0; a++; b++; }
    return *a == *b;
}

int mig_is_mapped_name(const char *listed){
    char f[16];
    for(int i = 0; MIG_LONG_NAMES[i]; i++){
        mig_fat16_name(MIG_LONG_NAMES[i], f);
        if(streq_ci(f, listed)) return 1;
        mig_fat16_legacy_name(MIG_LONG_NAMES[i], f);
        if(streq_ci(f, listed)) return 1;
    }
    return 0;
}

/* ── what is already in a partition ──────────────────────────────────
   The rule this whole command rests on: a partition that holds anything
   recognisable is not empty, whatever its type says, and is never taken.
   Windows' "New Simple Volume" without a format, diskpart's `create
   partition primary`, GParted's "unformatted" -- those leave no signature
   and are what this is for. */
static int at(const uint8_t *s, int off, const char *m){
    for(int i = 0; m[i]; i++) if(s[off + i] != (uint8_t)m[i]) return 0;
    return 1;
}

const char *mig_fs_signature(const uint8_t *s0, const uint8_t *s2,
                             const uint8_t *s7, const uint8_t *s128)
{
    if(!s0 || !s2 || !s7 || !s128) return "unreadable";
    if(at(s0, 3, "NTFS    "))       return "NTFS";
    if(at(s0, 3, "EXFAT   "))       return "exFAT";
    if(at(s0, 3, "-FVE-FS-"))       return "BitLocker";
    if(at(s0, 3, "ReFS"))           return "ReFS";
    if(at(s0, 0, "LUKS\xBA\xBE"))   return "LUKS";
    if(at(s0, 0, "XFSB"))           return "XFS";
    if(at(s0, 82, "FAT32"))         return "FAT32";
    if(at(s0, 54, "FAT1"))          return "FAT";
    if(at(s0, 3, "HAVEN"))          return "HavenDOS";
    if(s2[56] == 0x53 && s2[57] == 0xEF) return "ext2/3/4";
    if(at(s7, 502, "SWAPSPACE2") || at(s7, 502, "SWAP-SPACE")) return "Linux swap";
    if(at(s128, 64, "_BHRfS_M"))    return "btrfs";
    /* A boot-sector signature with none of the above: something we do not
       know, which is a reason to stay off it, not to guess. */
    if(s0[510] == 0x55 && s0[511] == 0xAA) return "an unknown filesystem";
    return 0;
}

static const uint8_t T_MSDATA[16] = { 0xA2,0xA0,0xD0,0xEB, 0xE5,0xB9, 0x33,0x44,
                                      0x87,0xC0, 0x68,0xB6,0xB7,0x26,0x99,0xC7 };
static const uint8_t T_LINUX[16]  = { 0xAF,0x3D,0xC6,0x0F, 0x83,0x84, 0x72,0x47,
                                      0x8E,0x79, 0x3D,0x69,0xD8,0x47,0x7D,0xE4 };
static const uint8_t T_ESP[16]    = { 0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11,
                                      0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B };
static const uint8_t T_MSR[16]    = { 0x16,0xE3,0xC9,0xE3, 0x5C,0x0B, 0xB8,0x4D,
                                      0x81,0x7D, 0xF9,0x2D,0xF0,0x02,0x15,0xAE };
static const uint8_t T_WINRE[16]  = { 0xA4,0xBB,0x94,0xDE, 0xD1,0x06, 0x40,0x4D,
                                      0xA1,0x6A, 0xBF,0xD5,0x01,0x79,0xD6,0xAC };
static const uint8_t T_HAVEN[16]  = { 'H','A','V','E','N','D','O','S',
                                      '-','P','A','R','T','-','0','1' };

static int geq(const uint8_t *a, const uint8_t *b){
    for(int i = 0; i < 16; i++) if(a[i] != b[i]) return 0;
    return 1;
}

int mig_type_takeable(const uint8_t t[16]){
    return geq(t, T_MSDATA) || geq(t, T_LINUX);
}

const char *mig_type_name(const uint8_t t[16]){
    if(geq(t, T_MSDATA)) return "Windows basic data";
    if(geq(t, T_LINUX))  return "Linux data";
    if(geq(t, T_ESP))    return "EFI System";
    if(geq(t, T_MSR))    return "Microsoft reserved";
    if(geq(t, T_WINRE))  return "Windows recovery";
    if(geq(t, T_HAVEN))  return "HavenDOS";
    return "unknown type";
}

/* ── menus ─────────────────────────────────────────────────────────── */
void mig_serial_str(uint32_t v, char *out){
    static const char hx[] = "0123456789abcdef";
    for(int i = 0; i < 4; i++) out[i]     = hx[(v >> (28 - 4 * i)) & 15];
    out[4] = '-';
    for(int i = 0; i < 4; i++) out[5 + i] = hx[(v >> (12 - 4 * i)) & 15];
    out[9] = 0;
}

typedef struct { char *p; int n, cap, bad; } sb_t;
static void put(sb_t *b, const char *s){
    while(*s){
        if(b->n + 1 >= b->cap){ b->bad = 1; return; }
        b->p[b->n++] = *s++;
    }
    b->p[b->n] = 0;
}

/* The kernel arguments both menus use -- the same ones fs/esp.c and
   fs/install.c write, which tests/test_esp.c holds equal -- and the volume
   named. Without vol= the kernel takes the first HavenDOS partition it
   finds, and with two on the disk that is the wrong one half the time. */
#define MIG_ARGS "mode=desktop gfx=text"

static void entry(sb_t *b, const char *title, uint32_t serial){
    char ser[10];
    mig_serial_str(serial, ser);
    put(b, "menuentry \""); put(b, title); put(b, "\" {\n");
    put(b, "    search --no-floppy --set=root --fs-uuid "); put(b, ser); put(b, "\n");
    put(b, "    multiboot /boot/havendos.elf " MIG_ARGS " vol="); put(b, ser); put(b, "\n");
    put(b, "    boot\n}\n\n");
}

int mig_esp_menu(char *out, int cap, uint32_t new_serial, uint32_t old_serial){
    sb_t b = { out, 0, cap, 0 };
    if(!out || cap < 2) return -1;
    out[0] = 0;
    put(&b, "# Written by HavenDOS " HAVENDOS_VER " `migrate`.\n");
    put(&b, "# Each HavenDOS entry names its partition by volume serial, so\n");
    put(&b, "# two of them on one disk cannot be confused. Windows stays the\n");
    put(&b, "# default, as it was.\n");
    put(&b, "set timeout=8\nset default=1\n\n");
    entry(&b, "HavenDOS " HAVENDOS_VER " " HAVENDOS_CHANNEL, new_serial);
    put(&b, "menuentry \"Windows Boot Manager\" {\n"
            "    search --no-floppy --set=root --file /EFI/Microsoft/Boot/bootmgfw.efi\n"
            "    chainloader /EFI/Microsoft/Boot/bootmgfw.efi\n"
            "}\n\n");
    if(old_serial)
        entry(&b, "HavenDOS - old FAT16 install (until 'migrate finish')", old_serial);
    put(&b, "menuentry \"Firmware setup\" {\n    fwsetup\n}\n");
    return b.bad ? -1 : b.n;
}

int mig_part_menu(char *out, int cap, uint32_t serial){
    sb_t b = { out, 0, cap, 0 };
    if(!out || cap < 2) return -1;
    out[0] = 0;
    put(&b, "set timeout=5\nset default=0\n\n");
    entry(&b, "HavenDOS " HAVENDOS_VER, serial);
    return b.bad ? -1 : b.n;
}
