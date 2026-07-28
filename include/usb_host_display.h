#pragma once

#include <stdint.h>

/**
 * Minimal text dashboard on the ESP32-S3-USB-OTG board's onboard 240x240 ST7789
 * LCD (SPI, GPIO4/5/6/7/8/9), used to show USB host diagnostics without needing
 * WiFi or a console connection -- both of which may be unreachable once USB
 * host mode claims the chip's single USB PHY.
 *
 * Not a general-purpose display driver: fixed-size text grid, no scrolling,
 * no graphics beyond what usb_serial_host.cpp needs to show its status.
 */

/// A handful of RGB565 colors for usb_host_display_set_line()'s color argument.
#define DISPLAY_COLOR_WHITE  0xFFFF
#define DISPLAY_COLOR_GREEN  0x07E0
#define DISPLAY_COLOR_YELLOW 0xFFE0
#define DISPLAY_COLOR_RED    0xF800
#define DISPLAY_COLOR_GRAY   0x8410

/**
 * Initialize the SPI bus and ST7789 panel, clear the screen, and draw a title.
 * Safe to call even if not on ESP32-S3 hardware or the panel isn't present --
 * becomes a no-op (only compiled in under #ifdef ESP32_S3 at the call site).
 */
void usb_host_display_init (void);

/**
 * Redraw one fixed-position row of text (row 0 is reserved for the title).
 * The row is cleared to black first, so shorter text simply erases whatever
 * was there before -- no need to pad with spaces.
 *
 * @param row 1-based text row index (0 is the title row).
 * @param text Text to draw, truncated to fit the row if too long.
 * @param color RGB565 foreground color, e.g. DISPLAY_COLOR_GREEN.
 */
void usb_host_display_set_line (int row, const char * text, uint16_t color);
