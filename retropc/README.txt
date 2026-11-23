RETRO PC  --  Tech Haven Studios
HavenDOS @VER@ -- the old builds on emulated PCs, as a program of its own
=======================================================================

Retro PC runs the oldest Tech Haven Studios builds that survived, on two
emulated PCs:

  - an 8086 with a PC BIOS and a floppy, for K-DOS Beta 1.1b
  - a 32-bit 386 that loads BOOT OS 0.3.1, 0.5.0 and 0.5.2 the way GRUB
    did, from their own boot menus

It is the same Retro PC that is inside HavenDOS (Apps > Retro PC), with
the same emulators, compiled for Windows and Linux.


STARTING IT
-----------

Windows   Double-click RetroPC.exe. Nothing to install, nothing beside it
          it needs. Windows may say "Windows protected your PC" the first
          time, because the program is not signed: More info > Run anyway.

Linux     ./retropc    (an X11 window; works under Wayland through XWayland)
          ./retropc-headless runs the same program with no window -- see
          below.


THE LIST
--------

  Up/Down   choose          Enter   boot
  O         open a file     R       look in the RETRO folder again
  Q / Esc   quit

The four built-in images come first. After them: every .IMG (a floppy)
and .ISO (a CD) in the RETRO folder beside the program. On Windows you
can also drop a file on the window, or on RetroPC.exe itself.

  RetroPC file.img        boots that file straight away
  RetroPC --boot KDOS11B  boots a built-in one: KDOS11B, BOOTOS31,
                          BOOTOS50, BOOTOS52
  RetroPC --mem 64        gives the 386 64 MB (4 to 256; 16 is the default)


INSIDE AN OLD PC
----------------

  F4          leaves it: back to the list, or quits if you started it on
              a file
  Alt+Enter   full screen and back

Every other key goes to the old PC. Logins:

  K-DOS Beta 1.1b       admin / 1234
  BOOT OS (all three)   bootos / 1234

When the old PC stops -- it crashed, halted or asked for something this
PC does not have -- a box says why. R restarts it.


ITS HARD DISK
-------------

A floppy's hard disk (drive 80h, 504 KB) is a file:

  a built-in image      RETRO\<ID>.HDD          e.g. RETRO\KDOS11B.HDD
  a file you opened     the same name, .HDD     e.g. GAME.IMG -> GAME.HDD

It is written when you leave, and only if the old system wrote to it.
These are the same files HavenDOS keeps in its own RETRO folder: copy
one across and the old PC finds its disk there too.

Nothing here ever reaches a real disk. The floppy image itself is never
written back either: a program that writes its own boot disk changes a
copy, for that session.


WHAT IT CANNOT DO YET
---------------------

  - graphics modes (mode 13h, VESA): an entry that uses one boots, and a
    box says the screen cannot be shown
  - 64-bit kernels (K-DOS 1.0 Beta, multiboot2): listed, and refused
  - sound, a mouse inside the old PC, a network

Old builds count their own delays in loops, so a boot animation runs at
the speed of this emulator rather than of the PC it was written for:
BOOT OS takes about 20 seconds to reach its login.


NO WINDOW
---------

retropc-headless (Linux) types a script into the old PC, keeps a virtual
clock, and prints the screen as text when it finishes:

  ./retropc-headless --boot KDOS11B --keys 'admin\n1234\nhelp\n'

  \n Enter   \t Tab   \e Esc   {F4} {UP} {DOWN} ...   {W500} wait 500 ms

It is what HavenDOS's `make test` runs (tests/test_retropc.py).


Built from the HavenDOS source: retropc/ in github.com/MGKFN/HavenDOS.
