#include "sai_vpp.h"

VPP_GENERIC_QUAD_ENTRY(OUTBOUND_CA_TO_PA_ENTRY, outbound_ca_to_pa_entry);
VPP_BULK_CREATE_ENTRY_EX(OUTBOUND_CA_TO_PA_ENTRY, outbound_ca_to_pa_entry, outbound_ca_to_pa_entries);
VPP_BULK_REMOVE_ENTRY_EX(OUTBOUND_CA_TO_PA_ENTRY, outbound_ca_to_pa_entry, outbound_ca_to_pa_entries);

const sai_dash_outbound_ca_to_pa_api_t vpp_dash_outbound_ca_to_pa_api = {
    VPP_GENERIC_QUAD_API(outbound_ca_to_pa_entry)
    vpp_bulk_create_outbound_ca_to_pa_entries,
    vpp_bulk_remove_outbound_ca_to_pa_entries,
};
