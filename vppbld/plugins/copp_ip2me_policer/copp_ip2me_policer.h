/*
 * Copyright (c) 2026 SONiC-VPP contributors
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
#ifndef __included_copp_ip2me_policer_h__
#define __included_copp_ip2me_policer_h__

#include <vnet/vnet.h>

/*
 * IPv4 destination addresses considered "IP2ME" -- traffic destined to
 * one of the router's own addresses that VPP's own dataplane doesn't
 * answer itself (i.e. anything that reaches ip4-punt at all: SSH, SNMP,
 * unregistered ports, ...). Enforced on the ip4-punt feature arc, which
 * VPP only reaches AFTER ip4-lookup/ip4-local have already decided this
 * packet is host-bound -- unlike device-input (too early: dest-IP alone
 * doesn't mean CPU-bound, e.g. ping-to-router is answered in-dataplane)
 * or VPP's built-in ip4-policer-classify feature (which only matches on
 * interfaces it's explicitly bound to, requiring per-RIF bookkeeping
 * that this plugin avoids entirely by living on a global, always-on
 * arc instead).
 */
#define COPP_IP2ME_POLICER_MAX_ADDRS 256
#define COPP_IP2ME_POLICER_NAME_LEN 64

typedef struct
{
  u32 addr;    /* network byte order, matches ip4_address_t.as_u32 */
  u8 in_use;
} copp_ip2me_policer_addr_t;

typedef struct
{
  /* API message ID base */
  u16 msg_id_base;

  /* IPv4 addresses currently classified as IP2ME (router's own
   * addresses reachable via ip4-punt). Small, bounded, linear-scanned
   * array -- looked up once per packet in the data path, so kept
   * simple; COPP_IP2ME_POLICER_MAX_ADDRS comfortably covers every
   * router-interface address on this testbed's largest topology. */
  copp_ip2me_policer_addr_t addrs[COPP_IP2ME_POLICER_MAX_ADDRS];
  u32 n_addrs;

  /* Single policer binding -- the SAI ip2me trap's policer (shared by
   * IP2ME/SNMP/SSH; see PROTOCOL_TO_TRAP_ID in sonic-mgmt's
   * copp_tests.py -- all three trap to the same SAI_HOSTIF_TRAP_TYPE_
   * IP2ME). Looked up lazily by name the same way copp_punt_policer
   * does, so bind order relative to policer_add() doesn't matter. */
  u8 policer_name[COPP_IP2ME_POLICER_NAME_LEN];
  u32 policer_index;
  u8 policer_bound;

  /* conform/exceed/violate packet counters */
  u64 conform_packets;
  u64 exceed_packets;
  u64 violate_packets;

  vlib_main_t *vlib_main;
  vnet_main_t *vnet_main;
} copp_ip2me_policer_main_t;

extern copp_ip2me_policer_main_t copp_ip2me_policer_main;

extern vlib_node_registration_t copp_ip2me_policer_node;

#define COPP_IP2ME_POLICER_PLUGIN_BUILD_VER "1.0"

#endif /* __included_copp_ip2me_policer_h__ */
