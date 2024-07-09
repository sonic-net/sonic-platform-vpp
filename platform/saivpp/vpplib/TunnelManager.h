#pragma once
# include "SwitchStateBase.h"
#include "vppxlate/SaiVppXlate.h"

namespace saivpp
{
    class SwitchStateBase;
    enum class Action {
        CREATE,
        UPDATE,
        DELETE
    };
    /**
     * @brief The TunnelVPPData class represents the VPP data associated with a tunnel.
     */
    class TunnelVPPData {
    public:
        TunnelVPPData() : sw_if_index(0), encap_vrf_id(0) {}
        u_int32_t sw_if_index;
        u_int32_t encap_vrf_id;
        u_int32_t bd_id;
        vpp_ip_addr_t bvi_addr;
        std::shared_ptr<IpVrfInfo> ip_vrf;
    };

    class TunnelManager {
    public:
        TunnelManager(SwitchStateBase* switch_db);

        // sai_status_t create_tunnel_map_entry(
        //                 _In_ const std::string &serializedObjectId,
        //                 _In_ sai_object_id_t switch_id,
        //                 _In_ uint32_t attr_count,
        //                 _In_ const sai_attribute_t *attr_list);

        /**
         * @brief Create a tunnel encap nexthop entry.
         *
         * This function creates a tunnel encap nexthop entry with the specified attributes.
         *
         * @param serializedObjectId The serialized object ID of the tunnel encap nexthop entry.
         * @param switch_id The switch ID.
         * @param attr_count The number of attributes in the attribute list.
         * @param attr_list The attribute list.
         * @return The status of the operation.
         */
        sai_status_t create_tunnel_encap_nexthop(
                _In_ const std::string& serializedObjectId,
                _In_ sai_object_id_t switch_id,
                _In_ uint32_t attr_count,
                _In_ const sai_attribute_t *attr_list);


        /**
         * @brief Remove a tunnel encap nexthop entry.
         *
         * This function removes the tunnel encap nexthop entry with the specified serialized object ID.
         *
         * @param serializedObjectId The serialized object ID of the tunnel encap nexthop entry to be removed.
         * @return The status of the operation.
         */
        sai_status_t remove_tunnel_encap_nexthop(
                _In_ const std::string& serializedObjectId);

        /**
         * @brief Get the tunnel interface index based on the given nexthop OID.
         *
         * This method returns the tunnel interface associated with the given nexthop OID.
         *
         * @param nexthop_oid The nexthop OID.
         * @param sw_if_index The output parameter to store the tunnel interface index.
         * @return The status of the operation.
         */
        sai_status_t get_tunnel_if(
            _In_  sai_object_id_t nexthop_oid, 
            _Out_ u_int32_t &sw_if_index) {
            auto it = m_tunnel_encap_nexthop_map.find(nexthop_oid);
            if (it != m_tunnel_encap_nexthop_map.end()) {
                sw_if_index = it->second.sw_if_index;
                return SAI_STATUS_SUCCESS;
            }
            return SAI_STATUS_ITEM_NOT_FOUND;
        }

    private:
        SwitchStateBase* m_switch_db;
        std::array<uint8_t, 6> m_router_mac;
        //nexthop SAI object ID to sw_if_index map
        std::unordered_map<sai_object_id_t, TunnelVPPData> m_tunnel_encap_nexthop_map;

        sai_status_t tunnel_encap_nexthop_action(
                        _In_ const SaiObject* tunnel_nh_obj, 
                        _In_ Action action);
        
        const std::array<uint8_t, 6>& get_router_mac() const;

        sai_status_t create_vpp_vxlan_encap(
                        _In_  vpp_vxlan_tunnel_t& req,
                        _Out_ TunnelVPPData& tunnel_data);

        sai_status_t remove_vpp_vxlan_encap(
                        _In_  vpp_vxlan_tunnel_t& req,            
                        _In_ TunnelVPPData& tunnel_data);

        sai_status_t create_vpp_vxlan_decap(
                        _Out_ TunnelVPPData& tunnel_data);

        sai_status_t remove_vpp_vxlan_decap(
                        _In_ TunnelVPPData& tunnel_data);
                        
    };

}