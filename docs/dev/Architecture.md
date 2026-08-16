# Architecture

**Who this is for:** Developers wanting to understand the codebase

## System Overview

```
┌─────────────┐     WiFi      ┌─────────────┐     CAT      ┌─────────────┐
│   Browser   │◄────────────►│   SOTAcat   │◄────────────►│    Radio    │
│   (Phone)   │    HTTP       │   (ESP32)   │  (see below) │             │
└─────────────┘               └─────────────┘               └─────────────┘
                                    │
                                    ▼
                              ┌─────────────┐
                              │  SOTAmat    │
                              │   (App)     │
                              └─────────────┘
```

The CAT link — and therefore which radios are supported — depends on the
hardware variant the firmware is built for:

| Hardware | Radio link | Supported radios |
|----------|------------|------------------|
| ESP32-C3 (classic SOTAcat: K5EM / AB6D boards) | TTL UART via the radio's ACC jack | Elecraft KX2, KX3, KH1 |
| ESP32-S3-USB-OTG | USB host (CDC-ACM) | QRP Labs QMX, Icom IC-705 |

The two sets are disjoint by construction: the S3 build never scans its UART
(`connect()` in `src/kx_radio.cpp` — the dev board's UART pins double as its
debug console), and the Elecraft radios have no USB CAT interface to offer the
S3's host port (Elecraft's KXUSB cable is a vendor-specific serial adapter,
not a CDC-ACM device, so it would need an additional host driver).

## Key Components

### Web Server
- ESP32 serves embedded web UI
- Assets gzip-compressed (`.htmlgz`, `.jsgz`, `.cssgz`)
- REST API for all radio/device operations

### REST API
- `GET/PUT /api/v1/frequency` — VFO frequency
- `GET/PUT /api/v1/mode` — Operating mode
- `GET/PUT /api/v1/power` — TX power
- `PUT /api/v1/keyer?message=<text>` — Send text as CW; on Elecraft radios already in DATA mode with FSK-D or PSK-D sub-mode, the radio keys it as RTTY/PSK31 instead (QMX and IC-705 keyer paths are CW-only)
- `PUT /api/v1/xmit` — Toggle TX
- See `src/` for full endpoint list

### CAT Drivers
- One driver per radio family behind the `IRadioDriver` interface; see [Radio-Drivers.md](Radio-Drivers.md)
- Elecraft KX2/KX3 — ASCII CAT over UART, forced to 38400 baud after detection
- Elecraft KH1 — reduced Elecraft dialect over UART at 9600 baud (some state is read by parsing the `DS` display strings)
- QRP Labs QMX — Kenwood TS-480-style ASCII CAT over USB CDC
- Icom IC-705 — binary CI-V protocol over USB CDC (line coding pinned to 19200)

### FT8 Synthesis
- No audio path: the RF carrier itself is keyed and stepped via CAT, one command per FT8 tone
- The carrier/keying technique is per-radio (details in [Radio-Drivers.md](Radio-Drivers.md)):
  - **KX2/KX3** — CW mode (MD3), carrier keyed via `SWH16;` (hold-XMIT = TUNE), tones by rewriting `FA`; transmits at a fixed 10 W TUN PWR
  - **KH1** — CW offset zeroed (`FO00;`), carrier keyed via `HK1;`/`HK0;`, tones by rewriting `FA`
  - **QMX** — DIGI mode (MD6), carrier and tones via `TA`; the QMX firmware shapes key-up/key-down with a Blackman-Harris RF envelope
  - **IC-705** — FM mode + PTT for a clean carrier, tones via CI-V set-frequency; transmits at the operator's RF POWER setting
- Note the deliberate power-policy split: Elecraft FT8 forces 10 W (TUNE carrier power), while the IC-705 honors whatever power the operator has set
- Computes and transmits the 79-symbol (~12.6 s) FT8 sequence within its 15-second slot
- API: `/api/v1/prepareft8`, `/api/v1/ft8`, `/api/v1/cancelft8`

### SOTAmat Integration
- Bidirectional communication with SOTAmat app
- App can read/set frequency, mode
- Triggers FT8 self-spot sequence

### Key Web Modules

The web UI is a small set of focused JS modules. See [Web-UI.md](Web-UI.md) for APIs and implementation notes.

- **`spots.js`** — single source of truth for spot data. Owns fetch, localStorage cache, rate-limit/dedup, auto-refresh, and a subscribe/notify channel that any page (CHASE, RUN, ...) reads from.
- **`bandprivileges.js`** — FCC privilege tables (HF + VHF/UHF), mode categories, bandwidth/edge helpers, and `MODE_SNAP_HZ` used by drag-to-tune.
- **`main.js`** — `tuneRadioHz()` (band/mode-aware tune; SSB auto-sideband by frequency), `RADIO_CAPABILITIES` (per-radio native band/mode table), `AppState` (including the opt-out `filterBandsEnabled` for CHASE).
- **`run.js`** — band-range chart, spot-tick rendering on the chart, drag-to-tune (mouse) / tap-to-jump (touch), tap-to-tune on spot ticks.
- **`chase.js`** — spot list + scan; consumes `spots.js`, applies optional radio-band filter.

### Radio Capabilities and Transverters

`main.js` holds `RADIO_CAPABILITIES`, a per-radio table of native bands and modes (KX2 / KX3 / KH1 / IC705; unknown radios = `null` = permissive). The QMX is **deliberately absent**: its hardware ships in band-group variants, so any static entry would be wrong for someone's unit (see the future-work note in [Radio-Drivers.md](Radio-Drivers.md) about querying coverage at connect time). The table is read by:

- The CHASE band filter (`AppState.filterBandsEnabled`, default on, exposed in Settings as "Show only bands my radio can access") — opt-out so transverter users can disable it.
- Helpers `getRadioBands(requireTx)`, `getRadioModes(requireTx)`, `radioCanTransmit(band, mode)` for any future gating.

The run-page band/mode buttons are deliberately **not** gated by this table — gating them would lock out users running transverters. Per-radio gating does exist separately, in `run.js`, for controls a radio's CAT interface genuinely cannot drive regardless of transverters: `RADIO_UNSUPPORTED_FEATURES` (e.g. power/ATU/FM on QMX) and `RADIO_MSG_BANKS` (Msg button availability by radio and mode).

### Firmware Distribution

GitHub Releases is the authoritative firmware source (#100). The OTA flow and the `make github-release` target both target the project's Releases page directly; mirrors are not trusted.

## Source Layout

```
src/
├── main.cpp           # Entry point
├── web/               # Embedded web assets
│   ├── *.html
│   ├── *.js           # spots.js, run.js, chase.js, main.js, settings.js, ...
│   └── *.css
├── ...                # CAT, API, FT8 code
```

Any new file added under `src/web/` must be wired in *two* places — see [Web-UI.md → Asset Pipeline](Web-UI.md#asset-pipeline).

---

[← BUILD](BUILD.md) · [Radio-Drivers →](Radio-Drivers.md) · [Web UI →](Web-UI.md)

