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
#include "FdbInfo.h"

#include "sai_serialize.h"

#include "swss/logger.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <nlohmann/json.hpp>
#pragma GCC diagnostic pop

using namespace saivpp;

using json = nlohmann::json;

FdbInfo::FdbInfo()
{
    SWSS_LOG_ENTER();

    m_portId = SAI_NULL_OBJECT_ID;

    m_vlanId = 0; // essential

    m_bridgePortId = SAI_NULL_OBJECT_ID;

    m_timestamp = 0;

    memset(&m_fdbEntry, 0, sizeof(m_fdbEntry));
}

sai_object_id_t FdbInfo::getPortId() const
{
    SWSS_LOG_ENTER();

    return m_portId;
}

sai_vlan_id_t FdbInfo::getVlanId() const
{
    SWSS_LOG_ENTER();

    return m_vlanId;
}

sai_object_id_t FdbInfo::getBridgePortId() const
{
    SWSS_LOG_ENTER();

    return m_bridgePortId;
}

const sai_fdb_entry_t& FdbInfo::getFdbEntry() const
{
    SWSS_LOG_ENTER();

    return m_fdbEntry;
}

uint32_t FdbInfo::getTimestamp() const
{
    SWSS_LOG_ENTER();

    return m_timestamp;
}

// TODO move to meta serialize
// requires to move FdbInfo to common

std::string FdbInfo::serialize() const
{
    SWSS_LOG_ENTER();

    json j;

    j["port_id"] = sai_serialize_object_id(m_portId);
    j["vlan_id"] = sai_serialize_vlan_id(m_vlanId);
    j["bridge_port_id"] = sai_serialize_object_id(m_bridgePortId);
    j["fdb_entry"] = sai_serialize_fdb_entry(m_fdbEntry);
    j["timestamp"] = sai_serialize_number(m_timestamp);

    SWSS_LOG_INFO("item: %s", j.dump().c_str());

    return j.dump();
}

FdbInfo FdbInfo::deserialize(
        _In_ const std::string& data)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("item: %s", data.c_str());

    const json& j = json::parse(data);

    FdbInfo fi;

    sai_deserialize_object_id(j["port_id"], fi.m_portId);
    sai_deserialize_vlan_id(j["vlan_id"], fi.m_vlanId);
    sai_deserialize_object_id(j["bridge_port_id"], fi.m_bridgePortId);
    sai_deserialize_fdb_entry(j["fdb_entry"], fi.m_fdbEntry);
    sai_deserialize_number(j["timestamp"], fi.m_timestamp);

    return fi;
}

bool FdbInfo::operator<(
        _In_ const FdbInfo& other) const
{
    SWSS_LOG_ENTER();

    int res = memcmp(m_fdbEntry.mac_address, other.m_fdbEntry.mac_address, sizeof(sai_mac_t));

    if (res < 0)
        return true;

    if (res > 0)
        return false;

    return m_vlanId < other.m_vlanId;
}

bool FdbInfo::operator()(
        _In_ const FdbInfo& lhs,
        _In_ const FdbInfo& rhs) const
{
    SWSS_LOG_ENTER();

    return lhs < rhs;
}

void FdbInfo::setFdbEntry(
        _In_ const sai_fdb_entry_t& entry)
{
    SWSS_LOG_ENTER();

    m_fdbEntry = entry;
}

void FdbInfo::setVlanId(
        _In_ sai_vlan_id_t vlanId)
{
    SWSS_LOG_ENTER();

    m_vlanId = vlanId;
}

void FdbInfo::setPortId(
        _In_ const sai_object_id_t portId)
{
    SWSS_LOG_ENTER();

    m_portId = portId;
}

void FdbInfo::setBridgePortId(
        _In_ const sai_object_id_t portId)
{
    SWSS_LOG_ENTER();

    m_bridgePortId = portId;
}

void FdbInfo::setTimestamp(
        _In_ uint32_t timestamp)
{
    SWSS_LOG_ENTER();

    m_timestamp = timestamp;
}
