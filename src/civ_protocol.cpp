#include "civ_protocol.h"
#include "kx_radio.h"

#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>

static const char * TAG8 = "sc:civ.....";

namespace civ {

// Read slice granularity while draining. A CI-V response at 19200 baud arrives
// well inside one slice; two quiet slices in a row means the radio is done
// talking for now.
static constexpr int READ_SLICE_MS = 50;

size_t build_frame (uint8_t * out, size_t cap, const uint8_t * cmd_and_data, size_t n) {
    size_t frame_len = 4 + n + 1;  // FE FE to from ... FD
    if (frame_len > cap)
        return 0;
    out[0] = PREAMBLE;
    out[1] = PREAMBLE;
    out[2] = RADIO_ADDR;
    out[3] = CTRL_ADDR;
    memcpy (out + 4, cmd_and_data, n);
    out[4 + n] = TERMINATOR;
    return frame_len;
}

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

// Shared drain-and-keep-last receive loop. Scans the incoming byte stream for
// complete frames, ignores frames not addressed to CTRL_ADDR (transceive
// broadcasts to 0x00, or another controller's traffic), and remembers the last
// frame accepted by `match`. Keeps reading until either the overall deadline
// passes, or -- once at least one match is held -- the port goes quiet for one
// read slice.
//
// The accumulator is deliberately larger than any burst we expect: a single
// usb_serial_host_read_blocking() call truncates data that exceeds the caller's
// buffer (see usb_serial_host.cpp), so read into a roomy buffer rather than
// frame-sized pieces.
struct FrameMatch {
    uint8_t expect_cmd;      // command byte to accept, or 0 to accept ACK/NAK
    int     expect_sub;      // required subcommand byte, or -1 for none
    bool    ack_nak;         // true: match ACK/NAK frames instead of expect_cmd
};

static bool drain_for_match (KXRadio & radio, const FrameMatch & want, uint8_t * frame_out, size_t & frame_len_out, int timeout_ms) {
    uint8_t acc[256];
    size_t  fill      = 0;
    bool    have_match = false;

    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (true) {
        int got = radio.cat_read_bytes (acc + fill, (int)(sizeof (acc) - fill), READ_SLICE_MS);
        if (got > 0)
            fill += (size_t)got;

        // Extract every complete frame currently in the accumulator.
        size_t scan = 0;
        while (true) {
            // Find the start of a frame: the first FE FE pair at or after scan.
            while (scan + 1 < fill && !(acc[scan] == PREAMBLE && acc[scan + 1] == PREAMBLE))
                ++scan;
            if (scan + 1 >= fill)
                break;
            // Skip any extra preamble bytes (wake-up runs are longer than 2).
            size_t body = scan + 2;
            while (body < fill && acc[body] == PREAMBLE)
                ++body;
            // Find the terminator.
            size_t term = body;
            while (term < fill && acc[term] != TERMINATOR)
                ++term;
            if (term >= fill)
                break;  // incomplete frame; wait for more bytes

            // Frame body: <to> <from> <cmd> [data...] at acc[body..term-1]
            size_t body_len = term - body;
            if (body_len >= 3 && acc[body] == CTRL_ADDR) {
                uint8_t cmd     = acc[body + 2];
                bool    matches = false;
                if (want.ack_nak)
                    matches = (cmd == ACK || cmd == NAK);
                else if (cmd == want.expect_cmd)
                    matches = (want.expect_sub < 0) ||
                              (body_len >= 4 && acc[body + 3] == (uint8_t)want.expect_sub);
                if (matches) {
                    frame_len_out = body_len;
                    memcpy (frame_out, acc + body, body_len);
                    have_match = true;
                }
                else {
                    ESP_LOGD (TAG8, "ignoring CI-V frame for cmd 0x%02x", cmd);
                }
            }
            // else: broadcast/echo/other destination -- discard silently

            scan = term + 1;
        }

        // Drop consumed bytes, keep any partial frame tail.
        if (scan > 0) {
            memmove (acc, acc + scan, fill - scan);
            fill -= scan;
        }
        else if (fill == sizeof (acc)) {
            // Accumulator full with no terminator in sight: garbage; reset.
            ESP_LOGW (TAG8, "CI-V accumulator overflow without terminator; discarding");
            fill = 0;
        }

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

void hz_to_bcd_le5 (long hz, uint8_t out[5]) {
    for (int i = 0; i < 5; ++i) {
        uint8_t low  = (uint8_t)(hz % 10);
        uint8_t high = (uint8_t)((hz / 10) % 10);
        out[i]       = (uint8_t)((high << 4) | low);
        hz /= 100;
    }
}

long bcd_le_to_hz (const uint8_t * bcd, size_t n) {
    long value = 0;
    long scale = 1;
    for (size_t i = 0; i < n; ++i) {
        value += (bcd[i] & 0x0F) * scale;
        value += ((bcd[i] >> 4) & 0x0F) * scale * 10;
        scale *= 100;
    }
    return value;
}

}  // namespace civ
