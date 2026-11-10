# HavenDOS v0.8.9.1 — the diagnostic that could not be read

9 November 2026. 59 suites, 5883/5883. A fix release: no new feature,
one command that did not exist, and one screen that was printing its
evidence somewhere nobody could see it.

## 1. `lockdown` IS a command now

It never was. The subcommand is `usb lockdown`, and **both** hardware
test docs -- 0.8.8.11's and 0.8.9's -- told people to type the bare
word. So the first thing anybody following either document saw was
`'lockdown' command not found`, twice, before giving up on it.

Fixing the docs is necessary and not sufficient. The bare word is what
a person types, it is unambiguous, and there is nothing else it could
mean, so it is a command: `lockdown` runs `usb lockdown`.

Also corrected in both docs: `lockdown dump`, which has never existed
either.

## 2. `usb lockdown` printed its verdict and hid its evidence

The command has always ended with "the raw line above is what arrived"
and **there has never been a raw line above it.** `lockdown_dump()` is
written entirely in `klog()`, and `kernel/main.c` calls
`klog_set_echo(0)` before the shell starts. Every byte of evidence this
command produces goes to the ring buffer; the screen gets the verdict
and a blank space where the reason should be.

That is not a small thing. This is the command somebody runs on a
machine with no debugger, photographs, and sends to somebody else --
and the half worth photographing was not on it. The 0.8.9 hardware-test
doc told Kyan to photograph that screen. It was my instruction and the
screen could not answer it.

`lockdown_show_reply()` prints the reply with vga rather than klog, and
prints three things because they answer different questions:

- **the byte count, and the 4-byte length prefix beside it.** If those
  two disagree, the framing is the bug -- which the old message could
  only speculate about, and which is one subtraction to know.
- **the first 16 bytes in hex.** `bplist00` is a binary plist. All
  zeroes is a buffer nobody filled. Hex says what it is when the text
  does not.
- **the first 320 bytes as text**, non-printables as `.`, so a binary
  reply is still photographable rather than scrolling the console
  sideways. A lockdownd reply has `<key>Type</key>` in the first two
  hundred bytes.

It runs on every lockdown step that does not go as expected, in
`usb lockdown` **and in `pair`** -- `pair` is where people actually
are when this fails, and sending them to a second command whose
evidence also went to an unechoed log was the whole problem.

## 3. `pair` said "the device answered" when nothing had answered

`the device answered "" rather than lockdown.` is printed whenever
QueryType returns anything but 1 -- and that includes negative codes,
which mean *no reply at all*. A reply we could not parse and a device
that never spoke are different failures with different next steps, and
the line claimed the first while meaning either.

It prints the code now, and the reply beneath it.

## 4. `shell/usb.c` called `lockdown_reply()` through an implicit declaration

Found while adding the above: the file carries hand-written externs
rather than including `include/lockdown.h`, so `lockdown_reply()` was
declared implicitly and taken as returning `int`. On this ABI a pointer
survives that. `include/fat16.h` records the same mistake costing a
release when a signature changed underneath it. The header is included
now and the compiler compares them.

## What this release does NOT fix

**The failure on the HP is not diagnosed.** `usb/usbmux.c` is
byte-identical between 0.8.8.11 and 0.8.9, and the only change on the
plaintext lockdown path is a branch that is not taken when there is no
session -- so 0.8.9 did not cause it and reverting to 0.8.9 would not
clear it. The reply parser is sound: it finds `<key>Type</key>` and
then the next `<string>`, and whitespace between them does not matter.

What is known: bytes arrived and were framed as a reply (a zero return
means that; a timeout returns negative), and that reply carried no
`Type`, no `Value` and no `Error`. Which bytes those were decides it,
and this release is the one that puts them on the screen.

Run `lockdown` and photograph it.
