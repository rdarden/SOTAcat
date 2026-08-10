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

// Fire-and-forget: flush pending input and send one frame. Returns true if the
// frame was built and written. Used for power-off, where the radio does not
// reliably ACK while shutting down.
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
