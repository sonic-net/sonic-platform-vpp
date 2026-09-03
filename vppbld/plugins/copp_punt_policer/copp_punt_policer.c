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
 * copp_punt_policer: per-ethertype policing on the device-input arc.
 *
 * BACKGROUND (see sonic-net/sonic-buildimage#25801, SONiC-on-VPP CoPP HLD,
 * item 4/8): VPP's existing classify-based policer feature
 * (policer-classify) only runs on three feature arcs: l2-input (bridged
 * L2 traffic), ip4-unicast, ip6-unicast. On linux-cp-paired, L3-routed
 * ports (this project's topology), ARP/LACP/LLDP/UDLD traffic never
 * traverses any of those three arcs -- ethernet-input dispatches it
 * directly to arp-input/linux-cp-punt-xc, which punt straight to the
 * TAP. BGP/DHCP already work because they do ride ip4/ip6-unicast.
 *
 * This plugin closes that gap by registering a small feature node on
 * device-input, the one arc every packet on a port crosses before
 * ethernet-input's protocol dispatch runs. It parses just the 14-byte
 * Ethernet header (cheap -- no full packet classification), looks up
 * a per-ethertype policer binding, and applies VPP's existing
 * vnet_police_packet() token-bucket primitive against the existing SAI-
 * created VPP policer object (same policer objects SwitchVppPolicer.cpp
 * already creates correctly with the right CIR/CBS from copp_cfg.json).
 * No new metering implementation, no VPP core patch -- this is a
 * self-contained plugin, following the same out-of-tree pattern as this
 * repo's existing ip_validate/tunterm_acl plugins
 * (platform/vpp/vppbld/plugins/).
 */

#include <vnet/vnet.h>
#include <vnet/plugin/plugin.h>
#include <policer/policer.h>
#include <copp_punt_policer/copp_punt_policer.h>
#include <vppinfra/elog.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

#include <copp_punt_policer/copp_punt_policer.api_enum.h>
#include <copp_punt_policer/copp_punt_policer.api_types.h>

#define REPLY_MSG_ID_BASE sm->msg_id_base
#include <vlibapi/api_helper_macros.h>

VLIB_PLUGIN_REGISTER () = {
  .version = COPP_PUNT_POLICER_PLUGIN_BUILD_VER,
  .description = "CoPP punt policer (per-ethertype policing on device-input)",
};

copp_punt_policer_main_t copp_punt_policer_main;

/*
 * Find an existing entry for this ethertype, or -1 if none. Linear scan
 * over a small, bounded array (COPP_PUNT_POLICER_MAX_ENTRIES) -- this is
 * control-plane/config-time code, not the per-packet data path (see
 * copp_punt_policer_node.c for that).
 */
static int
copp_punt_policer_find_entry (copp_punt_policer_main_t *cpm, u16 ethertype)
{
  for (u32 i = 0; i < cpm->n_entries; i++)
    {
      if (cpm->entries[i].in_use && cpm->entries[i].ethertype == ethertype)
        return (int) i;
    }
  return -1;
}

/*
 * Bind (or unbind) an ethertype -> policer-name entry. On bind, the
 * named policer is looked up now via policer_main's
 * policer_index_by_name hash (populated by policer_add(), which
 * SwitchVppPolicer.cpp already calls for every SAI POLICER object) --
 * if the name isn't found yet, the entry is still recorded with
 * policer_index left unresolved (~0); the data-path node re-attempts
 * the lookup lazily so binding order relative to policer_add() doesn't
 * matter.
 *
 * match_ip4_ttl_expiring: when set (only meaningful with ethertype ==
 * 0x0800), the data-path node additionally requires the packet's IPv4
 * TTL to be <= 1 to match this entry -- this is how CoPP's TTL_ERROR
 * trap reuses this plugin's classify+police+direct-to-TAP mechanism,
 * without policing ordinary IPv4 traffic (BGP, DHCP, ...) that also
 * carries ethertype 0x0800.
 */
int
copp_punt_policer_bind (u16 ethertype, const char *policer_name,
                         int is_bind, int match_ip4_ttl_expiring)
{
  copp_punt_policer_main_t *cpm = &copp_punt_policer_main;
  int idx = copp_punt_policer_find_entry (cpm, ethertype);

  if (!is_bind)
    {
      if (idx < 0)
        return 0;
      clib_memset (&cpm->entries[idx], 0, sizeof (cpm->entries[idx]));
      cpm->conform_packets[idx] = 0;
      cpm->exceed_packets[idx] = 0;
      cpm->violate_packets[idx] = 0;
      return 0;
    }

  if (idx < 0)
    {
      if (cpm->n_entries >= COPP_PUNT_POLICER_MAX_ENTRIES)
        return VNET_API_ERROR_QUEUE_FULL;
      idx = (int) cpm->n_entries++;
    }

  clib_memset (&cpm->entries[idx], 0, sizeof (cpm->entries[idx]));
  cpm->entries[idx].ethertype = ethertype;
  snprintf ((char *) cpm->entries[idx].name,
            sizeof (cpm->entries[idx].name), "%s", policer_name);
  cpm->entries[idx].policer_index = ~0;
  cpm->entries[idx].in_use = 1;
  cpm->entries[idx].match_ip4_ttl_expiring = match_ip4_ttl_expiring ? 1 : 0;
  cpm->conform_packets[idx] = 0;
  cpm->exceed_packets[idx] = 0;
  cpm->violate_packets[idx] = 0;

  return 0;
}

static void
vl_api_copp_punt_policer_bind_t_handler (
    vl_api_copp_punt_policer_bind_t *mp)
{
  copp_punt_policer_main_t *sm = &copp_punt_policer_main;
  vl_api_copp_punt_policer_bind_reply_t *rmp;
  int rv;
  char name[64];

  snprintf (name, sizeof (name), "%s", mp->policer_name);
  rv = copp_punt_policer_bind (ntohs (mp->ethertype), name, mp->is_bind,
                                mp->match_ip4_ttl_expiring);

  REPLY_MACRO (VL_API_COPP_PUNT_POLICER_BIND_REPLY);
}

static void
vl_api_copp_punt_policer_get_counters_t_handler (
    vl_api_copp_punt_policer_get_counters_t *mp)
{
  copp_punt_policer_main_t *sm = &copp_punt_policer_main;
  vl_api_copp_punt_policer_get_counters_reply_t *rmp;
  int rv = 0;
  u64 conform = 0, exceed = 0, violate = 0;
  int idx = copp_punt_policer_find_entry (sm, ntohs (mp->ethertype));

  if (idx < 0)
    rv = VNET_API_ERROR_NO_SUCH_ENTRY;
  else
    {
      conform = sm->conform_packets[idx];
      exceed = sm->exceed_packets[idx];
      violate = sm->violate_packets[idx];
    }

  REPLY_MACRO2 (VL_API_COPP_PUNT_POLICER_GET_COUNTERS_REPLY,
  ({
    rmp->conform_packets = clib_host_to_net_u64 (conform);
    rmp->exceed_packets = clib_host_to_net_u64 (exceed);
    rmp->violate_packets = clib_host_to_net_u64 (violate);
  }));
}

/* API definitions */
#include <copp_punt_policer/copp_punt_policer.api.c>

static clib_error_t *
copp_punt_policer_init (vlib_main_t *vm)
{
  copp_punt_policer_main_t *cpm = &copp_punt_policer_main;

  cpm->msg_id_base = setup_message_id_table ();
  cpm->vlib_main = vm;
  cpm->vnet_main = vnet_get_main ();
  cpm->n_entries = 0;

  /*
   * One-shot sanity event, unconditional (no `elog trace`/`event-logger
   * trace` arming needed to see it). If it is present in `vppctl show
   * event-logger` after boot, the event-logger ring is live end-to-end
   * and any absence of per-packet copp-punt-policer events from the
   * data-path node (see copp_punt_policer_node.c) is a real "no packets
   * reached this node" finding, not a broken/disabled logger.
   */
  ELOG_TYPE_DECLARE (e) = {
    .format = "copp-punt-policer: plugin initialized, elog is live",
  };
  elog_main_t *em = vlib_get_elog_main ();
  ELOG (em, e, 0);

  return 0;
}

VLIB_INIT_FUNCTION (copp_punt_policer_init);

/*
 * Auto-enable the feature on every interface as it is created, exactly
 * like ip_validate does for ip4/ip6-unicast -- no explicit per-interface
 * API call needed. Bindings (which ethertypes map to which policers)
 * are configured separately via copp_punt_policer_bind above; this only
 * wires the *feature* onto device-input for the interface so the node
 * runs at all.
 */
static clib_error_t *
copp_punt_policer_sw_interface_add_del (vnet_main_t *vnm, u32 sw_if_index,
                                         u32 is_add)
{
  vnet_feature_enable_disable ("device-input", "copp-punt-policer",
                                sw_if_index, is_add, 0, 0);
  return 0;
}

VNET_SW_INTERFACE_ADD_DEL_FUNCTION (copp_punt_policer_sw_interface_add_del);

/* CLI: bind/unbind, for manual testing and for a scriptable non-API path */
static clib_error_t *
copp_punt_policer_bind_command_fn (vlib_main_t *vm,
                                    unformat_input_t *input,
                                    vlib_cli_command_t *cmd)
{
  u32 ethertype = 0;
  u8 *policer_name = 0;
  int is_bind = 1;
  int match_ip4_ttl_expiring = 0;
  clib_error_t *error = 0;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "ethertype 0x%x", &ethertype))
        ;
      else if (unformat (input, "ethertype %d", &ethertype))
        ;
      else if (unformat (input, "policer %s", &policer_name))
        ;
      else if (unformat (input, "match-ip4-ttl-expiring"))
        match_ip4_ttl_expiring = 1;
      else if (unformat (input, "del"))
        is_bind = 0;
      else
        {
          error = clib_error_return (0, "unknown input `%U'",
                                      format_unformat_error, input);
          goto done;
        }
    }

  if (ethertype == 0 || (is_bind && !policer_name))
    {
      error = clib_error_return (0, "usage: copp punt policer bind "
                                     "ethertype <0xNNNN> policer <name> "
                                     "[match-ip4-ttl-expiring] [del]");
      goto done;
    }

  {
    int rv = copp_punt_policer_bind ((u16) ethertype,
                                      policer_name ? (char *) policer_name : "",
                                      is_bind, match_ip4_ttl_expiring);
    if (rv)
      error = clib_error_return (0, "bind failed: rv %d", rv);
  }

done:
  vec_free (policer_name);
  return error;
}

VLIB_CLI_COMMAND (copp_punt_policer_bind_command, static) = {
  .path = "copp punt policer bind",
  .short_help = "copp punt policer bind ethertype <0xNNNN> policer <name> "
                "[match-ip4-ttl-expiring] [del]",
  .function = copp_punt_policer_bind_command_fn,
};

static clib_error_t *
copp_punt_policer_show_command_fn (vlib_main_t *vm,
                                    unformat_input_t *input,
                                    vlib_cli_command_t *cmd)
{
  copp_punt_policer_main_t *cpm = &copp_punt_policer_main;

  vlib_cli_output (vm, "%-8s %-40s %-12s %10s %10s %10s %s",
                    "ethtype", "policer-name", "vpp-idx",
                    "conform", "exceed", "violate", "match");
  for (u32 i = 0; i < cpm->n_entries; i++)
    {
      if (!cpm->entries[i].in_use)
        continue;
      vlib_cli_output (vm, "0x%04x   %-40s %-12d %10llu %10llu %10llu %s",
                        cpm->entries[i].ethertype, cpm->entries[i].name,
                        (i32) cpm->entries[i].policer_index,
                        cpm->conform_packets[i], cpm->exceed_packets[i],
                        cpm->violate_packets[i],
                        cpm->entries[i].match_ip4_ttl_expiring ?
                          "ip4-ttl<=1" : "-");
    }

  return 0;
}

VLIB_CLI_COMMAND (copp_punt_policer_show_command, static) = {
  .path = "show copp punt policer",
  .short_help = "show copp punt policer",
  .function = copp_punt_policer_show_command_fn,
};
