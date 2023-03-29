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
#include "sai_vpp.h"

VPP_GENERIC_QUAD(DTEL,dtel);
VPP_GENERIC_QUAD(DTEL_QUEUE_REPORT,dtel_queue_report);
VPP_GENERIC_QUAD(DTEL_INT_SESSION,dtel_int_session);
VPP_GENERIC_QUAD(DTEL_REPORT_SESSION,dtel_report_session);
VPP_GENERIC_QUAD(DTEL_EVENT,dtel_event);

const sai_dtel_api_t vpp_dtel_api = {

    VPP_GENERIC_QUAD_API(dtel)
    VPP_GENERIC_QUAD_API(dtel_queue_report)
    VPP_GENERIC_QUAD_API(dtel_int_session)
    VPP_GENERIC_QUAD_API(dtel_report_session)
    VPP_GENERIC_QUAD_API(dtel_event)
};
