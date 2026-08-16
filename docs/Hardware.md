# SOTAcat Hardware

**Who this is for:** Users wanting to get or build a SOTAcat device

## What is SOTAcat?

A small WiFi module that plugs into your Elecraft KX2 or KX3 ACC (CAT) port, providing wireless control from your phone.

![SOTAcat plugged into KX radio](images/sotacat-on-radio.png)

## Get a SOTAcat

### Option 1: Buy Pre-Made (Recommended)

**K5EM's Inverted Labs Store**
- Professional SMT PCB
- Compact enclosure
- Ready to use

[Purchase from Inverted Labs Store →](https://store.invertedlabs.com/product/sotacat/)

![K5EM build](images/k5em-build.png)

### Option 2: Build Your Own

**Hand-solderable kit**
- [Assembly PDF](Hardware/SOTACAT%20BOM%20-%20schematic%20-%20assembly%20instructions.pdf) — BOM, schematic, instructions
- [YouTube build video](https://www.youtube.com/watch?v=iD3S-9icRn0)

![Hand-built SOTAcat](images/hand-built.png)

### Option 3: DIY from Scratch

- [K5EM's open-source PCB design](https://github.com/invertedlabs/sotacat-pcb/) — KiCad files + 3D-printable enclosure

## Compatibility

Radio support depends on which SOTAcat hardware you have — the two variants
connect to radios in physically different ways and support **different radio
sets**:

| SOTAcat hardware | Connection | Radios |
|------------------|------------|--------|
| Classic SOTAcat (ESP32-C3: K5EM / AB6D boards) | Cable to the radio's ACC/CAT jack | Elecraft KX2, KX3, KH1 |
| ESP32-S3-USB-OTG board | USB cable to the radio's USB port | QRP Labs QMX, Icom IC-705 |

The Elecraft radios have no USB CAT port, so they work only with the classic
(ACC-jack) SOTAcat; the USB radios are supported only on the S3 board.

## SOTAmat App (Beta)

Required for off-grid FT8 self-spotting. SOTAcat launches SOTAmat to enable self-spotting even in challenging conditions where traditional networks are unavailable.

**Note:** This requires the SOTAmat app on your phone plus a receiving gateway within FT8 range.

- [iOS TestFlight](https://testflight.apple.com/join/UQuW6g1E)
- [Android APK](https://1drv.ms/f/s!AhZ33h8betkWjOpAp6J0kgMQex3OWQ?e=xlfzSQ)

---

[Back to Documentation](README.md)

