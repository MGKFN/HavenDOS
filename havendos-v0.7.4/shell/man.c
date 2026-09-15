/*
 * man.c — HavenDOS v0.5.9.10 Manual Pages
 * Per-command help screens with paging
 */
#include "../include/vga.h"
#include "../include/string.h"
#include "../include/types.h"

extern int  keyboard_waitchar(void);
extern void sleep_ms(uint32_t);

/* ── Man page entry ─────────────────────────────────────── */
typedef struct {
    const char *cmd;
    const char *synopsis;
    const char *desc;
    const char *usage;
    const char *examples;
    const char *notes;
} manpage_t;

static const manpage_t pages[] = {
    {"help",    "help [page]",
     "Display list of all available shell commands.\nOptional page number for multi-page help.",
     "help\n  help 2",
     "help        Show first page\n  help 2      Show second page",
     "Use 'man <cmd>' for detailed help on any command."},

    {"echo",    "echo <text>",
     "Print text to the shell output.\nSupports $VAR expansion.",
     "echo <text>",
     "echo Hello World\n  echo OS is $OS\n  echo Version $VER",
     "Text is printed as-is. $VAR is expanded before printing."},

    {"ls",      "ls / dir",
     "List all files in the current filesystem.\nShows filename, size, and permissions.",
     "ls\n  dir",
     "ls\n  dir",
     "Both 'ls' and 'dir' are equivalent."},

    {"cat",     "cat <file>",
     "Display the contents of a file.",
     "cat <filename>",
     "cat README.TXT\n  cat HELLO.BSH",
     "File must exist in RAMFS. Binary files may display garbage."},

    {"cd",      "cd <dir>",
     "Change current directory.",
     "cd <directory>",
     "cd /home\n  cd ..",
     "RAMFS supports basic directory structure."},

    {"mkdir",   "mkdir <dir>",
     "Create a new directory.",
     "mkdir <dirname>",
     "mkdir mydir\n  mkdir projects",
     NULL},

    {"rm",      "rm <file>",
     "Delete a file from the filesystem.",
     "rm <filename>",
     "rm oldfile.txt\n  rm TEMP.BSH",
     "Cannot delete directories. This action is permanent."},

    {"touch",   "touch <file>",
     "Create an empty file, or update timestamp if it exists.",
     "touch <filename>",
     "touch newfile.txt",
     NULL},

    {"cp",      "cp <src> <dst>",
     "Copy a file to a new location.",
     "cp <source> <destination>",
     "cp README.TXT BACKUP.TXT",
     NULL},

    {"mv",      "mv <src> <dst>",
     "Move or rename a file.",
     "mv <source> <destination>",
     "mv OLD.TXT NEW.TXT",
     "Alias: rename"},

    {"wc",      "wc <file>",
     "Count lines, words, and characters in a file.",
     "wc <filename>",
     "wc README.TXT",
     NULL},

    {"head",    "head <file>",
     "Show the first 10 lines of a file.",
     "head <filename>",
     "head README.TXT",
     NULL},

    {"tail",    "tail <file>",
     "Show the last 10 lines of a file.",
     "tail <filename>",
     "tail CHANGELOG.TXT",
     NULL},

    {"grep",    "grep <pattern> <file>",
     "Search for lines matching a pattern in a file.",
     "grep <pattern> <filename>",
     "grep BOOT README.TXT\n  grep error LOG.TXT",
     "Pattern matching is case-sensitive."},

    {"find",    "find <name>",
     "Search for a file by name in the filesystem.",
     "find <filename>",
     "find README.TXT\n  find .BSH",
     "Partial name matching supported."},

    {"sort",    "sort <file>",
     "Sort lines of a file alphabetically.",
     "sort <filename>",
     "sort LIST.TXT",
     NULL},

    {"set",     "set <key> <value>",
     "Set an environment variable.\nAlso available as 'export'.",
     "set <key> <value>\n  export <key> <value>",
     "set NAME TechHaven\n  export OS HavenDOS\n  echo $NAME",
     "Variables persist for the session. Use 'env' to list all."},

    {"unset",   "unset <key>",
     "Remove an environment variable.",
     "unset <key>",
     "unset TEMP\n  unset MYVAR",
     "Permanent for the session. Cannot unset OS or VER."},

    {"env",     "env",
     "List all current environment variables.",
     "env",
     "env",
     "Variables set with 'set' or 'export' appear here."},

    {"alias",   "alias <name>=<cmd>",
     "Create a shortcut for a command. Saved to disk automatically\n"
     "and reloaded on every boot/login.",
     "alias <name>=<command>",
     "alias ll=ls\n  alias q=exit\n  alias gs=guide",
     "Saved to ETC/ALIASES.TXT. Built-ins: ll, q, h, ?, g. unalias also saves."},

    {"history", "history",
     "Show the list of previously entered commands.\nUse UP/DOWN arrows to navigate history.",
     "history",
     "history",
     "History holds up to 64 entries. UP arrow recalls previous commands."},

    {"run",     "run <script.bsh>",
     "Execute a BSH shell script file.\n"
     "Supports if/else/endif and while/endwhile for control flow,\n"
     "plus $VAR expansion and pipes on every line.\n"
     "Lines starting with # are comments.",
     "run <filename>",
     "run HOME/LOOP.BSH\n"
     "  if $i == 3\n  echo three\n  else\n  echo not three\n  endif\n"
     "  while $i <= 5\n  echo $i\n  set i $i+1\n  endwhile",
     "Ops: == != < > <= >=. 'exit' inside a script stops it early.\n"
     "Step limit 4000 guards against infinite loops."},

    {"date",    "date",
     "Show the current date from the RTC (Real Time Clock).",
     "date",
     "date",
     "Format: DD/MM/YY. Read from CMOS RTC hardware."},

    {"time",    "time",
     "Show the current time from the RTC (Real Time Clock).",
     "time",
     "time",
     "Format: HH:MM:SS. Read from CMOS RTC hardware."},

    {"sysinfo", "sysinfo",
     "Display full system information.\nShows OS version, CPU, RAM, disk, date/time, user, and session info.",
     "sysinfo",
     "sysinfo",
     NULL},

    {"uptime",  "uptime",
     "Show how long the system has been running since boot.",
     "uptime",
     "uptime",
     "Format: HH:MM:SS based on PIT timer ticks."},

    {"disk",    "disk",
     "Show disk and filesystem status.",
     "disk",
     "disk",
     "Shows whether ATA disk or RAMFS is active."},

    {"adduser", "adduser <name> <password>",
     "Create a new user account. Admin only.",
     "adduser <username> <password>",
     "adduser alice Secret123",
     "Max 8 users. Passwords are hashed. Admin account required."},

    {"color",   "color <fg> [bg]",
     "Set the shell text foreground and background colour.\nColour numbers 0-15.",
     "color <fg> [bg]",
     "color 10        Green text\n  color 14 1    Yellow on blue\n  color 7 0     Default",
     "0=Black 1=Blue 2=Green 3=Cyan 4=Red 7=LGrey 10=LGreen 14=Yellow 15=White"},

    {"cowsay",  "cowsay [message]",
     "Display a message spoken by a cow.\nVery important system utility.",
     "cowsay [message]",
     "cowsay Hello World\n  cowsay HavenDOS rocks",
     "If no message given, defaults to 'Moo!'"},

    {"ping",    "ping <ip>",
     "Send ICMP echo request to an IP address.",
     "ping <ip-address>",
     "ping 10.0.2.2\n  ping 8.8.8.8",
     "Requires network card (RTL8139 or e1000). QEMU/UTM default GW: 10.0.2.2"},

    {"ifconfig","ifconfig [ip] [mac]",
     "Show or set network interface configuration.",
     "ifconfig\n  ifconfig <ip> <mac>",
     "ifconfig\n  ifconfig 10.0.2.15 AA:BB:CC:DD:EE:FF",
     NULL},

    {"netstat", "netstat",
     "Show network status and statistics.",
     "netstat",
     "netstat",
     NULL},

    {"udpsend", "udpsend <ip> <port> <msg>",
     "Send a UDP datagram to an IP address and port.",
     "udpsend <ip> <port> <message>",
     "udpsend 10.0.2.2 1234 hello",
     "No connection required. Fire-and-forget UDP packet."},

    {"edit",    "edit [filename]",
     "Open the Notepad text editor.\nOptionally open an existing file.",
     "edit\n  edit <filename>",
     "edit\n  edit README.TXT\n  edit SCRIPT.BSH",
     "Alias: notepad. Save with F2, quit with ESC."},

    {"calc",    "calc",
     "Open the calculator application.",
     "calc",
     "calc",
     "Supports +, -, *, /. Coming in v0.5.9.10."},

    {"reboot",  "reboot",
     "Restart the system immediately.",
     "reboot",
     "reboot",
     "Triggers PS/2 keyboard controller reset (port 0x64)."},

    {"shutdown","shutdown",
     "Power off the system.\nAlso available as 'poweroff'.",
     "shutdown\n  poweroff",
     "shutdown",
     "Sends ACPI shutdown signal. Works in QEMU and UTM SE."},

    {"man",     "man <command>",
     "Display the manual page for a command.\nThis is the man command itself.",
     "man <command>",
     "man ls\n  man ping\n  man run",
     "Press any key to close the man page. Type 'man man' for this page."},

    {"ps1",
     "ps1 — Shell Prompt Customisation",
     "ps1 [format]",
     "Show or set the shell prompt format string.\n"
     "Without arguments, shows the current PS1 and available tokens.\n"
     "With a format string, sets the prompt immediately.",
     "Tokens: \\u=username  \\h=hostname  \\w=path  \\$=$/# \\t=HH:MM:SS  \\d=DD/MM/YYYY  \\n=newline",
     "ps1 \\u@\\h:\\w\\$ \n  ps1 [\\t] \\u\\$ "},
    {"fortune",
     "fortune — Print a Random Quote",
     "fortune",
     "Prints a random inspirational or humorous quote from the built-in\n"
     "quote database. Also shown automatically on every login.",
     "No arguments. Quote is selected based on current tick count.",
     "fortune"},
    {"banner",
     "banner — Large ASCII Art Text",
     "banner <text>",
     "Prints the given text in large 5x5 ASCII art block letters.\n"
     "Supports A-Z, 0-9, and spaces. Maximum 12 characters.\n"
     "Letters are rendered in light cyan.",
     "Text is automatically uppercased. Longer text is truncated at 12 chars.",
     "banner BOOT\n  banner Hello"},
    {"theme",
     "theme — Switch Colour Theme",
     "theme [number]",
     "List available colour themes or switch to one by number.\n"
     "Themes affect the taskbar, desktop, windows, and shell prompt colour.\n"
     "Changes take effect immediately.",
     "10 built-in themes: Midnight Cyan, Classic Blue, Terminal Green,\n"
     "Amber, Dark, Arctic, Neon, Mint, Sunset, Retro.",
     "theme        (list all)\n  theme 0      (Midnight Cyan)\n  theme 4      (Dark)"},
    {"tree",
     "tree — Show Filesystem Tree",
     "tree",
     "Display all files and directories as a tree rooted at /.\n"
     "Directories are shown in cyan, files in green, locked files in red.\n"
     "Uses safe characters only (no box-drawing).",
     "tree",
     "+-- item = branch entry, .-- item = last entry in list."},

    {"which",
     "which — Locate a Command",
     "which <command>",
     "Check whether a command is a built-in shell command, an alias,\n"
     "or a .BSH script in the filesystem.\n"
     "Prints the type and location of the command.",
     "which ls\n  which q\n  which myscript",
     "Returns 'not found' if the command does not exist anywhere."},

    {"guide",
     "guide — Interactive User Guide",
     "guide",
     "Open the built-in multi-page user guide covering:\n"
     "  Page 1: Welcome and navigation\n"
     "  Page 2: Files and filesystem\n"
     "  Page 3: Shell features and scripting\n"
     "  Page 4: Users and security\n"
     "  Page 5: Apps, games and screensavers\n"
     "  Page 6: Network and pro tips",
     "guide",
     "Press any key to advance pages. 6 pages total."},

    {NULL,NULL,NULL,NULL,NULL,NULL}
};

/* ── Render a man page ──────────────────────────────────── */
static void man_render(const manpage_t *p){
    vga_clear();
    /* Header */
    for(int x=0;x<80;x++) vga_putchar_at(' ',x,0,VGA_BLACK,VGA_CYAN);
    /* Pad title to centre */
    char hdr[80]; int hl=0;
    hdr[hl++]=' '; hdr[hl++]='M'; hdr[hl++]='A'; hdr[hl++]='N';
    hdr[hl++]=' '; hdr[hl++]='P'; hdr[hl++]='A'; hdr[hl++]='G'; hdr[hl++]='E';
    hdr[hl++]=' '; hdr[hl++]=' '; hdr[hl++]=' ';
    const char *c=p->cmd; while(*c) hdr[hl++]=*c++;
    hdr[hl]=0;
    vga_puts_at(hdr, (80-hl)/2, 0, VGA_BLACK, VGA_CYAN);

    int y=2;
    /* NAME */
    vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts_at("NAME",2,y++,VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts_at(p->cmd,6,y++,VGA_WHITE,VGA_BLACK);

    /* SYNOPSIS */
    y++;
    vga_puts_at("SYNOPSIS",2,y++,VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts_at(p->synopsis,6,y++,VGA_YELLOW,VGA_BLACK);

    /* DESCRIPTION */
    y++;
    vga_puts_at("DESCRIPTION",2,y++,VGA_LIGHT_CYAN,VGA_BLACK);
    /* Word-wrap desc at col 74 */
    const char *d=p->desc; int x=6;
    while(*d){
        if(*d=='\n'){y++;x=6;d++;continue;}
        vga_putchar_at(*d,x,y,VGA_LIGHT_GREY,VGA_BLACK);
        x++; if(x>=74){y++;x=6;}
        d++;
    }
    y+=2;

    /* USAGE */
    if(p->usage){
        vga_puts_at("USAGE",2,y++,VGA_LIGHT_CYAN,VGA_BLACK);
        const char *u=p->usage; x=6;
        while(*u&&y<20){
            if(*u=='\n'){y++;x=6;u++;continue;}
            vga_putchar_at(*u,x,y,VGA_LIGHT_GREEN,VGA_BLACK);
            x++; if(x>=74){y++;x=6;}
            u++;
        }
        y+=2;
    }

    /* EXAMPLES */
    if(p->examples&&y<20){
        vga_puts_at("EXAMPLES",2,y++,VGA_LIGHT_CYAN,VGA_BLACK);
        const char *e=p->examples; x=6;
        while(*e&&y<20){
            if(*e=='\n'){y++;x=6;e++;continue;}
            vga_putchar_at(*e,x,y,VGA_LIGHT_GREY,VGA_BLACK);
            x++; if(x>=74){y++;x=6;}
            e++;
        }
        y+=2;
    }

    /* NOTES */
    if(p->notes&&y<22){
        vga_puts_at("NOTES",2,y++,VGA_LIGHT_CYAN,VGA_BLACK);
        const char *n=p->notes; x=6;
        while(*n&&y<22){
            if(*n=='\n'){y++;x=6;n++;continue;}
            vga_putchar_at(*n,x,y,VGA_DARK_GREY,VGA_BLACK);
            x++; if(x>=74){y++;x=6;}
            n++;
        }
    }

    /* Footer */
    for(int fx=0;fx<80;fx++) vga_putchar_at(' ',fx,24,VGA_BLACK,VGA_DARK_GREY);
    vga_puts_at(" HavenDOS Manual  |  Press any key to close ",
                18,24,VGA_LIGHT_GREY,VGA_DARK_GREY);

    keyboard_waitchar();
    vga_clear();
}

/* ── Public entry: man <cmd> ────────────────────────────── */
void cmd_man(const char *cmd){
    if(!cmd||!cmd[0]){
        /* List all available man pages */
        vga_clear();
        for(int x=0;x<80;x++) vga_putchar_at(' ',x,0,VGA_BLACK,VGA_CYAN);
        vga_puts_at(" HavenDOS Manual — Available Pages ",23,0,VGA_BLACK,VGA_CYAN);
        int y=2,col=0;
        vga_set_color(VGA_LIGHT_GREY,VGA_BLACK);
        vga_puts_at("Commands with man pages:",2,y++,VGA_LIGHT_CYAN,VGA_BLACK);
        y++;
        for(int i=0;pages[i].cmd;i++){
            vga_puts_at(pages[i].cmd, 2+(col*20), y, VGA_LIGHT_GREEN,VGA_BLACK);
            col++;
            if(col>=4){col=0;y++;}
            if(y>=23) break;
        }
        for(int x=0;x<80;x++) vga_putchar_at(' ',x,24,VGA_BLACK,VGA_DARK_GREY);
        vga_puts_at(" Type 'man <command>' for details  |  Press any key ",
                    14,24,VGA_LIGHT_GREY,VGA_DARK_GREY);
        keyboard_waitchar();
        vga_clear();
        return;
    }
    for(int i=0;pages[i].cmd;i++){
        if(strcmp(pages[i].cmd,cmd)==0){
            man_render(&pages[i]);
            return;
        }
    }
    vga_printf("man: no manual entry for '%s'\n",cmd);
}
