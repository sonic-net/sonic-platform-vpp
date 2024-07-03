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
#include "SwitchStateBase.h"
#include "EventPayloadNotification.h"

#include "swss/logger.h"
#include "swss/select.h"

#include "meta/sai_serialize.h"
#include "meta/NotificationFdbEvent.h"

#include <linux/if_ether.h>
#include <arpa/inet.h>

#include "vppxlate/SaiVppXlate.h"

using namespace saivpp;

void SwitchStateBase::updateLocalDB(
        _In_ const sai_fdb_event_notification_data_t &data,
        _In_ sai_fdb_event_t fdb_event)
{
    SWSS_LOG_ENTER();

    sai_status_t status;

    switch (fdb_event)
    {
        case SAI_FDB_EVENT_LEARNED:

            {
                auto sid = sai_serialize_fdb_entry(data.fdb_entry);

                status = create(SAI_OBJECT_TYPE_FDB_ENTRY, sid, m_switch_id, data.attr_count, data.attr);

                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("failed to create fdb entry: %s",
                            sai_serialize_fdb_entry(data.fdb_entry).c_str());
                }
            }

            break;

        case SAI_FDB_EVENT_AGED:

            {
                auto sid = sai_serialize_fdb_entry(data.fdb_entry);

                status = remove(SAI_OBJECT_TYPE_FDB_ENTRY, sid);

                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("failed to remove fdb entry %s",
                            sai_serialize_fdb_entry(data.fdb_entry).c_str());
                }
            }

            break;

        default:
            SWSS_LOG_ERROR("unsupported fdb event: %d", fdb_event);
            break;
    }
}

void SwitchStateBase::processFdbInfo(
        _In_ const FdbInfo &fi,
        _In_ sai_fdb_event_t fdb_event)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attrs[2];

    attrs[0].id = SAI_FDB_ENTRY_ATTR_TYPE;
    attrs[0].value.s32 = SAI_FDB_ENTRY_TYPE_DYNAMIC;

    attrs[1].id = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;
    attrs[1].value.oid = fi.getBridgePortId();

    sai_fdb_event_notification_data_t data;

    data.event_type = fdb_event;

    data.fdb_entry = fi.getFdbEntry();

    data.attr_count = 2;
    data.attr = attrs;

    // update local DB
    updateLocalDB(data, fdb_event); // TODO we could move to send_fdb_event_notification and support flush

    send_fdb_event_notification(data);
}

void SwitchStateBase::findBridgeVlanForPortVlan(
        _In_ sai_object_id_t port_id,
        _In_ sai_vlan_id_t vlan_id,
        _Inout_ sai_object_id_t &bv_id,
        _Inout_ sai_object_id_t &bridge_port_id)
{
    SWSS_LOG_ENTER();

    bv_id = SAI_NULL_OBJECT_ID;
    bridge_port_id = SAI_NULL_OBJECT_ID;

    sai_object_id_t bridge_id;

    /*
     * The bridge port lookup process is two steps:
     *
     * - use (vlan_id, physical port_id) to match any .1D bridge port created.
     *   If there is match, then quit, found=true
     *
     * - use (physical port_id) to match any .1Q bridge created. if there is a
     *   match, the quite, found=true.
     *
     * If found==true, generate fdb learn event on the .1D or .1Q bridge port.
     * If not found, then do not generate fdb event. It means the packet is not
     * received on the bridge port.
     *
     * XXX this is not whats happening here, we are just looking for any
     * bridge id (as in our case this is shortcut, we will remove all bridge ports
     * when we will use router interface based port/lag and no bridge
     * will be found.
     */

    auto &objectHash = m_objectHash.at(SAI_OBJECT_TYPE_BRIDGE_PORT);

    // iterate via all bridge ports to find match on port id

    sai_object_id_t lag_id = SAI_NULL_OBJECT_ID;

    if (getLagFromPort(port_id,lag_id))
    {
        SWSS_LOG_INFO("got lag %s for port %s",
                sai_serialize_object_id(lag_id).c_str(),
                sai_serialize_object_id(port_id).c_str());
    }

    bool bv_id_set = false;

    for (auto it = objectHash.begin(); it != objectHash.end(); ++it)
    {
        sai_object_id_t bpid;

        sai_deserialize_object_id(it->first, bpid);

        sai_attribute_t attrs[2];

        attrs[0].id = SAI_BRIDGE_PORT_ATTR_PORT_ID;
        attrs[1].id = SAI_BRIDGE_PORT_ATTR_TYPE;

        sai_status_t status = get(SAI_OBJECT_TYPE_BRIDGE_PORT, bpid, (uint32_t)(sizeof(attrs)/sizeof(attrs[0])), attrs);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_WARN("failed to get attr PORT_ID and TYPE for bridge port %s",
                    sai_serialize_object_id(bpid).c_str());
            continue;
        }

        if (lag_id != SAI_NULL_OBJECT_ID)
        {
            // if port is member of lag, we should check if port_id is that LAG

            if (port_id == attrs[0].value.oid)
            {
                // there should be no case that the same port is lag member and has bridge port object on it

                SWSS_LOG_ERROR("port %s is member of lag %s, and also has bridge port created: %s",
                        sai_serialize_object_id(port_id).c_str(),
                        sai_serialize_object_id(lag_id).c_str(),
                        sai_serialize_object_id(attrs[0].value.oid).c_str());
                continue;
            }

            if (lag_id != attrs[0].value.oid)
            {
                // this is not expected port
                continue;
            }
        }
        else if (port_id != attrs[0].value.oid)
        {
            // this is not expected port
            continue;
        }

        bridge_port_id = bpid;

        // get the 1D bridge id if the bridge port type is subport
        auto bp_type = attrs[1].value.s32;

        SWSS_LOG_DEBUG("found bridge port %s of type %d",
                sai_serialize_object_id(bridge_port_id).c_str(),
                bp_type);

        if (bp_type == SAI_BRIDGE_PORT_TYPE_SUB_PORT)
        {
            sai_attribute_t attr;
            attr.id = SAI_BRIDGE_PORT_ATTR_BRIDGE_ID;

            status = get(SAI_OBJECT_TYPE_BRIDGE_PORT, bridge_port_id, 1, &attr);

            if (status != SAI_STATUS_SUCCESS)
            {
                break;
            }

            bridge_id = attr.value.oid;

            SWSS_LOG_DEBUG("found bridge %s for port %s",
                    sai_serialize_object_id(bridge_id).c_str(),
                    sai_serialize_object_id(port_id).c_str());

            attr.id = SAI_BRIDGE_ATTR_TYPE;

            status = get(SAI_OBJECT_TYPE_BRIDGE, bridge_id, 1, &attr);

            if (status != SAI_STATUS_SUCCESS)
            {
                break;
            }

            SWSS_LOG_DEBUG("bridge %s type is %d",
                    sai_serialize_object_id(bridge_id).c_str(),
                    attr.value.s32);
            bv_id = bridge_id;
            bv_id_set = true;
        }
        else
        {
            auto &objectHash2 = m_objectHash.at(SAI_OBJECT_TYPE_VLAN);

            // iterate via all vlans to find match on vlan id

            for (auto it2 = objectHash2.begin(); it2 != objectHash2.end(); ++it2)
            {
                sai_object_id_t vlan_oid;

                sai_deserialize_object_id(it2->first, vlan_oid);

                sai_attribute_t attr;
                attr.id = SAI_VLAN_ATTR_VLAN_ID;

                status = get(SAI_OBJECT_TYPE_VLAN, vlan_oid, 1, &attr);

                if (status != SAI_STATUS_SUCCESS)
                {
                    continue;
                }

                if (vlan_id == attr.value.u16)
                {
                    bv_id = vlan_oid;
                    bv_id_set = true;
                    break;
                }
            }
        }

        break;
    }

    if (!bv_id_set)
    {
        // if port is lag member, then we didn't found bridge_port for that lag (expected for rif lag)
        SWSS_LOG_WARN("failed to find bv_id for vlan %d and port_id %s",
                vlan_id,
                sai_serialize_object_id(port_id).c_str());
    }
}

bool SwitchStateBase::getLagFromPort(
        _In_ sai_object_id_t port_id,
        _Inout_ sai_object_id_t& lag_id)
{
    SWSS_LOG_ENTER();

    lag_id = SAI_NULL_OBJECT_ID;

    auto &objectHash = m_objectHash.at(SAI_OBJECT_TYPE_LAG_MEMBER);

    // iterate via all lag members to find match on port id

    for (auto it = objectHash.begin(); it != objectHash.end(); ++it)
    {
        sai_object_id_t lag_member_id;

        sai_deserialize_object_id(it->first, lag_member_id);

        sai_attribute_t attr;

        attr.id = SAI_LAG_MEMBER_ATTR_PORT_ID;

        sai_status_t status = get(SAI_OBJECT_TYPE_LAG_MEMBER, lag_member_id, 1, &attr);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("failed to get port id from leg member %s",
                    sai_serialize_object_id(lag_member_id).c_str());
            continue;
        }

        if (port_id != attr.value.oid)
        {
            // this is not the port we are looking for
            continue;
        }

        attr.id = SAI_LAG_MEMBER_ATTR_LAG_ID;

        status = get(SAI_OBJECT_TYPE_LAG_MEMBER, lag_member_id, 1, &attr);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("failed to get lag id from lag member %s",
                    sai_serialize_object_id(lag_member_id).c_str());
            continue;
        }

        lag_id = attr.value.oid;

        return true;
    }

    // this port does not belong to any lag

    return false;
}

bool SwitchStateBase::isLagOrPortRifBased(
        _In_ sai_object_id_t lag_or_port_id)
{
    SWSS_LOG_ENTER();

    auto &objectHash = m_objectHash.at(SAI_OBJECT_TYPE_ROUTER_INTERFACE);

    // iterate via all lag members to find match on port id

    for (auto it = objectHash.begin(); it != objectHash.end(); ++it)
    {
        sai_object_id_t rif_id;

        sai_deserialize_object_id(it->first, rif_id);

        sai_attribute_t attr;

        attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;

        sai_status_t status = get(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, 1, &attr);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("failed to get rif type from rif %s",
                    sai_serialize_object_id(rif_id).c_str());
            continue;
        }

        switch (attr.value.s32)
        {
            case SAI_ROUTER_INTERFACE_TYPE_PORT:
            case SAI_ROUTER_INTERFACE_TYPE_SUB_PORT:
                break;

            default:
                continue;
        }

        attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;

        status = get(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, 1, &attr);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("failed to get rif port id from rif %s",
                    sai_serialize_object_id(rif_id).c_str());
            continue;
        }

        if (attr.value.oid == lag_or_port_id)
        {
            return true;
        }
    }

    return false;
}

void SwitchStateBase::process_packet_for_fdb_event(
        _In_ sai_object_id_t portId,
        _In_ const std::string& name,
        _In_ const uint8_t *buffer,
        _In_ size_t size)
{
    SWSS_LOG_ENTER();

    // we would need hostif info here and maybe interface index, then we can
    // find host info from index

    uint32_t frametime = (uint32_t)time(NULL);

    /*
     * We add +2 in case if frame contains 1Q VLAN tag.
     */

    if (size < (sizeof(ethhdr) + 2))
    {
        SWSS_LOG_WARN("ethernet frame is too small: %zu", size);
        return;
    }

    const ethhdr *eh = (const ethhdr*)buffer;

    uint16_t proto = htons(eh->h_proto);

    uint16_t vlan_id = DEFAULT_VLAN_NUMBER;

    bool tagged = (proto == ETH_P_8021Q);

    if (tagged)
    {
        // this is tagged frame, get vlan id from frame

        uint16_t tci = htons(((const uint16_t*)&eh->h_proto)[1]); // tag is after h_proto field

        vlan_id = tci & 0xfff;

        if (vlan_id == 0xfff)
        {
            SWSS_LOG_WARN("invalid vlan id %u in ethernet frame on %s", vlan_id, name.c_str());
            return;
        }

        if (vlan_id == 0)
        {
            // priority packet, frame should be treated as non tagged
            tagged = false;
        }
    }

    if (tagged == false)
    {
        // untagged ethernet frame

        sai_attribute_t attr;

#ifdef SAI_LAG_ATTR_PORT_VLAN_ID

        sai_object_id_t lag_id;

        if (getLagFromPort(portid, lag_id))
        {
            // if port belongs to lag we need to get SAI_LAG_ATTR_PORT_VLAN_ID

            attr.id = SAI_LAG_ATTR_PORT_VLAN_ID

                sai_status_t status = get(SAI_OBJECT_TYPE_LAG, lag_id, 1, &attr);

            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_WARN("failed to get lag vlan id from lag %s",
                        sai_serialize_object_id(lag_id).c_str());
                return;
            }

            vlan_id = attr.value.u16;

            if (isLagOrPortRifBased(lag_id))
            {
                // this lag is router interface based, skip mac learning
                return;
            }
        }
        else
#endif
        {
            attr.id = SAI_PORT_ATTR_PORT_VLAN_ID;

            sai_status_t status = get(SAI_OBJECT_TYPE_PORT, portId, 1, &attr);

            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_WARN("failed to get port vlan id from port %s",
                        sai_serialize_object_id(portId).c_str());
                return;
            }

            // untagged port vlan (default is 1, but may change setting port attr)
            vlan_id = attr.value.u16;
        }
    }

    sai_object_id_t lag_id;
    if (getLagFromPort(portId, lag_id) && isLagOrPortRifBased(lag_id))
    {
        SWSS_LOG_DEBUG("lag %s is rif based, skip mac learning for port %s",
                sai_serialize_object_id(lag_id).c_str(),
                sai_serialize_object_id(portId).c_str());
        return;
    }

    if (isLagOrPortRifBased(portId))
    {
        SWSS_LOG_DEBUG("port %s is rif based, skip mac learning",
                sai_serialize_object_id(portId).c_str());
        return;
    }

    // we have vlan and mac address which is KEY, so just see if that is already defined

    FdbInfo fi;

    fi.setPortId((lag_id != SAI_NULL_OBJECT_ID) ? lag_id : portId);

    fi.setVlanId(vlan_id);

    memcpy(fi.m_fdbEntry.mac_address, eh->h_source, sizeof(sai_mac_t));

    std::set<FdbInfo>::iterator it = m_fdb_info_set.find(fi);

    if (it != m_fdb_info_set.end())
    {
        // this key was found, update timestamp
        // and since iterator is const we need to reinsert

        fi = *it;

        fi.setTimestamp(frametime);

        m_fdb_info_set.insert(fi);

        return;
    }

    // key was not found, get additional information

    fi.setTimestamp(frametime);

    fi.m_fdbEntry.switch_id = m_switch_id;

    findBridgeVlanForPortVlan(portId, vlan_id, fi.m_fdbEntry.bv_id, fi.m_bridgePortId);

    if (fi.getFdbEntry().bv_id == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_WARN("skipping mac learn for %s, since BV_ID was not found for mac",
                sai_serialize_fdb_entry(fi.getFdbEntry()).c_str());

        // bridge was not found, skip mac learning
        return;
    }

    sai_attribute_t attr;

    attr.id = SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE;

    sai_status_t status = get(SAI_OBJECT_TYPE_BRIDGE_PORT, fi.getBridgePortId(), 1, &attr);

    if (status == SAI_STATUS_SUCCESS)
    {
        if (attr.value.s32 == SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW)
        {
            SWSS_LOG_INFO("inserting to fdb_info set: %s, vlan id: %d",
                    sai_serialize_fdb_entry(fi.getFdbEntry()).c_str(),
                    fi.getVlanId());

            m_fdb_info_set.insert(fi);

            processFdbInfo(fi, SAI_FDB_EVENT_LEARNED);
        }
        else if (attr.value.s32 == SAI_BRIDGE_PORT_FDB_LEARNING_MODE_DISABLE)
        {
            // do not learn, actually linux kernel will learn that MAC
        }
        else
        {
            SWSS_LOG_WARN("not supported SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE: %d, for %s",
                    attr.value.s32,
                    sai_serialize_fdb_entry(fi.getFdbEntry()).c_str());
        }
    }
    else
    {
        SWSS_LOG_ERROR("failed to get SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE for %s: %s",
                sai_serialize_object_id(fi.getBridgePortId()).c_str(),
                sai_serialize_status(status).c_str());
    }
}

void SwitchStateBase::send_fdb_event_notification(
        _In_ const sai_fdb_event_notification_data_t& data)
{
    SWSS_LOG_ENTER();

    auto meta = getMeta();

    if (meta)
    {
        meta->meta_sai_on_fdb_event(1, &data);
    }

    sai_attribute_t attr;

    attr.id = SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY;

    sai_status_t status = get(SAI_OBJECT_TYPE_SWITCH, m_switch_id, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("unable to get SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY attribute for %s",
                sai_serialize_object_id(m_switch_id).c_str());

        return;
    }

    auto str = sai_serialize_fdb_event_ntf(1, &data);

    sai_switch_notifications_t sn = { };

    sn.on_fdb_event = (sai_fdb_event_notification_fn)attr.value.ptr;

    SWSS_LOG_INFO("send event SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY %s", str.c_str());

    auto ntf = std::make_shared<sairedis::NotificationFdbEvent>(str);

    auto payload = std::make_shared<EventPayloadNotification>(ntf, sn);

    m_switchConfig->m_eventQueue->enqueue(std::make_shared<Event>(EVENT_TYPE_NOTIFICATION, payload));
}

sai_status_t SwitchStateBase:: createVlanMember(
        _In_ sai_object_id_t object_id,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto sid = sai_serialize_object_id(object_id);

    CHECK_STATUS(create_internal(SAI_OBJECT_TYPE_VLAN_MEMBER, sid, switch_id, attr_count, attr_list));
    return vpp_create_vlan_member(attr_count, attr_list);

}

sai_status_t SwitchStateBase::vpp_create_vlan_member(
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    sai_object_id_t br_port_id;

    SWSS_LOG_ENTER();

    //find sw_if_index for given l2 interface
    auto attr_type = sai_metadata_get_attr_by_id(SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID, attr_count, attr_list);
    if (attr_type == NULL)
    {
        SWSS_LOG_ERROR("attr SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID was not passed");

        return SAI_STATUS_FAILURE;
    }
    br_port_id = attr_type->value.oid;
    sai_object_type_t obj_type = objectTypeQuery(br_port_id);

    if (obj_type != SAI_OBJECT_TYPE_BRIDGE_PORT)
    {
        SWSS_LOG_ERROR("SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID=%s expected to be PORT but is: %s",
                sai_serialize_object_id(br_port_id).c_str(),
                sai_serialize_object_type(obj_type).c_str());

        return SAI_STATUS_FAILURE;
    }

    const char *hwifname = nullptr;
    auto br_port_attrs = m_objectHash.at(SAI_OBJECT_TYPE_BRIDGE_PORT).at(sai_serialize_object_id(br_port_id));
    auto meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_BRIDGE_PORT, SAI_BRIDGE_PORT_ATTR_PORT_ID);
    auto bp_attr = br_port_attrs[meta->attridname];

    if (bp_attr == NULL) {
        meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_BRIDGE_PORT, SAI_BRIDGE_PORT_ATTR_TUNNEL_ID);
        bp_attr = br_port_attrs[meta->attridname];
    }

    if (bp_attr == NULL) {
        SWSS_LOG_NOTICE("BRIDGE_PORT 0x%lx has neither SAI_BRIDGE_PORT_ATTR_PORT_ID nor SAI_BRIDGE_PORT_ATTR_TUNNEL_ID",
                sai_serialize_object_id(br_port_id).c_str());
        return SAI_STATUS_FAILURE;
    }

    auto port_id = bp_attr->getAttr()->value.oid;
    obj_type = objectTypeQuery(port_id);

    if (obj_type != SAI_OBJECT_TYPE_PORT && obj_type != SAI_OBJECT_TYPE_TUNNEL)
    {
        SWSS_LOG_NOTICE("SAI_BRIDGE_PORT_ATTR_PORT_ID=%s expected to be PORT or TUNNEL but is: %s",
                sai_serialize_object_id(port_id).c_str(),
                sai_serialize_object_type(obj_type).c_str());
        return SAI_STATUS_FAILURE;
    }

    auto attr_vlan_member = sai_metadata_get_attr_by_id(SAI_VLAN_MEMBER_ATTR_VLAN_ID, attr_count, attr_list);

    sai_object_id_t vlan_oid;

    if (attr_vlan_member == NULL)
    {
	SWSS_LOG_NOTICE("attr SAI_VLAN_MEMBER_ATTR_VLAN_ID was not passed");
	return SAI_STATUS_FAILURE;
    } else {
	vlan_oid = attr_vlan_member->value.oid;
    }
    auto attr_vlanid_map = m_objectHash.at(SAI_OBJECT_TYPE_VLAN).at(sai_serialize_object_id(vlan_oid));
    auto md_vlan_id = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_VLAN, SAI_VLAN_ATTR_VLAN_ID);
    auto vlan_id =  attr_vlanid_map.at(md_vlan_id->attridname)->getAttr()->value.u16;

    if (vlan_id == 0)
    {
        SWSS_LOG_NOTICE("attr VLAN object id  was not passed");
        return SAI_STATUS_FAILURE;
    }

    uint32_t bridge_id = (uint32_t)vlan_id;
    char hw_vxlan_name[64];

    if (obj_type == SAI_OBJECT_TYPE_TUNNEL) {
        uint32_t hw_vxlan_ind;

        /*
         * We create VxLAN in vpp_create_vlan_member()
         * as it's the first place where we can unambiguously find out
         * which VLAN ID associated with VxLAN tunnel.
         */
        sai_status_t status = vpp_create_vxlan_tunnel(br_port_id, vlan_oid, &hw_vxlan_ind);
        if (status != SAI_STATUS_SUCCESS) {
            SWSS_LOG_ERROR("vpp_create_vxlan_tunnel() = %d", status);
            return status;
        }

        refresh_interfaces_list();

        if (vpp_get_if_name(hw_vxlan_ind, hw_vxlan_name, sizeof(hw_vxlan_name)) != 0) {
            SWSS_LOG_ERROR("Name of interface wasn't found for id %u", hw_vxlan_ind);
            return SAI_STATUS_FAILURE;
        }
        hwifname = hw_vxlan_name;
    } else {
        std::string if_name;
        bool found = getTapNameFromPortId(port_id, if_name);
        if (found == true) {
            hwifname = tap_to_hwif_name(if_name.c_str());
        } else {
            SWSS_LOG_NOTICE("No ports found for bridge port id :%s", sai_serialize_object_id(br_port_id).c_str());
            return SAI_STATUS_FAILURE;
        }
    }

    auto attr_tag_mode = sai_metadata_get_attr_by_id(SAI_VLAN_MEMBER_ATTR_VLAN_TAGGING_MODE, attr_count, attr_list);
    uint32_t tagging_mode = 0;
    const char *hw_ifname;
    char host_subifname[80];  // Enough for HW interface name of [64] and vlan id

    if (attr_tag_mode == NULL)
    {
        SWSS_LOG_ERROR("attr SAI_VLAN_MEMBER_ATTR_VLAN_ID was not passed");
        return SAI_STATUS_FAILURE;
    }

    /*
     * VPP documentation states that VxLAN interface in bridge domain
     * shall have non-zero Split Horizon Group.
     *
     * https://wiki.fd.io/view/VPP/Using_VPP_as_a_VXLAN_Tunnel_Terminator#Split_Horizon_Group
     */
    uint8_t shg = (obj_type == SAI_OBJECT_TYPE_TUNNEL) ? 1 : 0;

    tagging_mode = attr_tag_mode->value.u32;
    if (tagging_mode == SAI_VLAN_TAGGING_MODE_TAGGED)
    {
        /*
         create vpp subinterface and set it as bridge port
        */
        snprintf(host_subifname, sizeof(host_subifname), "%s.%u", hwifname, vlan_id);

        /* The host(tap) subinterface is also created as part of the vpp subinterface creation */
        create_sub_interface(hwifname, vlan_id, vlan_id);

        /* Get new list of physical interfaces from VPP */
        refresh_interfaces_list();

        hw_ifname = host_subifname;

        //Create bridge and set the l2 port
        set_sw_interface_l2_bridge(hw_ifname, bridge_id, true, VPP_API_PORT_TYPE_NORMAL, shg);

        //Set interface state up
        interface_set_state(hw_ifname, true);
    }else if (tagging_mode == SAI_VLAN_TAGGING_MODE_UNTAGGED) {
        hw_ifname = hwifname;

        //Create bridge and set the l2 port
        set_sw_interface_l2_bridge(hw_ifname, bridge_id, true, VPP_API_PORT_TYPE_NORMAL, shg);

        //Set the vlan member to bridge and tags rewrite
        vpp_l2_vtr_op_t vtr_op = L2_VTR_PUSH_1;
        vpp_vlan_type_t push_dot1q = VLAN_DOT1Q;
        uint32_t tag1 = (uint32_t)vlan_id;
        uint32_t tag2 = ~0;
        set_l2_interface_vlan_tag_rewrite(hw_ifname, tag1, tag2, push_dot1q, vtr_op);
    }else {
        SWSS_LOG_ERROR("Tagging Mode %d not implemented", tagging_mode);
        return SAI_STATUS_FAILURE;
    }


    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::removeVlanMember(
        _In_ sai_object_id_t objectId)
{
    SWSS_LOG_ENTER();

    vpp_remove_vlan_member(objectId);

    auto sid = sai_serialize_object_id(objectId);

    CHECK_STATUS(remove_internal(SAI_OBJECT_TYPE_VLAN_MEMBER, sid));

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::vpp_remove_vlan_member(
        _In_ sai_object_id_t vlan_member_oid)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;

    attr.id = SAI_VLAN_MEMBER_ATTR_VLAN_ID;

    sai_status_t status = get(SAI_OBJECT_TYPE_VLAN_MEMBER, vlan_member_oid, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("attr SAI_VLAN_MEMBER_ATTR_VLAN_ID is not present");

        return SAI_STATUS_FAILURE;
    }
    sai_object_id_t vlan_oid = attr.value.oid;

    sai_object_type_t obj_type = objectTypeQuery(vlan_oid);

    if (obj_type != SAI_OBJECT_TYPE_VLAN)
    {
        SWSS_LOG_ERROR("attr SAI_VLAN_MEMBER_ATTR_VLAN_ID is not valid");
        return SAI_STATUS_FAILURE;
    }

    attr.id = SAI_VLAN_ATTR_VLAN_ID;
    status = get(SAI_OBJECT_TYPE_VLAN, vlan_oid, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("attr SAI_VLAN_ATTR_VLAN_ID is not present");

        return SAI_STATUS_FAILURE;
    }
    auto vlan_id = attr.value.u16;
    uint32_t bridge_id = (uint32_t)vlan_id;

    attr.id = SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID;
    status = get(SAI_OBJECT_TYPE_VLAN_MEMBER, vlan_member_oid, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("attr SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID is not present");
        return SAI_STATUS_FAILURE;
    }

    sai_object_id_t br_port_oid = attr.value.oid;

    obj_type = objectTypeQuery(br_port_oid);
    if (obj_type != SAI_OBJECT_TYPE_BRIDGE_PORT)
    {
        SWSS_LOG_ERROR("SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID=%s expected to be BRIDGE PORT but is: %s",
                sai_serialize_object_id(br_port_oid).c_str(),
                sai_serialize_object_type(obj_type).c_str());

        return SAI_STATUS_FAILURE;
    }

    char hw_vxlan_name[64];
    const char *hw_ifname = nullptr;
    auto br_port_attrs = m_objectHash.at(SAI_OBJECT_TYPE_BRIDGE_PORT).at(sai_serialize_object_id(br_port_oid));
    auto meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_BRIDGE_PORT, SAI_BRIDGE_PORT_ATTR_PORT_ID);
    auto bp_attr = br_port_attrs[meta->attridname];
    if(bp_attr == NULL){
        meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_BRIDGE_PORT, SAI_BRIDGE_PORT_ATTR_TUNNEL_ID);
        bp_attr = br_port_attrs[meta->attridname];
    }
    auto port_id = bp_attr->getAttr()->value.oid;
    obj_type = objectTypeQuery(port_id);

    if (obj_type == SAI_OBJECT_TYPE_TUNNEL) {
        auto it = m_tunnel_oid_to_iface_map.find(port_id);
        if (it == m_tunnel_oid_to_iface_map.end()) {
            SWSS_LOG_ERROR("Interface id not found for tunnel oid 0x%lx", port_id);
            return SAI_STATUS_ITEM_NOT_FOUND;
        }

        if (vpp_get_if_name(it->second, hw_vxlan_name, sizeof(hw_vxlan_name)) != 0) {
            SWSS_LOG_ERROR("Name of interface wasn't found for id %u", it->second);
            return SAI_STATUS_FAILURE;
        }
        hw_ifname = hw_vxlan_name;
        set_sw_interface_l2_bridge(hw_ifname, bridge_id, false, VPP_API_PORT_TYPE_NORMAL, 0);

        status = vpp_remove_vxlan_tunnel(it->second);
        if (status != SAI_STATUS_SUCCESS) {
            SWSS_LOG_ERROR("vpp_remove_vxlan_tunnel(%u) = %d", it->second, status);
            return status;
        }

        refresh_interfaces_list();

        m_tunnel_oid_to_iface_map.erase(it);

        return SAI_STATUS_SUCCESS;
    }

    if (obj_type != SAI_OBJECT_TYPE_PORT)
    {
        SWSS_LOG_NOTICE("SAI_BRIDGE_PORT_ATTR_PORT_ID=%s expected to be PORT but is: %s",
                sai_serialize_object_id(port_id).c_str(),
                sai_serialize_object_type(obj_type).c_str());
        return SAI_STATUS_FAILURE;
    }
    std::string if_name;
    bool found = getTapNameFromPortId(port_id, if_name);
    if (found == true)
    {
        hw_ifname = tap_to_hwif_name(if_name.c_str());
    }else {
        SWSS_LOG_NOTICE("No ports found for bridge port id :%s",sai_serialize_object_id(br_port_oid).c_str());
        return SAI_STATUS_FAILURE;
    }


    attr.id = SAI_VLAN_MEMBER_ATTR_VLAN_TAGGING_MODE;
    status = get(SAI_OBJECT_TYPE_VLAN_MEMBER, vlan_member_oid, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("attr SAI_VLAN_MEMBER_ATTR_VLAN_TAGGING_MODE is not present");

        return SAI_STATUS_FAILURE;
    }
    uint32_t tagging_mode = attr.value.s32;
    char host_subifname[32];
    if (tagging_mode == SAI_VLAN_TAGGING_MODE_UNTAGGED) {

        //First disable tag-rewrite.
        vpp_l2_vtr_op_t vtr_op =L2_VTR_DISABLED;
        vpp_vlan_type_t push_dot1q = VLAN_DOT1Q;
        uint32_t tag1 = (uint32_t)vlan_id;
        uint32_t tag2 = ~0;
        set_l2_interface_vlan_tag_rewrite(hw_ifname, tag1, tag2, push_dot1q, vtr_op);

        //Remove interface from bridge, interface type should be changed to others types like l3.
        set_sw_interface_l2_bridge(hw_ifname, bridge_id, false, VPP_API_PORT_TYPE_NORMAL, 0);
    }else if (tagging_mode == SAI_VLAN_TAGGING_MODE_TAGGED) {

        // set interface l2 tag-rewrite GigabitEthernet0/8/0.200 disable
        snprintf(host_subifname, sizeof(host_subifname), "%s.%u", hw_ifname, vlan_id);
        hw_ifname = host_subifname;
        // Remove the l2 port from bridge
        set_sw_interface_l2_bridge(hw_ifname, bridge_id, false, VPP_API_PORT_TYPE_NORMAL, 0);

        // delete subinterface
        delete_sub_interface(hw_ifname, vlan_id);

        // Get new list of physical interfaces from VPP
        refresh_interfaces_list();
    }else {

        SWSS_LOG_ERROR("Tagging mode %d not implemented", tagging_mode);
        return SAI_STATUS_FAILURE;
    }

    //Check if the bridge has zero ports left, if so remove the bridge as well
    uint32_t member_count = 0;
    bridge_domain_get_member_count (bridge_id, &member_count);
    if (member_count == 0) {
        vpp_bridge_domain_add_del(bridge_id, false);
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::vpp_create_bvi_interface(
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto attr_vlan_oid = sai_metadata_get_attr_by_id(SAI_ROUTER_INTERFACE_ATTR_VLAN_ID, attr_count, attr_list);

    if (attr_vlan_oid == NULL)
    {
        SWSS_LOG_ERROR("attr SAI_ROUTER_INTERFACE_ATTR_VLAN_ID was not passed");
        return SAI_STATUS_SUCCESS;
    }

    sai_object_id_t vlan_oid = attr_vlan_oid->value.oid;

    sai_object_type_t obj_type = objectTypeQuery(vlan_oid);

    if (obj_type != SAI_OBJECT_TYPE_VLAN)
    {
        SWSS_LOG_ERROR(" VLAN object type was not passed");
        return SAI_STATUS_SUCCESS;
    }
    auto vlan_attrs = m_objectHash.at(SAI_OBJECT_TYPE_VLAN).at(sai_serialize_object_id(vlan_oid));
    auto md_vlan_id = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_VLAN, SAI_VLAN_ATTR_VLAN_ID);
    auto vlan_id = (uint32_t) vlan_attrs.at(md_vlan_id->attridname)->getAttr()->value.u16;

    if (vlan_id == 0)
    {
	SWSS_LOG_NOTICE("attr VLAN object id  was not passed");
	return SAI_STATUS_FAILURE;
    }

    auto attr_mac_addr = sai_metadata_get_attr_by_id(SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS, attr_count, attr_list);
    if (attr_mac_addr == NULL)
    {
	SWSS_LOG_NOTICE("attr ROUTER INTERFACE MAC Address is not found");
	return SAI_STATUS_FAILURE;
    }

    sai_mac_t mac_addr;
    memcpy(mac_addr, attr_mac_addr->value.mac, sizeof(sai_mac_t));

    //Create BVI interface
    create_bvi_interface(mac_addr,vlan_id);

    // Get new list of physical interfaces from VPP
    refresh_interfaces_list();

    char hw_bviifname[32];
    const char *hw_ifname;
    snprintf(hw_bviifname, sizeof(hw_bviifname), "bvi%u",vlan_id);
    hw_ifname = hw_bviifname;

    //Create bridge and set the l2 port as BVI
    set_sw_interface_l2_bridge(hw_ifname,vlan_id, true, VPP_API_PORT_TYPE_BVI, 0);

    //Set interface state up
    interface_set_state(hw_ifname, true);

    //Set the bvi as access or untagged port of the bridge
    vpp_l2_vtr_op_t vtr_op = L2_VTR_PUSH_1;
    vpp_vlan_type_t push_dot1q = VLAN_DOT1Q;
    uint32_t tag1 = (uint32_t)vlan_id;
    uint32_t tag2 = ~0;
    set_l2_interface_vlan_tag_rewrite(hw_ifname, tag1, tag2, push_dot1q, vtr_op);

    //Set the arp termination for bridge
    uint32_t bd_id = (uint32_t) vlan_id;
    set_bridge_domain_flags(bd_id, VPP_BD_FLAG_ARP_TERM,true);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::vpp_delete_bvi_interface(
        _In_ sai_object_id_t bvi_obj_id)
{
    sai_attribute_t attr;

    SWSS_LOG_ENTER();

    attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    sai_status_t status = get(SAI_OBJECT_TYPE_ROUTER_INTERFACE, bvi_obj_id, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("attr SAI_ROUTER_INTERFACE_ATTR_TYPE is not present");
        return SAI_STATUS_FAILURE;
    }

    if (attr.value.s32 != SAI_ROUTER_INTERFACE_TYPE_VLAN)
    {
        SWSS_LOG_ERROR("attr SAI_ROUTER_INTERFACE_ATTR_TYPE is not VLAN");
        return SAI_STATUS_FAILURE;
    }

    attr.id = SAI_ROUTER_INTERFACE_ATTR_VLAN_ID;
    status = get(SAI_OBJECT_TYPE_ROUTER_INTERFACE, bvi_obj_id, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("attr SAI_ROUTER_INTERFACE_ATTR_VLAN_ID is not present");
        return SAI_STATUS_FAILURE;
    }

    sai_object_id_t vlan_oid = attr.value.oid;
    sai_object_type_t obj_type = objectTypeQuery(vlan_oid);

    if (obj_type != SAI_OBJECT_TYPE_VLAN)
    {
        SWSS_LOG_ERROR("attr SAI_VLAN_MEMBER_ATTR_VLAN_ID is not valid");
        return SAI_STATUS_FAILURE;
    }

    attr.id = SAI_VLAN_ATTR_VLAN_ID;
    status = get(SAI_OBJECT_TYPE_VLAN, vlan_oid, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("attr SAI_VLAN_ATTR_VLAN_ID is not present");
        return SAI_STATUS_FAILURE;
    }
    auto vlan_id = attr.value.u16;
    char hw_bviifname[32];
    const char *hw_ifname;
    snprintf(hw_bviifname, sizeof(hw_bviifname), "bvi%u",vlan_id);
    hw_ifname = hw_bviifname;

    //Disable arp termination for bridge
    uint32_t bd_id = (uint32_t) vlan_id;
    set_bridge_domain_flags(bd_id, VPP_BD_FLAG_ARP_TERM, false);

    //First disable tag-rewrite.
    vpp_l2_vtr_op_t vtr_op = L2_VTR_DISABLED;
    vpp_vlan_type_t push_dot1q = VLAN_DOT1Q;
    uint32_t tag1 = (uint32_t)vlan_id;
    uint32_t tag2 = ~0;
    set_l2_interface_vlan_tag_rewrite(hw_ifname, tag1, tag2, push_dot1q, vtr_op);

    //Remove interface from bridge, interface type should be changed to others types like l3.
    set_sw_interface_l2_bridge(hw_ifname, bd_id, false, VPP_API_PORT_TYPE_BVI, 0);

    //Remove the bvi interface
    delete_bvi_interface(hw_ifname);

    // refresh interfaces from VPP
    refresh_interfaces_list();

    return SAI_STATUS_SUCCESS;
}


sai_status_t SwitchStateBase::vpp_create_vxlan_tunnel(
        _In_  sai_object_id_t br_port_id,
        _In_  sai_object_id_t vlan_oid,
        _Out_ uint32_t *if_ind)
{
    const sai_attribute_t     *sip_attr;
    const sai_attribute_t     *dip_attr;
    const sai_attribute_t     *vni_attr;
    const sai_attribute_t     *decap_attr;
    sai_uint16_t               vlan;
    sai_object_id_t            tunnel_id;
    const sai_attr_metadata_t *meta;
    sai_object_type_t          obj_type = objectTypeQuery(br_port_id);

    // Sanity checks
    {
        SWSS_LOG_NOTICE("[E](br_port_id=%s, vlan_oid=%s)", sai_serialize_object_id(br_port_id).c_str(),
                sai_serialize_object_id(vlan_oid).c_str());

        if (obj_type != SAI_OBJECT_TYPE_BRIDGE_PORT) {
            SWSS_LOG_ERROR("SAI_OBJECT_TYPE_BRIDGE_PORT expected, but %s is %s",
                    sai_serialize_object_id(br_port_id).c_str(), sai_serialize_object_type(obj_type).c_str());

            return SAI_STATUS_FAILURE;
        }

        auto br_port_attrs = m_objectHash.at(SAI_OBJECT_TYPE_BRIDGE_PORT).at(sai_serialize_object_id(br_port_id));
        meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_BRIDGE_PORT, SAI_BRIDGE_PORT_ATTR_TUNNEL_ID);

        auto bp_attr = br_port_attrs[meta->attridname];
        if (bp_attr == NULL) {
            SWSS_LOG_ERROR("BRIDGE_PORT 0x%lx has no SAI_BRIDGE_PORT_ATTR_TUNNEL_ID",
                    sai_serialize_object_id(br_port_id).c_str());
            return SAI_STATUS_FAILURE;
        }

        tunnel_id = bp_attr->getAttr()->value.oid;
        obj_type = objectTypeQuery(tunnel_id);

        if (obj_type != SAI_OBJECT_TYPE_TUNNEL) {
            SWSS_LOG_ERROR("SAI_BRIDGE_PORT_ATTR_PORT_ID=%s expected to be TUNNEL but is %s",
                    sai_serialize_object_id(tunnel_id).c_str(), sai_serialize_object_type(obj_type).c_str());
            return SAI_STATUS_FAILURE;
        }
    }

    auto tunnel_attrs = m_objectHash.at(SAI_OBJECT_TYPE_TUNNEL).at(sai_serialize_object_id(tunnel_id));

    // ATTR_PEER_MODE
    {
        meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_TUNNEL, SAI_TUNNEL_ATTR_PEER_MODE);
        auto attr = tunnel_attrs[meta->attridname];
        if (attr == NULL) {
            SWSS_LOG_ERROR("SAI_OBJECT_TYPE_TUNNEL 0x%lx has no SAI_TUNNEL_ATTR_PEER_MODE",
                    sai_serialize_object_id(tunnel_id).c_str());
            return SAI_STATUS_FAILURE;
        }

        if (attr->getAttr()->value.s32 != SAI_TUNNEL_PEER_MODE_P2P) {
            SWSS_LOG_ERROR("Only SAI_TUNNEL_PEER_MODE_P2P supported", sai_serialize_object_id(tunnel_id).c_str());
            return SAI_STATUS_FAILURE;
        }
    }

    // SIP
    meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_TUNNEL, SAI_TUNNEL_ATTR_ENCAP_SRC_IP);
    sip_attr = tunnel_attrs[meta->attridname]->getAttr();
    if (sip_attr == NULL) {
        SWSS_LOG_ERROR("SAI_OBJECT_TYPE_TUNNEL 0x%lx has no SAI_TUNNEL_ATTR_ENCAP_SRC_IP",
                sai_serialize_object_id(tunnel_id).c_str());
        return SAI_STATUS_FAILURE;
    }

    // DIP
    meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_TUNNEL, SAI_TUNNEL_ATTR_ENCAP_DST_IP);
    dip_attr = tunnel_attrs[meta->attridname]->getAttr();
    if (dip_attr == NULL) {
        SWSS_LOG_ERROR("SAI_OBJECT_TYPE_TUNNEL 0x%lx has no SAI_TUNNEL_ATTR_ENCAP_DST_IP",
                sai_serialize_object_id(tunnel_id).c_str());
        return SAI_STATUS_FAILURE;
    }

    // DECAP_MAPPERS
    meta = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_TUNNEL, SAI_TUNNEL_ATTR_DECAP_MAPPERS);
    decap_attr = tunnel_attrs[meta->attridname]->getAttr();

    // VLAN
    auto attr_vlanid_map = m_objectHash.at(SAI_OBJECT_TYPE_VLAN).at(sai_serialize_object_id(vlan_oid));
    auto md_vlan_id = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_VLAN, SAI_VLAN_ATTR_VLAN_ID);
    vlan = attr_vlanid_map.at(md_vlan_id->attridname)->getAttr()->value.u16;

    // VNI lookup
    if (vpp_find_vni(vlan, decap_attr, &vni_attr) != SAI_STATUS_SUCCESS ){
        SWSS_LOG_DEBUG("No VNI found for VLAN %hd", vlan);
        return SAI_STATUS_FAILURE;
    }

    sai_status_t status = vpp_create_vxlan_tunnel(sip_attr, dip_attr, vni_attr, if_ind);
    if (status == SAI_STATUS_SUCCESS)
        m_tunnel_oid_to_iface_map[tunnel_id] = *if_ind;

    return status;
}


/**
  Lookup for VNI in TUNNEL_MAP_ENTRY's

  Example of  TUNNEL_MAP_ENTRY:

    vpp_create_tunnel_map_entry(
      SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY
        #0 SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE=VNI_TO_VLAN_ID
        #1 SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP=0x2900000000
        #2 SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE=100
        #3 SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY=1000
    ) = 0x3b00000000
*/

sai_status_t SwitchStateBase::vpp_find_vni(
        _In_  sai_uint16_t vlan,
        _In_  const sai_attribute_t *decap_attr,
        _Out_ const sai_attribute_t **vni_attr)
{
    for (const auto &obj_it : m_objectHash.at(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY)) {
        // Check TUNNEL_MAP_TYPE
        auto m = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY,
                SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE);
        auto a = obj_it.second.at(m->attridname);
        if (a->getAttr()->value.s32 != SAI_TUNNEL_MAP_TYPE_VNI_TO_VLAN_ID
                && a->getAttr()->value.s32 != SAI_TUNNEL_MAP_TYPE_VLAN_ID_TO_VNI)
            continue;

        // Compare VLAN
        m = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE);
        a = obj_it.second.at(m->attridname);
        if (a->getAttr()->value.u16 != vlan)
            continue;

        // Compare TUNNEL_MAP
        m = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP);
        a = obj_it.second.at(m->attridname);

        bool found = false;
        for (uint32_t i = 0; i < decap_attr->value.objlist.count; ++i) {
            if (a->getAttr()->value.oid == decap_attr->value.objlist.list[i]) {
                found = true;
                break;
            }
        }
        if (!found)
            continue;

        m = sai_metadata_get_attr_metadata(SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, SAI_TUNNEL_MAP_ENTRY_ATTR_VNI_ID_KEY);
        a = obj_it.second.at(m->attridname);
        if (a->getAttr() != NULL) {
            *vni_attr = a->getAttr();
            return SAI_STATUS_SUCCESS;
        }
    }

    SWSS_LOG_DEBUG("No VNI found for VLAN %hd", vlan);
    return SAI_STATUS_FAILURE;
}

typedef struct _vpp_vxlan_t {
    vpp_ip_addr_t sip;
    vpp_ip_addr_t dip;
    uint32_t      vni;
} vpp_vxlan_t;

std::map<uint32_t,vpp_vxlan_t> vpp_vxlan_map;

sai_status_t SwitchStateBase::vpp_create_vxlan_tunnel(
        _In_ const sai_attribute_t *sip_attr,
        _In_ const sai_attribute_t *dip_attr,
        _In_ const sai_attribute_t *vni_attr,
        _Out_ uint32_t *if_ind)
{
    vpp_ip_addr_t v_sip;
    vpp_ip_addr_t v_dip;

    if (sip_attr == NULL || dip_attr == NULL || vni_attr == NULL) {
        SWSS_LOG_ERROR("input arg is NULL");
        return SAI_STATUS_FAILURE;
    }

    const sai_ip_address_t &s_sip = sip_attr->value.ipaddr;
    if (s_sip.addr_family == SAI_IP_ADDR_FAMILY_IPV4) {
        v_sip.sa_family = AF_INET;
        v_sip.addr.ip4.sin_addr.s_addr = s_sip.addr.ip4;
    } else {
        v_sip.sa_family = AF_INET6;
        memcpy(v_sip.addr.ip6.sin6_addr.s6_addr, s_sip.addr.ip6, sizeof(v_sip.addr.ip6.sin6_addr.s6_addr));
    }

    const sai_ip_address_t &s_dip = dip_attr->value.ipaddr;
    if (s_dip.addr_family == SAI_IP_ADDR_FAMILY_IPV4) {
        v_dip.sa_family = AF_INET;
        v_dip.addr.ip4.sin_addr.s_addr = s_dip.addr.ip4;
    } else {
        v_dip.sa_family = AF_INET6;
        memcpy(v_dip.addr.ip6.sin6_addr.s6_addr, s_dip.addr.ip6, sizeof(v_dip.addr.ip6.sin6_addr.s6_addr));
    }

    int ret = vpp_vxlan_tunnel_add_del(&v_sip, &v_dip, vni_attr->value.u32, true, if_ind);
    if (ret)
        return SAI_STATUS_FAILURE;

    vpp_vxlan_map[*if_ind].sip = v_sip;
    vpp_vxlan_map[*if_ind].dip = v_dip;
    vpp_vxlan_map[*if_ind].vni = vni_attr->value.u32;

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::vpp_remove_vxlan_tunnel( _In_ uint32_t if_ind)
{
    auto it = vpp_vxlan_map.find(if_ind);
    if (it == vpp_vxlan_map.end() ) {
        SWSS_LOG_NOTICE("if_ind %u not found in vpp_vxlan_map", if_ind);
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    int ret = vpp_vxlan_tunnel_add_del(&it->second.sip, &it->second.dip, it->second.vni, false, &if_ind);
    if (ret)
        return SAI_STATUS_FAILURE;

    vpp_vxlan_map.erase(it);

    return SAI_STATUS_SUCCESS;
}
