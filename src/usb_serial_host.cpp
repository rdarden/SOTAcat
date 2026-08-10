#include "usb_serial_host.h"
#include "hardware_specific.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <string.h>

#ifdef ESP32_S3
    #include <driver/gpio.h>
    #include <esp_intr_alloc.h>
    #include "usb/usb_host.h"
    #include "usb/cdc_acm_host.h"
    #include "usb_host_display.h"
#endif

static const char *TAG8 = "sc:usb_host";

// RX buffer for incoming data
#define USB_RX_BUFFER_SIZE 1024
#define USB_RX_QUEUE_SIZE 16

typedef struct {
    uint8_t data[USB_RX_BUFFER_SIZE];
    size_t len;
} usb_rx_packet_t;

// Global USB serial device state
static volatile bool g_usb_connected  = false;
static volatile bool g_usb_initialized = false;
static QueueHandle_t g_usb_rx_queue = NULL;

#ifdef ESP32_S3

#define USB_HOST_TASK_PRIORITY     10
#define USB_HOST_TASK_STACK_SIZE   4096
#define USB_OPEN_TASK_STACK_SIZE   4096
#define USB_DEVICE_OPEN_TIMEOUT_MS 1000
#define USB_DEVICE_POLL_DELAY_MS   500

// Onboard status LEDs, dedicated to USB host status so they can be read without
// WiFi or a console connection (both of which may be unavailable once USB host
// mode is active). Yellow = host mode active; Green = a CDC device is open.
#define USB_HOST_ACTIVE_LED_GPIO ((gpio_num_t)16)  // Yellow
#define USB_DEVICE_OPEN_LED_GPIO ((gpio_num_t)15)  // Green

static void usb_status_led_init (void) {
    gpio_set_direction (USB_HOST_ACTIVE_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction (USB_DEVICE_OPEN_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level (USB_HOST_ACTIVE_LED_GPIO, 0);
    gpio_set_level (USB_DEVICE_OPEN_LED_GPIO, 0);
}

#define USB_OVER_CURRENT_GPIO ((gpio_num_t)21)

static cdc_acm_dev_hdl_t g_cdc_dev              = NULL;
static uint16_t          g_cdc_vid              = 0;
static uint16_t          g_cdc_pid              = 0;
static bool              g_device_ever_seen     = false;  // any new_dev_cb, CDC-ACM or not
static esp_err_t         g_last_open_err        = ESP_OK; // last non-OK cdc_acm_host_open() result
static TaskHandle_t      g_usb_host_task_handle = NULL;
static TaskHandle_t      g_usb_open_task_handle = NULL;

// Which CDC interface of a multi-port device to open. The IC-705 exposes two
// ACM ports (control interfaces 0 and 2); interface 0 is the CI-V/CAT port on
// the unit examined, but firmware revisions could differ, so a failed CAT
// probe can rotate to the other port via usb_serial_host_cycle_interface().
// Single-port devices (QMX) always use interface 0.
static const uint8_t s_icom_iface_candidates[] = {0, 2};
static size_t        s_iface_rotor             = 0;

static uint8_t current_cdc_interface (void) {
    if (g_cdc_vid == USB_VID_ICOM)
        return s_icom_iface_candidates[s_iface_rotor % (sizeof (s_icom_iface_candidates))];
    return 0;
}

/**
 * Called by the CDC-ACM driver whenever data arrives from the device.
 * Runs in the driver's own task context, so we just copy into our RX queue
 * for usb_serial_host_read() to drain.
 */
static bool usb_cdc_rx_callback (const uint8_t * data, size_t data_len, void * user_arg) {
    if (!g_usb_rx_queue || data_len == 0)
        return true;

    usb_rx_packet_t packet;
    packet.len = (data_len < USB_RX_BUFFER_SIZE) ? data_len : USB_RX_BUFFER_SIZE;
    memcpy (packet.data, data, packet.len);

    if (xQueueSend (g_usb_rx_queue, &packet, 0) != pdTRUE)
        ESP_LOGW (TAG8, "USB RX queue full, dropping %d bytes", (int)packet.len);

    return true;
}

static void usb_cdc_event_callback (const cdc_acm_host_dev_event_data_t * event, void * user_ctx) {
    switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW (TAG8, "USB CDC device disconnected (VID=0x%04X PID=0x%04X)", g_cdc_vid, g_cdc_pid);
        g_usb_connected = false;
        s_iface_rotor   = 0;  // next device starts back at the first candidate interface
        gpio_set_level (USB_DEVICE_OPEN_LED_GPIO, 0);
        usb_host_display_set_line (2, "NO DEVICE", DISPLAY_COLOR_GRAY);
        usb_host_display_set_line (3, "", DISPLAY_COLOR_WHITE);
        cdc_acm_host_close (g_cdc_dev);
        g_cdc_dev = NULL;
        break;
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE (TAG8, "USB CDC error: %d", event->data.error);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGD (TAG8, "USB CDC serial state changed: 0x%04X", event->data.serial_state.val);
        break;
    default:
        break;
    }
}

/**
 * Called for every USB device that connects to the host port, CDC-ACM or not.
 * Just used here to log the VID/PID; cdc_acm_host_open() (in usb_cdc_open_task)
 * is what actually claims a matching device.
 */
static void usb_new_device_callback (usb_device_handle_t usb_dev) {
    const usb_device_desc_t * device_desc;
    if (usb_host_get_device_descriptor (usb_dev, &device_desc) == ESP_OK) {
        ESP_LOGI (TAG8, "USB device connected: VID=0x%04X PID=0x%04X", device_desc->idVendor, device_desc->idProduct);
        // Recorded here (rather than after cdc_acm_host_open()) because the open call doesn't
        // hand back descriptor info; this callback fires for the same device just beforehand.
        g_cdc_vid          = device_desc->idVendor;
        g_cdc_pid          = device_desc->idProduct;
        g_device_ever_seen = true;

        char line[16];
        snprintf (line, sizeof (line), "%04X:%04X", device_desc->idVendor, device_desc->idProduct);
        usb_host_display_set_line (2, line, DISPLAY_COLOR_YELLOW);
    }
}

/**
 * Owns the USB Host library event loop for the lifetime of USB host usage.
 * Must keep running (usb_host_lib_handle_events) or device enumeration stalls.
 */
static void usb_host_lib_task (void * arg) {
    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.intr_flags     = ESP_INTR_FLAG_LEVEL1;

    ESP_LOGW (TAG8, "Installing USB Host library; native USB console output may stop from this point");
    esp_err_t ret = usb_host_install (&host_config);
    if (ret != ESP_OK) {
        ESP_LOGE (TAG8, "usb_host_install() failed: %s", esp_err_to_name (ret));
        usb_host_display_set_line (1, "HOST FAIL", DISPLAY_COLOR_RED);
        xTaskNotifyGive ((TaskHandle_t)arg);
        vTaskDelete (NULL);
        return;
    }

    cdc_acm_host_driver_config_t driver_config = {};
    driver_config.driver_task_stack_size = USB_HOST_TASK_STACK_SIZE;
    driver_config.driver_task_priority   = USB_HOST_TASK_PRIORITY + 1;
    driver_config.xCoreID                = 0;
    driver_config.new_dev_cb             = usb_new_device_callback;
    ret = cdc_acm_host_install (&driver_config);
    if (ret != ESP_OK) {
        ESP_LOGE (TAG8, "cdc_acm_host_install() failed: %s", esp_err_to_name (ret));
        usb_host_display_set_line (1, "CDC FAIL", DISPLAY_COLOR_RED);
        usb_host_uninstall();
        xTaskNotifyGive ((TaskHandle_t)arg);
        vTaskDelete (NULL);
        return;
    }

    gpio_set_level (USB_HOST_ACTIVE_LED_GPIO, 1);  // Yellow: host mode is up
    usb_host_display_set_line (1, "HOST: UP", DISPLAY_COLOR_GREEN);
    xTaskNotifyGive ((TaskHandle_t)arg);

    while (g_usb_initialized) {
        uint32_t event_flags;
        usb_host_lib_handle_events (pdMS_TO_TICKS (1000), &event_flags);
    }

    gpio_set_level (USB_HOST_ACTIVE_LED_GPIO, 0);
    usb_host_display_set_line (1, "HOST: DOWN", DISPLAY_COLOR_GRAY);
    cdc_acm_host_uninstall();
    usb_host_uninstall();
    ESP_LOGI (TAG8, "USB host library task stopped");
    vTaskDelete (NULL);
}

/**
 * Repeatedly (re)opens a CDC-ACM device on the host port whenever one isn't
 * already connected. QMX's VID/PID isn't known in advance, so we accept any
 * CDC-ACM-compliant device (CDC_HOST_ANY_VID/PID).
 */
static void usb_cdc_open_task (void * arg) {
    cdc_acm_host_device_config_t dev_config = {};
    dev_config.connection_timeout_ms = USB_DEVICE_OPEN_TIMEOUT_MS;
    dev_config.out_buffer_size       = 256;
    dev_config.in_buffer_size        = 256;
    dev_config.event_cb              = usb_cdc_event_callback;
    dev_config.data_cb               = usb_cdc_rx_callback;
    dev_config.user_arg              = NULL;

    while (g_usb_initialized) {
        if (g_cdc_dev == NULL) {
            uint8_t   iface = current_cdc_interface();
            esp_err_t ret   = cdc_acm_host_open (CDC_HOST_ANY_VID, CDC_HOST_ANY_PID, iface, &dev_config, &g_cdc_dev);
            if (ret == ESP_OK) {
                // Many CDC-ACM device firmwares (STM32's USB VCP stack very much included)
                // gate actual UART activity on DTR being asserted, mirroring real RS-232
                // "DTR = a terminal is connected" semantics -- real terminal programs always
                // set this on open, but cdc_acm_host_open() itself does not. Without it, the
                // device can enumerate and open cleanly yet never respond to anything sent.
                esp_err_t line_ret = cdc_acm_host_set_control_line_state (g_cdc_dev, true, true);
                if (line_ret != ESP_OK)
                    ESP_LOGW (TAG8, "set_control_line_state() failed: %s (device may not respond)", esp_err_to_name (line_ret));

                // The IC-705's CI-V-over-USB port is a virtual UART, but set a sane
                // line coding anyway (matches its nominal CI-V rate). Left alone for
                // other vendors: the QMX/STM32 VCP path works without it today.
                if (g_cdc_vid == USB_VID_ICOM) {
                    cdc_acm_line_coding_t coding = {};
                    coding.dwDTERate   = 19200;
                    coding.bCharFormat = 0;  // 1 stop bit
                    coding.bParityType = 0;  // no parity
                    coding.bDataBits   = 8;
                    esp_err_t coding_ret = cdc_acm_host_line_coding_set (g_cdc_dev, &coding);
                    if (coding_ret != ESP_OK)
                        ESP_LOGW (TAG8, "line_coding_set() failed: %s", esp_err_to_name (coding_ret));
                }

                // g_cdc_vid/g_cdc_pid were captured by usb_new_device_callback() for this device.
                ESP_LOGI (TAG8, "USB CDC-ACM device opened (VID=0x%04X PID=0x%04X interface %u); ready for CAT commands", g_cdc_vid, g_cdc_pid, iface);
                g_usb_connected = true;
                gpio_set_level (USB_DEVICE_OPEN_LED_GPIO, 1);  // Green: device open

                char line[16];
                snprintf (line, sizeof (line), "%04X:%04X OK", g_cdc_vid, g_cdc_pid);
                usb_host_display_set_line (2, line, DISPLAY_COLOR_GREEN);
                usb_host_display_set_line (3, "", DISPLAY_COLOR_WHITE);
            }
            else {
                // Either nothing connected within the timeout, or a device connected (see
                // usb_new_device_callback's log/g_device_ever_seen) but didn't match the
                // CDC-ACM or CDC-like-vendor-specific descriptor shape this driver looks for.
                g_last_open_err = ret;
                if (g_device_ever_seen) {
                    // Strip the common "ESP_ERR_"/"ESP_" prefix so more of the
                    // meaningful part of the name survives the display's truncation.
                    const char * name = esp_err_to_name (ret);
                    if (strncmp (name, "ESP_ERR_", 8) == 0)
                        name += 8;
                    else if (strncmp (name, "ESP_", 4) == 0)
                        name += 4;
                    usb_host_display_set_line (3, name, DISPLAY_COLOR_RED);
                }
            }
        }
        else {
            vTaskDelay (pdMS_TO_TICKS (USB_DEVICE_POLL_DELAY_MS));
        }

        bool over_current = gpio_get_level (USB_OVER_CURRENT_GPIO) != 0;
        usb_host_display_set_line (4, over_current ? "OVERCUR: YES" : "OVERCUR: no",
            over_current ? DISPLAY_COLOR_RED : DISPLAY_COLOR_GRAY);
    }

    vTaskDelete (NULL);
}

#endif  // ESP32_S3

esp_err_t usb_serial_host_init (void) {
    if (g_usb_initialized) {
        ESP_LOGW (TAG8, "USB serial host already initialized");
        return ESP_OK;
    }

    #ifndef ESP32_S3
        ESP_LOGE (TAG8, "USB serial host only supported on ESP32-S3");
        return ESP_ERR_NOT_SUPPORTED;
    #else
        usb_status_led_init();
        usb_host_display_init();

        g_usb_rx_queue = xQueueCreate (USB_RX_QUEUE_SIZE, sizeof (usb_rx_packet_t));
        if (!g_usb_rx_queue) {
            ESP_LOGE (TAG8, "FAILED: Could not create USB RX queue");
            return ESP_ERR_NO_MEM;
        }

        g_usb_initialized = true;

        if (xTaskCreate (usb_host_lib_task, "usb_host_lib", USB_HOST_TASK_STACK_SIZE, xTaskGetCurrentTaskHandle(),
                          USB_HOST_TASK_PRIORITY, &g_usb_host_task_handle) != pdPASS) {
            ESP_LOGE (TAG8, "Failed to create USB host library task");
            g_usb_initialized = false;
            vQueueDelete (g_usb_rx_queue);
            g_usb_rx_queue = NULL;
            return ESP_ERR_NO_MEM;
        }

        // Wait for usb_host_install()/cdc_acm_host_install() to finish (or fail) before returning.
        ulTaskNotifyTake (pdTRUE, pdMS_TO_TICKS (2000));

        if (xTaskCreate (usb_cdc_open_task, "usb_cdc_open", USB_OPEN_TASK_STACK_SIZE, NULL,
                          USB_HOST_TASK_PRIORITY, &g_usb_open_task_handle) != pdPASS) {
            ESP_LOGE (TAG8, "Failed to create USB CDC open task");
            return ESP_ERR_NO_MEM;
        }

        ESP_LOGI (TAG8, "USB host initialized; watching host port for a CDC-ACM radio (QMX, IC-705)");
        return ESP_OK;
    #endif
}

bool usb_serial_host_is_connected (void) {
    return g_usb_connected;
}

uint16_t usb_serial_host_get_vid (void) {
    #ifdef ESP32_S3
        return g_cdc_vid;
    #else
        return 0;
    #endif
}

uint16_t usb_serial_host_get_pid (void) {
    #ifdef ESP32_S3
        return g_cdc_pid;
    #else
        return 0;
    #endif
}

void usb_serial_host_cycle_interface (void) {
    #ifdef ESP32_S3
        ESP_LOGI (TAG8, "Cycling to next CDC interface candidate");
        g_usb_connected = false;
        if (g_cdc_dev != NULL) {
            cdc_acm_host_close (g_cdc_dev);
            g_cdc_dev = NULL;
        }
        s_iface_rotor++;  // usb_cdc_open_task will re-open on the new candidate
    #endif
}

int usb_serial_host_write (const uint8_t * data, size_t len) {
    if (!g_usb_initialized) {
        ESP_LOGW (TAG8, "USB write attempted before initialization");
        return -1;
    }

    if (!data || len == 0)
        return 0;

    #ifdef ESP32_S3
        if (!usb_serial_host_is_connected() || g_cdc_dev == NULL) {
            ESP_LOGW (TAG8, "USB write: device not connected");
            return -1;
        }

        esp_err_t ret = cdc_acm_host_data_tx_blocking (g_cdc_dev, data, len, 1000);
        if (ret != ESP_OK) {
            ESP_LOGW (TAG8, "USB write failed: %s", esp_err_to_name (ret));
            return -1;
        }
        return (int)len;
    #else
        return -1;
    #endif
}

int usb_serial_host_read (uint8_t * data, size_t len) {
    if (!g_usb_initialized) {
        ESP_LOGW (TAG8, "USB read attempted before initialization");
        return -1;
    }

    if (!data || len == 0)
        return 0;

    // Try to dequeue a packet without blocking
    usb_rx_packet_t packet;
    if (xQueueReceive (g_usb_rx_queue, &packet, 0) == pdTRUE) {
        size_t copy_len = (packet.len < len) ? packet.len : len;
        memcpy (data, packet.data, copy_len);
        ESP_LOGD (TAG8, "USB read: %d bytes from queue", copy_len);
        return copy_len;
    }

    return 0;  // No data available
}

int usb_serial_host_read_blocking (uint8_t * data, size_t len, int wait_ms) {
    if (!g_usb_initialized) {
        ESP_LOGW (TAG8, "USB read attempted before initialization");
        return -1;
    }

    if (!data || len == 0)
        return 0;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS (wait_ms);
    size_t     total    = 0;

    while (total < len) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline)
            break;

        usb_rx_packet_t packet;
        if (xQueueReceive (g_usb_rx_queue, &packet, deadline - now) != pdTRUE)
            break;

        // Each queued packet is one CDC-ACM RX callback's worth of data; CAT command/response
        // sizes here are small (well under USB_RX_BUFFER_SIZE), so a packet larger than the
        // remaining space would be unexpected and is simply capped rather than carried over.
        size_t copy_len = packet.len;
        if (copy_len > len - total)
            copy_len = len - total;
        memcpy (data + total, packet.data, copy_len);
        total += copy_len;
    }

    return (int)total;
}

esp_err_t usb_serial_host_flush (void) {
    if (!usb_serial_host_is_connected())
        return ESP_ERR_INVALID_STATE;

    // USB bulk transfers are submitted synchronously by cdc_acm_host_data_tx_blocking();
    // there is no separate flush step.
    return ESP_OK;
}

esp_err_t usb_serial_host_deinit (void) {
    if (!g_usb_initialized)
        return ESP_OK;

    ESP_LOGI (TAG8, "Deinitializing USB serial host");

    g_usb_initialized = false;  // Signals usb_host_lib_task and usb_cdc_open_task to exit.

    #ifdef ESP32_S3
        if (g_cdc_dev != NULL) {
            cdc_acm_host_close (g_cdc_dev);
            g_cdc_dev = NULL;
        }
        gpio_set_level (USB_HOST_ACTIVE_LED_GPIO, 0);
        gpio_set_level (USB_DEVICE_OPEN_LED_GPIO, 0);
    #endif

    // Give the background tasks time to notice and exit their loops.
    vTaskDelay (pdMS_TO_TICKS (1200));

    if (g_usb_rx_queue) {
        vQueueDelete (g_usb_rx_queue);
        g_usb_rx_queue = NULL;
    }

    g_usb_connected = false;

    ESP_LOGI (TAG8, "USB serial host deinitialized");
    return ESP_OK;
}

const char * usb_serial_host_get_status (void) {
    static char status[256];

    if (!g_usb_initialized) {
        snprintf (status, sizeof (status), "USB: Not initialized | Waiting for init() call");
    }
    else if (g_usb_connected) {
        #ifdef ESP32_S3
            snprintf (status, sizeof (status),
                "USB: \xE2\x9C\x93 CONNECTED | VID=0x%04X PID=0x%04X | ready for CAT commands", g_cdc_vid, g_cdc_pid);
        #else
            snprintf (status, sizeof (status), "USB: \xE2\x9C\x93 CONNECTED | ready for CAT commands");
        #endif
    }
    else {
        #ifdef ESP32_S3
            bool over_current = gpio_get_level (USB_OVER_CURRENT_GPIO) != 0;
            if (g_device_ever_seen) {
                snprintf (status, sizeof (status),
                    "USB: \xE2\x9A\xA0 Device seen (VID=0x%04X PID=0x%04X) but not opened as CDC-ACM "
                    "| last error: %s | over_current=%s",
                    g_cdc_vid, g_cdc_pid, esp_err_to_name (g_last_open_err), over_current ? "yes" : "no");
            }
            else {
                snprintf (status, sizeof (status),
                    "USB: \xE2\x9A\xA0 Initialized | No device seen yet | over_current=%s | "
                    "Connect QMX via USB-A to host port", over_current ? "yes" : "no");
            }
        #else
            snprintf (status, sizeof (status),
                "USB: \xE2\x9A\xA0 Initialized | Waiting for device enumeration | "
                "Connect QMX via USB-A to host port");
        #endif
    }

    return status;
}
