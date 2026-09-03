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
#ifndef __included_copp_punt_policer_h__
#define __included_copp_punt_policer_h__

#include <vnet/vnet.h>
#include <vnet/ethernet/packet.h>

/*
 * A single ethertype -> policer-name binding, keyed by the raw Ethernet
 * ethertype field.
 */
#define COPP_PUNT_POLICER_MAX_ENTRIES 16
#define COPP_PUNT_POLICER_NAME_LEN 64

typedef struct
{
  u16 ethertype;         /* host byte order */
  u8 name[COPP_PUNT_POLICER_NAME_LEN];
  u32 policer_index;      
  u8 in_use;
  u8 match_ip4_ttl_expiring; /* TTL_ERROR trap */
} copp_punt_policer_entry_t;

typedef struct
{
  /* API message ID base */
  u16 msg_id_base;

  /* ethertype -> policer binding table */
  copp_punt_policer_entry_t entries[COPP_PUNT_POLICER_MAX_ENTRIES];
  u32 n_entries;

  /* per-entry conform/exceed/violate packet counters */
  u64 conform_packets[COPP_PUNT_POLICER_MAX_ENTRIES];
  u64 exceed_packets[COPP_PUNT_POLICER_MAX_ENTRIES];
  u64 violate_packets[COPP_PUNT_POLICER_MAX_ENTRIES];

  vlib_main_t *vlib_main;
  vnet_main_t *vnet_main;
} copp_punt_policer_main_t;

extern copp_punt_policer_main_t copp_punt_policer_main;

extern vlib_node_registration_t copp_punt_policer_node;

#define COPP_PUNT_POLICER_PLUGIN_BUILD_VER "1.0"

#endif /* __included_copp_punt_policer_h__ */
