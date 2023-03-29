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

sai_status_t sai_tam_telemetry_get_data(
        _In_ sai_object_id_t switch_id,
        _In_ sai_object_list_t obj_list,
        _In_ bool clear_on_read,
        _Inout_ sai_size_t *buffer_size,
        _Out_ void *buffer)
{
    SWSS_LOG_ENTER();

    return SAI_STATUS_NOT_IMPLEMENTED;
}

VPP_GENERIC_QUAD(TAM,tam);
VPP_GENERIC_QUAD(TAM_MATH_FUNC,tam_math_func);
VPP_GENERIC_QUAD(TAM_REPORT,tam_report);
VPP_GENERIC_QUAD(TAM_EVENT_THRESHOLD,tam_event_threshold);
VPP_GENERIC_QUAD(TAM_INT,tam_int);
VPP_GENERIC_QUAD(TAM_TEL_TYPE,tam_tel_type);
VPP_GENERIC_QUAD(TAM_TRANSPORT,tam_transport);
VPP_GENERIC_QUAD(TAM_TELEMETRY,tam_telemetry);
VPP_GENERIC_QUAD(TAM_COLLECTOR,tam_collector);
VPP_GENERIC_QUAD(TAM_EVENT_ACTION,tam_event_action);
VPP_GENERIC_QUAD(TAM_EVENT,tam_event);

const sai_tam_api_t vpp_tam_api = {

    VPP_GENERIC_QUAD_API(tam)
    VPP_GENERIC_QUAD_API(tam_math_func)
    VPP_GENERIC_QUAD_API(tam_report)
    VPP_GENERIC_QUAD_API(tam_event_threshold)
    VPP_GENERIC_QUAD_API(tam_int)
    VPP_GENERIC_QUAD_API(tam_tel_type)
    VPP_GENERIC_QUAD_API(tam_transport)
    VPP_GENERIC_QUAD_API(tam_telemetry)
    VPP_GENERIC_QUAD_API(tam_collector)
    VPP_GENERIC_QUAD_API(tam_event_action)
    VPP_GENERIC_QUAD_API(tam_event)

};
