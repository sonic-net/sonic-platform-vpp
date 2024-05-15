#include "swss/logger.h"
#include "meta/sai_serialize.h"
#include "SwitchStateBase.h"
#include "SaiObjectDB.h"
#include "TunnelManager.h"
#include "IpVrfInfo.h"

using namespace saivpp;
sai_status_t 
TunnelManager::create_tunnel_encap_nexthop(
                    _In_ sai_object_id_t object_id,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list) 
{
    sai_attribute_t              attr;
    sai_ip_address_t             src_ip;
    sai_ip_address_t             dst_ip;
    std::unordered_map<u_int32_t, std::shared_ptr<IpVrfInfo>> vni_to_vrf_map;
    std::string                  serializedObjectId = sai_serialize_object_id(object_id);
    SWSS_LOG_ENTER();
    SWSS_LOG_ERROR("enter create_tunnel_encap_nexthop %s", serializedObjectId.c_str());
    SaiCachedObject tunnel_nh_obj(m_switch_db, SAI_OBJECT_TYPE_NEXT_HOP, serializedObjectId, attr_count, attr_list);
    auto tunnel_obj = tunnel_nh_obj.get_linked_object(SAI_OBJECT_TYPE_TUNNEL, SAI_NEXT_HOP_ATTR_TUNNEL_ID);
    if (tunnel_obj == nullptr) {
        return SAI_STATUS_FAILURE;
    }
    attr.id = SAI_TUNNEL_ATTR_TYPE;
    CHECK_STATUS_W_MSG(tunnel_obj->get_attr(attr), "Missing SAI_TUNNEL_ATTR_TYPE in tunnel obj");

    if (attr.value.s32 != SAI_TUNNEL_TYPE_VXLAN) {
        SWSS_LOG_ERROR("Unsupported tunnel encap type %d in %s", attr.value.s32,
                        tunnel_obj->get_id().c_str());
        return SAI_STATUS_NOT_IMPLEMENTED;
    }

    attr.id = SAI_TUNNEL_ATTR_ENCAP_SRC_IP;
    CHECK_STATUS_W_MSG(tunnel_obj->get_attr(attr), "Missing SAI_TUNNEL_ATTR_ENCAP_SRC_IP in tunnel obj");
    // SAI_TUNNEL_ATTR_ENCAP_TTL_MODE and SAI_TUNNEL_ATTR_ENCAP_TTL_VAL are not supported in vpp
    src_ip = attr.value.ipaddr;
    
    attr.id = SAI_NEXT_HOP_ATTR_IP;
    CHECK_STATUS_W_MSG(tunnel_nh_obj.get_attr(attr), "Missing SAI_NEXT_HOP_ATTR_IP in %s", serializedObjectId.c_str());

    dst_ip = attr.value.ipaddr;
    
    // Iterate tunnel encap mapper
    auto tunnel_encap_mappers = tunnel_obj->get_linked_objects(SAI_OBJECT_TYPE_TUNNEL_MAP, SAI_TUNNEL_ATTR_ENCAP_MAPPERS);
    
    for (auto tunnel_encap_mapper : tunnel_encap_mappers) {
        attr.id = SAI_TUNNEL_MAP_ATTR_TYPE;
        CHECK_STATUS_W_MSG(tunnel_encap_mapper->get_attr(attr), 
                "Missing SAI_TUNNEL_MAP_ATTR_TYPE in %s", 
                tunnel_encap_mapper->get_id().c_str());
        if (attr.value.s32 != SAI_TUNNEL_MAP_TYPE_VIRTUAL_ROUTER_ID_TO_VNI) {
            continue;
        }
        auto mapper_table = std::dynamic_pointer_cast<SaiTable>(tunnel_encap_mapper);
        if (!mapper_table) {
            SWSS_LOG_ERROR("Failed to cast tunnel_encap_mapper to SaiTable. OID %s", 
                tunnel_encap_mapper->get_id().c_str());
            return SAI_STATUS_FAILURE;
        }
        auto tunnel_encap_mapper_entries = mapper_table->get_entries();
        for (auto pair : tunnel_encap_mapper_entries) {
            auto tunnel_encap_mapper_entry = pair.second;
            u_int32_t tunnel_vni;
            attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_VALUE;
            CHECK_STATUS_W_MSG(tunnel_encap_mapper_entry->get_attr(attr), 
                "Missing SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY in %s", 
                tunnel_encap_mapper_entry->get_id().c_str());
            tunnel_vni = attr.value.u32;
            attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_KEY;
            CHECK_STATUS_W_MSG(tunnel_encap_mapper_entry->get_attr(attr), 
                    "Missing SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_KEY in %s", 
                    tunnel_encap_mapper_entry->get_id().c_str());
            auto ip_vrf = m_switch_db->vpp_get_ip_vrf(attr.value.oid);
            if (!ip_vrf) {
                SWSS_LOG_ERROR("Failed to find VR from SAI_TUNNEL_MAP_ENTRY_ATTR_VIRTUAL_ROUTER_ID_KEY in %s", 
                    tunnel_encap_mapper_entry->get_id().c_str());
                return SAI_STATUS_FAILURE;
            }
            vni_to_vrf_map[tunnel_vni] = ip_vrf;
    
            SWSS_LOG_ERROR("create vxlan tunnel src %s dst %s vni %d", 
                    sai_serialize_ip_address(src_ip).c_str(),
                    sai_serialize_ip_address(dst_ip).c_str(),
                    tunnel_vni);
            //TODO: store vrf+nexthop-oid -> tunnel_id mapping
            m_tunnel_encap_nexthop_map[object_id] = "vxlan_tunnel0";
            SWSS_LOG_ERROR("set ip neighbor vxlan_tunnel%d %s 00:00:00:00:00:01",
                    0 /*tunnel_id*/, sai_serialize_ip_address(dst_ip).c_str());
            //Call VPP Xlate API to create tunnel and nexthop in VPP
        }
        
    }
    return SAI_STATUS_SUCCESS;
}
