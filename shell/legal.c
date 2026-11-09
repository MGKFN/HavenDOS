/*
 * legal.c - HavenDOS v0.7.11 Terms of Service / Privacy Policy
 *
 * MOVED OUT OF THE INSTALLER for v0.7.11.  Showing these during installation
 * was the wrong place: at that point the user has not yet chosen a keyboard,
 * a username or a theme, the machine may be someone else's, and refusing the
 * terms meant abandoning an install that had already selected a target drive.
 * They now run as part of the first-boot setup wizard, where the person
 * actually using the machine is the one reading them.
 */
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/types.h"

extern int  keyboard_waitchar(void);

#define LEGAL_HDR_BG   VGA_BLUE
#define LEGAL_BODY_BG  VGA_BLACK

static void legal_hdr(const char *title, int step, int total){
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,0,VGA_WHITE,LEGAL_HDR_BG);
    vga_puts_at(" HavenDOS Setup ",1,0,VGA_WHITE,LEGAL_HDR_BG);
    vga_puts_at(title,19,0,VGA_YELLOW,LEGAL_HDR_BG);
    char sb[16]; int si=0;
    sb[si++]='S'; sb[si++]='t'; sb[si++]='e'; sb[si++]='p'; sb[si++]=' ';
    sb[si++]=(char)('0'+step); sb[si++]='/'; sb[si++]=(char)('0'+total); sb[si]=0;
    vga_puts_at(sb,80-(int)strlen(sb)-1,0,VGA_WHITE,LEGAL_HDR_BG);
}
static void legal_footer(const char *hint){
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,24,VGA_BLACK,VGA_LIGHT_GREY);
    vga_puts_at(hint,2,24,VGA_BLACK,VGA_LIGHT_GREY);
}

static const char *tos_lines[]={
    "HavenDOS is developed and published by TechHaven Studios.",
    "By installing HavenDOS you agree to the terms below. Please",
    "read them in full before continuing.",
    "",
    "1. NO WARRANTY",
    "   HavenDOS is provided as-is, without warranty of any kind,",
    "   express or implied, including but not limited to warranties",
    "   of merchantability or fitness for a particular purpose.",
    "   TechHaven Studios is not liable for data loss, hardware",
    "   damage, downtime, or any other loss arising from its use.",
    "",
    "2. EXPERIMENTAL SOFTWARE",
    "   HavenDOS is a hobby operating system written from scratch",
    "   and is not certified for production, safety-critical, or",
    "   commercial deployment. Do not install it on a machine whose",
    "   contents you are not prepared to lose.",
    "",
    "3. DESTRUCTIVE INSTALLATION",
    "   Installation ERASES ALL DATA on the selected drive, including",
    "   any other operating systems and their partition tables. Back",
    "   up anything you care about before continuing. You are solely",
    "   responsible for selecting the correct target drive.",
    "",
    "4. LICENCE AND USE",
    "   HavenDOS is free for personal, educational and research use.",
    "   Commercial redistribution requires written permission from",
    "   TechHaven Studios. Source code is published on GitHub under",
    "   the TechHaven Studios open licence.",
    "",
    "5. NETWORKING",
    "   Optional network features transmit data over the internet,",
    "   including package downloads and HTTPS requests you initiate.",
    "   Use them responsibly and in accordance with the laws that",
    "   apply where you are.",
    "",
    "6. CRYPTOGRAPHY DISCLAIMER",
    "   The TLS implementation is written from scratch and has not",
    "   been independently audited. It supports TLS 1.2 with RSA key",
    "   exchange only, and does not verify certificate chains against",
    "   a certificate authority store. Treat HTTPS in HavenDOS as",
    "   protection against casual observation, NOT as a guarantee of",
    "   confidentiality or of the remote server's identity. Do not",
    "   transmit passwords, payment details, or other sensitive data.",
    "",
    "7. CHANGES",
    "   These terms may change between releases. The terms shown at",
    "   install time are the terms that apply to that installation.",
    "",
    "                    -- end of terms --",
};

static const char *privacy_lines[]={
    "TechHaven Studios Privacy Policy for HavenDOS.",
    "",
    "SUMMARY",
    "   HavenDOS does not collect, transmit, or sell your personal",
    "   data. There is no telemetry, no analytics, and no background",
    "   reporting of any kind. Network traffic happens only when you",
    "   ask for it.",
    "",
    "1. DATA STORED ON YOUR MACHINE",
    "   HavenDOS stores the following locally on the installed drive,",
    "   and nowhere else:",
    "     - user accounts and password hashes",
    "     - your files, notes, reminders and to-do items",
    "     - shell history and environment variables",
    "     - theme, display and network preferences",
    "     - system logs viewable with the dmesg command",
    "   None of this is transmitted anywhere by the system itself.",
    "",
    "2. WHEN HAVENDOS USES THE NETWORK",
    "   Network activity occurs only in response to an action you",
    "   take. Specifically:",
    "     - ping, curl, wget, and the bpkg package manager",
    "     - DNS lookups needed to resolve names you supply",
    "     - DHCP, to obtain an address on your local network",
    "     - checking for updates, when you run that command",
    "   No connection is opened at boot and none is kept open in the",
    "   background.",
    "",
    "3. WHAT REMOTE SERVERS CAN SEE",
    "   When you make a request, the server you contact will see your",
    "   IP address, the request itself, and the HavenDOS user-agent",
    "   string. This is inherent to how the internet works and is not",
    "   specific to HavenDOS. TechHaven Studios operates the package",
    "   repository and can see download requests made to it; no",
    "   account or identifier is attached to them.",
    "",
    "4. SECURITY LIMITATIONS - PLEASE READ",
    "   Stored credentials are protected against casual inspection",
    "   only. HavenDOS has no disk encryption, and anyone with",
    "   physical access to the drive can read its contents. Certificate",
    "   chains are not verified against a CA store, so an attacker on",
    "   your network path could impersonate an HTTPS server. Do not",
    "   store secrets on a HavenDOS installation.",
    "",
    "5. CHILDREN AND THIRD PARTIES",
    "   HavenDOS is not directed at children and collects nothing from",
    "   anyone. No data is shared with third parties, because none is",
    "   collected in the first place.",
    "",
    "6. CONTACT",
    "   Questions about this policy can be raised as an issue on the",
    "   TechHaven Studios GitHub repository.",
    "",
    "                   -- end of policy --",
};

/* ── Scrollable legal document viewer (v0.7.11) ───────────────────────────
 * The old Terms screen printed its array straight onto rows 5..21 and did
 *     if(5+i>=22) break;
 * so anything past the seventeenth line was silently discarded - the document
 * could not be read in full and there was no indication anything was missing.
 * This viewer scrolls with the arrow keys, shows a proportional scrollbar and
 * a percentage, and will not enable Accept until the reader has actually
 * reached the bottom.
 * ─────────────────────────────────────────────────────────────────────── */
#define DOC_TOP     5
#define DOC_BOT    21
#define DOC_ROWS   (DOC_BOT-DOC_TOP+1)

int legal_show_document(const char *title, const char *sub,
                           const char **lines, int nlines,
                           int step, int total)
{
    int top=0, seen_end=0;
    int maxtop = nlines>DOC_ROWS ? nlines-DOC_ROWS : 0;
    if(maxtop==0) seen_end=1;

    while(1){
        for(int y=0;y<25;y++) for(int x=0;x<80;x++)
            vga_putchar_at(' ',x,y,VGA_BLACK,LEGAL_HDR_BG);
        legal_hdr(title, step, total);

        /* The document sits on a solid panel.  Painting it before the text
           goes down is what makes it a panel: writing only the characters
           leaves the background colour showing through wherever a line is
           short, which reads as a ragged edge rather than a page. */
        for(int y=2;y<=23;y++) for(int x=2;x<78;x++)
            vga_putchar_at(' ',x,y,VGA_BLACK,LEGAL_BODY_BG);

        vga_puts_at(sub,(80-(int)strlen(sub))/2,3,VGA_LIGHT_CYAN,LEGAL_BODY_BG);
        for(int x=4;x<76;x++) vga_putchar_at('-',x,4,VGA_DARK_GREY,LEGAL_BODY_BG);

        /* body */
        for(int r=0;r<DOC_ROWS;r++){
            int li=top+r;
            if(li>=nlines) break;
            const char *t=lines[li];
            /* numbered clauses and headings in white, prose in grey */
            int head = (t[0]>='1'&&t[0]<='9') || (t[0]>='A'&&t[0]<='Z'&&t[1]>='A'&&t[1]<='Z');
            vga_puts_at(t,4,DOC_TOP+r,head?VGA_WHITE:VGA_LIGHT_GREY,LEGAL_BODY_BG);
        }

        /* proportional scrollbar in column 77 */
        if(maxtop>0){
            int track=DOC_ROWS;
            int thumb=(track*DOC_ROWS)/nlines; if(thumb<1) thumb=1;
            int tpos =(track-thumb)*top/maxtop;
            for(int r=0;r<track;r++){
                int on=(r>=tpos&&r<tpos+thumb);
                vga_putchar_at(on?'#':'.',77,DOC_TOP+r,
                               on?VGA_LIGHT_CYAN:VGA_DARK_GREY,LEGAL_BODY_BG);
            }
        }

        for(int x=4;x<76;x++) vga_putchar_at('-',x,22,VGA_DARK_GREY,LEGAL_BODY_BG);

        /* progress + prompt */
        int pct = maxtop? (top*100)/maxtop : 100;
        char pb[8]; utoa((uint32_t)pct,pb,10);
        vga_puts_at("Read: ",4,23,VGA_DARK_GREY,LEGAL_BODY_BG);
        vga_puts_at(pb,10,23,seen_end?VGA_LIGHT_GREEN:VGA_YELLOW,LEGAL_BODY_BG);
        vga_puts_at("%",10+(int)strlen(pb),23,
                    seen_end?VGA_LIGHT_GREEN:VGA_YELLOW,LEGAL_BODY_BG);

        if(seen_end){
            vga_puts_at("A = Accept and continue    D = Decline",24,23,
                        VGA_WHITE,LEGAL_BODY_BG);
            legal_footer("Up/Down/PgUp/PgDn=scroll  A=Accept  D=Decline  ESC=Cancel");
        } else {
            vga_puts_at("Scroll to the end to continue",24,23,
                        VGA_YELLOW,LEGAL_BODY_BG);
            legal_footer("Up/Down/PgUp/PgDn=scroll  END=jump to bottom  ESC=Cancel");
        }
        vga_flush();

        int k=keyboard_waitchar();
        if(k==0x148){ if(top>0) top--; }                       /* Up    */
        else if(k==0x150){ if(top<maxtop) top++; }             /* Down  */
        else if(k==0x149){ top-=DOC_ROWS; if(top<0) top=0; }   /* PgUp  */
        else if(k==0x151){ top+=DOC_ROWS; if(top>maxtop) top=maxtop; }
        else if(k==0x147){ top=0; }                            /* Home  */
        else if(k==0x14F){ top=maxtop; }                       /* End   */
        else if(k==' ')  { top+=DOC_ROWS; if(top>maxtop) top=maxtop; }
        else if(k==27)   return 0;
        else if(seen_end && (k=='a'||k=='A')) return 1;
        else if(seen_end && (k=='d'||k=='D')) return 0;

        if(top>=maxtop) seen_end=1;
    }
}

int legal_show_tos(void){
    return legal_show_document("Terms of Service","Terms of Service",
        tos_lines,(int)(sizeof(tos_lines)/sizeof(tos_lines[0])),1,7);
}
int legal_show_privacy(void){
    return legal_show_document("Privacy Policy","Privacy Policy",
        privacy_lines,(int)(sizeof(privacy_lines)/sizeof(privacy_lines[0])),2,7);
}
