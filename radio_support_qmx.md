# Radio Support and QMX Detection

## Purpose
This document summarizes how to determine which radio is connected to the serial interface, the CAT commands used by each supported radio, and which QMX CAT commands should not be implemented in SOTAcat.

The goal is to add QMX support to the SOTAcat firmware while retaining full compatibility with KX2, KX3, KH1, and any other radios already supported by SOTAcat.

## Current implementation snapshot (2026-07-27)
- QMX support is implemented and selectable via `RadioType::QMX`.
- Detection currently probes `VN;` and classifies QMX from the returned content.
- `QMXRadioDriver` is integrated into runtime driver selection and handles frequency, mode, power, volume, TX/RX, keyer message send, and time sync.
- QMX-specific guardrails are active in the web UI: `Min Power`, `Max Power`, `Tune ATU`, and FM mode are disabled when QMX is detected.
- QMX is treated as self-powered in this project.
- ESP32-S3 USB host board routing pins are configured (`USB_SEL` high, `DEV_VBUS_EN`/`BOOST_EN` high to supply VBUS), and the USB host transport layer uses the real `usb_host`/`usb_host_cdc_acm` components (see below), not just a placeholder.

## Documentation references
- QMX web page: https://qrp-labs.com/qmx.html
- QMX operating manual: https://qrp-labs.com/images/qmx/manuals/operation_1_04_004.pdf
- QMX schematics: https://qrp-labs.com/images/qmx/manuals/schematics_rev5.pdf
- QMX CAT manual: https://qrp-labs.com/images/qmx/manuals/cat_1_04_004.pdf
- KX2/KX3 programmer reference: https://ftp.elecraft.com/KX2/Manuals%20Downloads/K3S&K3&KX3&KX2%20Pgmrs%20Ref,%20G5.pdf

## QMX serial-port wiring and voltage compatibility
The QMX documentation confirms that the AUX jack can be configured as an additional TTL serial port.
- AUX jack: 3.5mm tip=TX, ring=RX, configurable baud rate as Serial 1.
- The AUX serial port is 3.3V logic level and is compatible with SOTAcat's 3.3V TTL UART interface.
- QMX GPS/serial signals on the paddle jack are also 3.3V logic level and 5V tolerant, so SOTAcat's 3.3V TTL UART levels are compatible.

This means the plan to use SOTAcat's UART for QMX AUX serial CAT access is reasonable, provided a common ground is shared.

### Suggested wiring diagram
```
QMX USB-C -> Raspberry Pi Zero USB host
  USB-C cable -> USB OTG adapter -> Pi Zero micro-USB data port
  (optionally via a powered USB hub for stable power)

Raspberry Pi Zero UART -> SOTAcat TRRS cable
  Pi GPIO header (UART pins):
    Pin 8  (GPIO14/TXD) -> SOTAcat RX
    Pin 10 (GPIO15/RXD) <- SOTAcat TX
    Pin 6  (GND)       <-> SOTAcat GND

SOTAcat TRRS plug:
    Tip    = SOTAcat TX -> Pi RXD (GPIO15)
    Ring 1 = SOTAcat RX <- Pi TXD (GPIO14)
    Ring 2 = not used for this bridge
    Sleeve = GND -> Pi GND
```
- The QMX is connected over USB-C to the Pi Zero as a USB serial device, not directly to the SOTAcat hardware.
- The Pi Zero provides the TTL bridge between the QMX USB serial port and the SOTAcat UART.
- Use a USB OTG adapter on the Pi Zero data port if you do not have a dedicated USB-A host connector.
- A powered USB hub can be used between the Pi and QMX if the QMX needs extra USB power stability.
- Use Pi GPIO pins 8 and 10 for UART TX/RX, and pin 6 for ground.
- Do not enable the serial login shell in `raspi-config`; only enable the serial hardware interface.
- Ensure the QMX, Pi, and SOTAcat share a common ground reference.

## How a user should configure the QMX AUX serial port
For SOTAcat to talk to QMX over the AUX jack, the AUX jack must be enabled as a serial port and the baud rate must match SOTAcat's UART setting.

### Option 1: On-device menu
Use the QMX on-screen/system menu and navigate to the serial-port settings:
- Enable Serial 1 on AUX.
- Set Serial 1 baud to the desired value (for example 9600 as the safest default, or 19200/38400 if both ends use the same rate).
- Save/restart if required by the firmware version.

### Option 2: Virtual serial-port CAT commands
If the user already has a terminal connection to QMX, the firmware can also configure the serial ports through the serial terminal interface. The relevant settings are the serial-port configuration menu entries for:
- Serial 1 on AUX (enable/disable)
- Serial 1 baud (set the baud rate)

For the initial implementation, SOTAcat should assume a conservative default of 9600 bps for QMX AUX until the radio is detected and the active baud is confirmed.

## Current QMX support architecture
On startup, SOTAcat detects radio type during `KXRadio::connect()` and then selects a model-specific driver.

Current behavior in firmware:
- a startup detection step probes for QMX using `VN;`
- radio type is stored in `m_radio_type`
- `KXRadio::select_driver()` dispatches to `QMXRadioDriver` when QMX is detected
- compatibility behavior for KX2/KX3/KH1 is preserved via existing driver paths

## Radio detection strategy
The current implementation uses a practical subset of this strategy.

### Implemented probe
- `VN;` is sent during connect and responses are checked for QMX-identifying patterns.

### Candidate probes for future hardening
- `UI;` — unique chip ID, QMX-specific
- `GP;` — GPS coordinates/date-time, QMX-specific
- `Q0;` … `Q9;`, `QA;` … `QC;` — QMX extended session parameters
- `PL;` — QMX PLL parameter control
- `MU;` — QMX configuration reload
- `MM...` / `ML...` — QMX menu manager discovery/access
- `TA<freq>;` / `TA0;` — QMX transmit audio tone control
- `TM;` and `TMhhmmss;` — QMX RTC control

### Response-based discrimination
Some commands exist on multiple radios, but the response can differentiate QMX from KX/KH1:
- `ID;` — supported by KX2/KX3 and QMX, but the returned model/ID value is different. QMX returns a TS-480-compatible ID value (`020`) and KX radios return a KX-specific ID.

### Commands not suitable for detection
- `OM;`, `IF;`, `RX;`, `TX;`, `SW;`, `TB;`, `ML;` are not reliable for detection because they are present on KX2/KX3/KH1 or are generic across radios.

## Supported radios
The code currently supports:
- `KX2`
- `KX3`
- `KH1`
- `QMX`

## ESP32-S3 USB host board policy for self-powered QMX
For the ESP32-S3 USB-OTG board target, startup now applies the following policy:
- `USB_SEL` is set high to route D+/D- to the host connector path.
- `DEV_VBUS_EN` and `LIMIT_EN` are set high so the board's incoming VBUS is passed through (via the
  current-limiting IC) to the host connector; `BOOST_EN` (the alternate, battery-powered VBUS
  source) stays low since we're cable-powered. QMX being self-powered means it doesn't draw its
  operating current from VBUS, but it still needs VBUS present to detect the USB attach and begin
  enumeration.
- The onboard USB-to-serial debug bridge (separate Micro-USB connector) is wired to UART0
  (GPIO43/44) -- the same pins this project already uses for the wired CAT-radio UART -- so it
  can't be used as a second console without conflicting with that.

Current limitation:
- USB host serial transport is not fully implemented yet (enumeration and endpoint transfer path are TODO).

## CAT command table
| Purpose | KX2 / KX3 | KH1 | QMX |
|---|---|---|---|
| Detect radio / baud negotiation | `;RVR;`, `BR3;`, `;AI0;` | `;I;`, `;RVR;` | N/A |
| Read frequency | `FA;` | `DS1;` | `FA;`, `FB;` |
| Set frequency | `FA<11-digit>;` | `FA<8-digit>;` | `FA<11-digit>;`, `FB<11-digit>;` |
| Read mode | `MD;` | `DS1;` | `MD;` |
| Set mode | `MD<n>;` | `MD0;`, `MD1;`, `MD2;` | `MD<n>;` |
| Read power | `PC;` | `DS1;` | `PC;` |
| Set power | `PC<3-digit>;` | `SW2H;SW2H;` | `PC<nn>;` (direct power control) |
| Read volume | `AG;` | `DS1;` | `AG;` |
| Set volume | `AG<3-digit>;` | `ENAU;` / `ENAD;` | `AG0nnn;` |
| Read transmit state | `TQ;` | `DS1;` | `TQ;` |
| Set transmit state | `TX;`, `RX;` | `HK1;`, `HK0;` | `TQ1;` / `TQ0;`, `TX;`, `RX;` |
| Send keyer text | `KY <text>;` | `SW2T;SW1T;` + `HK1;` / `HK0;` pulses | `KY <text>;`, `KSnnn;` |
| Play message bank | `SWT11;SWT19;`, `SWT11;SWT27;` | `SW4T;SW1T;`, `SW4T;SW2T;` | N/A |
| Tune ATU | `SWT44;` (KX3), `SWT20;` (KX2) | `SW3T;` | N/A |
| Sync/read clock | `DS;`, `SWT19;`, `SWT20;`, `SWT27;` | `DS2;`, `MNTIM;`, `SW2T;`, `SW4T;` | `TM;`, `Tmhhmmss;` |
| FT8 tone / FSK tone | `FR0;`, `FT0;`, `SWH16;` | `FO00;`, `FO99;`, `FO<offset>;`, `HK1;`, `HK0;` | `TA<freq>;`, `TA0;` |
| Menu item access | `MN<3-digit>;`, `MP<3-digit>;` | N/A | N/A |
| General status / model info | `OM;`, `DS;` | `DS1;`, `DS2;` | `IF;`, `OM;`, `VN;`, `ID;`, `UI;`, `GP;` |

## Gaps and feature limitations for QMX
Based on current information, the following SOTAcat features may not be fully portable to QMX or may behave differently:

### 1. Menu-based power control
- KX2/KX3 use `MN` / `MP` menu item access for TUN PWR and other menu-based settings.
- QMX does not use the same menu navigation or menu entry numbering for power.
- SOTAcat should not implement `MN` / `MP` for QMX; instead, use QMX-native direct CAT controls.

### 2. FT8 tone control and radio keying
- QMX uses `TA` for transmit audio tone and `TX` / `RX` for transmit control.
- KX uses `SWH16` and `FO` plus keyer pulse commands.
- Ensure the QMX driver does not reuse KX-specific FT8/FSK command patterns.

### 3. QMX-only commands that are not part of KX/KH1 compatibility
- `Q0..QB;`, `PL;`, `MU;`, `MM/ML;`, `GP;`, `VN;`, `UI;`, `TM;`, `TA;`.
- These can be used for detection or advanced QMX functionality, but they are outside the KX/KH1 compatibility layer.

### 4. Command overlap and response semantics
- `ID;` is supported by both QMX and KX2/KX3, so detection must inspect the returned ID rather than just command success.
- `IF;` and `OM;` are generic status commands and do not reliably distinguish radio type.
- QMX does not expose a known CAT command for querying the configured AF gain step size, so SOTAcat assumes 2 dB per web UI volume step in QMX mode.

## Recommended detection workflow
1. On startup, probe the serial link for an attached radio.
2. Send `VN;` and check for QMX-identifying response markers.
3. If QMX is not confirmed, continue existing KX/KH1 detection path.
4. Initialize the radio driver for the detected model and dispatch subsequent CAT commands through the model-specific handler.
5. Future improvement: add `UI;` and/or `GP;` as secondary QMX confirmation probes.

## Pi Zero / DigiPi bridge workaround for a regular QMX
This is a practical bridge option when the regular QMX does not expose a convenient AUX serial port and you want to keep SOTAcat unchanged.

### Hardware setup
1. Boot the DigiPi distro on the Raspberry Pi Zero.
2. Connect the QMX to a USB hub with a USB-C to USB-A cable. The hub should be connected to the Pi Zero.
3. Join the DigiPi Wi-Fi network and open `digipi.local` in a browser.
4. Open the Shell, then enter the password when prompted.
5. Run `sudo remount`.
8. Create the bridge file with `vi sotacat_qmx_bridge.py` and paste in the example below.
9. If the Pi reports that `serial0` is not available, run `sudo raspi-config`, open the Interfaces menu, and enable serial hardware. Do not enable the login shell on serial.
10. Reboot the Pi Zero.

### Example bridge program
Use the file [sotacat_qmx_bridge.py](sotacat_qmx_bridge.py) in the repository root as a starting point. The script:
- discovers the QMX USB serial device,
- probes the QMX with CAT commands such as `VN;`, `ID;`, and `IF;`,
- prints `QMX found, bridge ready` once a response is seen,
- retries every few seconds if the QMX is not yet connected or powered on,
- forwards bytes between the QMX USB serial port and the Pi UART so SOTAcat sees a serial link.

### Install the Python serial dependency
```bash
sudo apt update
sudo apt install -y python3-serial
```

### Run the bridge
```bash
python3 sotacat_qmx_bridge.py
```

With the Pi UART enabled and the QMX connected over USB, this bridge should provide a workable path to test SOTAcat-style CAT communication without modifying SOTAcat hardware or adding USB host support to the ESP32 firmware.

### Run as a systemd service on DigiPi
Create a systemd unit file named `qmxcatbridge.service` under the DigiPi systemd tree (for example `/etc/systemd/system/` on the target host or `systemd/system/` in the repo) with the following contents:

```ini
[Unit]
Description=QMX CAT Bridge
After=network.target

[Service]
ExecStart=/usr/bin/python3 /home/pi/sotacat_qmx_bridge.py
WorkingDirectory=/home/pi/
StandardOutput=inherit
StandardError=inherit
Restart=no
User=pi
TimeoutStopSec=3

[Install]
WantedBy=multi-user.target
```

Then enable and start it:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now qmxcatbridge
sudo systemctl status qmxcatbridge
```

The DigiPi web UI toggle can then control this service by starting and stopping `qmxcatbridge`.

### View service logs
Use journalctl to inspect bridge startup and runtime output:

```bash
sudo journalctl -u qmxcatbridge.service --since '1 hour ago'
sudo journalctl -u qmxcatbridge.service -n 100 --no-pager
sudo journalctl -u qmxcatbridge.service -f
```

## Testing plan
### Unit / integration tests
- Add unit tests for the detection logic using mocked serial responses.
- Validate that QMX-only commands (`VN;`, `UI;`, `GP;`) produce QMX identification.
- Validate that `ID;` response parsing distinguishes KX2/KX3 from QMX.

### Hardware / live tests
- Test startup detection with a KX2 connected.
- Test startup detection with a KX3 connected.
- Test startup detection with a KH1 connected.
- Test startup detection with a QMX connected.
- Confirm that after detection, a known command like frequency read/write behaves correctly for each radio.

### Compatibility checks
- Verify that KX2/KX3/KH1 behavior is unchanged when detection is added.
- Verify that QMX detection does not accidentally identify a KX or KH1 as QMX.
- Verify that commands routed through the QMX handler do not invoke KX-specific menu access patterns.
- Verify that QMX AUX/PTT serial ports share ground with SOTAcat and use 3.3V TTL logic.

### Regression scope
- TUN PWR / menu-item access should continue to work for KX2/KX3 via `MN` / `MP`.
- QMX should use native direct commands rather than KX menu-based commands.
- Ensure no new KX/KH1 command paths are broken by the QMX detection addition.

## Notes
- The QMX documentation references the AUX jack and modified PTT jack as 3.3V logic serial ports (5V tolerant), making them compatible with SOTAcat's 3.3V UART interface.
- The two CAT documentation sources are the authoritative references for command syntax and supported features.
- Any future QMX feature added for advanced capabilities should be isolated to the QMX driver layer, not the shared KX/KH1 compatibility layer.
