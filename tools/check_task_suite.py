#!/usr/bin/env python3
"""check_task_suite.py -- prove tests/test_task.c can fail.

tests/test_task.c passed 91 of 92 on its first run, and the one failure
was the fixture's own (a cleanup counter that carried over between
sections). A suite that is that close to green first time is the least
trustworthy result this tree recognises, so this is the judge.

kernel/task.c is a cooperative scheduler, and the way a scheduler is
wrong is rarely a wrong return value. It is a register not restored, a
console left live in the wrong task, a front that points at a task that
has gone, or a switch that happens somewhere it must not. Each
injection below is one of those:

  - the switch forgets a callee-saved register: every task works until
    the compiler happens to keep a value in %esi across a yield;
  - the console or the guard is not swapped: the app behind draws on
    the screen, or a fault lands in another task's frame;
  - a refusal is dropped: a switch while a driver holds a shared
    buffer, which is the whole argument of include/task.h undone;
  - the front is not handed on: the person is left looking at a task
    that no longer exists;
  - a paused task is runnable: games keep playing behind the front;
  - the canary is never checked, or the reap frees the running stack.

Run:  python3 tools/check_task_suite.py    (foreground, never &)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inject

SRC  = "kernel/task.c"
TEST = "tests/test_task.c"
CC = ["gcc", "-std=gnu99", "-m32", "-O2", "-fno-builtin", "-Iinclude", "-Itests",
      "-Wno-builtin-declaration-mismatch", "-Wno-unused-function"]

CHECKS = [["python3", "tools/check_task_sites.py"]]

BUGS = [
    ("the switch does not save %esi",
     "a callee-saved register the compiler is entitled to keep a value "
     "in across task_yield(). Every task works until one does, and then "
     "a loop counter or a pointer comes back holding another task's",
     """"   pushl %esi\\n"
"   pushl %edi\\n\"""",
     """"   pushl %edi\\n\""""),

    ("the console is not loaded on a switch",
     "the task switched in draws into the previous task's console -- "
     "the app behind paints over the app in front",
     """    vga_ctx_load(&n->vga);
    appguard_ctx_load(&n->guard);
    if(next == g_front) mouse_clamp_to_grid();""",
     """    appguard_ctx_load(&n->guard);
    if(next == g_front) mouse_clamp_to_grid();"""),

    ("the crash guard is not swapped",
     "a fault in one task jumps into the frame another task armed, on "
     "a stack that is not running -- the exact failure appguard.h "
     "describes as EIP 0x7",
     """    appguard_ctx_save(&p->guard);

    /* ── THE CANARY""",
     """
    /* ── THE CANARY"""),

    ("a switch is allowed while a driver holds a shared buffer",
     "the argument of include/task.h undone: a task switched out half "
     "way through usb_control() while another starts one",
     """    if(kpoll_depth() > 0)  return refuse(TY_EBUSY);""",
     """    /* depth not checked */"""),

    ("a switch is allowed inside kernel_poll()",
     "a periodic client that waits would run another task from inside "
     "the bottom half, with its buffers half written",
     """    if(kpoll_in_poll())    return refuse(TY_EPOLL);""",
     """    /* in_poll not checked */"""),

    ("task_lock() does not stop a switch",
     "Alt+Tab out of the installer half way through partitioning",
     """    if(g_lock)             return refuse(TY_ELOCKED);""",
     """    /* lock not checked */"""),

    ("a paused task behind the front is runnable",
     "it does not get keys -- its wait loop still checks the front -- "
     "but every parked app is switched in on every yield only to yield "
     "straight back: a switch and two console copies each, per keyboard "
     "read, and a yield can never say there is nothing else to run. "
     "FIRST RUN MISSED THIS: nothing a trace can see changes, so the "
     "suite now counts switches into the parked task",
     """    if(g_t[s].state == TS_WAIT && s == g_front) return 1;""",
     """    if(g_t[s].state == TS_WAIT) return 1;"""),

    ("the front is not handed on when its task ends",
     "the person is left with a front pointing at a dead task: nothing "
     "is drawn and the keyboard goes to no one",
     """    reparent(me);
    front_from(me);
    g_t[me].state = TS_DEAD;""",
     """    reparent(me);
    g_t[me].state = TS_DEAD;"""),

    ("the front goes to slot 0 even when the opener is alive",
     "closing an app opened from the shell drops you on the desktop "
     "instead of back at the prompt that opened it",
     """    int to = alive(g_t[s].opener) ? g_t[s].opener : 0;
    if(to == s) to = 0;""",
     """    int to = 0;"""),

    ("killing a task does not reparent its children",
     "an app opened by a task that was killed keeps an opener that no "
     "longer exists, and the slot can be reused by an unrelated task",
     """    if(g_t[s].cleanup) g_t[s].cleanup();
    reparent(s);""",
     """    if(g_t[s].cleanup) g_t[s].cleanup();"""),

    ("the cleanup hook is not run on kill",
     "killing the Music Player mid-note leaves the speaker sounding",
     """    if(g_t[s].cleanup) g_t[s].cleanup();
    reparent(s);""",
     """    reparent(s);"""),

    ("the canary is never checked",
     "a stack overflow writes into the heap and the task carries on, "
     "spreading the damage instead of being stopped",
     """    if(!canary_ok(prev)){""",
     """    if(0){"""),

    ("task_set_front() does not hide the old front",
     "two consoles visible at once: the one behind keeps painting the "
     "screen until it next yields",
     """    if(old == g_cur) vga_set_visible(0);
    else             g_t[old].vga.visible = 0;

    g_front = s;""",
     """    g_front = s;"""),

    ("spawn steals the front",
     "every background job jumps in front of what you are doing",
     """    t->front_seq = 0;
    vga_ctx_init(&t->vga, cel);""",
     """    t->front_seq = 0;
    g_front = s;
    vga_ctx_init(&t->vga, cel);"""),

    ("a failed spawn keeps the stack it did get",
     "each refused spawn on a low-memory machine leaks 96 KB",
     """        if(stk) kfree(stk);
        if(cel) kfree(cel);""",
     """        if(cel) kfree(cel);"""),

    ("the reaper does not tell the console the buffer is gone",
     "the next task's console can be given the same address, the screen "
     "thinks it already shows it, and the dead task's last frame stays up",
     """            vga_forget_cells(g_t[s].cells);""",
     """            /* not forgotten */"""),

    ("the gate lets a task behind the front have the key",
     "a running job takes the keystrokes meant for the app in front",
     """    if(g_t[g_cur].policy == TP_PAUSE) task_wait_front();
    else                              task_yield();
    return g_cur == g_front;""",
     """    if(g_t[g_cur].policy == TP_PAUSE) task_wait_front();
    else                              task_yield();
    return 1;"""),

    ("task_wait_front() spins on a refusal",
     "a paused task behind a held lock waits for ever inside getchar, "
     "and the machine looks hung",
     """        if(task_yield() != TY_RAN) break;""",
     """        (void)task_yield();"""),
]

BUGS.append(
    ("the front pin does not stop task_set_front()",
     "Alt+Tab on the lock screen switches straight to the shell behind "
     "it -- the lock does not lock",
     """    if(g_pin && s != g_front) return -1;""",
     """    /* pin not checked */"""))

HARMLESS = (
    "a comment is reworded (must not be caught)",
    "nothing about the code changed",
    """int task_ready(void){ return g_init; }""",
    """/* has task_init() run */
int task_ready(void){ return g_init; }""")

if __name__ == "__main__":
    sys.exit(inject.run(
        "task", SRC, TEST, CC, BUGS, HARMLESS, timeout=30, checks=CHECKS,
        preamble="Proving tests/test_task.c can fail.\n"
                 "kernel/task.c switches real stacks; each injection is a "
                 "way a scheduler goes wrong without a wrong return value."))
