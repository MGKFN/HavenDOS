// time.time() for extmod/modtime.c: seconds since 1970 from the RTC
static mp_obj_t mp_time_time_get(void) {
    return mp_obj_new_int_from_ull(mp_hal_time_ns() / 1000000000ull);
}
