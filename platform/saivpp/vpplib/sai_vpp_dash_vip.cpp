#include "sai_vpp.h"

VPP_GENERIC_QUAD_ENTRY(VIP_ENTRY, vip_entry);
VPP_BULK_CREATE_ENTRY_EX(VIP_ENTRY, vip_entry, vip_entries);
VPP_BULK_REMOVE_ENTRY_EX(VIP_ENTRY, vip_entry, vip_entries);

const sai_dash_vip_api_t vpp_dash_vip_api = {
    VPP_GENERIC_QUAD_API(vip_entry)
    vpp_bulk_create_vip_entries,
    vpp_bulk_remove_vip_entries,
};
