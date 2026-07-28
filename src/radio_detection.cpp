#include "radio_detection.h"

#include <cstring>

bool looks_like_qmx_response(const char *response, std::size_t response_length) {
    if (response == nullptr || response_length == 0) {
        return false;
    }

    const char *needle = "QMX";
    const char *found = std::strstr(response, needle);
    if (found != nullptr) {
        return true;
    }

    const char *prefix = "VN";
    if (response_length >= 2 && std::memcmp(response, prefix, 2) == 0) {
        return true;
    }

    return false;
}
