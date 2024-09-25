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
#include "VirtualSwitchSaiInterface.h"

#include "swss/logger.h"

#include "sai_serialize.h"
#include "SaiAttributeList.h"

#include <inttypes.h>

using namespace saivpp;
using namespace saimeta;

sai_status_t VirtualSwitchSaiInterface::preSetPort(
        _In_ sai_object_id_t port_id,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    // NOTE: should be used only when tap interface is enabled ?

    // Special handling for the sampling attribute modification
    if (attr->id == SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE)
    {
        // Get the sample-packet object id
        sai_object_id_t samplepacket_oid = attr->value.oid;

        // Get the interface name from the port id
        std::string if_name;

        sai_object_id_t vpp_switch_id = switchIdQuery(port_id);

        if (vpp_switch_id == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("vpp_switch_id is null");

            return SAI_STATUS_FAILURE;
        }

        auto it = m_switchStateMap.find(vpp_switch_id);

        if (it == m_switchStateMap.end())
        {
            SWSS_LOG_ERROR("No switch state found for the switch id %s",
                    sai_serialize_object_id(vpp_switch_id).c_str());

            return SAI_STATUS_FAILURE;
        }

        auto sw = it->second;

        if (sw == nullptr)
        {
            SWSS_LOG_ERROR("switch state for the switch id %s is null",
                    sai_serialize_object_id(vpp_switch_id).c_str());

            return SAI_STATUS_FAILURE;
        }

        if (sw->getTapNameFromPortId(port_id, if_name) == false)
        {
            SWSS_LOG_ERROR("tap interface name corresponding to the port id %s is not found",
                    sai_serialize_object_id(port_id).c_str());

            return SAI_STATUS_FAILURE;
        }

        std::string cmd;

        if (samplepacket_oid == SAI_NULL_OBJECT_ID)
        {
            // Delete the sampling session
            cmd.assign("tc qdisc delete dev " + if_name + " handle ffff: ingress");

            if (system(cmd.c_str()) == -1)
            {
                SWSS_LOG_ERROR("unable to delete the sampling session for the interface %s", if_name.c_str());

                SWSS_LOG_ERROR("failed to apply the command: %s", cmd.c_str());

                return SAI_STATUS_FAILURE;
            }

            SWSS_LOG_INFO("successfully applied the command: %s", cmd.c_str());

        }
        else
        {
            // Get the sample rate from the sample object
            sai_attribute_t samplepacket_attr;

            samplepacket_attr.id = SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE;

            if (SAI_STATUS_SUCCESS == get(SAI_OBJECT_TYPE_SAMPLEPACKET, samplepacket_oid, 1, &samplepacket_attr))
            {
                int rate = samplepacket_attr.value.u32;

                // Set the default sample group ID
                std::string group("1");

                // Check if sampling is already enabled on the port
                sai_attribute_t port_attr;

                port_attr.id = SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE;

                // When the sampling parameters are updated,
                // a delete and add operation is performed on the sampling session.
                // If the sampling session is already created, it is deleted below.
                if ((get(SAI_OBJECT_TYPE_PORT, port_id, 1, &port_attr) == SAI_STATUS_SUCCESS) &&
                        (port_attr.value.oid != SAI_NULL_OBJECT_ID))
                {
                    // Sampling session is already created
                    SWSS_LOG_INFO("sampling is already enabled on the port: %s .. Deleting it",
                            sai_serialize_object_id(port_id).c_str());

                    // Delete the sampling session
                    cmd.assign("tc qdisc delete dev " + if_name + " handle ffff: ingress");

                    if (system(cmd.c_str()) == -1)
                    {
                        SWSS_LOG_ERROR("unable to delete the sampling session for the interface %s", if_name.c_str());

                        SWSS_LOG_ERROR("failed to apply the command: %s", cmd.c_str());

                        return SAI_STATUS_FAILURE;
                    }

                    SWSS_LOG_INFO("successfully applied the command: %s", cmd.c_str());
                }

                // Create a new sampling session
                cmd.assign("tc qdisc add dev " + if_name + " handle ffff: ingress");

                if (system(cmd.c_str()) == -1)
                {
                    SWSS_LOG_ERROR("unable to create a sampling session for the interface %s", if_name.c_str());

                    SWSS_LOG_ERROR("failed to apply the command: %s", cmd.c_str());

                    return SAI_STATUS_FAILURE;
                }

                SWSS_LOG_INFO("successfully applied the command: %s", cmd.c_str());

                // Set the sampling rate of the port
                cmd.assign("tc filter add dev " + if_name +
                        " parent ffff: matchall action sample rate " + std::to_string(rate) +
                        " group " + group);

                if (system(cmd.c_str()) == -1)
                {
                    SWSS_LOG_ERROR("unable to update the sampling rate of the interface %s", if_name.c_str());

                    SWSS_LOG_ERROR("failed to apply the command: %s", cmd.c_str());

                    return SAI_STATUS_FAILURE;
                }

                SWSS_LOG_INFO("successfully applied the command: %s", cmd.c_str());

            }
            else
            {
                SWSS_LOG_ERROR("failed to update the port %s, unable to read the sample attr", if_name.c_str());

                return SAI_STATUS_FAILURE;
            }
        }

        SWSS_LOG_INFO("successfully modified the sampling config of the port: %s",
                sai_serialize_object_id(port_id).c_str());
    }

    return SAI_STATUS_SUCCESS;
}
