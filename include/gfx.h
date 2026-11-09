#pragma once
#include "types.h"
typedef enum {
    GFX_TEXT    = 0,
    GFX_MODE13  = 1,
    GFX_VESA640 = 2,
    GFX_VESA800 = 3,
} gfx_mode_t;
extern gfx_mode_t current_gfx;
