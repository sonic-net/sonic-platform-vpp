#pragma once
# include "SwitchStateBase.h"
namespace saivpp
{
    class SwitchStateBase;
    class TunnelManager {
    public:
        TunnelManager(SwitchStateBase* switch_db): m_switch_db(switch_db) {};
        // sai_status_t create_tunnel_mapper(
        //                 _In_ const std::string &serializedObjectId,
        //                 _In_ sai_object_id_t switch_id,
        //                 _In_ uint32_t attr_count,
        //                 _In_ const sai_attribute_t *attr_list);

        // sai_status_t create_tunnel_mapper_entry(
        //                 _In_ const std::string &serializedObjectId,
        //                 _In_ sai_object_id_t switch_id,
        //                 _In_ uint32_t attr_count,
        //                 _In_ const sai_attribute_t *attr_list);

        // sai_status_t create_tunnel(
        //                 _In_ const std::string &serializedObjectId,
        //                 _In_ sai_object_id_t switch_id,
        //                 _In_ uint32_t attr_count,
        //                 _In_ const sai_attribute_t *attr_list);

        sai_status_t create_tunnel_encap_nexthop(
                        _In_ sai_object_id_t serializedObjectId,
                        _In_ sai_object_id_t switch_id,
                        _In_ uint32_t attr_count,
                        _In_ const sai_attribute_t *attr_list);

        // sai_status_t remove_tunnel_mapper(SwitchStateBase& switch_db) {
        //     // Implementation of remove_tunnel_mapper
        // }        
        // sai_status_t remove_tunnel_mapper_entry(SwitchStateBase& switch_db) {
        //     // Implementation of remove_tunnel_mapper_entry
        // }
        // sai_status_t remove_tunnel(SwitchStateBase& switch_db) {
        //     // Implementation of remove_tunnel
        // }    
        sai_status_t remove_tunnel_encap_nexthop(
            _In_ sai_object_type_t object_type,
            _In_ sai_object_id_t object_id) {
            // Implementation of remove_tunnel_encap_nexthop
            return SAI_STATUS_SUCCESS;
        }
        sai_status_t get_tunnel_if(
            _In_  sai_object_id_t nexthop_oid, 
            _Out_ std::string &if_name) {
            // Implementation of get_tunnel_if_by_nexthop_oid
            // You need to implement this method to return the tunnel_if based on the given nexthop_oid
            auto it = m_tunnel_encap_nexthop_map.find(nexthop_oid);
            if (it != m_tunnel_encap_nexthop_map.end()) {
                if_name = it->second;
                return SAI_STATUS_SUCCESS;
            }
            return SAI_STATUS_ITEM_NOT_FOUND;
        }

    private:
        SwitchStateBase* m_switch_db;
        std::unordered_map<sai_object_id_t, std::string> m_tunnel_encap_nexthop_map;
    };
}