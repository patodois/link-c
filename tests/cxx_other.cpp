// A second C++ translation unit: only the declarations.
#include "../rewrite_link.h"

int other_unit_uses_the_api(void) {
    rbl_decoded d;
    uint8_t junk[3] = {1, 2, 3};
    return rbl_wire_decode(junk, sizeof junk, &d) == RBL_ERR_MALFORMED ? 0 : 1;
}
