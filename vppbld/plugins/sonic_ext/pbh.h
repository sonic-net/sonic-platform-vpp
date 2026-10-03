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
#ifndef __included_sonic_ext_pbh_h__
#define __included_sonic_ext_pbh_h__

#include <vnet/vnet.h>
#include <vnet/ip/ip4_packet.h>
#include <vnet/ip/ip6_packet.h>

/*
 * Policy Based Hashing (SAI PBH) for the SONiC-VPP datapath.
 *
 * SONiC PBH is a per-rule *override* of the switch-global flow hash: a
 * classifier selects tunnelled packets and points them at a named hash
 * profile, separately for the ECMP and the LAG stage.  It layers on top of
 * the switch-global inner-aware hash (patch 0011) rather than replacing it:
 * a packet that matches no rule keeps the global behaviour.
 *
 * This header carries the configuration types.  The hash itself is in
 * pbh_hash.h so it can be unit-tested and inlined into the node without
 * dragging in the control plane.
 */

/*
 * Fine-grained hash fields.
 *
 * SAI models a PBH hash as an ordered list of SAI_NATIVE_HASH_FIELD values,
 * each with its own sequence id and (for addresses) its own mask.  VPP's
 * flow_hash_config_t is a flat bitmap with a fixed field order and no masks,
 * so it cannot express this -- hence a private field list.
 *
 * Only the INNER_ fields are modelled: those are what SONiC's PbhOrch emits
 * (a PBH rule exists precisely to hash on the encapsulated flow) and what
 * the inner_hashing test configures.  Outer fields would also have to
 * survive the `ip_inner_resolve` gate below, which by construction they do
 * not.
 */
#define foreach_sonic_ext_pbh_hash_field                                      \
  _ (INNER_IP_PROTOCOL, "inner-ip-protocol")                                  \
  _ (INNER_L4_SRC_PORT, "inner-l4-src-port")                                  \
  _ (INNER_L4_DST_PORT, "inner-l4-dst-port")                                  \
  _ (INNER_SRC_IPV4, "inner-src-ipv4")                                        \
  _ (INNER_DST_IPV4, "inner-dst-ipv4")                                        \
  _ (INNER_SRC_IPV6, "inner-src-ipv6")                                        \
  _ (INNER_DST_IPV6, "inner-dst-ipv6")

typedef enum
{
#define _(sym, str) SONIC_EXT_PBH_HF_##sym,
  foreach_sonic_ext_pbh_hash_field
#undef _
    SONIC_EXT_PBH_HF_N_FIELDS,
} sonic_ext_pbh_hash_field_id_t;

typedef struct
{
  u8 field; /* sonic_ext_pbh_hash_field_id_t */
  u32 seq;  /* SAI sequence_id; equal ids are order-independent */
  union
  {
    ip4_address_t ip4;
    ip6_address_t ip6;
  } mask; /* network order, as on the wire; unused for non-address fields */
} sonic_ext_pbh_hash_field_t;

typedef struct
{
  /* Sorted ascending by seq at configuration time so the datapath never
   * sorts.  n_groups is the number of distinct seq values. */
  sonic_ext_pbh_hash_field_t *fields;
  u32 n_groups;
} sonic_ext_pbh_profile_t;

/*
 * Which encapsulation a rule matched.
 *
 * The inner header cannot be found without knowing the encap: for NVGRE the
 * outer IP protocol (0x2f) is self-describing and vnet/ip/ip_inner_aware_hash.h
 * resolves it, but a VXLAN packet is just UDP -- the only thing that makes it
 * VXLAN is the destination port, which is a per-deployment value.  The PBH
 * rule already qualifies on that port, so the encap is known at match time
 * and is carried on the rule rather than guessed per packet.
 */
typedef enum
{
  SONIC_EXT_PBH_ENCAP_NONE = 0,
  SONIC_EXT_PBH_ENCAP_VXLAN,
  SONIC_EXT_PBH_ENCAP_IP_GRE, /* NVGRE / GRE / IP-in-IP: self-describing */
} sonic_ext_pbh_encap_t;

/*
 * Rule match criteria.
 *
 * SAI PBH exposes a small closed set of qualifiers -- not the arbitrary byte
 * matching of an ACL -- so each is a named field with a presence bit rather
 * than a mask/match byte string fed to vnet_classify.  That keeps the match
 * correct in the presence of IPv4 options (classify's fixed offsets are not)
 * and keeps the rule readable in `show`.  Rule counts are small: PbhOrch
 * emits one rule per (encap, inner address family).
 */
#define foreach_sonic_ext_pbh_qualifier                                       \
  _ (ETHER_TYPE, "ether-type")                                                \
  _ (IP_PROTOCOL, "ip-protocol")                                              \
  _ (IPV6_NEXT_HEADER, "ipv6-next-header")                                    \
  _ (L4_DST_PORT, "l4-dst-port")                                              \
  _ (GRE_KEY, "gre-key")                                                      \
  _ (INNER_ETHER_TYPE, "inner-ether-type")

typedef enum
{
#define _(sym, str) SONIC_EXT_PBH_Q_BIT_##sym,
  foreach_sonic_ext_pbh_qualifier
#undef _
} sonic_ext_pbh_qualifier_bit_t;

typedef enum
{
#define _(sym, str) SONIC_EXT_PBH_Q_##sym = 1u << SONIC_EXT_PBH_Q_BIT_##sym,
  foreach_sonic_ext_pbh_qualifier
#undef _
} sonic_ext_pbh_qualifier_t;

typedef struct
{
  u32 present; /* bitmap of SONIC_EXT_PBH_Q_* */
  u16 ether_type;
  u16 inner_ether_type;
  u16 l4_dst_port;
  u8 ip_protocol;
  u8 ipv6_next_header;
  u32 gre_key;
  u32 gre_key_mask;
} sonic_ext_pbh_match_t;

/*
 * One PBH rule.  Both actions may be set, both may be absent: SAI models
 * SET_ECMP_HASH and SET_LAG_HASH as independent attributes of one entry even
 * though PbhOrch only ever emits one at a time.
 */
typedef struct
{
  u32 rule_id;        /* caller's id; stable across replace, used by `show` */
  u32 priority; /* higher wins; rules kept sorted descending */
  sonic_ext_pbh_match_t match;
  sonic_ext_pbh_encap_t encap; /* derived from the qualifiers at config */
  u32 ecmp_profile;               /* profile pool index, ~0 = no action */
  u32 lag_profile;               /* profile pool index, ~0 = no action */
  u8 flow_counter;               /* count matches into the table's counters */
} sonic_ext_pbh_rule_t;

typedef struct
{
  u8 *name;                         /* for `show`; vec, NUL-terminated */
  sonic_ext_pbh_rule_t *rules;         /* sorted by priority, descending */
  u32 *sw_if_indices;                 /* interfaces this table is attached to */
  u8 holds_sideband;                 /* this table holds a side-band reference */
  vlib_combined_counter_main_t counters; /* indexed by rule position */
} sonic_ext_pbh_table_t;

typedef struct
{
  sonic_ext_pbh_profile_t *profiles; /* pool */
  sonic_ext_pbh_table_t *tables;     /* pool */

  /* Attachment, indexed by sw_if_index; ~0 = not attached.  One table per
   * interface per direction, as SAI_PORT_ATTR_PBH_* is a single OID. */
  u32 *table_index_by_sw_if_index;

  /* Shadow attachment on a bridge domain's BVI.
   *
   * A table bound to a port that is an L2 bridge member would never run on
   * traffic the bridge terminates into L3: l2_to_bvi() rewrites
   * sw_if_index[VLIB_RX] to the BVI before jumping to ip4-input, so the
   * ip4-unicast arc is evaluated against the BVI and not against the member
   * port the table was bound to.  Attaching the BVI as well is what makes
   * such a binding take effect.
   *
   * bvi_by_sw_if_index records which BVI a member port's attachment pulled
   * in, so detach can release exactly that one even if the port has already
   * left the bridge by then.  bvi_refcount_by_sw_if_index counts the members
   * holding each BVI, since a table is normally bound to every member of a
   * VLAN and they all resolve to the same one.
   *
   * The shadow is bridge-domain-wide, but the binding it stands in for is
   * per-port, so the dataplane narrows it again: sonic-ext-capture stamps the
   * real ingress sw_if_index into the buffer cookie at device-input, and the
   * node only applies the table if that port is one the table is bound to.
   * See sonic_ext_pbh_shadow_admits().
   */
  u32 *bvi_by_sw_if_index;
  u32 *bvi_refcount_by_sw_if_index;

  /* Set once the ip4/ip6-unicast arc features have been registered on at
   * least one interface, so the arc cost is only paid while PBH is in use. */
  u32 n_attachments;

  u64 hits;   /* packets a rule matched and an action applied to */
  u64 misses; /* packets examined that matched no rule */
} sonic_ext_pbh_main_t;

extern sonic_ext_pbh_main_t sonic_ext_pbh_main;

/* Control plane.  All return 0 on success or a VNET_API_ERROR_* code. */

/* Create (is_add) or destroy a hash profile.  On add, *profile_index is set
 * to the new index.  `fields` is consumed on success and freed on failure. */
int sonic_ext_pbh_profile_add_del (sonic_ext_pbh_hash_field_t *fields,
                                   int is_add, u32 *profile_index);

/* Create or atomically replace a table's whole rule set.  `rules` is
 * consumed on success and freed on failure.  On create, *table_index is set;
 * on replace it names the table to overwrite. */
int sonic_ext_pbh_table_add_replace (u8 *name, sonic_ext_pbh_rule_t *rules,
                                     u32 *table_index);
int sonic_ext_pbh_table_del (u32 table_index);

int sonic_ext_pbh_interface_attach_detach (u32 sw_if_index, u32 table_index,
                                           int is_attach);

/* Derive the encapsulation a rule's qualifiers imply.  Exposed for the API
 * and CLI so both get the same answer. */
sonic_ext_pbh_encap_t
sonic_ext_pbh_encap_from_match (const sonic_ext_pbh_match_t *m);

format_function_t format_sonic_ext_pbh_profile;
format_function_t format_sonic_ext_pbh_rule;

extern vlib_node_registration_t sonic_ext_pbh_ip4_node;
extern vlib_node_registration_t sonic_ext_pbh_ip6_node;

#endif /* __included_sonic_ext_pbh_h__ */
