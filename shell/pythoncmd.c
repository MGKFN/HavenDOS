/* shell/pythoncmd.c -- v0.9.10: `python`, MicroPython inside HavenDOS.

     python            the REPL (>>>), Ctrl+D on an empty line leaves
     python file.py    runs the file, then back to the shell

   MicroPython itself (MIT licence, third_party/micropython) is built
   into the kernel as one library; its glue to the console, keyboard,
   clock and files is third_party/micropython/port/havendos_port.c. */
#include "../include/vga.h"
#include "../include/string.h"

extern int python_main(const char *path);

void cmd_python(const char *args)
{
    while (args && *args == ' ') args++;
    if (args && !strcmp(args, "--help")) {
        vga_puts("python            the Python prompt (>>>); Ctrl+D on an empty line leaves\n"
                 "python file.py    runs a Python file\n"
                 "Ctrl+C stops a running program.\n");
        return;
    }
    python_main(args && *args ? args : 0);
}
