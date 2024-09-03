/*
 * Copyright 2024 Microsoft, Inc.
 * Modifications copyright (c) 2024 Cisco and/or its affiliates.
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
#include "SwitchStateBase.h"
#include "EventPayloadNotification.h"
#include "swss/logger.h"
#include "swss/select.h"
#include "meta/sai_serialize.h"
#include <arpa/inet.h>
#include "vppxlate/SaiVppXlate.h"
#include "meta/NotificationBfdSessionStateChange.h"

using namespace saivpp;

/* Utility function to convert SAI IP address to VPP format */
static void sai_to_vpp_ip_addr(
    _Out_ vpp_ip_addr_t* vpp_ip_addr,
    _In_  sai_ip_address_t* ip_addr)
{
    if (NULL == vpp_ip_addr || NULL == ip_addr)
    {
        SWSS_LOG_ERROR(" Invalid argumets passed for memcpy NULL ptr!");
        return;
    }

    if( ip_addr->addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        vpp_ip_addr->sa_family = AF_INET;
        struct sockaddr_in* sin = &vpp_ip_addr->addr.ip4;
        memcpy(&sin->sin_addr.s_addr, &ip_addr->addr.ip4, sizeof(sin->sin_addr.s_addr));
    }
    else if (ip_addr->addr_family == SAI_IP_ADDR_FAMILY_IPV6)
    {
        vpp_ip_addr->sa_family = AF_INET6;
        struct sockaddr_in6* sin6 = &vpp_ip_addr->addr.ip6;
        memcpy(&sin6->sin6_addr.s6_addr, &ip_addr->addr.ip6, sizeof(sin6->sin6_addr.s6_addr));
    }
}

sai_status_t SwitchStateBase::bfd_session_add(
    _In_ const std::string &serializedObjectId,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t *attr_list)
{

    SWSS_LOG_ENTER();

    CHECK_STATUS(create_internal(SAI_OBJECT_TYPE_BFD_SESSION, serializedObjectId, switch_id, attr_count, attr_list));
    vpp_bfd_session_add(serializedObjectId, switch_id, attr_count, attr_list);

    return SAI_STATUS_SUCCESS;

}

sai_status_t SwitchStateBase::vpp_bfd_session_add(
    _In_ const std::string &serializedObjectId,
    _In_ sai_object_id_t switch_id,
    _In_ uint32_t attr_count,
    _In_ const sai_attribute_t *attr_list)
{
    int ret = 0;
    SWSS_LOG_ENTER();

    sai_object_id_t bfd_oid;
    sai_deserialize_object_id(serializedObjectId, bfd_oid);

    sai_attribute_t attr;
    /* Attribute#1 */
    attr.id = SAI_BFD_SESSION_ATTR_MIN_RX;
    CHECK_STATUS(get(SAI_OBJECT_TYPE_BFD_SESSION, bfd_oid, 1, &attr));
    uint32_t required_min_rx = attr.value.u32;

    /* Attribute#2 */
    attr.id = SAI_BFD_SESSION_ATTR_MIN_TX;
    CHECK_STATUS(get(SAI_OBJECT_TYPE_BFD_SESSION, bfd_oid, 1, &attr));
    uint32_t required_min_tx = attr.value.u32;

    /* Attribute#3 */
    attr.id = SAI_BFD_SESSION_ATTR_MULTIPLIER;
    CHECK_STATUS(get(SAI_OBJECT_TYPE_BFD_SESSION, bfd_oid, 1, &attr));
    uint8_t detect_mult = attr.value.u8;

    /* Attribute#4 */
    attr.id = SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS;
    CHECK_STATUS(get(SAI_OBJECT_TYPE_BFD_SESSION, bfd_oid, 1, &attr));
    sai_ip_address_t local_addr = attr.value.ipaddr;
    vpp_ip_addr_t vpp_local_addr;
    /*local addr */
    sai_to_vpp_ip_addr(&vpp_local_addr, &local_addr);

    /* Attribute#5 */
    attr.id = SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS;
    CHECK_STATUS(get(SAI_OBJECT_TYPE_BFD_SESSION, bfd_oid, 1, &attr));
    sai_ip_address_t peer_addr = attr.value.ipaddr;
    vpp_ip_addr_t vpp_peer_addr;
    /* Peer Addr*/
    sai_to_vpp_ip_addr(&vpp_peer_addr, &peer_addr);

    /* Attribute#6 */
    attr.id = SAI_BFD_SESSION_ATTR_MULTIHOP;
    CHECK_STATUS(get(SAI_OBJECT_TYPE_BFD_SESSION, bfd_oid, 1, &attr));
    bool multihop = attr.value.booldata;
    
    const char *hwif_name = NULL;
    if (!multihop) {
        /* Attribute#7 */
        attr.id = SAI_BFD_SESSION_ATTR_PORT;
        CHECK_STATUS(get(SAI_OBJECT_TYPE_BFD_SESSION, bfd_oid, 1, &attr));
        sai_object_id_t port_id = attr.value.oid;
        sai_object_type_t obj_type = objectTypeQuery(port_id);
        if (obj_type != SAI_OBJECT_TYPE_PORT)
        {
            SWSS_LOG_ERROR("SAI_BRIDGE_PORT_ATTR_PORT_ID=%s expected to be PORT but is: %s",
                    sai_serialize_object_id(port_id).c_str(),
                    sai_serialize_object_type(obj_type).c_str());
            return SAI_STATUS_FAILURE;
        }
        std::string ifname = "";
        if (vpp_get_hwif_name(port_id, 0, ifname) == true)
        {
            hwif_name = ifname.c_str();
        }
        else
        {
            SWSS_LOG_ERROR("BFD session create request FAILED due to invalid hwif name");
    
            return SAI_STATUS_FAILURE;
        }
    }

    if (multihop || hwif_name) {
        SWSS_LOG_NOTICE("BFD session create request sent to VPP, hwif: %s, multihop: %d, oid: %lld",  hwif_name, multihop, bfd_oid);
        /*vpp call to add bfd session*/
        ret = bfd_udp_add(multihop, hwif_name, &vpp_local_addr, &vpp_peer_addr, detect_mult, required_min_tx, required_min_rx);
        if (ret >= 0)
        {
            // store bfd attributes to sai oid mapping
            vpp_bfd_info_t bfd_info;
            bfd_info.multihop = multihop;
            memcpy(&bfd_info.local_addr, &local_addr, sizeof(local_addr));
            memcpy(&bfd_info.peer_addr, &peer_addr, sizeof(peer_addr));

            m_bfd_info_map[bfd_info] = bfd_oid;
        }
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::bfd_session_del(
    _In_ const std::string &serializedObjectId)
{
    SWSS_LOG_ENTER();

    vpp_bfd_session_del(serializedObjectId);
    CHECK_STATUS(remove_internal(SAI_OBJECT_TYPE_BFD_SESSION, serializedObjectId));

    return SAI_STATUS_SUCCESS;

}
sai_status_t SwitchStateBase::vpp_bfd_session_del(
    _In_ const std::string &serializedObjectId)
{
    int ret = 0;
    SWSS_LOG_ENTER();

    sai_object_id_t bfd_oid;
    sai_deserialize_object_id(serializedObjectId, bfd_oid);
    sai_attribute_t attr;

    /* Attribute#1 */
    attr.id = SAI_BFD_SESSION_ATTR_SRC_IP_ADDRESS;
    CHECK_STATUS(get(SAI_OBJECT_TYPE_BFD_SESSION, bfd_oid, 1, &attr));
    sai_ip_address_t local_addr = attr.value.ipaddr;
    vpp_ip_addr_t vpp_local_addr;
    /*local addr */
    sai_to_vpp_ip_addr(&vpp_local_addr, &local_addr);

    /* Attribute#2 */
    attr.id = SAI_BFD_SESSION_ATTR_DST_IP_ADDRESS;
    CHECK_STATUS(get(SAI_OBJECT_TYPE_BFD_SESSION, bfd_oid, 1, &attr));
    sai_ip_address_t peer_addr = attr.value.ipaddr;
    vpp_ip_addr_t vpp_peer_addr;
    /* Peer Addr*/
    sai_to_vpp_ip_addr(&vpp_peer_addr, &peer_addr);

    /* Attribute#3 */
    attr.id = SAI_BFD_SESSION_ATTR_MULTIHOP;
    CHECK_STATUS(get(SAI_OBJECT_TYPE_BFD_SESSION, bfd_oid, 1, &attr));
    bool multihop = attr.value.booldata;
    
    const char *hwif_name = NULL;
    if (!multihop) {
        /* Attribute#4 */
        attr.id = SAI_BFD_SESSION_ATTR_PORT;
        CHECK_STATUS(get(SAI_OBJECT_TYPE_BFD_SESSION, bfd_oid, 1, &attr));
        sai_object_id_t port_id = attr.value.oid;
        sai_object_type_t obj_type = objectTypeQuery(port_id);
        if (obj_type != SAI_OBJECT_TYPE_PORT)
        {
            SWSS_LOG_ERROR("SAI_BRIDGE_PORT_ATTR_PORT_ID=%s expected to be PORT but is: %s",
                    sai_serialize_object_id(port_id).c_str(),
                    sai_serialize_object_type(obj_type).c_str());
            return SAI_STATUS_FAILURE;
        }

        std::string ifname = "";
        if (vpp_get_hwif_name(port_id, 0, ifname) == true)
        {
            hwif_name = ifname.c_str();
        }
        else
        {
            SWSS_LOG_ERROR("BFD session delete request FAILED due to invalid hwif name");

            return SAI_STATUS_FAILURE;
        }
    }

    if (multihop || hwif_name) {
        SWSS_LOG_NOTICE("BFD session delete request sent to VPP, hwif: %s, multihop: %d, oid: %lld",  hwif_name, multihop, bfd_oid);
        /*vpp call to add bfd session*/
        ret = bfd_udp_del(multihop, hwif_name, &vpp_local_addr, &vpp_peer_addr);
        if (ret >= 0)
        {
            // remove bfd attributes to sai oid mapping
            vpp_bfd_info_t bfd_info;
            bfd_info.multihop = multihop;
            memcpy(&bfd_info.local_addr, &local_addr, sizeof(local_addr));
            memcpy(&bfd_info.peer_addr, &peer_addr, sizeof(peer_addr));

            auto it = m_bfd_info_map.find(bfd_info);

            // Check if the key exists and erase by iterator
            if (it != m_bfd_info_map.end()) {
                m_bfd_info_map.erase(it);
            }
        }
    }

    return SAI_STATUS_SUCCESS;
}

void SwitchStateBase::update_bfd_session_state(
        _In_ sai_object_id_t bfd_oid,
        _In_ sai_bfd_session_state_t state)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    attr.id = SAI_BFD_SESSION_ATTR_STATE;
    attr.value.s32 = state;

    sai_status_t status = set(SAI_OBJECT_TYPE_BFD_SESSION, bfd_oid, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("failed to update bfd state %s: %d",
                sai_serialize_object_id(bfd_oid).c_str(),
                state);
    }
}

void SwitchStateBase::send_bfd_state_change_notification(
        _In_ sai_object_id_t bfd_oid,
        _In_ sai_bfd_session_state_t state,
        _In_ bool force)
{
    SWSS_LOG_ENTER();

   sai_bfd_session_state_notification_t data;

    data.bfd_session_id = bfd_oid;
    data.session_state = state;

    auto meta = getMeta();

    if (meta)
    {
        meta->meta_sai_on_bfd_session_state_change(1, &data);
    }

    auto objectType = objectTypeQuery(bfd_oid);

    if (objectType != SAI_OBJECT_TYPE_BFD_SESSION)
    {
        SWSS_LOG_ERROR("object type %s not supported for BFD OID %s",
                sai_serialize_object_type(objectType).c_str(),
                sai_serialize_object_id(bfd_oid).c_str());
        return;
    }

    sai_attribute_t attr;

    attr.id = SAI_BFD_SESSION_ATTR_STATE;

    if (get(objectType, bfd_oid, 1, &attr) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("failed to get bfd session attribute SAI_BFD_SESSION_ATTR_STATE");
    }
    else
    {
        if (force)
        {
            SWSS_LOG_NOTICE("explicitly send SAI_SWITCH_ATTR_BFD_SESSION_STATE_CHANGE_NOTIFY for bfd session %s: %d",
                    sai_serialize_object_id(data.bfd_session_id).c_str(),
                    data.session_state);
                    //sai_serialize_bfd_session_state(data.state).c_str());

        }
        else if ((sai_bfd_session_state_t)attr.value.s32 == data.session_state)
        {
            SWSS_LOG_INFO("bfd session state didn't change, will not send notification");
            return;
        }
    }

    attr.id = SAI_SWITCH_ATTR_BFD_SESSION_STATE_CHANGE_NOTIFY;

    if (get(SAI_OBJECT_TYPE_SWITCH, m_switch_id, 1, &attr) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("failed to get SAI_SWITCH_ATTR_BFD_SESSION_STATE_CHANGE_NOTIFY for switch %s",
                sai_serialize_object_id(m_switch_id).c_str());
        return;
    }

    if (attr.value.ptr == NULL)
    {
        SWSS_LOG_INFO("SAI_SWITCH_ATTR_BFD_SESSION_STATE_CHANGE_NOTIFY callback is NULL");
        return;
    }

    sai_switch_notifications_t sn = { };

    sn.on_bfd_session_state_change = (sai_bfd_session_state_change_notification_fn)attr.value.ptr;

    attr.id = SAI_BFD_SESSION_ATTR_STATE;

    update_bfd_session_state(bfd_oid, data.session_state);

    SWSS_LOG_NOTICE("send event SAI_SWITCH_ATTR_BFD_SESSION_STATE_CHANGE_NOTIFY for bfd session %s: %d",
            sai_serialize_object_id(bfd_oid).c_str(),
            state);
            //sai_serialize_bfd_session_state(data.state).c_str());

    auto str = sai_serialize_bfd_session_state_ntf(1, &data);

    auto ntf = std::make_shared<sairedis::NotificationBfdSessionStateChange>(str);

    auto payload = std::make_shared<EventPayloadNotification>(ntf, sn);

    m_switchConfig->m_eventQueue->enqueue(std::make_shared<Event>(EVENT_TYPE_NOTIFICATION, payload));
}
