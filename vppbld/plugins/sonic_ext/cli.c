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
#include <sonic_ext/pbh.h>

#include <vlib/vlib.h>
#include <vppinfra/format.h>

static clib_error_t *
sonic_ext_punt_via_member_command_fn (vlib_main_t *vm,
				      unformat_input_t *input,
				      vlib_cli_command_t *cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "on") || unformat (line_input, "enable"))
	sonic_ext_set_punt_via_member (1);
      else if (unformat (line_input, "off") ||
	       unformat (line_input, "disable"))
	sonic_ext_set_punt_via_member (0);
      else
	{
	  unformat_free (line_input);
	  return clib_error_return (0, "unknown input `%U'",
				    format_unformat_error, line_input);
	}
    }

  unformat_free (line_input);
  return 0;
}

VLIB_CLI_COMMAND (sonic_ext_punt_via_member_command, static) = {
  .path = "sonic-ext punt-via-member",
  .short_help = "sonic-ext punt-via-member [on|enable|off|disable]",
  .function = sonic_ext_punt_via_member_command_fn,
};

static clib_error_t *
sonic_ext_host_xc_command_fn (vlib_main_t *vm, unformat_input_t *input,
			      vlib_cli_command_t *cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "on") || unformat (line_input, "enable"))
	sonic_ext_set_host_xc (1);
      else if (unformat (line_input, "off") ||
	       unformat (line_input, "disable"))
	sonic_ext_set_host_xc (0);
      else
	{
	  unformat_free (line_input);
	  return clib_error_return (0, "unknown input `%U'",
				    format_unformat_error, line_input);
	}
    }

  unformat_free (line_input);
  return 0;
}

VLIB_CLI_COMMAND (sonic_ext_host_xc_command, static) = {
  .path = "sonic-ext host-xc",
  .short_help = "sonic-ext host-xc [on|enable|off|disable]",
  .function = sonic_ext_host_xc_command_fn,
};

static clib_error_t *
sonic_ext_ip2me_command_fn (vlib_main_t *vm, unformat_input_t *input,
			    vlib_cli_command_t *cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  vnet_main_t *vnm = vnet_get_main ();
  u32 sw_if_index = ~0;
  int enable = 1;
  int got_enable = 0;
  clib_error_t *error = 0;

  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "%U", unformat_vnet_sw_interface, vnm,
		    &sw_if_index))
	;
      else if (unformat (line_input, "on") ||
	       unformat (line_input, "enable"))
	{
	  enable = 1;
	  got_enable = 1;
	}
      else if (unformat (line_input, "off") ||
	       unformat (line_input, "disable"))
	{
	  enable = 0;
	  got_enable = 1;
	}
      else
	{
	  error = clib_error_return (0, "unknown input `%U'",
				     format_unformat_error, line_input);
	  goto done;
	}
    }

  if (sw_if_index == ~0)
    {
      error = clib_error_return (0, "please specify an interface");
      goto done;
    }
  if (!got_enable)
    {
      error = clib_error_return (0, "please specify on|off");
      goto done;
    }

  sonic_ext_ip2me_enable_disable (sw_if_index, enable);

done:
  unformat_free (line_input);
  return error;
}

VLIB_CLI_COMMAND (sonic_ext_ip2me_command, static) = {
  .path = "sonic-ext ip2me",
  .short_help = "sonic-ext ip2me <interface> <on|enable|off|disable>",
  .function = sonic_ext_ip2me_command_fn,
};

static clib_error_t *
show_sonic_ext_command_fn (vlib_main_t *vm, unformat_input_t *input,
			   vlib_cli_command_t *cmd)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vlib_cli_output (vm, "sonic-ext state:");
#define _(symbol, field, name, default_enabled, owner)                        \
  if (SONIC_EXT_OWNER_##owner == SONIC_EXT_OWNER_VPP)                         \
    vlib_cli_output (vm, "  %-17s : %s", name, sem->field ? "on" : "off");
  foreach_sonic_ext_feature
#undef _
  /* Derived from the two cookie consumers above, so report the latch. */
  vlib_cli_output (vm, "  capture (derived) : %s",
		   sem->capture_enabled ? "on" : "off");
  /* Stored for saivpp, which wires these; arc membership does not reflect them. */
  vlib_cli_output (vm, "  -- saivpp-wired --");
#define _(symbol, field, name, default_enabled, owner)                        \
  if (SONIC_EXT_OWNER_##owner == SONIC_EXT_OWNER_SAIVPP)                      \
    vlib_cli_output (vm, "  %-17s : %s", name, sem->field ? "on" : "off");
  foreach_sonic_ext_feature
#undef _
  vlib_cli_output (vm, "  -- counters --");
  vlib_cli_output (vm, "  captures          : %llu", sem->captures);
  vlib_cli_output (vm, "  aggr-tap redir    : %llu", sem->aggr_tap_redirects);
  vlib_cli_output (vm, "  glean redirect    : %llu", sem->glean_redirects);
  vlib_cli_output (vm, "  host-xc direct    : %llu", sem->host_xc_direct);
  vlib_cli_output (vm, "  l2 trap fixups    : %llu", sem->l2_trap_fixups);
  vlib_cli_output (vm, "  ip2me hits        : %llu", sem->ip2me_hits);
  return 0;
}

VLIB_CLI_COMMAND (show_sonic_ext_command, static) = {
  .path = "show sonic-ext",
  .short_help = "show sonic-ext",
  .function = show_sonic_ext_command_fn,
};

static clib_error_t *
show_sonic_ext_mirror_encap_command_fn (vlib_main_t *vm,
					unformat_input_t *input,
					vlib_cli_command_t *cmd)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vnet_main_t *vnm = vnet_get_main ();
  u32 i;

  vlib_cli_output (vm, "sonic-ext mirror-encap fixups: %llu",
		   sem->mirror_encap_fixups);
  for (i = 0; i < vec_len (sem->mirror_encap_cfg); i++)
    {
      sonic_ext_mirror_encap_cfg_t *c =
	vec_elt_at_index (sem->mirror_encap_cfg, i);
      if (!c->enabled)
	continue;
      vlib_cli_output (vm, "  %U: gre-protocol 0x%04x ttl %u",
		       format_vnet_sw_if_index_name, vnm, i, c->gre_protocol,
		       c->ttl);
    }
  return 0;
}

VLIB_CLI_COMMAND (show_sonic_ext_mirror_encap_command, static) = {
  .path = "show sonic-ext mirror-encap",
  .short_help = "show sonic-ext mirror-encap",
  .function = show_sonic_ext_mirror_encap_command_fn,
};

static clib_error_t *
show_sonic_ext_pbh_command_fn (vlib_main_t *vm, unformat_input_t *input,
			       vlib_cli_command_t *cmd)
{
  sonic_ext_pbh_main_t *pm = &sonic_ext_pbh_main;
  sonic_ext_pbh_profile_t *p;
  sonic_ext_pbh_table_t *t;
  vnet_main_t *vnm = vnet_get_main ();
  int show_all = 1, profiles = 0, tables = 0, interfaces = 0;
  u32 i;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "profiles"))
	profiles = 1, show_all = 0;
      else if (unformat (input, "tables"))
	tables = 1, show_all = 0;
      else if (unformat (input, "interfaces"))
	interfaces = 1, show_all = 0;
      else
	return clib_error_return (0, "unknown input `%U'",
				  format_unformat_error, input);
    }

  if (!sonic_ext_main.pbh)
    {
      vlib_cli_output (vm, "pbh is disabled in startup.conf");
      return 0;
    }

  if (show_all || profiles)
    {
      vlib_cli_output (vm, "hash profiles:");
      pool_foreach (p, pm->profiles)
	vlib_cli_output (vm, "  [%u] %U", p - pm->profiles,
			 format_sonic_ext_pbh_profile, p);
    }

  if (show_all || tables)
    {
      vlib_cli_output (vm, "tables:");
      pool_foreach (t, pm->tables)
	{
	  u32 ti = t - pm->tables;
	  sonic_ext_pbh_rule_t *r;

	  vlib_cli_output (vm, "  [%u] %v  (%u rule%s, %u interface%s)", ti,
			   t->name, vec_len (t->rules),
			   vec_len (t->rules) == 1 ? "" : "s",
			   vec_len (t->sw_if_indices),
			   vec_len (t->sw_if_indices) == 1 ? "" : "s");

	  vec_foreach (r, t->rules)
	    {
	      vlib_counter_t c;

	      vlib_get_combined_counter (&t->counters, r - t->rules, &c);
	      vlib_cli_output (vm, "    %U\n      matches %llu, %llu bytes",
			       format_sonic_ext_pbh_rule, r, c.packets,
			       c.bytes);
	    }
	}
    }

  if (show_all || interfaces)
    {
      vlib_cli_output (vm, "interfaces:");
      for (i = 0; i < vec_len (pm->table_index_by_sw_if_index); i++)
	{
	  u32 refs = i < vec_len (pm->bvi_refcount_by_sw_if_index)
		       ? pm->bvi_refcount_by_sw_if_index[i]
		       : 0;

	  if (pm->table_index_by_sw_if_index[i] == ~0)
	    continue;

	  if (refs)
	    vlib_cli_output (
	      vm, "  %U: table %u (bridge domain shadow, %u member%s)",
	      format_vnet_sw_if_index_name, vnm, i,
	      pm->table_index_by_sw_if_index[i], refs, refs == 1 ? "" : "s");
	  else
	    vlib_cli_output (vm, "  %U: table %u",
			     format_vnet_sw_if_index_name, vnm, i,
			     pm->table_index_by_sw_if_index[i]);
	}
    }

  if (show_all)
    vlib_cli_output (vm, "hits %llu, misses %llu", pm->hits, pm->misses);

  return 0;
}

VLIB_CLI_COMMAND (show_sonic_ext_pbh_command, static) = {
  .path = "show sonic-ext pbh",
  .short_help = "show sonic-ext pbh [profiles|tables|interfaces]",
  .function = show_sonic_ext_pbh_command_fn,
};
