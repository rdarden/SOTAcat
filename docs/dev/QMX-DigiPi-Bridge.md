# QMX via DigiPi Bridge (UART path)

**Who this is for:** Users connecting a QRP Labs QMX to original (ESP32-C3)
SOTAcat hardware, where the QMX's USB CAT port must be bridged to SOTAcat's
wired UART. (On the ESP32-S3-USB-OTG board, SOTAcat talks to the QMX directly
over USB host — see [Radio-Drivers](Radio-Drivers.md) — and no bridge is
needed.)

The bridge is a Raspberry Pi Zero running DigiPi: the QMX plugs into the Pi
over USB, and the Pi's hardware UART connects to SOTAcat's TRRS CAT cable.
The bridge script [`sotacat_qmx_bridge.py`](../../sotacat_qmx_bridge.py)
forwards bytes both ways; [`digipi-qmxcatbridge.patch`](../../digipi-qmxcatbridge.patch)
adds a start/stop toggle to the DigiPi web UI.

## Wiring

```
QMX USB-C -> Raspberry Pi Zero USB host
  USB-C cable -> USB OTG adapter -> Pi Zero micro-USB data port
  (optionally via a powered USB hub for stable power)

Raspberry Pi Zero UART -> SOTAcat TRRS cable
  Pin 8  (GPIO14/TXD) -> SOTAcat RX
  Pin 10 (GPIO15/RXD) <- SOTAcat TX
  Pin 6  (GND)       <-> SOTAcat GND

SOTAcat TRRS plug:
  Tip    = SOTAcat TX -> Pi RXD (GPIO15)
  Ring 1 = SOTAcat RX <- Pi TXD (GPIO14)
  Ring 2 = not used for this bridge
  Sleeve = GND -> Pi GND
```

- All three devices must share a common ground.
- Enable the Pi's serial *hardware* in `raspi-config`; do **not** enable the
  serial login shell.
- Pi UART and QMX AUX signals are 3.3 V logic, compatible with SOTAcat's UART.

## Setup on DigiPi

1. Boot the DigiPi distro on the Pi Zero and join its Wi-Fi network.
2. Open `digipi.local` in a browser, open the Shell, and run `sudo remount`.
3. Install the serial dependency: `sudo apt update && sudo apt install -y python3-serial`
4. Copy `sotacat_qmx_bridge.py` (repo root) to `/home/pi/`.
5. Test interactively: `python3 sotacat_qmx_bridge.py` — it discovers the
   QMX's USB serial device, probes it (`VN;`, `ID;`, `IF;`), prints
   `QMX found, bridge ready`, and then forwards bytes between the QMX and the
   Pi UART. It retries every few seconds until the QMX appears.

## Run as a systemd service

Create `/etc/systemd/system/qmxcatbridge.service`:

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

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now qmxcatbridge
sudo systemctl status qmxcatbridge
```

The DigiPi web UI toggle (added by `digipi-qmxcatbridge.patch`) starts and
stops this service.

Logs: `sudo journalctl -u qmxcatbridge.service -f`

---

[← Radio-Drivers](Radio-Drivers.md)
