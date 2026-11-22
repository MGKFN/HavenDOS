# HavenDOS v0.9.7.3 Beta "Mnemosyne" — USB sticks you can save to, and replugging

Kyan, 22 November, after 0.9.7.2:

> update over USB holy fucking slow ... and maybe the abbility to save
> smth to usb and use some general file name extensions

And, while this was being built:

> usb devices plugging back in out still an pain point for this os that
> kind of stuff needs fixed before hand

Asked where saving to a stick should work, he chose "Everywhere". Asked
about extensions: "Just like windows or Mac OS Linux .txt .md etc .zip".

> **Not tested on a real machine.** Tested in QEMU and on the build
> machine. Nothing in it has run on the HP yet.

## Faster: reading from a USB stick

- A stick was read one 512-byte sector per USB command. Every command
  costs three transfers and a wait for the stick, so an update from a
  stick crawled.
- It now reads **64 sectors (32 KB) at a time** and serves the next
  reads from that. Writing a sector updates the copy, so nothing stale
  is ever returned.
- A stick that refuses a multi-sector read (sense 05/24) gets the old
  one-sector path, for that read only.
- In QEMU, reading an 8.5 MB update went from **16.7 s to 2.1 s**.

## New: /USB, everywhere

- The first USB stick with a FAT32 volume appears as **`/USB`** in every
  place that takes a path: Files, the Shell (`ls`, `cd`, `cp`, `mv`,
  `rm`, `mkdir`, `cat`, `>`), the editor's Save, and every app that saves
  a file.
- FAT32 sticks are read and write. exFAT sticks are read only. FAT16
  sticks are not shown yet.
- It is mounted for each operation and let go straight after. Pulling
  the stick between two saves loses nothing.
- Files lists **USB** in the root folder while a stick is in.
- Moving a file between `/USB` and the system disk copies it and then
  deletes the original, only once the copy has been written.
- Checked from outside HavenDOS: what it wrote, mtools reads back, and
  fsck.vfat finds the stick clean.

## New: file types

- Files has a **Type** column, as Windows, macOS and Linux do: Text,
  Markdown, C source, Disk image, Audio, Picture, ZIP archive, and so on,
  about 50 extensions. Sizes are shown as B, KB or MB.
- Opening a file goes to the app for its type:
  - text, source and Markdown → the editor;
  - `.IMG` and `.ISO` → Retro PC;
  - `.MP3` → Music;
  - programs and binary data → the hex viewer;
  - no extension → the editor if it reads as text, otherwise the hex
    viewer;
  - pictures, PDFs and archives → a message that HavenDOS cannot open
    them yet.
- `.zip` is recognised but cannot be opened yet. The Shell only unpacks
  HavenDOS's own `.z`.
- Long names such as `my notes.txt` work on FAT32: USB sticks, or a
  system installed on FAT32. A FAT16 system keeps 8.3 names
  (`MYNOTES.TXT`).

## Fixed: folders on a FAT16 system

- On the HP, Files showed the root folder's contents inside every
  folder. FAT16 could only list, create, rename and delete in the root.
- It now does all four in any folder, and `mv` can move between them.

## Fixed: plugging back in

- A device pulled and put back within one second was never noticed.
  Both checks saw the port connected, so nothing looked different. The
  mouse or keyboard stayed dead until a reboot.
- On xHCI, the port's own *connect status change* bit is now read as
  well. It is set by the controller when a device goes and comes back,
  however fast, and acknowledged when the port is reset during the walk.
- Only a port whose device had enumerated counts. The HP's internal
  port that never answers does not set off a walk every second.
- QEMU, unplug and replug while running:
  - a mouse, slowly and in under a second;
  - a stick, slowly and in under a second;
  - a keyboard.

  Each one works again after it comes back, 9 of 9.

## New: the `mouse` command

- `mouse` shows where the pointer is, which buttons are down, and how
  many USB pointers and trackpads are attached. It shows at once whether
  a dead pointer is a driver that never started or one that stopped.

## Other fixes

- `ls <folder>` lists that folder, not the current one.
- Deleting, renaming or making a folder on FAT32 reported failure when it
  had worked.

## Tests

- `make test`: 77 suites, 7723 checks.
  - test_msc 125: read-ahead, the one-sector fallback, writes keeping the
    copy fresh.
  - test_kpoll 165: a replug inside one poll is walked once, and a change
    on an empty or never-enumerated port is ignored.
- QEMU harnesses:
  - usb_files_0973 17/17: write, copy, move, folders and long names on a
    stick, checked with mtools and fsck.vfat;
  - replug_0973 9/9;
  - usb_storage 52/52, fixes_0971 7/7, retro386 7/7.

## Still open

- **Trackpad.** Still not working on the HP. The photos of
  `dmesg | grep I2C`, `i2c` and `mouse` will show which step stops.
- FAT16 long names, FAT16 sticks, `.zip`.
