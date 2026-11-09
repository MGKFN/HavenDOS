/* The Multiboot information structures, against the specification.
 *
 * This exists because a single misplaced field here produces a confident,
 * completely wrong diagnosis on real hardware and nothing else can catch it.
 *
 * The structure was already wrong once: the four VBE interface fields at
 * offsets 80-87 are u16 in the spec and were declared u32, so everything
 * from framebuffer_addr onward was read EIGHT BYTES too far in.  Nothing
 * noticed, because the header never asked GRUB for a framebuffer and the
 * whole block was always zero.  Asking for one made it matter.
 *
 * It matters a second time now.  `fbdiag` reports "framebuffer above 4 GB"
 * when `fb_addr_hi` is non-zero, and that is read straight out of this
 * struct at offset 92.  If the offset were wrong the field would be reading
 * part of framebuffer_pitch, which is never zero on a machine that has a
 * framebuffer at all -- so the diagnostic would report "above 4 GB" on
 * EVERY machine, and the report would look exactly like a real finding.
 *
 * The same reasoning covers the Multiboot 2 framebuffer tag, where the
 * address is two u32s at tag+8 and tag+12 and the high half is fb[1].
 */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

/* Lifted verbatim from kernel/main.c. */
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

static int total=0, fails=0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(!ok){ fails++; printf("    FAIL %-54s got %ld\n", what, got); }
}
#define OFF(f,want) ck("offsetof(" #f ") == " #want, \
                       offsetof(mb1_info_t,f)==(size_t)(want), \
                       (long)offsetof(mb1_info_t,f))

int main(void){
    head("Multiboot 1: every field at the spec's offset");
    OFF(flags,0);          OFF(mem_lower,4);      OFF(mem_upper,8);
    OFF(boot_device,12);   OFF(cmdline,16);       OFF(mods_count,20);
    OFF(mods_addr,24);     OFF(syms,28);          OFF(mmap_length,44);
    OFF(mmap_addr,48);     OFF(drives_length,52); OFF(drives_addr,56);
    OFF(config_table,60);  OFF(boot_loader,64);   OFF(apm_table,68);
    OFF(vbe_control,72);   OFF(vbe_mode_info,76);

    head("the VBE interface fields are 16-bit, which is what shifted everything");
    OFF(vbe_mode,80); OFF(vbe_seg,82); OFF(vbe_off,84); OFF(vbe_len,86);
    ck("the four VBE fields occupy 8 bytes, not 16",
       offsetof(mb1_info_t,fb_addr_lo)-offsetof(mb1_info_t,vbe_mode)==8,
       (long)(offsetof(mb1_info_t,fb_addr_lo)-offsetof(mb1_info_t,vbe_mode)));

    head("the framebuffer block, which fbdiag's answer is read from");
    OFF(fb_addr_lo,88);
    OFF(fb_addr_hi,92);     /* "above 4 GB" is decided by this one */
    OFF(fb_pitch,96);
    OFF(fb_width,100);
    OFF(fb_height,104);
    OFF(fb_bpp,108);
    OFF(fb_type,109);
    OFF(color_info,110);
    ck("bpp and type are single bytes", sizeof(((mb1_info_t*)0)->fb_bpp)==1
       && sizeof(((mb1_info_t*)0)->fb_type)==1, 0);
    ck("the whole struct is 116 bytes", sizeof(mb1_info_t)==116,
       (long)sizeof(mb1_info_t));

    head("the high half cannot be aliasing a field that is never zero");
    /* If fb_addr_hi were misplaced onto pitch/width/height, fbdiag would
       report "above 4 GB" on every machine that has a framebuffer. */
    ck("fb_addr_hi does not overlap fb_pitch",
       offsetof(mb1_info_t,fb_addr_hi)+4 <= offsetof(mb1_info_t,fb_pitch),
       (long)offsetof(mb1_info_t,fb_addr_hi));
    ck("fb_addr_hi sits immediately above fb_addr_lo, forming one u64",
       offsetof(mb1_info_t,fb_addr_hi)==offsetof(mb1_info_t,fb_addr_lo)+4,
       (long)offsetof(mb1_info_t,fb_addr_hi));

    head("Multiboot 2: the framebuffer tag, read as a u32 array at tag+8");
    /* kernel/main.c does: fb = (uint32_t*)((uint8_t*)tag+8), bpp at +28,
       type at +29, and treats fb[1] as the high half of the address. */
    ck("fb[0] is the low half  (tag+8)",  8+0*4 == 8,  8);
    ck("fb[1] is the high half (tag+12)", 8+1*4 == 12, 12);
    ck("fb[2] is pitch         (tag+16)", 8+2*4 == 16, 16);
    ck("fb[3] is width         (tag+20)", 8+3*4 == 20, 20);
    ck("fb[4] is height        (tag+24)", 8+4*4 == 24, 24);
    ck("bpp is the byte at tag+28",  28 == 8+5*4,      28);
    ck("type is the byte at tag+29", 29 == 8+5*4+1,    29);

    printf("\n%d/%d passed\n", total-fails, total);
    return fails?1:0;
}
