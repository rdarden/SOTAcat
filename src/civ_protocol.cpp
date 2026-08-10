/*
 * Transport-dependent half of the CI-V framing module: sending frames over
 * the active CAT transport and the drain-and-keep-last receive loops (see
 * include/civ_protocol.h for the rationale). The pure frame construction,
 * scanning, and BCD helpers live in civ_frames.cpp so they can be
 * unit-tested on the host.
 */
#include "civ_protocol.h"
#include "kx_radio.h"

#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>

static const char * TAG8 = "sc:civ.....";

namespace civ {

// Read slice granularity while draining. A CI-V response at 19200 baud arrives
// well inside one slice; a quiet slice after a match means the radio is done
// talking for now.
static constexpr int READ_SLICE_MS = 50;

bool send (KXRadio & radio, const uint8_t * cmd_and_data, size_t n) {
    uint8_t frame[MAX_FRAME];
    size_t  frame_len = build_frame (frame, sizeof (frame), cmd_and_data, n);
    if (frame_len == 0) {
        ESP_LOGE (TAG8, "CI-V frame too large (%u bytes of cmd+data)", (unsigned)n);
        return false;
    }
    radio.cat_flush_input();
    radio.cat_write_bytes (frame, (int)frame_len);
    return true;
}

// Shared drain-and-keep-last receive loop: keep reading (and scanning, via
// civ::scan_frames) until either the overall deadline passes, or -- once at
// least one matching frame is held -- the port goes quiet for one read slice.
//
// The accumulator is deliberately larger than any burst we expect: a single
// usb_serial_host_read_blocking() call truncates data that exceeds the
// caller's buffer (see usb_serial_host.cpp), so read into a roomy buffer
// rather than frame-sized pieces.
static bool drain_for_match (KXRadio & radio, const FrameMatch & want, uint8_t * frame_out, size_t & frame_len_out, int timeout_ms) {
    uint8_t acc[256];
    size_t  fill       = 0;
    bool    have_match = false;

    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (true) {
        int got = radio.cat_read_bytes (acc + fill, (int)(sizeof (acc) - fill), READ_SLICE_MS);
        if (got > 0)
            fill += (size_t)got;

        if (scan_frames (acc, fill, sizeof (acc), want, frame_out, frame_len_out))
            have_match = true;

        if (have_match && got <= 0)
            return true;  // port went quiet and we hold the last matching frame
        if (esp_timer_get_time() >= deadline_us)
            return have_match;
    }
}

bool transact (KXRadio & radio, const uint8_t * cmd_and_data, size_t n, uint8_t expect_cmd, int expect_sub, uint8_t * payload_out, size_t payload_cap, size_t & payload_len, int timeout_ms) {
    payload_len = 0;
    if (!send (radio, cmd_and_data, n))
        return false;

    FrameMatch want = { expect_cmd, expect_sub, false };
    uint8_t    frame[MAX_FRAME];
    size_t     frame_len = 0;
    if (!drain_for_match (radio, want, frame, frame_len, timeout_ms)) {
        ESP_LOGD (TAG8, "CI-V no response to cmd 0x%02x within %d ms", expect_cmd, timeout_ms);
        return false;
    }

    // frame = <to> <from> <cmd> [subcmd] [payload...]
    size_t skip = 3 + (expect_sub >= 0 ? 1 : 0);
    size_t len  = (frame_len > skip) ? frame_len - skip : 0;
    if (len > payload_cap)
        len = payload_cap;
    memcpy (payload_out, frame + skip, len);
    payload_len = len;
    return true;
}

bool send_expect_ack (KXRadio & radio, const uint8_t * cmd_and_data, size_t n, int timeout_ms) {
    if (!send (radio, cmd_and_data, n))
        return false;

    FrameMatch want = { 0, -1, true };
    uint8_t    frame[MAX_FRAME];
    size_t     frame_len = 0;
    if (!drain_for_match (radio, want, frame, frame_len, timeout_ms)) {
        ESP_LOGD (TAG8, "CI-V no ACK/NAK for cmd 0x%02x within %d ms", cmd_and_data[0], timeout_ms);
        return false;
    }
    if (frame[2] == NAK) {
        ESP_LOGW (TAG8, "CI-V NAK for cmd 0x%02x", cmd_and_data[0]);
        return false;
    }
    return true;
}

}  // namespace civ
