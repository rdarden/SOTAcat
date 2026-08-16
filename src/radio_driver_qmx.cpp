#include "radio_driver_qmx.h"
#include "kx_radio.h"
#include "hardware_specific.h"

#include <cstring>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
static const char * TAG8 = "sc:radio_qmx";

static bool puts_qmx (KXRadio & radio, const char * command) {
    return radio.put_to_kx_command_string (command, 1);
}

bool QMXRadioDriver::supports_keyer () const {
    return true;
}

bool QMXRadioDriver::supports_volume () const {
    return true;
}


bool QMXRadioDriver::get_frequency (KXRadio & radio, long & out_hz) {
    ESP_LOGD (TAG8, "QMX get_frequency");
    long frequency = radio.get_from_kx ("FA", SC_KX_COMMUNICATION_RETRIES, 11);
    if (frequency <= 0)
        return false;
    out_hz = frequency;
    return true;
}

bool QMXRadioDriver::set_frequency (KXRadio & radio, long hz, int tries) {
    return radio.put_to_kx ("FA", 11, hz, tries);
}

bool QMXRadioDriver::get_mode (KXRadio & radio, radio_mode_t & out_mode) {
    long mode = radio.get_from_kx ("MD", SC_KX_COMMUNICATION_RETRIES, 1);
    if (mode < MODE_UNKNOWN || mode > MODE_LAST)
        return false;
    out_mode = static_cast<radio_mode_t> (mode);
    return true;
}

bool QMXRadioDriver::set_mode (KXRadio & radio, radio_mode_t mode, int tries) {
    if (mode < MODE_UNKNOWN || mode > MODE_LAST)
        return false;
    return radio.put_to_kx ("MD", 1, mode, tries);
}

static constexpr long QMX_VOLUME_STEP_DB = 2; // volume step per web UI click when controlling QMX
static constexpr long QMX_VOLUME_STEP_UNITS = QMX_VOLUME_STEP_DB * 4; // 0.25 dB units per step

bool QMXRadioDriver::get_power (KXRadio & radio, long & out_power) {
    long power = radio.get_from_kx ("PC", SC_KX_COMMUNICATION_RETRIES, 3);
    if (power < 0)
        return false;
    out_power = power;
    return true;
}

bool QMXRadioDriver::set_power (KXRadio & radio, long power) {
    // QMX power commands are known to return nonstandard response formats for
    // the readback query, so send the power set without verification.
    return radio.put_to_kx ("PC", 3, power, 0);
}

bool QMXRadioDriver::get_volume (KXRadio & radio, long & out_volume) {
    long volume = radio.get_from_kx ("AG", SC_KX_COMMUNICATION_RETRIES, 3);
    if (volume < 0)
        return false;
    out_volume = volume;
    return true;
}

bool QMXRadioDriver::set_volume (KXRadio & radio, long delta) {
    // QMX expects an absolute AG0n value; the handler passes a delta amount.
    long current_volume = -1;
    if (!radio.get_volume (current_volume))
        return false;

    long target_volume = current_volume + delta * QMX_VOLUME_STEP_UNITS;
    if (target_volume < 0)
        target_volume = 0;
    if (target_volume > 255)
        target_volume = 255;

    ESP_LOGI (TAG8, "QMX volume: %ld + %ld*%ld = %ld", current_volume, delta, QMX_VOLUME_STEP_UNITS, target_volume);
    return radio.put_to_kx ("AG", 3, target_volume, SC_KX_COMMUNICATION_RETRIES);
}

bool QMXRadioDriver::get_xmit_state (KXRadio & radio, long & out_state) {
    long state = radio.get_from_kx ("TQ", SC_KX_COMMUNICATION_RETRIES, 1);
    if (state < 0)
        return false;
    out_state = state;
    return true;
}

bool QMXRadioDriver::set_xmit_state (KXRadio & radio, bool on) {
    const char * command = on ? "TX;" : "RX;";
    return puts_qmx (radio, command);
}

bool QMXRadioDriver::play_message_bank (KXRadio & radio, int bank) {
    (void) radio;
    (void) bank;
    return false;
}

bool QMXRadioDriver::tune_atu (KXRadio & radio) {
    (void) radio;
    return false;
}

/*
 * QMX keyer, per the QMX CAT programming manual (KY command, native mode --
 * i.e. TS480 compatibility OFF):
 *  - `KY <text>;` takes free-form text into an 80-character circular transmit
 *    buffer; further KY commands append while keying is in progress, so a
 *    typical SOTA exchange fits in a single command with no pacing gaps.
 *  - `KY;` (get) reports the buffer state: 0 = sending, buffer <= 75% full;
 *    1 = sending, > 75% full; 2 = idle/empty. KY0 guarantees at least 20
 *    characters free, which paces follow-on chunks of oversized messages.
 *  - Prosigns are sent as mapped single characters ([ = BT, _ = AR, etc.).
 */

// Longest chunk (up to `cap`) that ends at a word boundary when possible,
// mirroring next_ky_chunk_len() in radio_driver_kx.cpp.
static size_t next_qmx_ky_chunk_len (const char * pos, const char * end, size_t cap) {
    size_t remaining = (size_t)(end - pos);
    if (remaining <= cap)
        return remaining;
    const char * space = nullptr;
    for (size_t i = 0; i < cap; ++i) {
        if (pos[i] == ' ')
            space = pos + i;
    }
    if (space && space > pos)
        return (size_t)(space - pos);
    return cap;
}

// Read the KY transmit-buffer state: 0 = sending & <= 75% full, 1 = > 75%
// full, 2 = idle/empty; -1 if this firmware doesn't answer KY-get.
static long qmx_ky_state (KXRadio & radio) {
    return radio.get_from_kx ("KY", 1, 1);
}

// Fallback completion check for firmware without KY-get: poll TQ; with a
// stable-RX window, since TQ drops during CW inter-element and word gaps
// (same approach as the IC-705 driver).
static bool wait_for_qmx_tx_end (KXRadio & radio, TickType_t timeout_ms) {
    constexpr TickType_t POLL_INTERVAL_MS = 100;
    constexpr int        STABLE_RX_POLLS  = 6;  // ~600ms quiet; longer than a word gap at SOTA speeds
    const TickType_t     deadline_ticks   = xTaskGetTickCount() + pdMS_TO_TICKS (timeout_ms);

    int rx_streak = 0;
    while (true) {
        long tq = radio.get_from_kx ("TQ", SC_KX_COMMUNICATION_RETRIES, 1);
        if (tq == 0) {
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

bool QMXRadioDriver::send_keyer_message (KXRadio & radio, const char * message) {
    if (!message)
        return false;

    // Strip prosign markers, keeping their letters (so <AR> keys as "AR").
    // The CAT manual's prosign character map ([ = BT, _ = AR, ...) applies
    // only to TS480-compatibility mode; measured in native mode, those
    // characters key invalid garbage rather than prosigns, so no translation
    // is attempted (matches the KX driver's behavior).
    char   cleaned[128];
    size_t len = 0;
    for (const char * src = message; *src && len < sizeof (cleaned) - 1; ++src) {
        if (*src != '<' && *src != '>')
            cleaned[len++] = *src;
    }
    cleaned[len] = '\0';
    if (len == 0)
        return false;

    // First command carries up to 60 characters (comfortable margin in the
    // 80-char buffer), so a typical message goes out in one KY with zero
    // inter-chunk gaps. Oversized messages continue in <=20-char chunks,
    // sent only while the buffer reports <=75% full (KY0 => >=20 chars free).
    constexpr size_t FIRST_CHUNK_MAX = 60;
    constexpr size_t NEXT_CHUNK_MAX  = 20;

    const char * pos   = cleaned;
    const char * end   = cleaned + len;
    bool         first = true;
    while (pos < end) {
        if (!first) {
            // Pace follow-on chunks: KY0/KY2 mean >= 20 characters free; KY1
            // means wait. If KY-get isn't supported (-1), fall back to
            // waiting for the whole buffer to key out before appending.
            long       state    = qmx_ky_state (radio);
            TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS (30000);
            while (state == 1 && xTaskGetTickCount() < deadline) {
                vTaskDelay (pdMS_TO_TICKS (100));
                state = qmx_ky_state (radio);
            }
            if (state < 0)
                wait_for_qmx_tx_end (radio, 60000);
            else if (state == 1) {
                // Buffer still reports >75% full after the full grace period
                // -- something is stuck (hung firmware, wedged transport).
                // Sending the next chunk anyway risks the QMX silently
                // ignoring it (per the CAT manual, an over-full KY send is
                // just dropped with "?;"), truncating the message with no
                // trace. Fail loudly instead.
                ESP_LOGW (TAG8, "QMX keyer: KY buffer still full after 30s wait, aborting send");
                return false;
            }
        }
        size_t chunk_len = next_qmx_ky_chunk_len (pos, end, first ? FIRST_CHUNK_MAX : NEXT_CHUNK_MAX);
        if (chunk_len == 0)
            break;
        char command[72];  // "KY " + 60 chars + ";" + null
        snprintf (command, sizeof (command), "KY %.*s;", (int)chunk_len, pos);
        if (!puts_qmx (radio, command))
            return false;
        pos += chunk_len;
        first = false;
    }

    // Completion: wait for the transmit text buffer to drain (KY2), then let
    // the trailing element finish keying (short TQ wait). Falls back to TQ
    // polling alone if this firmware doesn't answer KY-get.
    vTaskDelay (pdMS_TO_TICKS (200));
    long state = qmx_ky_state (radio);
    if (state < 0)
        return wait_for_qmx_tx_end (radio, 60000);

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS (60000);
    while (state != 2 && xTaskGetTickCount() < deadline) {
        vTaskDelay (pdMS_TO_TICKS (100));
        long s = qmx_ky_state (radio);
        if (s >= 0)
            state = s;  // ignore transient read failures
    }
    if (state != 2)
        return false;
    wait_for_qmx_tx_end (radio, 3000);  // trailing element; best-effort
    return true;
}

bool QMXRadioDriver::sync_time (KXRadio & radio, const RadioTimeHms & client_time) {
    char cmd[32];
    snprintf (cmd, sizeof (cmd), "TM%02d%02d%02d;", client_time.hrs, client_time.min, client_time.sec);
    return puts_qmx (radio, cmd);
}

bool QMXRadioDriver::get_radio_state (KXRadio & radio, kx_state_t * state) {
    if (!state)
        return false;
    state->mode = MODE_UNKNOWN;
    state->active_vfo = 0;
    state->vfo_a_freq = 0;
    state->tun_pwr = 0;
    state->audio_peaking = 0;
    return true;
}

bool QMXRadioDriver::restore_radio_state (KXRadio & radio, const kx_state_t * state, int tries) {
    (void) radio;
    (void) state;
    (void) tries;
    return false;
}

bool QMXRadioDriver::ft8_prepare (KXRadio & radio, long rfFreq, int audioFreq) {
    // QMX does not allow frequency changes while in DIGI mode.
    // Set frequency FIRST (while in current mode), then switch to DIGI mode.
    
    // Store for use in ft8_set_tone
    m_ft8_rf_freq = rfFreq;
    m_ft8_audio_freq = audioFreq;
    
    // Set the USB dial frequency to the RF frequency only.
    // The TA command will handle the audio frequency offset.
    ESP_LOGI (TAG8, "QMX FT8: Attempting to set frequency FA=%ld", rfFreq);
    bool ok = radio.put_to_kx ("FA", 11, rfFreq, SC_KX_COMMUNICATION_RETRIES);
    if (!ok) {
        ESP_LOGE (TAG8, "QMX FT8: Failed to set frequency FA=%ld", rfFreq);
        return false;
    }
    ESP_LOGI (TAG8, "QMX FT8: Frequency set successfully to %ld Hz", rfFreq);

    // Now set DIGI mode (MD6) after frequency is locked in.
    // The TA command only works in Digi mode per QMX CAT manual.
    ESP_LOGI (TAG8, "QMX FT8: Attempting to set DIGI mode (MD6)");
    bool mode_ok = radio.put_to_kx ("MD", 1, 6, SC_KX_COMMUNICATION_RETRIES);
    if (!mode_ok) {
        ESP_LOGE (TAG8, "QMX FT8: Failed to set DIGI mode (MD6) - cannot proceed with FT8");
        return false;
    }
    ESP_LOGI (TAG8, "QMX FT8: DIGI mode (MD6) set successfully");
    
    // Give the radio time to settle in DIGI mode before starting transmission
    vTaskDelay (pdMS_TO_TICKS (100));

    ESP_LOGI (TAG8, "QMX FT8 prepared: RF_freq=%ld Hz, audio_freq=%d Hz, mode=DIGI(6)", rfFreq, audioFreq);
    return true;
}

void QMXRadioDriver::ft8_tone_on (KXRadio & radio) {
    // For QMX, enable transmit mode for FT8 tone transmission.
    // Uses the raw byte transport (UART or USB CDC) rather than the ASCII
    // command primitives to keep precise FT8 timing (160ms per tone).
    ESP_LOGI (TAG8, "CAT TX (tone_on): 'TX;'");
    radio.cat_write_bytes ((const uint8_t *)"TX;", sizeof ("TX;") - 1);
    radio.cat_flush_input();
}

void QMXRadioDriver::ft8_tone_off (KXRadio & radio) {
    // Per QMX CAT manual, proper FT8 key-up sequence:
    // 1. TA0; - key-up with shaped Blackman-Harris RF envelope
    // 2. Wait ~5ms for envelope shaping to finish
    // 3. RX; - return to receive mode
    // No mode switching needed - radio stays in DIGI mode between transmissions.
    ESP_LOGI (TAG8, "CAT TX (tone_off step 1): 'TA0;'");
    radio.cat_write_bytes ((const uint8_t *)"TA0;", sizeof ("TA0;") - 1);
    radio.cat_flush_input();
    vTaskDelay (pdMS_TO_TICKS (5));  // Wait for envelope shaping
    ESP_LOGI (TAG8, "CAT TX (tone_off step 2): 'RX;'");
    radio.cat_write_bytes ((const uint8_t *)"RX;", sizeof ("RX;") - 1);
    radio.cat_flush_input();
}

void QMXRadioDriver::ft8_set_tone (KXRadio & radio, long rfFreq, int audioFreq, long frequency) {
    // For QMX: TA<freq>; sets the transmit audio frequency (FSK tone frequency)
    // Use direct UART write to maintain precise FT8 timing (160ms per tone)
    // 
    // The handler passes: 
    //   frequency = rfFreq + audioFreq + tone_offset * 6.25 (absolute RF frequency)
    // We need to calculate:
    //   TA = audioFreq + (frequency - rfFreq - audioFreq) = frequency - rfFreq
    (void) audioFreq;  // Already stored in m_ft8_audio_freq if needed

    long audio_freq = frequency - rfFreq;
    
    // Clamp to reasonable FT8 audio range (0-3000 Hz)
    if (audio_freq < 0)
        audio_freq = 0;
    if (audio_freq > 3000)
        audio_freq = 3000;
    
    char command[16];
    snprintf (command, sizeof (command), "TA%ld;", audio_freq);
    ESP_LOGI (TAG8, "CAT TX (set_tone): '%s' (rf=%ld, target=%ld, audio=%ld)", command, rfFreq, frequency, audio_freq);
    radio.cat_write_bytes ((const uint8_t *)command, strlen (command));
    radio.cat_flush_input();
}
