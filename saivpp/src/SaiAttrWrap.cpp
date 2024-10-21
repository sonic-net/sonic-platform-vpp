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
#include "SaiAttrWrap.h"

#include "swss/logger.h"
#include "sai_serialize.h"

using namespace saivpp;

SaiAttrWrap::SaiAttrWrap(
        _In_ sai_object_type_t object_type,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    m_meta = sai_metadata_get_attr_metadata(object_type, attr->id);

    m_attr.id = attr->id;

    /*
     * We are making serialize and deserialize to get copy of
     * attribute, it may be a list so we need to allocate new memory.
     *
     * This copy will be used later to get previous value of attribute
     * if attribute will be updated. And if this attribute is oid list
     * then we need to release object reference count.
     */

    m_value = sai_serialize_attr_value(*m_meta, *attr, false);

    sai_deserialize_attr_value(m_value, *m_meta, m_attr, false);
}

SaiAttrWrap::SaiAttrWrap(
        _In_ const std::string& attrId,
        _In_ const std::string& attrValue)
{
    SWSS_LOG_ENTER();

    m_meta = sai_metadata_get_attr_metadata_by_attr_id_name(attrId.c_str());

    if (m_meta == NULL)
    {
        SWSS_LOG_THROW("failed to find metadata for %s", attrId.c_str());
    }

    m_attr.id = m_meta->attrid;

    m_value = attrValue;

    sai_deserialize_attr_value(attrValue.c_str(), *m_meta, m_attr, false);
}

SaiAttrWrap::~SaiAttrWrap()
{
    SWSS_LOG_ENTER();

    /*
     * On destructor we need to call free to deallocate possible
     * allocated list on constructor.
     */

    sai_deserialize_free_attribute_value(m_meta->attrvaluetype, m_attr);
}

const sai_attribute_t* SaiAttrWrap::getAttr() const
{
    SWSS_LOG_ENTER();

    return &m_attr;
}

const sai_attr_metadata_t* SaiAttrWrap::getAttrMetadata() const
{
    SWSS_LOG_ENTER();

    return m_meta;
}

const std::string& SaiAttrWrap::getAttrStrValue() const
{
    SWSS_LOG_ENTER();

    return m_value;
}
