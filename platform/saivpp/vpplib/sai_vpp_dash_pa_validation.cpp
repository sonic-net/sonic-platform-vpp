#include "sai_vpp.h"

VPP_GENERIC_QUAD_ENTRY(PA_VALIDATION_ENTRY, pa_validation_entry);
VPP_BULK_CREATE_ENTRY_EX(PA_VALIDATION_ENTRY, pa_validation_entry, pa_validation_entries);
VPP_BULK_REMOVE_ENTRY_EX(PA_VALIDATION_ENTRY, pa_validation_entry, pa_validation_entries);

const sai_dash_pa_validation_api_t vpp_dash_pa_validation_api = {
    VPP_GENERIC_QUAD_API(pa_validation_entry)
    vpp_bulk_create_pa_validation_entries,
    vpp_bulk_remove_pa_validation_entries,
};
