#pragma once

/**
 * Radio operating modes, shared across all radio drivers and the web API.
 *
 * The numeric values are the Elecraft CAT `MD` command digits (see the KX2/KX3
 * Programmer's Reference); non-Elecraft drivers translate to and from their
 * native mode encodings (e.g. include/ic705_modes.h for Icom CI-V).
 *
 * Kept free of platform dependencies so protocol-translation code that uses it
 * can be compiled and unit-tested on the host (see test/unit/).
 */
typedef enum {
    MODE_UNKNOWN = 0,
    MODE_LSB     = 1,
    MODE_USB     = 2,
    MODE_CW      = 3,
    MODE_FM      = 4,
    MODE_AM      = 5,
    MODE_DATA    = 6,
    MODE_CW_R    = 7,
    MODE_DATA_R  = 9,
    MODE_LAST    = 9
} radio_mode_t;
