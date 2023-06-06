#include "sai_vpp.h"

VPP_GENERIC_QUAD_ENTRY(ENI_ETHER_ADDRESS_MAP_ENTRY, eni_ether_address_map_entry);
VPP_BULK_CREATE_ENTRY_EX(ENI_ETHER_ADDRESS_MAP_ENTRY, eni_ether_address_map_entry, eni_ether_address_map_entries);
VPP_BULK_REMOVE_ENTRY_EX(ENI_ETHER_ADDRESS_MAP_ENTRY, eni_ether_address_map_entry, eni_ether_address_map_entries);

VPP_GENERIC_QUAD(ENI, eni);
VPP_BULK_CREATE(ENI, enis);
VPP_BULK_REMOVE(ENI, enis);

const sai_dash_eni_api_t vpp_dash_eni_api = {
    VPP_GENERIC_QUAD_API(eni_ether_address_map_entry)
    vpp_bulk_create_eni_ether_address_map_entries,
    vpp_bulk_remove_eni_ether_address_map_entries,

    VPP_GENERIC_QUAD_API(eni)
    vpp_bulk_create_enis,
    vpp_bulk_remove_enis,
};
