/*
 * Copyright (c) 2026 Microsoft and/or its affiliates.
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

/*
 * sonic_fib_init: remove built-in FIB_SOURCE_SPECIAL drop entries that
 * conflict with SONiC datapath expectations.
 *
 * Background
 * ----------
 * VPP installs hard-coded drop entries into every IPv4 FIB at creation
 * time (see src/vnet/fib/ip4_fib.c :: ip4_specials[]). One of them is
 * 240.0.0.0/4 (the IPv4 "class E reserved" range) installed with
 * FIB_SOURCE_SPECIAL + FIB_ENTRY_FLAG_DROP. FIB_SOURCE_SPECIAL is the
 * highest-priority FIB source (FIB_SOURCE_FIRST in fib_source.h) and
 * cannot be overridden via the FIB API or `ip route` CLI.
 *
 * Forwarding decisions for these address ranges should be governed by
 * the routes SONiC programs into the FIB, not by a hard-coded drop in
 * VPP. Other SONiC platforms (BCM, MLNX) do not impose this filter, so
 * we strip the SPECIAL drop here to maintain cross-platform parity and
 * let SONiC-installed routes win the longest-prefix match.
 *
 * Implementation
 * --------------
 * fib_table_entry_special_remove() is C-only (not exposed by VPP's
 * built-in binary API), so the cleanup must run from inside VPP.
 *
 * - The default VRF (fib 0) is created during VPP startup before any
 *   external client connects, so we strip its SPECIAL drop once via
 *   VLIB_MAIN_LOOP_ENTER_FUNCTION (after all VLIB_INIT_FUNCTIONs, so
 *   the FIB and its SPECIAL entry are already in place).
 *
 * - For VRFs created at runtime (SAI virtual_router create routed
 *   through the SAI shim's `ip_table_add_del`), the shim invokes our
 *   `sonic_fib_strip_specials` binary API immediately after the table
 *   is created. The handler looks up the fib_index for the requested
 *   table_id and removes the SPECIAL drop.
 *
 * Note on 224.0.0.0/4: the matching multicast drop entry is left
 * intact intentionally; SONiC does not currently require forwarding
 * into that range and removing it could affect IGMP/PIM behaviour.
 */

#include <vnet/vnet.h>
#include <vnet/plugin/plugin.h>
#include <vnet/fib/fib_table.h>
#include <vpp/app/version.h>

#include <sonic_fib_init/sonic_fib_init.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

#include <sonic_fib_init/sonic_fib_init.api_enum.h>
#include <sonic_fib_init/sonic_fib_init.api_types.h>

#define REPLY_MSG_ID_BASE sm->msg_id_base
#include <vlibapi/api_helper_macros.h>

VLIB_PLUGIN_REGISTER () = {
  .version = SONIC_FIB_INIT_PLUGIN_BUILD_VER,
  .description = "SONiC FIB init: remove implicit class-E drop",
};

sonic_fib_init_main_t sonic_fib_init_main;

static void
sonic_fib_init_strip_v4 (u32 fib_index)
{
  fib_prefix_t pfx;

  /* IPv4: 240.0.0.0/4 (class E reserved) -- drop installed by
   * ip4_fib_hash_load_specials() with FIB_SOURCE_SPECIAL. */
  clib_memset (&pfx, 0, sizeof (pfx));
  pfx.fp_proto = FIB_PROTOCOL_IP4;
  pfx.fp_len = 4;
  pfx.fp_addr.ip4.data_u32 = clib_host_to_net_u32 (0xf0000000);
  fib_table_entry_special_remove (fib_index, &pfx, FIB_SOURCE_SPECIAL);
}

static void
vl_api_sonic_fib_strip_specials_t_handler (
  vl_api_sonic_fib_strip_specials_t *mp)
{
  sonic_fib_init_main_t *sm = &sonic_fib_init_main;
  vl_api_sonic_fib_strip_specials_reply_t *rmp;
  int rv = 0;
  u32 vrf_id = ntohl (mp->vrf_id);
  u32 fib_index;

  fib_index = fib_table_find (FIB_PROTOCOL_IP4, vrf_id);
  if (fib_index == ~0)
    {
      rv = VNET_API_ERROR_NO_SUCH_FIB;
      goto done;
    }

  sonic_fib_init_strip_v4 (fib_index);

done:
  REPLY_MACRO (VL_API_SONIC_FIB_STRIP_SPECIALS_REPLY);
}

/* API definitions */
#include <sonic_fib_init/sonic_fib_init.api.c>

static clib_error_t *
sonic_fib_init_init (vlib_main_t *vm)
{
  sonic_fib_init_main_t *sm = &sonic_fib_init_main;

  sm->msg_id_base = setup_message_id_table ();

  return 0;
}

VLIB_INIT_FUNCTION (sonic_fib_init_init);

static clib_error_t *
sonic_fib_init_main_loop_enter (vlib_main_t *vm)
{
  /* fib 0 is the default VRF, created during VPP startup. Additional
   * VRFs are handled via the sonic_fib_strip_specials API. */
  sonic_fib_init_strip_v4 (0);
  return 0;
}

VLIB_MAIN_LOOP_ENTER_FUNCTION (sonic_fib_init_main_loop_enter);
