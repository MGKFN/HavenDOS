/* ================================================================
   shell/cmddb.c  --  every shell command, described once.  v0.9.5.

   `help`, `man` and the desktop's Manual app all read this table.
   tools/check_help.py holds it to exec_cmd() in shell/shell.c:

     * every name and alias here must dispatch,
     * every dispatched name must be here (only `.` is exempt: it is
       pipe syntax, described under `source` and in `help shell`),
     * every summary fits CMDDB_SUMMARY_MAX,
     * every `see` names an entry that exists.

   The text describes what the code does TODAY. When a command
   changes, change its entry in the same commit -- the build will not
   let a new command in without one.

   Conventions
     synopsis   one usage form per line; <x> required, [x] optional,
                a|b alternatives.
     desc       plain sentences, wrapped by the renderer to the
                console's width. A blank line starts a paragraph.
     examples   "command\twhat it does", one per line.
   ================================================================ */
#include "cmddb.h"

extern int strcmp(const char*, const char*);

const cmdcat_t cmddb_cats[CDB_NCAT] = {
    { "files",    "Files and folders",
      "list, read, copy, move, edit and find files" },
    { "text",     "Text and data",
      "filters for pipes, encoders, checksums" },
    { "shell",    "Shell and scripting",
      "variables, aliases, scripts, history, pipes" },
    { "system",   "System",
      "status, display, themes, logs, power" },
    { "disks",    "Disks, install and update",
      "volumes, installing, updating, packages" },
    { "hardware", "Hardware",
      "PCI, USB, iPhone, I2C, graphics, speaker" },
    { "network",  "Network",
      "addresses, DNS, HTTP, TCP/UDP, TLS, chat" },
    { "users",    "Users and security",
      "accounts, passwords, file locks, the lock screen" },
    { "apps",     "Apps and fun",
      "games, screensavers, toys, small apps" },
};

const cmddoc_t cmddb[] = {

/* ── Files and folders ────────────────────────────────────────────── */

    { "ls", "dir", CDB_FILES,
      "ls",
      "List the current directory",
      "Lists the files and folders in the current directory. Folders are "
      "cyan, files green, and files locked to administrators red. The "
      "path you are in is printed above the list.\n\n"
      "ls takes no path: cd into a folder first, or use tree to see "
      "everything at once. In a pipe it prints one name per line, so "
      "`ls . grep TXT` works.",
      "ls\tlist where you are\n"
      "ls . grep .TXT\tonly the names containing .TXT",
      "cd tree stat find" },

    { "tree", NULL, CDB_FILES,
      "tree",
      "Show every file as a tree",
      "Draws the whole filesystem from the root as a tree and counts the "
      "items in it.",
      NULL,
      "ls find" },

    { "cd", NULL, CDB_FILES,
      "cd [dir]\ncd ..\ncd /\ncd ~",
      "Change the current directory",
      "Moves into a folder. With no argument, or with ~, it goes to your "
      "home folder; .. goes up one level and / to the root. Relative "
      "names are taken from where you are now.",
      "cd docs\tinto ./docs\n"
      "cd ..\tup one level\n"
      "cd\tback home",
      "pwd ls mkdir" },

    { "pwd", NULL, CDB_FILES,
      "pwd",
      "Print the current directory",
      "Prints the full path of the folder you are in. The prompt shows it "
      "too, unless you changed the prompt with ps1.",
      NULL,
      "cd ps1" },

    { "cat", "type", CDB_FILES,
      "cat <file>\n<cmd> . cat",
      "Print a file",
      "Prints a file to the screen (up to 4 KB). At the end of a pipe it "
      "prints whatever came through the pipe.",
      "cat notes.txt\tshow a file\n"
      "cat log.txt . grep error\tonly the lines with \"error\"",
      "head tail hex edit" },

    { "head", NULL, CDB_FILES,
      "head [-n] <file>\nhead <file> [n]",
      "First lines of a file (default 10)",
      "Prints the first n lines of a file, 10 if you do not say. Works "
      "as a pipe filter too.",
      "head readme.txt\tfirst 10 lines\n"
      "head -3 readme.txt\tfirst 3 lines\n"
      "dmesg . head 5\tfirst 5 lines of the kernel log",
      "tail cat" },

    { "tail", NULL, CDB_FILES,
      "tail [-n] <file>\ntail <file> [n]",
      "Last lines of a file (default 10)",
      "Prints the last n lines of a file, 10 if you do not say. Works as "
      "a pipe filter too.",
      "tail -20 log.txt\tlast 20 lines\n"
      "history . tail 5\tyour last 5 commands",
      "head cat" },

    { "write", NULL, CDB_FILES,
      "write <file> <text>",
      "Write a line of text to a file",
      "Replaces the file's contents with the text, creating it if it does "
      "not exist. To add to a file instead, use `echo text >> file`.",
      "write todo.txt buy milk\tcreate or overwrite\n"
      "echo more >> todo.txt\tappend a line",
      "touch echo edit" },

    { "touch", NULL, CDB_FILES,
      "touch <file>",
      "Create an empty file",
      "Creates an empty file if it does not exist. An existing file is "
      "left as it is.",
      NULL,
      "write mkdir" },

    { "mkdir", NULL, CDB_FILES,
      "mkdir <name>",
      "Create a folder",
      "Creates a folder in the current directory, or at the path given.",
      "mkdir projects\tcreate ./projects",
      "cd rm" },

    { "rm", "del", CDB_FILES,
      "rm <file>",
      "Delete a file",
      "Deletes a file. There is no recycle bin: it is gone.",
      NULL,
      "mv chmod" },

    { "mv", "rename", CDB_FILES,
      "mv <old> <new>",
      "Rename or move a file",
      "Renames a file, or moves it to another folder by giving a path as "
      "the new name.",
      "mv a.txt b.txt\trename\n"
      "mv a.txt docs/a.txt\tmove into docs",
      "cp rm" },

    { "cp", "copy", CDB_FILES,
      "cp <source> <dest>",
      "Copy a file",
      "Copies a file. An existing destination is overwritten. To copy "
      "onto a USB stick, use `fat16 cp`.",
      "cp notes.txt notes.bak\tmake a backup",
      "mv fat16" },

    { "find", NULL, CDB_FILES,
      "find <pattern>",
      "Find files by name (* wildcards)",
      "Searches the current folder and every folder below it for names "
      "that match, and prints their paths. * matches any run of "
      "characters and ? any one; case does not matter. A pattern with "
      "neither matches anywhere in a name. Start from / to search "
      "everything.",
      "find *.txt\tevery .txt file from here down\n"
      "find note\tanything with \"note\" in its name",
      "tree grep ls" },

    { "stat", NULL, CDB_FILES,
      "stat <file>",
      "Size, type and lock state of a file",
      "Shows whether a path is a file or a folder, its size in bytes, and "
      "whether it is locked to administrators.",
      NULL,
      "ls chmod hex" },

    { "hex", NULL, CDB_FILES,
      "hex <file>\nhex -s <text>",
      "Hex dump of a file or a string",
      "Prints a file as a classic hex dump: offset, sixteen bytes in hex, "
      "and the same bytes as ASCII. With -s it dumps the text you type "
      "instead of a file.",
      "hex /boot/grub.cfg\tdump a file\n"
      "hex -s Hello\tdump a string",
      "cat stat hash" },

    { "edit", "notepad", CDB_FILES,
      "edit [file]",
      "Open the text editor",
      "Opens the full-screen text editor, with the file loaded if you name "
      "one. Ctrl+S saves, Ctrl+W saves under a new name, Ctrl+Q quits. "
      "Ctrl+C, Ctrl+X and Ctrl+V copy, cut and paste; Ctrl+A selects all.",
      "edit notes.txt\topen or create notes.txt",
      "cat write" },

    { "zip", NULL, CDB_FILES,
      "zip <file> [dest.z]",
      "Compress a file",
      "Compresses one file with HavenDOS's own run-length format. The "
      "result is named file.z unless you give a name. It is not the PKZIP "
      "format: other systems cannot open it.",
      "zip log.txt\tmakes log.txt.z",
      "unzip" },

    { "unzip", NULL, CDB_FILES,
      "unzip <file.z> [dest]",
      "Expand a file made by zip",
      "Restores a file compressed with zip. Refuses anything that does not "
      "start with zip's ZR signature.",
      NULL,
      "zip" },

/* ── Text and data ────────────────────────────────────────────────── */

    { "grep", NULL, CDB_TEXT,
      "grep <pattern> <file>\n<cmd> . grep <pattern>",
      "Lines that contain a pattern",
      "Prints the lines of a file, or of a pipe, that contain the pattern. "
      "The match is plain text, not a regular expression.",
      "grep TODO notes.txt\tin a file\n"
      "dmesg . grep usb\tin the kernel log",
      "find sort wc" },

    { "wc", NULL, CDB_TEXT,
      "wc <file>\n<cmd> . wc",
      "Count lines, words and characters",
      "Counts the lines, words and characters of a file or a pipe.",
      "ls . wc\thow many entries are here",
      "grep" },

    { "sort", NULL, CDB_TEXT,
      "sort [-r] <file>\n<cmd> . sort",
      "Sort lines (reverse with -r)",
      "Prints a file's lines in alphabetical order, or reversed with -r. "
      "Works as a pipe filter too.",
      "sort -r names.txt\tZ to A",
      "uniq grep" },

    { "uniq", NULL, CDB_TEXT,
      "uniq <file>\n<cmd> . uniq",
      "Sort lines and drop duplicates",
      "Sorts the lines and prints each distinct line once.",
      NULL,
      "sort" },

    { "diff", NULL, CDB_TEXT,
      "diff <file1> <file2>",
      "Compare two files line by line",
      "Shows the lines that differ between two files, marked < for the "
      "first and > for the second.",
      NULL,
      "cat hash" },

    { "tee", NULL, CDB_TEXT,
      "<cmd> . tee <file>",
      "Show a pipe and save it to a file",
      "Passes a pipe through to the screen and writes a copy of it to a "
      "file at the same time.",
      "dmesg . tee boot.txt\tread the log and keep it",
      "write" },

    { "seq", NULL, CDB_TEXT,
      "seq <to>\nseq <from> <to>",
      "Print a run of numbers",
      "Prints the numbers from 1 (or from) to to, one per line, counting "
      "down if to is smaller. At most 200 at a time.",
      "seq 5\t1 to 5\n"
      "seq 10 1\t10 down to 1",
      "repeat" },

    { "base64", NULL, CDB_TEXT,
      "base64 <text>\nbase64 -d <base64>",
      "Encode or decode Base64",
      "Encodes the text as Base64, or decodes Base64 back to text with -d.",
      "base64 hello\taGVsbG8=\n"
      "base64 -d aGVsbG8=\thello",
      "hash hex" },

    { "hash", NULL, CDB_TEXT,
      "hash <text>\nhash -f <file>",
      "Fingerprint a string or a file",
      "Prints a 64-bit fingerprint (two djb2 hashes) of the text or of a "
      "file. Good for telling whether two files are the same; it is not "
      "a cryptographic hash.",
      "hash -f a.txt\tfingerprint a file",
      "diff base64" },

    { "yes", NULL, CDB_TEXT,
      "yes [text]",
      "Print a line over and over",
      "Prints the text (y if none) 200 times, or until ESC or Q. Useful "
      "for feeding a pipe.",
      NULL,
      "repeat seq" },

/* ── Shell and scripting ──────────────────────────────────────────── */

    { "help", "?", CDB_SHELL,
      "help\nhelp <topic>\nhelp <command>\nhelp all",
      "This reference",
      "With nothing after it, lists the topics. `help <topic>` lists the "
      "commands in one topic, `help <command>` gives one command's usage "
      "in a few lines, and `help all` lists every command.\n\n"
      "Topics: files, text, shell, system, disks, hardware, network, users "
      "and apps.",
      "help\tthe topics\n"
      "help network\tnetwork commands\n"
      "help cp\thow to use cp",
      "man guide" },

    { "man", NULL, CDB_SHELL,
      "man <command>\nman",
      "The full manual page for a command",
      "Opens a command's manual page full screen: what it does, every form "
      "it takes, examples and related commands. Up/Down and PgUp/PgDn "
      "scroll, Home/End jump, Q or ESC closes. With no command it lists "
      "every page.",
      "man ls\tthe page for ls",
      "help guide" },

    { "guide", NULL, CDB_SHELL,
      "guide [page]",
      "The user guide, ten pages",
      "A tour of HavenDOS in ten pages: welcome, the desktop, the shell, "
      "files and disks, scripts, Settings, accounts, the network, "
      "installing and updating, and what to do when something goes wrong. "
      "Left/Right turn pages, Up/Down scroll, ESC closes. A number opens "
      "on that page.",
      "guide\tfrom the start\n"
      "guide 9\tinstalling and updating",
      "help man" },

    { "echo", NULL, CDB_SHELL,
      "echo <text>",
      "Print text ($VARS are expanded)",
      "Prints its arguments. $NAME is replaced by the variable's value "
      "before the command runs, so echo is how you look at a variable. "
      "Add > file or >> file to write or append the line to a file.",
      "echo Hello\tprint Hello\n"
      "echo $OS $VER\tHavenDOS and the version\n"
      "echo done >> log.txt\tappend a line",
      "set env write" },

    { "set", "export", CDB_SHELL,
      "set <name> <value>",
      "Set a shell variable",
      "Stores a variable for $NAME expansion. If the value is arithmetic "
      "it is worked out first, so `set i $i+1` counts in a script. OS, "
      "VER, ARCH and PATH are set at startup.",
      "set NAME Kyan\tthen: echo $NAME\n"
      "set n 6*7\tn is 42",
      "unset env echo" },

    { "unset", NULL, CDB_SHELL,
      "unset <name>",
      "Remove a shell variable",
      "Forgets a variable set with set.",
      NULL,
      "set env" },

    { "env", NULL, CDB_SHELL,
      "env",
      "List shell variables",
      "Prints every variable and its value.",
      NULL,
      "set unset" },

    { "alias", NULL, CDB_SHELL,
      "alias\nalias <name>=<command>",
      "List or create command shortcuts",
      "With nothing after it, lists the aliases. alias name=command makes "
      "name run command; anything typed after the alias is passed along. "
      "Aliases are saved in ETC/ALIASES.TXT and come back after a reboot.\n\n"
      "Built in: ll runs ls, q exits, ? is help and g is guide.",
      "alias la=ls\tla now lists\n"
      "alias up=cd ..\tup goes up a folder",
      "unalias which" },

    { "unalias", NULL, CDB_SHELL,
      "unalias <name>",
      "Remove an alias",
      "Deletes an alias and saves the change.",
      NULL,
      "alias" },

    { "history", NULL, CDB_SHELL,
      "history",
      "Your recent commands",
      "Lists the last 50 commands with their numbers. Up and Down at the "
      "prompt walk through the same list.",
      NULL,
      "repeat" },

    { "which", NULL, CDB_SHELL,
      "which <command>",
      "Is this a command, an alias or a script?",
      "Says whether a name is a built-in command, an alias (and what for), "
      "or a .BSH script in the root folder.",
      "which ll\tll is aliased to ls",
      "alias help" },

    { "run", NULL, CDB_SHELL,
      "run <script.bsh>",
      "Run a shell script",
      "Runs a .BSH script: a text file of shell commands, one per line. "
      "Scripts understand variables, if/else/endif, while/endwhile and "
      "input <name> to read a line from the keyboard; lines starting with "
      "# are comments. The guide has a worked example.",
      "run ~/hello.bsh\trun a script from home",
      "source script set" },

    { "source", NULL, CDB_SHELL,
      "source <script.bsh>\n. <script.bsh>",
      "Run a script in this shell",
      "The same as run; `. file` is the short spelling. Variables a "
      "script sets stay set afterwards either way.\n\n"
      "A dot between two commands is something else: it is the pipe. "
      "`ls . grep TXT` sends ls's output into grep.",
      NULL,
      "run" },

    { "script", NULL, CDB_SHELL,
      "script [file]",
      "Record a shell session to a file",
      "Starts a recording prompt ([rec]$). Every command and what it "
      "printed is saved to the file, SCRIPT.LOG if you do not name one. "
      "exit or ESC stops recording.",
      NULL,
      "history run" },

    { "repeat", NULL, CDB_SHELL,
      "repeat <n> <command>",
      "Run a command n times (1-64)",
      "Runs the rest of the line n times in a row.",
      "repeat 3 beep\tthree beeps",
      "watch seq" },

    { "watch", NULL, CDB_SHELL,
      "watch <seconds> <command>",
      "Re-run a command every few seconds",
      "Runs the command again every so many seconds, numbering each "
      "run, until ESC or Q.",
      "watch 2 mem\tmemory, every 2 seconds",
      "repeat timer" },

    { "timer", NULL, CDB_SHELL,
      "timer <command> [args]",
      "Time how long a command takes",
      "Runs the command and prints how long it took, to the second (the "
      "clock it reads is the RTC).",
      "timer selftest\thow long the self-test takes",
      "time watch" },

    { "calc", NULL, CDB_SHELL,
      "calc <expression>",
      "Integer calculator",
      "Works out + - * / % and ^ (power) on whole numbers, with the usual "
      "precedence: ^ first, then * / %, then + -. Spaces are optional. A "
      "bad expression says where it went wrong.",
      "calc 2+3*4\t14\n"
      "calc 2^10\t1024",
      "set" },

    { "ps1", NULL, CDB_SHELL,
      "ps1\nps1 <format>",
      "Show or change the prompt",
      "With nothing after it, shows the prompt format and its tokens. "
      "\\u is your user name, \\h the host, \\w the folder, \\$ is # for an "
      "administrator and $ otherwise, \\t the time, \\d the date and \\n "
      "a new line.",
      "ps1 \\u@\\h:\\w\\$ \tkyan@havendos:/home$",
      "color" },

    { "color", NULL, CDB_SHELL,
      "color <1-15>\ncolor reset",
      "The shell's text colour, saved",
      "Sets the colour commands print in, and keeps it across commands "
      "and reboots. 1 blue, 2 green, 3 cyan, 4 red, 5 magenta, 6 brown, "
      "7 light grey, 8 dark grey, 9-14 the bright versions, 15 white. "
      "reset goes back to white. The background stays black: most "
      "commands draw their own colours on black.",
      "color 10\tgreen text\n"
      "color reset\tback to white",
      "theme ps1" },

    { "cls", "clear", CDB_SHELL,
      "cls",
      "Clear the screen",
      "Clears the screen and puts the prompt at the top.",
      NULL,
      NULL },

    { "exit", "quit", CDB_SHELL,
      "exit",
      "Leave the shell for the desktop",
      "Returns to the desktop. q is a built-in alias for it.",
      NULL,
      "shutdown reboot" },

    { "h", "hplusplus boot", CDB_SHELL,
      "h\nh <file.h>",
      "The H programming language",
      "Runs a program written in H, HavenDOS's own scripting language, "
      "or opens its interactive prompt with no file. H has var, if/then/"
      "else/end, while/do, input, println and a small library (abs, len, "
      "str).\n\n"
      "It is a package: `bpkg install h-lang` first. HavenCode on the "
      "desktop is its editor.",
      "h\tthe H prompt\n"
      "h ~/count.h\trun a program",
      "bpkg run" },

/* ── System ───────────────────────────────────────────────────────── */

    { "system", "dashboard htop sysinfo sysinfo-plus sysmon", CDB_SYSTEM,
      "system [overview|memory|storage|network|devices]",
      "The System app: status at a glance",
      "One app for everything that used to be Dashboard, System Monitor, "
      "System Info and Info+. Tabs: Overview (version, CPU, memory, "
      "uptime, display), Memory, Storage, Network and Devices. Left/Right "
      "or Tab switches tabs, R refreshes, ESC closes. It lays itself out "
      "for the console it is given, from 64x24 to 240x135.\n\n"
      "Naming a tab opens straight on it.",
      "system\tthe overview\n"
      "system network\tthe network tab",
      "neofetch mem disk ifconfig" },

    { "open", NULL, CDB_SYSTEM,
      "open\nopen <app>",
      "Open a desktop app from the shell",
      "Opens any app from the start menu by its id or by the start of its "
      "name, and comes back to the shell when it closes. With nothing after "
      "it, lists every app and its id.",
      "open calc\tthe Calculator\n"
      "open text editor\tby name\n"
      "open 2048\ta game",
      "system settings" },

    { "settings", NULL, CDB_SYSTEM,
      "settings [section]",
      "The Settings app",
      "Every setting in one place, each saved the moment you change it. "
      "Appearance: theme with a live preview, wallpaper, the desktop info "
      "strip. Display: console size. Desktop: which apps have icons and "
      "which are favourites. Shell: prompt and text colour. Accounts: "
      "passwords, roles, new and deleted accounts, auto-login. Network: "
      "DHCP, a manual address, DNS. Startup: boot log, screensaver, "
      "hostname. About.\n\n"
      "Naming a section opens straight on it.",
      "settings\tthe whole app\n"
      "settings accounts\tstraight to Accounts",
      "theme resolution system" },

    { "ver", "version", CDB_SYSTEM,
      "ver",
      "Version of HavenDOS",
      "Prints the HavenDOS version and its codename.",
      NULL,
      "neofetch system" },

    { "neofetch", NULL, CDB_SYSTEM,
      "neofetch",
      "System summary with a logo",
      "Prints the logo beside a summary: version, kernel, uptime, user, "
      "console size, memory and disk. Good for screenshots.",
      NULL,
      "system ver" },

    { "screenshot", NULL, CDB_SYSTEM,
      "screenshot [file]",
      "Save the screen as a PNG",
      /* bus-label-ok: "a USB stick" is the thing you carry to another PC, not a disk's bus label */
      "The same as the PrtSc key: the whole screen, as a PNG. With a FAT32 "
      "USB stick in, it goes to Pictures on the stick -- take the stick "
      "to another computer to send it. Otherwise to Pictures on the "
      "system disk (RAM on a live boot). Give a file name to choose.",
      NULL,
      "perf" },

    { "python", "py", CDB_SYSTEM,
      "python [file.py]",
      "Python (MicroPython)",
      "With no file, the Python prompt: type Python and it runs, line by "
      "line. Ctrl+D on an empty line goes back to the shell, Ctrl+C stops a "
      "program that is running. With a file, runs it. import finds .py files "
      "in the current folder. math, random, time and sys are built in; "
      "open() and files from Python are not, yet.",
      NULL,
      "edit" },

    { "perf", NULL, CDB_SYSTEM,
      "perf [seconds|bench]",
      "How fast the screen is drawn",
      "perf 10 watches for ten seconds while you use the desktop, then says "
      "how much was drawn and how long it took. perf bench draws a fixed set "
      "of things (a window moved, the Terminal scrolled, the whole screen) "
      "and times each, so two machines or two versions can be compared.",
      NULL,
      "screenshot sysmon" },

    { "mem", NULL, CDB_SYSTEM,
      "mem",
      "Free and used memory",
      "Prints free, used and total physical memory in KB.",
      NULL,
      "system df" },

    { "mouse", NULL, CDB_HW,
      "mouse",
      "Where the pointer is and what drives it",
      "Prints the pointer's position and buttons, and how many USB pointers "
      "and trackpads are driving it -- after a mouse is plugged back in, or "
      "to see whether the trackpad was found.",
      "mouse\tthe position, the buttons, and the devices",
      "usb i2c dmesg" },
    { "uptime", NULL, CDB_SYSTEM,
      "uptime",
      "How long since boot",
      "Prints the time since boot as hours:minutes:seconds.",
      NULL,
      "date system" },

    { "date", NULL, CDB_SYSTEM,
      "date",
      "Today's date",
      "Prints the date from the machine's real-time clock.",
      NULL,
      "time uptime" },

    { "time", NULL, CDB_SYSTEM,
      "time",
      "The time",
      "Prints the time from the machine's real-time clock. To time a "
      "command, use timer.",
      NULL,
      "date timer" },

    { "ps", "jobs", CDB_SYSTEM,
      "ps",
      "Open apps, jobs and the desktop",
      "Lists every task: the desktop, each open app and each job, most "
      "recently used first. STATE is \"in front\" for the one you are "
      "using, \"running\" for one that carries on behind it, and "
      "\"paused\" for one waiting to be brought back. STACK is how much "
      "of its own stack it has used.\n\n"
      "Alt+Tab switches between them. Tasks are off in safe mode and "
      "with `notasks` on the boot line.",
      "ps\tlist what is open",
      "fg kill system" },

    { "fg", NULL, CDB_SYSTEM,
      "fg <id>\nfg <name>",
      "Bring an open app or job forward",
      "Brings an open task to the front, as Alt+Tab does, by the id `ps` "
      "shows or the start of its name. The shell stays open behind it; "
      "Alt+Tab comes back.",
      "fg 3\tshow job 3\nfg notes\tback to Notes",
      "ps kill" },

    { "kill", NULL, CDB_SYSTEM,
      "kill <id>\nkill <name>",
      "Close an open app or job",
      "Closes a task that is behind the front, by the id `ps` shows or "
      "the start of its name. Use it on an app that is stuck, or a job "
      "you no longer want. It is stopped where it was waiting; anything "
      "it had allocated stays allocated until the next reboot. The "
      "desktop and the task you type it in cannot be closed this way.",
      "kill 4\tclose task 4",
      "ps fg" },

    { "dmesg", NULL, CDB_SYSTEM,
      "dmesg\ndmesg . grep <word>",
      "The kernel log",
      "Pages through everything the kernel logged since boot: drivers, "
      "disks, the network, the display. Any key for more, Q stops. Pipe "
      "it into grep to find one driver's lines.",
      "dmesg . grep nvme\tjust the NVMe lines",
      "selftest log" },

    { "theme", NULL, CDB_SYSTEM,
      "theme\ntheme <n>",
      "List or switch colour themes",
      "With nothing after it, lists the themes with a colour sample. A "
      "number switches to that theme straight away and saves it. Every "
      "theme has its own desktop pattern. Settings > Appearance does the "
      "same with a preview.",
      "theme\tthe list\n"
      "theme 3\tswitch to theme 3",
      "settings color" },

    { "font", NULL, CDB_SYSTEM,
      "font\nfont smooth|classic",
      "Smooth or classic console font",
      "smooth (the default since 0.9.7.10) draws every letter in IBM Plex "
      "Mono, the Tech Haven Studios face, with soft edges, sized to the "
      "screen. classic is the 8x16 bitmap font of earlier releases, scaled "
      "up. The choice is saved. On a small grid, or on real VGA text, the "
      "classic font is used either way.",
      "font\twhich one is in use\n"
      "font classic\tthe old blocky font",
      "theme resolution" },

    { "resolution", "res", CDB_SYSTEM,
      "resolution\nresolution fit|max|reset\nresolution <cols>x<rows>",
      "Console size in characters",
      "With nothing after it, shows the current grid and what the screen "
      "can do. On a framebuffer any grid up to 240x135 fits: fit picks "
      "readable text, max the most cells, reset goes back to 80x25. On a "
      "VGA text screen the choices are 80x25, 80x50, 90x30 and 90x60. "
      "The size you pick is saved.",
      "resolution fit\trecommended\n"
      "resolution 160x50\ta size of your own",
      "settings layout" },

    { "screensaver", NULL, CDB_SYSTEM,
      "screensaver\nscreensaver <seconds>\nscreensaver off",
      "Screensaver timeout",
      "Shows the timeout, sets it (5 to 3600 seconds) or turns the "
      "screensaver off.",
      "screensaver 300\tafter 5 minutes",
      "matrix stars pipes" },

    { "motd", NULL, CDB_SYSTEM,
      "motd\nmotd set <message>",
      "Message of the day",
      "Shows the message of the day, or sets it. It is saved in "
      "ETC/MOTD.TXT.",
      NULL,
      "notify" },

    { "notify", NULL, CDB_SYSTEM,
      "notify <info|success|warn|error> <message>",
      "Pop up a notification",
      "Shows a notification toast in the corner, in the colour of its "
      "type. Handy at the end of a long script.",
      "notify success Backup done\ta green toast",
      "motd" },

    { "selftest", NULL, CDB_SYSTEM,
      "selftest",
      "Run the built-in checks",
      "Runs HavenDOS's offline self-checks (string handling, parsers, "
      "maths, the filesystem) and prints passed and failed counts. No "
      "network needed.",
      NULL,
      "dmesg crash" },

    { "layout", NULL, CDB_SYSTEM,
      "layout",
      "Check that every app fits the screen",
      "Opens every desktop app in turn and reports any that drew outside "
      "its console. The screen flickers while it runs.",
      NULL,
      "resolution" },

    { "kpoll", NULL, CDB_SYSTEM,
      "kpoll\nkpoll on|off",
      "Background polling: status, on, off",
      "Shows the periodic jobs the kernel runs while you type: DHCP "
      "retries, USB plug-in checks and the like, how often each ran and "
      "why any were held back. off stops them; on starts them again.",
      NULL,
      "ps dmesg" },

    { "crash", NULL, CDB_SYSTEM,
      "crash where\ncrash div|null|opcode|wild|stack",
      "Test the crash guard on purpose",
      "Causes a real CPU fault to prove HavenDOS recovers from it: div "
      "divides by zero, null writes through a null pointer, opcode runs "
      "an invalid instruction, wild calls a bad address. where only says "
      "whether the guard is armed.\n\n"
      "stack cannot be recovered from: it needs a reboot.",
      NULL,
      "selftest" },

    { "reboot", NULL, CDB_SYSTEM,
      "reboot",
      "Restart the machine",
      "Restarts straight away.",
      NULL,
      "shutdown" },

    { "shutdown", "poweroff", CDB_SYSTEM,
      "shutdown",
      "Turn the machine off",
      "Powers the machine off through ACPI, and says so on screen if the "
      "firmware would not let it.",
      NULL,
      "reboot lock" },

/* ── Disks, install and update ────────────────────────────────────── */

    { "disk", NULL, CDB_DISKS,
      "disk",
      "Disks, partitions and the system volume",
      "Lists every disk HavenDOS can see (NVMe, AHCI/SATA, IDE, VirtIO, "
      "USB), what is on each, and which volume the system is running "
      "from. Disks that are not HavenDOS's are read and never written.",
      NULL,
      "df install fat32" },

    { "df", NULL, CDB_DISKS,
      "df",
      "Space on the system volume",
      "Shows the system volume's filesystem, size, used and free space.",
      NULL,
      "disk mem" },

    { "install", NULL, CDB_DISKS,
      "install",
      "Install HavenDOS on a disk",
      "Starts the installer. Beside another system it adds a HavenDOS "
      "partition and a boot entry and touches nothing else; on an empty "
      "or chosen disk it can use the whole disk. Volumes up to 2 GB are "
      "FAT16, bigger ones FAT32 (to 32 GB).\n\n"
      "From an installed system it installs this system onto another "
      "disk. Nothing is written until you type the confirmation word.",
      NULL,
      "uninstall disk update" },

    { "migrate", NULL, CDB_DISKS,
      "migrate\nmigrate <n>\nmigrate finish",
      "Move this install from FAT16 to FAT32",
      "FAT16 stops at 2 GB. `migrate` lists the places on this disk a new "
      "FAT32 install can go -- unallocated space, or an empty partition "
      "you made for it (never one with a filesystem) -- and `migrate <n>` "
      "makes it there, copies your users, passwords, settings and files, "
      "and adds it to the boot menu as the first HavenDOS entry. The old "
      "install stays in the menu. Boot the new one, check your files, then "
      "`migrate finish` deletes the old partition and its menu entry.",
      "migrate\tsee where it can go\nmigrate 1\tmove to place 1\n"
      "migrate finish\tremove the old FAT16 install",
      "install uninstall df" },

    { "uninstall", NULL, CDB_DISKS,
      "uninstall",
      "Remove HavenDOS from a disk",
      "Previews exactly what it would undo -- the boot loader it would "
      "restore, the folders and partition entries it would remove -- and "
      "only does it after you type the confirmation word. Data in the "
      "partition is not erased, only its table entry.",
      NULL,
      "install" },

    { "update", NULL, CDB_DISKS,
      "update",
      "Install a newer HavenDOS kernel",
      "Looks for a new kernel and its update.inf in /EFI/HAVENDOS/update "
      "on a USB stick or the EFI System Partition, or on an update CD, "
      "checks it, and replaces the installed kernel. The old one is kept "
      "as /boot/havendos.bak. The release zip has scripts that put the "
      "files in place from Windows or Linux. `update channel` shows which "
      "channel this system follows (Beta, Public Beta, Release Preview, "
      "Stable); `update channel stable` changes it -- an update on a less "
      "stable channel is then refused, by name.",
      "update\tinstall a newer kernel from a stick, the ESP or a CD\n"
      "update channel\tshow which channel this system follows\n"
      "update channel stable\ttake only Stable updates from now on",
      "rollback install" },

    { "rollback", NULL, CDB_DISKS,
      "rollback",
      "Go back to the previous kernel",
      "Puts back the kernel that update saved as /boot/havendos.bak, after "
      "you confirm. Reboot to use it.",
      NULL,
      "update" },

    { "bpkg", NULL, CDB_DISKS,
      "bpkg list\nbpkg install|remove|info <id>\nbpkg update\n"
      "bpkg server <url>",
      "The package manager",
      "list shows the packages and which are installed. install and "
      "remove add or take away one; info describes it. update fetches "
      "the package index from the server, and server changes where that "
      "is.",
      "bpkg list\twhat there is\n"
      "bpkg install h-lang\tthe H language",
      "h" },

    { "fat32", NULL, CDB_DISKS,
      "fat32\nfat32 ls <n>[:dir]\nfat32 cat <n>:<path>",
      "Look inside FAT32 volumes",
      "With nothing after it, lists every FAT32 volume found, numbered "
      "from 0, and how each was found. ls and cat read a folder or file "
      "on volume n. Read-only.",
      "fat32 ls 0:EFI\tlist a folder on volume 0",
      "fat16 exfat disk" },

    { "fat16", NULL, CDB_DISKS,
      "fat16\nfat16 ls <n>\nfat16 cat <n>:<path>\nfat16 cp <src> <n>:<path>",
      "FAT16 volumes and USB sticks",
      "Lists FAT16 volumes, reads their root folder and files, and copies "
      "a file onto removable media -- the only command that writes to a "
      "stick.",
      "fat16 cp notes.txt 0:NOTES.TXT\tcopy onto volume 0",
      "fat32 exfat" },

    { "exfat", NULL, CDB_DISKS,
      "exfat\nexfat ls <n>[:dir]\nexfat cat <n>:<path>",
      "Look inside exFAT volumes",
      "Lists exFAT volumes -- most big USB sticks -- and reads their "
      "folders and files. Read-only.",
      NULL,
      "fat32 fat16" },

/* ── Hardware ─────────────────────────────────────────────────────── */

    { "lspci", NULL, CDB_HW,
      "lspci\nlspci -vv",
      "PCI devices",
      "Lists every PCI device with its vendor and device IDs and what it "
      "is. -vv adds each device's BARs and interrupt.",
      NULL,
      "usb system" },

    /* bus-label-ok: this entry documents the usb command, which is about
       the USB bus itself, not a label on a disk. */
    { "usb", NULL, CDB_HW,
      "usb [-v|-vv]\nusb rescan\nusb cfg [addr]\nusb eth\nusb mux",
      "USB controllers and devices",
      "Lists the USB controllers and what is plugged into them. -v and -vv "
      "show more, down to empty ports. rescan walks the bus again; cfg "
      "dumps a device's configurations; eth shows USB network adapters "
      "(including a tethered iPhone) and mux the iPhone's data channel.",
      "usb\twhat is plugged in\n"
      "usb eth\tUSB network adapters",
      "lspci pair lockdown" },

    { "lockdown", NULL, CDB_HW,
      "lockdown",
      "Ask a plugged-in iPhone what it is",
      "Talks to the iPhone's lockdown service over USB and prints what "
      "it says it is. The same as usb lockdown; pair goes further.",
      NULL,
      "pair usb" },

    { "pair", NULL, CDB_HW,
      "pair\npair session|show|forget",
      "Pair with an iPhone",
      "Asks a plugged-in, unlocked iPhone to trust this computer; tap "
      "Trust on the phone and run pair again. The pairing record is "
      "stored, so later sessions skip the key generation. session reuses "
      "it, show prints it, forget deletes it.",
      NULL,
      "lockdown rsa usb" },

    { "rsa", NULL, CDB_HW,
      "rsa keygen [bits]\nrsa show|verify\nrsa prime <n>",
      "RSA keys, as used for pairing",
      "Generates an RSA key (2048 bits unless you say), shows or re-checks "
      "the last one, or tests whether a number is prime. Mostly here to "
      "measure how long key generation takes on this machine.",
      NULL,
      "pair" },

    { "i2c", NULL, CDB_HW,
      "i2c\ni2c scan [bus]",
      "I2C controllers and devices",
      "Lists the I2C controllers (on laptops these carry the touchpad), or "
      "scans one or all buses for addresses that answer.",
      NULL,
      "i2chid" },

    { "i2chid", NULL, CDB_HW,
      "i2chid",
      "I2C touchpads and pointers",
      "Shows each I2C-HID device found at boot, what its report says it "
      "is, and which one is driving the mouse cursor.",
      NULL,
      "i2c" },

    { "gfx", NULL, CDB_HW,
      "gfx\ngfx regs|vbt|edid|plan|b\ngfx on|off\ngfx color <colour>\ngfx try\ngfx fit [keep|stretch|centre]\ngfx auto [on|off]",
      "Intel display engine tools",
      "Looks at the Intel display engine as the firmware left it: regs "
      "dumps its registers, vbt reads the Video BIOS Table, edid reads "
      "the monitor's EDID, plan says what gfx on would do. on lights the "
      "HDMI output after asking, off darkens it again. fit chooses how "
      "the picture fills the monitor (shape kept, stretched, or a box in "
      "the middle) and auto whether a monitor plugged in lights by "
      "itself; both are in Settings > Display too. try tests changes to "
      "the picture layer one at a time. Everything else is read-only.",
      NULL,
      "resolution" },

    { "beep", NULL, CDB_HW,
      "beep [hz] [ms]",
      "Sound the PC speaker",
      "Beeps at the frequency and length given, 440 Hz for 200 ms if not.",
      "beep 880 100\ta short high beep",
      "play music" },

    { "play", NULL, CDB_HW,
      "play",
      "Play the startup jingle",
      "Plays HavenDOS's startup tune on the PC speaker.",
      NULL,
      "beep music" },

/* ── Network ──────────────────────────────────────────────────────── */

    { "ifconfig", NULL, CDB_NET,
      "ifconfig\nifconfig dhcp\nifconfig set <ip> <gateway>\n"
      "ifconfig dns <ip>",
      "Network address and settings",
      "Shows the network card, its address, gateway, DNS server and "
      "whether the address came from DHCP, was set by hand, or is only the "
      "built-in fallback (which does not route). dhcp asks the network "
      "for an address; set and dns set them by hand.",
      "ifconfig dhcp\tget an address",
      "ping netstat usb" },

    { "ping", NULL, CDB_NET,
      "ping <host|ip>",
      "Check that a host answers",
      "Sends four pings and prints each reply time, or why there was "
      "none.",
      "ping 1.1.1.1\tis the internet there?",
      "ifconfig nslookup" },

    { "nslookup", "host", CDB_NET,
      "nslookup <name>\nnslookup -cache\nnslookup -flush",
      "Look up a name's addresses",
      "Asks the DNS server for every address of a name. -cache shows the "
      "resolver's cache with time left on each entry; -flush empties it.",
      "nslookup example.com\tits addresses",
      "dns ping" },

    { "dns", NULL, CDB_NET,
      "dns <name>",
      "Resolve a name to one address",
      "The short form of nslookup: prints the first address only.",
      NULL,
      "nslookup" },

    { "wget", NULL, CDB_NET,
      "wget <url> [file]",
      "Download a URL to a file",
      "Fetches a URL over HTTP or HTTPS and saves it, as the file name "
      "given or index.html.",
      "wget http://example.com/ page.htm\tsave a page",
      "curl tls" },

    { "curl", NULL, CDB_NET,
      "curl <url>",
      "Fetch a URL and print it",
      "Fetches a URL over HTTP or HTTPS and prints the response.",
      NULL,
      "wget tls" },

    { "tls", "tlsinfo", CDB_NET,
      "tls",
      "What the last HTTPS handshake did",
      "Shows how far the last TLS handshake got, the cipher suite each "
      "side chose and any alert. HavenDOS speaks TLS 1.2 with RSA key "
      "exchange; a server that insists on ECDHE or TLS 1.3 refuses it, "
      "and this says so.",
      NULL,
      "curl wget" },

    { "netstat", NULL, CDB_NET,
      "netstat\nnetstat -s\nnetstat -z",
      "Connections and network counters",
      "Shows the interface, its address and whether the link is up. -s "
      "prints the stack's packet and connection counters; -z resets "
      "them.",
      NULL,
      "ifconfig" },

    { "tcptest", NULL, CDB_NET,
      "tcptest <host|ip> <port>",
      "Test a TCP port",
      "Connects to a port, sends an HTTP HEAD request and prints whatever "
      "comes back.",
      "tcptest 10.0.2.2 80\tis a web server up?",
      "tcpsend udpsend" },

    { "tcpsend", NULL, CDB_NET,
      "tcpsend <host|ip> <port>",
      "Type into a TCP connection",
      "Opens a connection and sends what you type; an empty line sends, "
      "ESC hangs up. Replies are printed as they arrive.",
      NULL,
      "tcptest" },

    { "udpsend", NULL, CDB_NET,
      "udpsend <ip> <port> <message>",
      "Send one UDP packet",
      "Sends a single UDP datagram with the message in it.",
      NULL,
      "tcpsend" },

    { "chat", NULL, CDB_NET,
      "chat <user>@<host>[:port]",
      "BOOT Chat client (the service is closed)",
      "The BOOT Chat service is shut down until further notice; IRC, "
      "Haven News and LAN chat replace it in 27H2. The client still "
      "connects to a server you run yourself: "
      "it connects (port 6700 unless given) and asks "
      "whether to log in or register. Inside: TAB for the server and "
      "channel sidebar, /help for commands, /quit to leave. From a QEMU "
      "guest the host machine is 10.0.2.2.",
      "chat kyan@10.0.2.2\ton the host machine",
      "ifconfig" },

/* ── Users and security ───────────────────────────────────────────── */

    { "whoami", NULL, CDB_USERS,
      "whoami",
      "Who you are logged in as",
      "Prints your user name and whether you are an administrator.",
      NULL,
      "users" },

    { "users", NULL, CDB_USERS,
      "users\nusers <name>",
      "Accounts on this machine",
      "Lists every account: its role, home folder, whether it has a "
      "password, and when it last logged in. You are marked with *. A "
      "name shows that one account in full, with its recent logins.\n\n"
      "Settings > Accounts manages them with a menu.",
      "users\tevery account\n"
      "users kyan\tone account",
      "adduser passwd log whoami" },

    { "adduser", NULL, CDB_USERS,
      "adduser <name> <password>",
      "Create an account (admin)",
      "Creates a standard account and its home folder, HOME/<name>. "
      "Administrators only.",
      NULL,
      "deluser passwd users" },

    { "deluser", NULL, CDB_USERS,
      "deluser <name>",
      "Delete an account (admin)",
      "Deletes an account. Administrators only; root cannot be deleted.",
      NULL,
      "adduser users" },

    { "passwd", NULL, CDB_USERS,
      "passwd [user]",
      "Change a password",
      "Changes your password, asking for the current one first. An "
      "administrator can change anyone's without it.",
      NULL,
      "users lock" },

    { "chmod", NULL, CDB_USERS,
      "chmod <file> lock|unlock",
      "Lock a file to admins (RAM filesystem)",
      "lock makes a file readable and deletable by administrators only; "
      "unlock opens it to everyone again. ls shows locked files in red.\n\n"
      "Locks are kept by the RAM filesystem a live session uses. FAT16 and "
      "FAT32 have nowhere to store them, so on an installed system chmod "
      "says so and changes nothing.",
      NULL,
      "chown stat" },

    { "chown", NULL, CDB_USERS,
      "chown <file> <user>",
      "Give a file to a user (RAM filesystem)",
      "Changes which user owns a file. Administrators only, and like "
      "chmod, only on the live session's RAM filesystem.",
      NULL,
      "chmod" },

    { "log", NULL, CDB_USERS,
      "log",
      "The login log (admin)",
      "Prints every login and failed login with its time. "
      "Administrators only.",
      NULL,
      "users" },

    { "lock", NULL, CDB_USERS,
      "lock",
      "Lock the screen",
      "Locks the screen until your password is typed. A wrong password "
      "locks it for three seconds.",
      NULL,
      "passwd" },

/* ── Apps and fun ─────────────────────────────────────────────────── */

    { "snake", NULL, CDB_APPS,
      "snake",
      "Snake",
      "The classic: arrow keys steer, eat to grow, do not hit the wall or "
      "yourself. Q quits.",
      NULL,
      NULL },

    { "matrix", NULL, CDB_APPS,
      "matrix",
      "Falling green code",
      "The digital-rain screensaver. Any key stops it.",
      NULL,
      "stars pipes screensaver" },

    { "stars", NULL, CDB_APPS,
      "stars",
      "Starfield",
      "Fly through a starfield. Any key stops it.",
      NULL,
      "matrix pipes" },

    { "pipes", NULL, CDB_APPS,
      "pipes",
      "Pipes screensaver",
      "Pipes grow across the screen. Any key stops it.",
      NULL,
      "matrix stars" },

    { "fortune", NULL, CDB_APPS,
      "fortune",
      "A random saying",
      "Prints a random fortune.",
      NULL,
      "cowsay" },

    { "cowsay", NULL, CDB_APPS,
      "cowsay [text]",
      "A cow says your text",
      "Draws a cow with your text in a speech bubble.",
      "cowsay Hello\tmoo",
      "fortune banner" },

    { "banner", NULL, CDB_APPS,
      "banner <text>",
      "Text in big letters",
      "Prints a short text in large block letters.",
      NULL,
      "ascii-art cowsay" },

    { "ascii-art", NULL, CDB_APPS,
      "ascii-art <text>",
      "Your text as big ASCII lettering",
      "Draws your text (up to 12 characters) in large ASCII lettering.",
      NULL,
      "banner" },

    { "paint", NULL, CDB_APPS,
      "paint",
      "Draw with characters",
      "A character-cell paint program: move with the arrow keys and draw "
      "with # * . + - | @ % 0. S saves to HOME/PAINT.TXT.",
      NULL,
      NULL },

    { "music", NULL, CDB_APPS,
      "music",
      "Tunes on the PC speaker",
      "Pick a song by number and it plays on the PC speaker. Any key "
      "stops it.",
      NULL,
      "play beep" },

    { "calendar", NULL, CDB_APPS,
      "calendar",
      "This month's calendar",
      "Shows the current month from the real-time clock.",
      NULL,
      "date" },

    { "quiz", NULL, CDB_APPS,
      "quiz",
      "Eight questions about computers",
      "A multiple-choice quiz: answer A to D, get a score at the end.",
      NULL,
      NULL },

    { "hacker", NULL, CDB_APPS,
      "hacker",
      "Movie-style hacking, for show",
      "Prints a film-style hacking sequence. It does nothing at all to "
      "any network; it is a screensaver with a plot.",
      NULL,
      "matrix" },

    { "2048", NULL, CDB_APPS,
      "2048",
      "Slide and merge tiles to reach 2048",
      "Arrow keys slide every tile on the board; two tiles with the same "
      "number merge into one with their sum. Reach 2048 to win, and keep "
      "going if you like. R starts a new game, Q quits.",
      NULL,
      "breakout memory" },

    { "breakout", NULL, CDB_APPS,
      "breakout",
      "Knock out a wall of bricks",
      "Space launches the ball; Left and Right move the paddle. Where the "
      "ball hits the paddle decides its angle. Three lives; a cleared wall "
      "comes back faster. Q quits.",
      NULL,
      "2048 snake" },

    { "tictactoe", "ttt", CDB_APPS,
      "tictactoe",
      "Tic-Tac-Toe against the computer",
      "You are X. Arrow keys choose a square and Enter places your mark. "
      "D switches the computer between easy (it makes mistakes) and "
      "perfect (it never loses). The score is kept until you quit.",
      NULL,
      "memory" },

    { "memory", NULL, CDB_APPS,
      "memory",
      "Find the eight pairs of cards",
      "Sixteen cards, face down. Turn two over with Enter; a pair stays "
      "up, anything else turns back. Fewest moves wins -- your best is "
      "kept until you quit.",
      NULL,
      "tictactoe 2048" },

    { "stopwatch", NULL, CDB_APPS,
      "stopwatch",
      "A stopwatch with laps, and a timer",
      "Big digits, to the hundredth of a second. Space starts and stops, L "
      "records a lap (up to eight), R resets. Tab switches to the timer: "
      "Up and Down set the minutes, Space starts it, and it beeps when "
      "it runs out.",
      NULL,
      "timer time" },

    { "convert", NULL, CDB_APPS,
      "convert",
      "Convert lengths, weights, temperatures...",
      "Pick a kind of quantity -- length, weight, temperature, data size, "
      "speed or time -- and a unit, type a number, and every other unit "
      "is shown at once. Tab moves between the fields.",
      NULL,
      "calc" },

    { "retro", NULL, CDB_APPS,
      "retro [file]",
      "Run the old builds on an emulated PC",
      "Retro PC: an 80186 with the 386's 16-bit additions, a BIOS, a floppy "
      "and a hard disk of its own, in a window. K-DOS Beta 1.1b, the first "
      "build that survived, is built in (log in as admin, password 1234). "
      "With no file it lists the built-in images and any .IMG in RETRO; "
      "with a file it boots that one. F4 or the window's X leaves. The old "
      "PC's hard disk is a .HDD file, saved when you leave -- never a real "
      "disk.",
      "retro\tthe list\n"
      "retro RETRO/OLD.IMG\tboot an image from the disk",
      "open dmesg" },

    { "notes", NULL, CDB_APPS,
      "notes",
      "Short notes, kept in your home folder",
      "Up to eighteen one-line notes, saved to HOME/<you>/NOTES.TXT as "
      "you change them. A adds one, Enter edits, D deletes.",
      NULL,
      "todo edit" },

    { "todo", NULL, CDB_APPS,
      "todo [list]\ntodo add <task>\ntodo done <n>\ntodo clear",
      "A simple to-do list",
      "Keeps up to 32 tasks. add puts one on the list, done <n> ticks "
      "off item n, clear empties it. (Reminders on the desktop is a "
      "separate list, with due dates.)",
      "todo add backup the laptop\tadd a task\n"
      "todo done 1\ttick off the first",
      "notify" },

    { NULL, NULL, 0, NULL, NULL, NULL, NULL, NULL }
};

int cmddb_count(void){
    int n = 0;
    while (cmddb[n].name) n++;
    return n;
}

int cmddb_is_alias(const cmddoc_t *d, const char *word){
    if (!d || !d->aliases || !word || !word[0]) return 0;
    const char *a = d->aliases;
    while (*a) {
        while (*a == ' ') a++;
        int i = 0;
        while (a[i] && a[i] != ' ' && word[i] && a[i] == word[i]) i++;
        if ((a[i] == ' ' || a[i] == 0) && word[i] == 0 && i > 0) return 1;
        while (*a && *a != ' ') a++;
    }
    return 0;
}

const cmddoc_t *cmddb_find(const char *word){
    if (!word || !word[0]) return 0;
    for (int i = 0; cmddb[i].name; i++)
        if (!strcmp(cmddb[i].name, word)) return &cmddb[i];
    for (int i = 0; cmddb[i].name; i++)
        if (cmddb_is_alias(&cmddb[i], word)) return &cmddb[i];
    return 0;
}

int cmddb_cat_find(const char *word){
    static const struct { const char *w; int cat; } syn[] = {
        {"file", CDB_FILES}, {"folders", CDB_FILES}, {"fs", CDB_FILES},
        {"data", CDB_TEXT}, {"filters", CDB_TEXT},
        {"scripting", CDB_SHELL}, {"scripts", CDB_SHELL},
        {"sys", CDB_SYSTEM}, {"display", CDB_SYSTEM},
        {"disk", CDB_DISKS}, {"storage", CDB_DISKS}, {"install", CDB_DISKS},
        {"hw", CDB_HW}, {"devices", CDB_HW},
        {"net", CDB_NET}, {"networking", CDB_NET},
        {"user", CDB_USERS}, {"accounts", CDB_USERS}, {"security", CDB_USERS},
        {"fun", CDB_APPS}, {"games", CDB_APPS},
        {0, 0}
    };
    if (!word || !word[0]) return -1;
    for (int c = 0; c < CDB_NCAT; c++)
        if (!strcmp(cmddb_cats[c].key, word)) return c;
    for (int i = 0; syn[i].w; i++)
        if (!strcmp(syn[i].w, word)) return syn[i].cat;
    return -1;
}
