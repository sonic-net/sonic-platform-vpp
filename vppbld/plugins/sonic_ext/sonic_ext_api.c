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

  if (!strcmp (name, "ip2me"))
    enabled = sem->ip2me;
  else if (!strcmp (name, "l2-trap-fixup"))
    enabled = sem->l2_trap_fixup;
  else if (!strcmp (name, "l2-vlan-filter"))
    enabled = sem->l2_vlan_filter;

  /* The following is wired by VPP and is not queried.
   * Answer for completeness/correctness */
  else if (!strcmp (name, "punt-via-member"))
    enabled = sem->punt_via_member;
  else if (!strcmp (name, "host-xc"))
    enabled = sem->host_xc;
  else if (!strcmp (name, "drop-member-stats"))
    enabled = sem->drop_member_stats;
  else if (!strcmp (name, "capture"))
    enabled = sem->capture_enabled;

  REPLY_MACRO2 (VL_API_SONIC_EXT_FEATURE_GET_REPLY,
		({ rmp->enabled = enabled; }));
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
