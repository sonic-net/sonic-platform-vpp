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

#include "swss/sal.h"

#include <sys/types.h>

namespace saivpp
{
    typedef enum _FilterPriority
    {
        MACSEC_FILTER,

    } FilterPriority;

    class TrafficFilter
    {
        public:

            typedef enum _FilterStatus
            {
                CONTINUE,

                TERMINATE,

                ERROR,

            } FilterStatus;

        public:

            TrafficFilter() = default;

            virtual ~TrafficFilter() = default;

        public:

            virtual FilterStatus execute(
                    _Inout_ void *buffer,
                    _Inout_ size_t &length) = 0;
    };
}
