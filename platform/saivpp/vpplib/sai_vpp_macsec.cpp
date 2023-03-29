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

VPP_GENERIC_QUAD(MACSEC,macsec);

VPP_GENERIC_QUAD(MACSEC_PORT,macsec_port);
VPP_GENERIC_STATS(MACSEC_PORT,macsec_port);

VPP_GENERIC_QUAD(MACSEC_FLOW,macsec_flow);
VPP_GENERIC_STATS(MACSEC_FLOW,macsec_flow);

VPP_GENERIC_QUAD(MACSEC_SC,macsec_sc);
VPP_GENERIC_STATS(MACSEC_SC,macsec_sc);

VPP_GENERIC_QUAD(MACSEC_SA,macsec_sa);
VPP_GENERIC_STATS(MACSEC_SA,macsec_sa);

const sai_macsec_api_t vpp_macsec_api = {

    VPP_GENERIC_QUAD_API(macsec)

    VPP_GENERIC_QUAD_API(macsec_port)
    VPP_GENERIC_STATS_API(macsec_port)

    VPP_GENERIC_QUAD_API(macsec_flow)
    VPP_GENERIC_STATS_API(macsec_flow)

    VPP_GENERIC_QUAD_API(macsec_sc)
    VPP_GENERIC_STATS_API(macsec_sc)

    VPP_GENERIC_QUAD_API(macsec_sa)
    VPP_GENERIC_STATS_API(macsec_sa)
};
