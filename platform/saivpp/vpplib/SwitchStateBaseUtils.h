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
#include "vppxlate/SaiVppXlate.h"
#include "swss/ipaddress.h"
#include "swss/ipprefix.h"

namespace saivpp
{
inline static sai_status_t find_attrib_in_list(_In_ uint32_t                       attr_count,
                                 _In_ const sai_attribute_t         *attr_list,
                                 _In_ sai_attr_id_t                  attrib_id,
                                 _Out_ const sai_attribute_value_t **attr_value,
                                 _Out_ uint32_t                     *index)
{
    uint32_t ii;

    SWSS_LOG_ENTER();

    if ((attr_count) && (NULL == attr_list)) {
        SWSS_LOG_ERROR("NULL value attr list\n");
        return SAI_STATUS_INVALID_PARAMETER;
    }

    if (NULL == attr_value) {
        SWSS_LOG_ERROR("NULL value attr value\n");
        return SAI_STATUS_INVALID_PARAMETER;
    }

    if (NULL == index) {
        SWSS_LOG_ERROR("NULL value index\n");
        return SAI_STATUS_INVALID_PARAMETER;
    }

    for (ii = 0; ii < attr_count; ii++) {
        if (attr_list[ii].id == attrib_id) {
            *attr_value = &(attr_list[ii].value);
            *index = ii;
            return SAI_STATUS_SUCCESS;
        }
    }

    *attr_value = NULL;

    return SAI_STATUS_ITEM_NOT_FOUND;
}

inline int getPrefixLenFromAddrMask(const uint8_t *addr, int len)
{
    // Iterate over each byte from left to right
    for (int i = 0; i < len; ++i)
    {
        uint8_t byte = addr[i];

        // If the byte is 0xFF, it means all bits are set, continue to the next byte
        if (byte == 0xFF)
        {
            continue;
        }

        // If the byte is not 0xFF, count the number of leading 1s in this byte
        int leading_ones = 0;
        for (int j = 7; j >= 0; --j)
        {
            if ((byte >> j) & 0x1)
            {
                ++leading_ones;
            }
            else
            {
                break;
            }
        }

        // Return the total number of leading 1s in the address mask
        return i * 8 + leading_ones;
    }

    // If all bytes are 0xFF, return the total length in bits
    return len * 8;
}

inline static swss::IpPrefix getIpPrefixFromSaiPrefix(const sai_ip_prefix_t& src)
{
    swss::ip_addr_t ip;
    switch(src.addr_family)
    {
        case SAI_IP_ADDR_FAMILY_IPV4:
            ip.family = AF_INET;
            ip.ip_addr.ipv4_addr = src.addr.ip4;
            return swss::IpPrefix(ip, getPrefixLenFromAddrMask(reinterpret_cast<const uint8_t*>(&src.mask.ip4), 4));
        case SAI_IP_ADDR_FAMILY_IPV6:
            ip.family = AF_INET6;
            memcpy(ip.ip_addr.ipv6_addr, src.addr.ip6, 16);
            return swss::IpPrefix(ip, getPrefixLenFromAddrMask(src.mask.ip6, 16));
        default:
            throw std::logic_error("Invalid family");
    }
}

inline static sai_ip_prefix_t& subnet(sai_ip_prefix_t& dst, const sai_ip_prefix_t& src)
{
    dst.addr_family = src.addr_family;
    switch(src.addr_family)
    {
        case SAI_IP_ADDR_FAMILY_IPV4:
            dst.addr.ip4 = src.addr.ip4 & src.mask.ip4;
            dst.mask.ip4 = src.mask.ip4;
            break;
        case SAI_IP_ADDR_FAMILY_IPV6:
            for (size_t i = 0; i < 16; i++)
            {
                dst.addr.ip6[i] = src.addr.ip6[i] & src.mask.ip6[i];
                dst.mask.ip6[i] = src.mask.ip6[i];
            }
            break;
        default:
            throw std::logic_error("Invalid family");
    }
    return dst;
}

inline static sai_ip_prefix_t& copy(sai_ip_prefix_t& dst, const swss::IpPrefix& src)
{
    auto ia = src.getIp().getIp();
    auto ma = src.getMask().getIp();
    switch(ia.family)
    {
        case AF_INET:
            dst.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
            dst.addr.ip4 = ia.ip_addr.ipv4_addr;
            dst.mask.ip4 = ma.ip_addr.ipv4_addr;
            break;
        case AF_INET6:
            dst.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
            memcpy(dst.addr.ip6, ia.ip_addr.ipv6_addr, 16);
            memcpy(dst.mask.ip6, ma.ip_addr.ipv6_addr, 16);
            break;
        default:
            throw std::logic_error("Invalid family");
    }
    return dst;
}

inline static void
sai_ip_address_t_to_vpp_ip_addr_t(sai_ip_address_t& src, vpp_ip_addr_t& dst)
{
    if (SAI_IP_ADDR_FAMILY_IPV4 == src.addr_family) {
        struct sockaddr_in *sin =  &dst.addr.ip4;

        dst.sa_family = AF_INET;
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = src.addr.ip4;
    } else {
        struct sockaddr_in6 *sin6 =  &dst.addr.ip6;

        dst.sa_family = AF_INET6;
        sin6->sin6_family = AF_INET6;
	    memcpy(sin6->sin6_addr.s6_addr, src.addr.ip6, sizeof(sin6->sin6_addr.s6_addr));
    }
}
}

