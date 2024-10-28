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

#include "LaneMap.h"
#include "EventQueue.h"
#include "ResourceLimiter.h"
#include "CorePortIndexMap.h"

#include <string>
#include <memory>

extern "C" {
#include "sai.h"
}

namespace saivpp
{
    typedef enum _sai_vpp_switch_type_t
    {
        SAI_VPP_SWITCH_TYPE_NONE,

        SAI_VPP_SWITCH_TYPE_BCM56850,

        SAI_VPP_SWITCH_TYPE_BCM56971B0,

        SAI_VPP_SWITCH_TYPE_BCM81724,

        SAI_VPP_SWITCH_TYPE_MLNX2700,

    } sai_vpp_switch_type_t;

    typedef enum _sai_vpp_boot_type_t
    {
        SAI_VPP_BOOT_TYPE_COLD,

        SAI_VPP_BOOT_TYPE_WARM,

        SAI_VPP_BOOT_TYPE_FAST,

    } sai_vpp_boot_type_t;

    class SwitchConfig
    {
        public:

            SwitchConfig(
                    _In_ uint32_t switchIndex,
                    _In_ const std::string& hwinfo);

            virtual ~SwitchConfig() = default;

        public:

            static bool parseSaiSwitchType(
                    _In_ const char* saiSwitchTypeStr,
                    _Out_ sai_switch_type_t& saiSwitchType);

            static bool parseSwitchType(
                    _In_ const char* switchTypeStr,
                    _Out_ sai_vpp_switch_type_t& switchType);

            static bool parseBootType(
                    _In_ const char* bootTypeStr,
                    _Out_ sai_vpp_boot_type_t& bootType);

            static bool parseUseTapDevice(
                    _In_ const char* useTapDeviceStr);

        public:

            sai_switch_type_t m_saiSwitchType;

            sai_vpp_switch_type_t m_switchType;

            sai_vpp_boot_type_t m_bootType;

            uint32_t m_switchIndex;

            std::string m_hardwareInfo;

            bool m_useTapDevice;

            std::shared_ptr<LaneMap> m_laneMap;

            std::shared_ptr<LaneMap> m_fabricLaneMap;

            std::shared_ptr<EventQueue> m_eventQueue;

            std::shared_ptr<ResourceLimiter> m_resourceLimiter;

            std::shared_ptr<CorePortIndexMap> m_corePortIndexMap;
    };
}
