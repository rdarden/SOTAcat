#include "hardware_specific.h"
#include "build_info.h"

#include <cstdio>
#include <driver/gpio.h>
#include <driver/uart.h>

#include <esp_log.h>
static const char * TAG8 = "sc:hw_spec.";

SOTAcat_HW_Type HW_TYPE     = SOTAcat_HW_Type::unknown;
const char *    HW_TYPE_STR = "unknown";
uart_port_t     UART_NUM;
gpio_num_t      UART2_TX_PIN = ((gpio_num_t)-1);
gpio_num_t      UART2_RX_PIN = ((gpio_num_t)-1);
gpio_num_t      LED_BLUE     = ((gpio_num_t)-1);
gpio_num_t      LED_RED_SUPL = ((gpio_num_t)-1);
gpio_num_t      LED_RED      = ((gpio_num_t)-1);
gpio_num_t      I2C_SCL_PIN  = ((gpio_num_t)-1);
gpio_num_t      I2C_SDA_PIN  = ((gpio_num_t)-1);
gpio_num_t      USB_DET_PIN  = ((gpio_num_t)-1);
int             LED_OFF;
int             LED_ON;
int             ADC_BATTERY;

static SOTAcat_HW_Type detect_hardware_type (void) {
    ESP_LOGV (TAG8, "trace: %s()", __func__);
    /*
     * Here is where we should do the magic to determine
     * which hardware we are using.
     * See https://github.com/SOTAmat/SOTAcat/pull/42
     * and https://sota-na.slack.com/archives/C06N224JD1R/p1722108863599379 (ff.)
     * for possible methods.
     */

    // configure GPIO6 as input with a weak pull-down
    gpio_config_t io_conf;
    io_conf.intr_type    = GPIO_INTR_DISABLE;     // Disable interrupt
    io_conf.mode         = GPIO_MODE_INPUT;       // Set as input mode
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_6);  // Select GPIO6
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;  // Enable weak pull-down resistor
    io_conf.pull_up_en   = GPIO_PULLUP_DISABLE;   // Disable pull-up resistor
    gpio_config (&io_conf);

    // read the GPIO6 level
    int gpio_level = gpio_get_level (GPIO_NUM_6);

    // de-init GPIO6 (set back to default configuration)
    gpio_reset_pin (GPIO_NUM_6);

    // determine hardware type based on GPIO level
    if (gpio_level == 1) {
        ESP_LOGI (TAG8, "K5EM_1 hardware detected");
        return SOTAcat_HW_Type::K5EM_1;  // GPIO6 is high, K5EM_1 detected
    }
    else {
        ESP_LOGI (TAG8, "AB6D_1 hardware detected");
        return SOTAcat_HW_Type::AB6D_1;  // GPIO6 is low, AB6D_1 detected
    }
}

const char * get_version_string (void) {
    static char version[64];
    if (version[0] == '\0')
        snprintf (version, sizeof (version), "%s:%s-%s", HW_TYPE_STR, BUILD_DATE_TIME, SC_BUILD_TYPE);
    return version;
}

void set_hardware_specific (void) {
    ESP_LOGV (TAG8, "trace: %s()", __func__);

    LED_OFF = 1;
    LED_ON  = 0;

    // Default UART configuration (will be overridden for specific hardware below)
    UART_NUM = UART_NUM_1;
    UART2_RX_PIN = ((gpio_num_t)20);
    LED_BLUE     = ((gpio_num_t)10);
    ADC_BATTERY  = 0;

    // Check if we're running bare XIAO FIRST, before any other detection
    #ifdef SEEED_XIAO
        // BARE XIAO ESP32C3 uses UART0 with GPIO20/GPIO21
        UART_NUM = UART_NUM_0;
        UART2_TX_PIN = ((gpio_num_t)21);  // GPIO21 = D6 (TX)
        UART2_RX_PIN = ((gpio_num_t)20);  // GPIO20 = D7 (RX)
        HW_TYPE = SOTAcat_HW_Type::K5EM_1;  // Use K5EM_1 type to avoid inversion
        HW_TYPE_STR = "XIAO_BARE";
        LED_RED = ((gpio_num_t)-1);  // No red LED on bare board
        LED_RED_SUPL = ((gpio_num_t)-1);  // No supplementary LED
        USB_DET_PIN = ((gpio_num_t)-1);  // No USB detection pin
        I2C_SCL_PIN = ((gpio_num_t)-1);  // No I2C on bare board
        I2C_SDA_PIN = ((gpio_num_t)-1);  // No I2C on bare board
        ESP_LOGI (TAG8, "Bare XIAO ESP32C3 detected");
    #elif defined(ESP32_S3)
        // ESP32-S3 USB OTG Dev Board
        UART_NUM = UART_NUM_0;  // Use default UART0 for serial communication
        UART2_TX_PIN = ((gpio_num_t)43);  // GPIO43 (TX)
        UART2_RX_PIN = ((gpio_num_t)44);  // GPIO44 (RX)
        HW_TYPE = SOTAcat_HW_Type::K5EM_1;
        HW_TYPE_STR = "ESP32_S3_USB_OTG";
        LED_BLUE = ((gpio_num_t)1);  // Blue LED on GPIO1 (if available)
        LED_RED = ((gpio_num_t)-1);  // No red LED
        LED_RED_SUPL = ((gpio_num_t)-1);  // No supplementary LED
        USB_DET_PIN = ((gpio_num_t)-1);  // USB detection via native USB OTG
        I2C_SCL_PIN = ((gpio_num_t)8);   // I2C SCL
        I2C_SDA_PIN = ((gpio_num_t)9);   // I2C SDA
        ADC_BATTERY = 1;
        ESP_LOGI (TAG8, "ESP32-S3 USB OTG Dev Board detected");
    #else
        // Production hardware detection
        HW_TYPE = detect_hardware_type();
        
        switch (HW_TYPE) {
        case SOTAcat_HW_Type::AB6D_1:
            HW_TYPE_STR  = "AB6D_1";
            UART2_TX_PIN = ((gpio_num_t)21);
            LED_RED_SUPL = ((gpio_num_t)9);
            LED_RED      = ((gpio_num_t)8);
            break;
        case SOTAcat_HW_Type::K5EM_1:
            HW_TYPE_STR  = "K5EM_1";
            UART2_TX_PIN = ((gpio_num_t)4);  // deconflict with the fsbl outputs
            LED_RED      = ((gpio_num_t)9);
            LED_RED_SUPL = ((gpio_num_t)-1);  // remove second control line for red/amber LED
            USB_DET_PIN  = ((gpio_num_t)3);   // add USB detection
            I2C_SCL_PIN  = ((gpio_num_t)7);   // add I2C/SMBus battery monitor
            I2C_SDA_PIN  = ((gpio_num_t)6);   // add I2C/SMBus battery monitor
            break;
        default:
            ESP_LOGE (TAG8, "unknown hardware");
            break;
        }
    #endif
}
