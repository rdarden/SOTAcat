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
| IC-705 | `IC705RadioDriver` | CI-V binary codes | ❌ (deferred) | ✅ | `src/radio_driver_ic705.cpp` |

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

**Radio power (`PUT /api/v1/radioPower?state=0|1`):** CI-V `0x18` powers the
radio off/on, exposed for automated test workflows. Measured behavior (2026-08,
battery-less on external DC): power-**off** works, but the radio drops off the
USB bus ~2.5 s later and does not re-enumerate while off, so power-**on** over
USB cannot reach it. It also stays fully dark after a DC power cycle (no USB,
no WLAN standby — the RS-BA1 network server does not listen while off), so no
remote path can power the IC-705 on. Turning it back on needs the front-panel
button, after which USB re-enumerates and CAT resumes automatically. (The
`0xFE`-run wake + `0x18 0x01` ON sequence is implemented anyway; it can only
matter in the brief window before de-enumeration.)

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

## Adding a New Radio Driver

To add support for a new radio:

1. **Create header** (`include/radio_driver_xxx.h`):
   ```cpp
   class XXXRadioDriver : public RadioDriver {
   public:
       bool ft8_prepare(KXRadio & radio, long base_freq) override;
       void ft8_tone_on(KXRadio & radio) override;
       void ft8_tone_off(KXRadio & radio) override;
       void ft8_set_tone(KXRadio & radio, long base_freq, long frequency) override;
       // ... other methods
   };
   ```

2. **Create implementation** (`src/radio_driver_xxx.cpp`):
   - Implement all pure virtual methods
   - Refer to KX or QMX driver for pattern

3. **Register driver** in factory/initialization code

4. **Test thoroughly:**
   - Frequency changes
   - Mode switching
   - FT8 transmission (check logs for mode transitions and timing)

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
