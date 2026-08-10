// Host unit tests for the IC-705 mode translation (src/ic705_modes.cpp):
// Icom CI-V mode byte + data-mode flag <-> radio_mode_t, both directions.
//
// Build & run via `make test-unit` (compiled with the host g++, no ESP-IDF).

#include "ic705_modes.h"

#include <cassert>

static void test_icom_to_radio () {
    radio_mode_t m;

    assert (ic705::icom_to_radio_mode (ic705::MODE_LSB, false, m) && m == MODE_LSB);
    assert (ic705::icom_to_radio_mode (ic705::MODE_USB, false, m) && m == MODE_USB);
    assert (ic705::icom_to_radio_mode (ic705::MODE_AM, false, m) && m == MODE_AM);
    assert (ic705::icom_to_radio_mode (ic705::MODE_CW, false, m) && m == MODE_CW);
    assert (ic705::icom_to_radio_mode (ic705::MODE_FM, false, m) && m == MODE_FM);
    assert (ic705::icom_to_radio_mode (ic705::MODE_CW_R, false, m) && m == MODE_CW_R);

    // RTTY aliases onto the DATA modes for the web API
    assert (ic705::icom_to_radio_mode (ic705::MODE_RTTY, false, m) && m == MODE_DATA);
    assert (ic705::icom_to_radio_mode (ic705::MODE_RTTY_R, false, m) && m == MODE_DATA_R);

    // SSB + data flag = USB-D/LSB-D, reported as DATA/DATA_R
    assert (ic705::icom_to_radio_mode (ic705::MODE_USB, true, m) && m == MODE_DATA);
    assert (ic705::icom_to_radio_mode (ic705::MODE_LSB, true, m) && m == MODE_DATA_R);

    // Data flag is meaningless for non-SSB modes and must not corrupt them
    assert (ic705::icom_to_radio_mode (ic705::MODE_CW, true, m) && m == MODE_CW);
    assert (ic705::icom_to_radio_mode (ic705::MODE_FM, true, m) && m == MODE_FM);

    // Unhandled codes (WFM 0x06, DV 0x17) are rejected
    assert (!ic705::icom_to_radio_mode (0x06, false, m));
    assert (!ic705::icom_to_radio_mode (0x17, false, m));
}

static void test_radio_to_icom () {
    uint8_t icom, fil;
    bool    data;

    assert (ic705::radio_mode_to_icom (MODE_USB, icom, fil, data));
    assert (icom == ic705::MODE_USB && fil == ic705::FIL1 && !data);

    assert (ic705::radio_mode_to_icom (MODE_LSB, icom, fil, data));
    assert (icom == ic705::MODE_LSB && fil == ic705::FIL1 && !data);

    // CW modes take the narrower FIL2
    assert (ic705::radio_mode_to_icom (MODE_CW, icom, fil, data));
    assert (icom == ic705::MODE_CW && fil == ic705::FIL2 && !data);
    assert (ic705::radio_mode_to_icom (MODE_CW_R, icom, fil, data));
    assert (icom == ic705::MODE_CW_R && fil == ic705::FIL2 && !data);

    // DATA modes become SSB with the data flag on (USB-D/LSB-D)
    assert (ic705::radio_mode_to_icom (MODE_DATA, icom, fil, data));
    assert (icom == ic705::MODE_USB && fil == ic705::FIL1 && data);
    assert (ic705::radio_mode_to_icom (MODE_DATA_R, icom, fil, data));
    assert (icom == ic705::MODE_LSB && fil == ic705::FIL1 && data);

    assert (ic705::radio_mode_to_icom (MODE_AM, icom, fil, data));
    assert (icom == ic705::MODE_AM && !data);
    assert (ic705::radio_mode_to_icom (MODE_FM, icom, fil, data));
    assert (icom == ic705::MODE_FM && !data);

    assert (!ic705::radio_mode_to_icom (MODE_UNKNOWN, icom, fil, data));
    assert (!ic705::radio_mode_to_icom ((radio_mode_t)8, icom, fil, data));  // unused enum gap
}

static void test_round_trip () {
    // Every settable radio_mode_t must survive the Icom round trip.
    const radio_mode_t modes[] = { MODE_LSB, MODE_USB, MODE_CW, MODE_FM, MODE_AM, MODE_DATA, MODE_CW_R, MODE_DATA_R };
    for (radio_mode_t m : modes) {
        uint8_t icom, fil;
        bool    data;
        assert (ic705::radio_mode_to_icom (m, icom, fil, data));
        radio_mode_t back;
        assert (ic705::icom_to_radio_mode (icom, data, back));
        assert (back == m);
    }
}

int main () {
    test_icom_to_radio();
    test_radio_to_icom();
    test_round_trip();
    return 0;
}
