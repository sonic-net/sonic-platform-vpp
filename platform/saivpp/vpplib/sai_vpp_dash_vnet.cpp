#include "sai_vpp.h"

VPP_GENERIC_QUAD(VNET, vnet);
VPP_BULK_CREATE(VNET, vnets);
VPP_BULK_REMOVE(VNET, vnets);

const sai_dash_vnet_api_t vpp_dash_vnet_api = {
    VPP_GENERIC_QUAD_API(vnet)
    vpp_bulk_create_vnets,
    vpp_bulk_remove_vnets,
};
