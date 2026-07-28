#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <esp_err.h>

/**
 * USB Serial Host Driver for ESP32-S3
 *
 * Provides CDC-ACM (Communications Device Class) support for communicating with
 * serial devices like the QMX radio over USB on ESP32-S3 boards, built on top of
 * the ESP-IDF `usb_host` library and the `usb_host_cdc_acm` component.
 *
 * QMX's exact USB VID/PID is not known in advance, so any CDC-ACM-compliant device
 * connected to the host port is accepted (see CDC_HOST_ANY_VID/CDC_HOST_ANY_PID usage
 * in usb_serial_host.cpp). If QMX turns out to enumerate via a USB-UART bridge chip
 * (FTDI/CP210x/CH34x) rather than native CDC-ACM, this driver will need one of the
 * matching espressif/usb_host_*_vcp components added instead of/alongside cdc_acm.
 *
 * NOTE: the ESP32-S3 has a single physical USB PHY, shared between the native
 * "USB Serial/JTAG" console peripheral and the "USB-OTG" host peripheral used here.
 * Once usb_serial_host_init() successfully installs the USB Host library, the console
 * (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) will likely stop producing output, because the
 * two peripherals cannot own the PHY at the same time.
 */

/**
 * Initialize USB host and start scanning for connected devices
 * Must be called before any other USB operations
 * 
 * @return ESP_OK if successful, ESP_ERR_* otherwise
 */
esp_err_t usb_serial_host_init(void);

/**
 * Check if a USB CDC device is currently connected
 * 
 * @return true if device connected and ready, false otherwise
 */
bool usb_serial_host_is_connected(void);

/**
 * Send data to the connected USB device
 * 
 * @param data Pointer to data buffer
 * @param len Number of bytes to send
 * @return Number of bytes actually sent, -1 on error
 */
int usb_serial_host_write(const uint8_t *data, size_t len);

/**
 * Receive data from the connected USB device (non-blocking)
 *
 * @param data Pointer to buffer for received data
 * @param len Maximum number of bytes to read
 * @return Number of bytes read, 0 if no data available, -1 on error
 */
int usb_serial_host_read(uint8_t *data, size_t len);

/**
 * Receive data from the connected USB device, blocking until either `len` bytes
 * have been collected or `wait_ms` has elapsed (mirrors ESP-IDF's uart_read_bytes()
 * semantics, so it's a drop-in replacement for CAT command/response transport).
 *
 * @param data Pointer to buffer for received data
 * @param len Maximum number of bytes to read
 * @param wait_ms Total time budget to wait for data, in milliseconds
 * @return Number of bytes actually read (may be less than len), -1 on error
 */
int usb_serial_host_read_blocking(uint8_t *data, size_t len, int wait_ms);

/**
 * Flush any pending data to be sent
 * 
 * @return ESP_OK if successful
 */
esp_err_t usb_serial_host_flush(void);

/**
 * Deinitialize USB host and clean up resources
 * 
 * @return ESP_OK if successful
 */
esp_err_t usb_serial_host_deinit(void);

/**
 * Get connection status string for logging
 * 
 * @return Status string describing current USB state
 */
const char *usb_serial_host_get_status(void);
