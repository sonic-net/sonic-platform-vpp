#include "swss/logger.h"
#include "meta/sai_serialize.h"
#include "SwitchStateBase.h"
#include "SaiObjectDB.h"
#include "TunnelManager.h"
#include "IpVrfInfo.h"
#include "vppxlate/SaiVppXlate.h"
#include "SwitchStateBaseUtils.h"

using namespace saivpp;
sai_status_t 
TunnelManager::tunnel_encap_nexthop_action(
                    _In_ const SaiObject* tunnel_nh_obj, 
                    _In_ Action action)
{
    sai_attribute_t              attr;
    sai_ip_address_t             src_ip;
    sai_ip_address_t             dst_ip;
    sai_status_t                 status = SAI_STATUS_SUCCESS;
    std::unordered_map<u_int32_t, std::shared_ptr<IpVrfInfo>> vni_to_vrf_map;
    sai_object_id_t              object_id;
    uint8_t                      nbr_mac[8] = {0,0,0,0,0,1};
    SWSS_LOG_ENTER();
    
    sai_deserialize_object_id(tunnel_nh_obj->get_id(), object_id);     
    auto tunnel_obj = tunnel_nh_obj->get_linked_object(SAI_OBJECT_TYPE_TUNNEL, SAI_NEXT_HOP_ATTR_TUNNEL_ID);
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
    CHECK_STATUS_W_MSG(tunnel_nh_obj->get_attr(attr), "Missing SAI_NEXT_HOP_ATTR_IP in %s", tunnel_nh_obj->get_id().c_str());

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
        auto mapper_table = std::dynamic_pointer_cast<SaiDBObject>(tunnel_encap_mapper);
        if (!mapper_table) {
            SWSS_LOG_ERROR("Failed to cast tunnel_encap_mapper to SaiDBObject. OID %s", 
                tunnel_encap_mapper->get_id().c_str());
            return SAI_STATUS_FAILURE;
        }
        auto tunnel_encap_mapper_entries = mapper_table->get_child_objs(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY);
        if (tunnel_encap_mapper_entries == nullptr) {
            SWSS_LOG_DEBUG("Empty tunnel_encap_mapper table. OID %s", 
                tunnel_encap_mapper->get_id().c_str());
            continue;
        }
        for (auto pair : *tunnel_encap_mapper_entries) {
            auto tunnel_encap_mapper_entry = pair.second;
            vpp_vxlan_tunnel_t req;
            u_int32_t sw_if_index;
            u_int32_t tunnel_vni;

            memset(&req, 0, sizeof(req));
            req.instance = ~0;
            sai_ip_address_t_to_vpp_ip_addr_t(src_ip, req.src_address);
            sai_ip_address_t_to_vpp_ip_addr_t(dst_ip, req.dst_address);
            req.is_l3 = 1;

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
    
            req.vni = tunnel_vni;
            bool is_add = 0;
            if (action == Action::CREATE) {
                is_add = 1;
                status = vpp_vxlan_tunnel_add_del(&req, is_add, &sw_if_index);
                SWSS_LOG_INFO("create vxlan tunnel src %s dst %s vni %d: sw_if_index,%d, status %d", 
                        sai_serialize_ip_address(src_ip).c_str(),
                        sai_serialize_ip_address(dst_ip).c_str(),
                        tunnel_vni, sw_if_index, status);            

                m_tunnel_encap_nexthop_map[object_id] = sw_if_index;
                if (req.dst_address.sa_family == AF_INET6) {
                    ip6_nbr_add_del(NULL, sw_if_index, &req.dst_address.addr.ip6, false, nbr_mac, is_add);
                } else {
                    ip4_nbr_add_del(NULL, sw_if_index, &req.dst_address.addr.ip4, false, nbr_mac, is_add);
                }
                
                SWSS_LOG_INFO("set ip neighbor %d %s 00:00:00:00:00:01",
                        sw_if_index, sai_serialize_ip_address(dst_ip).c_str());
            } else if (action == Action::DELETE) {
                is_add = 0;
                auto encap_map_it = m_tunnel_encap_nexthop_map.find(object_id);
                if (encap_map_it == m_tunnel_encap_nexthop_map.end()) {
                    SWSS_LOG_ERROR("Failed to find sw_if_index for %s", 
                        tunnel_nh_obj->get_id().c_str());
                    continue;
                }
                sw_if_index = encap_map_it->second;
                if (req.dst_address.sa_family == AF_INET6) {
                    ip6_nbr_add_del(NULL, sw_if_index, &req.dst_address.addr.ip6, false, nbr_mac, is_add);
                } else {
                    ip4_nbr_add_del(NULL, sw_if_index, &req.dst_address.addr.ip4, false, nbr_mac, is_add);
                }
                
                SWSS_LOG_INFO("delete ip neighbor %d %s 00:00:00:00:00:01",
                        sw_if_index, sai_serialize_ip_address(dst_ip).c_str());

                status = vpp_vxlan_tunnel_add_del(&req, is_add, &sw_if_index);
                SWSS_LOG_INFO("delete vxlan tunnel src %s dst %s vni %d: sw_if_index,%d, status %d", 
                        sai_serialize_ip_address(src_ip).c_str(),
                        sai_serialize_ip_address(dst_ip).c_str(),
                        tunnel_vni, sw_if_index, status);            

                m_tunnel_encap_nexthop_map.erase(encap_map_it);
            }
        }
    }
    return SAI_STATUS_SUCCESS;
}
sai_status_t 
TunnelManager::create_tunnel_encap_nexthop(
                    _In_ const std::string& serializedObjectId,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list) 
{
    SWSS_LOG_ENTER();
    
    SWSS_LOG_DEBUG("enter create_tunnel_encap_nexthop %s", serializedObjectId.c_str());
    SaiCachedObject tunnel_nh_obj(m_switch_db, SAI_OBJECT_TYPE_NEXT_HOP, serializedObjectId, attr_count, attr_list);
    return tunnel_encap_nexthop_action(&tunnel_nh_obj, Action::CREATE);
}

sai_status_t 
TunnelManager::remove_tunnel_encap_nexthop(
                _In_ const std::string& serializedObjectId) 
{
    SWSS_LOG_ENTER();
    auto tunnel_nh_obj = m_switch_db->get_sai_object(SAI_OBJECT_TYPE_NEXT_HOP, serializedObjectId);

    if (!tunnel_nh_obj) {
        SWSS_LOG_ERROR("Failed to find SAI_OBJECT_TYPE_NEXT_HOP SaiObject: %s", serializedObjectId);
        return SAI_STATUS_FAILURE;
    } 
    return tunnel_encap_nexthop_action(tunnel_nh_obj.get(), Action::DELETE);
 }