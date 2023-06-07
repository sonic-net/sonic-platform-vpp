#include "sai_vpp.h"

VPP_GENERIC_QUAD(ARS, ars);

const sai_ars_api_t vpp_ars_api = {
    VPP_GENERIC_QUAD_API(ars)
};
