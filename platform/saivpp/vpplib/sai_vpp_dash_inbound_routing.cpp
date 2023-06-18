#include "sai_vpp.h"

VPP_GENERIC_QUAD_ENTRY(INBOUND_ROUTING_ENTRY, inbound_routing_entry);
VPP_BULK_CREATE_ENTRY_EX(INBOUND_ROUTING_ENTRY, inbound_routing_entry, inbound_routing_entries);
VPP_BULK_REMOVE_ENTRY_EX(INBOUND_ROUTING_ENTRY, inbound_routing_entry, inbound_routing_entries);

const sai_dash_inbound_routing_api_t vpp_dash_inbound_routing_api = {
    VPP_GENERIC_QUAD_API(inbound_routing_entry)
    vpp_bulk_create_inbound_routing_entries,
    vpp_bulk_remove_inbound_routing_entries,
};
