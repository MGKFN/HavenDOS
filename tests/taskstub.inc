/* ════════════════════════════════════════════════════════════════════
   tests/taskstub.inc -- the task layer, switched OFF.  v0.9.6.

   For the suites that compile shell/desktop.c, shell/ui.c or
   kernel/main.c to test something else (menu geometry, themes, the
   console). With task_ready() at 0, desktop_run_app() takes the same
   direct path it took before 0.9.6 and safe mode still takes -- which
   is what those suites were written against.

   WEAK, every one, so a suite that defines its own wins without a
   clash. tests/test_task.c links the real kernel/task.c and does not
   include this.
   ════════════════════════════════════════════════════════════════════ */
#define TS_WEAK __attribute__((weak))
TS_WEAK void task_init(void){}
TS_WEAK void taskswitch_init(void){}
TS_WEAK int  task_ready(void){ return 0; }
TS_WEAK int  task_locked(void){ return 0; }
TS_WEAK void task_lock(void){}
TS_WEAK void task_pin_front(void){}
TS_WEAK void task_unpin_front(void){}
TS_WEAK int  task_front_pinned(void){ return 0; }
TS_WEAK int  task_kill(int s){ (void)s; return -1; }
TS_WEAK void task_unlock(void){}
TS_WEAK int  task_spawn(const char *n, void (*e)(void *), void *a, int p)
             { (void)n; (void)e; (void)a; (void)p; return -1; }
TS_WEAK int  task_set_front(int s){ (void)s; return -1; }
TS_WEAK int  task_wait_front(void){ return 1; }
TS_WEAK int  task_current(void){ return 0; }
TS_WEAK int  task_front(void){ return 0; }
TS_WEAK int  task_is_front(void){ return 1; }
TS_WEAK void task_set_policy(int p){ (void)p; }
TS_WEAK void task_set_name(const char *n){ (void)n; }
TS_WEAK void task_set_cleanup(void (*f)(void)){ (void)f; }
TS_WEAK int  task_count(void){ return 1; }
TS_WEAK int  task_state(int s){ return s == 0 ? 1 : 0; }
TS_WEAK const char *task_name(int s){ (void)s; return "Desktop"; }
TS_WEAK int  task_slot_policy(int s){ (void)s; return 0; }
TS_WEAK int  task_running_behind(int s){ (void)s; return 0; }
TS_WEAK unsigned int task_id(int s){ (void)s; return 0; }
TS_WEAK void kpoll_sleep_ms(unsigned int ms){ (void)ms; }
TS_WEAK void *kmalloc(unsigned long n){ (void)n; return 0; }
TS_WEAK void kfree(void *p){ (void)p; }
