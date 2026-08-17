#pragma once

#include "radio_driver.h"

/**
 * Radio driver for the Icom IC-705, speaking Icom's binary CI-V protocol over
 * the active CAT transport (in practice, USB CDC-ACM on the ESP32-S3 USB-host
 * board; the radio's CI-V CAT port is CDC interface 0 of its composite USB
 * device).
 *
 * Unlike the ASCII Kenwood-style drivers (KX/KH1/QMX), all radio I/O goes
 * through the civ:: framing helpers (include/civ_protocol.h), which implement
 * the drain-and-keep-last read strategy needed to coexist with the radio's
 * unsolicited transceive broadcasts.
 *
 * Capability notes (see docs/dev/Radio-Drivers.md for protocol details):
 *  - MODE_DATA / MODE_DATA_R map to USB-D / LSB-D via the radio's separate
 *    data-mode flag rather than distinct mode codes.
 *  - FT8 is synthesized by stepping the dial frequency under a CW carrier
 *    (the IC-705 has no CAT command for audio tone generation). CW mode is
 *    used rather than FM specifically because CW has no audio-modulation
 *    path, so the carrier cannot be contaminated by mic pickup the way an
 *    FM carrier structurally could be -- see ft8_prepare()'s comment in
 *    radio_driver_ic705.cpp for the pending bench-verification items.
 *  - ATU tune tries the native tuner protocol first (AH-705-compatible
 *    tuners), then falls back to keying a low-power carrier for RF-sensing
 *    tuners; refused above 6 m where no supported tuner operates.
 *  - Message banks play the radio's voice TX memories (voice modes only;
 *    CI-V exposes no trigger for the CW keyer memories).
 */
class IC705RadioDriver : public IRadioDriver {
  public:
    bool supports_keyer () const override;
    bool supports_volume () const override;

    bool get_frequency (KXRadio & radio, long & out_hz) override;
    bool set_frequency (KXRadio & radio, long hz, int tries) override;

    bool get_mode (KXRadio & radio, radio_mode_t & out_mode) override;
    bool set_mode (KXRadio & radio, radio_mode_t mode, int tries) override;

    bool get_power (KXRadio & radio, long & out_power) override;
    bool set_power (KXRadio & radio, long power) override;

    bool get_volume (KXRadio & radio, long & out_volume) override;
    bool set_volume (KXRadio & radio, long volume) override;

    bool get_xmit_state (KXRadio & radio, long & out_state) override;
    bool set_xmit_state (KXRadio & radio, bool on) override;

    bool play_message_bank (KXRadio & radio, int bank) override;
    bool tune_atu (KXRadio & radio) override;

    bool send_keyer_message (KXRadio & radio, const char * message) override;

    bool sync_time (KXRadio & radio, const RadioTimeHms & client_time) override;

    bool get_radio_state (KXRadio & radio, kx_state_t * state) override;
    bool restore_radio_state (KXRadio & radio, const kx_state_t * state, int tries) override;

    bool ft8_prepare (KXRadio & radio, long rfFreq, int audioFreq) override;
    void ft8_tone_on (KXRadio & radio) override;
    void ft8_tone_off (KXRadio & radio) override;
    void ft8_set_tone (KXRadio & radio, long rfFreq, int audioFreq, long frequency) override;
};
