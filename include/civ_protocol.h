#pragma once

/*
 * Icom CI-V binary framing over the radio's active CAT transport.
 *
 * Frame format (controller -> radio):
 *     FE FE <radio addr> <controller addr> <cmd> [subcmd] [data...] FD
 * and (radio -> controller):
 *     FE FE <controller addr> <radio addr> <cmd> [subcmd] [payload...] FD
 *
 * A one-byte reply of ACK (0xFB) or NAK (0xFA) in the command position
 * acknowledges set-type commands.
 *
 * The radio also emits unsolicited "transceive" broadcast frames (destination
 * address 0x00) whenever the operator turns the dial or changes mode.  These
 * interleave freely with command responses, and responses can back up behind
 * them, so a naive send-one/read-one exchange falls progressively behind the
 * radio's real state.  transact() therefore drains the port until it goes
 * quiet, parses every complete frame seen, discards frames not addressed to
 * us, and keeps the LAST frame matching the expected command.  (This mirrors
 * the proven behavior of the reference Python implementation.)
 */

#include <cstddef>
#include <cstdint>

class KXRadio;

namespace civ {

constexpr uint8_t PREAMBLE   = 0xFE;
constexpr uint8_t TERMINATOR = 0xFD;
constexpr uint8_t RADIO_ADDR = 0xA4;  // IC-705 default CI-V address
constexpr uint8_t CTRL_ADDR  = 0xE0;  // conventional PC/controller address
constexpr uint8_t ACK        = 0xFB;
constexpr uint8_t NAK        = 0xFA;

// Largest CI-V frame we ever exchange: preamble(2) + addrs(2) + cmd/subcmd(2)
// + 30-char CW text + terminator(1), rounded up.
constexpr size_t MAX_FRAME = 48;

// Build "FE FE <to> <from> <cmd_and_data...> FD" into out.
// Returns the frame length, or 0 if it would not fit in cap.
size_t build_frame (uint8_t * out, size_t cap, const uint8_t * cmd_and_data, size_t n);

// Selection criteria for scan_frames()/drain_for_match(): which received
// frame counts as the answer we're waiting for.
struct FrameMatch {
    uint8_t expect_cmd;  // command byte to accept (ignored when ack_nak)
    int     expect_sub;  // required subcommand byte, or -1 for none
    bool    ack_nak;     // true: match ACK/NAK frames instead of expect_cmd
};

// Pure frame scanner (host-unit-testable; see test/unit/test_civ_protocol.cpp).
// Consumes every complete "FE FE ... FD" frame currently in acc[0..fill),
// compacting the unconsumed tail to the front and updating fill. Frames not
// addressed to CTRL_ADDR (transceive broadcasts, echoes) are discarded; of the
// frames addressed to us, the LAST one satisfying `want` AND fitting within
// frame_out_cap is copied to frame_out (body only: <to> <from> <cmd>
// [data...]). An oversized matching frame (noise, or a reply larger than any
// command we send) is skipped rather than copied, so callers with a
// MAX_FRAME-sized frame_out are safe regardless of what the accumulator
// holds. If the buffer fills to `cap` without a terminator, the garbage is
// discarded. Returns true if at least one matching, in-bounds frame was
// found in this call.
bool scan_frames (uint8_t * acc, size_t & fill, size_t cap, const FrameMatch & want, uint8_t * frame_out, size_t frame_out_cap, size_t & frame_len_out);

// Fire-and-forget: flush pending input and send one frame. Returns true if the
// frame was built and written.
bool send (KXRadio & radio, const uint8_t * cmd_and_data, size_t n);

// Send a command and return the payload of the response.
//
// Drain-and-keep-last semantics (see file header): reads until the port has
// been quiet for a beat, keeps the LAST frame addressed to CTRL_ADDR whose
// command byte equals expect_cmd (and, when expect_sub >= 0, whose next byte
// equals expect_sub). The payload bytes after cmd[/subcmd] are copied to
// payload_out. Returns false on timeout without a matching frame, or if the
// radio answered the command with NAK.
bool transact (KXRadio &       radio,
               const uint8_t * cmd_and_data,
               size_t          n,
               uint8_t         expect_cmd,
               int             expect_sub,
               uint8_t *       payload_out,
               size_t          payload_cap,
               size_t &        payload_len,
               int             timeout_ms = 300);

// Send a set-type command and wait for the ACK/NAK reply (same drain parser).
// Returns true on ACK, false on NAK or timeout.
bool send_expect_ack (KXRadio & radio, const uint8_t * cmd_and_data, size_t n, int timeout_ms = 500);

// Frequency <-> 5-byte little-endian packed BCD (1 Hz resolution, 10 digits).
// e.g. 14060000 Hz -> 00 00 06 14 00 (low byte holds the two least-significant
// decimal digits).
void hz_to_bcd_le5 (long hz, uint8_t out[5]);
long bcd_le_to_hz (const uint8_t * bcd, size_t n);

}  // namespace civ
