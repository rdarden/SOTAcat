#!/usr/bin/env python3
"""Bridge a QRP Labs QMX USB virtual COM port to the Raspberry Pi UART.

This script bridges a QRP Labs QMX ultra-compact SSB transceiver (https://qrp-labs.com/qmx.html)
connected via USB to a Raspberry Pi's UART serial interface. It runs on a Pi Zero (typically DigiPi)
to allow SOTAcat firmware (running on an ESP32) to control the QMX via UART.

Flow:
  1. Wait for UART (/dev/serial0) to be ready
  2. Discover QMX USB serial device (/dev/ttyACM0, /dev/ttyUSB0, etc.)
  3. Probe QMX with CAT commands (VN;, ID;, IF;) until response received
  4. Once verified, enter forwarding loop: QMX <-> UART
  5. On USB disconnect or I/O error, wait for QMX to reappear and reconnect
  6. UART connection is maintained across QMX reconnects

Reliability features:
  - Catches both SerialException and OSError (USB disconnect can raise OSError)
  - Graceful reconnection: USB removal doesn't crash the process
  - Heartbeat logging: "bridge heartbeat: running" every 10 seconds
  - Suppresses CR and null bytes from QMX to prevent protocol corruption
"""

import argparse
import glob
import os
import sys
import time
from typing import Optional

import serial

DEBUG = False

# Serial port configuration
QMX_BAUDRATE = 9600  # QMX USB virtual COM port baud rate
UART_BAUDRATE = 38400  # Pi UART baud rate (matches ESP32-C3)
POLL_INTERVAL_SECONDS = 3.0  # Wait time between reconnect attempts
PROBE_TIMEOUT_SECONDS = 0.4  # Timeout for CAT command responses during probe
HEARTBEAT_SECONDS = 10.0  # Interval between status heartbeat messages


def debug_print(message: str) -> None:
    """Print debug message if DEBUG flag is enabled."""
    if DEBUG:
        print(message, flush=True)


def ascii_printable(data: bytes) -> str:
    """Extract printable ASCII characters from bytes."""
    return ''.join(chr(b) for b in data if 32 <= b <= 126)


def format_debug_bytes(data: bytes) -> str:
    """Format bytes as 'hex=XX XX XX ascii=<text>' for logging."""
    if not data:
        return ""
    hex_repr = ' '.join(f"{b:02x}" for b in data)
    ascii_repr = ascii_printable(data)
    return f"hex={hex_repr} ascii={ascii_repr}"


def build_usb_port_candidates() -> list[str]:
    """Build ordered list of candidate USB serial device paths.
    
    Returns ports in priority order: preferred paths first, then discovered
    paths sorted. Removes duplicates and filters out non-existent paths.
    """
    preferred = [
        "/dev/ttyACM0",  # Most common on Pi/Linux
        "/dev/ttyACM1",
        "/dev/ttyACM2",
        "/dev/ttyUSB0",  # Alternative USB serial
        "/dev/ttyUSB1",
        "/dev/ttyUSB2",
        "/dev/tty.usbmodem0",  # macOS
        "/dev/tty.usbmodem1",
        "/dev/tty.usbmodem2",
    ]
    discovered = []
    discovered.extend(glob.glob("/dev/ttyACM*"))
    discovered.extend(glob.glob("/dev/ttyUSB*"))
    discovered.extend(glob.glob("/dev/tty.usbmodem*"))
    discovered = sorted(set(discovered))

    ordered = []
    for path in preferred + discovered:
        if path not in ordered and os.path.exists(path):
            ordered.append(path)
    return ordered


def find_usb_serial_port() -> Optional[str]:
    """Find first available USB serial device (QMX on /dev/ttyACMx or /dev/ttyUSBx).
    
    Returns:
        Path to USB serial device, or None if not found.
    """
    for path in build_usb_port_candidates():
        if os.path.exists(path):
            return path
    return None


def find_uart_port() -> Optional[str]:
    """Find UART device on Pi (typically /dev/serial0).
    
    Returns:
        Path to UART device, or None if not found.
    """
    for path in ("/dev/serial0", "/dev/ttyS0", "/dev/ttyAMA0"):
        if os.path.exists(path):
            return path
    return None


def wait_for_qmx(qmx_port: str) -> serial.Serial:
    """Wait for QMX to appear on USB and verify with CAT probe.
    
    Opens the USB serial port and sends CAT commands (VN;, ID;, IF;) to verify
    the QMX is present and responding. Retries indefinitely with POLL_INTERVAL_SECONDS
    delay between attempts.
    
    Args:
        qmx_port: Path to USB serial device (e.g., /dev/ttyACM0)
        
    Returns:
        Open serial.Serial object connected to QMX at QMX_BAUDRATE.
        
    Raises:
        KeyboardInterrupt: If user interrupts with Ctrl+C.
    """
    while True:
        try:
            ser = serial.Serial(qmx_port, QMX_BAUDRATE, timeout=PROBE_TIMEOUT_SECONDS)
        except (serial.SerialException, OSError) as exc:
            print(f"Unable to open {qmx_port}: {exc}", flush=True)
            time.sleep(POLL_INTERVAL_SECONDS)
            continue

        try:
            for cmd in (b"VN;", b"ID;", b"IF;"):
                ser.write(cmd)
                time.sleep(0.15)
                data = ser.read(128)
                if data:
                    print(f"CAT response from {qmx_port}: {format_debug_bytes(data)}", flush=True)
                    print("QMX found, bridge ready", flush=True)
                    return ser
        except (serial.SerialException, OSError) as exc:
            print(f"QMX probe failed: {exc}", flush=True)
            try:
                ser.close()
            except Exception:
                pass
            time.sleep(POLL_INTERVAL_SECONDS)
            continue

        try:
            ser.close()
        except Exception:
            pass
        print(f"Waiting for QMX USB connection on {qmx_port}...", flush=True)
        time.sleep(POLL_INTERVAL_SECONDS)


def wait_for_uart(uart_port: str) -> serial.Serial:
    """Wait for UART device to be ready.
    
    Opens the UART port at UART_BAUDRATE with 100ms read timeout. Retries
    indefinitely if the port is not ready (e.g., disabled in raspi-config).
    
    Args:
        uart_port: Path to UART device (e.g., /dev/serial0).
        
    Returns:
        Open serial.Serial object connected to UART at UART_BAUDRATE.
        
    Raises:
        KeyboardInterrupt: If user interrupts with Ctrl+C.
    """
    while True:
        try:
            ser = serial.Serial(uart_port, UART_BAUDRATE, timeout=0.1)
            print(f"UART ready at {uart_port}", flush=True)
            return ser
        except (serial.SerialException, OSError) as exc:
            print(f"UART not ready at {uart_port}: {exc}", flush=True)
            print("  → Enable via: sudo raspi-config → 3 Interface Options → I6 Serial Port", flush=True)
            print("  → Select: No (disable login shell), Yes (enable serial port hardware)", flush=True)
            time.sleep(POLL_INTERVAL_SECONDS)


def suppress_control_bytes(data: bytes) -> bytes:
    """Remove CR and null bytes from data to prevent protocol corruption.
    
    QMX may send CR and null bytes that interfere with CAT command framing.
    Removes these control characters before forwarding UART -> QMX.
    
    Args:
        data: Raw bytes from UART.
        
    Returns:
        Filtered bytes with CR and null bytes removed.
    """
    if not data:
        return data

    data = data.replace(b"\r", b"")
    data = data.replace(b"\x00", b"")
    return data


def bridge(qmx: serial.Serial, uart: serial.Serial) -> None:
    """Forward bytes between QMX USB and Pi UART until disconnect.
    
    Continuously checks both ports for incoming data and forwards it to the other:
    - QMX USB -> UART: Raw bytes forwarded
    - UART -> QMX USB: Control bytes (CR, null) stripped before forwarding
    
    Handles graceful shutdown:
    - KeyboardInterrupt (Ctrl+C): Close both ports and exit
    - SerialException: USB disconnected, print message and return
    - OSError: USB I/O error (can happen when device removed), treat as disconnect
    
    Prints heartbeat every HEARTBEAT_SECONDS to confirm process is running.
    
    Args:
        qmx: Open serial.Serial object to QMX USB device.
        uart: Open serial.Serial object to Pi UART.
    """
    last_heartbeat = time.time()
    try:
        while True:
            try:
                if qmx.in_waiting:
                    data = qmx.read(qmx.in_waiting)
                    debug_print(f"QMX -> UART: {format_debug_bytes(data)}")
                    uart.write(data)
            except (serial.SerialException, OSError):
                # QMX became unavailable; will reconnect in main()
                raise
                
            try:
                if uart.in_waiting:
                    data = uart.read(uart.in_waiting)
                    data = suppress_control_bytes(data)
                    if data:
                        debug_print(f"UART -> QMX: {format_debug_bytes(data)}")
                        qmx.write(data)
            except (serial.SerialException, OSError):
                # UART became unavailable; should not happen, but handle gracefully
                raise

            now = time.time()
            if now - last_heartbeat >= HEARTBEAT_SECONDS:
                print("bridge heartbeat: running", flush=True)
                last_heartbeat = now

            time.sleep(0.001)
    except KeyboardInterrupt:
        print("Stopping bridge", flush=True)
        try:
            qmx.close()
        except Exception:
            pass
        try:
            uart.close()
        except Exception:
            pass
        raise
    except (serial.SerialException, OSError) as exc:
        print(f"Bridge disconnected (QMX removed?): {exc}", flush=True)
    finally:
        try:
            qmx.close()
        except Exception:
            pass


def parse_args() -> argparse.Namespace:
    """Parse command-line arguments.
    
    Returns:
        Namespace with debug, uart_baud, qmx_baud attributes.
    """
    parser = argparse.ArgumentParser(
        description="Bridge QMX USB serial port to Raspberry Pi UART for SOTAcat control",
        epilog="Run as: python3 sotacat_qmx_bridge.py [--debug] [--qmx-baud 9600] [--uart-baud 9600]"
    )
    parser.add_argument("--debug", action="store_true", help="enable verbose debug logging")
    parser.add_argument(
        "--uart-baud",
        type=int,
        default=UART_BAUDRATE,
        help=f"baud rate for the Pi UART side (default: {UART_BAUDRATE})"
    )
    parser.add_argument(
        "--qmx-baud",
        type=int,
        default=QMX_BAUDRATE,
        help=f"baud rate for the QMX USB side (default: {QMX_BAUDRATE})"
    )
    return parser.parse_args()


def main() -> None:
    """Main entry point: initialize and run bridge indefinitely.
    
    Flow:
    1. Initialize settings from command-line args
    2. Wait for UART to be ready (typically /dev/serial0)
    3. Loop: wait for QMX USB to appear, probe it, run bridge, handle disconnect
    4. On Ctrl+C, close ports and exit gracefully
    
    The bridge maintains the UART connection across QMX reconnects, so if QMX is
    power-cycled or unplugged, the script just waits for it to reappear.
    """
    global DEBUG, QMX_BAUDRATE, UART_BAUDRATE
    args = parse_args()
    DEBUG = args.debug
    UART_BAUDRATE = args.uart_baud
    QMX_BAUDRATE = args.qmx_baud

    print("Starting QMX bridge (Ctrl+C to stop)", flush=True)
    if DEBUG:
        print("  Debug logging enabled", flush=True)
    print(f"  QMX USB baudrate: {QMX_BAUDRATE}", flush=True)
    print(f"  Pi UART baudrate: {UART_BAUDRATE}", flush=True)

    # Find and open UART (stays open across QMX reconnects)
    uart_port = None
    while uart_port is None:
        uart_port = find_uart_port()
        if uart_port is None:
            print("Waiting for UART device (/dev/serial0, /dev/ttyS0, or /dev/ttyAMA0) ...", flush=True)
            time.sleep(POLL_INTERVAL_SECONDS)

    uart = wait_for_uart(uart_port)
    print(f"UART port: {uart_port}", flush=True)

    # Reconnection loop: wait for QMX, probe it, bridge until disconnect
    while True:
        qmx_port = None
        while qmx_port is None:
            qmx_port = find_usb_serial_port()
            if qmx_port is None:
                print("Waiting for QMX USB serial device (/dev/ttyACM*, /dev/ttyUSB*, etc.)...", flush=True)
                time.sleep(POLL_INTERVAL_SECONDS)

        qmx = wait_for_qmx(qmx_port)
        print(f"Bridging {qmx_port} <-> {uart_port}", flush=True)
        try:
            bridge(qmx, uart)
        except KeyboardInterrupt:
            print("\nShutting down (Ctrl+C received)", flush=True)
            try:
                uart.close()
            except Exception:
                pass
            return
        print("QMX disconnected — waiting for reconnect...", flush=True)
        time.sleep(POLL_INTERVAL_SECONDS)


if __name__ == "__main__":
    main()
