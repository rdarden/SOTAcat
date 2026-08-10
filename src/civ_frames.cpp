/*
 * Pure (platform-independent) half of the CI-V framing module: frame
 * construction, BCD frequency conversion, and the receive-side frame scanner.
 * Split from civ_protocol.cpp so this logic can be compiled and unit-tested
 * on the host (test/unit/test_civ_protocol.cpp) without ESP-IDF, following
 * the radio_detection.cpp precedent. The transport-dependent half (send /
 * transact / drain loops) lives in civ_protocol.cpp.
 */
#include "civ_protocol.h"

#include <cstring>

namespace civ {

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

bool scan_frames (uint8_t * acc, size_t & fill, size_t cap, const FrameMatch & want, uint8_t * frame_out, size_t & frame_len_out) {
    bool   have_match = false;
    size_t scan       = 0;

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
        }
        // else: broadcast/echo/other destination -- discard silently

        scan = term + 1;
    }

    // Drop consumed bytes, keep any partial frame tail.
    if (scan > 0) {
        memmove (acc, acc + scan, fill - scan);
        fill -= scan;
    }
    else if (fill == cap) {
        // Accumulator full with no terminator in sight: garbage; reset.
        fill = 0;
    }

    return have_match;
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
