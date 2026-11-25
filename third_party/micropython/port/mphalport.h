#include <stdint.h>
static inline void mp_hal_set_interrupt_char(char c) { (void)c; }
mp_uint_t mp_hal_ticks_ms(void);
mp_uint_t mp_hal_ticks_us(void);
mp_uint_t mp_hal_ticks_cpu(void);
void mp_hal_delay_ms(mp_uint_t ms);
void mp_hal_delay_us(mp_uint_t us);
uint64_t mp_hal_time_ns(void);
