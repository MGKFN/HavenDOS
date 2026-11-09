/*
 * update_src.c — where an update comes from.
 *
 * Two sources behind one interface, and one function that chooses between
 * them.  include/update_src.h says why this file exists at all; the short
 * version is that fs/update.c could only read from an ATAPI CD-ROM, and
 * the laptop this project is aimed at has no optical drive.
 *
 * Reading order, if you are picking this up cold:
 *
 *   1. upd_manifest_parse()   the only part with no I/O in it.  The suite
 *                             compiles THIS function natively, extracted
 *                             by tools/make_bars_inc.py, so it cannot
 *                             certify a stale copy of it.
 *   2. the ESP source         fat32vol finds the partition, fs/fat32.c
 *                             reads it, and nothing writes to it.
 *   3. the CD-ROM source      the four steps lifted out of fs/update.c,
 *                             behaving exactly as they did before.
 *   4. upd_source_select()    the one place a source is chosen.
 */

#include "../include/types.h"
#include "../include/string.h"
#include "../include/update_src.h"
#include "../include/fat32.h"
#include "../include/fat32vol.h"
#include "../include/exfat.h"
#include "../include/atapi.h"
#include "../include/iso9660.h"
#include "../include/gpt.h"          /* gpt_crc32 -- see the note below */
#include "../include/blkdev.h"       /* blk_removable() -- the USB source */

/* ════════════════════════════════════════════════════════════════════
   1. The manifest parser
   ════════════════════════════════════════════════════════════════════

   Deliberately self-contained: no strlen, no strcmp, no helper of its
   own.  tools/make_bars_inc.py extracts a function by name and brace
   matching, so anything this called would have to be extracted too, and
   a parser that drifts from the one under test is the whole reason that
   tool exists.

   It is also the reason this is written with explicit indices rather
   than pointer walks.  `text` is a buffer read off a FAT32 volume that
   somebody else's operating system also writes to; it is not a C string
   and it is not promised to be terminated.  Every read here is bounded
   by `len` and nothing walks off the end looking for a NUL. */
int upd_manifest_parse(const char *text, uint32_t len, upd_manifest_t *out)
{
    /* Always fully initialised, including on every refusal below, so a
       caller that ignores the return code cannot read a stale version
       string out of a struct that was left half-filled. */
    {
        uint32_t z;
        for(z = 0; z < UPD_VER_MAX;  z++) out->version[z] = 0;
        for(z = 0; z < UPD_NAME_MAX; z++) out->kernel[z]  = 0;
        out->size = 0; out->crc = 0;
        out->have_version = 0; out->have_kernel = 0;
        out->have_size = 0;    out->have_crc = 0;
    }
    if(!text || len == 0) return UPDM_E_MAGIC;

    /* ── the first line must identify the format ──────────────────
       "HAVENDOS-UPDATE 1".  A file that does not say this is not a
       manifest, and is refused before any field is looked at -- which
       matters because the path it was read from is one a person types
       by hand at a Windows prompt. */
    {
        static const char MAGIC[] = "HAVENDOS-UPDATE";
        uint32_t i = 0;
        while(MAGIC[i]){
            if(i >= len || text[i] != MAGIC[i]) return UPDM_E_MAGIC;
            i++;
        }
        while(i < len && (text[i] == ' ' || text[i] == '\t')) i++;
        /* The format number.  A manifest written for a later HavenDOS
           than this one is refused by number rather than misread field
           by field: that is what a format number is for. */
        {
            uint32_t fv = 0; int any = 0;
            while(i < len && text[i] >= '0' && text[i] <= '9'){
                fv = fv * 10u + (uint32_t)(text[i] - '0'); i++; any = 1;
                if(fv > 9999u) return UPDM_E_FORMAT;
            }
            if(!any)     return UPDM_E_MAGIC;
            if(fv != 1u) return UPDM_E_FORMAT;
        }
        /* Nothing but spaces and CR may follow on that line. */
        while(i < len && (text[i] == ' ' || text[i] == '\t' ||
                          text[i] == '\r')) i++;
        if(i < len && text[i] != '\n') return UPDM_E_MAGIC;
        if(i < len) i++;

        /* ── the body, one key=value per line ─────────────────────── */
        while(i < len){
            uint32_t ks, ke, vs, ve;

            /* skip blank space at the start of a line */
            while(i < len && (text[i] == ' ' || text[i] == '\t' ||
                              text[i] == '\r' || text[i] == '\n')) i++;
            if(i >= len) break;
            if(text[i] == '#'){                 /* a comment line */
                while(i < len && text[i] != '\n') i++;
                continue;
            }
            ks = i;
            while(i < len && text[i] != '=' && text[i] != '\n') i++;
            if(i >= len || text[i] != '=') {
                /* A line with no '=' on it.  Skip to the end of it and
                   carry on rather than refusing the whole file: a
                   manifest a later release adds a section header to
                   should still be readable by this one. */
                while(i < len && text[i] != '\n') i++;
                continue;
            }
            ke = i; i++;                        /* step over '='      */
            while(ke > ks && (text[ke-1] == ' ' || text[ke-1] == '\t')) ke--;
            while(i < len && (text[i] == ' ' || text[i] == '\t')) i++;
            vs = i;
            while(i < len && text[i] != '\n') i++;
            ve = i;
            while(ve > vs && (text[ve-1] == ' '  || text[ve-1] == '\t' ||
                              text[ve-1] == '\r')) ve--;
            if(i < len) i++;                    /* step over '\n'     */

            /* ── version= ── */
            if(ke - ks == 7 &&
               text[ks  ]=='v' && text[ks+1]=='e' && text[ks+2]=='r' &&
               text[ks+3]=='s' && text[ks+4]=='i' && text[ks+5]=='o' &&
               text[ks+6]=='n'){
                uint32_t n = ve - vs, z;
                if(n == 0) return UPDM_E_VALUE;
                if(n > UPD_VER_MAX - 1) n = UPD_VER_MAX - 1;
                for(z = 0; z < n; z++) out->version[z] = text[vs + z];
                out->version[n] = 0;
                out->have_version = 1;
                continue;
            }
            /* ── kernel= ── */
            if(ke - ks == 6 &&
               text[ks  ]=='k' && text[ks+1]=='e' && text[ks+2]=='r' &&
               text[ks+3]=='n' && text[ks+4]=='e' && text[ks+5]=='l'){
                uint32_t n = ve - vs, z;
                if(n == 0) return UPDM_E_VALUE;
                /* A name, not a path.  The image lives beside the
                   manifest and nowhere else: a manifest that could name
                   ..\..\EFI\Microsoft\Boot\bootmgfw.efi is a manifest
                   that can be pointed at Windows' loader. */
                for(z = vs; z < ve; z++)
                    if(text[z] == '/' || text[z] == '\\' || text[z] == ':')
                        return UPDM_E_VALUE;
                if(n > UPD_NAME_MAX - 1) return UPDM_E_VALUE;
                for(z = 0; z < n; z++) out->kernel[z] = text[vs + z];
                out->kernel[n] = 0;
                out->have_kernel = 1;
                continue;
            }
            /* ── size= , decimal ── */
            if(ke - ks == 4 &&
               text[ks  ]=='s' && text[ks+1]=='i' &&
               text[ks+2]=='z' && text[ks+3]=='e'){
                uint32_t v = 0, z; int any = 0;
                for(z = vs; z < ve; z++){
                    if(text[z] < '0' || text[z] > '9') return UPDM_E_VALUE;
                    /* Refuse rather than wrap.  A size that overflowed
                       silently would be compared against a buffer length
                       and could pass. */
                    if(v > 0x19999999u) return UPDM_E_VALUE;
                    v = v * 10u + (uint32_t)(text[z] - '0');
                    any = 1;
                }
                if(!any) return UPDM_E_VALUE;
                out->size = v; out->have_size = 1;
                continue;
            }
            /* ── crc32= , hex, with or without 0x ── */
            if(ke - ks == 5 &&
               text[ks  ]=='c' && text[ks+1]=='r' && text[ks+2]=='c' &&
               text[ks+3]=='3' && text[ks+4]=='2'){
                uint32_t v = 0, z = vs; int n = 0;
                if(ve - vs > 2 && text[vs] == '0' &&
                   (text[vs+1] == 'x' || text[vs+1] == 'X')) z = vs + 2;
                for(; z < ve; z++){
                    char c = text[z]; uint32_t d;
                    if     (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
                    else if(c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
                    else if(c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
                    else return UPDM_E_VALUE;
                    if(n >= 8) return UPDM_E_VALUE;   /* wider than 32 bits */
                    v = (v << 4) | d; n++;
                }
                if(n == 0) return UPDM_E_VALUE;
                out->crc = v; out->have_crc = 1;
                continue;
            }
            /* Any other key is ignored, so a manifest written by a later
               release still parses here. */
        }
    }

    /* version, size and kernel are required; crc32 is not, because the
       CD-ROM path synthesises a manifest for ISOs made before 0.8.4 and
       there is no checksum to be had from one of those. */
    if(!out->have_version || !out->have_size || !out->have_kernel)
        return UPDM_E_FIELD;
    if(out->size == 0) return UPDM_E_VALUE;
    return UPDM_OK;
}

const char *upd_manifest_strerror(int code)
{
    switch(code){
    case UPDM_OK:         return "ok";
    case UPDM_E_MAGIC:    return "not a HavenDOS update manifest";
    case UPDM_E_FORMAT:   return "manifest is newer than this HavenDOS";
    case UPDM_E_FIELD:    return "manifest is missing version, size or kernel";
    case UPDM_E_VALUE:    return "a manifest value will not parse";
    case UPDM_E_READ:     return "the manifest would not read";
    case UPDM_E_NONE:     return "no manifest here";
    default:              return "unknown manifest error";
    }
}

/* ════════════════════════════════════════════════════════════════════
   2. The EFI System Partition source
   ════════════════════════════════════════════════════════════════════

   probe() leaves the volume MOUNTED when it succeeds, and release()
   unmounts it.  That is stated here because fs/fat32.c holds one volume
   at a time, so this borrows the system's own mount for the duration of
   an update and fat32_vol_release() puts it back.  fs/update.c calls
   release() on every path out, including every failure. */

/* ── ONE MOUNTED FAT32 VOLUME, TWO WAYS OF FINDING IT ────────────────
   v0.8.6.3. The USB source and the ESP source differ ONLY in which
   volume they go looking for: the ESP wants the partition whose type
   GUID says EFI System Partition, a stick wants any FAT32 volume on a
   removable disk. Once one is mounted, reading a manifest and a kernel
   off it is the same work -- so it is the same code. f32_src_manifest()
   and f32_src_read() below serve both source structs, and this flag is
   "a FAT32 volume is open as the update source", whichever probe opened
   it.

   Two copies of that read loop is the shape v0.8.6 spent its release
   deleting three of. */
static int g_f32_open = 0;

static int esp_src_probe(void)
{
    fat32_vol_t v[FAT32_MAX_VOLS];
    int n, i;

    if(g_f32_open) return 1;
    n = fat32_scan_volumes(v, FAT32_MAX_VOLS);
    for(i = 0; i < n; i++){
        if(!v[i].is_esp) continue;              /* found by type GUID    */
        if(!fat32_vol_mount_ro(&v[i])) continue;
        if(fat32_exists(UPD_ESP_MANIFEST)){
            g_f32_open = 1;
            return 1;
        }
        /* An ESP with no update staged on it is not an error and not a
           source.  Put it back and keep looking: a machine can have more
           than one, and the one with the update on it is the one we
           want. */
        fat32_vol_release();
    }
    return 0;
}

static void f32_src_release(void)
{
    if(!g_f32_open) return;
    fat32_vol_release();
    g_f32_open = 0;
}

static int f32_src_manifest(upd_manifest_t *m)
{
    char buf[1024];
    uint32_t sz;
    int r;

    if(!g_f32_open) return UPDM_E_NONE;
    sz = fat32_file_size(UPD_ESP_MANIFEST);
    if(sz == 0) return UPDM_E_NONE;
    if(sz > sizeof buf - 1) return UPDM_E_MAGIC;   /* far too big to be one */
    r = fat32_read_at(UPD_ESP_MANIFEST, 0, buf, sz);
    if(r != (int)sz) return UPDM_E_READ;
    return upd_manifest_parse(buf, sz, m);
}

/* ── THE STAGED KERNEL'S PATH, IN ONE PLACE ──────────────────────────
   UPD_ESP_DIR "/" <kernel>, built here and NOT taken from the
   manifest, so the directory is not something a manifest can choose.

   v0.8.8.2 gave the USB source an exFAT arm, and this block was
   copied into it -- which immediately made three injections in
   tools/check_update_suite.py match twice and go stale, because an
   injection is a pointer into source and duplicated source has two.
   That is the warning this project already carries from v0.8.7.9,
   and it fired within the hour.

   So: one copy. The FAT32 arm and the exFAT arm cannot disagree
   about where a kernel lives, and a rule added here applies to both.
   Returns 0 if the name does not fit. */
static int staged_kernel_path(const upd_manifest_t *m, char *path,
                              int cap)
{
    const char *d = UPD_ESP_DIR;
    int i = 0, j;
    for(i = 0; d[i] && i < cap - 2; i++) path[i] = d[i];
    path[i++] = '/';
    for(j = 0; m->kernel[j] && i < cap - 1; j++)
        path[i++] = m->kernel[j];
    path[i] = 0;
    if(m->kernel[j]) return 0;                  /* name did not fit      */
    return 1;
}

static int f32_src_read(const upd_manifest_t *m, uint8_t *buf, uint32_t bufsz,
                        void (*progress)(uint32_t done, uint32_t total))
{
    char path[96];
    uint32_t off = 0;

    if(!g_f32_open || !m || !buf) return 0;
    if(m->size > bufsz) return 0;

    /* UPD_ESP_DIR "/" kernel, built here rather than in the manifest, so
       the directory is not something a manifest can choose. */
    if(!staged_kernel_path(m, path, (int)sizeof path)) return 0;

    if(!fat32_exists(path)) return 0;
    if(fat32_file_size(path) != m->size) return 0;  /* before a byte is read */

    while(off < m->size){
        uint32_t chunk = m->size - off;
        if(chunk > 32768u) chunk = 32768u;
        if(fat32_read_at(path, off, (char *)(buf + off), chunk) != (int)chunk)
            return 0;
        off += chunk;
        if(progress) progress(off, m->size);
    }
    return 1;
}

static const upd_source_t SRC_ESP = {
    UPD_SRC_ESP, esp_src_probe, f32_src_manifest, f32_src_read, f32_src_release
};

/* ════════════════════════════════════════════════════════════════════
   2b. The USB source
   ════════════════════════════════════════════════════════════════════

   v0.8.6.3.  Staging an update meant booting Windows, mounting the EFI
   System Partition with `mountvol` and copying two files into it.  Every
   piece needed to do it from a USB stick instead arrived in v0.8.6:
   fs/blkdev.c registers USB disks, so fat32_scan_volumes() already walks
   them, and blk_removable() already says which disks are sticks.

   What this source is, exactly: the FIRST FAT32 volume on a REMOVABLE
   disk carrying a manifest at the same path the ESP uses.

   ── WHY THE SAME PATH ────────────────────────────────────────────────
   \EFI\HAVENDOS\update\update.inf is an odd place for a folder on a
   plain stick.  It is used anyway because the stager scripts shipped
   with every release already build exactly that layout -- so
   stage-havendos-update.sh pointed at a mounted stick produces a
   working update stick with no new code and no second convention to
   keep in step.  One layout that looks strange beats two that can
   disagree.

   ── WHY FAT32 AND NOT FAT16 ──────────────────────────────────────────
   Not an oversight, and worth reading before anyone adds it.

   A source stays OPEN until fs/update.c reaches `done:`, which is after
   the new kernel has been written to the system volume.  The ESP source
   holds fs/fat32.c's mount slot for that whole time, which is harmless:
   the kernel is written through fs/fat16.c, a different driver.

   A FAT16 source would have to hold fat16_borrow(), which points THAT
   driver at the stick -- and fs/update.c reads and writes the system
   volume through it in between, to take the backup at step 3.  So the
   backup would be looked for on the stick, and the write would be
   refused by the borrow.

   The fix is not to reorder those steps: step 3's own comment says the
   backup is taken before the image is read "because both use the same
   staging buffer", and there is one 8 MB buffer.  Supporting a FAT16
   stick therefore means changing how `update` stages bytes, which is
   the most dangerous command in this system and does not belong in a
   patch release.  It gets its own.

   Until then a FAT16 stick is refused by name rather than reported as
   no source at all.

   ── WHY exFAT IS FINE AND FAT16 IS NOT, v0.8.8.2 ─────────────────────
   The argument above is NOT about the filesystem. It is about which
   DRIVER the source holds open while fs/update.c writes the system
   volume through fs/fat16.c. A FAT16 stick would hold that same
   driver, and the backup at step 3 would be looked for on the stick.

   fs/exfat.c is its own driver with its own mount slot, exactly like
   fs/fat32.c. Holding it open costs fat16 nothing, so an exFAT stick
   is safe for precisely the reason a FAT32 one is.

   And it is needed: **a USB stick of 32 GB or more is formatted exFAT
   by default**, which is the size somebody reaches for to carry a
   kernel. "USB discovery for 32 GB drives still broken for updating"
   was reported four releases running, and each time the answer was
   this file refusing exFAT on purpose and saying so in small print.

   fs/exfat.c is READ-ONLY, and an update source is a read. The driver
   that cannot be trusted to install can be trusted with this. */
/* ── WHY THE STICK WAS NOT USED, v0.8.6.5 ────────────────────────────
   "It only found the ESP" is four different failures wearing the same
   face: no removable disk on the bus, a removable disk with no FAT32
   volume on it, a volume that would not mount, or a volume with no
   manifest at the path. Each has a different fix and the screen said
   none of them.

   So the probe counts what it saw on the way past, and fs/update.c
   prints it when the source it chose is not the stick. Same idea as
   `fat32`'s "partitions this kernel does not read" list, which is what
   turned "fat32 prints nothing" into an answer in 0.8.6.1. */
static upd_usb_why_t g_usb_why;

void upd_usb_why(upd_usb_why_t *out){ if(out) *out = g_usb_why; }

/* Which driver the USB source is open on. g_f32_open is shared with
   the ESP source; this one is the stick's alone. */
static int g_xf_open = 0;

static int usb_src_probe(void)
{
    fat32_vol_t v[FAT32_MAX_VOLS];
    int n, i;

    if(g_f32_open || g_xf_open) return 1;

    {   /* Removable disks the block layer holds, counted here rather
           than inferred from the volume list: zero of them and zero
           FAT32 volumes on them are different problems. */
        int d;
        g_usb_why.removable = 0;
        g_usb_why.volumes   = 0;
        g_usb_why.mounted   = 0;
        g_usb_why.nomanifest= 0;
        g_usb_why.xvolumes  = 0;
        g_usb_why.xmounted  = 0;
        for(d = 0; d < blk_count(); d++)
            if(blk_removable(d)) g_usb_why.removable++;
    }

    n = fat32_scan_volumes(v, FAT32_MAX_VOLS);
    for(i = 0; i < n; i++){
        /* The whole difference from esp_src_probe(): a removable disk
           rather than an EFI System Partition.  Any FAT32 volume on one
           will do -- a stick formatted by Windows has no ESP type GUID
           and would never be found by the test above. */
        if(!blk_removable(v[i].disk)) continue;
        g_usb_why.volumes++;
        if(!fat32_vol_mount_ro(&v[i])) continue;
        g_usb_why.mounted++;
        if(fat32_exists(UPD_ESP_MANIFEST)){
            g_f32_open = 1;
            return 1;
        }
        g_usb_why.nomanifest++;
        /* A stick with no update staged on it is not an error and not a
           source -- somebody's photos are not a failed update.  Put it
           back and keep looking. */
        fat32_vol_release();
    }

    /* ── THEN exFAT, v0.8.8.2 ────────────────────────────────────────
       FAT32 first, deliberately: it is the format the stager scripts
       have always produced and the one every earlier release could
       read, so a stick that worked before keeps taking the same path.
       exFAT is the fallback, not the preference.

       See the long note above for why this is safe and a FAT16 arm
       still is not. */
    n = exfat_scan_volumes(v, FAT32_MAX_VOLS);
    for(i = 0; i < n; i++){
        exfat_dirent_t st;
        if(!blk_removable(v[i].disk)) continue;
        g_usb_why.xvolumes++;
        if(!exfat_vol_mount_ro(&v[i])) continue;
        g_usb_why.xmounted++;
        if(exfat_stat(UPD_ESP_MANIFEST, &st) == EXFAT_OK && !st.is_dir){
            g_xf_open = 1;
            return 1;
        }
        g_usb_why.nomanifest++;
        exfat_unmount();
    }
    return 0;
}

/* ── THE exFAT ARM OF THE USB SOURCE ─────────────────────────────────
   The USB source can now be open on either driver, so its three
   operations dispatch on which. The ESP source is untouched and still
   goes straight to the FAT32 ones -- an EFI System Partition is FAT32
   by specification and there is nothing to choose between. */

static int usb_src_manifest(upd_manifest_t *m)
{
    char buf[1024];
    exfat_dirent_t st;
    int r;

    if(!g_xf_open) return f32_src_manifest(m);
    if(exfat_stat(UPD_ESP_MANIFEST, &st) != EXFAT_OK) return UPDM_E_NONE;
    if(st.size == 0) return UPDM_E_NONE;
    if(st.size > sizeof buf - 1) return UPDM_E_MAGIC;  /* not a manifest */
    r = exfat_read(UPD_ESP_MANIFEST, 0, buf, (uint32_t)st.size);
    if(r != (int)st.size) return UPDM_E_READ;
    return upd_manifest_parse(buf, (uint32_t)st.size, m);
}

static int usb_src_read(const upd_manifest_t *m, uint8_t *buf, uint32_t bufsz,
                        void (*progress)(uint32_t done, uint32_t total))
{
    char path[96];
    exfat_dirent_t st;
    uint32_t off = 0;

    if(!g_xf_open) return f32_src_read(m, buf, bufsz, progress);
    if(!m || !buf) return 0;
    if(m->size > bufsz) return 0;

    /* UPD_ESP_DIR "/" kernel, built here and not taken from the
       manifest, so the directory is not something a manifest can
       choose. The same rule as the FAT32 arm, and the same code
       shape -- if one of these ever grows a check the other must. */
    if(!staged_kernel_path(m, path, (int)sizeof path)) return 0;

    if(exfat_stat(path, &st) != EXFAT_OK) return 0;
    if(st.is_dir) return 0;
    if(st.size != m->size) return 0;            /* before a byte is read */

    while(off < m->size){
        uint32_t chunk = m->size - off;
        if(chunk > 32768u) chunk = 32768u;
        if(exfat_read(path, (uint64_t)off, buf + off, chunk) != (int)chunk)
            return 0;
        off += chunk;
        if(progress) progress(off, m->size);
    }
    return 1;
}

static void usb_src_release(void)
{
    if(g_xf_open){ exfat_unmount(); g_xf_open = 0; return; }
    f32_src_release();
}

static const upd_source_t SRC_USB = {
    UPD_SRC_USB, usb_src_probe, usb_src_manifest, usb_src_read, usb_src_release
};

/* ════════════════════════════════════════════════════════════════════
   3. The CD-ROM source
   ════════════════════════════════════════════════════════════════════

   The four steps that used to be inline in fs/update.c, behaving as they
   did.  The one addition: an ISO built by 0.8.4 or later carries
   /update.inf, so the CD path gets the same checksum the ESP path has.
   An ISO built before that does not, and a manifest is synthesised from
   /version.txt and the directory entry instead -- with have_crc left at
   zero, which is the honest record of what can be checked. */

static uint32_t g_cd_lba, g_cd_size;

static int cd_src_probe(void)
{
    if(!atapi_init())   return 0;
    if(!iso9660_init()) return 0;
    /* A disc with no kernel on it is not an update source, and finding
       that out here means the version screen is never drawn for one. */
    if(!iso9660_find("/boot/havendos.elf", &g_cd_lba, &g_cd_size) &&
       !iso9660_find("/BOOT/HAVENDOS.ELF", &g_cd_lba, &g_cd_size))
        return 0;
    /* A directory entry of length zero is a file that is there and is not
       a kernel.  Found by the suite: probe() returned 1 for it, the
       manifest step then refused it, and upd_source_select() had already
       committed to a source it could not serve.  A probe that says yes to
       something it cannot read is the shape of bug that costs an
       afternoon the first time there are three sources. */
    if(g_cd_size == 0) return 0;
    return 1;
}

static void cd_src_release(void) { }

static int cd_src_manifest(upd_manifest_t *m)
{
    uint8_t buf[1024];
    int r, i;

    /* An ISO from 0.8.4 onward carries a real manifest. */
    r = iso9660_read_file("/update.inf", buf, sizeof buf - 1);
    if(r < 1) r = iso9660_read_file("/UPDATE.INF", buf, sizeof buf - 1);
    if(r > 0){
        int code = upd_manifest_parse((const char *)buf, (uint32_t)r, m);
        /* The ISO's own directory entry is the authority on length: a
           manifest that disagrees with the filesystem it sits in is a
           manifest to refuse, not to believe. */
        if(code == UPDM_OK && m->size != g_cd_size) return UPDM_E_VALUE;
        if(code == UPDM_OK) return UPDM_OK;
        return code;
    }

    /* Older ISO: synthesise.  Everything here is read off the disc, so
       nothing is invented -- but there is no checksum to be had, and
       have_crc stays 0 to say so. */
    {
        uint32_t z;
        for(z = 0; z < UPD_VER_MAX;  z++) m->version[z] = 0;
        for(z = 0; z < UPD_NAME_MAX; z++) m->kernel[z]  = 0;
        m->have_version = 0; m->have_kernel = 0;
        m->have_size = 0;    m->have_crc = 0;
        m->size = 0;         m->crc = 0;
    }
    r = iso9660_read_file("/version.txt", buf, 127);
    if(r < 1) r = iso9660_read_file("/VERSION.TXT", buf, 127);
    if(r > 0){
        buf[r] = 0;
        for(i = 0; i < UPD_VER_MAX - 1 && buf[i] &&
                   buf[i] != '\n' && buf[i] != '\r'; i++)
            m->version[i] = (char)buf[i];
        m->version[i] = 0;
        if(i > 0) m->have_version = 1;
    }
    if(!m->have_version){
        /* No /version.txt.  Say so rather than inventing a number: the
           caller asks the user whether to apply an update of unknown
           version, exactly as it did before this file existed. */
        const char *u = "unknown";
        for(i = 0; u[i]; i++) m->version[i] = u[i];
        m->version[i] = 0;
        m->have_version = 1;
    }
    {
        const char *k = "havendos.elf";
        for(i = 0; k[i]; i++) m->kernel[i] = k[i];
        m->kernel[i] = 0;
        m->have_kernel = 1;
    }
    m->size = g_cd_size;
    m->have_size = (g_cd_size > 0);
    return m->have_size ? UPDM_OK : UPDM_E_FIELD;
}

static int cd_src_read(const upd_manifest_t *m, uint8_t *buf, uint32_t bufsz,
                       void (*progress)(uint32_t done, uint32_t total))
{
    uint32_t done = 0, left, lba;
    uint8_t  sec[2048];

    if(!m || !buf) return 0;
    if(m->size > bufsz) return 0;
    if(m->size != g_cd_size) return 0;

    left = m->size;
    lba  = g_cd_lba;
    while(left > 0){
        uint32_t chunk = left > 2048u ? 2048u : left;
        if(!atapi_read_sector(lba, sec)) return 0;
        memcpy(buf + done, sec, chunk);
        done += chunk; left -= chunk; lba++;
        if(progress) progress(done, m->size);
    }
    return 1;
}

static const upd_source_t SRC_CD = {
    UPD_SRC_CDROM, cd_src_probe, cd_src_manifest, cd_src_read, cd_src_release
};

/* ════════════════════════════════════════════════════════════════════
   4. Choosing, and naming
   ════════════════════════════════════════════════════════════════════ */

/* The ESP is probed first.  On a machine that has both -- a desktop with
   an optical drive and a UEFI install -- the ESP is the one somebody just
   copied a file onto from Windows, and the disc is likely to be whatever
   was left in the tray. */
/* ── THE ORDER, AND WHY A STICK COMES FIRST ──────────────────────────
   v0.8.6.3 put the USB source ahead of the ESP, which changes what an
   existing machine does, so the reason is written down.

   fs/update.c does not delete a staged update after applying it.  So an
   ESP that has been updated from once keeps that manifest forever, and
   with the ESP probed first it would be found every time -- `update`
   would answer "that version is already installed" and a stick plugged
   in two minutes ago would never be looked at.  The leftover would win
   against the thing somebody just did.

   A removable disk is present because a person put it there. That is
   the more recent intent, so it is asked first.

   It is not a silent change either way: fs/update.c prints
   upd_source_label() before it touches anything, so the screen names
   the source it chose. */
const upd_source_t *upd_source_select(void)
{
    if(SRC_USB.probe()) return &SRC_USB;
    if(SRC_ESP.probe()) return &SRC_ESP;
    if(SRC_CD.probe())  return &SRC_CD;
    return 0;
}

/* Every screen asks this.  Four screens working a label out for
   themselves is exactly the bug 0.8.3.1 shipped, where the disk picker
   said NVMe and the screen gated behind typing YES said VirtIO. */
const char *upd_source_label(const upd_source_t *s)
{
    if(!s) return "nowhere";
    switch(s->kind){
    case UPD_SRC_USB:   return "USB stick";
    case UPD_SRC_ESP:   return "EFI System Partition";
    case UPD_SRC_CDROM: return "CD-ROM";
    default:            return "unknown source";
    }
}

/* The checksum, in one place, so the two sources cannot drift in how
   carefully they are checked.  gpt_crc32() is the CRC-32 the GPT header
   already uses: reflected 0xEDB88320, which is what python3's
   zlib.crc32 computes, so a manifest can be checked by somebody with no
   HavenDOS source at all. */
uint32_t upd_image_crc(const uint8_t *buf, uint32_t len)
{
    return gpt_crc32(buf, len);
}
