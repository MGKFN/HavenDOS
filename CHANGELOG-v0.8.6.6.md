# HavenDOS v0.8.6.6 "Argo" (Beta)

**`usb` refuses an argument it does not understand, instead of quietly
doing something else.**

On 25 October `usb cfg` was typed on a v0.8.6.1 kernel — which has no
such command. `cmd_usb()` ignored the unknown argument and printed the
ordinary bus listing, so the screen looked completely normal and was
read as the survey's output. The survey had never run.

```
usb: no such option: cfg
  usb              the controllers and what is on them
  usb -v | -vv     more of it, and every empty port
  usb rescan       walk the bus again
  usb cfg [addr]   every configuration a device offers
```

A command that quietly does something else when it does not understand
you is worse than one that refuses: the output is indistinguishable from
success.

## Everything 0.8.6.2 through 0.8.6.5, in one bundle

The HP is still running **v0.8.6.1**. Nothing since has been installed on
it, so this bundle carries all of it:

| | |
|---|---|
| **0.8.6.2** | the VFS refuses to read a disk the FAT16 driver is borrowed onto; `make clean && make` builds again |
| **0.8.6.3** | `update` from a USB stick; `fat16 cp` onto one |
| **0.8.6.4** | `usb cfg` — every configuration a device offers, selecting none; `wTotalLength` bounds the descriptor walk |
| **0.8.6.5** | `update` says **which** of four reasons the stick was not used |
| **0.8.6.6** | `usb` refuses an argument it does not know |

**36 suites, 2527 checks.**
