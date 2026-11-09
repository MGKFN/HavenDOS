/* ================================================================
   fs/gpt.c -- reading a GUID Partition Table
   ================================================================
   See include/gpt.h: this is here so the installer can recognise a
   disk it must not touch.
   ================================================================ */

#include "types.h"
#include "gpt.h"

/* include/string.h rather than local prototypes with uint32_t lengths:
   those matched size_t in the 32-bit kernel and not in a native test, so
   this file could not share a translation unit with any that includes
   string.h. tests/test_installadd.c needs it to share one with fs/fat32.c
   and fs/install.c. Same change, same reason, in fs/gpt_write.c. */
#include "string.h"

/* The standard CRC-32 (reflected, polynomial 0xEDB88320), which is what the
   UEFI specification uses for both the header and the entry array.  Computed
   rather than table-driven: this runs a handful of times at boot and a 1 KB
   table is 1 KB the kernel does not have to carry. */
uint32_t gpt_crc32(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;
    for(uint32_t i = 0; i < len; i++){
        crc ^= p[i];
        for(int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

static const uint8_t GUID_ESP[16] = {
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11,
    0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B };          /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
static const uint8_t GUID_MSFT_DATA[16] = {
    0xA2,0xA0,0xD0,0xEB, 0xE5,0xB9, 0x33,0x44,
    0x87,0xC0, 0x68,0xB6,0xB7,0x26,0x99,0xC7 };          /* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 */
static const uint8_t GUID_MSFT_RESV[16] = {
    0x16,0xE3,0xC9,0xE3, 0x5C,0x0B, 0xB8,0x4D,
    0x81,0x7D, 0xF9,0x2D,0xF0,0x02,0x15,0xAE };          /* E3C9E316-0B5C-4DB8-817D-F92DF00215AE */

static int guid_eq(const uint8_t *a, const uint8_t *b){
    for(int i = 0; i < 16; i++) if(a[i] != b[i]) return 0;
    return 1;
}
static int guid_zero(const uint8_t *a){
    for(int i = 0; i < 16; i++) if(a[i]) return 0;
    return 1;
}

int gpt_present(int (*rd)(void*,uint64_t,void*), void *ctx)
{
    static uint8_t sec[512];
    if(!rd) return 0;
    if(rd(ctx, 1, sec) != 0) return 0;
    /* "EFI PART" */
    return sec[0]=='E' && sec[1]=='F' && sec[2]=='I' && sec[3]==' '
        && sec[4]=='P' && sec[5]=='A' && sec[6]=='R' && sec[7]=='T';
}

int gpt_scan(int (*rd)(void*,uint64_t,void*), void *ctx,
             gpt_part_t *out, int max)
{
    static uint8_t sec[512];
    if(!rd || !out || max <= 0) return -1;
    if(rd(ctx, 1, sec) != 0) return -1;
    if(!(sec[0]=='E'&&sec[1]=='F'&&sec[2]=='I'&&sec[3]==' '
       &&sec[4]=='P'&&sec[5]=='A'&&sec[6]=='R'&&sec[7]=='T')) return 0;

    uint32_t hdr_size = 0, hdr_crc = 0;
    for(int i=0;i<4;i++) hdr_size |= (uint32_t)sec[12+i] << (8*i);
    for(int i=0;i<4;i++) hdr_crc  |= (uint32_t)sec[16+i] << (8*i);
    if(hdr_size < 92 || hdr_size > 512) return 0;

    /* The header's own CRC is computed with the CRC field zeroed.  Checking
       it matters: a disk whose GPT does not verify is a disk we understand
       less well than we think, and the right response to that is to stay
       off it, not to parse it harder. */
    {
        static uint8_t tmp[512];
        memcpy(tmp, sec, 512);
        tmp[16]=tmp[17]=tmp[18]=tmp[19]=0;
        if(gpt_crc32(tmp, hdr_size) != hdr_crc) return 0;
    }

    uint64_t ent_lba = 0;
    uint32_t n_ent = 0, ent_size = 0;
    for(int i=0;i<8;i++) ent_lba  |= (uint64_t)sec[72+i] << (8*i);
    for(int i=0;i<4;i++) n_ent    |= (uint32_t)sec[80+i] << (8*i);
    for(int i=0;i<4;i++) ent_size |= (uint32_t)sec[84+i] << (8*i);
    if(ent_size < 128 || ent_size > 512) return 0;
    if(n_ent > 256) n_ent = 256;

    int found = 0;
    uint32_t per_sec = 512u / ent_size;
    if(!per_sec) return 0;

    for(uint32_t i = 0; i < n_ent && found < max; i++){
        if(i % per_sec == 0){
            if(rd(ctx, ent_lba + i/per_sec, sec) != 0) break;
        }
        const uint8_t *e = sec + (i % per_sec) * ent_size;
        if(guid_zero(e)) continue;                   /* unused entry */

        gpt_part_t *p = &out[found];
        memset(p, 0, sizeof *p);
        memcpy(p->type_guid, e, 16);
        for(int k=0;k<8;k++) p->first_lba |= (uint64_t)e[32+k] << (8*k);
        for(int k=0;k<8;k++) p->last_lba  |= (uint64_t)e[40+k] << (8*k);
        if(p->last_lba < p->first_lba) continue;

        /* The name is UTF-16LE.  Folded to ASCII rather than rendered:
           everything that displays it goes through the VGA console, whose
           safe character set is letters, numbers and a few punctuation
           marks (see the house rules). */
        for(int k=0;k<36;k++){
            uint16_t u = (uint16_t)(e[56+k*2] | (e[57+k*2] << 8));
            p->name[k] = (u >= 32 && u < 127) ? (char)u : (u ? '.' : 0);
            if(!u) break;
        }
        p->name[36] = 0;
        p->is_esp  = guid_eq(p->type_guid, GUID_ESP);
        p->is_msft = guid_eq(p->type_guid, GUID_MSFT_DATA)
                  || guid_eq(p->type_guid, GUID_MSFT_RESV);
        found++;
    }
    return found;
}
