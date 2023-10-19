#include "sai_vpp.h"

VPP_GENERIC_QUAD(TWAMP_SESSION,twamp_session);
VPP_GENERIC_STATS(TWAMP_SESSION,twamp_session);

const sai_twamp_api_t vpp_twamp_api = {
    VPP_GENERIC_QUAD_API(twamp_session)
    VPP_GENERIC_STATS_API(twamp_session)
};
