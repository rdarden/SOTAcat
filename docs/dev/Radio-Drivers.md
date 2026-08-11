# Radio Drivers

**Who this is for:** Developers working on radio driver implementations or FT8/FSK features

## Overview

SOTAcat supports multiple radio models through a driver interface. Each radio has different CAT command implementations and capabilities. This document covers the driver architecture and specific implementation details for each supported radio.

## Driver Architecture

### Base Classes

**`RadioDriver`** (`include/radio_driver.h`)
- Abstract base class defining the radio driver interface
- All radios must implement these methods:
  - `get_frequency()`, `set_frequency()`
  - `get_mode()`, `set_mode()`
  - `get_power()`, `set_power()`
  - `get_volume()`, `set_volume()`
  - `get_xmit_state()`, `set_xmit_state()`
  - `get_radio_state()`, `restore_radio_state()`
  - FT8 methods: `ft8_prepare()`, `ft8_tone_on()`, `ft8_set_tone()`, `ft8_tone_off()`
  - Keyer/ATU methods: `send_keyer_message()`, `play_message_bank()`, `tune_atu()`

**`KXRadio`** (`include/kx_radio.h`)
- Wrapper around the active radio driver
- Provides delegation to driver methods
- Handles locking and sequencing

### Supported Radios

| Radio | Class | Mode IDs | FT8 Support | Keyer | File |
|-------|-------|----------|-------------|-------|------|
| KX2/KX3 | `KXRadioDriver` | MD0-MD8 (USB, LSB, CW, etc.) | ✅ | ✅ | `src/radio_driver_kx.cpp` |
| KH1 | `KH1RadioDriver` | TBD | ✅ | ✅ | `src/radio_driver_kh1.cpp` |
| QMX | `QMXRadioDriver` | MD0-MD8+ | ✅ | ✅ | `src/radio_driver_qmx.cpp` |
| IC-705 | `IC705RadioDriver` | CI-V binary codes | ✅ | ✅ | `src/radio_driver_ic705.cpp` |

## IC-705 (Icom CI-V)

Unlike the ASCII Kenwood-style protocols above, the IC-705 speaks Icom's binary
CI-V protocol: frames of `FE FE <to> <from> <cmd> [data...] FD`, with the radio
at address `0xA4` and the controller at `0xE0`. The framing lives in
`src/civ_protocol.cpp`; the driver in `src/radio_driver_ic705.cpp` builds on it
via `KXRadio`'s byte-oriented transport methods (`cat_write_bytes` /
`cat_read_bytes`), since the ASCII primitives (`get_from_kx` etc.) can't carry
binary frames.

**Drain-and-keep-last reads:** the IC-705 emits unsolicited "transceive"
broadcast frames (destination `0x00`) whenever the operator turns the dial, and
responses can queue behind them. `civ::transact()` therefore drains the port
until it goes quiet, parses every frame, discards frames not addressed to us,
and keeps the *last* one matching the expected command — otherwise polled
frequency falls progressively behind the dial. Transceive is deliberately left
enabled on the radio (no `0x1A 0x05` settings writes).

**USB detection:** the IC-705 (VID `0x0C26`, PID `0x0036`) is a composite CDC
device behind the radio's internal USB hub (which also carries a separate USB
audio codec — hub support in sdkconfig is required and already enabled). It has
two CDC-ACM ports: interface pair 0/1 is CI-V (CAT), pair 2/3 is the
GPS/RS-232C port. `usb_serial_host` opens interface 0 first and
`KXRadio::connect()` confirms with a CI-V ID probe (`0x19 0x00` → model `0xA4`);
on no answer it rotates to the other interface. Non-Icom VIDs keep the QMX
`VN;` probe path.

**Mode mapping:** Icom modes (LSB `0x00`, USB `0x01`, AM `0x02`, CW `0x03`,
FM `0x05`, CW-R `0x07`) map to `radio_mode_t`; `MODE_DATA`/`MODE_DATA_R` map to
USB-D/LSB-D via the separate data-mode flag (`0x1A 0x06`), which the driver
always sets explicitly so leaving DATA actually clears the `-D` on the radio.
RTTY (`0x04`/`0x08`) is reported as DATA/DATA_R.

**CW keyer:** CI-V `0x17` sends ASCII text (max 30 chars/frame; charset
`A-Z 0-9 space / ? , .`). The driver chunks long messages at word boundaries
and polls TX status (`0x1C 0x00`) between chunks. The `0x17` command keys the
transmitter by itself (verified on hardware).

**Radio power:** measured (2026-08, battery-less on external DC): after a
CI-V power-off (`0x18 0x00`) the radio drops off the USB bus ~2.5 s later and
does not re-enumerate while off; after power-on from the front panel, USB
re-enumerates and CAT resumes automatically without a SOTAcat reboot. (A CAT
power-toggle endpoint was prototyped and bench-verified during development
but is deliberately not shipped — it isn't useful in field operation.)

**FT8:** the IC-705 has no CAT command for audio tone generation (no CI-V
equivalent of the QMX's `TA`), so FSK is synthesized KX-style: transmit a
steady carrier and step the dial for each of the 79 tones. FM mode provides
the carrier — PTT with no audio transmits an unmodulated carrier at exactly
the dial frequency, and the VFO retunes cleanly mid-transmit (bench-verified:
8/8 six-Hz steps while keyed; a full 79-tone transmission completes in the
canonical 12.68 s with no queue timeouts, and a SOTAmat-initiated
transmission was received and decoded correctly by an independent nearby
receiver). Each 160 ms tone step is a
fire-and-forget CI-V `0x05` set-frequency frame; the next frame's input flush
clears accumulated ACKs.

**FT8 power policy (deliberate):** the IC-705 transmits FT8 at whatever RF
POWER the operator has set — the driver never adjusts it. This differs from
the KX2/KX3 driver, which forces TUN PWR to 10 W, but that is a mechanical
necessity unique to Elecraft (the KX generates its FT8 carrier via the TUNE
function, governed by the separate TUN PWR menu rather than normal operating
power). The IC-705's FM carrier uses the ordinary RF POWER setting, so the
operator's deliberate power choice is respected. (CI-V `0x14 0x0A` is the
hook if programmatic power control is ever wanted.)
`get_radio_state`/`restore_radio_state` capture and restore mode + VFO
frequency so the pre-FT8 state comes back after transmission.

**RF power and volume:** CI-V level commands (`0x14`, two-byte BCD 0-255).
RF power (sub `0x0A`) is exposed in watts on the REST API assuming the 10 W
external-supply scale (the CI-V value is percent-of-max, so on battery the
same percentage yields up to 5 W); UI requests above 10 W cap gracefully,
matching KX2 behavior. AF volume (sub `0x01`) reports the raw 0-255 level and
steps ~5% per UI click.

**ATU tune** is two-stage. First the native tuner protocol: enable
(`0x1C 0x01 0x01`) then tune-start (`0x1C 0x01 0x02`), polling status until
it leaves "tuning" — this is the normal route for a tuner the radio
recognizes on its control jack: genuine AH-705, mAT-705Plus, or an Elecraft
T1 via an AH-705-emulating cable (verified on a real antenna with a
mAT-705Plus and with a T1 + Xteenna SpeedTune cable, including forced
re-tunes across band changes). When no tuner is recognized, the commands
ACK but status snaps back to "off" within ~300 ms with no RF keyed; the
driver then falls back to what RF-sensing tuners without control-jack
integration (original mAT-705, LDG, Elecraft T1...) need: it keys a ~3 W FM
carrier for 5 s so the tuner can match, reads the radio's SWR meter
(`0x15 0x12`) during the carrier, reports matched only if the final SWR is
≲2.5:1, and restores the operator's power and mode afterward. Tuners with a
manual tune button need it pressed before invoking ATU tune.

Tune requests above 54 MHz are refused outright: the IC-705 bypasses its
tuner jack on 144/430 MHz and all supported tuners top out at 6 m, so
neither the native cycle nor a fallback carrier serves any purpose there.

Bench-testing note: into a dummy load, every tune "matches" instantly (SWR
is already 1:1), which proves nothing about the tune cycle — validate tuner
behavior against a real antenna.

**Time sync:** the REST time API supplies UTC, but the IC-705 clock keeps
local time with a UTC-offset setting. The driver reads the offset
(`0x1A 0x05 0170`) and writes correctly-offset local time (`0x1A 0x05 0166`,
hh:mm only — rounded to the nearest minute, since the clock setting carries
no seconds). Note the radio omits the setting-number echo in `0x1A 0x05`
responses — the payload is bare data.

**Message banks:** the UI's M1/M2 buttons play the radio's voice TX
memories T1/T2 via CI-V `0x28 0x00 <n>` (1-8 accepted). The radio rejects
playback in non-voice modes and for empty memories.

With this, the IC-705 driver implements the complete IRadioDriver surface.

**Bench debugging tips (ESP32-S3-USB-OTG):** the default console is USB
Serial/JTAG, which goes silent once USB host mode claims the PHY. For serial
logs/panics during bench work, edit the generated (gitignored)
`sdkconfig.esp32_s3_usb_otg_debug`: set `CONFIG_ESP_CONSOLE_UART_DEFAULT=y`,
`CONFIG_ESP_CONSOLE_UART_NUM=0`, and unset `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG*`
— logs then appear on the board's CP2102 port at 115200. Do not put this in
`sdkconfig.defaults` (it would move the C3 envs' console too). Beware: opening
that CP2102 port toggles DTR/RTS and **resets the board** (macOS does this
regardless of application settings), so keep one monitor process open across a
session rather than reopening per check.

## FT8 Implementation

### Overall Sequence

FT8 transmission is managed by [handler_ft8.cpp](../../src/handler_ft8.cpp). The typical flow is:

```
User clicks "Self-Spot FT8" or calls /api/v1/prepareft8
    ↓
ft8_prepare() called — radio setup (mode, frequency, etc.)
    ↓
ft8_tone_on() called — start transmit
    ↓
ft8_set_tone() called 79 times (160ms intervals) — transmit FSK tones
    ↓
ft8_tone_off() called — stop transmit and clean up
    ↓
cleanup_ft8_task() — restore radio state
```

### Radio state save/restore

FT8 is the **only** client of `get_radio_state()` / `restore_radio_state()`
(the `kx_state_t` snapshot in `include/kx_radio.h`) — nothing else in SOTAcat
captures or restores radio state. The lifecycle:

1. **Capture**: `prepareft8` snapshots the radio via `get_radio_state()`
   before anything is touched.
2. **Mutate**: `ft8_prepare()` retunes and switches mode (USB on Elecraft,
   FM on IC-705); the KX driver additionally forces TUN PWR to 10 W.
3. **Restore** via `restore_radio_state()` in three places: immediately if
   prepare fails partway; in `cleanup_ft8_task()` after the transmission
   sequence ends; and on `cancelft8`.

Each driver snapshots what *its own* FT8 path perturbs — the depth
differences are deliberate, not gaps:

| Field | KX2/KX3 | KH1 | IC-705 | QMX |
|---|---|---|---|---|
| Mode | ✅ | — | ✅ | zeroed |
| VFO A frequency | ✅ | ✅ | ✅ | zeroed |
| Active VFO | ✅ | — | — | zeroed |
| TUN PWR (menu 58) | ✅ | — | n/a | zeroed |
| Audio peaking (APF) | ✅ | — | n/a | zeroed |

The KX needs the extra fields because its FT8 commandeers the TUN PWR menu
setting and mode-jumping can disturb a CW operator's APF; the IC-705's FT8
touches only mode and frequency (power is left at the operator's setting by
policy, and CI-V has no TUN-PWR/APF analogues), so its leaner snapshot is
complete for everything its FT8 changes. The QMX driver zero-fills the
snapshot and returns `false` from restore — after FT8 the QMX stays in DIGI
mode at the FT8 frequency.

### Per-Radio Details

#### KX2/KX3 Radio Driver

**File:** `src/radio_driver_kx.cpp`

Uses MD0 (USB) mode for FT8 transmission.

**Sequence:**
- `ft8_prepare()`: Set frequency (FA), set mode to USB (MD0)
- `ft8_tone_on()`: Send TX; command
- `ft8_set_tone()`: Send TA (Transmit Audio) commands with tone frequency
- `ft8_tone_off()`: Send TA0; then RX; to key-up and return to RX

#### KH1 Radio Driver

**File:** `src/radio_driver_kh1.cpp`

Similar to KX driver; see source for implementation details.

#### QMX Radio Driver

**File:** `src/radio_driver_qmx.cpp`

**Known QMX-over-USB quirk (measured, 2026-08):** if the ESP32-S3 resets
while a QMX stays powered and attached, the QMX's USB stack keeps stale
session state and every subsequent enumeration fails
(`CHECK_SHORT_DEV_DESC`) until the **radio itself** is power-cycled. This is
a QMX firmware issue, not fixable host-side: the QMX does not sense VBUS
(port power cycling is invisible to it — verified with repeated VBUS kicks)
and repeated bus resets don't clear the stuck state. The IC-705 is
unaffected by host resets. Worth reporting upstream to QRP Labs.

The QMX requires special handling per its CAT manual. Key differences:

**Mode Selection:**
- Uses **MD6 (DIGI/FSK mode)** instead of USB mode for FT8
- The `TA` (Transmit Audio) command only works in DIGI mode per QMX CAT documentation

**FT8 Sequence (per QMX CAT Manual):**

1. **Prepare:** Set frequency and switch to DIGI mode
   ```cpp
   FA<freq>;  // Set USB dial frequency (11 digits)
   MD6;       // Switch to DIGI mode
   ```

2. **Start transmission:** Switch to TX
   ```cpp
   TX;        // Enter transmit mode
   ```

3. **Transmit tones:** Send audio frequency commands
   ```cpp
   TA<freq>;  // Set audio tone (float Hz)
   ```
   - First `TA` command keys down with shaped Blackman-Harris RF envelope
   - Subsequent `TA` commands change the tone frequency
   - Total: 79 tone commands at 160ms intervals for full FT8 message

4. **Stop transmission:** Shaped key-up and return to RX
   ```cpp
   TA0;       // Shaped key-up with Blackman-Harris RF envelope
   // Wait ~5ms for envelope shaping to complete
   RX;        // Return to receive mode
   ```

**Important Notes:**
- **RF Frequency:** The actual RF frequency is: `FA (USB dial) + TA (audio frequency)`
  - Example: `FA14075000;` + `TA1500;` = 14,076,500 Hz RF
- **Shaped Envelopes:** Using `TA0;` for key-up instead of hard `RX;` produces a proper Blackman-Harris shaped RF envelope, which is better for FT8 than an instant hard key-off
- **Mode Persistence:** After FT8, radio remains in DIGI mode (MD6) until explicitly changed back

**Implementation Details:**

```cpp
bool QMXRadioDriver::ft8_prepare(KXRadio & radio, long base_freq) {
    // Set USB dial frequency
    radio.put_to_kx("FA", 11, base_freq, SC_KX_COMMUNICATION_RETRIES);
    
    // Set DIGI mode (MD6) — required for TA command
    radio.put_to_kx("MD", 1, 6, SC_KX_COMMUNICATION_RETRIES);
}

void QMXRadioDriver::ft8_tone_off(KXRadio & radio) {
    uart_write_bytes(UART_NUM, "TA0;", sizeof("TA0;") - 1);  // Shaped key-up
    uart_flush(UART_NUM);
    vTaskDelay(pdMS_TO_TICKS(5));     // Wait for envelope
    uart_write_bytes(UART_NUM, "RX;", sizeof("RX;") - 1);    // Return to RX
    uart_flush(UART_NUM);
}
```

**CAT Command Timing:**
- Direct UART writes are used for TA, TX, RX commands (bypasses CAT interface delays for precision)
- MD mode commands use CAT interface with verification for reliability

**Future work (QMX)** — both verified feasible on real hardware (QMX
firmware 1.04.005, bench 2026-08-10):

- **Band coverage:** QMX hardware ships in band-group variants (and the QMX+
  covers 160m–6m), so `RADIO_CAPABILITIES` in `main.js` deliberately has no
  static QMX entry — band filtering is permissive. TODO: query the connected
  unit and populate an entry dynamically. **Verified recipe (read-only, no
  VFO disturbance):** the Menu Manager Get command reads the Band
  Configuration table per column, e.g. `MMBand config.|Band name (m)[3];`
  → `MM12;` (12m in column 3). Useful rows: `Band name (m)`,
  `Frequency min.`, `Frequency center`, `Frequency max.`, `Transmit`
  (ENABLED/DISABLED). Unconfigured columns return `0`/`DISABLED`; 16 columns
  total. The `BN`/`BN<n>` band-number command also exists (set of a
  non-configured index errors with `?;`) but it moves the VFO and refused
  the configured 11m column on the bench unit, so the MM table read is the
  authoritative path.
- **AM mode:** recent QMX firmware supports AM (`MD5;`), normally hidden
  behind a menu setting. On the bench unit the CAT set `MD5;` was accepted
  regardless. **Firmware quirk (measured, 1.04.005): once in AM, every
  subsequent `MD` set returns `?;` until an `MU;` (reload configuration
  parameters) is issued — after `MU;` the radio reverts to its configured
  mode.** Any future AM support must handle this escape; also worth
  reporting to QRP Labs.

## Adding a New Radio Driver

To add support for a new radio:

1. **Create the driver** (`include/radio_driver_xxx.h` +
   `src/radio_driver_xxx.cpp`): subclass `IRadioDriver`
   (`include/radio_driver.h`) and implement every pure-virtual method —
   return `false` for operations the radio can't support (see
   `QMXRadioDriver` for the minimal pattern, `IC705RadioDriver` for a full
   binary-protocol implementation).

2. **Register the type**: add a value to the `RadioType` enum and a case to
   `get_radio_type_string()` (`include/kx_radio.h`), a static driver
   instance and a branch in `KXRadio::select_driver()` (`src/kx_radio.cpp`).

3. **Add detection**: a probe in `KXRadio::connect()` — on the USB-host
   path, branch on the enumerated VID (`usb_serial_host_get_vid()`) before
   probing; on the UART path, extend the baud-scan ladder.

4. **Gate the web UI**: add a `RADIO_CAPABILITIES` entry (bands/modes) in
   `src/web/main.js`, and entries in `RADIO_UNSUPPORTED_FEATURES` /
   `RADIO_MSG_BANKS` in `src/web/run.js` for anything the driver returns
   `false` for.

5. **Test thoroughly** against real hardware: frequency and mode round-trips
   (including a dial-spin test while polling), keyer, TX state, FT8 timing —
   and see the bench-testing notes above for pitfalls (dummy-load tunes,
   console access on the S3 board).

## CAT Command Reference (QMX)

Common commands used in FT8 operation:

| Command | Format | Description | Example |
|---------|--------|-------------|---------|
| FA | FA`nnnnnnnnnnn`;  | Set frequency (11 digits, USB) | FA00014075000; = 14,075,000 Hz |
| MD | MD`n`;  | Set mode (0-9) | MD6; = DIGI mode |
| TX | TX;  | Start transmit | TX; |
| RX | RX;  | Stop transmit | RX; |
| TA | TA`nnnn.nn`;  | Set audio tone (float Hz) | TA1500; = 1500 Hz |
| MD | MD;  | Query current mode | MD; → MD6; |

---

[← Architecture](Architecture.md) · [BUILD →](BUILD.md)
