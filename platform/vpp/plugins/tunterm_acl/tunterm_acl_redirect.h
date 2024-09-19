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

#ifndef TUNTERM_ACL_REDIRECT_H_
#define TUNTERM_ACL_REDIRECT_H_

#include <vnet/fib/fib_node.h>

clib_error_t * tunterm_acl_redirect_init (vlib_main_t *vm);

int tunterm_acl_redirect_add (vlib_main_t *vm, u32 table_index,
			     u32 opaque_index, dpo_proto_t proto,
			     const u8 *match, const fib_route_path_t *rpaths);

int tunterm_acl_redirect_del (vlib_main_t *vm, u32 table_index,
			     const u8 *match);

int tunterm_acl_redirect_clear (vlib_main_t *vm, u32 table_index);

#endif /* TUNTERM_ACL_REDIRECT_H_ */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
