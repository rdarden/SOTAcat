# QMX Integration Summary

## Purpose
This document summarizes the work completed for adding QMX support and a DigiPi-based QMX CAT bridge to the SOTAcat project.

## What was added

### 1. DigiPi web UI integration
- Added a `QMX CAT Bridge` toggle to `var/www/html/index.php` in the DigiPi repo.
- The toggle starts and stops a `qmxcatbridge` systemd service via `systemctl`.
- The toggle also reflects the current service state and resets failed service status for `qmxcatbridge`.
- Patch saved in the SOTAcat repo as `digipi-qmxcatbridge.patch`.

### 2. QMX bridge script
- Created `sotacat_qmx_bridge.py` in the SOTAcat root.
- The script:
  - discovers available USB serial devices for the QMX virtual COM port,
  - probes with CAT commands like `VN;`, `ID;`, and `IF;` until QMX responds,
  - logs a ready message and heartbeats,
  - forwards bytes between the QMX USB serial device and the Pi UART (`/dev/serial0` or equivalent).
- The script uses 9600 baud by default for both QMX and the Pi UART.

### 3. Documentation updates
- Updated `radio_support_qmx.md` with:
  - a practical Pi Zero / DigiPi bridge workflow,
  - instructions for creating a `qmxcatbridge.service` unit,
  - how to view logs with `journalctl`,
  - updated wiring diagram details.
- Removed the unnecessary working-directory creation instructions from the bridge setup.
- Added the USB OTG adapter option for connecting QMX USB-C to the Pi Zero.

### 4. Firmware/QMX integration work
- Extended `RadioType` to include `QMX` in `include/kx_radio.h`.
- Added `QMX` to `get_radio_type_string()`.
- Added `radio_detection.h` / `src/radio_detection.cpp` helper for recognizing QMX responses.
- Updated `src/kx_radio.cpp` to probe `VN;` during connect and detect QMX.
- Added a fallback QMX detection path when `OM` detection fails.
- Added `include/radio_driver_qmx.h` and `src/radio_driver_qmx.cpp` with a minimal QMX driver.
- Updated `src/radio_driver_qmx.cpp` so QMX power (`PC`) writes are sent without readback verification, avoiding known QMX response-format issues.
- Updated `src/web/run.js` to load the detected radio type on the RUN page and disable `Min Power`, `Max Power`, and `Tune ATU` buttons when QMX is connected.
- Integrated `QMXRadioDriver` into the existing `KXRadio::select_driver()` logic.

## Current status
- QMX detection is implemented in firmware and can be recognized during radio connect.
- QMX-specific power writes are now sent without readback verification to avoid malformed response handling.
- The RUN page now loads radio type and disables `Min Power`, `Max Power`, and `Tune ATU` controls when connected to QMX.
- Full QMX CAT command support remains minimal; power and tune operations are gated until behavior is stabilized.

## Notes for handoff

### Files changed or added
- `digipi-qmxcatbridge.patch`
- `sotacat_qmx_bridge.py`
- `radio_support_qmx.md`
- `include/kx_radio.h`
- `src/kx_radio.cpp`
- `include/radio_driver_qmx.h`
- `src/radio_driver_qmx.cpp`
- `include/radio_detection.h`
- `src/radio_detection.cpp`
- `var/www/html/index.php` (in DigiPi repo)

### Recommended next steps
- Confirm the `qmxcatbridge.service` unit file exists and points to `/home/pi/sotacat_qmx_bridge.py`.
- Build and flash SOTAcat firmware to verify the QMX detection changes compile and run.
- Test the Pi bridge on the DigiPi host with QMX connected over USB-C and SOTAcat connected to the Pi UART.
- Use `sudo systemctl status qmxcatbridge` and `sudo journalctl -u qmxcatbridge.service -f` for runtime debugging.
- Verify the DigiPi UI toggle correctly starts/stops the service.

## Known limitations
- The QMX driver implementation is minimal and only supports a subset of direct CAT commands.
- The detection helper is currently based on `VN;` response content and only basic QMX identification.
- Full QMX-specific command support and advanced features still need further implementation and testing.
