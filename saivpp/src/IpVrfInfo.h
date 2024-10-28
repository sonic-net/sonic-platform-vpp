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
#include "sai.h"
}

namespace saivpp
{
    class IpVrfInfo
    {
        public:
	    IpVrfInfo(
		      _In_ sai_object_id_t obj_id,
		      _In_ uint32_t vrf_id,
		      _In_ std::string &vrf_name,
		      bool is_ipv6);

	    virtual ~IpVrfInfo();

        public:
	    sai_object_id_t m_obj_id;
	    uint32_t m_vrf_id;
	    std::string m_vrf_name;
	    bool m_is_ipv6;
    };
}
