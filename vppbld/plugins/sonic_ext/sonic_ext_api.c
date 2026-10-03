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
/**
 * @file
 * @brief sonic_ext plugin binary API.
 *
 * Exposes sonic_ext_ip2me_enable_disable so the SAI-VPP layer
 * (sonic-sairedis) can enable the "receive-DPO check before ACL" feature
 * on exactly the L2 ports where an ingress drop ACL that could discard
 * ip2me traffic is bound.
 *
 * Exposes sonic_ext_feature_get so the same layer can read the startup.conf
 * selection for the features it wires itself, and skip installing them.
 */

#include <vnet/vnet.h>
#include <vnet/plugin/plugin.h>
#include <sonic_ext/sonic_ext.h>
#include <sonic_ext/pbh.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

#include <sonic_ext/sonic_ext.api_enum.h>
#include <sonic_ext/sonic_ext.api_types.h>

#define REPLY_MSG_ID_BASE sonic_ext_main.msg_id_base
#include <vlibapi/api_helper_macros.h>

static void
vl_api_sonic_ext_ip2me_enable_disable_t_handler (
  vl_api_sonic_ext_ip2me_enable_disable_t *mp)
{
  vnet_interface_main_t *im = &vnet_get_main ()->interface_main;
  vl_api_sonic_ext_ip2me_enable_disable_reply_t *rmp;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  int rv = 0;

  if (pool_is_free_index (im->sw_interfaces, sw_if_index))
    {
      rv = VNET_API_ERROR_INVALID_SW_IF_INDEX;
      goto exit;
    }

  sonic_ext_ip2me_enable_disable (sw_if_index, mp->enable ? 1 : 0);

exit:
  REPLY_MACRO (VL_API_SONIC_EXT_IP2ME_ENABLE_DISABLE_REPLY);
}

static void
vl_api_sonic_ext_egress_mirror_enable_disable_t_handler (
  vl_api_sonic_ext_egress_mirror_enable_disable_t *mp)
{
  vl_api_sonic_ext_egress_mirror_enable_disable_reply_t *rmp;
  int rv = sonic_ext_egress_mirror_enable_disable (mp->enable ? 1 : 0);

  REPLY_MACRO (VL_API_SONIC_EXT_EGRESS_MIRROR_ENABLE_DISABLE_REPLY);
}

static void
vl_api_sonic_ext_mirror_encap_fixup_enable_disable_t_handler (
  vl_api_sonic_ext_mirror_encap_fixup_enable_disable_t *mp)
{
  vnet_interface_main_t *im = &vnet_get_main ()->interface_main;
  vl_api_sonic_ext_mirror_encap_fixup_enable_disable_reply_t *rmp;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  int rv = 0;

  if (pool_is_free_index (im->sw_interfaces, sw_if_index))
    {
      rv = VNET_API_ERROR_INVALID_SW_IF_INDEX;
      goto exit;
    }

  rv = sonic_ext_mirror_encap_fixup_enable_disable (
    sw_if_index, ntohs (mp->gre_protocol), mp->hop_limit,
    mp->enable ? 1 : 0);

exit:
  REPLY_MACRO (VL_API_SONIC_EXT_MIRROR_ENCAP_FIXUP_ENABLE_DISABLE_REPLY);
}

static void
vl_api_sonic_ext_feature_get_t_handler (vl_api_sonic_ext_feature_get_t *mp)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vl_api_sonic_ext_feature_get_reply_t *rmp;
  /* Fixed-width field: a caller that fills all 64 bytes leaves no NUL, so copy
   * out and terminate rather than trusting the wire to be a C string. */
  char name[sizeof (mp->feature) + 1];
  /* An unrecognized feature defaults to disabled */
  u8 enabled = 0;
  int rv = 0;

  clib_memcpy (name, mp->feature, sizeof (mp->feature));
  name[sizeof (mp->feature)] = 0;

#define _(symbol, field, str, default_enabled, owner)                         \
  if (!strcmp (name, str))                                                    \
    enabled = sem->field;                                                     \
  else
  foreach_sonic_ext_feature
#undef _
  if (!strcmp (name, "capture"))
    enabled = sem->capture_enabled;

  REPLY_MACRO2 (VL_API_SONIC_EXT_FEATURE_GET_REPLY,
		({ rmp->enabled = enabled; }));
}

/*
 * Policy Based Hashing.
 *
 * Handlers live here rather than beside pbh.c because the generated glue
 * below resolves every handler by name within this translation unit.  They
 * only unmarshal and validate shape; everything semantic is in pbh.c, so
 * the CLI reaches exactly the same code.
 */

static void
vl_api_sonic_ext_pbh_profile_add_del_t_handler (
  vl_api_sonic_ext_pbh_profile_add_del_t *mp)
{
  vl_api_sonic_ext_pbh_profile_add_del_reply_t *rmp;
  sonic_ext_pbh_hash_field_t *fields = 0;
  u32 profile_index = ntohl (mp->profile_index);
  u32 i, n_fields = ntohl (mp->n_fields);
  int rv = 0;

  if (!sonic_ext_main.pbh)
    {
      rv = VNET_API_ERROR_FEATURE_DISABLED;
      goto exit;
    }

  if (mp->is_add)
    {
      if (n_fields == 0)
	{
	  rv = VNET_API_ERROR_INVALID_VALUE;
	  goto exit;
	}

      vec_validate (fields, n_fields - 1);

      for (i = 0; i < n_fields; i++)
	{
	  fields[i].field = mp->fields[i].field;
	  fields[i].seq = ntohl (mp->fields[i].sequence_id);
	  /* The mask is carried as 16 wire-order bytes whatever the address
	   * family, so an IPv4 mask simply occupies the first four. */
	  clib_memcpy (&fields[i].mask, mp->fields[i].mask,
		       sizeof (fields[i].mask));
	}
    }

  rv = sonic_ext_pbh_profile_add_del (fields, mp->is_add ? 1 : 0,
				      &profile_index);

exit:
  REPLY_MACRO2 (VL_API_SONIC_EXT_PBH_PROFILE_ADD_DEL_REPLY,
		({ rmp->profile_index = htonl (profile_index); }));
}

static void
vl_api_sonic_ext_pbh_table_add_replace_t_handler (
  vl_api_sonic_ext_pbh_table_add_replace_t *mp)
{
  vl_api_sonic_ext_pbh_table_add_replace_reply_t *rmp;
  sonic_ext_pbh_rule_t *rules = 0;
  u32 table_index = ntohl (mp->table_index);
  u32 i, n_rules = ntohl (mp->n_rules);
  u8 *name;
  int rv = 0;

  if (!sonic_ext_main.pbh)
    {
      rv = VNET_API_ERROR_FEATURE_DISABLED;
      goto exit;
    }

  if (n_rules)
    vec_validate (rules, n_rules - 1);

  for (i = 0; i < n_rules; i++)
    {
      sonic_ext_pbh_rule_t *r = &rules[i];
      sonic_ext_pbh_match_t *m = &r->match;

      r->rule_id = ntohl (mp->rules[i].rule_id);
      r->priority = ntohl (mp->rules[i].priority);
      r->ecmp_profile = ntohl (mp->rules[i].ecmp_profile);
      r->lag_profile = ntohl (mp->rules[i].lag_profile);
      r->flow_counter = mp->rules[i].flow_counter ? 1 : 0;

      m->present = ntohl (mp->rules[i].qualifiers);
      m->ether_type = ntohs (mp->rules[i].ether_type);
      m->inner_ether_type = ntohs (mp->rules[i].inner_ether_type);
      m->l4_dst_port = ntohs (mp->rules[i].l4_dst_port);
      m->ip_protocol = mp->rules[i].ip_protocol;
      m->ipv6_next_header = mp->rules[i].ipv6_next_header;
      m->gre_key = ntohl (mp->rules[i].gre_key);
      m->gre_key_mask = ntohl (mp->rules[i].gre_key_mask);

      r->encap = sonic_ext_pbh_encap_from_match (m);
    }

  name = format (0, "%s", mp->name);
  rv = sonic_ext_pbh_table_add_replace (name, rules, &table_index);

exit:
  REPLY_MACRO2 (VL_API_SONIC_EXT_PBH_TABLE_ADD_REPLACE_REPLY,
		({ rmp->table_index = htonl (table_index); }));
}

static void
vl_api_sonic_ext_pbh_table_del_t_handler (
  vl_api_sonic_ext_pbh_table_del_t *mp)
{
  vl_api_sonic_ext_pbh_table_del_reply_t *rmp;
  int rv;

  rv = sonic_ext_pbh_table_del (ntohl (mp->table_index));

  REPLY_MACRO (VL_API_SONIC_EXT_PBH_TABLE_DEL_REPLY);
}

static void
vl_api_sonic_ext_pbh_interface_attach_detach_t_handler (
  vl_api_sonic_ext_pbh_interface_attach_detach_t *mp)
{
  vnet_interface_main_t *im = &vnet_get_main ()->interface_main;
  vl_api_sonic_ext_pbh_interface_attach_detach_reply_t *rmp;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  int rv = 0;

  if (pool_is_free_index (im->sw_interfaces, sw_if_index))
    {
      rv = VNET_API_ERROR_INVALID_SW_IF_INDEX;
      goto exit;
    }

  rv = sonic_ext_pbh_interface_attach_detach (
    sw_if_index, ntohl (mp->table_index), mp->is_attach ? 1 : 0);

exit:
  REPLY_MACRO (VL_API_SONIC_EXT_PBH_INTERFACE_ATTACH_DETACH_REPLY);
}

/* API definitions */
#include <sonic_ext/sonic_ext.api.c>

static clib_error_t *
sonic_ext_api_init (vlib_main_t *vm)
{
  sonic_ext_main.msg_id_base = setup_message_id_table ();
  return 0;
}

VLIB_INIT_FUNCTION (sonic_ext_api_init);
