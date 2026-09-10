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
 *
 * copp_ip2me_policer: CoPP enforcement for IP2ME/SNMP/SSH traffic --
 * traffic destined to one of the router's own IPv4 addresses that VPP's
 * dataplane does not answer itself (see sonic-net/sonic-buildimage#25801,
 * SONiC-on-VPP CoPP HLD).
 *
 * BACKGROUND: VPP's built-in classify-based policer feature
 * (ip4-policer-classify, on the ip4-unicast arc) only meters traffic on
 * whichever interface it has been explicitly bound to. Since the
 * classify table matches purely on destination IP, IP2ME traffic
 * destined to router-interface A's address can legitimately arrive on
 * router-interface B -- if B never had the feature bound, that traffic
 * skips policing entirely and reaches VPP's ip4-punt-redirect mechanism
 * completely unpoliced. Binding to every possible L3 ingress interface
 * is the fix VPP's own classify feature requires, but it needs lazy,
 * per-RIF VAPI calls (deferred to avoid syncd's SAI-call watchdog) and
 * has proven fragile in practice.
 *
 * This plugin avoids the whole binding-scope problem by living on
 * ip4-punt instead: a single, always-on, global feature arc that every
 * packet reaching this point has already been routed through
 * (ip4-lookup -> ip4-local -> ip4-punt), regardless of which interface
 * it arrived on. No per-interface binding is needed -- ip4-punt is
 * reached the same way no matter the ingress interface, once VPP's own
 * dataplane has already concluded "nothing here handles this packet, it
 * needs the host." That is exactly SAI's IP2ME semantics, so this
 * plugin only needs to answer one question (is the destination address
 * one we're tracking?) and apply the existing SAI-created policer
 * object, then let the packet continue unchanged to ip4-punt-redirect
 * (conform) or drop it (exceed/violate) -- it never needs to redirect
 * to a TAP itself, unlike copp_punt_policer's device-input node, since
 * ip4-punt-redirect already does that for every packet that reaches it.
 */

#include <vnet/vnet.h>
#include <vnet/plugin/plugin.h>
#include <policer/policer.h>
#include <copp_ip2me_policer/copp_ip2me_policer.h>
#include <vppinfra/elog.h>
#include <vnet/ip/ip4_packet.h>
#include <vnet/ip/format.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

#include <copp_ip2me_policer/copp_ip2me_policer.api_enum.h>
#include <copp_ip2me_policer/copp_ip2me_policer.api_types.h>

#define REPLY_MSG_ID_BASE sm->msg_id_base
#include <vlibapi/api_helper_macros.h>

VLIB_PLUGIN_REGISTER () = {
  .version = COPP_IP2ME_POLICER_PLUGIN_BUILD_VER,
  .description = "CoPP IP2ME/SNMP/SSH policing on the ip4-punt arc",
};

copp_ip2me_policer_main_t copp_ip2me_policer_main;

/*
 * Find an existing entry for this address, or -1 if none. Linear scan
 * over a small, bounded array (COPP_IP2ME_POLICER_MAX_ADDRS) -- this is
 * control-plane/config-time code, not the per-packet data path (see
 * copp_ip2me_policer_node.c for that).
 */
static int
copp_ip2me_policer_find_addr (copp_ip2me_policer_main_t *cpm, u32 addr)
{
  for (u32 i = 0; i < cpm->n_addrs; i++)
    {
      if (cpm->addrs[i].in_use && cpm->addrs[i].addr == addr)
        return (int) i;
    }
  return -1;
}

int
copp_ip2me_policer_addr_add_del (u32 addr, int is_add)
{
  copp_ip2me_policer_main_t *cpm = &copp_ip2me_policer_main;
  int idx = copp_ip2me_policer_find_addr (cpm, addr);

  if (!is_add)
    {
      if (idx < 0)
        return 0;
      clib_memset (&cpm->addrs[idx], 0, sizeof (cpm->addrs[idx]));
      return 0;
    }

  if (idx >= 0)
    return 0; /* already present */

  if (cpm->n_addrs >= COPP_IP2ME_POLICER_MAX_ADDRS)
    return VNET_API_ERROR_QUEUE_FULL;

  idx = (int) cpm->n_addrs++;
  cpm->addrs[idx].addr = addr;
  cpm->addrs[idx].in_use = 1;

  return 0;
}

int
copp_ip2me_policer_bind (const char *policer_name, int is_bind)
{
  copp_ip2me_policer_main_t *cpm = &copp_ip2me_policer_main;

  if (!is_bind)
    {
      clib_memset (cpm->policer_name, 0, sizeof (cpm->policer_name));
      cpm->policer_index = ~0;
      cpm->policer_bound = 0;
      cpm->conform_packets = 0;
      cpm->exceed_packets = 0;
      cpm->violate_packets = 0;
      return 0;
    }

  snprintf ((char *) cpm->policer_name, sizeof (cpm->policer_name), "%s",
            policer_name);
  cpm->policer_index = ~0;
  cpm->policer_bound = 1;
  cpm->conform_packets = 0;
  cpm->exceed_packets = 0;
  cpm->violate_packets = 0;

  return 0;
}

static void
vl_api_copp_ip2me_policer_addr_add_del_t_handler (
    vl_api_copp_ip2me_policer_addr_add_del_t *mp)
{
  copp_ip2me_policer_main_t *sm = &copp_ip2me_policer_main;
  vl_api_copp_ip2me_policer_addr_add_del_reply_t *rmp;
  int rv;

  rv = copp_ip2me_policer_addr_add_del (mp->addr, mp->is_add);

  REPLY_MACRO (VL_API_COPP_IP2ME_POLICER_ADDR_ADD_DEL_REPLY);
}

static void
vl_api_copp_ip2me_policer_bind_t_handler (
    vl_api_copp_ip2me_policer_bind_t *mp)
{
  copp_ip2me_policer_main_t *sm = &copp_ip2me_policer_main;
  vl_api_copp_ip2me_policer_bind_reply_t *rmp;
  int rv;
  char name[64];

  snprintf (name, sizeof (name), "%s", mp->policer_name);
  rv = copp_ip2me_policer_bind (name, mp->is_bind);

  REPLY_MACRO (VL_API_COPP_IP2ME_POLICER_BIND_REPLY);
}

static void
vl_api_copp_ip2me_policer_get_counters_t_handler (
    vl_api_copp_ip2me_policer_get_counters_t *mp)
{
  copp_ip2me_policer_main_t *sm = &copp_ip2me_policer_main;
  vl_api_copp_ip2me_policer_get_counters_reply_t *rmp;
  int rv = 0;

  REPLY_MACRO2 (VL_API_COPP_IP2ME_POLICER_GET_COUNTERS_REPLY,
  ({
    rmp->conform_packets = clib_host_to_net_u64 (sm->conform_packets);
    rmp->exceed_packets = clib_host_to_net_u64 (sm->exceed_packets);
    rmp->violate_packets = clib_host_to_net_u64 (sm->violate_packets);
  }));
}

/* API definitions */
#include <copp_ip2me_policer/copp_ip2me_policer.api.c>

static clib_error_t *
copp_ip2me_policer_init (vlib_main_t *vm)
{
  copp_ip2me_policer_main_t *cpm = &copp_ip2me_policer_main;

  cpm->msg_id_base = setup_message_id_table ();
  cpm->vlib_main = vm;
  cpm->vnet_main = vnet_get_main ();
  cpm->n_addrs = 0;
  cpm->policer_index = ~0;
  cpm->policer_bound = 0;

  /*
   * ip4-punt is a global feature arc, not per-interface -- enable this
   * feature on it ONCE at init, unconditionally, the same way
   * ip4-punt-redirect itself is always enabled. No per-interface
   * enable/disable call is needed or ever made (see
   * copp_punt_policer_sw_interface_add_del in the sibling
   * copp_punt_policer plugin for contrast -- that one has to do this
   * per-interface because device-input is a per-interface arc; ip4-punt
   * is not).
   */
  vnet_feature_enable_disable ("ip4-punt", "copp-ip2me-policer", 0, 1, 0, 0);

  return 0;
}

VLIB_INIT_FUNCTION (copp_ip2me_policer_init);

/* CLI: bind/unbind policer, for manual testing and a scriptable non-API path */
static clib_error_t *
copp_ip2me_policer_bind_command_fn (vlib_main_t *vm,
                                     unformat_input_t *input,
                                     vlib_cli_command_t *cmd)
{
  u8 *policer_name = 0;
  int is_bind = 1;
  clib_error_t *error = 0;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "policer %s", &policer_name))
        ;
      else if (unformat (input, "del"))
        is_bind = 0;
      else
        {
          error = clib_error_return (0, "unknown input `%U'",
                                      format_unformat_error, input);
          goto done;
        }
    }

  if (is_bind && !policer_name)
    {
      error = clib_error_return (0, "usage: copp ip2me policer bind "
                                     "policer <name> [del]");
      goto done;
    }

  {
    int rv = copp_ip2me_policer_bind (
        policer_name ? (char *) policer_name : "", is_bind);
    if (rv)
      error = clib_error_return (0, "bind failed: rv %d", rv);
  }

done:
  vec_free (policer_name);
  return error;
}

VLIB_CLI_COMMAND (copp_ip2me_policer_bind_command, static) = {
  .path = "copp ip2me policer bind",
  .short_help = "copp ip2me policer bind policer <name> [del]",
  .function = copp_ip2me_policer_bind_command_fn,
};

/* CLI: add/remove an IP2ME address, for manual testing */
static clib_error_t *
copp_ip2me_policer_addr_command_fn (vlib_main_t *vm,
                                     unformat_input_t *input,
                                     vlib_cli_command_t *cmd)
{
  ip4_address_t addr;
  int have_addr = 0;
  int is_add = 1;
  clib_error_t *error = 0;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "%U", unformat_ip4_address, &addr))
        have_addr = 1;
      else if (unformat (input, "del"))
        is_add = 0;
      else
        {
          error = clib_error_return (0, "unknown input `%U'",
                                      format_unformat_error, input);
          goto done;
        }
    }

  if (!have_addr)
    {
      error = clib_error_return (0, "usage: copp ip2me policer addr "
                                     "<ip4-address> [del]");
      goto done;
    }

  {
    int rv = copp_ip2me_policer_addr_add_del (addr.as_u32, is_add);
    if (rv)
      error = clib_error_return (0, "addr add/del failed: rv %d", rv);
  }

done:
  return error;
}

VLIB_CLI_COMMAND (copp_ip2me_policer_addr_command, static) = {
  .path = "copp ip2me policer addr",
  .short_help = "copp ip2me policer addr <ip4-address> [del]",
  .function = copp_ip2me_policer_addr_command_fn,
};

static clib_error_t *
copp_ip2me_policer_show_command_fn (vlib_main_t *vm,
                                     unformat_input_t *input,
                                     vlib_cli_command_t *cmd)
{
  copp_ip2me_policer_main_t *cpm = &copp_ip2me_policer_main;

  vlib_cli_output (vm, "policer: %s (index %d) bound=%d",
                    cpm->policer_bound ? (char *) cpm->policer_name : "-",
                    (i32) cpm->policer_index, cpm->policer_bound);
  vlib_cli_output (vm, "conform %llu exceed %llu violate %llu",
                    cpm->conform_packets, cpm->exceed_packets,
                    cpm->violate_packets);
  vlib_cli_output (vm, "%-8s addresses:", "count");
  vlib_cli_output (vm, "%u", cpm->n_addrs);
  for (u32 i = 0; i < cpm->n_addrs; i++)
    {
      if (!cpm->addrs[i].in_use)
        continue;
      vlib_cli_output (vm, "  %U", format_ip4_address, &cpm->addrs[i].addr);
    }

  return 0;
}

VLIB_CLI_COMMAND (copp_ip2me_policer_show_command, static) = {
  .path = "show copp ip2me policer",
  .short_help = "show copp ip2me policer",
  .function = copp_ip2me_policer_show_command_fn,
};
