// HavenDOS port of MicroPython: 32-bit x86, freestanding, inside the kernel.
#include <stdint.h>

#define MICROPY_CONFIG_ROM_LEVEL          (MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES)

#define MICROPY_ENABLE_COMPILER           (1)
#define MICROPY_ENABLE_GC                 (1)
#define MICROPY_HELPER_REPL               (1)
#define MICROPY_REPL_AUTO_INDENT          (1)
#define MICROPY_KBD_EXCEPTION             (1)
#define MICROPY_ENABLE_EXTERNAL_IMPORT    (1)
#define MICROPY_READER_VFS                (0)
#define MICROPY_ALLOC_PATH_MAX            (160)
#define MICROPY_ERROR_REPORTING           (MICROPY_ERROR_REPORTING_NORMAL)
#define MICROPY_LONGINT_IMPL              (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_FLOAT_IMPL                (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_PY_MATH                   (1)
#define MICROPY_PY_BUILTINS_FLOAT         (1)
#define MICROPY_PY_BUILTINS_INPUT         (1)
#define MICROPY_PY_BUILTINS_HELP          (1)
#define MICROPY_PY_BUILTINS_HELP_TEXT     havendos_help_text
#define MICROPY_PY_SYS                    (1)
#define MICROPY_PY_SYS_EXIT               (1)
#define MICROPY_PY_SYS_PATH               (0)
#define MICROPY_PY_SYS_ARGV               (0)
#define MICROPY_PY_SYS_MODULES            (1)
#define MICROPY_PY_IO                     (0)
#define MICROPY_PY_TIME                   (1)
#define MICROPY_PY_TIME_TIME_TIME_NS      (1)
#define MICROPY_PY_TIME_INCLUDEFILE       "modtime_port.h"
#define MICROPY_PY_RANDOM                 (1)
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC  (havendos_random_seed())
#define MICROPY_PY_RANDOM_EXTRA_FUNCS     (1)
#define MICROPY_STACK_CHECK               (1)
#define MICROPY_USE_INTERNAL_PRINTF       (1)
#define MICROPY_USE_INTERNAL_ERRNO        (1)
#define MICROPY_VM_HOOK_COUNT             (2000)
#define MICROPY_VM_HOOK_INIT              static unsigned int vm_hook_divisor = MICROPY_VM_HOOK_COUNT;
#define MICROPY_VM_HOOK_POLL              if (--vm_hook_divisor == 0) { vm_hook_divisor = MICROPY_VM_HOOK_COUNT; havendos_poll(); }
#define MICROPY_VM_HOOK_LOOP              MICROPY_VM_HOOK_POLL
#define MICROPY_VM_HOOK_RETURN            MICROPY_VM_HOOK_POLL
#define MICROPY_HW_BOARD_NAME             "HavenDOS"
#define MICROPY_HW_MCU_NAME               "x86"
#define MICROPY_PY_SYS_PLATFORM           "havendos"
#define MICROPY_GCREGS_SETJMP             (0)

extern const char havendos_help_text[];
uint32_t havendos_random_seed(void);
void havendos_poll(void);

typedef long mp_off_t;
#define MP_STATE_PORT MP_STATE_VM
#include <alloca.h>
#define MICROPY_HEAP_SIZE (2 * 1024 * 1024)
