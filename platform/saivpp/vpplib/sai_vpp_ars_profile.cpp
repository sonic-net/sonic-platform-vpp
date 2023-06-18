#include "sai_vpp.h"

VPP_GENERIC_QUAD(ARS_PROFILE, ars_profile);

const sai_ars_profile_api_t vpp_ars_profile_api = {
    VPP_GENERIC_QUAD_API(ars_profile)
};
