#pragma once

/**
 * Pure translation between Icom CI-V mode encodings and radio_mode_t, used by
 * the IC-705 driver (src/radio_driver_ic705.cpp).
 *
 * The IC-705 represents "data" operation as SSB plus a separate data-mode
 * flag (USB-D / LSB-D) rather than as distinct mode codes, so both directions
 * carry that flag alongside the mode byte. RTTY (0x04/0x08) is mapped onto
 * MODE_DATA / MODE_DATA_R for the web API's benefit.
 *
 * Free of platform dependencies so it can be unit-tested on the host
 * (test/unit/test_ic705_modes.cpp), following the radio_detection.cpp pattern.
 */

#include "radio_mode.h"

#include <cstdint>

namespace ic705 {

// Icom mode codes (payload byte of CI-V commands 0x04/0x06)
constexpr uint8_t MODE_LSB    = 0x00;
constexpr uint8_t MODE_USB    = 0x01;
constexpr uint8_t MODE_AM     = 0x02;
constexpr uint8_t MODE_CW     = 0x03;
constexpr uint8_t MODE_RTTY   = 0x04;
constexpr uint8_t MODE_FM     = 0x05;
constexpr uint8_t MODE_CW_R   = 0x07;
constexpr uint8_t MODE_RTTY_R = 0x08;

// IF filter selections
constexpr uint8_t FIL1 = 0x01;
constexpr uint8_t FIL2 = 0x02;

/**
 * Translate an Icom mode byte (+ the radio's data-mode flag) to radio_mode_t.
 * SSB with the data flag set becomes MODE_DATA/MODE_DATA_R (USB-D/LSB-D).
 * Returns false for mode codes SOTAcat doesn't handle (WFM, DV, ...).
 */
bool icom_to_radio_mode (uint8_t icom_mode, bool data_flag, radio_mode_t & out_mode);

/**
 * Translate a radio_mode_t to the Icom mode byte, IF filter, and data-mode
 * flag to set. MODE_DATA/MODE_DATA_R become USB/LSB with the data flag on;
 * CW modes select FIL2, everything else FIL1. Returns false for
 * MODE_UNKNOWN or values outside the enum.
 */
bool radio_mode_to_icom (radio_mode_t mode, uint8_t & out_icom_mode, uint8_t & out_filter, bool & out_data_flag);

}  // namespace ic705
