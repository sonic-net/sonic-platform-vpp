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
/** @file MACsec debug CLI. Never prints SAK or salt. */

#include <sonic_ext/macsec.h>

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vppinfra/format.h>

static const char *
sonic_ext_macsec_cipher_name (u8 cipher)
{
  switch (cipher)
    {
    case SONIC_EXT_MACSEC_CS_GCM_AES_128:
      return "gcm-aes-128";
    case SONIC_EXT_MACSEC_CS_GCM_AES_256:
      return "gcm-aes-256";
    case SONIC_EXT_MACSEC_CS_GCM_AES_XPN_128:
      return "gcm-aes-xpn-128";
    case SONIC_EXT_MACSEC_CS_GCM_AES_XPN_256:
      return "gcm-aes-xpn-256";
    default:
      return "unknown";
    }
}

static u64
sonic_ext_macsec_sa_stat (u32 sa_index, u32 stat)
{
  vlib_simple_counter_main_t *cm = &sonic_ext_macsec_sa_stats[stat];

  if (!cm->counters)
    return 0;
  if (sa_index >= vlib_simple_counter_n_counters (cm))
    return 0;
  return vlib_get_simple_counter (cm, sa_index);
}

static void
sonic_ext_macsec_show_sa (vlib_main_t *vm, sonic_ext_macsec_sc_t *sc,
			  sonic_ext_macsec_sa_t *sa, u32 sa_index)
{
  vlib_cli_output (
    vm,
    "    sa %u an %u sak-len %u cipher %s xpn-en %u pn %llu lowest %llu "
    "out-enc %llu in-ok %llu octets-enc %llu in-octets-dec %llu "
    "in-not-valid %llu in-late %llu",
    sa_index, sa->an, sa->sak_len, sonic_ext_macsec_cipher_name (sc->cipher),
    sa->xpn_en, (unsigned long long) sa->next_xpn,
    (unsigned long long) sa->lowest_acceptable_xpn,
    (unsigned long long) sonic_ext_macsec_sa_stat (
      sa_index, SONIC_EXT_MACSEC_STAT_OUT_PKTS_ENCRYPTED),
    (unsigned long long) sonic_ext_macsec_sa_stat (
      sa_index, SONIC_EXT_MACSEC_STAT_IN_PKTS_OK),
    (unsigned long long) sonic_ext_macsec_sa_stat (
      sa_index, SONIC_EXT_MACSEC_STAT_OCTETS_ENCRYPTED),
    (unsigned long long) sonic_ext_macsec_sa_stat (
      sa_index, SONIC_EXT_MACSEC_STAT_IN_OCTETS_DECRYPTED),
    (unsigned long long) sonic_ext_macsec_sa_stat (
      sa_index, SONIC_EXT_MACSEC_STAT_IN_PKTS_NOT_VALID),
    (unsigned long long) sonic_ext_macsec_sa_stat (
      sa_index, SONIC_EXT_MACSEC_STAT_IN_PKTS_LATE));
}

static void
sonic_ext_macsec_show_port (vlib_main_t *vm, vnet_main_t *vnm,
			    sonic_ext_macsec_port_t *port)
{
  sonic_ext_macsec_sc_t *sc;
  u32 an;

  if (!port || !port->enabled)
    return;

  vlib_cli_output (vm, "  %U %s %s send-sci %u replay %u window %u",
		   format_vnet_sw_if_index_name, vnm, port->sw_if_index,
		   port->direction == SONIC_EXT_MACSEC_INGRESS ? "ingress" :
								       "egress",
		   port->encrypt ? "encrypt" : "integrity", port->send_sci,
		   port->replay_protect, port->replay_window);

  sc = sonic_ext_macsec_sc_get (port->sc_index);
  if (!sc)
    {
      vlib_cli_output (vm, "    (no SC)");
      return;
    }

  {
    u8 ieee[8];

    sonic_ext_macsec_sci_store (ieee, sc->sci);
    vlib_cli_output (
      vm,
      "    sc 0x%016llx ieee %02x%02x%02x%02x%02x%02x%02x%02x cipher %s "
      "encoding-an %u",
      (unsigned long long) sc->sci, ieee[0], ieee[1], ieee[2], ieee[3], ieee[4],
      ieee[5], ieee[6], ieee[7], sonic_ext_macsec_cipher_name (sc->cipher),
      sc->encoding_an);
  }

  for (an = 0; an < SONIC_EXT_MACSEC_AN_MAX; an++)
    {
      sonic_ext_macsec_sa_t *sa = sonic_ext_macsec_sa_get (sc->sa_index[an]);
      if (sa)
	sonic_ext_macsec_show_sa (vm, sc, sa, sc->sa_index[an]);
    }
}

static clib_error_t *
show_macsec_command_fn (vlib_main_t *vm, unformat_input_t *input,
			vlib_cli_command_t *cmd)
{
  sonic_ext_macsec_main_t *mm = &sonic_ext_macsec_main;
  vnet_main_t *vnm = vnet_get_main ();
  u32 sw_if_index = ~0;
  int show_sa = 0;
  u8 dir;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "interface %U", unformat_vnet_sw_interface, vnm,
		    &sw_if_index))
	;
      else if (unformat (input, "sa"))
	show_sa = 1;
      else
	return clib_error_return (0, "unknown input `%U'", format_unformat_error,
				  input);
    }

  if (!sonic_ext_main.macsec)
    {
      vlib_cli_output (vm, "macsec: off");
      return 0;
    }

  vlib_cli_output (vm, "macsec: on");

  if (show_sa)
    {
      sonic_ext_macsec_sa_t *sa;
      pool_foreach (sa, mm->sas)
	{
	  sonic_ext_macsec_sc_t *sc = sonic_ext_macsec_sc_get (sa->sc_index);
	  if (sc)
	    sonic_ext_macsec_show_sa (vm, sc, sa, sa - mm->sas);
	}
      return 0;
    }

  if (sw_if_index != ~0)
    {
      for (dir = 0; dir < SONIC_EXT_MACSEC_N_DIR; dir++)
	sonic_ext_macsec_show_port (vm, vnm,
				    sonic_ext_macsec_port_get (sw_if_index, dir));
      return 0;
    }

  for (dir = 0; dir < SONIC_EXT_MACSEC_N_DIR; dir++)
    {
      u32 i;
      for (i = 0; i < vec_len (mm->ports[dir]); i++)
	sonic_ext_macsec_show_port (vm, vnm, &mm->ports[dir][i]);
    }
  return 0;
}

VLIB_CLI_COMMAND (show_macsec_command, static) = {
  .path = "show macsec",
  .short_help = "show macsec [interface <intf>] [sa]",
  .function = show_macsec_command_fn,
};

static clib_error_t *
clear_macsec_counters_command_fn (vlib_main_t *vm, unformat_input_t *input,
				  vlib_cli_command_t *cmd)
{
  vnet_main_t *vnm = vnet_get_main ();
  sonic_ext_macsec_main_t *mm = &sonic_ext_macsec_main;
  u32 sw_if_index = ~0;
  sonic_ext_macsec_sa_t *sa;

  (void) vm;
  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "interface %U", unformat_vnet_sw_interface, vnm,
		    &sw_if_index))
	;
      else
	return clib_error_return (0, "unknown input `%U'", format_unformat_error,
				  input);
    }

  if (sw_if_index == ~0)
    {
      sonic_ext_macsec_clear_stats_all ();
      return 0;
    }

  pool_foreach (sa, mm->sas)
    {
      sonic_ext_macsec_sc_t *sc = sonic_ext_macsec_sc_get (sa->sc_index);
      if (sc && sc->port_index == sw_if_index)
	sonic_ext_macsec_sa_stats_zero (sa - mm->sas);
    }
  return 0;
}

VLIB_CLI_COMMAND (clear_macsec_counters_command, static) = {
  .path = "clear macsec counters",
  .short_help = "clear macsec counters [interface <intf>]",
  .function = clear_macsec_counters_command_fn,
};
