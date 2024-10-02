/* Copyright (c) 2024 Cisco and/or its affiliates.
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
 * limitations under the License. */
/**
 * @file
 * @brief Tunnel Terminated Plugin Redirect Functionality.
 */
#include <vlib/vlib.h>
#include <vnet/fib/fib_path_list.h>
#include <vnet/classify/vnet_classify.h>
#include <vnet/classify/in_out_acl.h>
#include <vnet/plugin/plugin.h>
#include <vpp/app/version.h>

typedef struct
{
  u8 *match_and_table_index;
  dpo_id_t dpo;	   /* forwarding dpo */
  fib_node_t node; /* linkage into the FIB graph */
  fib_node_index_t pl;
  u32 sibling;
  u32 parent_node_index;
  u32 opaque_index;
  u32 table_index;
  fib_forward_chain_type_t payload_type;
  u8 is_ip6 : 1;
} tunterm_acl_redirect_t;

typedef struct
{
  tunterm_acl_redirect_t *pool;
  u32 *session_by_match_and_table_index;
  fib_node_type_t fib_node_type;
} tunterm_acl_redirect_main_t;

static tunterm_acl_redirect_main_t tunterm_acl_redirect_main;

static int
tunterm_acl_redirect_stack (tunterm_acl_redirect_t *ipr)
{
  dpo_id_t dpo = DPO_INVALID;

  fib_path_list_contribute_forwarding (ipr->pl, ipr->payload_type,
				       fib_path_list_is_popular (ipr->pl) ?
					       FIB_PATH_LIST_FWD_FLAG_NONE :
					       FIB_PATH_LIST_FWD_FLAG_COLLAPSE,
				       &dpo);
  dpo_stack_from_node (ipr->parent_node_index, &ipr->dpo, &dpo);
  dpo_reset (&dpo);

  /* update session with new next_index */
  return vnet_classify_add_del_session (
    &vnet_classify_main, ipr->table_index, ipr->match_and_table_index,
    ipr->dpo.dpoi_next_node /* hit_next_index */, ipr->opaque_index,
    0 /* advance */, CLASSIFY_ACTION_SET_METADATA,
    ipr->dpo.dpoi_index /* metadata */, 1 /* is_add */);
}

static tunterm_acl_redirect_t *
tunterm_acl_redirect_find (tunterm_acl_redirect_main_t *im, u32 table_index,
			   const u8 *match)
{
  /* we are adding the table index at the end of the match string so we
   * can disambiguiate identical matches in different tables in
   * im->session_by_match_and_table_index */
  u8 *match_and_table_index = vec_dup (match);
  vec_add (match_and_table_index, (void *) &table_index, 4);
  uword *p =
    hash_get_mem (im->session_by_match_and_table_index, match_and_table_index);
  vec_free (match_and_table_index);
  if (!p)
    return 0;
  return pool_elt_at_index (im->pool, p[0]);
}

__clib_export int
tunterm_acl_redirect_add (vlib_main_t *vm, u32 table_index, u32 opaque_index,
			  dpo_proto_t proto, const u8 *match,
			  const fib_route_path_t *rpaths)
{
  tunterm_acl_redirect_main_t *im = &tunterm_acl_redirect_main;
  fib_forward_chain_type_t payload_type;
  tunterm_acl_redirect_t *ipr;
  const char *pname;

  payload_type = fib_forw_chain_type_from_dpo_proto (proto);
  switch (payload_type)
    {
    case FIB_FORW_CHAIN_TYPE_UNICAST_IP4:
    case FIB_FORW_CHAIN_TYPE_UNICAST_IP6:
      pname = "tunterm-acl";
      break;
    default:
      return VNET_API_ERROR_INVALID_ADDRESS_FAMILY;
    }

  ipr = tunterm_acl_redirect_find (im, table_index, match);
  if (ipr)
    {
      /* update to an existing session */
      fib_path_list_child_remove (ipr->pl, ipr->sibling);
      dpo_reset (&ipr->dpo);
    }
  else
    {
      /* allocate a new entry */
      pool_get (im->pool, ipr);
      fib_node_init (&ipr->node, im->fib_node_type);
      ipr->match_and_table_index = vec_dup ((u8 *) match);
      /* we are adding the table index at the end of the match string so we
       * can disambiguiate identical matches in different tables in
       * im->session_by_match_and_table_index */
      vec_add (ipr->match_and_table_index, (void *) &table_index, 4);
      ipr->table_index = table_index;
      hash_set_mem (im->session_by_match_and_table_index,
		    ipr->match_and_table_index, ipr - im->pool);
    }

  ipr->payload_type = payload_type;
  ipr->pl = fib_path_list_create (
    FIB_PATH_LIST_FLAG_SHARED | FIB_PATH_LIST_FLAG_NO_URPF, rpaths);
  ipr->sibling =
    fib_path_list_child_add (ipr->pl, im->fib_node_type, ipr - im->pool);
  ipr->parent_node_index = vlib_get_node_by_name (vm, (u8 *) pname)->index;
  ipr->opaque_index = opaque_index;
  ipr->is_ip6 = payload_type == FIB_FORW_CHAIN_TYPE_UNICAST_IP6;

  return tunterm_acl_redirect_stack (ipr);
}
int
tunterm_acl_redirect_del_ipr (tunterm_acl_redirect_main_t *im,
			      tunterm_acl_redirect_t *ipr)
{
  vnet_classify_main_t *cm = &vnet_classify_main;
  int rv;

  rv = vnet_classify_add_del_session (
    cm, ipr->table_index, ipr->match_and_table_index, 0 /* hit_next_index */,
    0 /* opaque_index */, 0 /* advance */, 0 /* action */, 0 /* metadata */,
    0 /* is_add */);
  if (rv)
    return rv;

  hash_unset_mem (im->session_by_match_and_table_index,
		  ipr->match_and_table_index);
  vec_free (ipr->match_and_table_index);
  fib_path_list_child_remove (ipr->pl, ipr->sibling);
  dpo_reset (&ipr->dpo);
  pool_put (im->pool, ipr);
  return 0;
}

__clib_export int
tunterm_acl_redirect_del (vlib_main_t *vm, u32 table_index, const u8 *match)
{
  tunterm_acl_redirect_main_t *im = &tunterm_acl_redirect_main;
  tunterm_acl_redirect_t *ipr;
  int rv;

  ipr = tunterm_acl_redirect_find (im, table_index, match);
  if (!ipr)
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  rv = tunterm_acl_redirect_del_ipr (im, ipr);

  return rv;
}

int
tunterm_acl_redirect_clear (vlib_main_t *vm, u32 table_index)
{
  tunterm_acl_redirect_main_t *im = &tunterm_acl_redirect_main;
  tunterm_acl_redirect_t *ipr;
  tunterm_acl_redirect_t **to_be_deleted = NULL;
  int rv = 0;

  // Iterate through the pool to find all entries with the specified
  // table_index
  pool_foreach (ipr, im->pool)
    {
      if (ipr->table_index == table_index)
	{
	  vec_add1 (to_be_deleted, ipr);
	}
    }

  tunterm_acl_redirect_t **ipr_ptr;
  vec_foreach (ipr_ptr, to_be_deleted)
    {
      rv = tunterm_acl_redirect_del_ipr (im, *ipr_ptr);
      if (rv)
	{
	  vec_free (to_be_deleted);
	  return rv;
	}
    }

  vec_free (to_be_deleted);
  return 0;
}

static fib_node_t *
tunterm_acl_redirect_get_node (fib_node_index_t index)
{
  tunterm_acl_redirect_main_t *im = &tunterm_acl_redirect_main;
  tunterm_acl_redirect_t *ipr = pool_elt_at_index (im->pool, index);
  return &ipr->node;
}

static tunterm_acl_redirect_t *
tunterm_acl_redirect_get_from_node (fib_node_t *node)
{
  return (tunterm_acl_redirect_t *) (((char *) node) -
				     STRUCT_OFFSET_OF (tunterm_acl_redirect_t,
						       node));
}

static void
tunterm_acl_redirect_last_lock_gone (fib_node_t *node)
{
  /* the lifetime of the entry is managed by the table. */
  ASSERT (0);
}

/* A back walk has reached this entry */
static fib_node_back_walk_rc_t
tunterm_acl_redirect_back_walk_notify (fib_node_t *node,
				       fib_node_back_walk_ctx_t *ctx)
{
  int rv;
  tunterm_acl_redirect_t *ipr = tunterm_acl_redirect_get_from_node (node);
  rv = tunterm_acl_redirect_stack (ipr);
  ASSERT (0 == rv);
  if (rv)
    clib_warning ("tunterm_acl_redirect_stack() error %d", rv);
  return FIB_NODE_BACK_WALK_CONTINUE;
}

static const fib_node_vft_t tunterm_acl_redirect_vft = {
  .fnv_get = tunterm_acl_redirect_get_node,
  .fnv_last_lock = tunterm_acl_redirect_last_lock_gone,
  .fnv_back_walk = tunterm_acl_redirect_back_walk_notify,
};

clib_error_t *
tunterm_acl_redirect_init (vlib_main_t *vm)
{
  tunterm_acl_redirect_main_t *im = &tunterm_acl_redirect_main;
  im->session_by_match_and_table_index =
    hash_create_vec (0, sizeof (u8), sizeof (u32));
  im->fib_node_type =
    fib_node_register_new_type ("tunterm-redirect", &tunterm_acl_redirect_vft);
  return 0;
}
