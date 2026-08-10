#include "ic705_modes.h"

namespace ic705 {

bool icom_to_radio_mode (uint8_t icom_mode, bool data_flag, radio_mode_t & out_mode) {
    switch (icom_mode) {
    case MODE_LSB: out_mode = ::MODE_LSB; break;
    case MODE_USB: out_mode = ::MODE_USB; break;
    case MODE_AM: out_mode = ::MODE_AM; break;
    case MODE_CW: out_mode = ::MODE_CW; break;
    case MODE_FM: out_mode = ::MODE_FM; break;
    case MODE_CW_R: out_mode = ::MODE_CW_R; break;
    case MODE_RTTY: out_mode = ::MODE_DATA; break;
    case MODE_RTTY_R: out_mode = ::MODE_DATA_R; break;
    default:
        return false;
    }

    // SSB with the data flag set is USB-D/LSB-D -- report as DATA.
    if (data_flag) {
        if (out_mode == ::MODE_USB)
            out_mode = ::MODE_DATA;
        else if (out_mode == ::MODE_LSB)
            out_mode = ::MODE_DATA_R;
    }
    return true;
}

bool radio_mode_to_icom (radio_mode_t mode, uint8_t & out_icom_mode, uint8_t & out_filter, bool & out_data_flag) {
    out_filter    = FIL1;
    out_data_flag = false;

    switch (mode) {
    case ::MODE_LSB: out_icom_mode = MODE_LSB; break;
    case ::MODE_USB: out_icom_mode = MODE_USB; break;
    case ::MODE_AM: out_icom_mode = MODE_AM; break;
    case ::MODE_FM: out_icom_mode = MODE_FM; break;
    case ::MODE_CW:
        out_icom_mode = MODE_CW;
        out_filter    = FIL2;
        break;
    case ::MODE_CW_R:
        out_icom_mode = MODE_CW_R;
        out_filter    = FIL2;
        break;
    case ::MODE_DATA:  // FT8 and friends: USB-D
        out_icom_mode = MODE_USB;
        out_data_flag = true;
        break;
    case ::MODE_DATA_R:
        out_icom_mode = MODE_LSB;
        out_data_flag = true;
        break;
    default:
        return false;
    }
    return true;
}

}  // namespace ic705
