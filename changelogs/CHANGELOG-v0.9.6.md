# HavenDOS v0.9.6 Beta "Daedalus": several apps at once

*15 November 2026.*

Until this release HavenDOS ran one program at a time. The desktop
called an app, and the app had the machine until it returned. Kyan
chose both halves of the old 0.8.x promise:

- **tasks** that switch when an app waits for a key or yields, with
  background jobs;
- **switching between open apps** with Alt+Tab, each keeping its state,
  with background tasks still running.

Both are in this release.

## What you see

- **Alt+Tab.** Hold Alt and press Tab: a panel lists what is open,
  most recently used first. More Tabs move down, Shift+Tab moves up,
  and letting go of Alt switches. A quick Alt+Tab flips between the
  last two. Enter switches too, Esc cancels, and X closes the
  highlighted app.
- **Open apps on the bottom bar.** One chip per open app, between your
  name and the alerts: grey when it is paused, green when it is running
  behind. Click one to bring it forward.
- **Apps keep their state.** An app you switch away from stops where
  it was and comes back exactly as you left it, screen and all. Games
  pause. Editors keep what you typed.
- **Two apps keep running behind.**
  - **Stopwatch:** a timer still rings while you do something else.
  - **Music Player:** the song plays on while you switch away. (The
    player had never played a note before this; see below.)
- **One of each.** Opening an app that is already open brings it
  forward instead of starting a second copy, so two Notes can't save
  over each other. The Shell is the exception: open as many as you like.
- **Jobs.** In the shell, `ping 8.8.8.8 &` runs the command behind, in
  its own task with its own screen. Alt+Tab or `fg <id>` shows it. When
  it ends, the alerts say so; any key then closes it.
- **`ps`** lists the desktop, each open app and each job: id, name,
  state, how long it has been open, how much of its stack it has used,
  and how often it has run. **`jobs`** is the same command.
- **`fg <id or name>`** brings one forward. **`kill <id or name>`**
  closes one.
- **System > Overview > Tasks** counts what is open and what is running
  behind, instead of "one app at a time".
- **The user guide** has a page on it ("Several apps at once") and the
  shell page has jobs.

## How it works

The design is in `include/task.h`. The short version:

- **Every app is a task**: its own 96 KB stack, its own console and its
  own crash guard. Task 0 is the context the kernel booted on (the
  desktop, or the shell in Shell Mode).
- **One task is in front.** It owns the screen, the keyboard and the
  mouse buttons. Each task behind is either **paused** (the default: it
  stops at its next keyboard read) or **running** (it carries on and
  draws into its own console, which nobody sees until it comes forward).
- **Cooperative, not preemptive.** Every layer in this tree keeps
  module-level buffers and none of them is reentrant, so switching from
  the timer interrupt would corrupt transfers in flight. A switch
  happens only where `kernel_poll()` is already allowed to run --
  `keyboard_getchar()`, `kpoll_sleep_ms()` and the desktop's idle wait
  -- and is refused for the same reasons: a driver holds a shared buffer
  (`kpoll_depth()`), the bottom half is running, or the switch is
  locked. A switch is therefore exactly as safe as the polling that
  already happened at the same instruction.
- **`sleep_ms()` does not switch.** It is the driver-safe wait. An app
  that keeps running behind waits with `kpoll_sleep_ms()` instead, or it
  would hold the app in front up for every wait.

New files:

- `include/task.h`, `kernel/task.c`: the table, the switch (eight
  instructions of assembly), who is in front, and the refusals.
- `shell/taskswitch.c`: the Alt+Tab panel, `ps`, `fg`, `kill` and jobs.

Changed:

- `drivers/vga.c`: **a console per task.** The shadow buffer is a
  pointer now; `vga_ctx_t` holds the buffer, cursor, colour, viewport,
  frame depth and clip count. Only the console in front paints. Bringing
  one forward repaints from its own buffer, so an app reappears without
  drawing anything. A console that is not on the screen cannot change
  the mode (`VGA_MODE_EHIDDEN`).
- `kernel/appguard.c`: **a crash guard per task**, swapped on every
  switch, so a fault lands in the frame of the task that faulted. An app
  opened from the shell (`open notes`) used to run unguarded, because
  the shell's guard was already armed; it has its own guard now.
- `drivers/keyboard.c`, `usb/hid.c`: **Alt is tracked** on both
  keyboards (PS/2 left and right, the USB modifier byte), and Alt+Tab
  produces `KEY_TASKSW`, which never reaches an app. The keyboard asks
  the task layer before every read, through a hook.
- `drivers/mouse.c`: buttons and clicks belong to the task in front. A
  task behind sees where the pointer is but never a press, and never
  consumes one.
- `kernel/kpoll.c`: `kpoll_sleep_ms()` gives other tasks a turn between
  slices; `kpoll_in_poll()` is new.
- `shell/desktop.c`: `desktop_run_app()` starts a task and waits to be
  in front again. The old body is `app_run_inline()`, still used when no
  task can be started (no slot, no memory, safe mode, `notasks`). The
  registry gained a `tasking` field: `TK_PAUSE`, `TK_RUN`, `TK_SOLO`.
- `kernel/main.c`: tasks start after the heap and the console.
  **`notasks`** on the boot line turns them off, and safe mode never
  starts them; either way apps run one at a time, as in 0.9.5.

## Security: the lock and login screens

Writing this turned up three holes that task switching would have
opened. All three are closed:

- **Alt+Tab on the lock screen** (`lock`, or Lock in the start menu)
  would have switched straight out of it to whatever was open behind.
  The front is now **pinned** while a lock or login screen is up:
  nothing can be brought forward, and the switcher says "The screen is
  locked". Apps behind keep running, so a song plays on under the lock.
- **Switch user** left the previous user's apps open, so the next person
  was one Alt+Tab from somebody else's shell, signed in as them. When a
  **different** user signs in, the previous user's apps are now closed,
  and an alert says so. The same user signing back in finds everything
  as it was.
- **The installer** holds a switch lock (`TK_SOLO`): nothing else runs
  and Alt+Tab is refused while it is open.

## Fixed along the way

- **The Music Player had never played a note.** "Any key stops it" was
  written as `keyboard_getchar()!=0`, and "no key" is -1, so the loop
  stopped before its first note on every machine. The screen said "Now
  playing" over silence. It now plays, with each note timed by the clock
  instead of a busy loop calibrated in CPU cycles.
- **`tools/make_appreg_inc.py` left out every app that set `fits`**
  (Shell, System, Minefield, Settings): its pattern ended at the
  category. `test_menugeom` was checking 43 apps out of 50. It reads
  all 50 now, and fails the build if a registry row can't be parsed. The
  menus still fit.
- **The boot harnesses had not booted since 0.9.5.1.**
  `appguard_boot.py`, `helios_boot.py`, `lockdown_boot.py` and
  `layout_audit.py` waited for "System ready" on screen, and 0.9.5.1
  clears the screen before the shell's welcome. They wait for the
  prompt as well now. `appguard_boot.py` also waits out the crash
  notice's 600 ms key hold before pressing Esc; pressing it sooner was
  discarded about one time in three, by design.
- **`ping` yields between echoes**, so `ping ... &` doesn't hold the
  machine for all four.
- **`resolution` refuses with other apps open.** Each open app's console
  is laid out for the current grid.

## Tests

- **New `test_task`: 99 checks.** It is built `-m32`, so the real switch
  runs on real stacks in a user process. Every task in it is a C
  function running on the stack `task_spawn()` gave it, and the suite
  checks the address of a local to prove it. The console and the guard
  are fakes that remember which one is live, so "a task runs on its own
  console" is checked, not assumed. It passes at `-O2` and at `-O0`,
  where register allocation differs.
- **New `tools/check_task_suite.py`: 19 injections, all caught**, plus
  the harmless one correctly missed. The first run missed one: making a
  parked task runnable changes nothing a trace can see, because its wait
  loop yields straight back. Its cost is a switch per parked app per
  keyboard read. The suite now counts switches into a parked task. The
  injection's own description had been wrong about the consequence, and
  is corrected.
- **New `tools/check_task_sites.py`**, run by `make`:
  - no call into the task layer from `drivers/`, `usb/`, `net/` or `fs/`;
  - `task_switch_stack()` called only from `kernel/task.c`;
  - no `sleep_ms()` in an app the registry marks `TK_RUN`.
  All three rules were planted and refused before it was trusted.
- **`test_console`** gained 17 checks for per-task consoles: a hidden
  console never paints, one brought forward is repainted from its own
  buffer, a hidden console cannot set the mode, and the viewport travels
  with its console. **`check_console_suite.py`** gained four injections,
  all caught. Writing one of them showed that clearing the framebuffer's
  painted record on every switch was redundant: the record is always
  accurate, so the diff alone is correct and cheaper. The clear is gone.
- **`test_kpoll`**: the polling sleep yields every slice, and sleeps as
  before with no task layer. **`check_kpoll_suite.py`** gained "the
  polling sleep does not yield", caught.
- **`test_usbhid`**: 30 checks became 36. Alt+Tab, right Alt,
  Shift+Alt+Tab, Tab without Alt, and the Alt release reaching the
  switcher.
- Two stale injections, re-aimed at the changed code:
  - in `check_kpoll_suite.py`, at `kpoll_sleep_ms()`'s loop;
  - in `check_console_suite.py`, at `vga_set_mode()`'s first branch.
- **`make test`: 7138/7138 across 68 suites.**

### In QEMU

- **`appguard_boot.py` 19/19.** Four crash types, each inside a task;
  the desktop answers after every one; the tick keeps arriving; the
  kernel counts four recoveries; `crash stack` still wedges, as
  documented.
- **`helios_boot.py` 32/32.**
- **Scripted runs** of:
  - Notes opened from the shell, Alt+Tab back, `fg notes`;
  - the Stopwatch running behind the desktop;
  - the switcher held open with Alt down, then a second Tab;
  - chips on the bar;
  - the Music Player behind (its switch count went 182, 770, 1699
    while it played);
  - `ping 10.0.2.2 &` finishing behind, its alert, and its console
    brought forward;
  - `kill music`;
  - `crash div` inside a task;
  - Alt+Tab on the lock screen (still locked afterwards);
  - `resolution` refused with apps open;
  - opening Notes a second time.

## Limits, said plainly

- **Cooperative means a task runs until it waits.** A job runs between
  its keyboard reads and polling sleeps. `wget` is one blocking
  `http_get()`, so `wget ... &` holds the machine until it is done, job
  or not. So does anything that computes without waiting. `crash stack`
  still wedges: a recursion that never yields never reaches a switch.
- **This is task switching, not protection.** Every task can still write
  anywhere in memory, as before.
- **No guard page.** A stack overflow is caught by a canary when the
  task next switches out, and the task is stopped with a log line. What
  it already overwrote below its stack is not repaired.
- **`kill` stops a task where it waits.** Memory it allocated stays
  allocated until reboot. Its cleanup hook runs if it has one; the Music
  Player's turns the speaker off.
- **At most 8 tasks**, the desktop included. Each takes 96 KB of stack
  and a 64 KB console from the heap. If there is no room, the app runs
  the old way, directly, and the log says why.
- **Two tasks on the network at once share UDP's single receive slot**,
  so two DNS lookups at the same instant can collide.
- **Not proved on the HP.** Everything above ran in QEMU. The
  framebuffer console and the USB keyboard's Alt are the parts the
  laptop has to confirm.
