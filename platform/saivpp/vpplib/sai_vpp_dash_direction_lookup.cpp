#include "sai_vpp.h"

VPP_GENERIC_QUAD_ENTRY(DIRECTION_LOOKUP_ENTRY, direction_lookup_entry);
VPP_BULK_CREATE_ENTRY_EX(DIRECTION_LOOKUP_ENTRY, direction_lookup_entry, direction_lookup_entries);
VPP_BULK_REMOVE_ENTRY_EX(DIRECTION_LOOKUP_ENTRY, direction_lookup_entry, direction_lookup_entries);

const sai_dash_direction_lookup_api_t vpp_dash_direction_lookup_api = {
    VPP_GENERIC_QUAD_API(direction_lookup_entry)
    vpp_bulk_create_direction_lookup_entries,
    vpp_bulk_remove_direction_lookup_entries,
};
