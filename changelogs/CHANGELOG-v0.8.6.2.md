# HavenDOS v0.8.6.2 "Argo" (Beta)

A one-fix patch release on top of v0.8.6.1. It exists because the fix
landed after the v0.8.6.1 bundle had already been built and run on the HP,
and shipping a second, different kernel under the same version number is
exactly the failure this tree spent v0.7.18 eliminating: two binaries that
answer `ver` identically, and an `update` that compares them as equal and
declines to install the newer one. So it gets a number of its own.

## The fix: the VFS cannot read a borrowed disk

`fat16_borrow(dev, lba)` points the FAT16 driver at a partition that is
not the boot volume -- that is how `fat16 ls 1` reads a partition on a USB
stick without disturbing the mounted system volume. The borrow is returned
by re-mounting, and `fat16_raw_write()` already refused while borrowed.

`fs/vfs.c` did not. `using_fat16` is decided once, at `vfs_init()`, and
cached; it does not know the driver has been aimed somewhere else. A
`vfs_read()` during a borrow would therefore have read a path off the
stick's partition and handed it back as a system file -- and a
`vfs_write()` would have been refused by the raw layer underneath, but one
layer further down than it should have been.

Eight routes now return failure while a borrow is outstanding:

    static int fs_borrowed(void){ return fat16_borrowed(); }
    ...
    if (using_fat16 && fs_borrowed()) return -1;

**No path in v0.8.6.1 could reach this.** `fat16 cat` is a single
command that borrows, reads and returns before the shell takes another
line, and nothing re-enters the VFS in between. The guard is in anyway, for
the reason the header comment now gives in the file: *"unreachable by
construction" is what the three bus-dispatch switches v0.8.6 deleted were
also true by, and reading the wrong disk is not a failure that announces
itself.*

## Also in this bundle

`HANDOFF.md` is rewritten against what the HP actually did on 25 October,
rather than what v0.8.6.1 was hoping for:

* The **hub walk is still unproved, and this laptop may not be able to
  prove it.** Two boots, and `usb -vv` shows no hub anywhere on the bus --
  the side sockets appear to be wired straight to the xHCI root ports.
  Testing 0.8.6's route-string code on this machine needs an external hub.
* The **one command that has not been run** is now stated as the first
  item: the Kingston's second partition is 32 MB of FAT16, `fat32` found
  and named it, and `fat16 ls 1` would be the first file read off a USB
  stick on real silicon.

## Unchanged from v0.8.6.1

Everything else. Same xHCI slot release, same `fat_fs_name()` reporting,
same live-mode probe order -- all three confirmed on the HP before this
release was cut. 35 suites, 2376/2376 checks.
