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
#ifndef __included_sonic_ext_pbh_hash_h__
#define __included_sonic_ext_pbh_hash_h__

#include <sonic_ext/pbh.h>

#include <vppinfra/hash.h>
#include <vnet/ethernet/packet.h>
#include <vnet/ip/ip4_packet.h>
#include <vnet/ip/ip6_packet.h>
#include <vnet/ip/ip_inner_aware_hash.h>
#include <vnet/udp/udp_packet.h>

/* VXLAN's own header lives in the vxlan *plugin*, which sonic_ext must not
 * take a build dependency on for eight bytes of fixed-size RFC 7348 shim. */
#define SONIC_EXT_PBH_VXLAN_HDR_BYTES 8

/*
 * Fine-grained PBH hash.
 *
 * Reads *only* fields resolved out of the inner header.  The outer header
 * contributes nothing -- that is the property sonic-mgmt's inner_hashing
 * suite pins with its `outer-tuples` pseudo hash key, which holds the inner
 * 5-tuple constant, randomises the outer addresses and ports, and asserts
 * that every packet lands on a single ECMP next hop.
 */

/*
 * Resolve the inner IP header of a packet the PBH classifier has already
 * matched.
 *
 * `l3` points at the outer IP header, `encap` is the one the matching rule
 * qualified on.  Returns non-zero and fills @c out on success; on failure
 * the caller falls back to the switch-global hash.
 *
 * Bounds come from the outer IP header's own length field, exactly as
 * hash_eth.c does, so a truncated or lying packet is rejected rather than
 * read past.
 */
static_always_inline int
sonic_ext_pbh_inner_resolve (const void *l3, int is_ip6,
                             sonic_ext_pbh_encap_t encap, ip_inner_hdr_t *out)
{
  const u8 *payload;
  u32 remaining;
  u8 protocol;

  out->valid = 0;

  if (is_ip6)
    {
      const ip6_header_t *ip6 = l3;

      /* Outer IPv6 extension headers are not chased: a transit tunnel uses
       * a tunnel next-header value, not an extension header. */
      payload = (const u8 *) (ip6 + 1);
      remaining = clib_net_to_host_u16 (ip6->payload_length);
      protocol = ip6->protocol;
    }
  else
    {
      const ip4_header_t *ip4 = l3;
      u32 total_len, ihl;

      if (PREDICT_FALSE (ip4_is_fragment (ip4)))
        return 0;

      total_len = clib_net_to_host_u16 (ip4->length);
      ihl = ip4_header_bytes (ip4);
      if (PREDICT_FALSE (total_len < ihl))
        return 0;

      payload = (const u8 *) ip4 + ihl;
      remaining = total_len - ihl;
      protocol = ip4->protocol;
    }

  if (encap == SONIC_EXT_PBH_ENCAP_VXLAN)
    {
      /* UDP + VXLAN + inner Ethernet, then an autodetected inner IP.  VXLAN
       * is not handled by ip_inner_resolve() because nothing in the packet
       * identifies it -- only the destination port the rule matched on. */
      const u32 hdrs = sizeof (udp_header_t) + SONIC_EXT_PBH_VXLAN_HDR_BYTES +
                       sizeof (ethernet_header_t);
      const ethernet_header_t *eth;
      u16 eth_type;

      if (PREDICT_FALSE (protocol != IP_PROTOCOL_UDP))
        return 0;
      if (PREDICT_FALSE (remaining < hdrs))
        return 0;

      eth = (const ethernet_header_t *) (payload + sizeof (udp_header_t) +
                                         SONIC_EXT_PBH_VXLAN_HDR_BYTES);
      eth_type = clib_net_to_host_u16 (clib_mem_unaligned (&eth->type, u16));

      payload = (const u8 *) (eth + 1);
      remaining -= hdrs;

      if (eth_type == ETHERNET_TYPE_IP4)
        ip_inner_resolve_v4 (payload, remaining, out);
      else if (eth_type == ETHERNET_TYPE_IP6)
        ip_inner_resolve_v6 (payload, remaining, out);

      return out->valid;
    }

  /* NVGRE / GRE / IP-in-IP: the outer protocol says what follows. */
  ip_inner_resolve (protocol, payload, remaining, out);
  return out->valid;
}

/* Position-sensitive fold of a masked IPv6 address.  A plain XOR of the four
 * words would hash a::b and b::a identically; the rotate keeps word position
 * significant for one extra instruction. */
static_always_inline u32
sonic_ext_pbh_fold_ip6 (const ip6_address_t *a, const ip6_address_t *m)
{
  u32 h = 0;

  for (int i = 0; i < 4; i++)
    h = ((h << 7) | (h >> 25)) ^ (a->as_u32[i] & m->as_u32[i]);

  return h;
}

/* L4 ports live in the first four bytes of TCP / UDP / SCTP alike.
 * ip_inner_resolve() guarantees IP_INNER_L4_MIN_BYTES readable at l4. */
static_always_inline int
sonic_ext_pbh_has_l4_ports (u8 protocol)
{
  return protocol == IP_PROTOCOL_TCP || protocol == IP_PROTOCOL_UDP ||
         protocol == IP_PROTOCOL_SCTP;
}

/*
 * Value contributed by one field of the profile.
 *
 * A field whose address family does not match the resolved inner header
 * contributes 0 rather than garbage: the rule's inner-ether-type qualifier
 * normally makes this unreachable, but a profile shared between an IPv4 and
 * an IPv6 rule would otherwise read an IPv4 mask over IPv6 bytes.
 */
static_always_inline u32
sonic_ext_pbh_field_value (const sonic_ext_pbh_hash_field_t *f,
                           const ip_inner_hdr_t *inner)
{
  switch (f->field)
    {
    case SONIC_EXT_PBH_HF_INNER_IP_PROTOCOL:
      return inner->protocol;

    case SONIC_EXT_PBH_HF_INNER_L4_SRC_PORT:
      if (!sonic_ext_pbh_has_l4_ports (inner->protocol))
        return 0;
      return clib_net_to_host_u16 (clib_mem_unaligned (inner->l4, u16));

    case SONIC_EXT_PBH_HF_INNER_L4_DST_PORT:
      if (!sonic_ext_pbh_has_l4_ports (inner->protocol))
        return 0;
      return clib_net_to_host_u16 (
        clib_mem_unaligned ((const u8 *) inner->l4 + 2, u16));

    case SONIC_EXT_PBH_HF_INNER_SRC_IPV4:
      if (inner->is_v6)
        return 0;
      return inner->ip.v4->src_address.as_u32 & f->mask.ip4.as_u32;

    case SONIC_EXT_PBH_HF_INNER_DST_IPV4:
      if (inner->is_v6)
        return 0;
      return inner->ip.v4->dst_address.as_u32 & f->mask.ip4.as_u32;

    case SONIC_EXT_PBH_HF_INNER_SRC_IPV6:
      if (!inner->is_v6)
        return 0;
      return sonic_ext_pbh_fold_ip6 (&inner->ip.v6->src_address,
                                     &f->mask.ip6);

    case SONIC_EXT_PBH_HF_INNER_DST_IPV6:
      if (!inner->is_v6)
        return 0;
      return sonic_ext_pbh_fold_ip6 (&inner->ip.v6->dst_address,
                                     &f->mask.ip6);

    default:
      return 0;
    }
}

/*
 * Compute a profile's hash over an already-resolved inner header.
 *
 * The inner header is resolved once per packet by the matching path -- a
 * rule's inner-ether-type qualifier needs it too -- and handed here, rather
 * than re-parsed per profile.
 *
 * Fields sharing a sequence_id are XOR-folded, which makes them
 * order-independent -- that is how SONiC expresses "src and dst are
 * interchangeable", i.e. symmetric hashing, by giving inner_src_ipv4 and
 * inner_dst_ipv4 the same sequence id.  Distinct sequence ids are mixed in
 * order, with the id itself mixed in so that moving a field to a different
 * position changes the result.
 *
 * There is no per-stage salt: a rule carries at most one action, so the ECMP
 * and LAG hashes of a given packet are never both consumed, and polarization
 * between the two stages cannot arise.
 *
 * Returns a non-zero hash.
 */
static_always_inline u32
sonic_ext_pbh_hash_inner (const sonic_ext_pbh_profile_t *p,
                          const ip_inner_hdr_t *inner)
{
  /* Jenkins' three-word state, seeded with the golden ratio as upstream
   * does.  All three words must be carried across groups and all three
   * handed to the finalizer: hash_v3_* mix their arguments into each other,
   * so passing one variable three times would make the finalizer's opening
   * `c ^= b` a self-XOR and collapse every hash to zero. */
  u32 a = 0x9e3779b9, b = 0x9e3779b9, c = 0x9e3779b9;
  u32 i = 0, n = vec_len (p->fields);

  /* Fields are sorted by seq at configuration time, so one pass walks each
   * sequence_id's run in turn: the inner loop advances the shared `i`. */
  while (i < n)
    {
      u32 group = 0, s = p->fields[i].seq;

      /* Same sequence_id => XOR-fold => order independent. */
      for (; i < n && p->fields[i].seq == s; i++)
        group ^= sonic_ext_pbh_field_value (&p->fields[i], inner);

      /* Distinct sequence_id => ordered mix, with the id itself mixed in so
       * that moving a field between sequence positions changes the result. */
      a += group;
      b += s;
      hash_v3_mix32 (a, b, c);
    }

  hash_v3_finalize32 (a, b, c);

  /* 0 is the caller's "no PBH hash" sentinel; never collide with it. */
  return c | (c == 0);
}

#endif /* __included_sonic_ext_pbh_hash_h__ */
