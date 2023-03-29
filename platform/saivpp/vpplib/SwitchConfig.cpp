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
#include "saivpp.h"
#include "SwitchConfig.h"

#include "swss/logger.h"

#include <cstring>

using namespace saivpp;

SwitchConfig::SwitchConfig(
        _In_ uint32_t switchIndex,
        _In_ const std::string& hwinfo):
    m_saiSwitchType(SAI_SWITCH_TYPE_NPU),
    m_switchType(SAI_VPP_SWITCH_TYPE_NONE),
    m_bootType(SAI_VPP_BOOT_TYPE_COLD),
    m_switchIndex(switchIndex),
    m_hardwareInfo(hwinfo),
    m_useTapDevice(false)
{
    SWSS_LOG_ENTER();

    // empty
}

bool SwitchConfig::parseSaiSwitchType(
        _In_ const char* saiSwitchTypeStr,
        _Out_ sai_switch_type_t& saiSwitchType)
{
    SWSS_LOG_ENTER();

    std::string st = (saiSwitchTypeStr == NULL) ? "unknown" : saiSwitchTypeStr;

    if (st == SAI_VALUE_SAI_SWITCH_TYPE_NPU)
    {
        saiSwitchType = SAI_SWITCH_TYPE_NPU;
    }
    else if (st == SAI_VALUE_SAI_SWITCH_TYPE_PHY)
    {
        saiSwitchType = SAI_SWITCH_TYPE_PHY;
    }
    else
    {
        SWSS_LOG_ERROR("unknown SAI switch type: '%s', expected (%s|%s)",
                saiSwitchTypeStr,
                SAI_VALUE_SAI_SWITCH_TYPE_NPU,
                SAI_VALUE_SAI_SWITCH_TYPE_PHY);

        return false;
    }

    return true;
}

bool SwitchConfig::parseSwitchType(
        _In_ const char* switchTypeStr,
        _Out_ sai_vpp_switch_type_t& switchType)
{
    SWSS_LOG_ENTER();

    std::string st = (switchTypeStr == NULL) ? "unknown" : switchTypeStr;

    if (st == SAI_VALUE_VPP_SWITCH_TYPE_BCM56850)
    {
        switchType = SAI_VPP_SWITCH_TYPE_BCM56850;
    }
    else if (st == SAI_VALUE_VPP_SWITCH_TYPE_BCM56971B0)
    {
        switchType = SAI_VPP_SWITCH_TYPE_BCM56971B0;
    }
    else if (st == SAI_VALUE_VPP_SWITCH_TYPE_BCM81724)
    {
        switchType = SAI_VPP_SWITCH_TYPE_BCM81724;
    }
    else if (st == SAI_VALUE_VPP_SWITCH_TYPE_MLNX2700)
    {
        switchType = SAI_VPP_SWITCH_TYPE_MLNX2700;
    }
    else
    {
        SWSS_LOG_ERROR("unknown switch type: '%s', expected (%s|%s|%s|%s)",
                switchTypeStr,
                SAI_VALUE_VPP_SWITCH_TYPE_BCM81724,
                SAI_VALUE_VPP_SWITCH_TYPE_BCM56850,
                SAI_VALUE_VPP_SWITCH_TYPE_BCM56971B0,
                SAI_VALUE_VPP_SWITCH_TYPE_MLNX2700);

        return false;
    }

    return true;
}

bool SwitchConfig::parseBootType(
        _In_ const char* bootTypeStr,
        _Out_ sai_vpp_boot_type_t& bootType)
{
    SWSS_LOG_ENTER();

    std::string bt = (bootTypeStr == NULL) ? "cold" : bootTypeStr;

    if (bt == "cold" || bt == SAI_VALUE_VPP_BOOT_TYPE_COLD)
    {
        bootType = SAI_VPP_BOOT_TYPE_COLD;
    }
    else if (bt == "warm" || bt == SAI_VALUE_VPP_BOOT_TYPE_WARM)
    {
        bootType = SAI_VPP_BOOT_TYPE_WARM;
    }
    else if (bt == "fast" || bt == SAI_VALUE_VPP_BOOT_TYPE_FAST)
    {
        bootType = SAI_VPP_BOOT_TYPE_FAST;
    }
    else
    {
        SWSS_LOG_ERROR("unknown boot type: '%s', expected (cold|warm|fast)", bootTypeStr);

        return false;
    }

    return true;
}

bool SwitchConfig::parseUseTapDevice(
        _In_ const char* useTapDeviceStr)
{
    SWSS_LOG_ENTER();

    if (useTapDeviceStr)
    {
        return strcmp(useTapDeviceStr, "true") == 0;
    }

    return false;
}
