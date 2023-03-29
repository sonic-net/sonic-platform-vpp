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

extern "C" {
#include "saimetadata.h"
}

#include <string>

namespace saivpp
{
    // TODO unify wrapper and add to common
    class SaiAttrWrap
    {
        private:

            SaiAttrWrap(const SaiAttrWrap&) = delete;
            SaiAttrWrap& operator=(const SaiAttrWrap&) = delete;

        public:

            SaiAttrWrap(
                    _In_ sai_object_type_t object_type,
                    _In_ const sai_attribute_t *attr);

            SaiAttrWrap(
                    _In_ const std::string& attrId,
                    _In_ const std::string& attrValue);

            virtual ~SaiAttrWrap();

        public:

            const sai_attribute_t* getAttr() const;

            const sai_attr_metadata_t* getAttrMetadata() const;

            const std::string& getAttrStrValue() const;

        private:

            const sai_attr_metadata_t *m_meta;

            sai_attribute_t m_attr;

            std::string m_value;
    };
}
