#include "sai_vpp.h"

VPP_GENERIC_QUAD(POE_DEVICE,poe_device);
VPP_GENERIC_QUAD(POE_PSE,poe_pse);
VPP_GENERIC_QUAD(POE_PORT,poe_port);

const sai_poe_api_t vpp_poe_api = {

    VPP_GENERIC_QUAD_API(poe_device)
    VPP_GENERIC_QUAD_API(poe_pse)
    VPP_GENERIC_QUAD_API(poe_port)
};
