/*
 * Copyright (c) 2024 Cisco and/or its affiliates.
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
 * @brief Tunnel Terminated Plugin API Code
 */

#include <vnet/vnet.h>
#include <vnet/plugin/plugin.h>
#include <tunterm_acl/tunterm_acl_api.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

#include <tunterm_acl/tunterm_acl.api_enum.h>
#include <tunterm_acl/tunterm_acl.api_types.h>

#define REPLY_MSG_ID_BASE sm->msg_id_base
#include <vlibapi/api_helper_macros.h>

/* *INDENT-OFF* */
VLIB_PLUGIN_REGISTER () = {
    .version = TUNTERM_ACL_PLUGIN_BUILD_VER,
    .description = "Tunnel Terminated ACL Plugin",
};
/* *INDENT-ON* */

tunterm_acl_main_t tunterm_acl_main;

#include <vnet/classify/vnet_classify.h>
#include <vnet/ip/ip4_packet.h>
#include <vppinfra/string.h>

#include <vnet/fib/fib_api.h>
#include <vnet/ip/ip_format_fns.h>
#include <vnet/ip/ip_types_api.h>

#include "tunterm_acl_redirect.h"

static int update_classify_table_and_sessions (bool is_ipv6, u32 count, vl_api_tunterm_acl_rule_t rules[], u32 * tunterm_acl_index)
{
  tunterm_acl_main_t * sm = &tunterm_acl_main;
  vnet_classify_main_t *cm = &vnet_classify_main;
  u32 table_index = *tunterm_acl_index;
  int rv = 0;
#define CLASSIFY_TABLE_VECTOR_SIZE 16
  u8 mask[7*CLASSIFY_TABLE_VECTOR_SIZE];

  clib_memset (mask, 0, sizeof (mask));
  u32 nbuckets = 2;
  u32 memory_size = 2 << 22;
  u32 skip = 5;
  u32 match;

  /* Create the table if it's an add operation */
  if (table_index == ~0) {
    if (is_ipv6) {
      match = 2;
      for (int i = 8; i <= 23; i++) {
        mask[i] = 0xff;
      }
    } else {
      match = 1;
      for (int i = 0; i <= 3; i++) {
        mask[i] = 0xff;
      }
    }

    rv = vnet_classify_add_del_table (cm, mask, nbuckets /* nbuckets */, memory_size /* memory_size */,
                                      skip /* skip */, match,  ~0 /* next_table_index */,
                                      ~0 /* miss_next_index */, &table_index,
                                      0 /* current_data_flag */, 0 /* current_data_offset */,
                                      1 /* is_add */, 0 /* del_chain */);

    if (rv != 0) {
      clib_warning ("vnet_classify_add_del_table failed");
      return rv;
    }

    *tunterm_acl_index = table_index;

    vec_validate_init_empty (sm->classify_table_index_is_v6, table_index, 0);
    sm->classify_table_index_is_v6[table_index] = is_ipv6;
  }

  /* Make sure table AF is not being changed in the replace case */
  if (sm->classify_table_index_is_v6[table_index] != is_ipv6) {
    clib_error ("Table AF mismatch");
    return VNET_API_ERROR_INVALID_VALUE_2;
  }

  /* First clear anything already in the table */
  rv = tunterm_acl_redirect_clear (vlib_get_main (), table_index);

  if (rv != 0) {
    clib_warning ("tunterm_acl_redirect_clear failed");
    return rv;
  }

  /* Now add the new stuff */
  for (int i = 0; i < count; i++) {
    /* (1) Process Route */
    fib_route_path_t *paths_ = 0;
    u8 n_paths = 1; //rules[i].n_paths;
    if (n_paths <= 0) {
      return VNET_API_ERROR_NO_PATHS_IN_ROUTE;
    }

    for (int j = 0; j < n_paths; j++) {
      fib_route_path_t path;
      clib_memset(&path, 0, sizeof(path));

      if ((rv = fib_api_path_decode (&rules[i].path, &path))) {
        vec_free (paths_);
        return rv;
      }
      vec_add1 (paths_, path);
    }

    /* (2) Process DST IP */
    if (is_ipv6 != rules[i].dst.af) {
      return VNET_API_ERROR_INVALID_VALUE_3;
      vec_free (paths_);
    }

    ip46_address_t dst;
    ip_address_decode (&rules[i].dst, &dst);

    u8 *match_vec = 0;
    clib_memset (mask, 0, sizeof (mask));

    if (is_ipv6) {
      for (int i = 0; i < 16; i++) {
        mask[8 + i + skip * CLASSIFY_TABLE_VECTOR_SIZE] = dst.ip6.as_u8[i];
      }
    } else {
      for (int i = 0; i < 4; i++) {
        mask[i + skip * CLASSIFY_TABLE_VECTOR_SIZE] = dst.ip4.data[i];
      }
    }

    /* (3) Add Session */
    vec_validate (match_vec, sizeof (mask) - 1);
    clib_memcpy (match_vec, mask, sizeof (mask));

    rv = tunterm_acl_redirect_add (vlib_get_main (), table_index,
                               0 /* opaque_index */, is_ipv6 ? DPO_PROTO_IP6 : DPO_PROTO_IP4,
                               match_vec /* match */, paths_);

    vec_free(paths_);
    vec_free(match_vec);

    if (rv != 0) {
      clib_warning ("tunterm_acl_redirect_add failed");
      return rv;
    }
  }

  return rv;
}

static int
verify_message_len (void *mp, u64 expected_len, char  *where)
{
  u32 supplied_len = vl_msg_api_get_msg_length (mp);
  if (supplied_len < expected_len) {
    clib_warning ("%s: Supplied message length %d is less than expected %d",
                  where, supplied_len, expected_len);
    return 0;
  } else {
    return 1;
  }
}

/**
 * @brief Plugin API message handler.
 */
static void
vl_api_tunterm_acl_add_replace_t_handler (vl_api_tunterm_acl_add_replace_t * mp)
{
  vl_api_tunterm_acl_add_replace_reply_t *rmp;
  tunterm_acl_main_t * sm = &tunterm_acl_main;
  int rv = -1;
  u32 tunterm_acl_index = ntohl (mp->tunterm_acl_index);
  u32 acl_count = ntohl (mp->count);
  u64 expected_len = sizeof (*mp) + acl_count * sizeof (mp->r[0]);

  if (verify_message_len (mp, expected_len, "tunterm_acl_add_replace")) {
    rv = update_classify_table_and_sessions(mp->is_ipv6, acl_count, mp->r, &tunterm_acl_index);
  } else {
    rv = VNET_API_ERROR_INVALID_VALUE;
  }

  REPLY_MACRO2(VL_API_TUNTERM_ACL_ADD_REPLACE_REPLY,
  ({
    rmp->tunterm_acl_index = htonl(tunterm_acl_index);
  }));
}

static void
vl_api_tunterm_acl_del_t_handler (vl_api_tunterm_acl_del_t * mp)
{
  tunterm_acl_main_t * sm = &tunterm_acl_main;
  vnet_classify_main_t *cm = &vnet_classify_main;
  vl_api_tunterm_acl_del_reply_t *rmp;
  u32 tunterm_acl_index = ntohl (mp->tunterm_acl_index);
  int rv = 0;

  if (tunterm_acl_index == ~0) {
    rv = VNET_API_ERROR_INVALID_VALUE;
    goto exit;
  }

  /* If tunterm index still being used, reject delete */
  for (int i = 0; i < vec_len(sm->classify_table_index_by_sw_if_index_v4); i++) {
    if (sm->classify_table_index_by_sw_if_index_v4[i] == tunterm_acl_index) {
      rv = VNET_API_ERROR_RSRC_IN_USE;
      goto exit;
    }
  }

  for (int i = 0; i < vec_len(sm->classify_table_index_by_sw_if_index_v6); i++) {
    if (sm->classify_table_index_by_sw_if_index_v6[i] == tunterm_acl_index) {
      rv = VNET_API_ERROR_RSRC_IN_USE;
      goto exit;
    }
  }

  /* First clear all the redirect sessions on that table */
  rv = tunterm_acl_redirect_clear (vlib_get_main (), tunterm_acl_index);
  if (rv != 0) {
    clib_warning ("tunterm_acl_redirect_clear failed");
    goto exit;
  }

  /* Then delete the classify table */
  vnet_classify_delete_table_index (cm, tunterm_acl_index, 1 /* del_chain */);

  sm->classify_table_index_is_v6[tunterm_acl_index] = 0;

exit:
  REPLY_MACRO (VL_API_TUNTERM_ACL_DEL_REPLY);
}

static void
vl_api_tunterm_acl_interface_add_del_t_handler (vl_api_tunterm_acl_interface_add_del_t * mp)
{
  tunterm_acl_main_t * sm = &tunterm_acl_main;
  vnet_interface_main_t *im = &sm->vnet_main->interface_main;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  u32 tunterm_acl_index = ntohl (mp->tunterm_acl_index);
  vl_api_tunterm_acl_interface_add_del_reply_t *rmp;
  int rv = 0;

  if (tunterm_acl_index == ~0) {
    rv = VNET_API_ERROR_INVALID_VALUE;
    goto exit;
  }

  if (pool_is_free_index (im->sw_interfaces, sw_if_index)) {
    rv = VNET_API_ERROR_INVALID_SW_IF_INDEX;
    goto exit;
  }

  /* make sure both are init as you can get v6 packets while only v4 acl installed. node will do a lookup in v6 table and crash otherwise */
  vec_validate_init_empty (sm->classify_table_index_by_sw_if_index_v6, sw_if_index, ~0);
  vec_validate_init_empty (sm->classify_table_index_by_sw_if_index_v4, sw_if_index, ~0);

  bool is_ipv6 = sm->classify_table_index_is_v6[tunterm_acl_index];

  if (mp->is_add) {

    /* First setup forwarding data, then enable */
    if (is_ipv6) {
      sm->classify_table_index_by_sw_if_index_v6[sw_if_index] = tunterm_acl_index;
    } else {
      sm->classify_table_index_by_sw_if_index_v4[sw_if_index] = tunterm_acl_index;
    }

    if (!(vnet_feature_is_enabled("ip4-unicast", "tunterm-ip4-vxlan-bypass", sw_if_index))) {
      rv = vnet_feature_enable_disable ("ip4-unicast", "tunterm-ip4-vxlan-bypass",
                                        sw_if_index, mp->is_add, 0, 0);
    }
  } else {
    u32 tunterm_acl_index_v4 = sm->classify_table_index_by_sw_if_index_v4[sw_if_index];
    u32 tunterm_acl_index_v6 = sm->classify_table_index_by_sw_if_index_v6[sw_if_index];

    if (tunterm_acl_index != tunterm_acl_index_v4 && tunterm_acl_index != tunterm_acl_index_v6) {
      /* tunterm being removed is not attached */
      rv = VNET_API_ERROR_INVALID_VALUE_2;
      goto exit;
    }

    /* if last tunterm being removed from intf, then disable */
    if (tunterm_acl_index_v4 == ~0 || tunterm_acl_index_v6 == ~0) {
      rv = vnet_feature_enable_disable ("ip4-unicast", "tunterm-ip4-vxlan-bypass",
                                        sw_if_index, mp->is_add, 0, 0);

      if (rv != 0) {
        goto exit;
      }
    }

    /* finally, remove forwarding data */
    if (is_ipv6) {
      sm->classify_table_index_by_sw_if_index_v6[sw_if_index] = ~0;
    } else {
      sm->classify_table_index_by_sw_if_index_v4[sw_if_index] = ~0;
    }
  }

exit:
  REPLY_MACRO (VL_API_TUNTERM_ACL_INTERFACE_ADD_DEL_REPLY);
}

/* API definitions */
#include <tunterm_acl/tunterm_acl.api.c>

/**
 * @brief Initialize the tunterm plugin.
 */
static clib_error_t * tunterm_acl_init (vlib_main_t * vm)
{
  tunterm_acl_main_t * sm = &tunterm_acl_main;

  sm->vnet_main =  vnet_get_main ();

  /* Add our API messages to the global name_crc hash table */
  sm->msg_id_base = setup_message_id_table ();

  return tunterm_acl_redirect_init(vm);
}

VLIB_INIT_FUNCTION (tunterm_acl_init);
