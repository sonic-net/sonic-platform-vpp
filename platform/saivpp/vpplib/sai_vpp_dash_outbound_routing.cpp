#include "sai_vpp.h"

VPP_GENERIC_QUAD_ENTRY(OUTBOUND_ROUTING_ENTRY, outbound_routing_entry);
VPP_BULK_CREATE_ENTRY_EX(OUTBOUND_ROUTING_ENTRY, outbound_routing_entry, outbound_routing_entries);
VPP_BULK_REMOVE_ENTRY_EX(OUTBOUND_ROUTING_ENTRY, outbound_routing_entry, outbound_routing_entries);

const sai_dash_outbound_routing_api_t vpp_dash_outbound_routing_api = {
    VPP_GENERIC_QUAD_API(outbound_routing_entry)
    vpp_bulk_create_outbound_routing_entries,
    vpp_bulk_remove_outbound_routing_entries,
};
