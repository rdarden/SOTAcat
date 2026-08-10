/*
 * Icom IC-705 driver, speaking CI-V (binary) over the active CAT transport --
 * in practice the USB CDC-ACM pipe on the ESP32-S3-USB-OTG board.
 *
 * Protocol references: the Icom IC-705 Full/Advanced manuals (CI-V command
 * tables) and the hand-tested Python implementation in the author's
 * sota-chaser-python project (IcomIC705ControllerSerial), from which the
 * drain-and-keep-last response handling in civ_protocol.cpp was ported.
 */
#include "radio_driver_ic705.h"
#include "civ_protocol.h"
#include "kx_radio.h"

#include <cctype>
#include <cstring>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char * TAG8 = "sc:radio705";

// CI-V command bytes
static constexpr uint8_t CIV_CMD_GET_FREQ  = 0x03;
static constexpr uint8_t CIV_CMD_GET_MODE  = 0x04;
static constexpr uint8_t CIV_CMD_SET_FREQ  = 0x05;
static constexpr uint8_t CIV_CMD_SET_MODE  = 0x06;
static constexpr uint8_t CIV_CMD_CW_SEND   = 0x17;
static constexpr uint8_t CIV_CMD_SETTINGS  = 0x1A;
static constexpr uint8_t CIV_CMD_VOICE_TX  = 0x28;  // sub 0x00: play voice TX memory (0=stop, 1-8)
static constexpr uint8_t CIV_CMD_LEVEL     = 0x14;
static constexpr uint8_t CIV_CMD_METER     = 0x15;  // read-only meters; sub 0x12 = SWR
static constexpr uint8_t CIV_CMD_PTT       = 0x1C;  // 0x1C family: sub 0x00 = TX, sub 0x01 = tuner
static constexpr uint8_t CIV_SUB_DATA_MODE = 0x06;
static constexpr uint8_t CIV_SUB_AF_GAIN   = 0x01;
static constexpr uint8_t CIV_SUB_RF_POWER  = 0x0A;
static constexpr uint8_t CIV_SUB_ATU       = 0x01;  // status: 00=off, 01=on/matched, 02=tuning
static constexpr uint8_t CIV_SUB_SWR       = 0x12;  // SWR meter (0-255; ~48=1.5:1, ~80=2:1, ~120=3:1)
static constexpr uint8_t CIV_SUB_SETTINGS  = 0x05;  // 0x1A 0x05 <2-byte setting number> [data]

// IC-705 setting numbers for 0x1A 0x05 (BCD byte pairs, per the CI-V reference)
static constexpr uint8_t CIV_SET_TIME[2]       = { 0x01, 0x66 };  // clock hh mm (local)
static constexpr uint8_t CIV_SET_UTC_OFFSET[2] = { 0x01, 0x70 };  // hh mm + sign (01 = negative)

// Icom mode codes (payload byte of commands 0x04/0x06)
static constexpr uint8_t ICOM_MODE_LSB    = 0x00;
static constexpr uint8_t ICOM_MODE_USB    = 0x01;
static constexpr uint8_t ICOM_MODE_AM     = 0x02;
static constexpr uint8_t ICOM_MODE_CW     = 0x03;
static constexpr uint8_t ICOM_MODE_RTTY   = 0x04;
static constexpr uint8_t ICOM_MODE_FM     = 0x05;
static constexpr uint8_t ICOM_MODE_CW_R   = 0x07;
static constexpr uint8_t ICOM_MODE_RTTY_R = 0x08;

static constexpr uint8_t ICOM_FIL1 = 0x01;
static constexpr uint8_t ICOM_FIL2 = 0x02;

// CW send (0x17) accepts at most 30 characters per frame.
static constexpr size_t CW_CHUNK_MAX = 30;

bool IC705RadioDriver::supports_keyer () const {
    return true;
}

bool IC705RadioDriver::supports_volume () const {
    return true;
}

/*
 * CI-V "level" (0x14) and meter (0x15) commands carry a 0-255 value as
 * 4-digit big-endian BCD in two bytes: 255 -> 02 55, 42 -> 00 42.
 */
static bool get_civ_level (KXRadio & radio, uint8_t civ_cmd, uint8_t sub, long & out_level) {
    uint8_t cmd[]       = { civ_cmd, sub };
    uint8_t payload[8];
    size_t  payload_len = 0;
    if (!civ::transact (radio, cmd, sizeof (cmd), civ_cmd, sub, payload, sizeof (payload), payload_len))
        return false;
    if (payload_len < 2)
        return false;
    out_level = (payload[0] & 0x0F) * 100 + ((payload[1] >> 4) & 0x0F) * 10 + (payload[1] & 0x0F);
    return true;
}

static bool set_civ_level (KXRadio & radio, uint8_t sub, long level) {
    if (level < 0)
        level = 0;
    if (level > 255)
        level = 255;
    uint8_t cmd[] = { CIV_CMD_LEVEL, sub,
                      (uint8_t)(level / 100),
                      (uint8_t)((((level / 10) % 10) << 4) | (level % 10)) };
    return civ::send_expect_ack (radio, cmd, sizeof (cmd));
}

// Read the active VFO frequency (cmd 0x03, 5-byte little-endian BCD, 1 Hz).
bool IC705RadioDriver::get_frequency (KXRadio & radio, long & out_hz) {
    uint8_t       cmd[] = { CIV_CMD_GET_FREQ };
    uint8_t       payload[16];
    size_t        payload_len = 0;
    if (!civ::transact (radio, cmd, sizeof (cmd), CIV_CMD_GET_FREQ, -1, payload, sizeof (payload), payload_len))
        return false;
    if (payload_len < 5) {
        ESP_LOGW (TAG8, "short frequency payload (%u bytes)", (unsigned)payload_len);
        return false;
    }
    out_hz = civ::bcd_le_to_hz (payload, 5);
    return true;
}

// Set the active VFO frequency (cmd 0x05) and verify by reading it back,
// retrying up to `tries` times.
bool IC705RadioDriver::set_frequency (KXRadio & radio, long hz, int tries) {
    uint8_t cmd[6] = { CIV_CMD_SET_FREQ };
    civ::hz_to_bcd_le5 (hz, cmd + 1);

    int attempts = (tries > 0) ? tries : 1;
    for (int i = 0; i < attempts; ++i) {
        if (civ::send_expect_ack (radio, cmd, sizeof (cmd))) {
            long readback = 0;
            if (get_frequency (radio, readback) && readback == hz)
                return true;
            ESP_LOGW (TAG8, "set_frequency readback mismatch: wanted %ld", hz);
        }
        vTaskDelay (pdMS_TO_TICKS (30));
    }
    return false;
}

// Read the radio's data-mode flag (USB-D / LSB-D). Defaults to "off" if the
// query fails, so plain-mode reporting still works.
static bool get_data_mode_flag (KXRadio & radio) {
    uint8_t cmd[]       = { CIV_CMD_SETTINGS, CIV_SUB_DATA_MODE };
    uint8_t payload[8];
    size_t  payload_len = 0;
    if (!civ::transact (radio, cmd, sizeof (cmd), CIV_CMD_SETTINGS, CIV_SUB_DATA_MODE, payload, sizeof (payload), payload_len))
        return false;
    return payload_len >= 1 && payload[0] == 0x01;
}

bool IC705RadioDriver::get_mode (KXRadio & radio, radio_mode_t & out_mode) {
    uint8_t cmd[]       = { CIV_CMD_GET_MODE };
    uint8_t payload[8];
    size_t  payload_len = 0;
    if (!civ::transact (radio, cmd, sizeof (cmd), CIV_CMD_GET_MODE, -1, payload, sizeof (payload), payload_len))
        return false;
    if (payload_len < 1)
        return false;

    switch (payload[0]) {
    case ICOM_MODE_LSB: out_mode = MODE_LSB; break;
    case ICOM_MODE_USB: out_mode = MODE_USB; break;
    case ICOM_MODE_AM: out_mode = MODE_AM; break;
    case ICOM_MODE_CW: out_mode = MODE_CW; break;
    case ICOM_MODE_FM: out_mode = MODE_FM; break;
    case ICOM_MODE_CW_R: out_mode = MODE_CW_R; break;
    case ICOM_MODE_RTTY: out_mode = MODE_DATA; break;
    case ICOM_MODE_RTTY_R: out_mode = MODE_DATA_R; break;
    default:
        ESP_LOGW (TAG8, "unhandled Icom mode code 0x%02x", payload[0]);
        return false;
    }

    // SSB with the data flag set is USB-D/LSB-D -- report as DATA.
    if (out_mode == MODE_USB || out_mode == MODE_LSB) {
        if (get_data_mode_flag (radio))
            out_mode = (out_mode == MODE_USB) ? MODE_DATA : MODE_DATA_R;
    }
    return true;
}

bool IC705RadioDriver::set_mode (KXRadio & radio, radio_mode_t mode, int tries) {
    uint8_t icom_mode;
    uint8_t filter    = ICOM_FIL1;
    bool    data_flag = false;

    switch (mode) {
    case MODE_LSB: icom_mode = ICOM_MODE_LSB; break;
    case MODE_USB: icom_mode = ICOM_MODE_USB; break;
    case MODE_AM: icom_mode = ICOM_MODE_AM; break;
    case MODE_FM: icom_mode = ICOM_MODE_FM; break;
    case MODE_CW:
        icom_mode = ICOM_MODE_CW;
        filter    = ICOM_FIL2;
        break;
    case MODE_CW_R:
        icom_mode = ICOM_MODE_CW_R;
        filter    = ICOM_FIL2;
        break;
    case MODE_DATA:  // FT8 and friends: USB-D
        icom_mode = ICOM_MODE_USB;
        data_flag = true;
        break;
    case MODE_DATA_R:
        icom_mode = ICOM_MODE_LSB;
        data_flag = true;
        break;
    default:
        return false;
    }

    uint8_t mode_cmd[] = { CIV_CMD_SET_MODE, icom_mode, filter };
    // Always set the data flag explicitly: switching from DATA (USB-D) back to
    // plain USB must clear the "-D" on the radio, not just re-set USB.
    uint8_t data_cmd[] = { CIV_CMD_SETTINGS, CIV_SUB_DATA_MODE,
                           (uint8_t)(data_flag ? 0x01 : 0x00),
                           (uint8_t)(data_flag ? ICOM_FIL1 : 0x00) };

    int attempts = (tries > 0) ? tries : 1;
    for (int i = 0; i < attempts; ++i) {
        if (civ::send_expect_ack (radio, mode_cmd, sizeof (mode_cmd)) &&
            civ::send_expect_ack (radio, data_cmd, sizeof (data_cmd))) {
            radio_mode_t readback = MODE_UNKNOWN;
            if (get_mode (radio, readback) && readback == mode)
                return true;
            ESP_LOGW (TAG8, "set_mode readback mismatch: wanted %d got %d", mode, readback);
        }
        vTaskDelay (pdMS_TO_TICKS (30));
    }
    return false;
}

// The REST power API speaks watts (the UI sends 0 or 15, expecting radios to
// cap gracefully). The IC-705's CI-V scale is 0-255 = 0-100% of maximum power,
// nominally 10 W on external supply (5 W on battery -- the percent scale is of
// whichever max applies, so watt figures assume the 10 W scale).
static constexpr long IC705_MAX_WATTS = 10;

bool IC705RadioDriver::get_power (KXRadio & radio, long & out_power) {
    long level = 0;
    if (!get_civ_level (radio, CIV_CMD_LEVEL, CIV_SUB_RF_POWER, level))
        return false;
    out_power = (level * IC705_MAX_WATTS + 127) / 255;
    return true;
}

bool IC705RadioDriver::set_power (KXRadio & radio, long power) {
    if (power < 0)
        power = 0;
    if (power > IC705_MAX_WATTS)
        power = IC705_MAX_WATTS;  // e.g. the UI's KX3-shaped "15" request
    long level = (power * 255 + IC705_MAX_WATTS / 2) / IC705_MAX_WATTS;
    ESP_LOGI (TAG8, "IC-705 set power: %ld W -> level %ld/255", power, level);
    return set_civ_level (radio, CIV_SUB_RF_POWER, level);
}

bool IC705RadioDriver::get_volume (KXRadio & radio, long & out_volume) {
    return get_civ_level (radio, CIV_CMD_LEVEL, CIV_SUB_AF_GAIN, out_volume);
}

// One UI click per ~2.5% of the 0-255 AF range (the handler passes +/-1).
static constexpr long IC705_VOLUME_STEP_UNITS = 6;

bool IC705RadioDriver::set_volume (KXRadio & radio, long delta) {
    long current = 0;
    if (!get_civ_level (radio, CIV_CMD_LEVEL, CIV_SUB_AF_GAIN, current))
        return false;
    long target = current + delta * IC705_VOLUME_STEP_UNITS;
    ESP_LOGI (TAG8, "IC-705 volume: %ld + %ld*%ld -> %ld", current, delta, IC705_VOLUME_STEP_UNITS, target);
    return set_civ_level (radio, CIV_SUB_AF_GAIN, target);
}

// Read the PTT/transmit status (cmd 0x1C 0x00): 1 = transmitting.
bool IC705RadioDriver::get_xmit_state (KXRadio & radio, long & out_state) {
    uint8_t cmd[]       = { CIV_CMD_PTT, 0x00 };
    uint8_t payload[8];
    size_t  payload_len = 0;
    if (!civ::transact (radio, cmd, sizeof (cmd), CIV_CMD_PTT, 0x00, payload, sizeof (payload), payload_len))
        return false;
    if (payload_len < 1)
        return false;
    out_state = (payload[0] == 0x01) ? 1 : 0;
    return true;
}

// Key or unkey the transmitter (cmd 0x1C 0x00 <1|0>).
bool IC705RadioDriver::set_xmit_state (KXRadio & radio, bool on) {
    uint8_t cmd[] = { CIV_CMD_PTT, 0x00, (uint8_t)(on ? 0x01 : 0x00) };
    return civ::send_expect_ack (radio, cmd, sizeof (cmd));
}

bool IC705RadioDriver::play_message_bank (KXRadio & radio, int bank) {
    // Maps SOTAcat's message banks onto the radio's voice TX memories T1-T8
    // (the UI exposes 1 and 2). Voice memories only play in voice modes; the
    // radio rejects the command in CW/RTTY, which surfaces as an error.
    if (bank < 1 || bank > 8)
        return false;
    uint8_t cmd[] = { CIV_CMD_VOICE_TX, 0x00, (uint8_t)bank };
    return civ::send_expect_ack (radio, cmd, sizeof (cmd));
}

// Attempt a tune through the radio's native tuner protocol (0x1C 0x01),
// which works only for a tuner the radio recognizes on its control jack
// (genuine AH-705). Returns true on a confirmed match; false if the tuner
// never engaged or the tune failed -- measured behavior with no recognized
// tuner is that the enable/tune commands ACK but the status snaps back to
// "off" within ~300 ms without keying any RF.
static bool civ_tuner_tune (KXRadio & radio) {
    // The tune-start command is a no-op while the tuner function is off
    // (status 0x00), so switch the tuner on first if needed.
    uint8_t query[]     = { CIV_CMD_PTT, CIV_SUB_ATU };
    uint8_t payload[8];
    size_t  payload_len = 0;
    if (civ::transact (radio, query, sizeof (query), CIV_CMD_PTT, CIV_SUB_ATU, payload, sizeof (payload), payload_len) &&
        payload_len >= 1 && payload[0] == 0x00) {
        uint8_t enable[] = { CIV_CMD_PTT, CIV_SUB_ATU, 0x01 };
        if (!civ::send_expect_ack (radio, enable, sizeof (enable)))
            return false;
        vTaskDelay (pdMS_TO_TICKS (300));
    }

    uint8_t start[] = { CIV_CMD_PTT, CIV_SUB_ATU, 0x02 };
    if (!civ::send_expect_ack (radio, start, sizeof (start)))
        return false;

    // Give the tune cycle time to engage before trusting a non-tuning status.
    vTaskDelay (pdMS_TO_TICKS (750));

    for (int i = 0; i < 60; ++i) {  // up to ~15s more
        if (civ::transact (radio, query, sizeof (query), CIV_CMD_PTT, CIV_SUB_ATU, payload, sizeof (payload), payload_len) &&
            payload_len >= 1 && payload[0] != 0x02) {
            bool matched = (payload[0] == 0x01);
            ESP_LOGI (TAG8, "native tuner tune finished: %s", matched ? "matched" : "not engaged / failed");
            return matched;
        }
        vTaskDelay (pdMS_TO_TICKS (250));
    }
    ESP_LOGE (TAG8, "native tuner tune did not finish within timeout");
    return false;
}

// Fallback for RF-sensing third-party tuners (mAT-705, LDG, Elecraft T1...):
// key a reduced-power FM carrier for a few seconds so the tuner can detect RF
// and match, watching the radio's SWR meter to report an honest result. The
// operator's power setting and mode are restored afterward. (For tuners with
// a manual tune button, like the original mAT-705, press it first.)
static bool carrier_tune (KXRadio & radio, IC705RadioDriver & driver) {
    constexpr long TUNE_POWER_LEVEL = 77;   // ~30% = ~3 W: enough to tune, kind to the tuner
    constexpr long SWR_GOOD_MAX     = 100;  // meter units; ~2.5:1 (48=1.5, 80=2.0, 120=3.0)

    long         saved_power = -1;
    radio_mode_t saved_mode  = MODE_UNKNOWN;
    if (!get_civ_level (radio, CIV_CMD_LEVEL, CIV_SUB_RF_POWER, saved_power) ||
        !driver.get_mode (radio, saved_mode))
        return false;

    bool ok = set_civ_level (radio, CIV_SUB_RF_POWER, TUNE_POWER_LEVEL) &&
              driver.set_mode (radio, MODE_FM, SC_KX_COMMUNICATION_RETRIES) &&
              driver.set_xmit_state (radio, true);

    long last_swr = -1;
    if (ok) {
        // ~5s of carrier; sample the SWR meter as the tuner works.
        for (int i = 0; i < 10; ++i) {
            vTaskDelay (pdMS_TO_TICKS (500));
            long swr = -1;
            if (get_civ_level (radio, CIV_CMD_METER, CIV_SUB_SWR, swr) && swr >= 0)
                last_swr = swr;
        }
    }

    driver.set_xmit_state (radio, false);
    set_civ_level (radio, CIV_SUB_RF_POWER, saved_power);
    if (saved_mode != MODE_UNKNOWN)
        driver.set_mode (radio, saved_mode, SC_KX_COMMUNICATION_RETRIES);

    bool matched = ok && last_swr >= 0 && last_swr <= SWR_GOOD_MAX;
    ESP_LOGI (TAG8, "carrier tune done: keyed=%d final SWR meter=%ld -> %s",
              ok, last_swr, matched ? "matched" : "no match");
    return matched;
}

bool IC705RadioDriver::tune_atu (KXRadio & radio) {
    // Tuners only apply on HF/6m: the IC-705 bypasses its tuner jack on
    // 144/430 MHz, and the supported tuners (AH-705, mAT-705, T1) all top out
    // at 54 MHz. Refusing here also keeps the carrier fallback from keying a
    // pointless carrier on VHF/UHF.
    constexpr long TUNER_MAX_HZ = 54000000;
    long           hz           = 0;
    if (!get_frequency (radio, hz))
        return false;
    if (hz > TUNER_MAX_HZ) {
        ESP_LOGW (TAG8, "ATU tune refused: %ld Hz is above 6m; no tuner support on VHF/UHF", hz);
        return false;
    }

    if (civ_tuner_tune (radio))
        return true;
    ESP_LOGI (TAG8, "no native tuner result; falling back to carrier keying for RF-sensing tuners");
    return carrier_tune (radio, *this);
}

// Poll the PTT/TX status until the radio reports receive, or timeout.
// Mirrors wait_for_tx_end() in radio_driver_kx.cpp, with one difference:
// in CW break-in the IC-705's TX status drops between elements and words
// (measured on hardware), so a single TX=0 reading mid-message is not "done".
// Require several consecutive TX=0 polls before declaring the keying finished.
static bool wait_for_civ_tx_end (KXRadio & radio, IC705RadioDriver & driver, TickType_t timeout_ms) {
    constexpr TickType_t POLL_INTERVAL_MS = 100;
    constexpr int        STABLE_RX_POLLS  = 6;  // ~600ms quiet; longer than a word gap at SOTA speeds
    const TickType_t     deadline_ticks   = xTaskGetTickCount() + pdMS_TO_TICKS (timeout_ms);

    int rx_streak = 0;
    while (true) {
        long tx = -1;
        if (driver.get_xmit_state (radio, tx) && tx == 0) {
            if (++rx_streak >= STABLE_RX_POLLS)
                return true;
        }
        else {
            rx_streak = 0;
        }
        if (xTaskGetTickCount() >= deadline_ticks)
            return false;
        vTaskDelay (pdMS_TO_TICKS (POLL_INTERVAL_MS));
    }
}

// Longest chunk (up to CW_CHUNK_MAX) that ends at a word boundary when
// possible, so a chunk break lands on an inter-word gap instead of splitting
// a word mid-air. Same approach as next_ky_chunk_len() in radio_driver_kx.cpp.
static size_t next_cw_chunk_len (const char * pos, const char * end) {
    size_t remaining = (size_t)(end - pos);
    if (remaining <= CW_CHUNK_MAX)
        return remaining;

    const char * space = nullptr;
    for (size_t i = 0; i < CW_CHUNK_MAX; ++i) {
        if (pos[i] == ' ')
            space = pos + i;
    }
    if (space && space > pos)
        return (size_t)(space - pos);
    return CW_CHUNK_MAX;  // hard split: no whitespace in window
}

bool IC705RadioDriver::send_keyer_message (KXRadio & radio, const char * message) {
    if (!message)
        return false;

    // Keep only characters the IC-705 CW keyer accepts (per the CI-V 0x17
    // command spec): letters, digits, space, and / ? , .
    char   cleaned[128];
    size_t len = 0;
    for (const char * src = message; *src && len < sizeof (cleaned) - 1; ++src) {
        char c = (char)toupper ((unsigned char)*src);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == ' ' || c == '/' || c == '?' || c == ',' || c == '.')
            cleaned[len++] = c;
        else if (c != '<' && c != '>')
            ESP_LOGW (TAG8, "dropping unsupported CW character 0x%02x", (unsigned char)c);
    }
    cleaned[len] = '\0';
    if (len == 0)
        return false;

    const char * pos = cleaned;
    const char * end = cleaned + len;
    bool         ok  = true;

    while (pos < end) {
        size_t chunk_len = next_cw_chunk_len (pos, end);
        if (chunk_len == 0)
            break;

        uint8_t cmd[1 + CW_CHUNK_MAX];
        cmd[0] = CIV_CMD_CW_SEND;
        memcpy (cmd + 1, pos, chunk_len);
        if (!civ::send_expect_ack (radio, cmd, 1 + chunk_len)) {
            ESP_LOGE (TAG8, "CW send chunk rejected");
            ok = false;
            break;
        }
        pos += chunk_len;

        // Let keying begin before polling, then wait for this chunk to finish
        // keying before sending the next (0x17 buffers only ~30 characters).
        // Also keeps the keyer-active claim honest for the message duration.
        vTaskDelay (pdMS_TO_TICKS (200));
        if (!wait_for_civ_tx_end (radio, *this, 60000)) {
            ESP_LOGE (TAG8, "timed out waiting for CW transmission to end");
            ok = false;
            break;
        }
    }
    return ok;
}

static inline uint8_t to_bcd (int v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

bool IC705RadioDriver::sync_time (KXRadio & radio, const RadioTimeHms & client_time) {
    // client_time is UTC (see handler_time.cpp), but the IC-705's clock keeps
    // LOCAL time alongside a configured UTC-offset setting. Read the offset
    // and write correctly-offset local time so both the on-screen clock and
    // the radio's notion of UTC end up right.
    uint8_t off_query[] = { CIV_CMD_SETTINGS, CIV_SUB_SETTINGS, CIV_SET_UTC_OFFSET[0], CIV_SET_UTC_OFFSET[1] };
    uint8_t payload[8];
    size_t  payload_len = 0;
    if (!civ::transact (radio, off_query, sizeof (off_query), CIV_CMD_SETTINGS, CIV_SUB_SETTINGS, payload, sizeof (payload), payload_len))
        return false;
    // Measured on hardware: the IC-705 does NOT echo the setting number in
    // 0x1A 0x05 responses (matching its 0x1A 0x06 behavior) -- the payload is
    // the bare data: hh mm sign (sign 01 = negative offset). Tolerate an
    // echoing variant defensively in case other firmware revisions differ.
    const uint8_t * d = payload;
    if (payload_len >= 5 && payload[0] == CIV_SET_UTC_OFFSET[0] && payload[1] == CIV_SET_UTC_OFFSET[1])
        d = payload + 2;
    else if (payload_len != 3) {
        ESP_LOGW (TAG8, "unexpected UTC-offset payload (%u bytes)", (unsigned)payload_len);
        return false;
    }
    int  off_minutes = ((d[0] >> 4) * 10 + (d[0] & 0x0F)) * 60 + ((d[1] >> 4) * 10 + (d[1] & 0x0F));
    bool negative    = (d[2] == 0x01);

    // The clock setting carries only hh:mm, so round to the nearest minute.
    // (Rare edge: rounding across midnight leaves the date one day stale
    // until the next sync -- accepted for simplicity, as on the KX.)
    int total = client_time.hrs * 60 + client_time.min + (client_time.sec >= 30 ? 1 : 0);
    total += negative ? -off_minutes : off_minutes;
    total = ((total % 1440) + 1440) % 1440;

    uint8_t set_cmd[] = { CIV_CMD_SETTINGS, CIV_SUB_SETTINGS, CIV_SET_TIME[0], CIV_SET_TIME[1],
                          to_bcd (total / 60), to_bcd (total % 60) };
    ESP_LOGI (TAG8, "IC-705 clock set to %02d:%02d local (UTC offset %s%d min)",
              total / 60, total % 60, negative ? "-" : "+", off_minutes);
    return civ::send_expect_ack (radio, set_cmd, sizeof (set_cmd));
}

bool IC705RadioDriver::get_radio_state (KXRadio & radio, kx_state_t * state) {
    if (!state)
        return false;
    // Capture what this driver can restore (mode + VFO frequency); the
    // remaining fields are Elecraft menu concepts with no CI-V equivalent.
    radio_mode_t mode = MODE_UNKNOWN;
    long         hz   = 0;
    if (!get_mode (radio, mode) || !get_frequency (radio, hz))
        return false;
    state->mode          = mode;
    state->active_vfo    = 0;
    state->vfo_a_freq    = hz;
    state->tun_pwr       = 0;
    state->audio_peaking = 0;
    return true;
}

bool IC705RadioDriver::restore_radio_state (KXRadio & radio, const kx_state_t * state, int tries) {
    if (!state)
        return false;
    bool ok = true;
    if (state->vfo_a_freq > 0)
        ok = set_frequency (radio, state->vfo_a_freq, tries) && ok;
    if (state->mode != MODE_UNKNOWN)
        ok = set_mode (radio, state->mode, tries) && ok;
    return ok;
}

/*
 * FT8: the IC-705 has no CAT command for audio tone generation (the QMX's TA
 * command has no CI-V equivalent), so FSK is synthesized the way the KX driver
 * does it -- transmit a steady carrier and step the dial frequency for each of
 * the 79 tones. FM mode provides that carrier: PTT with no audio transmits an
 * unmodulated carrier at exactly the displayed frequency, and bench testing
 * confirmed the VFO retunes cleanly mid-transmit in FM. Each 160 ms tone step
 * is a fire-and-forget CI-V set-frequency frame (no ACK wait -- the next
 * send()'s input flush clears accumulated ACKs), keeping per-tone latency to
 * the frame transmission time.
 *
 * Power policy (deliberate): FT8 transmits at whatever RF POWER the operator
 * has set; this driver never adjusts it. The KX driver's forcing of TUN PWR
 * is an Elecraft-specific mechanical need (its TUNE carrier has a separate
 * power setting); the IC-705's FM carrier uses the normal RF POWER control,
 * so the operator's choice stands. (CI-V 0x14 0x0A is the hook if
 * programmatic power control is ever wanted.)
 */
bool IC705RadioDriver::ft8_prepare (KXRadio & radio, long rfFreq, int audioFreq) {
    long base = rfFreq + audioFreq;
    ESP_LOGI (TAG8, "IC-705 FT8 prepare: rf=%ld audio=%d -> carrier base %ld", rfFreq, audioFreq, base);
    if (!set_mode (radio, MODE_FM, SC_KX_COMMUNICATION_RETRIES)) {
        ESP_LOGE (TAG8, "FT8 prepare: failed to set FM mode");
        return false;
    }
    if (!set_frequency (radio, base, SC_KX_COMMUNICATION_RETRIES)) {
        ESP_LOGE (TAG8, "FT8 prepare: failed to set base frequency %ld", base);
        return false;
    }
    vTaskDelay (pdMS_TO_TICKS (100));  // let the radio settle before keying
    return true;
}

void IC705RadioDriver::ft8_tone_on (KXRadio & radio) {
    if (!set_xmit_state (radio, true))
        ESP_LOGE (TAG8, "FT8 tone_on: PTT command failed");
}

void IC705RadioDriver::ft8_tone_off (KXRadio & radio) {
    if (!set_xmit_state (radio, false))
        ESP_LOGE (TAG8, "FT8 tone_off: PTT release failed");
}

void IC705RadioDriver::ft8_set_tone (KXRadio & radio, long rfFreq, int audioFreq, long frequency) {
    // `frequency` is the absolute RF target for this tone; the FM carrier sits
    // exactly at the dial frequency, so just retune. Fire-and-forget for timing.
    (void) rfFreq;
    (void) audioFreq;
    uint8_t cmd[6] = { CIV_CMD_SET_FREQ };
    civ::hz_to_bcd_le5 (frequency, cmd + 1);
    civ::send (radio, cmd, sizeof (cmd));
}
