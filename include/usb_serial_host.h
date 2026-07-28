#pragma once

#include <stdint.h>
#include <stddef.h>
#include <esp_err.h>

/**
 * USB Serial Host Driver for ESP32-S3
 * 
 * Provides CDC (Communications Device Class) support for communicating with 
 * serial devices like the QMX radio over USB on ESP32-S3 boards.
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
