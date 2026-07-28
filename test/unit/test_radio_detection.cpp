#include "radio_detection.h"

#include <cassert>
#include <cstring>

int main() {
    assert(!looks_like_qmx_response(nullptr, 0));
    assert(!looks_like_qmx_response("", 0));
    assert(looks_like_qmx_response("QMX 1.0", 7));
    assert(looks_like_qmx_response("VN QMX", 6));
    assert(!looks_like_qmx_response("OM APF---TBXI01;", 16));
    return 0;
}
