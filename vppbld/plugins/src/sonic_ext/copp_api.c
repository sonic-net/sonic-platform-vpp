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

#include <sonic_ext/sonic_ext.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

#include <sonic_ext/sonic_ext_copp.api_enum.h>
#include <sonic_ext/sonic_ext_copp.api_types.h>

#define REPLY_MSG_ID_BASE sonic_ext_main.copp_msg_id_base
#include <vlibapi/api_helper_macros.h>

static void
vl_api_sonic_ext_copp_neighbor_miss_bind_t_handler (
  vl_api_sonic_ext_copp_neighbor_miss_bind_t *mp)
{
  vl_api_sonic_ext_copp_neighbor_miss_bind_reply_t *rmp;
  int rv;
  char name[65];

  clib_memcpy (name, mp->policer_name, sizeof (mp->policer_name));
  name[sizeof (mp->policer_name)] = 0;
  rv = sonic_ext_copp_neighbor_miss_bind (name, mp->is_bind);

  REPLY_MACRO (VL_API_SONIC_EXT_COPP_NEIGHBOR_MISS_BIND_REPLY);
}

#include <sonic_ext/sonic_ext_copp.api.c>

clib_error_t *
sonic_ext_copp_api_hookup (vlib_main_t *vm)
{
  sonic_ext_main.copp_msg_id_base = setup_message_id_table ();
  return 0;
}
