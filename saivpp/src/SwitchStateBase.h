/*
 * Copyright 2016 Microsoft, Inc.
 * Modifications copyright (c) 2023 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "SwitchState.h"
#include "FdbInfo.h"
#include "HostInterfaceInfo.h"
#include "WarmBootState.h"
#include "SwitchConfig.h"
#include "RealObjectIdManager.h"
#include "EventPayloadNetLinkMsg.h"
#include "MACsecManager.h"
#include "IpVrfInfo.h"
#include "SaiObjectDB.h"
#include "BitResourcePool.h"
#include "TunnelManager.h"
#include "vppxlate/SaiVppXlate.h"

#include <set>
#include <unordered_set>
#include <vector>
#include <list>

#include "SwitchStateBaseNexthop.h"
#include "SwitchStateBaseAcl.h"

#define BFD_MUTEX std::lock_guard<std::mutex> lock(bfdMapMutex);

#define SAI_VPP_FDB_INFO "SAI_VPP_FDB_INFO"

#define DEFAULT_VLAN_NUMBER 1

#define MAX_OBJLIST_LEN 128

#define CHECK_STATUS(status) {                                  \
    sai_status_t _status = (status);                            \
    if (_status != SAI_STATUS_SUCCESS) { SWSS_LOG_ERROR("ERROR status %d", status); return _status; } }
#define CHECK_STATUS_W_MSG(status, msg, ...) {                                  \
    sai_status_t _status = (status);                            \
    if (_status != SAI_STATUS_SUCCESS) { \
        char buffer[512]; \
        snprintf(buffer, 512, msg, ##__VA_ARGS__); \
        SWSS_LOG_ERROR("%s: status %d", buffer, status); \
        return _status; } }
#define CHECK_STATUS_QUIET(status) {                                  \
    sai_status_t _status = (status);                            \
    if (_status != SAI_STATUS_SUCCESS) { return _status; } }
typedef struct vpp_ace_cntr_info_ {
    sai_object_id_t tbl_oid;
    sai_object_id_t ace_oid;
    uint32_t acl_index;
    uint32_t ace_index;
} vpp_ace_cntr_info_t;

namespace saivpp
{
    class SwitchStateBase:
        public SwitchState
    {
        public:

            typedef std::map<sai_object_id_t, std::shared_ptr<SwitchStateBase>> SwitchStateMap;

        public:

            SwitchStateBase(
                    _In_ sai_object_id_t switch_id,
                    _In_ std::shared_ptr<RealObjectIdManager> manager,
                    _In_ std::shared_ptr<SwitchConfig> config);

            SwitchStateBase(
                    _In_ sai_object_id_t switch_id,
                    _In_ std::shared_ptr<RealObjectIdManager> manager,
                    _In_ std::shared_ptr<SwitchConfig> config,
                    std::shared_ptr<WarmBootState> warmBootState);

            virtual ~SwitchStateBase();

        protected:

            virtual sai_status_t set_switch_mac_address();

            virtual sai_status_t set_switch_supported_object_types();

            virtual sai_status_t set_switch_default_attributes();

            virtual sai_status_t create_default_vlan();

            virtual sai_status_t create_cpu_port();

            virtual sai_status_t create_default_1q_bridge();

            virtual sai_status_t create_ports();

            virtual sai_status_t set_port_list();

            virtual sai_status_t set_port_capabilities();

            virtual sai_status_t create_fabric_ports();

            virtual sai_status_t set_fabric_port_list();

            virtual sai_status_t create_default_virtual_router();

            virtual sai_status_t create_default_stp_instance();

            virtual sai_status_t create_default_trap_group();

            virtual sai_status_t create_ingress_priority_groups_per_port(
                    _In_ sai_object_id_t port_id);

            virtual sai_status_t create_ingress_priority_groups();

            virtual sai_status_t create_vlan_members();

            virtual sai_status_t create_bridge_ports();

            virtual sai_status_t set_acl_entry_min_prio();

            virtual sai_status_t set_acl_capabilities();

            virtual sai_status_t set_maximum_number_of_childs_per_scheduler_group();

            virtual sai_status_t set_number_of_ecmp_groups();

            virtual sai_status_t set_static_crm_values();

            virtual sai_status_t set_static_acl_resource_list(
                    _In_ sai_switch_attr_t acl_resource,
                    _In_ int max_count);

            sai_status_t filter_available_lanes(
                    _Inout_ std::vector<std::vector<uint32_t>> &lanes_vector);

            sai_status_t create_system_ports(
                    _In_ int32_t voq_switch_id,
                    _In_ uint32_t sys_port_count,
                    _In_ const sai_system_port_config_t *sys_port_cfg_list);

            sai_status_t set_system_port_list();

        public:

            virtual sai_status_t initialize_default_objects(
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);

	    sai_status_t vpp_dp_initialize();
            sai_status_t vpp_create_default_1q_bridge();

	    sai_status_t createVlanMember(
		    _In_ sai_object_id_t object_id,
		    _In_ sai_object_id_t switch_id,
		    _In_ uint32_t attr_count,
		    _In_ const sai_attribute_t *attr_list);
            sai_status_t vpp_create_vlan_member(
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);
            sai_status_t removeVlanMember(
                    _In_ sai_object_id_t objectId);
            sai_status_t vpp_remove_vlan_member(
                    _In_ sai_object_id_t vlan_member_oid);
            sai_status_t vpp_create_bvi_interface(
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);
            sai_status_t vpp_delete_bvi_interface(
                    _In_ sai_object_id_t bvi_obj_id);

            /*FDB Entry and Flush SAI Objects*/
            sai_status_t FdbEntryadd(
                    _In_ const std::string &serializedObjectId,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);
            sai_status_t vpp_fdbentry_add(
                    _In_ const std::string &serializedObjectId,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);
	    sai_status_t FdbEntrydel(
                    _In_ const std::string &serializedObjectId);
            sai_status_t vpp_fdbentry_del(
                    _In_ const std::string &serializedObjectId);

            sai_status_t vpp_fdbentry_flush(
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);

            /* BFD Session */
            sai_status_t bfd_session_add(
                    _In_ const std::string &serializedObjectId,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);
            sai_status_t vpp_bfd_session_add(
                    _In_ const std::string &serializedObjectId,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);
            sai_status_t bfd_session_del(
                    _In_ const std::string &serializedObjectId);
            sai_status_t vpp_bfd_session_del(
                    _In_ const std::string &serializedObjectId);

        private: // BFD related
            struct vpp_bfd_info_t {
                //uint32_t sw_if_index;
                bool multihop;
                sai_ip_address_t local_addr;
                sai_ip_address_t peer_addr;
            
                // Define the < operator for comparison
                bool operator<(const vpp_bfd_info_t& other) const {
            
                    // compare local IP address first
                    int cmp = std::memcmp(&local_addr, &other.local_addr, sizeof(sai_ip_address_t));
                    if (cmp != 0) {
                        return cmp < 0;
                    }
            
                    // compare peer IP address
                    cmp = std::memcmp(&peer_addr, &other.peer_addr, sizeof(sai_ip_address_t));
                    if (cmp != 0) {
                        return cmp < 0;
                    }
            
                    // compare multihop flag
                    return multihop < other.multihop;
                }
            };

            std::map<vpp_bfd_info_t, sai_object_id_t> m_bfd_info_map;
            std::mutex bfdMapMutex;

            void send_bfd_state_change_notification(
                    _In_ sai_object_id_t bfd_oid,
                    _In_ sai_bfd_session_state_t state,
                    _In_ bool force);

            void update_bfd_session_state(
                    _In_ sai_object_id_t bfd_oid,
                    _In_ sai_bfd_session_state_t state);

            sai_status_t asyncBfdStateUpdate(vpp_bfd_state_notif_t *bfd_notif);
 
        protected:

            virtual sai_status_t create_port_dependencies(
                    _In_ sai_object_id_t port_id);

            sai_status_t initialize_voq_switch_objects(
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);

        protected : // refresh

            virtual sai_status_t refresh_ingress_priority_group(
                    _In_ const sai_attr_metadata_t *meta,
                    _In_ sai_object_id_t port_id);

            virtual sai_status_t refresh_qos_queues(
                    _In_ const sai_attr_metadata_t *meta,
                    _In_ sai_object_id_t port_id);

            virtual sai_status_t refresh_scheduler_groups(
                    _In_ const sai_attr_metadata_t *meta,
                    _In_ sai_object_id_t port_id);

            virtual sai_status_t refresh_bridge_port_list(
                    _In_ const sai_attr_metadata_t *meta,
                    _In_ sai_object_id_t bridge_id);

            virtual sai_status_t refresh_vlan_member_list(
                    _In_ const sai_attr_metadata_t *meta,
                   _In_ sai_object_id_t vlan_id);

            virtual sai_status_t refresh_port_list(
                    _In_ const sai_attr_metadata_t *meta);

            virtual sai_status_t refresh_system_port_list(
                    _In_ const sai_attr_metadata_t *meta);

            virtual sai_status_t refresh_macsec_sci_in_ingress_macsec_acl(
                    _In_ sai_object_id_t object_id);

            virtual sai_status_t refresh_queue_pause_status(
                    _In_ sai_object_id_t object_id);

            virtual sai_status_t refresh_macsec_sa_stat(
                    _In_ sai_object_id_t object_id);

            virtual sai_status_t refresh_port_serdes_id(
                    _In_ sai_object_id_t bridge_id);

        public:

            virtual sai_status_t warm_boot_initialize_objects();

        protected:

            virtual sai_status_t refresh_read_only(
                    _In_ const sai_attr_metadata_t *meta,
                    _In_ sai_object_id_t object_id);

        protected:

            virtual sai_status_t warm_update_queues();

            virtual sai_status_t warm_update_scheduler_groups();

            virtual sai_status_t warm_update_ingress_priority_groups();

            virtual sai_status_t warm_update_switch();

            virtual sai_status_t warm_update_cpu_queues();

        protected: // TODO should be pure

            virtual sai_status_t create_cpu_qos_queues(
                    _In_ sai_object_id_t port_id);

            virtual sai_status_t create_qos_queues_per_port(
                    _In_ sai_object_id_t port_id);

            virtual sai_status_t create_qos_queues();

            virtual sai_status_t create_scheduler_group_tree(
                    _In_ const std::vector<sai_object_id_t>& sgs,
                    _In_ sai_object_id_t port_id);

            virtual sai_status_t create_scheduler_groups_per_port(
                    _In_ sai_object_id_t port_id);

            virtual sai_status_t create_scheduler_groups();

            virtual sai_status_t create_port_serdes();

            virtual sai_status_t create_port_serdes_per_port(
                    _In_ sai_object_id_t port_id);

        protected: // will generate new OID

            virtual sai_status_t create(
                    _In_ sai_object_type_t object_type,
                    _Out_ sai_object_id_t *object_id,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);

            virtual sai_status_t remove(
                    _In_ sai_object_type_t object_type,
                    _In_ sai_object_id_t object_id);

            virtual sai_status_t set(
                    _In_ sai_object_type_t objectType,
                    _In_ sai_object_id_t objectId,
                    _In_ const sai_attribute_t* attr);

            virtual sai_status_t get(
                    _In_ sai_object_type_t object_type,
                    _In_ sai_object_id_t object_id,
                    _In_ uint32_t attr_count,
                    _Out_ sai_attribute_t *attr_list);

        public:

            virtual sai_status_t create(
                    _In_ sai_object_type_t object_type,
                    _In_ const std::string &serializedObjectId,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);

            virtual sai_status_t remove(
                    _In_ sai_object_type_t object_type,
                    _In_ const std::string &serializedObjectId);

            virtual sai_status_t set(
                    _In_ sai_object_type_t objectType,
                    _In_ const std::string &serializedObjectId,
                    _In_ const sai_attribute_t* attr);

            virtual sai_status_t get(
                    _In_ sai_object_type_t object_type,
                    _In_ const std::string &serializedObjectId,
                    _In_ uint32_t attr_count,
                    _Out_ sai_attribute_t *attr_list);

            virtual sai_status_t get(
                    _In_ sai_object_type_t objectType,
		    _In_ const std::string &serializedObjectId,
		    _In_ const uint32_t max_attr_count,
		    _Out_ uint32_t *attr_count,
		    _Out_ sai_attribute_t *attr_list);

            virtual sai_status_t bulkCreate(
                    _In_ sai_object_id_t switch_id,
                    _In_ sai_object_type_t object_type,
                    _In_ const std::vector<std::string> &serialized_object_ids,
                    _In_ const uint32_t *attr_count,
                    _In_ const sai_attribute_t **attr_list,
                    _In_ sai_bulk_op_error_mode_t mode,
                    _Out_ sai_status_t *object_statuses);

            virtual sai_status_t bulkRemove(
                    _In_ sai_object_type_t object_type,
                    _In_ const std::vector<std::string> &serialized_object_ids,
                    _In_ sai_bulk_op_error_mode_t mode,
                    _Out_ sai_status_t *object_statuses);

            virtual sai_status_t bulkSet(
                    _In_ sai_object_type_t object_type,
                    _In_ const std::vector<std::string> &serialized_object_ids,
                    _In_ const sai_attribute_t *attr_list,
                    _In_ sai_bulk_op_error_mode_t mode,
                    _Out_ sai_status_t *object_statuses);

            virtual sai_status_t queryAttrEnumValuesCapability(
                              _In_ sai_object_id_t switch_id,
                              _In_ sai_object_type_t object_type,
                              _In_ sai_attr_id_t attr_id,
                             _Inout_ sai_s32_list_t *enum_values_capability);

            std::shared_ptr<SaiDBObject> get_sai_object(
                    _In_ sai_object_type_t object_type,
                    _In_ const std::string &serialized_object_id);
        protected:

            virtual sai_status_t remove_internal(
                    _In_ sai_object_type_t object_type,
                    _In_ const std::string &serializedObjectId);

            virtual sai_status_t create_internal(
                    _In_ sai_object_type_t object_type,
                    _In_ const std::string &serializedObjectId,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);

            virtual sai_status_t set_internal(
                    _In_ sai_object_type_t objectType,
                    _In_ const std::string &serializedObjectId,
                    _In_ const sai_attribute_t* attr);

        private:

            sai_object_type_t objectTypeQuery(
                    _In_ sai_object_id_t objectId);

            sai_object_id_t switchIdQuery(
                    _In_ sai_object_id_t objectId);

        public:

            void processFdbEntriesForAging();

        private: // fdb related

            void updateLocalDB(
                    _In_ const sai_fdb_event_notification_data_t &data,
                    _In_ sai_fdb_event_t fdb_event);

            void processFdbInfo(
                    _In_ const FdbInfo &fi,
                    _In_ sai_fdb_event_t fdb_event);

            void findBridgeVlanForPortVlan(
                    _In_ sai_object_id_t port_id,
                    _In_ sai_vlan_id_t vlan_id,
                    _Inout_ sai_object_id_t &bv_id,
                    _Inout_ sai_object_id_t &bridge_port_id);

            bool getLagFromPort(
                    _In_ sai_object_id_t port_id,
                    _Inout_ sai_object_id_t& lag_id);

            bool isLagOrPortRifBased(
                    _In_ sai_object_id_t lag_or_port_id);

        public:

            void process_packet_for_fdb_event(
                    _In_ sai_object_id_t portId,
                    _In_ const std::string& name,
                    _In_ const uint8_t *buffer,
                    _In_ size_t size);

            void debugSetStats(
                    _In_ sai_object_id_t oid,
                    _In_ const std::map<sai_stat_id_t, uint64_t>& stats);

        protected: // custom port

            sai_status_t createPort(
                    _In_ sai_object_id_t object_id,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);

            sai_status_t removePort(
                    _In_ sai_object_id_t objectId);

            sai_status_t setPort(
                    _In_ sai_object_id_t objectId,
                    _In_ const sai_attribute_t* attr);

            bool isPortReadyToBeRemove(
                    _In_ sai_object_id_t portId);

            std::vector<sai_object_id_t> getPortDependencies(
                    _In_ sai_object_id_t portId);

            sai_status_t check_port_dependencies(
                    _In_ sai_object_id_t port_id,
                    _Out_ std::vector<sai_object_id_t>& dep);

            bool check_port_reference_count(
                    _In_ sai_object_id_t port_id);

            bool get_object_list(
                    _In_ sai_object_id_t object_id,
                    _In_ sai_attr_id_t attr_id,
                    _Out_ std::vector<sai_object_id_t>& objlist);

            bool get_port_queues(
                    _In_ sai_object_id_t port_id,
                    _Out_ std::vector<sai_object_id_t>& queues);

            bool get_port_ipgs(
                    _In_ sai_object_id_t port_id,
                    _Out_ std::vector<sai_object_id_t>& ipgs);

            bool get_port_sg(
                    _In_ sai_object_id_t port_id,
                    _Out_ std::vector<sai_object_id_t>& sg);

            bool check_object_default_state(
                    _In_ sai_object_id_t object_id);

            bool check_object_list_default_state(
                    _Out_ const std::vector<sai_object_id_t>& objlist);

            sai_status_t createVoqSystemNeighborEntry(
                    _In_ const std::string &serializedObjectId,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);

        protected: // custom debug counter

            uint32_t getNewDebugCounterIndex();

            void releaseDebugCounterIndex(
                    _In_ uint32_t index);

            sai_status_t createDebugCounter(
                    _In_ sai_object_id_t object_id,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);

            sai_status_t removeDebugCounter(
                    _In_ sai_object_id_t objectId);

        public:

            static int promisc(
                    _In_ const char *dev);

        protected: // custom hostif

            sai_status_t createHostif(
                    _In_ sai_object_id_t object_id,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);

            sai_status_t removeHostif(
                    _In_ sai_object_id_t objectId);

            sai_status_t vpp_remove_hostif_tap_interface(
                    _In_ sai_object_id_t hostif_id);

            sai_status_t vpp_create_hostif_tap_interface(
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);

            bool hostif_create_tap_veth_forwarding(
                    _In_ const std::string &tapname,
                    _In_ int tapfd,
                    _In_ sai_object_id_t port_id);

            static int vpp_create_tap_device(
                    _In_ const char *dev,
                    _In_ int flags);

            static int vpp_set_dev_mac_address(
                    _In_ const char *dev,
                    _In_ const sai_mac_t& mac);

            static int get_default_gw_mac_address(
                    _Out_ sai_mac_t& mac);

            int vpp_set_dev_mtu(
                    _In_ const char*name,
                    _In_ int mtu);

            int ifup(
                    _In_ const char *dev,
                    _In_ sai_object_id_t port_id,
                    _In_ bool up,
                    _In_ bool explicitNotification);

            std::string vpp_get_veth_name(
                    _In_ const std::string& tapname,
                    _In_ sai_object_id_t port_id);

            void send_port_oper_status_notification(
                    _In_ sai_object_id_t port_id,
                    _In_ sai_port_oper_status_t status,
                    _In_ bool force);

            bool hasIfIndex(
                    _In_ int ifIndex) const;

        public: // TODO move inside warm boot load state

            sai_status_t vpp_recreate_hostif_tap_interfaces();

            void update_port_oper_status(
                    _In_ sai_object_id_t port_id,
                    _In_ sai_port_oper_status_t port_oper_status);

            std::string dump_switch_database_for_warm_restart() const;

            void syncOnLinkMsg(
                    _In_ std::shared_ptr<EventPayloadNetLinkMsg> payload);

            void send_fdb_event_notification(
                    _In_ const sai_fdb_event_notification_data_t& data);

        protected:

            void findObjects(
                    _In_ sai_object_type_t object_type,
                    _In_ const sai_attribute_t &expect,
                    _Out_ std::vector<sai_object_id_t> &objects);

            bool dumpObject(
                    _In_ const sai_object_id_t object_id,
                    _Out_ std::vector<sai_attribute_t> &attrs);

        protected:

            sai_status_t setAclEntryMACsecFlowActive(
                    _In_ sai_object_id_t entry_id,
                    _In_ const sai_attribute_t* attr);

            sai_status_t setMACsecSA(
                    _In_ sai_object_id_t macsec_sa_id,
                    _In_ const sai_attribute_t* attr);

            sai_status_t createMACsecPort(
                    _In_ sai_object_id_t macsec_sa_id,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);

            sai_status_t createMACsecSA(
                    _In_ sai_object_id_t macsec_sa_id,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);

            sai_status_t createMACsecSC(
                    _In_ sai_object_id_t macsec_sa_id,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);

            sai_status_t removeMACsecPort(
                    _In_ sai_object_id_t macsec_port_id);

            sai_status_t removeMACsecSC(
                    _In_ sai_object_id_t macsec_sc_id);

            sai_status_t removeMACsecSA(
                    _In_ sai_object_id_t macsec_sa_id);

            sai_status_t getACLTable(
                    _In_ sai_object_id_t entry_id,
                    _Out_ sai_object_id_t &table_id);

            sai_status_t findPortByMACsecFlow(
                    _In_ sai_object_id_t macsec_flow_id,
                    _Out_ sai_object_id_t &line_port_id);

            std::shared_ptr<HostInterfaceInfo> findHostInterfaceInfoByPort(
                    _In_ sai_object_id_t line_port_id);

            sai_status_t loadMACsecAttrFromMACsecPort(
                    _In_ sai_object_id_t object_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list,
                    _Out_ MACsecAttr &macsec_attr);

            sai_status_t loadMACsecAttrFromMACsecSC(
                    _In_ sai_object_id_t object_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list,
                    _Out_ MACsecAttr &macsec_attr);

            sai_status_t loadMACsecAttrFromMACsecSA(
                    _In_ sai_object_id_t object_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list,
                    _Out_ MACsecAttr &macsec_attr);

            sai_status_t loadMACsecAttr(
                    _In_ sai_object_type_t object_type,
                    _In_ sai_object_id_t object_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list,
                    _Out_ MACsecAttr &macsec_attr);

            sai_status_t loadMACsecAttr(
                    _In_ sai_object_type_t object_type,
                    _In_ sai_object_id_t object_id,
                    _Out_ MACsecAttr &macsec_attr);

            sai_status_t loadMACsecAttrsFromACLEntry(
                    _In_ sai_object_id_t entry_id,
                    _In_ const sai_attribute_t* entry_attr,
                    _In_ sai_object_type_t object_type,
                    _Out_ std::vector<MACsecAttr> &macsec_attrs);

            sai_status_t getMACsecSAPacketNumber(
                    _In_ sai_object_id_t macsec_sa_id,
                    _Out_ sai_attribute_t &attr);

            void retryCreateIngressMaCsecSAs();

            MACsecManager m_macsecManager;

            std::unordered_map<sai_object_id_t, sai_object_id_t> m_macsecFlowPortMap;

            std::unordered_set<MACsecAttr, MACsecAttr::Hash> m_uncreatedIngressMACsecSAs;

        protected:

            constexpr static const int maxDebugCounters = 32;

            std::unordered_set<uint32_t> m_indices;

        protected:

            std::vector<sai_object_id_t> m_port_list;
            std::vector<sai_object_id_t> m_bridge_port_list_port_based;

            std::vector<sai_object_id_t> m_fabric_port_list;

            std::vector<sai_acl_action_type_t> m_ingress_acl_action_list;
            std::vector<sai_acl_action_type_t> m_egress_acl_action_list;

            sai_object_id_t m_cpu_port_id;
            sai_object_id_t m_default_1q_bridge;
            sai_object_id_t m_default_bridge_port_1q_router;
            sai_object_id_t m_default_vlan_id;

            std::vector<sai_object_id_t> m_system_port_list;

        protected:

            constexpr static const int m_maxIPv4RouteEntries = 100000;
            constexpr static const int m_maxIPv6RouteEntries = 10000;

            constexpr static const int m_maxIPv4NextHopEntries = 32000;
            constexpr static const int m_maxIPv6NextHopEntries = 32000;

            constexpr static const int m_maxIPv4NeighborEntries = 4000;
            constexpr static const int m_maxIPv6NeighborEntries = 2000;

            constexpr static const int m_maxNextHopGroupMemberEntries = 16000;
            constexpr static const int m_maxNextHopGroupEntries = 400;

            constexpr static const int m_maxFdbEntries = 800;

            constexpr static const int m_maxSNATEntries = 100;
            constexpr static const int m_maxDNATEntries = 100;
            constexpr static const int m_maxIPMCEntries = 100;
            constexpr static const int m_maxDoubleNATEntries = 50; /* Half of single NAT entry */

            constexpr static const int m_maxAclTables = 3;
            constexpr static const int m_maxAclTableGroups = 200;

        protected:

            virtual sai_status_t queryTunnelPeerModeCapability(
                                      _Inout_ sai_s32_list_t *enum_values_capability);

            virtual sai_status_t queryVlanfloodTypeCapability(
                                      _Inout_ sai_s32_list_t *enum_values_capability);

            virtual sai_status_t queryNextHopGroupTypeCapability(
                                      _Inout_ sai_s32_list_t *enum_values_capability);

        private:
	    std::map<sai_object_id_t, std::shared_ptr<IpVrfInfo>> vrf_objMap;
            bool nbr_env_read = false;
            bool nbr_active = false;
	    std::map<std::string, std::string> m_intf_prefix_map;
	    std::unordered_map<std::string, uint32_t> lpbInstMap;
	    std::unordered_map<std::string, std::string> lpbIpToHostIfMap;
	    std::unordered_map<std::string, std::string> lpbIpToIfMap;
            std::unordered_map<std::string, std::string> lpbHostIfToVppIfMap;
        protected:
            sai_status_t fillNHGrpMember(
                nexthop_grp_member_t *nxt_grp_member, 
                sai_object_id_t next_hop_oid, 
                uint32_t next_hop_weight, 
                uint32_t next_hop_sequence);

            sai_status_t IpRouteNexthopGroupEntry(
                _In_ sai_object_id_t next_hop_grp_oid,
                _Out_ nexthop_grp_config_t **nxthop_group);

            sai_status_t IpRouteNexthopEntry(
                    _In_ sai_object_id_t next_hop_oid,
		    _Out_ nexthop_grp_config_t **nxthop_group_cfg);

	    sai_status_t createNexthop(
		_In_ const std::string& serializedObjectId,
		_In_ sai_object_id_t switch_id,
		_In_ uint32_t attr_count,
		_In_ const sai_attribute_t *attr_list);

            sai_status_t removeNexthop(
                _In_ const std::string &serializedObjectId);
	    
            sai_status_t createNexthopGroupMember(
		_In_ const std::string& serializedObjectId,
		_In_ sai_object_id_t switch_id,
		_In_ uint32_t attr_count,
		_In_ const sai_attribute_t *attr_list);

            sai_status_t removeNexthopGroupMember(
		_In_ const std::string& serializedObjectId);

        protected:
	    sai_status_t createRouterif(
		    _In_ sai_object_id_t object_id,
		    _In_ sai_object_id_t switch_id,
		    _In_ uint32_t attr_count,
		    _In_ const sai_attribute_t *attr_list);

            sai_status_t removeRouterif(
                    _In_ sai_object_id_t objectId);

	    sai_status_t vpp_create_router_interface(
		    _In_ uint32_t attr_count,
		    _In_ const sai_attribute_t *attr_list);

	    sai_status_t vpp_update_router_interface(
		    _In_ sai_object_id_t object_id,
		    _In_ uint32_t attr_count,
		    _In_ const sai_attribute_t *attr_list);

	    sai_status_t vpp_remove_router_interface(
		    _In_ sai_object_id_t objectId);
            sai_status_t vpp_router_interface_remove_vrf(
                    _In_ sai_object_id_t obj_id);

            sai_status_t vpp_add_del_intf_ip_addr (
                    _In_ sai_ip_prefix_t& ip_prefix,
                    _In_ sai_object_id_t nexthop_oid,
		    _In_ bool is_add);
            sai_status_t vpp_add_del_intf_ip_addr_norif (
		    _In_ const std::string& ip_prefix_key,
                    _In_ sai_route_entry_t& route_entry,
                    _In_ bool is_add);
            sai_status_t vpp_interface_ip_address_update (
                    _In_ const char *hw_ifname,
                    _In_ const std::string &serializedObjectId,
                    _In_ bool is_add);
            sai_status_t process_interface_loopback (
                    _In_ const std::string &serializedObjectId,
                    _In_ bool &isLoopback,
                    _In_ bool is_add);
            sai_status_t vpp_add_lpb_intf_ip_addr (
                    _In_ const std::string &serializedObjectId);
            sai_status_t vpp_del_lpb_intf_ip_addr (
                    _In_ const std::string &serializedObjectId);
            sai_status_t vpp_get_router_intf_name (
                    _In_ sai_ip_prefix_t& ip_prefix,
                    _In_ sai_object_id_t rif_id,
                    std::string& nexthop_ifname);

            int getNextLoopbackInstance();

            void markLoopbackInstanceDeleted(
                    _In_ int instance);

            bool vpp_intf_get_prefix_entry(
                    _In_ const std::string& intf_name,
                    _In_ std::string& ip_prefix);
            void vpp_intf_remove_prefix_entry(const std::string& intf_name);

            sai_status_t asyncIntfStateUpdate(
		    _In_ const char *hwif_name,
		    _In_ bool link_up);

            sai_status_t vpp_set_interface_state (
                    _In_ sai_object_id_t object_id,
                    _In_ uint32_t vlan_id,
                    _In_ bool is_up);
            sai_status_t vpp_set_port_mtu (
                    _In_ sai_object_id_t object_id,
                    _In_ uint32_t vlan_id,
                    _In_ uint32_t mtu);
            sai_status_t vpp_set_interface_mtu (
                    _In_ sai_object_id_t object_id,
                    _In_ uint32_t vlan_id,
                    _In_ uint32_t mtu,
                    _In_ int type);

            sai_status_t UpdatePort(
                   _In_ sai_object_id_t object_id,
                   _In_ uint32_t attr_count,
                   _In_ const sai_attribute_t *attr_list);

	    sai_status_t removeVrf(
		    _In_ sai_object_id_t objectId);
            std::shared_ptr<IpVrfInfo> vpp_get_ip_vrf(_In_ sai_object_id_t objectId);

            sai_status_t addRemoveIpNbr(
                    _In_ const std::string &serializedObjectId,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list,
                    _In_ bool is_add);

            sai_status_t addIpNbr(
		    _In_ const std::string &serializedObjectId,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);
            sai_status_t removeIpNbr(_In_ const std::string &serializedObjectId);
            bool is_ip_nbr_active();

            sai_status_t addIpRoute(
                    _In_ const std::string &serializedObjectId,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);
            sai_status_t removeIpRoute(
		    _In_ const std::string &serializedObjectId);

            sai_status_t IpRouteNexthopEntry(
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list,
                    sai_ip_address_t *ip_address,
                    sai_object_id_t *next_rif_oid);
            std::string convertIPToString(
                    _In_ const sai_ip_addr_t &ipAddress);
            std::string convertIPv6ToString(
                    _In_ const sai_ip_addr_t &ipAddress,
                    _In_ int ipFamily);
            std::string extractDestinationIP(
                    _In_ const std::string &serializedObjectId);
            unsigned long ipToInteger(
                    _In_ const std::string &ipAddress);

            sai_status_t IpRouteAddRemove(
                    _In_ const SaiObject* route_obj,
                    _In_ bool is_add);
            sai_status_t updateIpRoute(
                    _In_ const std::string &serializedObjectId,
                    _In_ const sai_attribute_t *attr_list);


            int vpp_add_ip_vrf(_In_ sai_object_id_t objectId, uint32_t vrf_id);
	    int vpp_del_ip_vrf(_In_ sai_object_id_t objectId);

            int vpp_get_vrf_id(const char *linux_ifname, uint32_t *vrf_id);

        private:
            std::map<sai_object_id_t, std::list<sai_object_id_t>> m_acl_tbl_rules_map;
            std::map<sai_object_id_t, std::list<std::string>> m_acl_tbl_hw_ports_map;
	    std::map<sai_object_id_t, uint32_t> m_acl_swindex_map;
            std::map<sai_object_id_t, uint32_t> m_tunterm_acl_swindex_map;
            std::map<sai_object_id_t, std::list<sai_object_id_t>> m_acl_tbl_grp_mbr_map;
            std::map<sai_object_id_t, std::list<sai_object_id_t>> m_acl_tbl_grp_ports_map;
	    std::map<sai_object_id_t, vpp_ace_cntr_info_t> m_ace_cntr_info_map;

        protected:
	    sai_status_t createAclEntry(
		_In_ sai_object_id_t object_id,
		_In_ sai_object_id_t switch_id,
		_In_ uint32_t attr_count,
		_In_ const sai_attribute_t *attr_list);

	    sai_status_t setAclEntry(
		_In_ sai_object_id_t entry_id,
		_In_ const sai_attribute_t* attr);

	    sai_status_t removeAclEntry(
		_In_ const std::string &serializedObjectId);

	    sai_status_t addRemoveAclEntrytoMap(
		_In_ sai_object_id_t entry_id,
		_In_ sai_object_id_t tbl_oid,
		_In_ bool is_add);

	    sai_status_t getAclTableId(
		_In_ sai_object_id_t entry_id, sai_object_id_t *tbl_oid);

	    sai_status_t AclTblConfig(
		_In_ sai_object_id_t tbl_oid);

            sai_status_t AclTblRemove(
		_In_ sai_object_id_t tbl_oid);

	    sai_status_t AclAddRemoveCheck(
		_In_ sai_object_id_t tbl_oid);

            sai_status_t aclTableRemove(
		_In_ const std::string &serializedObjectId);

            /**
            * @brief Retrieves ACEs and list of ordered ACE info for a given table.
            *
            * @param[in] tbl_oid The object ID of the table for which to retrieve ACEs.
            * @param[out] n_total_entries The total number of ACEs.
            * @param[out] aces Pointer to the array of ACEs.
            * @param[out] ordered_aces Ordered ACE info list.
            * @return SAI_STATUS_SUCCESS on success, or an appropriate error code otherwise.
            */
            sai_status_t get_sorted_aces(
                    _In_ sai_object_id_t tbl_oid,
                    _Out_ size_t &n_total_entries,
                    _Out_ acl_tbl_entries_t *&aces,
                    _Out_ std::list<ordered_ace_list_t> &ordered_aces);

            /**
            * @brief Cleans up AclTblConfig variables.
            *
            * @param[in] aces Pointer to the ACEs to be cleaned up.
            * @param[in] ordered_aces Ordered ACE list to be cleaned up.
            * @param[in] acl Pointer to the VPP ACL to be cleaned up.
            * @param[in] tunterm_acl Pointer to the VPP tunnel termination ACL to be cleaned up.
            */
            void cleanup_AclTblConfig(
                    _In_ acl_tbl_entries_t *&aces,
                    _In_ std::list<ordered_ace_list_t> &ordered_aces,
                    _In_ vpp_acl_t *&acl,
                    _In_ vpp_tunterm_acl_t *&tunterm_acl);

            /**
            * @brief Fills ACL and tunnel termination ACL rules based on the provided ACEs.
            * 
            * @param[in] aces Pointer to ACEs.
            * @param[in] ordered_aces Reference to Ordered ACE list.
            * @param[in] acl Pointer to the allocated VPP ACL.
            * @param[in] tunterm_acl Pointer to the allocated VPP tunnel termination ACL.
            * @return SAI_STATUS_SUCCESS on success, or an appropriate error code otherwise.
            */
            sai_status_t fill_acl_rules(
                    _In_ acl_tbl_entries_t *aces,
                    _In_ std::list<ordered_ace_list_t> &ordered_aces,
                    _In_ vpp_acl_t *acl,
                    _In_ vpp_tunterm_acl_t *tunterm_acl);

            /**
            * @brief Creates or replaces the provided ACL in VPP.
            *
            * @param[in] acl Pointer to the VPP ACL to be added or replaced.
            * @param[in] tbl_oid Object ID of the ACL table where the entry will be added or replaced.
            * @param[in] aces Pointer to the ACEs.
            * @param[in] ordered_aces Ordered ACE list.
            * @return SAI_STATUS_SUCCESS on success, or an appropriate error code otherwise.
            */
            sai_status_t acl_add_replace(
                    _In_ vpp_acl_t *&acl,
                    _In_ sai_object_id_t tbl_oid,
                    _In_ acl_tbl_entries_t *aces,
                    _In_ std::list<ordered_ace_list_t> &ordered_aces);


            /**
             * @brief Allocates memory for VPP ACL.
             *
             * @param[in] n_entries Number of entries in the ACL.
             * @param[in] tbl_oid Object ID of the table to which the ACL belongs.
             * @param[out] acl_name Name of the allocated ACL.
             * @param[out] acl Pointer to the allocated ACL.
             * @return SAI_STATUS_SUCCESS on success, or an appropriate error code otherwise.
             */
            sai_status_t allocate_acl(
                    _In_ size_t n_entries,
                    _In_ sai_object_id_t tbl_oid,
                    _Out_ char (&acl_name)[64],
                    _Out_ vpp_acl_t *&acl);

            /**
             * @brief Allocates memory for VPP tunnel termination ACL.
             *
             * @param[in] n_tunterm_entries Number of tunnel termination entries to allocate.
             * @param[in] tbl_oid Object ID of the ACL table.
             * @param[out] acl_name Name of the allocated ACL.
             * @param[out] tunterm_acl Pointer to the allocated tunnel termination ACL.
             * @return SAI_STATUS_SUCCESS on success, or an appropriate error code otherwise.
             */
            sai_status_t allocate_tunterm_acl(
                    _In_ size_t n_tunterm_entries,
                    _In_ sai_object_id_t tbl_oid,
                    _Out_ char (&acl_name)[64],
                    _Out_ vpp_tunterm_acl_t *&tunterm_acl);

            /**
            * @brief Counts the total number of ACL rules and tunnel termination ACL rules, and sets is_tunterm in the ordered ACE list.
            *
            * @param[in] aces Pointer to ACEs.
            * @param[in] ordered_aces Reference to the ordered ACE list.
            * @param[out] n_entries Reference to the number of ACL entries.
            * @param[out] n_tunterm_entries Reference to the number of tunnel termination ACL entries.
            */
            void count_tunterm_acl_rules(
                    _In_ acl_tbl_entries_t *aces,
                    _In_ std::list<ordered_ace_list_t> &ordered_aces,
                    _Out_ size_t &n_entries,
                    _Out_ size_t &n_tunterm_entries);

            /**
            * @brief Deletes the hardware ports map associated with the given table object ID.
            *
            * @param[in] tbl_oid The object ID of the table whose hardware ports map is to be deleted.
            * @return SAI_STATUS_SUCCESS on success, or an appropriate error code otherwise.
            */
            sai_status_t tbl_hw_ports_map_delete(
                    _In_ sai_object_id_t tbl_oid);

            /**
            * @brief Deletes a tunnel termination ACL in VPP.
            *
            * @param[in] tbl_oid The object ID of the ACL table.
            * @param[in] table_delete Boolean flag indicating whether the overall ACL table is being deleted.
            * @return SAI_STATUS_SUCCESS on success, or an appropriate error code otherwise.
            */
            sai_status_t tunterm_acl_delete(
                    _In_ sai_object_id_t tbl_oid,
                    _In_ bool table_delete);
                    
            /**
            * @brief Creates or replaces the provided tunnel termination ACL in VPP.
            *
            * @param[in] acl Pointer to the tunnel termination ACL.
            * @param[in] tbl_oid Object ID of the ACL table where the entry will be added or replaced.
            * @return SAI_STATUS_SUCCESS on success, or an appropriate error code otherwise.
            */
            sai_status_t tunterm_acl_add_replace(
                    _In_ vpp_tunterm_acl_t *acl,
                    _In_ sai_object_id_t tbl_oid);

            /**
            * @brief Sets the redirect action for a tunnel termination ACL rule.
            *
            * @param[in] attr_id The ID of the SAI attribute to be updated.
            * @param[in] value Pointer to the SAI attribute value.
            * @param[in] rule Pointer to the tunnel termination ACL rule to be updated.
            * @return SAI_STATUS_SUCCESS on success, or an appropriate error code otherwise.
            */
            sai_status_t tunterm_set_action_redirect(
                    _In_ sai_acl_entry_attr_t          attr_id,
                    _In_ const sai_attribute_value_t  *value,
                    _In_ tunterm_acl_rule_t      *rule);

            /**
             * @brief Updates an ACE field for a tunnel termination ACL rule.
             *
             * @param[in] attr_id The ID of the SAI attribute to be updated.
             * @param[in] value Pointer to the SAI attribute value.
             * @param[in] rule Pointer to the tunnel termination ACL rule to be updated.
             * @return SAI_STATUS_SUCCESS on success, or an appropriate error code otherwise.
             */
            sai_status_t tunterm_acl_rule_field_update(
                    _In_ sai_acl_entry_attr_t          attr_id,
                    _In_ const sai_attribute_value_t  *value,
                    _In_ tunterm_acl_rule_t      *rule);

            /**
            * @brief Binds or unbinds a tunnel termination ACL table to/from an interface.
            *
            * @param[in] tbl_oid The object ID of the ACL table to be bound or unbound.
            * @param[in] is_add A boolean flag indicating whether to bind (true) or unbind (false) the ACL table.
            * @param[in] hwif_name The name of the hardware interface to which the ACL table is to be bound or from which it is to be unbound.
            * @return SAI_STATUS_SUCCESS on success, or an appropriate error code otherwise.
            */
            sai_status_t tunterm_acl_bindunbind(
                    _In_ sai_object_id_t tbl_oid,
                    _In_ bool is_add,
                    _In_ std::string hwif_name);

	    sai_status_t aclTableCreate(
		_In_ sai_object_id_t object_id,
		_In_ sai_object_id_t switch_id,
		_In_ uint32_t attr_count,
		_In_ const sai_attribute_t *attr_list);

	    sai_status_t aclDefaultAllowConfigure(
		_In_ sai_object_id_t tbl_oid);

	    sai_status_t acl_rule_range_get(
		_In_ const sai_object_list_t   *range_list,
		_Out_ sai_u32_range_t *range_limit_list,
		_Out_ sai_acl_range_type_t *range_type_list,
		_Out_ uint32_t *range_count);

	    sai_status_t acl_range_attr_get (
		_In_ const std::string &serializedObjectId,
		_In_ uint32_t attr_count,
		_In_ const sai_attribute_t *attr_list,
		_Out_ sai_attribute_t *attr_range);

	    sai_status_t removeAclGrp(
		_In_ const std::string &serializedObjectId);

            sai_status_t setAclGrpMbr(
		_In_ sai_object_id_t member_oid,
		_In_ const sai_attribute_t* attr);

	    sai_status_t removeAclGrpMbr(
		_In_ const std::string &serializedObjectId);

	   sai_status_t createAclGrpMbr(
	       _In_ sai_object_id_t object_id,
	       _In_ sai_object_id_t switch_id,
	       _In_ uint32_t attr_count,
	       _In_ const sai_attribute_t *attr_list);

	   sai_status_t addRemoveAclGrpMbr(
	       _In_ sai_object_id_t member_id,
	       _In_ sai_object_id_t tbl_grp_oid,
	       _In_ bool is_add);

	   sai_status_t getAclTableGroupId(
	       _In_ sai_object_id_t member_id, sai_object_id_t *tbl_grp_oid);

	   sai_status_t addRemovePortTblGrp(
	       _In_ sai_object_id_t port_oid,
	       _In_ sai_object_id_t tbl_grp_oid,
	       _In_ bool is_add);

	   sai_status_t aclBindUnbindPort(
	       _In_ sai_object_id_t port_oid,
	       _In_ sai_object_id_t tbl_grp_oid,
	       _In_ bool is_input,
	       _In_ bool is_bind);

	   sai_status_t aclBindUnbindPorts(
	       _In_ sai_object_id_t tbl_grp_oid,
	       _In_ sai_object_id_t tbl_oid,
	       _In_ bool is_bind);

	   sai_status_t getAclEntryStats(
	       _In_ sai_object_id_t ace_cntr_oid,
	       _In_ uint32_t attr_count,
	       _Out_ sai_attribute_t *attr_list);

        public:
           sai_status_t aclGetVppIndices(
	       _In_ sai_object_id_t ace_oid,
	       _Out_ uint32_t *acl_index,
	       _Out_ uint32_t *ace_index);

           bool vpp_get_hwif_name (
	       _In_ sai_object_id_t object_id,
	       _In_ uint32_t vlan_id,
	       _Out_ std::string& ifname);

        protected:
	    void populate_if_mapping();
	    const char *tap_to_hwif_name(const char *name);
            const char *hwif_to_tap_name(const char *name);

            void vppProcessEvents ();
            void startVppEventsThread();

        private:
	    std::map<std::string, std::string> m_hostif_hwif_map;
	    std::map<std::string, std::string> m_hwif_hostif_map;
	    int mapping_init = 0;
            bool m_run_vpp_events_thread = true;
            bool VppEventsThreadStarted = false;
	    std::shared_ptr<std::thread> m_vpp_thread;

        private:
            static int currentMaxInstance;
            std::set<int> availableInstances;
            //1-4095 BD are statically allocated for VLAN by VLAN-ID. Use 4K-16K for dynamic allocation
            static const uint32_t dynamic_bd_id_base =  4*1024;
            static const uint16_t dynamic_bd_id_pool_size =  12*1024;
            BitResourcePool dynamic_bd_id_pool = BitResourcePool(dynamic_bd_id_pool_size, dynamic_bd_id_base);
        public: // TODO private

            std::set<FdbInfo> m_fdb_info_set;

            std::map<std::string, std::shared_ptr<HostInterfaceInfo>> m_hostif_info_map;

            std::shared_ptr<RealObjectIdManager> m_realObjectIdManager;
            
            friend class TunnelManager;

        private:
            SaiObjectDB m_object_db;
            TunnelManager m_tunnel_mgr;
        
    };
}
