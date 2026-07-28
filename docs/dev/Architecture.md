# Architecture

**Who this is for:** Developers wanting to understand the codebase

## System Overview

```
┌─────────────┐     WiFi      ┌─────────────┐     CAT      ┌─────────────┐
│   Browser   │◄────────────►│   SOTAcat   │◄────────────►│  KX2/KX3/KH1 │
│   (Phone)   │    HTTP       │   (ESP32)   │    Serial    │   Radio    │
└─────────────┘               └─────────────┘               └─────────────┘
                                    │
                                    ▼
                              ┌─────────────┐
                              │  SOTAmat    │
                              │   (App)     │
                              └─────────────┘
```

## Key Components

### Web Server
- ESP32 serves embedded web UI
- Assets gzip-compressed (`.htmlgz`, `.jsgz`, `.cssgz`)
- REST API for all radio/device operations

### REST API
- `GET/PUT /api/v1/frequency` — VFO frequency
- `GET/PUT /api/v1/mode` — Operating mode
- `GET/PUT /api/v1/power` — TX power
- `PUT /api/v1/keyer?message=<text>` — Send text as CW, or as RTTY/PSK31 when the radio is already in DATA mode with FSK-D or PSK-D sub-mode
- `PUT /api/v1/xmit` — Toggle TX
- See `src/` for full endpoint list

### CAT Driver
- Serial communication with Elecraft radio
- 38400 baud default (KX2 / KX3)
- 9600 baud default (KH1)
- Handles command/response protocol

### FT8 Synthesis
- Per-radio CAT driver implementation
- DIGI mode (MD6) for QMX, USB mode (MD0) for KX2/KX3
- Shaped Blackman-Harris RF envelope for key-up/key-down
- No audio required
- Computes and transmits 15-second FT8 sequence
- API: `/api/v1/prepareft8`, `/api/v1/ft8`, `/api/v1/cancelft8`
- See [Radio-Drivers.md](Radio-Drivers.md) for implementation details

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

`main.js` holds `RADIO_CAPABILITIES`, a per-radio table of native bands and modes (KX2 / KX3 / KH1; unknown radios = `null` = permissive). It's read by:

- The CHASE band filter (`AppState.filterBandsEnabled`, default on, exposed in Settings as "Show only bands my radio can access") — opt-out so transverter users can disable it.
- Helpers `getRadioBands(requireTx)`, `getRadioModes(requireTx)`, `radioCanTransmit(band, mode)` for any future gating.

The run-page band/mode buttons are deliberately **not** gated by this table — gating them would lock out users running transverters.

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

