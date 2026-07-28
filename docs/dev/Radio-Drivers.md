# Radio Drivers

**Who this is for:** Developers working on radio driver implementations or FT8/FSK features

## Overview

SOTAcat supports multiple Elecraft radio models through a driver interface. Each radio has different CAT command implementations and capabilities. This document covers the driver architecture and specific implementation details for each supported radio.

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
