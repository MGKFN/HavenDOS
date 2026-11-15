/* tests/test_migrate.c -- v0.9.6: the pure half of `migrate`.
 *
 * fs/migrate_plan.c decides three things that are expensive to get wrong:
 * which names the copy carries under another name (wrong: the new system
 * has no users and runs setup), what counts as an empty partition (wrong:
 * somebody's data is formatted), and what the boot menu says (wrong: the
 * machine boots the old install, or neither). Each is checked here against
 * an outside opinion where one exists: mkfs.vfat / mkfs.ntfs-shaped boot
 * sectors made by hand from the published layouts, mtools for 8.3 names.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../fs/migrate_plan.c"

static int g_pass, g_fail;
static void ck(const char *what, int ok, long info){
    if(ok){ g_pass++; printf("  ok    %s\n", what); }
    else  { g_fail++; printf("  FAIL  %s  (%ld)\n", what, info); }
}
static void head(const char *s){ printf("\n%s\n", s); }

static uint8_t s0[512], s2[512], s7[512], s128[512];
static void zero(void){ memset(s0,0,512); memset(s2,0,512); memset(s7,0,512); memset(s128,0,512); }
static const char *sig(void){ return mig_fs_signature(s0, s2, s7, s128); }

static int has(const char *hay, const char *needle){ return strstr(hay, needle) != 0; }
static int count(const char *hay, const char *needle){
    int n = 0; const char *p = hay;
    while((p = strstr(p, needle))){ n++; p += strlen(needle); }
    return n;
}

int main(void){
    char b[64];
    printf("migrate: names, emptiness, menus\n");
    printf("================================\n");

    head("FAT16's names for the dotfiles (fs/fat16.c name_to_83, 0.9.3 on)");
    mig_fat16_name(".accounts", b);        ck(".accounts      -> _account", !strcmp(b, "_account"), 0);
    mig_fat16_name(".shell_history", b);   ck(".shell_history -> _shell_h", !strcmp(b, "_shell_h"), 0);
    mig_fat16_name(".havendos_config", b); ck(".havendos_config -> _havendo", !strcmp(b, "_havendo"), 0);
    mig_fat16_name(".theme", b);           ck(".theme         -> _theme", !strcmp(b, "_theme"), 0);
    mig_fat16_name("notes.txt", b);        ck("an 8.3 name is itself", !strcmp(b, "notes.txt"), 0);
    mig_fat16_name("verylongname.txt", b); ck("a long one is cut as FAT16 cuts it", !strcmp(b, "verylong.txt"), 0);

    head("and before 0.9.3: a blank name and the first three letters");
    mig_fat16_legacy_name(".accounts", b); ck(".accounts -> .acc", !strcmp(b, ".acc"), 0);
    mig_fat16_legacy_name(".bpkg_db", b);  ck(".bpkg_db  -> .bpk", !strcmp(b, ".bpk"), 0);
    mig_fat16_legacy_name(".theme", b);    ck(".theme    -> .the", !strcmp(b, ".the"), 0);

    head("the copy's skip list: every mapped form, nothing else");
    ck("_account is carried by name", mig_is_mapped_name("_account"), 0);
    ck("_ACCOUNT, any case", mig_is_mapped_name("_ACCOUNT"), 0);
    ck(".acc (legacy) is carried by name", mig_is_mapped_name(".acc"), 0);
    ck("_pairrec (the iPhone pairing) is", mig_is_mapped_name("_pairrec"), 0);
    ck("notes.txt is copied as itself", !mig_is_mapped_name("notes.txt"), 0);
    ck("home is copied as itself", !mig_is_mapped_name("home"), 0);
    ck("_other (an unknown dotfile) is copied as itself", !mig_is_mapped_name("_other"), 0);
    {
        /* two long names landing on one FAT16 entry would copy one file
           under two names and lose the other */
        int clash = 0;
        for(int i = 0; MIG_LONG_NAMES[i]; i++) for(int j = i + 1; MIG_LONG_NAMES[j]; j++){
            char a[16], c[16];
            mig_fat16_name(MIG_LONG_NAMES[i], a); mig_fat16_name(MIG_LONG_NAMES[j], c);
            if(!strcmp(a, c)) clash++;
        }
        ck("no two listed names share a FAT16 entry", clash == 0, clash);
    }
    {
        int ok = 1;
        for(int i = 0; MIG_LONG_NAMES[i]; i++) if(MIG_LONG_NAMES[i][0] != '.') ok = 0;
        ck("every listed name is a dotfile (the rest are 8.3 already)", ok, 0);
    }

    head("what a partition holds: never take one with a filesystem");
    zero();                                 ck("all zero: nothing", sig() == 0, 0);
    zero(); memcpy(s0+3, "NTFS    ", 8); s0[510]=0x55; s0[511]=0xAA;
                                            ck("NTFS", sig() && !strcmp(sig(), "NTFS"), 0);
    zero(); memcpy(s0+3, "NTFS    ", 8);    ck("NTFS without 55AA still NTFS", sig() && !strcmp(sig(), "NTFS"), 0);
    zero(); memcpy(s0+3, "EXFAT   ", 8);    ck("exFAT", sig() && !strcmp(sig(), "exFAT"), 0);
    zero(); memcpy(s0+3, "-FVE-FS-", 8);    ck("BitLocker", sig() && !strcmp(sig(), "BitLocker"), 0);
    zero(); memcpy(s0+3, "ReFS", 4);        ck("ReFS", sig() && !strcmp(sig(), "ReFS"), 0);
    zero(); memcpy(s0+82, "FAT32   ", 8);   ck("FAT32", sig() && !strcmp(sig(), "FAT32"), 0);
    zero(); memcpy(s0+54, "FAT16   ", 8);   ck("FAT16", sig() && !strcmp(sig(), "FAT"), 0);
    zero(); memcpy(s0+54, "FAT12   ", 8);   ck("FAT12", sig() && !strcmp(sig(), "FAT"), 0);
    zero(); memcpy(s0, "LUKS\xBA\xBE", 6);  ck("LUKS", sig() && !strcmp(sig(), "LUKS"), 0);
    zero(); memcpy(s0, "XFSB", 4);          ck("XFS", sig() && !strcmp(sig(), "XFS"), 0);
    zero(); s2[56]=0x53; s2[57]=0xEF;       ck("ext4 (magic at byte 1080)", sig() && !strcmp(sig(), "ext2/3/4"), 0);
    zero(); memcpy(s7+502, "SWAPSPACE2", 10); ck("Linux swap (byte 4086)", sig() && !strcmp(sig(), "Linux swap"), 0);
    zero(); memcpy(s128+64, "_BHRfS_M", 8); ck("btrfs (byte 65600)", sig() && !strcmp(sig(), "btrfs"), 0);
    zero(); s0[510]=0x55; s0[511]=0xAA;     ck("a boot signature we cannot name: refused",
                                               sig() && !strcmp(sig(), "an unknown filesystem"), 0);
    zero(); s0[100]=0x77; s0[300]=0x12;     ck("stray bytes, no signature: not a filesystem", sig() == 0, 0);
    zero();
    ck("a sector that could not be read: refused",
       mig_fs_signature(0, s2, s7, s128) && mig_fs_signature(s0, s2, 0, s128), 0);
    /* and the same, against the real tool */
    if(system("command -v mkfs.vfat >/dev/null 2>&1") == 0){
        if(system("rm -f /tmp/hd_tmig.img; dd if=/dev/zero of=/tmp/hd_tmig.img bs=1M count=40 status=none;"
               "mkfs.vfat -F 32 /tmp/hd_tmig.img >/dev/null 2>&1")) ck("mkfs.vfat ran", 0, 0);
        FILE *f = fopen("/tmp/hd_tmig.img", "rb");
        if(f){
            int ok = fread(s0, 1, 512, f) == 512;
            fseek(f, 1024, SEEK_SET);  ok &= fread(s2, 1, 512, f) == 512;
            fseek(f, 3584, SEEK_SET);  ok &= fread(s7, 1, 512, f) == 512;
            fseek(f, 65536, SEEK_SET); ok &= fread(s128, 1, 512, f) == 512; fclose(f);
            ck("mkfs.vfat's FAT32 is recognised", ok && sig() != 0, 0);
        }
        if(system("rm -f /tmp/hd_tmig.img")){}
    }

    head("partition types: only what a disk tool makes for 'new partition'");
    {
        static const uint8_t msdata[16] = { 0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7 };
        static const uint8_t linux_[16] = { 0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4 };
        static const uint8_t esp[16]    = { 0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B };
        static const uint8_t msr[16]    = { 0x16,0xE3,0xC9,0xE3,0x5C,0x0B,0xB8,0x4D,0x81,0x7D,0xF9,0x2D,0xF0,0x02,0x15,0xAE };
        static const uint8_t winre[16]  = { 0xA4,0xBB,0x94,0xDE,0xD1,0x06,0x40,0x4D,0xA1,0x6A,0xBF,0xD5,0x01,0x79,0xD6,0xAC };
        static const uint8_t haven[16]  = { 'H','A','V','E','N','D','O','S','-','P','A','R','T','-','0','1' };
        ck("Windows basic data: may", mig_type_takeable(msdata), 0);
        ck("Linux data: may", mig_type_takeable(linux_), 0);
        ck("the ESP: never", !mig_type_takeable(esp), 0);
        ck("Microsoft reserved (no filesystem, ever): never", !mig_type_takeable(msr), 0);
        ck("Windows recovery: never", !mig_type_takeable(winre), 0);
        ck("HavenDOS's own: never", !mig_type_takeable(haven), 0);
        ck("and each is named for the screen",
           !strcmp(mig_type_name(msr), "Microsoft reserved") && !strcmp(mig_type_name(winre), "Windows recovery"), 0);
    }

    head("the serial, as GRUB prints a FAT volume's UUID");
    mig_serial_str(0x1A2B3C4Du, b); ck("1a2b-3c4d", !strcmp(b, "1a2b-3c4d"), 0);
    mig_serial_str(0x0000000Fu, b); ck("leading zeros kept: 0000-000f", !strcmp(b, "0000-000f"), 0);

    head("the boot menu while both installs exist");
    {
        static char m[2048];
        int n = mig_esp_menu(m, sizeof m, 0xAAAA1111u, 0xBBBB2222u);
        ck("it fits", n > 0, n);
        ck("the new install, found by its serial", has(m, "--fs-uuid aaaa-1111\n"), 0);
        ck("and told which volume it is", has(m, "mode=desktop gfx=text vol=aaaa-1111\n"), 0);
        ck("the old one, by ITS serial", has(m, "--fs-uuid bbbb-2222\n") && has(m, "vol=bbbb-2222\n"), 0);
        ck("no entry searches by label (both are HAVENDOS)", !has(m, "--label"), 0);
        ck("the new one comes first", strstr(m, "aaaa-1111") < strstr(m, "bbbb-2222"), 0);
        ck("Windows is second and still the default",
           has(m, "set default=1\n") && strstr(m, "bootmgfw") < strstr(m, "bbbb-2222")
           && strstr(m, "bootmgfw") > strstr(m, "aaaa-1111"), 0);
        ck("four entries", count(m, "menuentry ") == 4, count(m, "menuentry "));
        ck("every { is closed", count(m, "{\n") == count(m, "}\n"), 0);
        ck("the old one says how it goes away", has(m, "migrate finish"), 0);
    }
    head("and after `migrate finish`");
    {
        static char m[2048];
        int n = mig_esp_menu(m, sizeof m, 0xAAAA1111u, 0);
        ck("it fits", n > 0, n);
        ck("one HavenDOS entry", count(m, "/boot/havendos.elf") == 1, 0);
        ck("the old serial is nowhere", !has(m, "bbbb"), 0);
        ck("three entries", count(m, "menuentry ") == 3, count(m, "menuentry "));
        ck("Windows still the default", has(m, "set default=1\n") && has(m, "bootmgfw"), 0);
    }
    head("the partition's own menu, and a buffer too small");
    {
        static char m[512];
        ck("the partition menu names its own volume",
           mig_part_menu(m, sizeof m, 0x12345678u) > 0 && has(m, "--fs-uuid 1234-5678") && has(m, "vol=1234-5678"), 0);
        char tiny[64];
        ck("too small: -1, not a truncated menu", mig_esp_menu(tiny, sizeof tiny, 1, 2) == -1, 0);
        ck("and the partition menu too", mig_part_menu(tiny, 20, 1) == -1, 0);
    }

    printf("\n%d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
