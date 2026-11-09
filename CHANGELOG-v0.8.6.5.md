# HavenDOS v0.8.6.5 "Argo" (Beta)

**`update` says why it did not use the stick.**

A FAT32 stick staged correctly at `\EFI\HAVENDOS\update\` was not found
on the HP, and `update` fell through to the leftover on the EFI System
Partition. The screen said:

```
  Source  : EFI System Partition
```

and nothing else. That one line is **four different failures wearing the
same face**, each with a different fix:

| What happened | What to do about it |
|---|---|
| No removable disk on the bus at all | `usb rescan`, then `disk` |
| A stick is there with no FAT32 volume on it | `fat32` names what it is — exFAT and FAT16 are not read here |
| A FAT32 volume that would not mount | a different problem entirely |
| It mounted and carries no manifest | check the path and the file name |

Reformatting a stick that was never enumerated is the wrong fix applied
to the first case, and the screen gave no way to tell which case it was.

## What it prints now

A stick is probed **before** the ESP, so reaching any other source means
the USB probe said no. It now counts what it saw on the way past, and
`update` prints it under the source line it already shows — once, in the
place somebody is already looking:

```
  Source  : EFI System Partition
            no update staged on a USB stick -- no removable disk
            on the bus. Try 'usb rescan', then 'disk'.
```

or

```
            no update staged on a USB stick -- a stick is there with
            no FAT32 volume on it. 'fat32' says what it is;
            exFAT and FAT16 are not read here.
```

or

```
            no update staged on a USB stick -- no manifest on it at
            /EFI/HAVENDOS/update/update.inf
```

Same idea as `fat32`'s "partitions this kernel does not read" list in
v0.8.6.1, which is what turned *"`fat32` prints nothing"* into an answer.

## Testing

`tests/test_update.c`: **119 → 133 checks.** All four arms are driven
and each is asserted distinguishable from the others — a diagnostic that
cannot tell them apart is worse than none, because it sends somebody to
reformat a stick that was never enumerated.

**36 suites, 2527 checks.**

## What this does NOT do

It does not make the stick work. It makes the machine say which of the
four things is wrong, so the next boot answers the question instead of
raising it again.
