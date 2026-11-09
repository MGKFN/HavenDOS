/* ================================================================
   shell/help.c  --  the shell command reference
   ================================================================

   Moved out of shell.c for 0.8.1, and corrected against the real
   dispatch table while it was moving.  What was wrong:

     * `pong` was listed, and described as "also on the desktop,
       under Fun".  There is no pong command: app_pong() lives in
       desktop.c and has never been reachable from the shell.  Help
       documented a command that does nothing, and the tab-completion
       table completed it.  Removed from both; the desktop-only apps
       get an honest line of their own at the bottom instead.

     * `h-lang` was listed as the HavenCode command.  The commands
       are `h`, `hplusplus` and `boot`; `h-lang` is the bpkg *package*
       id, which is a different namespace and only appears after
       `bpkg install`.

     * Eleven working commands were undocumented: boot, copy, host,
       hplusplus, lock, resolution, screensaver, script, type,
       version, and `.` as the pipe operator.

   How to keep it honest: the list below is checked against
   exec_cmd() by tools/check_help.py, which fails the build on a
   documented command that does not dispatch, the way
   check_boot_log.py fails it on invented boot output.  A help screen
   that drifts is worse than no help screen, because it is believed.
   ================================================================ */

#include "types.h"
#include "vga.h"
#include "version.h"

extern int  keyboard_waitchar(void);
extern uint32_t strlen(const char*);

/* Category: Files */
static const char *cat_files[]={
    "ls / dir        List files (color: cyan=dir green=file red=locked)",
    "tree            Show filesystem tree",
    "cat / type <f>  Show file contents",
    "head [-n] <f>   First N lines (default 10)",
    "tail [-n] <f>   Last N lines (default 10)",
    "write <f> <txt> Write text to file",
    "touch <file>    Create empty file",
    "mkdir <name>    Create directory",
    "del / rm <file> Delete file",
    "rename / mv <o> <n>  Rename or move file",
    "cp / copy <s> <d>    Copy file",
    "find <pattern>  Find files (* glob supported)",
    "grep <pat> <f>  Search in file",
    "wc <file>       Line/word/char count",
    "sort [-r] <f>   Sort lines (reverse with -r)",
    "uniq <file>     Sort and deduplicate",
    "diff <f1> <f2>  Compare two files",
    "tee <file>      Output to screen and file",
    "edit / notepad  Text editor",
    "stat <file>     Show file metadata (size, type, lock)",
    "hex <file>      Hexdump a file (hex -s <str> for strings)",
    "zip / unzip     Archive and extract files",
    "base64 <f>      Encode or decode (-d) a file",
    "hash <file>     Checksum a file",
    "df              Free space on the mounted filesystem",
    "pwd / cd <dir>  Where you are, and go somewhere else",
    NULL
};
/* Category: Shell */
static const char *cat_shell[]={
    "echo <text>     Print text (supports $VAR)",
    "set <k> <v>     Set environment variable",
    "unset <k>       Remove environment variable",
    "export <k> <v>  Alias for set",
    "env             Show all env variables",
    "alias [k=v]     Set or list aliases",
    "unalias <name>  Remove alias",
    "history         Command history",
    "ps1 [format]    Show or set prompt format",
    "run <file>      Run a .BSH shell script",
    "script <file>   Record a session to a file",
    "source <file>   Run a script in the current shell",
    "which <cmd>     Check if command exists",
    "calc <expr>     Calculator (5+3*2 works)",
    "repeat <n> <cmd>  Run command N times",
    "seq <from> <to> Number sequence",
    "cls / clear     Clear screen",
    "watch <cmd>     Re-run a command until a key is pressed",
    "timer <secs>    Count down, then beep",
    "yes [text]      Repeat a line until stopped",
    "color / motd    Prompt colours; the message of the day",
    "res / resolution  Text grid: 80x25, 80x50, 90x30, 90x60",
    "cmd > file      Redirect output to file",
    "cmd >> file     Append output to file",
    "cmd . cmd       Pipe output between commands",
    NULL
};
/* Category: System */
static const char *cat_sys[]={
    "sysinfo         Full system information",
    "sysinfo-plus    Deeper system information panel",
    "mem             Memory stats",
    "uptime          System uptime",
    "disk            Disk/storage status",
    "dmesg           Kernel boot log, paged",
    "kpoll [on|off]  What runs on a timer, and why a poll was refused",
    "ps              Process list",
    "ver / version   Version info",
    "date            Current date (RTC)",
    "time            Current time (RTC)",
    "dashboard / htop  Live system monitor",
    "neofetch        The system at a glance, with the logo",
    "lspci [-v]      Every PCI device, decoded by class",
    "usb [-v|-vv]    USB controllers, ports, hubs and devices",
    "usb rescan      walk the bus again (a stick plugged in after boot)",
    "usb cfg [addr]  every configuration a device offers (read-only)",
    "usb eth         USB network adapters, and the raw framing",
    "usb mux         Apple Mobile Device channel (lockdown/pairing)",
    "usb lockdown    ask lockdownd what it is -- pairing stage 1",
    "fat32           FAT32 volumes, incl. the ESP (read-only)",
    "exfat           exFAT volumes -- what a stick over 32 GB is (read-only)",
    "rsa             RSA key generation and the primality test",
    "i2c             I2C controllers, and what answers on the bus",
    "i2chid          the trackpad: what was found on the I2C buses",
    "lockdown        ask lockdownd what it is -- pairing stages 1 and 2",
    "pair            pair with a plugged-in iPhone, then open a session",
    "pair session    open an encrypted session from the stored record",
    "pair show       what is in the stored pair record",
    "pair forget     delete the stored pair record",
    "crash <how>     fault on purpose, to test the app guard",
    "layout          open every app and report what draws off-screen",
    "fat16           FAT16 volumes on any disk; ls, cat, cp onto a stick",
    "selftest        Run the built-in self test",
    "notify <t> <m>  Send toast notification",
    "beep [hz] [ms]  Play tone via speaker",
    "play            Boot jingle",
    "install         Install HavenDOS to a disk (VirtIO or ATA)",
    "uninstall       Remove HavenDOS from the disk it is installed on",
    "update          Upgrade kernel from inserted ISO",
    "rollback        Restore previous kernel version",
    "reboot          Restart the system",
    "shutdown / poweroff  Power the machine off",
    NULL
};
/* Category: Users */
static const char *cat_users[]={
    "whoami          Current user",
    "users           List all accounts",
    "passwd [user]   Change password",
    "adduser <n> <p> Add user (admin only)",
    "deluser <user>  Delete user (admin only)",
    "chmod <f> lock  Lock file (admin only)",
    "chown <f> <usr> Change file owner (admin only)",
    "lock            Lock the screen until a password is entered",
    "log             Audit log (admin only)",
    NULL
};
/* Category: Network */
static const char *cat_net[]={
    "ifconfig        Show or set network config",
    "ping <ip>       Ping an IP address",
    "netstat         Network connection status",
    "udpsend         Send a UDP datagram: udpsend <ip> <port> <msg>",
    "tcptest         Test a TCP connection: tcptest <host> <port>",
    "tcpsend         Interactive TCP: tcpsend <host> <port>",
    "chat <server>   BOOT Chat client",
    "wget / curl     Fetch a URL over HTTP",
    "nslookup / dns / host  Resolve a name",
    "tls / tlsinfo   The TLS layer and what it agreed",
    NULL
};
/* Category: Fun and Apps */
static const char *cat_fun[]={
    "matrix          Matrix screensaver",
    "pipes           Pipes screensaver",
    "stars           Starfield screensaver",
    "screensaver <n>  Pick a screensaver by name",
    "snake           Snake game",
    "theme [n]       List or switch theme",
    "fortune         Random quote",
    "banner <text>   Big ASCII-art text",
    "cowsay [msg]    Important system utility",
    "ascii-art <txt> Large ASCII banner text",
    "calendar        This month, with today marked",
    "todo            The to-do list",
    "paint           ASCII drawing canvas",
    "music           PC speaker melodies",
    "quiz            Trivia quiz",
    "hacker          Retro intrusion animation",
    "bpkg            List and install the optional packages",
    "h / hplusplus / boot  HavenCode: the TechHaven H language",
    "guide           Built-in user guide",
    "man <cmd>       Detailed help for a command",
    "help / ?        This help screen",
    "exit / quit     Return to desktop",
    NULL
};

typedef struct { const char *title; const char **cmds; vga_color_t col; } cat_t;
static const cat_t cats[]={
    {"Files and Filesystem", cat_files, VGA_LIGHT_CYAN},
    {"Shell and Scripting",  cat_shell, VGA_LIGHT_GREEN},
    {"System",               cat_sys,   VGA_YELLOW},
    {"Users and Security",   cat_users, VGA_LIGHT_RED},
    {"Network",              cat_net,   VGA_LIGHT_BLUE},
    {"Fun and Apps",         cat_fun,   VGA_LIGHT_MAGENTA},
    {NULL,NULL,0}
};

void cmd_help(void){
    vga_set_color(VGA_BLACK,VGA_LIGHT_CYAN);
    for(int x=0;x<80;x++) vga_putchar(' ');
    vga_set_color(VGA_WHITE,VGA_BLACK);
    vga_puts("\r  HavenDOS " HAVENDOS_VER "  Shell Command Reference\n");
    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    for(int i=0;i<80;i++) vga_putchar('-');
    vga_putchar('\n');
    vga_set_color(VGA_WHITE,VGA_BLACK);

    int printed=0;
    for(int c=0;cats[c].title;c++){
        vga_set_color(cats[c].col,VGA_BLACK);
        vga_puts("  ");
        vga_puts(cats[c].title);
        vga_putchar('\n');
        vga_set_color(VGA_DARK_GREY,VGA_BLACK);
        vga_puts("  ");
        for(int k=0;k<(int)strlen(cats[c].title);k++) vga_putchar('-');
        vga_putchar('\n');
        for(int i=0;cats[c].cmds[i];i++){
            /* Split the name from its description.  A run of two spaces is
               the separator where one exists; column 16 is the fallback for
               the older entries that only ever had one.  (Cutting blindly at
               16 is what turned "nslookup / dns / host" into
               "nslookup / dns /host".) */
            const char *entry=cats[c].cmds[i];
            int nlen=0;
            while(entry[nlen] && !(entry[nlen]==' ' && entry[nlen+1]==' ')) nlen++;
            if(!entry[nlen]){
                nlen=0;
                while(entry[nlen] && nlen<16) nlen++;
                while(nlen>0 && entry[nlen-1]==' ') nlen--;
            }
            vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
            vga_puts("    ");
            for(int k=0;k<nlen;k++) vga_putchar(entry[k]);
            /* Pad to column 16, but never fewer than two spaces.  The
               do/while alone padded only up to 16, so a name AT or OVER
               16 chars got exactly one space and ran into its own
               description: "rename / mv <o> <n> Rename or move file"
               reads as if "Rename" were part of the syntax.  0.8.1 added
               several such entries ("h / hplusplus / boot",
               "nslookup / dns / host"), which is what surfaced it. */
            int col=nlen, pad=0;
            do { vga_putchar(' '); col++; pad++; } while(col<16||pad<2);
            vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
            int j=nlen;
            while(entry[j]==' ') j++;
            vga_puts(entry+j);
            vga_putchar('\n');
            printed++;
            if(printed%20==0&&(cats[c].cmds[i+1]||cats[c+1].title)){
                vga_set_color(VGA_BLACK,VGA_DARK_GREY);
                vga_puts(" -- more -- press any key -- ");
                vga_set_color(VGA_WHITE,VGA_BLACK);
                keyboard_waitchar();
                vga_puts("\r                               \r");
            }
        }
        vga_putchar('\n');
    }
    /* The desktop-only apps.  Named here because they are part of the
       system and someone reading the shell reference should know where
       they are, and NOT listed above because they are not commands --
       which is exactly the mistake `pong` used to be. */
    vga_set_color(VGA_LIGHT_MAGENTA,VGA_BLACK);
    vga_puts("  On the desktop only\n");
    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    vga_puts("  -------------------\n");
    vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
    vga_puts("    Pong, Tetris, File Manager, Clock, Hex Viewer, Disk Info,\n");
    vga_puts("    Network Info, User Manager, Log Viewer, Notifications.\n");
    vga_puts("    Type 'exit' for the desktop, then ESC for the start menu.\n\n");
    vga_set_color(VGA_DARK_GREY,VGA_BLACK);
    vga_puts("  Type 'man <cmd>' for details on any command. Type 'guide' for the user guide.\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);
}
