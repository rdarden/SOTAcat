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

bool QMXRadioDriver::send_keyer_message (KXRadio & radio, const char * message) {
    if (!message)
        return false;
    return puts_qmx (radio, message);
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
    // For QMX, enable transmit mode for FT8 tone transmission
    // Use direct UART write to maintain precise FT8 timing (160ms per tone)
    (void) radio;
    ESP_LOGI (TAG8, "CAT TX (tone_on): 'TX;'");
    uart_write_bytes (UART_NUM, "TX;", sizeof ("TX;") - 1);
    uart_flush (UART_NUM);
}

void QMXRadioDriver::ft8_tone_off (KXRadio & radio) {
    (void) radio;
    // Per QMX CAT manual, proper FT8 key-up sequence:
    // 1. TA0; - key-up with shaped Blackman-Harris RF envelope
    // 2. Wait ~5ms for envelope shaping to finish
    // 3. RX; - return to receive mode
    // No mode switching needed - radio stays in DIGI mode between transmissions.
    ESP_LOGI (TAG8, "CAT TX (tone_off step 1): 'TA0;'");
    uart_write_bytes (UART_NUM, "TA0;", sizeof ("TA0;") - 1);
    uart_flush (UART_NUM);
    vTaskDelay (pdMS_TO_TICKS (5));  // Wait for envelope shaping
    ESP_LOGI (TAG8, "CAT TX (tone_off step 2): 'RX;'");
    uart_write_bytes (UART_NUM, "RX;", sizeof ("RX;") - 1);
    uart_flush (UART_NUM);
}

void QMXRadioDriver::ft8_set_tone (KXRadio & radio, long rfFreq, int audioFreq, long frequency) {
    // For QMX: TA<freq>; sets the transmit audio frequency (FSK tone frequency)
    // Use direct UART write to maintain precise FT8 timing (160ms per tone)
    // 
    // The handler passes: 
    //   frequency = rfFreq + audioFreq + tone_offset * 6.25 (absolute RF frequency)
    // We need to calculate:
    //   TA = audioFreq + (frequency - rfFreq - audioFreq) = frequency - rfFreq
    (void) radio;
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
    uart_write_bytes (UART_NUM, (const char *) command, strlen (command));
    uart_flush (UART_NUM);
}
