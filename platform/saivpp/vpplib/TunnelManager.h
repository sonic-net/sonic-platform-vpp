#pragma once
# include "SwitchStateBase.h"
namespace saivpp
{
    class SwitchStateBase;
    enum class Action {
        CREATE,
        UPDATE,
        DELETE
    };

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
                        _In_ const std::string& serializedObjectId,
                        _In_ sai_object_id_t switch_id,
                        _In_ uint32_t attr_count,
                        _In_ const sai_attribute_t *attr_list);


        sai_status_t remove_tunnel_encap_nexthop(
                        _In_ const std::string& serializedObjectId);

        sai_status_t get_tunnel_if(
            _In_  sai_object_id_t nexthop_oid, 
            _Out_ u_int32_t &sw_if_index) {
            // Implementation of get_tunnel_if_by_nexthop_oid
            // You need to implement this method to return the tunnel_if based on the given nexthop_oid
            auto it = m_tunnel_encap_nexthop_map.find(nexthop_oid);
            if (it != m_tunnel_encap_nexthop_map.end()) {
                sw_if_index = it->second;
                return SAI_STATUS_SUCCESS;
            }
            return SAI_STATUS_ITEM_NOT_FOUND;
        }

    private:
        SwitchStateBase* m_switch_db;
        //nexthop SAI object ID to sw_if_index map
        std::unordered_map<sai_object_id_t, u_int32_t> m_tunnel_encap_nexthop_map;
        sai_status_t tunnel_encap_nexthop_action(
                        _In_ const SaiObject* tunnel_nh_obj, 
                        _In_ Action action);
    };
}