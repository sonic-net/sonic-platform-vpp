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
/*
 * This file is copied from src/sonic-sairedis/vslib/RealObjectIdManager.cpp by replacing
 * SAI_VS with SAI_VPP and changing the namespace from saivs to saivpp.
 */
#include "RealObjectIdManager.h"

#include "sai_serialize.h"
#include "swss/logger.h"

extern "C" {
#include "saimetadata.h"
}

#define SAI_OBJECT_ID_BITS_SIZE (8 * sizeof(sai_object_id_t))

static_assert(SAI_OBJECT_ID_BITS_SIZE == 64, "sai_object_id_t must have 64 bits");
static_assert(sizeof(sai_object_id_t) == sizeof(uint64_t), "SAI object ID size should be uint64_t");

#define SAI_VPP_OID_RESERVED_BITS_SIZE ( 8 )

#define SAI_VPP_SWITCH_INDEX_BITS_SIZE ( 8 )
#define SAI_VPP_SWITCH_INDEX_MAX ( (1ULL << SAI_VPP_SWITCH_INDEX_BITS_SIZE) - 1 )
#define SAI_VPP_SWITCH_INDEX_MASK (SAI_VPP_SWITCH_INDEX_MAX)

#define SAI_VPP_GLOBAL_CONTEXT_BITS_SIZE ( 8 )
#define SAI_VPP_GLOBAL_CONTEXT_MAX ( (1ULL << SAI_VPP_GLOBAL_CONTEXT_BITS_SIZE) - 1 )
#define SAI_VPP_GLOBAL_CONTEXT_MASK (SAI_VPP_GLOBAL_CONTEXT_MAX)

#define SAI_VPP_OBJECT_TYPE_BITS_SIZE ( 8 )
#define SAI_VPP_OBJECT_TYPE_MAX ( (1ULL << SAI_VPP_OBJECT_TYPE_BITS_SIZE) - 1 )
#define SAI_VPP_OBJECT_TYPE_MASK (SAI_VPP_OBJECT_TYPE_MAX)

#define SAI_VPP_OBJECT_TYPE_EXTENSIONS_FLAG_BITS_SIZE ( 1 )
#define SAI_VPP_OBJECT_TYPE_EXTENSIONS_FLAG_MAX ( (1ULL << SAI_VPP_OBJECT_TYPE_EXTENSIONS_FLAG_BITS_SIZE) - 1 )
#define SAI_VPP_OBJECT_TYPE_EXTENSIONS_FLAG_MASK (SAI_VPP_OBJECT_TYPE_EXTENSIONS_FLAG_MAX)

#define SAI_VPP_OBJECT_INDEX_BITS_SIZE ( 31 )
#define SAI_VPP_OBJECT_INDEX_MAX ( (1ULL << SAI_VPP_OBJECT_INDEX_BITS_SIZE) - 1 )
#define SAI_VPP_OBJECT_INDEX_MASK (SAI_VPP_OBJECT_INDEX_MAX)

#define SAI_VPP_OBJECT_ID_BITS_SIZE (      \
        SAI_VPP_OID_RESERVED_BITS_SIZE +   \
        SAI_VPP_GLOBAL_CONTEXT_BITS_SIZE + \
        SAI_VPP_SWITCH_INDEX_BITS_SIZE +   \
        SAI_VPP_OBJECT_TYPE_BITS_SIZE +    \
        SAI_VPP_OBJECT_TYPE_EXTENSIONS_FLAG_BITS_SIZE + \
        SAI_VPP_OBJECT_INDEX_BITS_SIZE )

static_assert(SAI_VPP_OBJECT_ID_BITS_SIZE == SAI_OBJECT_ID_BITS_SIZE, "vs object id size must be equal to SAI object id size");

/*
 * This condition must be met, since we need to be able to encode SAI object
 * type in object id on defined number of bits.
 */
static_assert(SAI_OBJECT_TYPE_MAX < 256, "object type must be possible to encode on 1 byte");
static_assert((SAI_OBJECT_TYPE_EXTENSIONS_RANGE_END - SAI_OBJECT_TYPE_EXTENSIONS_RANGE_START) < 256,
        "extensions object type must be possible to encode on 1 byte");

/*
 * Current OBJECT ID format:
 *
 * bits 63..56 - reserved (must be zero)
 * bits 55..48 - global context
 * bits 47..40 - switch index
 * bits 49..32 - SAI object type
 * bits 31..31 - object type extension flag
 * bits 30..0  - object index
 *
 * So large number of bits is required, otherwise we would need to have map of
 * OID to some struct that will have all those values.  But having all this
 * information in OID itself is more convenient.
 *
 * To be backward compatible with previous sairedis, we will still encode base
 * object type on bit's 49..32, and extensions which will now start from range
 * 0x20000000, will be encoded from 0x0, but extensions flag will be set to 1.
 *
 * For example SAI_OBJECT_TYPE_VIRTUAL_ROUTER oid will be encoded as 0x0300000001
 * SAI_OBJECT_TYPE_DASH_ACL_GROUP oid will be encoded as 0x000380000000
 */

#define SAI_VPP_GET_OBJECT_INDEX(oid) \
    ( ((uint64_t)oid) & ( SAI_VPP_OBJECT_INDEX_MASK ) )

#define SAI_VPP_GET_OBJECT_TYPE_EXTENSIONS_FLAG(oid) \
    ( (((uint64_t)oid) >> (SAI_VPP_OBJECT_INDEX_BITS_SIZE) ) & ( SAI_VPP_OBJECT_TYPE_EXTENSIONS_FLAG_MAX ) )

#define SAI_VPP_GET_OBJECT_TYPE(oid) \
    ( (((uint64_t)oid) >> ( SAI_VPP_OBJECT_TYPE_EXTENSIONS_FLAG_BITS_SIZE + SAI_VPP_OBJECT_INDEX_BITS_SIZE) ) & ( SAI_VPP_OBJECT_TYPE_MASK ) )

#define SAI_VPP_GET_SWITCH_INDEX(oid) \
    ( (((uint64_t)oid) >> ( SAI_VPP_OBJECT_TYPE_BITS_SIZE + SAI_VPP_OBJECT_TYPE_EXTENSIONS_FLAG_BITS_SIZE + SAI_VPP_OBJECT_INDEX_BITS_SIZE) ) & ( SAI_VPP_SWITCH_INDEX_MASK ) )

#define SAI_VPP_GET_GLOBAL_CONTEXT(oid) \
    ( (((uint64_t)oid) >> ( SAI_VPP_SWITCH_INDEX_BITS_SIZE + SAI_VPP_OBJECT_TYPE_BITS_SIZE + SAI_VPP_OBJECT_TYPE_EXTENSIONS_FLAG_BITS_SIZE + SAI_VPP_OBJECT_INDEX_BITS_SIZE) ) & ( SAI_VPP_GLOBAL_CONTEXT_MASK ) )

#define SAI_VPP_TEST_OID (0x01234567f9abcdef)

static_assert(SAI_VPP_GET_GLOBAL_CONTEXT(SAI_VPP_TEST_OID) == 0x23, "test global context");
static_assert(SAI_VPP_GET_SWITCH_INDEX(SAI_VPP_TEST_OID) == 0x45, "test switch index");
static_assert(SAI_VPP_GET_OBJECT_TYPE(SAI_VPP_TEST_OID) == 0x67, "test object type");
static_assert(SAI_VPP_GET_OBJECT_INDEX(SAI_VPP_TEST_OID) == 0x79abcdef, "test object index");
static_assert(SAI_VPP_GET_OBJECT_TYPE_EXTENSIONS_FLAG(SAI_VPP_TEST_OID) == 0x1, "test object type extensions flag");

using namespace saivpp;

RealObjectIdManager::RealObjectIdManager(
        _In_ uint32_t globalContext,
        _In_ std::shared_ptr<SwitchConfigContainer> container):
    m_globalContext(globalContext),
    m_container(container)
{
    SWSS_LOG_ENTER();

    if (globalContext > SAI_VPP_GLOBAL_CONTEXT_MAX)
    {
        SWSS_LOG_THROW("specified globalContext(0x%x) > maximum global context 0x%llx",
                globalContext,
                SAI_VPP_GLOBAL_CONTEXT_MAX);
    }

    if (container == nullptr)
    {
        SWSS_LOG_THROW("switch config container cannot be nullptr");
    }
}

sai_object_id_t RealObjectIdManager::saiSwitchIdQuery(
        _In_ sai_object_id_t objectId) const
{
    SWSS_LOG_ENTER();

    if (objectId == SAI_NULL_OBJECT_ID)
    {
        return SAI_NULL_OBJECT_ID;
    }

    sai_object_type_t objectType = saiObjectTypeQuery(objectId);

    if (objectType == SAI_OBJECT_TYPE_NULL)
    {
        SWSS_LOG_THROW("invalid object type of oid %s",
                sai_serialize_object_id(objectId).c_str());
    }

    if (objectType == SAI_OBJECT_TYPE_SWITCH)
    {
        return objectId;
    }

    // NOTE: we could also check:
    // - if object id has correct global context
    // - if object id has existing switch index
    // but then this method can't be made static

    uint32_t switchIndex = (uint32_t)SAI_VPP_GET_SWITCH_INDEX(objectId);

    return constructObjectId(SAI_OBJECT_TYPE_SWITCH, switchIndex, switchIndex, m_globalContext);
}

sai_object_type_t RealObjectIdManager::saiObjectTypeQuery(
        _In_ sai_object_id_t objectId) const
{
    SWSS_LOG_ENTER();

    if (objectId == SAI_NULL_OBJECT_ID)
    {
        return SAI_OBJECT_TYPE_NULL;
    }

    sai_object_type_t objectType = SAI_VPP_GET_OBJECT_TYPE_EXTENSIONS_FLAG(objectId)
        ? (sai_object_type_t)(SAI_VPP_GET_OBJECT_TYPE(objectId) + SAI_OBJECT_TYPE_EXTENSIONS_RANGE_START)
        : (sai_object_type_t)(SAI_VPP_GET_OBJECT_TYPE(objectId));

    if (sai_metadata_is_object_type_valid(objectType) == false)
    {
        SWSS_LOG_ERROR("invalid object id %s",
                sai_serialize_object_id(objectId).c_str());

        /*
         * We can't throw here, since it would give no meaningful message.
         * Throwing at one level up is better.
         */

        return SAI_OBJECT_TYPE_NULL;
    }

    // NOTE: we could also check:
    // - if object id has correct global context
    // - if object id has existing switch index
    // but then this method can't be made static

    return objectType;
}

void RealObjectIdManager::clear()
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("clearing switch index set");

    m_switchIndexes.clear();
    m_indexer.clear();
}

void RealObjectIdManager::releaseSwitchIndex(
        _In_ uint32_t index)
{
    SWSS_LOG_ENTER();

    auto it = m_switchIndexes.find(index);

    if (it == m_switchIndexes.end())
    {
        SWSS_LOG_THROW("switch index 0x%x is invalid! programming error", index);
    }

    m_switchIndexes.erase(it);

    SWSS_LOG_DEBUG("released switch index 0x%x", index);
}


sai_object_id_t RealObjectIdManager::allocateNewObjectId(
        _In_ sai_object_type_t objectType,
        _In_ sai_object_id_t switchId)
{
    SWSS_LOG_ENTER();

    if (sai_metadata_is_object_type_valid(objectType) == false)
    {
        SWSS_LOG_THROW("invalid objct type: %d", objectType);
    }

    if (objectType == SAI_OBJECT_TYPE_SWITCH)
    {
        SWSS_LOG_THROW("this function can't be used to allocate switch id");
    }

    sai_object_type_t switchObjectType = saiObjectTypeQuery(switchId);

    if (switchObjectType != SAI_OBJECT_TYPE_SWITCH)
    {
        SWSS_LOG_THROW("object type of switch %s is %s, should be SWITCH",
                sai_serialize_object_id(switchId).c_str(),
                sai_serialize_object_type(switchObjectType).c_str());
    }

    uint32_t switchIndex = (uint32_t)SAI_VPP_GET_SWITCH_INDEX(switchId);

    // count from zero
    uint64_t objectIndex = m_indexer[objectType]++; // allocation !

    if (objectIndex > SAI_VPP_OBJECT_INDEX_MAX)
    {
        SWSS_LOG_THROW("no more object indexes available, given: 0x%lx but limit is 0x%llx",
                objectIndex,
                SAI_VPP_OBJECT_INDEX_MAX);
    }

    sai_object_id_t objectId = constructObjectId(objectType, switchIndex, objectIndex, m_globalContext);

    SWSS_LOG_DEBUG("created RID %s",
            sai_serialize_object_id(objectId).c_str());

    return objectId;
}

sai_object_id_t RealObjectIdManager::allocateNewSwitchObjectId(
        _In_ const std::string& hardwareInfo)
{
    SWSS_LOG_ENTER();

    auto config = m_container->getConfig(hardwareInfo);

    if (config == nullptr)
    {
        SWSS_LOG_ERROR("no switch config for hardware info: '%s'", hardwareInfo.c_str());

        return SAI_NULL_OBJECT_ID;
    }

    uint32_t switchIndex = config->m_switchIndex;

    if (switchIndex > SAI_VPP_SWITCH_INDEX_MAX)
    {
        SWSS_LOG_THROW("switch index %u > %llu (max)", switchIndex, SAI_VPP_SWITCH_INDEX_MAX);
    }

    m_switchIndexes.insert(switchIndex);

    sai_object_id_t objectId = constructObjectId(SAI_OBJECT_TYPE_SWITCH, switchIndex, switchIndex, m_globalContext);

    SWSS_LOG_NOTICE("created SWITCH RID %s for hwinfo: '%s'",
            sai_serialize_object_id(objectId).c_str(),
            hardwareInfo.c_str());

    return objectId;
}

void RealObjectIdManager::releaseObjectId(
        _In_ sai_object_id_t objectId)
{
    SWSS_LOG_ENTER();

    if (saiObjectTypeQuery(objectId) == SAI_OBJECT_TYPE_SWITCH)
    {
        releaseSwitchIndex((uint32_t)SAI_VPP_GET_SWITCH_INDEX(objectId));
    }
}

sai_object_id_t RealObjectIdManager::constructObjectId(
        _In_ sai_object_type_t objectType,
        _In_ uint32_t switchIndex,
        _In_ uint64_t objectIndex,
        _In_ uint32_t globalContext)
{
    SWSS_LOG_ENTER();

    if (sai_metadata_is_object_type_valid(objectType) == false)
    {
        SWSS_LOG_THROW("FATAL: invalid object type (0x%x), logic error, this is a bug!", objectType);
    }

    uint64_t extensionsFlag = (uint64_t)objectType >= SAI_OBJECT_TYPE_EXTENSIONS_RANGE_START;

    objectType = extensionsFlag
        ? (sai_object_type_t)(objectType - SAI_OBJECT_TYPE_EXTENSIONS_RANGE_START)
        : objectType;

    return (sai_object_id_t)(
            ((uint64_t)globalContext << ( SAI_VPP_SWITCH_INDEX_BITS_SIZE + SAI_VPP_OBJECT_TYPE_BITS_SIZE + SAI_VPP_OBJECT_TYPE_EXTENSIONS_FLAG_BITS_SIZE + SAI_VPP_OBJECT_INDEX_BITS_SIZE )) |
            ((uint64_t)switchIndex << ( SAI_VPP_OBJECT_TYPE_BITS_SIZE + SAI_VPP_OBJECT_TYPE_EXTENSIONS_FLAG_BITS_SIZE + SAI_VPP_OBJECT_INDEX_BITS_SIZE )) |
            ((uint64_t)objectType << ( SAI_VPP_OBJECT_TYPE_EXTENSIONS_FLAG_BITS_SIZE + SAI_VPP_OBJECT_INDEX_BITS_SIZE)) |
            ((uint64_t)extensionsFlag << (SAI_VPP_OBJECT_INDEX_BITS_SIZE)) |
            objectIndex);
}

sai_object_id_t RealObjectIdManager::switchIdQuery(
        _In_ sai_object_id_t objectId)
{
    SWSS_LOG_ENTER();

    if (objectId == SAI_NULL_OBJECT_ID)
    {
        return SAI_NULL_OBJECT_ID;
    }

    sai_object_type_t objectType = objectTypeQuery(objectId);

    if (objectType == SAI_OBJECT_TYPE_NULL)
    {
        SWSS_LOG_ERROR("invalid object type of oid %s",
                sai_serialize_object_id(objectId).c_str());

        return SAI_NULL_OBJECT_ID;
    }

    if (objectType == SAI_OBJECT_TYPE_SWITCH)
    {
        return objectId;
    }

    uint32_t switchIndex = (uint32_t)SAI_VPP_GET_SWITCH_INDEX(objectId);
    uint32_t globalContext = (uint32_t)SAI_VPP_GET_GLOBAL_CONTEXT(objectId);

    return constructObjectId(SAI_OBJECT_TYPE_SWITCH, switchIndex, switchIndex, globalContext);
}

sai_object_type_t RealObjectIdManager::objectTypeQuery(
        _In_ sai_object_id_t objectId)
{
    SWSS_LOG_ENTER();

    if (objectId == SAI_NULL_OBJECT_ID)
    {
        return SAI_OBJECT_TYPE_NULL;
    }

    sai_object_type_t objectType = SAI_VPP_GET_OBJECT_TYPE_EXTENSIONS_FLAG(objectId)
        ? (sai_object_type_t)(SAI_VPP_GET_OBJECT_TYPE(objectId) + SAI_OBJECT_TYPE_EXTENSIONS_RANGE_START)
        : (sai_object_type_t)(SAI_VPP_GET_OBJECT_TYPE(objectId));

    if (!sai_metadata_is_object_type_valid(objectType))
    {
        SWSS_LOG_ERROR("invalid object id %s",
                sai_serialize_object_id(objectId).c_str());

        return SAI_OBJECT_TYPE_NULL;
    }

    return objectType;
}

uint32_t RealObjectIdManager::getSwitchIndex(
        _In_ sai_object_id_t objectId)
{
    SWSS_LOG_ENTER();

    auto switchId = switchIdQuery(objectId);

    return SAI_VPP_GET_SWITCH_INDEX(switchId);
}

void RealObjectIdManager::updateWarmBootObjectIndex(
        _In_ sai_object_id_t oid)
{
    SWSS_LOG_ENTER();

    sai_object_type_t objectType = objectTypeQuery(oid);

    if (objectType == SAI_OBJECT_TYPE_NULL)
    {
        SWSS_LOG_THROW("invalid object type on warm boot object: %s",
                sai_serialize_object_id(oid).c_str());
    }

    if (objectType == SAI_OBJECT_TYPE_SWITCH)
    {
        uint32_t switchIndex = (uint32_t)SAI_VPP_GET_SWITCH_INDEX(oid);

        m_switchIndexes.insert(switchIndex);
    }

    uint64_t index = SAI_VPP_GET_OBJECT_INDEX(oid);

    if (m_indexer[objectType] <= index)
    {
        uint64_t prev = m_indexer[objectType];

        m_indexer[objectType] = index + 1; // +1 since this will be next object number

        SWSS_LOG_DEBUG("update %s:%s real id to from %lu to %lu",
                sai_serialize_object_type(objectType).c_str(),
                sai_serialize_object_id(oid).c_str(),
                prev,
                index + 1);
    }
}
