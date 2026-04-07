/*
 * Copyright (c) 2024 Cisco and/or its affiliates.
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
#ifndef __included_sonic_ip_validate_h__
#define __included_sonic_ip_validate_h__

#include <vnet/vnet.h>
#include <vnet/ip/ip.h>

typedef struct
{
  /* API message ID base */
  u16 msg_id_base;

  vnet_main_t *vnet_main;
} sonic_ip_validate_main_t;

extern sonic_ip_validate_main_t sonic_ip_validate_main;

extern vlib_node_registration_t sonic_ip4_validate_node;
extern vlib_node_registration_t sonic_ip6_validate_node;

#define SONIC_IP_VALIDATE_PLUGIN_BUILD_VER "1.0"

#endif /* __included_sonic_ip_validate_h__ */
