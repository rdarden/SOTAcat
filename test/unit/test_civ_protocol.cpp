// Host unit tests for the pure half of the CI-V framing module
// (src/civ_frames.cpp): frame construction, BCD frequency conversion, and
// the drain-and-keep-last frame scanner.
//
// Build & run via `make test-unit` (compiled with the host g++, no ESP-IDF).

#include "civ_protocol.h"

#include <cassert>
#include <cstring>

using namespace civ;

// Convenience: append a complete frame to a buffer. to/from are raw address
// bytes so tests can construct broadcasts and echoes as well as replies.
static size_t put_frame (uint8_t * buf, size_t at, uint8_t to, uint8_t from, const uint8_t * body, size_t body_len) {
    buf[at++] = PREAMBLE;
    buf[at++] = PREAMBLE;
    buf[at++] = to;
    buf[at++] = from;
    memcpy (buf + at, body, body_len);
    at += body_len;
    buf[at++] = TERMINATOR;
    return at;
}

static void test_build_frame () {
    uint8_t out[MAX_FRAME];
    uint8_t cmd[] = { 0x19, 0x00 };

    size_t len = build_frame (out, sizeof (out), cmd, sizeof (cmd));
    assert (len == 7);
    const uint8_t expect[] = { 0xFE, 0xFE, 0xA4, 0xE0, 0x19, 0x00, 0xFD };
    assert (memcmp (out, expect, len) == 0);

    // Oversized command data must be rejected, not truncated.
    uint8_t big[MAX_FRAME] = { 0 };
    assert (build_frame (out, sizeof (out), big, sizeof (big)) == 0);
}

static void test_bcd_round_trips () {
    const long cases[] = { 0, 1, 52, 7030000, 14060000, 21074700, 146520000, 449999999, 9999999999L };
    for (long hz : cases) {
        uint8_t bcd[5];
        hz_to_bcd_le5 (hz, bcd);
        assert (bcd_le_to_hz (bcd, 5) == hz);
    }

    // Known layout: 14,060,000 Hz -> 00 00 06 14 00 (little-endian digit pairs)
    uint8_t bcd[5];
    hz_to_bcd_le5 (14060000, bcd);
    const uint8_t expect[] = { 0x00, 0x00, 0x06, 0x14, 0x00 };
    assert (memcmp (bcd, expect, 5) == 0);

    // Partial-width decode (cmd 0x25 style payloads use fewer bytes)
    const uint8_t two[] = { 0x34, 0x12 };
    assert (bcd_le_to_hz (two, 2) == 1234);
}

static void test_scan_basic_match_and_payload () {
    uint8_t acc[256];
    size_t  fill = 0;
    const uint8_t body[] = { 0x03, 0x00, 0x00, 0x06, 0x14, 0x00 };  // freq reply
    fill = put_frame (acc, fill, CTRL_ADDR, RADIO_ADDR, body, sizeof (body));

    FrameMatch want = { 0x03, -1, false };
    uint8_t    frame[MAX_FRAME];
    size_t     frame_len = 0;
    assert (scan_frames (acc, fill, sizeof (acc), want, frame, frame_len));
    assert (fill == 0);  // fully consumed
    assert (frame_len == 2 + sizeof (body));
    assert (frame[0] == CTRL_ADDR && frame[1] == RADIO_ADDR && frame[2] == 0x03);
    assert (bcd_le_to_hz (frame + 3, 5) == 14060000);
}

static void test_scan_discards_broadcasts_and_keeps_last () {
    // Transceive broadcasts (to 0x00) interleave with two replies to us; the
    // scanner must ignore the broadcasts and keep the LAST matching reply --
    // the behavior that stops polled frequency lagging behind the dial.
    uint8_t acc[256];
    size_t  fill = 0;
    const uint8_t bcast[] = { 0x00, 0x00, 0x00, 0x07, 0x14, 0x00 };
    const uint8_t old_reply[] = { 0x03, 0x00, 0x00, 0x06, 0x14, 0x00 };
    const uint8_t new_reply[] = { 0x03, 0x52, 0x00, 0x06, 0x14, 0x00 };
    fill = put_frame (acc, fill, 0x00, RADIO_ADDR, bcast, sizeof (bcast));
    fill = put_frame (acc, fill, CTRL_ADDR, RADIO_ADDR, old_reply, sizeof (old_reply));
    fill = put_frame (acc, fill, 0x00, RADIO_ADDR, bcast, sizeof (bcast));
    fill = put_frame (acc, fill, CTRL_ADDR, RADIO_ADDR, new_reply, sizeof (new_reply));

    FrameMatch want = { 0x03, -1, false };
    uint8_t    frame[MAX_FRAME];
    size_t     frame_len = 0;
    assert (scan_frames (acc, fill, sizeof (acc), want, frame, frame_len));
    assert (bcd_le_to_hz (frame + 3, 5) == 14060052);  // the LAST reply won
}

static void test_scan_subcommand_matching () {
    uint8_t acc[256];
    size_t  fill = 0;
    const uint8_t wrong_sub[] = { 0x1A, 0x05, 0x01 };
    const uint8_t right_sub[] = { 0x1A, 0x06, 0x01, 0x01 };
    fill = put_frame (acc, fill, CTRL_ADDR, RADIO_ADDR, wrong_sub, sizeof (wrong_sub));
    fill = put_frame (acc, fill, CTRL_ADDR, RADIO_ADDR, right_sub, sizeof (right_sub));

    FrameMatch want = { 0x1A, 0x06, false };
    uint8_t    frame[MAX_FRAME];
    size_t     frame_len = 0;
    assert (scan_frames (acc, fill, sizeof (acc), want, frame, frame_len));
    assert (frame[3] == 0x06);

    // Sub-command mismatch alone must not match.
    fill = put_frame (acc, 0, CTRL_ADDR, RADIO_ADDR, wrong_sub, sizeof (wrong_sub));
    assert (!scan_frames (acc, fill, sizeof (acc), want, frame, frame_len));
}

static void test_scan_ack_nak () {
    uint8_t acc[256];
    size_t  fill = 0;
    const uint8_t ack_body[] = { ACK };
    fill = put_frame (acc, fill, CTRL_ADDR, RADIO_ADDR, ack_body, sizeof (ack_body));

    FrameMatch want = { 0, -1, true };
    uint8_t    frame[MAX_FRAME];
    size_t     frame_len = 0;
    assert (scan_frames (acc, fill, sizeof (acc), want, frame, frame_len));
    assert (frame[2] == ACK);

    const uint8_t nak_body[] = { NAK };
    fill = put_frame (acc, 0, CTRL_ADDR, RADIO_ADDR, nak_body, sizeof (nak_body));
    assert (scan_frames (acc, fill, sizeof (acc), want, frame, frame_len));
    assert (frame[2] == NAK);  // NAK matches too; caller distinguishes
}

static void test_scan_partial_frames_across_reads () {
    // A frame split across two reads must survive the first scan intact and
    // complete on the second.
    uint8_t acc[256];
    uint8_t whole[64];
    const uint8_t body[] = { 0x04, 0x03, 0x02 };  // mode reply
    size_t whole_len = put_frame (whole, 0, CTRL_ADDR, RADIO_ADDR, body, sizeof (body));

    size_t split = 4;  // mid-frame
    size_t fill  = split;
    memcpy (acc, whole, split);

    FrameMatch want = { 0x04, -1, false };
    uint8_t    frame[MAX_FRAME];
    size_t     frame_len = 0;
    assert (!scan_frames (acc, fill, sizeof (acc), want, frame, frame_len));
    assert (fill == split);  // partial frame retained

    memcpy (acc + fill, whole + split, whole_len - split);
    fill += whole_len - split;
    assert (scan_frames (acc, fill, sizeof (acc), want, frame, frame_len));
    assert (frame[2] == 0x04 && frame[3] == 0x03);
}

static void test_scan_garbage_and_preamble_runs () {
    uint8_t acc[256];
    size_t  fill = 0;

    // Leading garbage, then a long preamble run (wake-style), then a frame.
    const uint8_t junk[] = { 0x12, 0x34, 0x56 };
    memcpy (acc, junk, sizeof (junk));
    fill = sizeof (junk);
    acc[fill++] = PREAMBLE;
    acc[fill++] = PREAMBLE;
    acc[fill++] = PREAMBLE;
    acc[fill++] = PREAMBLE;  // extra preamble bytes beyond the standard two
    acc[fill++] = CTRL_ADDR;
    acc[fill++] = RADIO_ADDR;
    acc[fill++] = 0x03;
    acc[fill++] = TERMINATOR;

    FrameMatch want = { 0x03, -1, false };
    uint8_t    frame[MAX_FRAME];
    size_t     frame_len = 0;
    assert (scan_frames (acc, fill, sizeof (acc), want, frame, frame_len));
    assert (frame_len == 3);
    assert (fill == 0);
}

static void test_scan_overflow_resets () {
    FrameMatch want = { 0x03, -1, false };
    uint8_t    frame[MAX_FRAME];
    size_t     frame_len = 0;

    // Garbage with no preamble pair is consumed by the scan itself (at most
    // one trailing byte is kept in case it starts a preamble).
    uint8_t small[32];
    memset (small, 0x55, sizeof (small));
    size_t fill = sizeof (small);
    assert (!scan_frames (small, fill, sizeof (small), want, frame, frame_len));
    assert (fill <= 1);

    // An unterminated frame that fills the whole buffer is unrecoverable:
    // it must be discarded so the stream can resynchronize.
    small[0] = PREAMBLE;
    small[1] = PREAMBLE;
    memset (small + 2, 0x55, sizeof (small) - 2);  // frame body, never a terminator
    fill = sizeof (small);
    assert (!scan_frames (small, fill, sizeof (small), want, frame, frame_len));
    assert (fill == 0);
}

int main () {
    test_build_frame();
    test_bcd_round_trips();
    test_scan_basic_match_and_payload();
    test_scan_discards_broadcasts_and_keeps_last();
    test_scan_subcommand_matching();
    test_scan_ack_nak();
    test_scan_partial_frames_across_reads();
    test_scan_garbage_and_preamble_runs();
    test_scan_overflow_resets();
    return 0;
}
