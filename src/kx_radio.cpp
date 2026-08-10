#include "kx_radio.h"
#include "civ_protocol.h"
#include "globals.h"
#include "hardware_specific.h"
#include "radio_driver_ic705.h"
#include "radio_driver_kh1.h"
#include "radio_driver_kx.h"
#include "radio_driver_qmx.h"
#include "radio_detection.h"
#include "timed_lock.h"

#include <cstdlib>
#include <cstring>
#include <driver/uart.h>
#include <esp_timer.h>

#ifdef ESP32_S3
    #include "usb_serial_host.h"
    #include "usb_host_display.h"
#endif

/*
 * See https://ftp.elecraft.com/KX2/Manuals%20Downloads/K3S&K3&KX3&KX2%20Pgmrs%20Ref,%20G4.pdf
 * for full KX command documentation
 *
 * Example commands:
 *   APn; - Get the current Audio Peaking filter setting for CW: 0 for APF OFF and 1 for APF ON
 *   MDn; - Get the current mode: 1 (LSB), 2 (USB), 3 (CW), 4 (FM), 5 (AM), 6 (DATA), 7 (CWREV), or 9 (DATA-REV)
 *   FTn; - Get the current VFO:  0 for VFO A, 1 for VFO B
 *   MN058;MP; - Get the current TUN PWR setting
 *   FAnnnnnnnnnnn; - Get the current frequency A
 */

#include <esp_log.h>
static const char * TAG8 = "sc:kx_radio";

// Global static instance
KXRadio & kxRadio = KXRadio::getInstance();

static KXRadioDriver    g_kx_driver;
static KH1RadioDriver   g_kh1_driver;
static QMXRadioDriver   g_qmx_driver;
static IC705RadioDriver g_ic705_driver;

// UART timeouts for radio commands
// Short commands (status checks): 100ms is sufficient
// Long commands (frequency changes): Radio needs time to settle VFO, use 2000ms
#define KX_TIMEOUT_MS_SHORT_COMMANDS 100
#define KX_TIMEOUT_MS_LONG_COMMANDS  2000

/*
 * Utilities
 */

// True once connect() has selected the USB CDC pipe (see usb_serial_host.h) as the
// active CAT transport instead of the wired ACC UART. All the retry/parsing logic
// below is transport-agnostic; only these three functions know which one is in use.
static bool g_use_usb_transport = false;

static void cat_flush () {
#ifdef ESP32_S3
    if (g_use_usb_transport) {
        // No separate flush concept for the USB CDC queue; just drain anything pending.
        uint8_t discard[64];
        while (usb_serial_host_read (discard, sizeof (discard)) > 0) {}
        return;
    }
#endif
    uart_flush (UART_NUM);
}

static void cat_write (const char * data, int len) {
#ifdef ESP32_S3
    if (g_use_usb_transport) {
        usb_serial_host_write ((const uint8_t *)data, (size_t)len);
        return;
    }
#endif
    uart_write_bytes (UART_NUM, data, len);
}

static int cat_read (uint8_t * buf, int max_len, int wait_ms) {
#ifdef ESP32_S3
    if (g_use_usb_transport)
        return usb_serial_host_read_blocking (buf, (size_t)max_len, wait_ms);
#endif
    return uart_read_bytes (UART_NUM, buf, max_len, pdMS_TO_TICKS (wait_ms));
}

// Public byte-oriented transport access, for binary protocols (Icom CI-V) that
// can't go through the ASCII command primitives.
void KXRadio::cat_flush_input () {
    if (!is_locked())
        ESP_LOGE (TAG8, "RADIO NOT LOCKED! (coding error in caller)");
    cat_flush();
}

int KXRadio::cat_write_bytes (const uint8_t * data, int len) {
    if (!is_locked())
        ESP_LOGE (TAG8, "RADIO NOT LOCKED! (coding error in caller)");
    cat_write ((const char *)data, len);
    return len;
}

int KXRadio::cat_read_bytes (uint8_t * buf, int max_len, int wait_ms) {
    if (!is_locked())
        ESP_LOGE (TAG8, "RADIO NOT LOCKED! (coding error in caller)");
    return cat_read (buf, max_len, wait_ms);
}

/**
 * Sends a command via the active CAT transport (USB CDC or wired UART), reads the
 * response, checks for validity, and retries if necessary. Handles errors like the
 * device being busy and logs detailed communication status.
 *
 * @param cmd Command to be sent, expressed as a null-terminated string.
 * @param response Buffer to store the response.
 * @param expected_chars Expected number of characters in the response.
 * @param tries Number of retries for the command.
 * @param wait_ms Milliseconds to wait for a response.
 * @return bool True if successful, false otherwise.
 */
static bool uart_get_command (const char * command, char * response, int expected_chars, int tries, int wait_ms) {
    ESP_LOGV (TAG8, "trace: %s(command='%s', expect=%d)", __func__, command, expected_chars);

    cat_flush ();
    int command_length = strlen (command);
    cat_write (command, command_length);  // Send command

    int64_t start_time     = esp_timer_get_time();
    int     returned_chars = cat_read ((uint8_t *)response, expected_chars, wait_ms);
    int64_t end_time       = esp_timer_get_time();
    float   elapsed_ms     = (end_time - start_time) / 1000.0;

    // Null-terminate the response buffer safely
    if (returned_chars > 0)
        if (returned_chars < expected_chars)
            response[returned_chars] = '\0';  // Normally, terminate after the last character in the response
        else
            response[expected_chars] = '\0';  // When we exceed expecations, terminate at the expected size
    else
        response[0] = '\0';  // No characters received, so ensure it's an empty string

    ESP_LOGD (TAG8, "command '%s' returned %d chars, '%s', after %.3f ms", command, returned_chars, response, elapsed_ms);

    // Return if valid response achieved
    if (response[0] == command[0] && response[1] == command[1] &&  // got what we asked for
        returned_chars == expected_chars &&                        // as much as we wanted
        (response[expected_chars - 1] == ';' ||
         (strncmp (command, "AG", 2) == 0 && response[0] == 'A' && response[1] == 'G')))
        return true;                                               // success

    // Invalid response, retry
    ESP_LOGE (TAG8, "bad response from command '%s' after %.3f ms, expected %d bytes, received %d bytes, response=%c%c%c%c%c%c...", command, elapsed_ms, expected_chars, returned_chars, response[0], response[1], response[2], response[3], response[4], response[5]);
    if ((returned_chars == 2 && response[0] == '?' && response[1] == ';') ||  // radio busy, don't count as retry
        --tries > 0) {
        ESP_LOGI (TAG8, "Retrying...");
        kxRadio.empty_kx_input_buffer (wait_ms);
        vTaskDelay (pdMS_TO_TICKS (30));  // Delay before retrying
        return uart_get_command (command, response, expected_chars, tries - 1, wait_ms);
    }
    return false;
}

/**
 * Parses a numeric response based on the expected format and number of digits.
 *
 * @param response Buffer containing the response.
 * @param num_digits Number of digits expected in the response.
 * @return long Parsed numeric value from the response.
 */
static long parse_response (const char * response, int num_digits) {
    switch (num_digits) {
    case 1:  // Handling n-type response
        return response[2] - '0';
    case 3:  // Handling nnn-type response
        return strtol (response + 2, NULL, 10);
    case 11:  // Handling long-type response
        return strtol (response + 2, NULL, 10);
    default:
        // Invalid response size
        break;
    }
    return -1;  // Invalid response size
}

#ifdef ESP32_S3
// True if an Icom IC-705 answers the CI-V ID request (0x19 0x00) on the active
// transport. The reply payload ends in the model ID 0xA4 (164) -- confirmed to
// be the model code, not an echo of the CI-V address, by probing with a
// non-default address in the reference Python implementation.
static bool probe_for_ic705 () {
    ESP_LOGD (TAG8, "Entering probe_for_ic705()");

    uint8_t cmd[]       = { 0x19, 0x00 };
    uint8_t payload[8];
    size_t  payload_len = 0;
    if (!civ::transact (kxRadio, cmd, sizeof (cmd), 0x19, 0x00, payload, sizeof (payload), payload_len, 400)) {
        ESP_LOGV (TAG8, "IC-705 probe: no CI-V response");
        return false;
    }
    if (payload_len < 1 || payload[payload_len - 1] != 0xA4) {
        ESP_LOGI (TAG8, "CI-V ID response with unexpected model id (payload %u bytes)", (unsigned)payload_len);
        return false;
    }
    return true;
}
#endif

static bool probe_for_qmx () {
    char qmx_response[64] = {0};

    ESP_LOGD (TAG8, "Entering probe_for_qmx()");
    cat_flush ();
    cat_write ("VN;", 3);

    int returned_chars = cat_read ((uint8_t *)qmx_response, sizeof (qmx_response) - 1, 250);
    if (returned_chars <= 0) {
        ESP_LOGV (TAG8, "QMX probe: no response");
        return false;
    }

    qmx_response[returned_chars] = '\0';
    ESP_LOGI (TAG8, "QMX probe response (%d chars): '%s'", returned_chars, qmx_response);
    bool is_qmx = looks_like_qmx_response (qmx_response, returned_chars);
    if (!is_qmx)
        ESP_LOGV (TAG8, "QMX probe did not match QMX response");

    return is_qmx;
}

KXRadio::KXRadio()
    : m_mutex (nullptr)
    , m_is_connected (false)
    , m_radio_type (RadioType::UNKNOWN)
    , m_driver (&g_kx_driver) {
    m_mutex = xSemaphoreCreateMutex();
    if (!m_mutex) {
        ESP_LOGE (TAG8, "Failed to create radio mutex");
        abort();
    }
}

KXRadio & KXRadio::getInstance() {
    static KXRadio instance;  // Static instance
    return instance;
}

TimedLock KXRadio::timed_lock (TickType_t timeout_ms, const char * operation) {
    return TimedLock (m_mutex, timeout_ms, operation);
}

void KXRadio::select_driver() {
    if (m_radio_type == RadioType::KH1)
        m_driver = &g_kh1_driver;
    else if (m_radio_type == RadioType::QMX)
        m_driver = &g_qmx_driver;
    else if (m_radio_type == RadioType::IC705)
        m_driver = &g_ic705_driver;
    else
        m_driver = &g_kx_driver;
}

/*
 * Functions that form our public radio API
 * These should all assert that the Radio is is_locked()
 * It's an error somewhere up in the call stack if not.
 */

/**
 * Tries to establish a UART connection with the radio at various baud rates, configures UART settings,
 * and attempts to lock in the baud rate at 38400 for subsequent communication.
 *
 * @return int Baud rate that was successfully set.
 *
 * Preconditions:
 *   The radio must be locked before calling this function. If not, an error is logged.
 */
#ifdef ESP32_S3
// Periodically shows the current VFO frequency on the onboard LCD (row 6), so CAT
// traffic can be visually confirmed as ongoing/working without WiFi or a console --
// both of which may be unavailable once USB host mode claims the chip's USB PHY.
static void radio_status_display_task (void * arg) {
    while (true) {
        vTaskDelay (pdMS_TO_TICKS (1000));

        if (!kxRadio.is_connected())
            continue;

        long hz        = 0;
        bool got_freq  = false;
        {
            TimedLock lock = kxRadio.timed_lock (RADIO_LOCK_TIMEOUT_FAST_MS, "status display FA");
            if (lock.acquired())
                got_freq = kxRadio.get_frequency (hz);
        }

        if (got_freq) {
            char line[24];
            snprintf (line, sizeof (line), "%ld.%03ld kHz", hz / 1000, hz % 1000);
            usb_host_display_set_line (6, line, DISPLAY_COLOR_WHITE);
        }
    }
}

static void start_radio_status_display_task_once () {
    static bool started = false;
    if (started)
        return;
    started = true;
    xTaskCreate (radio_status_display_task, "radio_status_disp", 3072, NULL, SC_TASK_PRIORITY_LOW, NULL);
}
#endif

int KXRadio::connect() {
    ESP_LOGV (TAG8, "trace: %s()", __func__);

    if (!is_locked())
        ESP_LOGE (TAG8, "RADIO NOT LOCKED! (coding error in caller)");

#ifdef ESP32_S3
    start_radio_status_display_task_once();

    // This dev board's wired ACC UART (UART_NUM_0, GPIO43/44) is the same peripheral and pins
    // as its onboard debug-UART bridge, and this USB-host configuration has no wired radio
    // connection anyway -- scanning it here would garble that console forever (confirmed: it
    // was masking a real panic's backtrace behind a wall of baud-mismatched noise). Unlike
    // other hardware, just keep waiting for a QMX to show up over USB; never fall back to UART.
    while (true) {
        // Re-checked on every lap (not just once at startup) so a QMX that attaches over
        // USB later -- e.g. after a runtime reset while it was already plugged in and
        // powered, which this board's USB host doesn't reliably re-enumerate -- still gets
        // picked up once it's freshly power-cycled, without requiring a full SOTAcat reboot.
        if (usb_serial_host_is_connected()) {
            g_use_usb_transport = true;
            ESP_LOGI (TAG8, "USB CDC device open; using USB transport for CAT communication");
            // Give the device a moment after DTR/RTS assertion (see usb_cdc_open_task()) to
            // actually start servicing its virtual UART before we probe it.
            vTaskDelay (pdMS_TO_TICKS (1500));
            empty_kx_input_buffer (100);

            // The enumerated VID picks the probe: Icom devices speak binary CI-V
            // and would only be confused by an ASCII "VN;", and vice versa.
            if (usb_serial_host_get_vid() == USB_VID_ICOM) {
                if (probe_for_ic705()) {
                    ESP_LOGI (TAG8, "IC-705 radio detected over USB CDC");
                    usb_host_display_set_line (5, "CAT: OK (IC705)", DISPLAY_COLOR_GREEN);
                    m_radio_type   = RadioType::IC705;
                    m_is_connected = true;
                    select_driver();
                    empty_kx_input_buffer (100);
                    return 0;  // no baud rate to report for USB
                }
                // Probably opened the radio's GPS/RS-232C port instead of CI-V;
                // rotate to the other CDC interface and re-probe next lap.
                // (Note: a powered-off IC-705 never enumerates at all, so there
                // is no auto-power-on opportunity here -- the radio must be on
                // for USB to come up the first time.)
                ESP_LOGW (TAG8, "Icom device open but no CI-V response; trying next CDC interface");
                usb_host_display_set_line (5, "CAT: no resp", DISPLAY_COLOR_RED);
                g_use_usb_transport = false;
                usb_serial_host_cycle_interface();
            }
            else if (probe_for_qmx()) {
                ESP_LOGI (TAG8, "QMX radio detected over USB CDC");
                usb_host_display_set_line (5, "CAT: OK (QMX)", DISPLAY_COLOR_GREEN);
                m_radio_type   = RadioType::QMX;
                m_is_connected = true;
                select_driver();
                empty_kx_input_buffer (100);
                return 0;  // no baud rate to report for USB
            }
            else {
                ESP_LOGW (TAG8, "USB CDC device open but didn't respond like a QMX; will keep retrying");
                usb_host_display_set_line (5, "CAT: no resp", DISPLAY_COLOR_RED);
                g_use_usb_transport = false;
            }
        }
        vTaskDelay (pdMS_TO_TICKS (500));
    }
#else
    // Try 38400 baud first (QMX standard), then fall back to other rates
    int    baud_rates[] = {38400, 9600, 19200, 4800};
    size_t num_rates    = sizeof (baud_rates) / sizeof (baud_rates[0]);

    // Install the UART driver using an event queue to handle UART events
    uart_driver_install (UART_NUM, 1024, 0, 0, NULL, 0);

    // Configure the pins for UART2 (Serial2)
    uart_config_t uart_config = {
        .baud_rate           = 38400,  // Start with 38400 for QMX
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0, // not used since flow_ctrl is disabled
        .source_clk          = UART_SCLK_APB,
        .flags               = {.allow_pd = 0, .backup_before_sleep = 0},
    };
    uart_param_config (UART_NUM, &uart_config);
    uart_set_pin (UART_NUM, UART2_TX_PIN, UART2_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (HW_TYPE == SOTAcat_HW_Type::AB6D_1) {
        // Invert UART2 TX and RX signals
        uart_set_line_inverse (UART_NUM, UART_SIGNAL_RXD_INV | UART_SIGNAL_TXD_INV);
    }

    uint8_t buffer[256];
    while (true) {
        for (size_t i = 0; i < num_rates; ++i) {
            uart_set_baudrate (UART_NUM, baud_rates[i]);
            ESP_LOGI (TAG8, "Trying baud rate: %d", baud_rates[i]);
            vTaskDelay (pdMS_TO_TICKS (250));             // Delay for stability before next try

            if (baud_rates[i] == 9600) {
                // Send I command to check for KH
                uart_flush (UART_NUM);
                uart_write_bytes (UART_NUM, ";I;", strlen (";I;"));

                int length = uart_read_bytes (UART_NUM, buffer, 256, 250 / portTICK_PERIOD_MS);
                if (length > 0) {
                    buffer[length] = '\0';  // Null terminate the string
                    ESP_LOGV (TAG8, "received %d bytes: %s", length, buffer);

                    if (strstr ((char *)buffer, "KH1;") != NULL) {
                        ESP_LOGI (TAG8, "detected KH1 radio at 9600 baud");
                        m_radio_type   = RadioType::KH1;
                        m_is_connected = true;
                        select_driver();
                        empty_kx_input_buffer (100);
                        return baud_rates[i];
                    }
                }
                else {
                    ESP_LOGI (TAG8, "no response received for baud rate %d", baud_rates[i]);
                }
            }

            ESP_LOGD (TAG8, "Calling probe_for_qmx() at baud rate %d", baud_rates[i]);
            if (probe_for_qmx()) {
                ESP_LOGI (TAG8, "QMX radio detected at baud rate %d", baud_rates[i]);
                m_radio_type   = RadioType::QMX;
                m_is_connected = true;
                select_driver();
                empty_kx_input_buffer (100);
                return baud_rates[i];
            }

            uart_flush (UART_NUM);
            uart_write_bytes (UART_NUM, ";RVR;", strlen (";RVR;"));

            int length = uart_read_bytes (UART_NUM, buffer, 256, 250 / portTICK_PERIOD_MS);
            if (length > 0) {
                buffer[length] = '\0';  // Null terminate the string
                ESP_LOGV (TAG8, "received %d bytes: %s", length, buffer);

                if (strstr ((char *)buffer, "RVR99.99;") != NULL) {
                    ESP_LOGI (TAG8, "correct baud rate found: %d", baud_rates[i]);
                    uart_write_bytes (UART_NUM, ";AI0;", strlen (";AI0;"));

                    if (baud_rates[i] != 38400) {
                        ESP_LOGI (TAG8, "forcing baud rate to 38400 for fsk use (ft8, etc.)...");
                        // Normally we would call "put_to_kx()" but the KX BRn; command does not allow a "get" response so we can't use that function here.
                        for (int j = 0; j < 2; j++) {
                            uart_write_bytes (UART_NUM, "BR3;", strlen ("BR3;"));
                            empty_kx_input_buffer (100);
                            uart_set_baudrate (UART_NUM, 38400);  // Change baud rate
                        }
                    }
                    m_is_connected = true;
                    empty_kx_input_buffer (600);
                    detect_radio_type();
                    return baud_rates[i];
                }
            }
            else
                ESP_LOGI (TAG8, "no response received for baud rate %d", baud_rates[i]);
        }
    }
#endif
}

/**
 * Clears the UART input buffer, logging the discarded data.
 *
 * @param wait_ms Milliseconds to wait while reading from the buffer.
 *
 * Preconditions:
 *   The radio must be locked before calling this function. If not, an error is logged.
 */
void KXRadio::empty_kx_input_buffer (int wait_ms) {
    ESP_LOGV (TAG8, "trace: %s()", __func__);

    if (!is_locked())
        ESP_LOGE (TAG8, "RADIO NOT LOCKED! (coding error in caller)");

    char in_buff[64];
    int  returned_chars     = cat_read ((uint8_t *)in_buff, sizeof (in_buff) - 1, wait_ms);
    in_buff[returned_chars] = '\0';
    ESP_LOGV (TAG8, "empty_kx_input_buffer() called, ate %d bytes in %d ms with chars: %s", returned_chars, wait_ms, in_buff);
}

/**
 * Sends a command to the radio and retrieves a numeric response, handling retries and timeouts.
 *
 * @param command Command to be sent.
 * @param tries Number of attempts to successfully execute the command.
 * @param num_digits Number of digits in the expected response.
 * @return long Value retrieved from the response.
 *
 * Preconditions:
 *   The radio must be locked before calling this function. If not, an error is logged.
 */
long KXRadio::get_from_kx (const char * command, int tries, int num_digits) {
    ESP_LOGV (TAG8, "trace: %s(command = '%s')", __func__, command);

    if (!is_locked())
        ESP_LOGE (TAG8, "RADIO NOT LOCKED! (coding error in caller)");

    char command_buff[8] = {0};
    char response[16]    = {0};

    int command_size = strlen (command);
    if ((command_size != 2 && command_size != 3) || num_digits < 1 || num_digits > 11) {
        ESP_LOGE (TAG8, "invalid command '%s' and expected digits of %d", command, num_digits);
        return '\0';
    }

    int wait_time = KX_TIMEOUT_MS_SHORT_COMMANDS;

    const char * long_command_prefixes = "AP FA FR FT MD PC";
    if (command != NULL && strstr (long_command_prefixes, command) != NULL)
        wait_time = KX_TIMEOUT_MS_LONG_COMMANDS;

    snprintf (command_buff, sizeof (command_buff), "%s;", command);
    int response_size = num_digits + command_size + 1;
    if (!uart_get_command (command_buff, response, response_size, tries, wait_time))
        return -1;  // Error was already logged

    long result = parse_response (response, num_digits);
    ESP_LOGD (TAG8, "kx command '%s' returns %ld", command, result);
    return result;
}

/**
 * Sends a command to set a value on the radio, verifies the set operation, and retries if necessary.
 *
 * @param command Command to send.
 * @param num_digits Expected number of digits in the command.
 * @param value Value to be set by the command.
 * @param tries Number of attempts to successfully execute the command.  If non-positive, send once without verification.
 * @return bool True if successful, false otherwise.
 *
 * Preconditions:
 *   The radio must be locked before calling this function. If not, an error is logged.
 */
bool KXRadio::put_to_kx (const char * command, int num_digits, long value, int tries) {
    ESP_LOGV (TAG8, "put_to_kx('%s') attempting value %ld", command, value);

    if (!is_locked())
        ESP_LOGE (TAG8, "RADIO NOT LOCKED! (coding error in caller)");

    if (strlen (command) != 2 || value < 0) {
        ESP_LOGE (TAG8, "invalid command '%s' or value %ld", command, value);
        return false;
    }

    char request[16];
    switch (num_digits) {
    case 1:  // Handling n-type request
        if (value > 9) {
            ESP_LOGE (TAG8, "invalid value %u for command '%s'", (unsigned int)value, command);
            return false;
        }
        snprintf (request, sizeof (request), "%s%u;", command, (unsigned int)value);
        break;
    case 3:  // Handling nnn-type request
        if (value > 999) {
            ESP_LOGE (TAG8, "invalid value %u for command '%s'", (unsigned int)value, command);
            return false;
        }
        snprintf (request, sizeof (request), "%s%03u;", command, (unsigned int)value);
        break;
    case 11:  // Handling long-type request
        snprintf (request, sizeof (request), "%s%011ld;", command, value);
        break;
    default:
        ESP_LOGE (TAG8, "invalid num_digits and command '%s' with value %ld", command, value);
        return false;
    }

    long adjusted_value = value;
    if (num_digits == 11) {
        // Some radios report 10 Hz resolution; accept both exact and rounded readback.
        adjusted_value = ((long)(value / 10)) * 10;
    }

    if (tries <= 0) {
        // simply write the command to the radio
        ESP_LOGI (TAG8, "CAT TX (no verify): '%s'", request);
        cat_flush ();
        cat_write (request, num_digits + 3);
        return true;
    }

    // validate the write was successful
    for (int attempt = 0; attempt < tries; attempt++) {
        ESP_LOGI (TAG8, "CAT TX: '%s'", request);
        cat_flush ();
        cat_write (request, num_digits + 3);

        // Now read-back the value to verify it was set correctly
        long out_value = get_from_kx (command, 2, num_digits);

        if (out_value == value || (num_digits == 11 && out_value == adjusted_value)) {
            ESP_LOGI (TAG8, "command '%s' successful; value = %ld", command, out_value);
            return true;
        }

        ESP_LOGE (TAG8, "failed to set '%s' to %ld on %d tries", command, value, attempt + 1);
    }

    return false;
}

/**
 * Retrieves a specific menu item's value from the radio. It involves switching to the
 * menu mode, retrieving the value, and then exiting the menu mode.
 *
 * @param menu_item The menu item number to query.
 * @param tries The number of attempts to execute the command and retrieve the value.
 * @return long The retrieved value of the menu item.
 *
 * Preconditions:
 *   The radio must be locked before calling this function. If not, an error is logged.
 */
long KXRadio::get_from_kx_menu_item (uint8_t menu_item, int tries) {
    ESP_LOGV (TAG8, "trace: %s()", __func__);

    if (!is_locked())
        ESP_LOGE (TAG8, "RADIO NOT LOCKED! (coding error in caller)");

    put_to_kx ("MN", 3, menu_item, SC_KX_COMMUNICATION_RETRIES);  // Ex. MN058;  - Switch into menu mode and select the TUN PWR menu item

    long value = get_from_kx ("MP", tries, 3);  // Get the menu item value

    put_to_kx ("MN", 3, 255, SC_KX_COMMUNICATION_RETRIES);  // Switch out of Menu mode

    return value;
}

/**
 * Sets a specific menu item's value on the radio. This function includes steps to switch
 * into menu mode, set the menu item value, and then exit menu mode.
 *
 * @param menu_item The menu item number to be set.
 * @param value The value to set for the menu item.
 * @param tries The number of attempts to successfully execute the command.
 * @return bool True if the menu item value is successfully set, false otherwise.
 *
 * Preconditions:
 *   The radio must be locked before calling this function. If not, an error is logged.
 */
bool KXRadio::put_to_kx_menu_item (uint8_t menu_item, long value, int tries) {
    ESP_LOGV (TAG8, "trace: %s()", __func__);

    if (!is_locked())
        ESP_LOGE (TAG8, "RADIO NOT LOCKED! (coding error in caller)");

    put_to_kx ("MN", 3, menu_item, SC_KX_COMMUNICATION_RETRIES);  // Ex. MN058;  - Switch into menu mode and select the TUN PWR menu item

    // Set the menu item value
    bool success = put_to_kx ("MP", 3, value, tries);  // Ex. MP010; - Set the TUN PWR to 1.0 watts

    // Switch out of Menu mode
    put_to_kx ("MN", 3, 255, SC_KX_COMMUNICATION_RETRIES);  // Switch out of Menu mode

    return success;
}

/**
 * Sends a string command to the radio and retrieves a string response. It handles retries
 * and uses specific timeout settings for communication.
 *
 * @param command The command string to be sent to the radio.
 * @param tries The number of attempts to execute the command successfully.
 * @param response Buffer to store the received response.
 * @param response_size The size of the response buffer.
 * @return bool True if the command was executed and a response was received successfully, false otherwise.
 *
 * Preconditions:
 *   The radio must be locked before calling this function. If not, an error is logged.
 */
bool KXRadio::get_from_kx_string (const char * command, int tries, char * response, int response_size) {
    ESP_LOGV (TAG8, "trace: %s(command = '%s')", __func__, command);

    if (!is_locked())
        ESP_LOGE (TAG8, "RADIO NOT LOCKED! (coding error in caller)");

    // add trailing semi-colon
    char command_buff[8] = {0};
    snprintf (command_buff, sizeof (command_buff), "%s;", command);

    return uart_get_command (command_buff, response, response_size, tries, KX_TIMEOUT_MS_SHORT_COMMANDS);
}

/**
 * Sends a custom command string to the radio via UART. This function is typically used for
 * commands that do not require a response to be checked.
 *
 * @param command The command string to be sent to the radio.
 * @param tries The number of attempts to send the command.
 * @return bool Always returns true, indicating the command was sent.
 *
 * Preconditions:
 *   The radio must be locked before calling this function. If not, an error is logged.
 */
bool KXRadio::put_to_kx_command_string (const char * command, int tries) {
    ESP_LOGV (TAG8, "trace: %s(command = '%s')", __func__, command);

    if (!is_locked())
        ESP_LOGE (TAG8, "RADIO NOT LOCKED! (coding error in caller)");

    ESP_LOGI (TAG8, "CAT TX (direct): '%s'", command);
    cat_flush ();
    cat_write (command, strlen (command));

    return true;
}

/**
 * Driver-delegation macros.  Each KXRadio public method below is a thin wrapper
 * that forwards to the corresponding method on the currently-selected driver
 * (m_driver), which is either KXRadioDriver or KH1RadioDriver.
 *
 * Three variants are defined:
 *
 *   DELEGATE_BOOL(name, PARAMS, ...)
 *     Generates: bool KXRadio::name PARAMS
 *     Preconditions: radio must be locked (logged error if not), m_driver must be set.
 *     Returns false if m_driver is null; otherwise returns the driver result.
 *
 *   DELEGATE_BOOL_CONST(name)
 *     Generates: bool KXRadio::name() const
 *     Preconditions: m_driver must be set (no lock required).
 *     Returns false if m_driver is null; otherwise returns the driver result.
 *
 *   DELEGATE_VOID(name, PARAMS, ...)
 *     Generates: void KXRadio::name PARAMS
 *     Preconditions: radio must be locked (logged error if not), m_driver must be set.
 *     Does nothing if m_driver is null.
 *
 * All three variants add LOGV tracing; the non-const variants also check is_locked().
 */
#define DELEGATE_BOOL(name, PARAMS, ...)                                   \
    bool KXRadio::name PARAMS {                                            \
        ESP_LOGV (TAG8, "trace: %s()", __func__);                          \
        if (!is_locked())                                                  \
            ESP_LOGE (TAG8, "RADIO NOT LOCKED! (coding error in caller)"); \
        return m_driver && m_driver->name (*this, ##__VA_ARGS__);          \
    }
#define DELEGATE_BOOL_CONST(name)                 \
    bool KXRadio::name() const {                  \
        ESP_LOGV (TAG8, "trace: %s()", __func__); \
        return m_driver && m_driver->name();      \
    }
#define DELEGATE_VOID(name, PARAMS, ...)                                   \
    void KXRadio::name PARAMS {                                            \
        ESP_LOGV (TAG8, "trace: %s()", __func__);                          \
        if (!is_locked())                                                  \
            ESP_LOGE (TAG8, "RADIO NOT LOCKED! (coding error in caller)"); \
        if (m_driver)                                                      \
            m_driver->name (*this, ##__VA_ARGS__);                         \
    }
// clang-format off
DELEGATE_BOOL (ft8_prepare,         (long rfFreq, int audioFreq),                         rfFreq, audioFreq)
DELEGATE_BOOL (get_frequency,       (long & out_hz),                          out_hz)
DELEGATE_BOOL (get_mode,            (radio_mode_t & out_mode),                out_mode)
DELEGATE_BOOL (get_power,           (long & out_power),                       out_power)
DELEGATE_BOOL (get_radio_state,     (kx_state_t * in_state),                  in_state)
DELEGATE_BOOL (get_volume,          (long & out_volume),                      out_volume)
DELEGATE_BOOL (get_xmit_state,      (long & out_state),                       out_state)
DELEGATE_BOOL (play_message_bank,   (int bank),                               bank)
DELEGATE_BOOL (restore_radio_state, (const kx_state_t * in_state, int tries), in_state, tries)
DELEGATE_BOOL (send_keyer_message,  (const char * message),                   message)
DELEGATE_BOOL (set_frequency,       (long hz, int tries),                     hz, tries)
DELEGATE_BOOL (set_mode,            (radio_mode_t mode, int tries),           mode, tries)
DELEGATE_BOOL (set_power,           (long power),                             power)
DELEGATE_BOOL (set_radio_power,     (bool on),                                on)
DELEGATE_BOOL (set_volume,          (long volume),                            volume)
DELEGATE_BOOL (set_xmit_state,      (bool on),                                on)
DELEGATE_BOOL (sync_time,           (const RadioTimeHms & client_time),       client_time)
DELEGATE_BOOL (tune_atu,            ())

DELEGATE_BOOL_CONST (supports_keyer)
DELEGATE_BOOL_CONST (supports_volume)
DELEGATE_BOOL_CONST (supports_power_toggle)

DELEGATE_VOID (ft8_set_tone, (long rfFreq, int audioFreq, long frequency), rfFreq, audioFreq, frequency)
DELEGATE_VOID (ft8_tone_off, ())
DELEGATE_VOID (ft8_tone_on,  ())
// clang-format on

#undef DELEGATE_BOOL
#undef DELEGATE_BOOL_CONST
#undef DELEGATE_VOID

/**
 * Detects the type of radio (KX2 or KX3) by using the OM command.
 * According to the programmer's reference, the OM response format differs:
 * - KX3: "OM APF---TBXI0n;" where n=2 for KX3
 * - KX2: "OM APF---TBXI0n;" where n=1 for KX2
 * Stores detected type in m_radio_type.
 *
 * Preconditions:
 *   The radio must be locked before calling this function. If not, an error is logged.
 */
void KXRadio::detect_radio_type() {
    ESP_LOGV (TAG8, "trace: %s()", __func__);

    if (!is_locked())
        ESP_LOGE (TAG8, "RADIO NOT LOCKED! (coding error in caller)");

    if (m_radio_type == RadioType::KH1 || m_radio_type == RadioType::QMX || m_radio_type == RadioType::IC705)
        return;

    char response[17] = {0};

    // Send OM command to get option module information
    if (get_from_kx_string ("OM", SC_KX_COMMUNICATION_RETRIES, response, sizeof (response) - 1)) {
        // Check the product identifier in the response
        // Format: "OM APF---TBXI0n;" where n is the product ID
        int len = strlen (response);
        if (len == 16 && response[len - 3] == '0') {
            char product_id = response[len - 2];
            if (product_id == '1') {
                m_radio_type = RadioType::KX2;
                ESP_LOGI (TAG8, "detected KX2 radio");
            }
            else if (product_id == '2') {
                m_radio_type = RadioType::KX3;
                ESP_LOGI (TAG8, "detected KX3 radio");
            }
            else {
                m_radio_type = RadioType::UNKNOWN;
                ESP_LOGW (TAG8, "unknown radio product id: %c", product_id);
            }
            select_driver();
        }
        else {
            m_radio_type = RadioType::UNKNOWN;
            ESP_LOGW (TAG8, "unexpected OM response format: '%s'", response);
            select_driver();
        }
    }
    else {
        ESP_LOGW (TAG8, "OM response failed; attempting QMX probe fallback");
        if (probe_for_qmx()) {
            m_radio_type = RadioType::QMX;
            ESP_LOGI (TAG8, "detected QMX radio in detect_radio_type fallback");
        }
        else {
            m_radio_type = RadioType::UNKNOWN;
            ESP_LOGE (TAG8, "failed to get OM response for radio type detection");
        }
        select_driver();
    }
}
