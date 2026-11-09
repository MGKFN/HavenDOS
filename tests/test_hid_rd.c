/* ================================================================
   tests/test_hid_rd.c -- HID report descriptors, judged by hid_parser
   ================================================================
   usb/hid_rd.c decides which bits of a device's report are X. Every
   trackpad and every non-boot-protocol keyboard depends on it being
   right, and the failure mode is not a crash: it is a pointer that
   moves diagonally, or only right, or in 256-pixel jumps.

   ── WHY THE JUDGE IS NOT ME ─────────────────────────────────────────

   The obvious way to test this is to work the bit offsets out by hand
   from the HID specification and type them in. That is the same
   person making the same mistake twice, and the mistake is invisible:
   a descriptor parsed wrongly still produces plausible offsets.

   `hid_parser` is an independent Python implementation of the same
   grammar. tests/hid_rd_judge.py hands it the same bytes and compares
   every field -- usage page, usage, bit offset, bit width -- against
   ours. It also reports how many descriptors and fields it looked at,
   which this suite asserts, because a judge that silently examined
   nothing would pass forever.

   The descriptors below are real shapes: boot mouse, boot keyboard,
   a wheel mouse, a five-button 16-bit mouse, a Windows Precision
   Touchpad digitizer, and the two traps -- a Feature report between
   two Inputs, and Push/Pop.
   ================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "types.h"
#include "hid_rd.h"
#include "../usb/hid_rd.c"

static int total = 0, fails = 0;
static void head(const char *s){ printf("\n  %s\n", s); }
static void ck(const char *what, int ok, long got){
    total++;
    if(ok) printf("    ok    %s\n", what);
    else { printf("    FAIL  %s   got %ld\n", what, got); fails++; }
}

/* ── the descriptors ───────────────────────────────────────────── */

/* The HID specification's own boot mouse, Appendix E.10. */
static const uint8_t D_MOUSE[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01,
      0x09,0x01, 0xA1,0x00,
        0x05,0x09, 0x19,0x01, 0x29,0x03,
        0x15,0x00, 0x25,0x01, 0x95,0x03, 0x75,0x01, 0x81,0x02,
        0x95,0x01, 0x75,0x05, 0x81,0x03,            /* padding */
        0x05,0x01, 0x09,0x30, 0x09,0x31,
        0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x02, 0x81,0x06,
      0xC0,
    0xC0
};

/* The boot keyboard, Appendix E.6: modifiers, a reserved byte, an LED
   OUTPUT report -- which must not move the input offsets -- and a
   six-slot keycode ARRAY. */
static const uint8_t D_KBD[] = {
    0x05,0x01, 0x09,0x06, 0xA1,0x01,
      0x05,0x07, 0x19,0xE0, 0x29,0xE7,
      0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02,
      0x95,0x01, 0x75,0x08, 0x81,0x03,              /* reserved */
      0x95,0x05, 0x75,0x01, 0x05,0x08, 0x19,0x01, 0x29,0x05, 0x91,0x02,
      0x95,0x01, 0x75,0x03, 0x91,0x03,              /* LED padding */
      0x95,0x06, 0x75,0x08, 0x15,0x00, 0x25,0x65,
      0x05,0x07, 0x19,0x00, 0x29,0x65, 0x81,0x00,   /* array */
    0xC0
};

/* A wheel mouse: three buttons, X, Y, Wheel. */
static const uint8_t D_WHEEL[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01,
      0x09,0x01, 0xA1,0x00,
        0x05,0x09, 0x19,0x01, 0x29,0x03,
        0x15,0x00, 0x25,0x01, 0x95,0x03, 0x75,0x01, 0x81,0x02,
        0x95,0x01, 0x75,0x05, 0x81,0x01,
        0x05,0x01, 0x09,0x30, 0x09,0x31,
        0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x02, 0x81,0x06,
        0x09,0x38, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x01, 0x81,0x06,
      0xC0,
    0xC0
};

/* Five buttons and SIXTEEN-BIT axes -- the high-resolution mouse,
   where an eight-bit assumption shows up as a pointer that moves in
   jumps of 256. */
static const uint8_t D_HIRES[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01,
      0x09,0x01, 0xA1,0x00,
        0x05,0x09, 0x19,0x01, 0x29,0x05,
        0x15,0x00, 0x25,0x01, 0x95,0x05, 0x75,0x01, 0x81,0x02,
        0x95,0x01, 0x75,0x03, 0x81,0x03,
        0x05,0x01, 0x09,0x30, 0x09,0x31,
        0x16,0x00,0x80, 0x26,0xFF,0x7F,
        0x75,0x10, 0x95,0x02, 0x81,0x06,
      0xC0,
    0xC0
};

/* Report IDs: two input reports in one descriptor. Each id has its
   OWN bit offsets starting at zero, and a parser with one counter
   puts the second report's fields after the first report's. */
static const uint8_t D_IDS[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01,
      0x85,0x01,
      0x05,0x09, 0x19,0x01, 0x29,0x02,
      0x15,0x00, 0x25,0x01, 0x95,0x02, 0x75,0x01, 0x81,0x02,
      0x95,0x01, 0x75,0x06, 0x81,0x03,
      0x85,0x02,
      0x05,0x01, 0x09,0x30, 0x09,0x31,
      0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x02, 0x81,0x06,
    0xC0
};

/* A FEATURE report between two INPUT items. HID counts the three
   report types separately; a parser that uses one running offset puts
   X and Y eight bits too far along. */
static const uint8_t D_FEAT[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01,
      0x05,0x09, 0x19,0x01, 0x29,0x08,
      0x15,0x00, 0x25,0x01, 0x95,0x08, 0x75,0x01, 0x81,0x02,
      0x06,0x00,0xFF, 0x09,0xC5, 0x95,0x01, 0x75,0x08, 0xB1,0x02,
      0x05,0x01, 0x09,0x30, 0x09,0x31,
      0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x02, 0x81,0x06,
    0xC0
};

/* PUSH and POP around a usage page change. */
static const uint8_t D_PUSH[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01,
      0x05,0x09, 0x19,0x01, 0x29,0x08,
      0x15,0x00, 0x25,0x01, 0x95,0x08, 0x75,0x01, 0x81,0x02,
      0xA4,                                   /* Push              */
      0x05,0x01, 0x09,0x30, 0x15,0x81, 0x25,0x7F,
      0x75,0x08, 0x95,0x01, 0x81,0x06,
      0xB4,                                   /* Pop               */
      0x09,0x31, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x01, 0x81,0x06,
    0xC0
};

/* A Windows Precision Touchpad, cut down to one contact plus the
   contact count and a button -- the shape a real trackpad reports,
   with ABSOLUTE 16-bit X and Y whose logical minimum is zero. Sign
   extension here would put the pointer in the wrong half of the
   screen for every coordinate past the midpoint. */
static const uint8_t D_TOUCH[] = {
    0x05,0x0D, 0x09,0x05, 0xA1,0x01,
      0x85,0x04,
      0x09,0x22, 0xA1,0x02,
        0x09,0x47, 0x09,0x42, 0x15,0x00, 0x25,0x01,
        0x75,0x01, 0x95,0x02, 0x81,0x02,
        0x95,0x01, 0x75,0x02, 0x81,0x03,
        0x95,0x01, 0x75,0x04, 0x09,0x51, 0x25,0x0F, 0x81,0x02,
        0x05,0x01, 0x15,0x00, 0x26,0xFF,0x0F,
        0x75,0x10, 0x95,0x01, 0x09,0x30, 0x81,0x02,
        0x09,0x31, 0x81,0x02,
      0xC0,
      0x05,0x0D, 0x09,0x54, 0x15,0x00, 0x25,0x7F,
      0x75,0x08, 0x95,0x01, 0x81,0x02,
      0x05,0x09, 0x09,0x01, 0x15,0x00, 0x25,0x01,
      0x75,0x01, 0x95,0x01, 0x81,0x02,
      0x95,0x07, 0x75,0x01, 0x81,0x03,
    0xC0
};

/* A four-byte Usage, which carries the page in its high half. */
static const uint8_t D_USAGE32[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01,
      0x0B,0x30,0x00,0x01,0x00,              /* Usage 0x00010030 = X */
      0x0B,0x31,0x00,0x01,0x00,              /* Usage 0x00010031 = Y */
      0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x02, 0x81,0x06,
    0xC0
};

/* ── THE TWO DESCRIPTORS THE JUDGE IS NOT THE AUTHORITY ON ───────────
   Both are marked "skip:" in the case file rather than dropped
   silently, and both are recorded here because "the external judge
   disagreed and I decided I was right" is a claim that has to be
   checkable.

   push_pop: hid_parser raises NotImplementedError on the Push and Pop
     global tags -- it does not implement them at all. There is
     nothing to compare against. The local checks below do the work
     instead, and they test the thing that matters: that the usage
     page in force after a Pop is the one that was in force before the
     Push.

   usage32: hid_parser and this parser disagree, and the HID
     specification settles it. For a four-byte Usage item hid_parser
     reports page 1 and usage 0x10030 -- the full 32-bit value, page
     bits included. HID 1.11 section 6.2.2.8: "If the usage item is 4
     bytes, the high order 16 bits is the usage page and the low order
     16 bits is the usage ID." So the usage is 0x30 and ours is right.
     A reader that kept 0x10030 would never match usage 0x30 and would
     find no X on any device that uses the extended form. */
/* A four-byte Usage whose page DIFFERS from the Usage Page in force.
   The previous descriptor could not tell the two readings apart: its
   Usage Page was already 0x01, so truncating the 32-bit usage to its
   low half gave 0x30 either way and an injection that kept the page
   bits survived the suite. Here the page in force is Button, so a
   reader that ignores the extended form puts X on the button page and
   finds no X at all. */
static const uint8_t D_USAGE32_PAGE[] = {
    0x05,0x09,                               /* Usage Page = Button  */
    0x0B,0x30,0x00,0x01,0x00,                /* Usage 0x00010030 = X */
    0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x01, 0x81,0x06
};

/* ONE usage, report count THREE. HID says the last usage declared
   applies to every remaining control, which is what Linux's
   hid-core does (hid_parse_main_item takes usage[min(j, n-1)]). */
static const uint8_t D_REPEAT[] = {
    0x05,0x09, 0x09,0x01,
    0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x03, 0x81,0x02,
    0x95,0x01, 0x75,0x05, 0x81,0x03
};

/* TWO X fields: a touchpad's own collection first, then a
   mouse-compatibility one. Real Precision Touchpads do this, and the
   first is the one the device wants used. */
static const uint8_t D_TWOX[] = {
    0x05,0x0D, 0x09,0x05, 0xA1,0x01,
      0x05,0x01, 0x09,0x30, 0x15,0x00, 0x26,0xFF,0x0F,
      0x75,0x10, 0x95,0x01, 0x81,0x02,
      0x09,0x31, 0x81,0x02,
    0xC0,
    0x05,0x01, 0x09,0x02, 0xA1,0x01,
      0x09,0x30, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x01, 0x81,0x06,
      0x09,0x31, 0x81,0x06,
    0xC0
};

typedef struct { const char *name; const uint8_t *d; uint32_t n; } desc_t;
static const desc_t DESCS[] = {
    { "boot_mouse",  D_MOUSE,   sizeof D_MOUSE   },
    { "boot_kbd",    D_KBD,     sizeof D_KBD     },
    { "wheel_mouse", D_WHEEL,   sizeof D_WHEEL   },
    { "hires_mouse", D_HIRES,   sizeof D_HIRES   },
    { "report_ids",  D_IDS,     sizeof D_IDS     },
    { "feature_mid", D_FEAT,    sizeof D_FEAT    },
    { "skip:push-pop-unsupported:push_pop", D_PUSH, sizeof D_PUSH },
    { "touchpad",    D_TOUCH,   sizeof D_TOUCH   },
    { "skip:hid-spec-6.2.2.8:usage32", D_USAGE32, sizeof D_USAGE32 },
    { "skip:hid-spec-6.2.2.8:usage32_page",
      D_USAGE32_PAGE, sizeof D_USAGE32_PAGE },
    { "skip:last-usage-repeats:repeat", D_REPEAT, sizeof D_REPEAT },
    { "two_x",       D_TWOX,    sizeof D_TWOX    },
};
#define NDESCS ((int)(sizeof DESCS / sizeof DESCS[0]))

static hrd_t g_h;

int main(void)
{
    int i;

    printf("\n=== test_hid_rd: report descriptors, judged by hid_parser ===\n");

    if(system("python3 -c 'import hid_parser' >/dev/null 2>&1") != 0){
        printf("\n  hid_parser is not installed and it is the judge for\n"
               "  every offset here. SKIP, not a pass.\n"
               "  (pip install --break-system-packages hid-parser)\n\nSKIP\n");
        return 77;
    }

    /* ── all nine descriptors, against an independent parser ─────── */
    head("every field of every descriptor, against hid_parser");
    {
        FILE *f = fopen("/tmp/hd_hrd_cases.txt", "wb");
        int parsed = 0;
        ck("the case file opened", f != 0, 0);
        for(i = 0; f && i < NDESCS; i++){
            int r = hrd_parse(DESCS[i].d, DESCS[i].n, &g_h);
            char what[128];
            int k;
            snprintf(what, sizeof what, "%s parses", DESCS[i].name);
            ck(what, r == HRD_OK, r);
            if(r != HRD_OK) continue;
            parsed++;
            fprintf(f, "%s ", DESCS[i].name);
            for(k = 0; k < (int)DESCS[i].n; k++)
                fprintf(f, "%02x", DESCS[i].d[k]);
            fputc(' ', f);
            for(k = 0; k < g_h.nfields; k++)
                fprintf(f, "%x:%x:%u:%u%s;",
                        (unsigned)g_h.field[k].usage_page,
                        (unsigned)g_h.field[k].usage,
                        (unsigned)g_h.field[k].bit_offset,
                        (unsigned)g_h.field[k].bit_size,
                        g_h.field[k].array ? "a" : "");
            fputc('\n', f);
        }
        if(f) fclose(f);
        ck("all of them parsed", parsed == NDESCS, parsed);
        {
            int rc = system("python3 -I tests/hid_rd_judge.py "
                            "/tmp/hd_hrd_cases.txt >/tmp/hd_hrd.out "
                            "2>/tmp/hd_hrd.err");
            int nd = 0, nf = 0;
            FILE *o = fopen("/tmp/hd_hrd.out", "rb");
            if(o){ if(fscanf(o, "%d %d", &nd, &nf) != 2) nd = -1; fclose(o); }
            ck("hid_parser agrees with every field",
               WIFEXITED(rc) && WEXITSTATUS(rc) == 0,
               WIFEXITED(rc) ? WEXITSTATUS(rc) : -1);
            /* Seven, not nine: push_pop and usage32 are marked skip
               for the reasons recorded above the DESCS table. */
            /* Eight of twelve. Four are marked skip, for the three
               reasons recorded above the DESCS table. */
            ck("...and it examined the eight it can judge", nd == 8, nd);
            /* A judge that found no fields would also "agree". */
            ck("...and found a substantial number of fields", nf > 30, nf);
            if(!(WIFEXITED(rc) && WEXITSTATUS(rc) == 0))
                system("sed 's/^/          | /' /tmp/hd_hrd.err | head -14");
        }
    }

    /* ── the resolution, which the judge does not check ──────────── */
    head("the boot mouse resolves to a pointer");
    hrd_parse(D_MOUSE, sizeof D_MOUSE, &g_h);
    ck("X is found",  g_h.i_x >= 0, g_h.i_x);
    ck("Y is found",  g_h.i_y >= 0, g_h.i_y);
    ck("no wheel",    g_h.i_wheel < 0, g_h.i_wheel);
    ck("three buttons", g_h.nbuttons == 3, g_h.nbuttons);
    ck("X is at bit 8",  g_h.field[g_h.i_x].bit_offset == 8,
       (long)g_h.field[g_h.i_x].bit_offset);
    ck("Y is at bit 16", g_h.field[g_h.i_y].bit_offset == 16,
       (long)g_h.field[g_h.i_y].bit_offset);
    ck("X is eight bits", g_h.field[g_h.i_x].bit_size == 8,
       g_h.field[g_h.i_x].bit_size);
    ck("X is RELATIVE",   g_h.field[g_h.i_x].relative == 1, 0);
    ck("buttons are not", g_h.field[g_h.i_button[0]].relative == 0, 0);
    ck("no report ids",   !g_h.has_report_ids, 0);
    ck("the report is three bytes", g_h.input_bits == 24,
       (long)g_h.input_bits);

    head("the wheel mouse has a wheel");
    hrd_parse(D_WHEEL, sizeof D_WHEEL, &g_h);
    ck("the wheel is found", g_h.i_wheel >= 0, g_h.i_wheel);
    ck("...at bit 24", g_h.field[g_h.i_wheel].bit_offset == 24,
       (long)g_h.field[g_h.i_wheel].bit_offset);
    ck("the report is four bytes", g_h.input_bits == 32, (long)g_h.input_bits);

    head("the high-resolution mouse has 16-bit axes");
    hrd_parse(D_HIRES, sizeof D_HIRES, &g_h);
    ck("X is sixteen bits", g_h.field[g_h.i_x].bit_size == 16,
       g_h.field[g_h.i_x].bit_size);
    ck("five buttons", g_h.nbuttons == 5, g_h.nbuttons);
    ck("X starts at bit 8", g_h.field[g_h.i_x].bit_offset == 8,
       (long)g_h.field[g_h.i_x].bit_offset);
    ck("Y starts at bit 24", g_h.field[g_h.i_y].bit_offset == 24,
       (long)g_h.field[g_h.i_y].bit_offset);
    /* Its logical minimum is -32768, so the field is signed. */
    ck("X can be negative", g_h.field[g_h.i_x].logical_min == -32768,
       (long)g_h.field[g_h.i_x].logical_min);

    head("report ids get their own offsets, each starting at zero");
    hrd_parse(D_IDS, sizeof D_IDS, &g_h);
    ck("ids are declared", g_h.has_report_ids, 0);
    ck("X is in report 2", g_h.field[g_h.i_x].report_id == 2,
       g_h.field[g_h.i_x].report_id);
    /* THE CHECK THIS DESCRIPTOR EXISTS FOR. With one shared counter X
       would land at bit 8 -- after report 1's eight bits. */
    ck("X is at bit 0 of ITS report, not bit 8",
       g_h.field[g_h.i_x].bit_offset == 0,
       (long)g_h.field[g_h.i_x].bit_offset);
    ck("the buttons are in report 1",
       g_h.field[g_h.i_button[0]].report_id == 1,
       g_h.field[g_h.i_button[0]].report_id);

    head("a Feature report between two Inputs moves nothing");
    hrd_parse(D_FEAT, sizeof D_FEAT, &g_h);
    /* Eight buttons, then a Feature byte, then X. X belongs at bit 8:
       a single running offset would put it at 16. */
    ck("X is at bit 8, not bit 16", g_h.field[g_h.i_x].bit_offset == 8,
       (long)g_h.field[g_h.i_x].bit_offset);
    ck("the input report is three bytes", g_h.input_bits == 24,
       (long)g_h.input_bits);

    head("push and pop restore the usage page");
    hrd_parse(D_PUSH, sizeof D_PUSH, &g_h);
    ck("X survived the push", g_h.i_x >= 0, g_h.i_x);
    /* The descriptor sets the BUTTON page, pushes, switches to the
       desktop page for X, then pops. So the usage after the pop --
       0x31 -- is Button 49 and NOT Y, and the first version of this
       check expected Y and was wrong about the descriptor rather than
       about the parser. That the page came back is the whole point of
       Pop, and the last field proves it. */
    ck("the usage page came back to Button after the pop",
       g_h.nfields > 0
       && g_h.field[g_h.nfields-1].usage_page == HRD_PAGE_BUTTON,
       g_h.nfields ? g_h.field[g_h.nfields-1].usage_page : -1);
    ck("...and that field is at bit 16",
       g_h.nfields > 0 && g_h.field[g_h.nfields-1].bit_offset == 16,
       g_h.nfields ? (long)g_h.field[g_h.nfields-1].bit_offset : -1);

    head("the touchpad");
    hrd_parse(D_TOUCH, sizeof D_TOUCH, &g_h);
    ck("it parses", g_h.nfields > 4, g_h.nfields);
    ck("X is found", g_h.i_x >= 0, g_h.i_x);
    ck("the contact count is found", g_h.i_contacts >= 0, g_h.i_contacts);
    ck("the tip switch is found", g_h.i_tip >= 0, g_h.i_tip);
    ck("the button is found", g_h.i_button[0] >= 0, g_h.i_button[0]);
    ck("X is ABSOLUTE, not relative",
       g_h.i_x >= 0 && g_h.field[g_h.i_x].relative == 0, 0);
    /* An absolute coordinate with a logical minimum of zero must not
       be sign-extended: 0x0F00 is 3840, not a negative number. */
    ck("X's logical minimum is zero",
       g_h.i_x >= 0 && g_h.field[g_h.i_x].logical_min == 0, 0);
    ck("it is in report 4",
       g_h.i_x >= 0 && g_h.field[g_h.i_x].report_id == 4, 0);

    head("a four-byte Usage carries its own page");
    hrd_parse(D_USAGE32, sizeof D_USAGE32, &g_h);
    ck("X is found on the desktop page", g_h.i_x >= 0, g_h.i_x);
    ck("Y too", g_h.i_y >= 0, g_h.i_y);
    /* The one that can tell the two readings apart: the Usage Page in
       force here is Button, so a reader that ignores the extended
       form never finds X. */
    hrd_parse(D_USAGE32_PAGE, sizeof D_USAGE32_PAGE, &g_h);
    ck("...even when the page in force is a different one",
       g_h.i_x >= 0, g_h.i_x);
    ck("...and the field is on page 1, not page 9",
       g_h.i_x >= 0 && g_h.field[g_h.i_x].usage_page == HRD_PAGE_DESKTOP,
       g_h.i_x >= 0 ? g_h.field[g_h.i_x].usage_page : -1);

    head("the last usage declared applies to the remaining controls");
    hrd_parse(D_REPEAT, sizeof D_REPEAT, &g_h);
    ck("three fields from one usage and a count of three",
       g_h.nfields == 3, g_h.nfields);
    ck("all three are Button 1",
       g_h.nfields == 3
       && g_h.field[0].usage == 1 && g_h.field[1].usage == 1
       && g_h.field[2].usage == 1, 0);
    ck("...at bits 0, 1 and 2",
       g_h.nfields == 3 && g_h.field[0].bit_offset == 0
       && g_h.field[1].bit_offset == 1 && g_h.field[2].bit_offset == 2, 0);

    head("the FIRST X wins when a device declares two");
    hrd_parse(D_TWOX, sizeof D_TWOX, &g_h);
    ck("two X fields are present",
       g_h.nfields >= 4, g_h.nfields);
    /* The digitizer collection comes first and its X is 16 bits
       absolute; the mouse shim's is 8 bits relative. Picking the last
       would hand a driver the compatibility one. */
    ck("i_x is the digitizer's, not the mouse shim's",
       g_h.i_x >= 0 && g_h.field[g_h.i_x].bit_size == 16,
       g_h.i_x >= 0 ? g_h.field[g_h.i_x].bit_size : -1);
    ck("...and it is absolute",
       g_h.i_x >= 0 && g_h.field[g_h.i_x].relative == 0, 0);
    ck("i_y likewise", g_h.i_y >= 0 && g_h.field[g_h.i_y].bit_size == 16,
       g_h.i_y >= 0 ? g_h.field[g_h.i_y].bit_size : -1);

    /* ── reading values out of a report ──────────────────────────── */
    head("pulling values out of an actual report");
    hrd_parse(D_MOUSE, sizeof D_MOUSE, &g_h);
    {
        /* buttons=0b101, X=-1, Y=+5 */
        uint8_t rep[3] = { 0x05, 0xFF, 0x05 };
        int32_t v = 0;
        ck("button 1 is down",
           hrd_field_value(&g_h.field[g_h.i_button[0]], rep, 3, &v) == HRD_OK
           && v == 1, v);
        ck("button 2 is up",
           hrd_field_value(&g_h.field[g_h.i_button[1]], rep, 3, &v) == HRD_OK
           && v == 0, v);
        ck("button 3 is down",
           hrd_field_value(&g_h.field[g_h.i_button[2]], rep, 3, &v) == HRD_OK
           && v == 1, v);
        /* THE one that decides whether a mouse can move left. */
        ck("X reads as -1, not 255",
           hrd_field_value(&g_h.field[g_h.i_x], rep, 3, &v) == HRD_OK
           && v == -1, v);
        ck("Y reads as +5",
           hrd_field_value(&g_h.field[g_h.i_y], rep, 3, &v) == HRD_OK
           && v == 5, v);
        ck("a two-byte report is refused for a field in byte three",
           hrd_field_value(&g_h.field[g_h.i_y], rep, 2, &v) == HRD_E_RANGE, 0);
    }

    head("an absolute 16-bit coordinate is not sign-extended");
    hrd_parse(D_TOUCH, sizeof D_TOUCH, &g_h);
    {
        /* Report 4, one contact: confidence+tip set in byte 0, then
           X at byte 1 and Y at byte 3, little-endian.
           X = 0x0F00 = 3840, Y = 0x0800 = 2048.
           The first version of these bytes had X and Y a byte early
           and read 0 and 15 -- the test data was wrong, the parser
           was not, which is what the bit offsets above already said. */
        uint8_t rep[7] = { 0x03, 0x00,0x0F, 0x00,0x08, 0x01, 0x00 };
        int32_t v = 0;
        int r = hrd_field_value(&g_h.field[g_h.i_x], rep, sizeof rep, &v);
        ck("X reads 3840", r == HRD_OK && v == 3840, v);
        r = hrd_field_value(&g_h.field[g_h.i_y], rep, sizeof rep, &v);
        ck("Y reads 2048", r == HRD_OK && v == 2048, v);
    }

    /* ── descriptors this parser refuses ─────────────────────────── */
    head("descriptors a parser must refuse");
    {
        static const uint8_t trunc[] = { 0x05,0x01, 0x09 };       /* cut */
        static const uint8_t pop_only[] = { 0xB4, 0x05,0x01, 0x09,0x30,
                                            0x75,0x08, 0x95,0x01, 0x81,0x02 };
        static const uint8_t endcoll_only[] = { 0xC0 };
        static const uint8_t longitem[] = { 0xFE, 0x02, 0x00, 0x00, 0x00 };
        static const uint8_t rid_zero[] = { 0x85,0x00, 0x05,0x01, 0x09,0x30,
                                            0x75,0x08, 0x95,0x01, 0x81,0x02 };
        static const uint8_t huge[] = { 0x05,0x01, 0x09,0x30,
                                        0x75,0x20, 0x96,0xFF,0xFF, 0x81,0x02 };
        static const uint8_t nothing[] = { 0x05,0x01, 0x09,0x02 };
        int i2;
        ck("an item cut off at the end",
           hrd_parse(trunc, sizeof trunc, &g_h) == HRD_E_TRUNC, 0);
        ck("Pop with no Push",
           hrd_parse(pop_only, sizeof pop_only, &g_h) == HRD_E_NEST, 0);
        ck("End Collection with no Collection",
           hrd_parse(endcoll_only, sizeof endcoll_only, &g_h) == HRD_E_NEST, 0);
        ck("a long item",
           hrd_parse(longitem, sizeof longitem, &g_h) == HRD_E_ITEM, 0);
        ck("report id zero (reserved)",
           hrd_parse(rid_zero, sizeof rid_zero, &g_h) == HRD_E_ITEM, 0);
        ck("a report of 65535 32-bit fields",
           hrd_parse(huge, sizeof huge, &g_h) == HRD_E_RANGE, 0);
        ck("a descriptor with no fields at all",
           hrd_parse(nothing, sizeof nothing, &g_h) == HRD_E_EMPTY, 0);
        {
            /* A Collection that is never closed. Every descriptor
               above is balanced, so the end-of-parse depth check had
               nothing testing it and an injection removing it
               survived. */
            static const uint8_t unclosed[] = {
                0x05,0x01, 0x09,0x02, 0xA1,0x01,
                0x09,0x30, 0x15,0x81, 0x25,0x7F,
                0x75,0x08, 0x95,0x01, 0x81,0x06
            };
            /* End Collection FIRST, then a balanced one: the depth
               ends at zero, so only the per-item check catches it. */
            static const uint8_t negdepth[] = {
                0xC0, 0x05,0x01, 0x09,0x02, 0xA1,0x01,
                0x09,0x30, 0x15,0x81, 0x25,0x7F,
                0x75,0x08, 0x95,0x01, 0x81,0x06
            };
            /* Sixty-four buttons: more fields than HRD_MAX_FIELDS. */
            static const uint8_t toomany[] = {
                0x05,0x09, 0x19,0x01, 0x29,0x40,
                0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x40, 0x81,0x02
            };
            /* A zero-WIDTH field before a real one. The field itself
               contributes nothing, and a parser that records it
               anyway hands a driver an X it cannot read. */
            static const uint8_t zerowide[] = {
                0x05,0x01, 0x09,0x30, 0x15,0x00, 0x25,0x01,
                0x75,0x00, 0x95,0x01, 0x81,0x02,
                0x09,0x30, 0x15,0x81, 0x25,0x7F,
                0x75,0x08, 0x95,0x01, 0x81,0x06
            };
            ck("a Collection that is never closed",
               hrd_parse(unclosed, sizeof unclosed, &g_h) == HRD_E_NEST, 0);
            ck("an End Collection before any Collection",
               hrd_parse(negdepth, sizeof negdepth, &g_h) == HRD_E_NEST, 0);
            ck("more fields than it can hold",
               hrd_parse(toomany, sizeof toomany, &g_h) == HRD_E_FULL, 0);
            ck("a zero-width field is not recorded",
               hrd_parse(zerowide, sizeof zerowide, &g_h) == HRD_OK
               && g_h.nfields == 1, g_h.nfields);
            ck("...and X is the readable one",
               g_h.i_x == 0 && g_h.field[0].bit_size == 8
               && g_h.field[0].bit_offset == 0,
               g_h.i_x);
        }
        ck("a NULL descriptor",
           hrd_parse(0, 10, &g_h) == HRD_E_PARAM, 0);
        ck("a zero length",
           hrd_parse(D_MOUSE, 0, &g_h) == HRD_E_PARAM, 0);

        /* ── EVERY PREFIX of a real descriptor ────────────────────
           A descriptor comes off a device, so it is untrusted, and
           the normal shape of this bug is a parser that handles the
           whole thing and reads one item past a short one. Under the
           ASAN build the Makefile uses, each prefix is in an
           allocation of exactly its own length. */
        {
            int bad = -1;
            for(i2 = 1; i2 < (int)sizeof D_TOUCH; i2++){
                uint8_t *q = (uint8_t *)malloc((size_t)i2);
                int r;
                memcpy(q, D_TOUCH, (size_t)i2);
                r = hrd_parse(q, (uint32_t)i2, &g_h);
                free(q);
                /* A prefix may legitimately parse -- a descriptor cut
                   after a complete Input item is a valid shorter
                   descriptor. What it must not do is read past the
                   end, which ASAN decides, or report HRD_OK with a
                   field beyond the report. */
                if(r == HRD_OK){
                    int k;
                    for(k = 0; k < g_h.nfields; k++)
                        if(g_h.field[k].bit_offset
                           + g_h.field[k].bit_size > HRD_MAX_BITS){
                            bad = i2; break;
                        }
                    if(bad >= 0) break;
                }
            }
            ck("no prefix of a touchpad descriptor produces a wild field",
               bad < 0, bad);
        }
    }

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
