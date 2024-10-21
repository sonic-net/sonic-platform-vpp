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

#define SAI_KEY_VPP_SWITCH_TYPE              "SAI_VPP_SWITCH_TYPE"
#define SAI_KEY_VPP_SAI_SWITCH_TYPE          "SAI_VPP_SAI_SWITCH_TYPE"

#define SAI_VALUE_SAI_SWITCH_TYPE_NPU       "SAI_SWITCH_TYPE_NPU"
#define SAI_VALUE_SAI_SWITCH_TYPE_PHY       "SAI_SWITCH_TYPE_PHY"

/**
 * @def SAI_KEY_VPP_INTERFACE_LANE_MAP_FILE
 *
 * If specified in profile.ini it should point to eth interface to lane map.
 *
 * Example:
 * eth0:1,2,3,4
 * eth1:5,6,7,8
 *
 * TODO must support hardware info for multiple switches
 */
#define SAI_KEY_VPP_INTERFACE_LANE_MAP_FILE  "SAI_VPP_INTERFACE_LANE_MAP_FILE"

/**
 * @def SAI_KEY_VPP_RESOURCE_LIMITER_FILE
 *
 * File with resource limitations for object type create.
 *
 * Example:
 * SAI_OBJECT_TYPE_ACL_TABLE=3
 */
#define SAI_KEY_VPP_RESOURCE_LIMITER_FILE    "SAI_VPP_RESOURCE_LIMITER_FILE"

/**
 * @def SAI_KEY_VPP_INTERFACE_FABRIC_LANE_MAP_FILE
 *
 * If specified in profile.ini it should point to fabric port to lane map.
 *
 * Example:
 * fabric0:1
 * fabric1:2
 *
 */
#define SAI_KEY_VPP_INTERFACE_FABRIC_LANE_MAP_FILE  "SAI_VPP_INTERFACE_FABRIC_LANE_MAP_FILE"

/**
 * @def SAI_KEY_VPP_HOSTIF_USE_TAP_DEVICE
 *
 * Bool flag, (true/false). If set to true, then during create host interface
 * sai object also tap device will be created and mac address will be assigned.
 * For this operation root privileges will be required.
 *
 * By default this flag is set to false.
 */
#define SAI_KEY_VPP_HOSTIF_USE_TAP_DEVICE      "SAI_VPP_HOSTIF_USE_TAP_DEVICE"

/**
 * @def SAI_KEY_VPP_CORE_PORT_INDEX_MAP_FILE
 *
 * For VOQ systems if specified in profile.ini it should point to eth interface to
 * core and core port index map as port name:core_index,core_port_index
 *
 * Example:
 * eth1:0,1
 * eth17:1,1
 *
 */
#define SAI_KEY_VPP_CORE_PORT_INDEX_MAP_FILE  "SAI_VPP_CORE_PORT_INDEX_MAP_FILE"

/**
 * @brief Context config.
 *
 * Optional. Should point to a context_config.json which will contain how many
 * contexts (syncd) we have in the system globally and each context how many
 * switches it manages. Only one of this contexts will be used in VS.
 */
#define SAI_KEY_VPP_CONTEXT_CONFIG             "SAI_VPP_CONTEXT_CONFIG"

/**
 * @brief Global context.
 *
 * Optional. Should point to GUID value which is provided in context_config.json
 * by SAI_KEY_VPP_CONTEXT_CONFIG. Default is 0.
 */
#define SAI_KEY_VPP_GLOBAL_CONTEXT             "SAI_VPP_GLOBAL_CONTEXT"

#define SAI_VALUE_VPP_SWITCH_TYPE_BCM56850     "SAI_VPP_SWITCH_TYPE_BCM56850"
#define SAI_VALUE_VPP_SWITCH_TYPE_BCM56971B0   "SAI_VPP_SWITCH_TYPE_BCM56971B0"
#define SAI_VALUE_VPP_SWITCH_TYPE_BCM81724     "SAI_VPP_SWITCH_TYPE_BCM81724"
#define SAI_VALUE_VPP_SWITCH_TYPE_MLNX2700     "SAI_VPP_SWITCH_TYPE_MLNX2700"

/*
 * Values for SAI_KEY_BOOT_TYPE (defined in saiswitch.h)
 */

#define SAI_VALUE_VPP_BOOT_TYPE_COLD "0"
#define SAI_VALUE_VPP_BOOT_TYPE_WARM "1"
#define SAI_VALUE_VPP_BOOT_TYPE_FAST "2"

/**
 * @def SAI_VPP_UNITTEST_CHANNEL
 *
 * Notification channel for redis database.
 */
#define SAI_VPP_UNITTEST_CHANNEL     "SAI_VPP_UNITTEST_CHANNEL"

/**
 * @def SAI_VPP_UNITTEST_SET_RO_OP
 *
 * Notification operation for "SET" READ_ONLY attribute.
 */
#define SAI_VPP_UNITTEST_SET_RO_OP   "set_ro"

/**
 * @def SAI_VPP_UNITTEST_SET_STATS
 *
 * Notification operation for "SET" stats on specific object.
 */
#define SAI_VPP_UNITTEST_SET_STATS_OP      "set_stats"

/**
 * @def SAI_VPP_UNITTEST_ENABLE
 *
 * Notification operation for enabling unittests.
 */
#define SAI_VPP_UNITTEST_ENABLE_UNITTESTS  "enable_unittests"

typedef enum _sai_vpp_switch_attr_t
{
    /**
     * @brief Will enable metadata unittests.
     *
     * @type bool
     * @flags CREATE_AND_SET
     * @default false
     */
    SAI_VPP_SWITCH_ATTR_META_ENABLE_UNITTESTS = SAI_SWITCH_ATTR_CUSTOM_RANGE_START,

    /**
     * @brief Will allow to set value that is read only.
     *
     * Unittests must be enabled.
     *
     * Value is the attribute to be allowed.
     *
     * @type sai_int32_t
     * @flags CREATE_AND_SET
     */
    SAI_VPP_SWITCH_ATTR_META_ALLOW_READ_ONLY_ONCE,

} sau_vpp_switch_attr_t;
