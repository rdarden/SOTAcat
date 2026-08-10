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
static constexpr uint8_t CIV_CMD_POWER     = 0x18;
static constexpr uint8_t CIV_CMD_GET_ID    = 0x19;
static constexpr uint8_t CIV_CMD_SETTINGS  = 0x1A;
static constexpr uint8_t CIV_CMD_PTT       = 0x1C;
static constexpr uint8_t CIV_SUB_DATA_MODE = 0x06;

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

static constexpr uint8_t IC705_MODEL_ID = 0xA4;  // model 164; same value as the default CI-V address

// CW send (0x17) accepts at most 30 characters per frame.
static constexpr size_t CW_CHUNK_MAX = 30;

bool IC705RadioDriver::supports_keyer () const {
    return true;
}

bool IC705RadioDriver::supports_volume () const {
    return false;
}

bool IC705RadioDriver::supports_power_toggle () const {
    return true;
}

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

bool IC705RadioDriver::get_power (KXRadio & radio, long & out_power) {
    (void) radio;
    (void) out_power;
    return false;  // RF power control deferred (CI-V 0x14 0x0A when needed)
}

bool IC705RadioDriver::set_power (KXRadio & radio, long power) {
    (void) radio;
    (void) power;
    return false;
}

bool IC705RadioDriver::get_volume (KXRadio & radio, long & out_volume) {
    (void) radio;
    (void) out_volume;
    return false;
}

bool IC705RadioDriver::set_volume (KXRadio & radio, long volume) {
    (void) radio;
    (void) volume;
    return false;
}

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

bool IC705RadioDriver::set_xmit_state (KXRadio & radio, bool on) {
    uint8_t cmd[] = { CIV_CMD_PTT, 0x00, (uint8_t)(on ? 0x01 : 0x00) };
    return civ::send_expect_ack (radio, cmd, sizeof (cmd));
}

bool IC705RadioDriver::set_radio_power (KXRadio & radio, bool on) {
    if (on) {
        // Wake-up sequence: a run of 0xFE preamble bytes lets the sleeping CPU
        // sync to the data rate before the actual power-on frame arrives.
        // Icom specifies ~25-30 bytes at 19200 baud (more at higher rates).
        //
        // NOTE: over USB this can only succeed if the radio is still enumerated,
        // and measurement shows the IC-705 drops off the USB bus ~2.5s after
        // power-off -- so in practice ON works only via the wired CI-V jack.
        // Kept for correctness and for the brief window before de-enumeration.
        uint8_t wake[30];
        memset (wake, civ::PREAMBLE, sizeof (wake));
        radio.cat_flush_input();
        radio.cat_write_bytes (wake, sizeof (wake));

        uint8_t cmd[] = { CIV_CMD_POWER, 0x01 };
        civ::send (radio, cmd, sizeof (cmd));

        // The radio takes several seconds to boot; poll the ID probe until it
        // answers or we give up.
        uint8_t id_cmd[]    = { CIV_CMD_GET_ID, 0x00 };
        uint8_t payload[8];
        size_t  payload_len = 0;
        for (int i = 0; i < 30; ++i) {
            vTaskDelay (pdMS_TO_TICKS (500));
            if (civ::transact (radio, id_cmd, sizeof (id_cmd), CIV_CMD_GET_ID, 0x00, payload, sizeof (payload), payload_len) &&
                payload_len >= 1 && payload[payload_len - 1] == IC705_MODEL_ID) {
                ESP_LOGI (TAG8, "IC-705 powered on and responding");
                return true;
            }
        }
        ESP_LOGE (TAG8, "IC-705 did not respond after power-on attempt");
        return false;
    }

    // Power off: the radio does not reliably ACK while shutting down, so a
    // successful transport write is the best confirmation available.
    uint8_t cmd[] = { CIV_CMD_POWER, 0x00 };
    bool    sent  = civ::send (radio, cmd, sizeof (cmd));
    if (sent)
        ESP_LOGI (TAG8, "IC-705 power-off command sent");
    return sent;
}

bool IC705RadioDriver::play_message_bank (KXRadio & radio, int bank) {
    (void) radio;
    (void) bank;
    return false;
}

bool IC705RadioDriver::tune_atu (KXRadio & radio) {
    (void) radio;
    return false;
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

bool IC705RadioDriver::sync_time (KXRadio & radio, const RadioTimeHms & client_time) {
    (void) radio;
    (void) client_time;
    return false;
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
 * the frame transmission time. TX power is whatever RF POWER the radio is set
 * to; this driver does not adjust it.
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
