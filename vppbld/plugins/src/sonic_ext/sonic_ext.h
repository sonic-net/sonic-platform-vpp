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
#ifndef __included_sonic_ext_h__
#define __included_sonic_ext_h__

#include <vnet/vnet.h>
#include <vnet/buffer.h>
#include <vnet/ethernet/ethernet.h>
#include <vnet/ip/ip4_packet.h>
#include <vnet/ip/ip6_packet.h>
#include <vnet/feature/feature.h>
#include <vnet/l2/l2_input.h>
#include <vnet/l2/l2_bvi.h>
#include <plugins/linux-cp/lcp_interface.h>

#define SONIC_EXT_PLUGIN_BUILD_VER "1.0"

/* Locate a transport header from an IPv6 base header without depending on
 * b->current_data. Punt/reinject paths can restore the wire L2 header while
 * retaining a later current_data position, which makes ip6_locate_header()
 * reject an otherwise valid chain. The explicit byte span is authoritative. */
static_always_inline int
sonic_ext_ip6_locate_transport (const ip6_header_t *ip6, u32 available,
				u8 wanted, u32 *offset)
{
  u8 next;
  u32 off;

  if (available < sizeof (*ip6))
    return 0;

  next = ip6->protocol;
  off = sizeof (*ip6);
  for (u32 depth = 0; depth < IP6_EXT_HDR_MAX; depth++)
    {
      const u8 *hdr;
      u32 len;

      if (next == wanted)
	{
	  *offset = off;
	  return 1;
	}

      if (off + sizeof (ip6_ext_header_t) > available)
	return 0;
      hdr = (const u8 *) ip6 + off;

      if (ip6_ext_hdr (next))
	len = (((const ip6_ext_header_t *) hdr)->n_data_u64s + 1) << 3;
      else if (next == IP_PROTOCOL_IPV6_FRAGMENTATION)
	{
	  const ip6_frag_hdr_t *frag = (const ip6_frag_hdr_t *) hdr;
	  if (off + sizeof (*frag) > available || ip6_frag_hdr_offset (frag) != 0)
	    return 0;
	  len = sizeof (*frag);
	}
      else if (next == IP_PROTOCOL_IPSEC_AH)
	len = (((const ip6_ext_header_t *) hdr)->n_data_u64s + 2) << 2;
      else
	return 0;

      if (len < sizeof (ip6_ext_header_t) || off + len > available)
	return 0;
      next = hdr[0];
      off += len;
    }

  return 0;
}

/*
 * Per-buffer metadata stash, overlaid on vnet_buffer2(b)->unused[].
 *
 * The capture node writes orig_rx_sw_if_index (the phy/sub-if the
 * packet was actually received on) and a magic cookie; downstream
 * redirect nodes only consume orig_rx_sw_if_index when the cookie
 * matches.  Storing inside opaque2 (rather than a global sidecar
 * keyed by buffer_index) gives us four properties for free:
 *
 *   1. Survives vlib_buffer_clone / vlib_buffer_copy -- VPP memcpys
 *      opaque2 wholesale into every clone, so the value follows the
 *      packet through l2-flood, ip-mcast-replicate, mpls-replicate,
 *      etc.  A bi-keyed external sidecar cannot do this without a
 *      core-VPP patch on every clone caller.
 *
 *   2. No memory allocation, no per-thread or atomic ops in the hot
 *      path -- the slot is buffer-local.
 *
 *   3. Self-cleaning -- when the buffer is freed and recycled, opaque2
 *      is reset by vlib_buffer_pool_init, so a stale value cannot be
 *      mis-attributed to an unrelated future packet.
 *
 *   4. No conflict with stock VPP plugins.  We use only the trailing
 *      `unused[]` array of vnet_buffer_opaque2_t, never the union with
 *      the nat / cnat scratch fields.  The magic cookie defends
 *      against the (unlikely) case where another plugin also decides
 *      to scribble in unused[]: a non-matching cookie causes us to
 *      ignore the slot rather than redirect to a bogus interface.
 */
#define SONIC_EXT_BUFFER_MAGIC 0x534e4358u  /* 'SNCX' */

/* --- Deferred egress mirror (Everflow MIRROR_EGRESS) extension ----------- *
 * mirror_sw_if_index carries the ERSPAN/GRE tunnel the deferred clone is
 * sent to; the ACL dataplane node stamps it into the cookie alongside the
 * magic when a matched rule has ACL_MIRROR_F_DEFERRED set. */
#define SONIC_EXT_INVALID_SW_IF_INDEX ~0U
#define SONIC_EXT_INVALID_VLAN_TAG ~0U
/* Buffer flag: a deferred egress-mirror clone is pending on this packet. */
#define SONIC_EXT_BUFFER_F_MIRROR_PENDING VNET_BUFFER_F_AVAIL1

/*
 * AVAIL2 is SONIC_EXT_BUFFER_F_VNET_BUF, defined in sonic_ext_vnet_buf.h
 * beside the facility it guards.  Noted here so the plugin's claims on the
 * nine shared AVAIL bits stay visible in one place.
 */

/* A DHCP COPY has already emitted its direct CPU copy. L2 flood clones
 * inherit this flag, so the BVI receive copy can be discarded on the host
 * tap before delivery and metering happen a second time. Buffer allocation
 * clears it through VPP's per-incarnation flags template. */
#define SONIC_EXT_BUFFER_F_DHCP_CPU_COPIED VNET_BUFFER_F_AVAIL3

/*
 * orig_vlan_tag: outermost 802.1Q (or 802.1ad) tag observed on the
 * wire frame at sonic-ext-capture time, stored as raw 4 bytes in
 * network byte order: [TPID (2)] [TCI (2)].  Zero means the frame
 * was untagged when it entered VPP.
 *
 * Why a wire-byte snapshot rather than reconstructing from sub-if
 * config (vnet_sw_interface_t.sub.eth.outer_vlan_id):
 *
 *   The capture node runs on the device-input arc, before
 *   ethernet-input has classified the inner ethertype.  At that
 *   point VLIB_RX is the *main* hw_if_index, not the sub-if -- the
 *   sub-if is only resolved later in ethernet-input.  So at capture
 *   time we cannot look up sub-if vlan config; we can only see the
 *   wire bytes.  The snapshot is also future-proof against bridge
 *   configurations that don't use a vlan sub-interface at all
 *   (e.g. bridge group with explicit l2 vtr push/pop on the main
 *   phy), and against multi-tag stacks where the inner tag is not
 *   recoverable from a single sub.eth.outer_vlan_id field.
 *
 * The aggr-tap-redirect node uses this saved tag to re-push the
 * VLAN before re-entering interface-output on the member tap, so
 * Linux observes the same wire frame on the right netdev.
 */
typedef struct
{
  u32 magic;
  u32 orig_rx_sw_if_index;
  u32 orig_vlan_tag;
  /*
   * Packet-trim per-buffer state, sharing the same opaque2->unused
   * overlay as the capture cookie above.
   *
   *   orig_queue -- the SONiC egress queue the packet resolved to on
   *     the admission-failure path.  The admission node writes it
   *     immediately before diverting the buffer to sonic-ext-trim, and
   *     the trim node reads it in the same graph dispatch, so it is
   *     always freshly written before it is read -- it never depends on
   *     opaque2 surviving (or being cleared on) a buffer recycle.
   *
   * NOTE: there is deliberately NO persistent "already trimmed" flag
   * here.  opaque2 is NOT zeroed on buffer recycle, so a stale flag on
   * a reused buffer would make a genuinely new packet read as
   * "already trimmed" and wrongly bypass trimming.  Instead the trim
   * node re-injects the truncated copy straight to
   * "interface-output-arc-end" (past the admission feature), so a
   * trimmed copy never re-enters admission and no one-shot guard is
   * needed.  See trim_node.c / trim_admission_node.c.
   */
  u8 orig_queue;  /* 0 .. SONIC_EXT_TRIM_MAX_QUEUES-1 */
  u8 trim_pad0;
  u16 trim_pad1;
  u32 mirror_sw_if_index; /* deferred egress mirror dst; ~0 = none */
} sonic_ext_buffer_opaque_t;

STATIC_ASSERT (sizeof (sonic_ext_buffer_opaque_t) <=
                 sizeof (((vnet_buffer_opaque2_t *) 0)->unused),
               "sonic-ext per-buffer metadata too large for opaque2->unused");

static_always_inline sonic_ext_buffer_opaque_t *
sonic_ext_buffer (vlib_buffer_t *b)
{
  return (sonic_ext_buffer_opaque_t *) vnet_buffer2 (b)->unused;
}

/*
 * ------------------------------------------------------------------
 * Packet trimming (SAI DROP_AND_TRIM) -- software egress admission +
 * trim action.  See trim_admission_node.c, trim_node.c, trim_api.c.
 *
 * VPP has no native egress QoS / per-queue admission substrate, so the
 * SONiC scheduler's PIR is a dataplane no-op today.  The trim feature
 * therefore carries its own software per-(port,queue) token-bucket
 * admission shim, sized from the SONiC scheduler rate and buffer
 * profile capacity programmed over the binary API.  A blocking
 * scheduler (PIR=1) drains the bucket and produces a real, recoverable
 * admission failure that the trim action node hangs off of.
 * ------------------------------------------------------------------
 */

/* SONiC standard egress unicast queue count per port (0..7). */
#define SONIC_EXT_TRIM_MAX_QUEUES 8

/* Sentinel returned by sonic_ext_trim_packet_queue() for frames that have
 * no resolvable egress queue (non-IP: ARP, LACP, LLDP, ...).  Such frames
 * must never be policed or trimmed -- indexing port->q[] with this value
 * is invalid, so callers must treat it as an unconditional bypass. */
#define SONIC_EXT_TRIM_QUEUE_NONE 0xff

/* PACKET_TRIM_DSCP_RESOLUTION_MODE. */
typedef enum
{
  SONIC_EXT_TRIM_DSCP_MODE_SYMMETRIC = 0, /* use configured dscp_value */
  SONIC_EXT_TRIM_DSCP_MODE_FROM_TC = 1,   /* asymmetric egress-map lookup */
} sonic_ext_trim_dscp_mode_t;

/*
 * One software token-bucket admission slot per egress queue.  Bytes are
 * the unit throughout: `tokens` and `capacity_bytes` are byte credits,
 * `rate_bytes_per_sec` is the SONiC scheduler PIR translated to bytes/s
 * (0 => the queue can never admit, i.e. a fully blocking scheduler).
 *
 * NOTE (first increment): the bucket is a single shared slot per
 * (sw_if_index, queue) with no per-worker partitioning, so it assumes
 * the vlab single-worker dispatch.  Multi-worker correctness (per-thread
 * buckets or atomics) is deferred; see the HLD limitations.
 */
typedef struct
{
  u8 eligible;             /* queue buffer profile uses DROP_AND_TRIM */
  u8 configured;           /* a rate/capacity has been programmed */
  u64 rate_bytes_per_sec;  /* SONiC scheduler PIR, bytes/s (0 => block) */
  u64 capacity_bytes;      /* token-bucket depth (buffer profile size) */
  f64 tokens;              /* current byte credits */
  f64 last_refill;         /* vlib_time_now() of the last refill */
} sonic_ext_trim_queue_t;

typedef struct
{
  /* Tracks whether the interface-output "sonic-ext-trim-admission" feature
   * arc is currently enabled on this port. vnet_feature_enable_disable() is
   * not idempotent (each enable pushes another instance of the arc onto the
   * feature config), so the arc is toggled only on an actual transition of
   * this state. Zero-initialized by vec_validate -> arc off. */
  u8 feature_enabled;
  sonic_ext_trim_queue_t q[SONIC_EXT_TRIM_MAX_QUEUES];
} sonic_ext_trim_port_t;

/* Per-interface Everflow mirror encap fixup config, indexed by the mirror
 * GRE tunnel sw_if_index. */
typedef struct
{
  u16 gre_protocol; /* GRE ethertype to stamp on the wire (host order) */
  u8 ttl;	    /* exact outer IPv4 TTL to stamp */
  u8 enabled;
} sonic_ext_mirror_encap_cfg_t;

/* Who wires a feature: VPP-owned ones are wired by this plugin, SAIVPP-owned
 * ones are only stored here and answered via sonic_ext_feature_get(). */
typedef enum
{
  SONIC_EXT_OWNER_VPP,
  SONIC_EXT_OWNER_SAIVPP,
} sonic_ext_feature_owner_t;

/* Configurable features: _ (symbol, field, name, default_enabled, owner) */
#define foreach_sonic_ext_feature                                             \
  _ (PUNT_VIA_MEMBER, punt_via_member, "punt-via-member", 1, VPP)             \
  _ (HOST_XC, host_xc, "host-xc", 1, VPP)                                     \
  _ (DROP_MEMBER_STATS, drop_member_stats, "drop-member-stats", 1, VPP)       \
  _ (ND_PUNT, nd_punt, "nd-punt", 1, VPP)                                     \
  _ (COPP, copp, "copp", 1, VPP)                                               \
  _ (TRIM, trim, "trim", 1, VPP)                                               \
  _ (IP2ME, ip2me, "ip2me", 1, SAIVPP)                                        \
  _ (L2_TRAP_FIXUP, l2_trap_fixup, "l2-trap-fixup", 1, SAIVPP)                \
  _ (L2_VLAN_FILTER, l2_vlan_filter, "l2-vlan-filter", 1, SAIVPP)             \
  _ (PBH, pbh, "pbh", 1, SAIVPP)                                              \
  _ (IFACE_LOOPBACK, iface_loopback, "iface-loopback", 1, SAIVPP)

/* Router-interface loopback (hairpin) packet action values, carried in the
 * sonic_ext_iface_loopback_set_action API and stored per sw_if_index. */
#define SONIC_EXT_LOOPBACK_ACTION_FORWARD 0
#define SONIC_EXT_LOOPBACK_ACTION_DROP    1

typedef enum
{
#define _(symbol, field, name, default_enabled, owner)                        \
  SONIC_EXT_FEATURE_##symbol,
  foreach_sonic_ext_feature
#undef _
    SONIC_EXT_FEATURE_COUNT,
} sonic_ext_feature_id_t;

/*
 * sonic-ext-copp-ifout: interface-output CoPP policer for
 * ARP/LACP/LLDP/UDLD/TTL_ERROR/NEIGHBOR_MISS.
 */
#define SONIC_EXT_COPP_IFOUT_MAX_ENTRIES 16
#define SONIC_EXT_COPP_IFOUT_NAME_LEN 64

/*
 * Internal bind-table key sonic-ext-copp-ifout uses for the UDLD row --
 * NOT a real wire ethertype (UDLD is not Ethernet-II framed; see
 * copp_ifout_node.c's structural LLC/SNAP/Cisco-OUI match, which is what
 * actually classifies real UDLD traffic). SwitchVppHostifTrap.cpp's SAI
 * classify-match builder uses this same value purely so the existing
 * ethertype-keyed bind/lookup table can be reused as a policer-name
 * lookup key, without needing a second, UDLD-only table.
 */
#define SONIC_EXT_COPP_UDLD_ETHERTYPE 0x0067

typedef struct
{
  u16 ethertype;		/* host byte order */
  u8 name[SONIC_EXT_COPP_IFOUT_NAME_LEN];
  u32 policer_index;
  u8 in_use;
  u8 match_ip4_ttl_expiring; /* TTL_ERROR trap */
  u8 match_neighbor_miss; /* per-buffer semantic reason, not an ethertype */
  u8 match_dhcp4; /* IPv4 UDP ports 67/68 */
  u8 match_dhcp6; /* IPv6 UDP ports 546/547 */
  u8 dhcp_copy; /* COPY preserves the original L2 forwarding path */
  u8 rate_is_pps;
} sonic_ext_copp_ifout_entry_t;

#define SONIC_EXT_COPP_IP2ME_MAX_ADDRS 256

typedef struct
{
  u32 addr;    /* network byte order, matches ip4_address_t.as_u32 */
  u8 in_use;
} sonic_ext_copp_ip2me_addr_t;

/*
 * sonic-ext-copp-ip2me supports more than one independently bound
 * policer on the ip4-punt arc, keyed by SAI trap group
 */
#define SONIC_EXT_COPP_IP2ME_MAX_POLICERS 8

typedef enum
{
  SONIC_EXT_COPP_IP2ME_MATCH_ADDR = 0,	/* dst IPv4 in copp_ip2me_addrs[] */
  SONIC_EXT_COPP_IP2ME_MATCH_TCP_PORT = 1, /* src OR dst TCP port == match_tcp_port */
} sonic_ext_copp_ip2me_match_kind_t;

/*
 * Generic condition for sonic_ext_copp_ip2me_bind_condition() -- a single
 * TCP-port field for now (the only real use case, BGP/BGPV6 dst 179).
 * Deliberately a struct, not a bare u16 parameter, so a future caller can
 * add another match dimension (e.g. a protocol/address field) without
 * changing this function's signature again.
 */
typedef struct
{
  u16 tcp_port; /* match if EITHER the packet's src or dst TCP port == this */
} sonic_ext_copp_ip2me_condition_t;

typedef struct
{
  u8 name[SONIC_EXT_COPP_IFOUT_NAME_LEN];
  u32 policer_index;
  u8 in_use;
  u8 match_kind; /* sonic_ext_copp_ip2me_match_kind_t */
  u16 match_tcp_port; /* only used when match_kind == MATCH_TCP_PORT */
  u8 rate_is_pps;
  u64 conform_packets;
  u64 exceed_packets;
  u64 violate_packets;
} sonic_ext_copp_ip2me_policer_t;

typedef struct
{
  /* API message ID base */
  u16 msg_id_base;

  /* Feature toggles, one per foreach_sonic_ext_feature entry. */
#define _(symbol, field, name, default_enabled, owner) u8 field;
  foreach_sonic_ext_feature
#undef _

  /* Per-sw_if_index RIF loopback (hairpin) packet action,
   * SONIC_EXT_LOOPBACK_ACTION_* (default FORWARD). Written only from the
   * main/API thread; read by the ip4/ip6-loopback output-arc nodes on
   * worker threads. Gated by the iface_loopback feature toggle above. */
  u8 *loopback_action_by_sw_if_index;

  /* Set once capture/host-xc have been enabled on all existing
   * interfaces, so that toggling on/off is idempotent. */
  u8 capture_enabled;
  u8 host_xc_enabled;
  u8 glean_redirect_enabled;
  u8 aggr_tap_redirect_enabled;
  u8 dhcp_l2_enabled;

  /* Deferred egress mirror: set once the sonic-ext-egress-mirror feature
   * has been enabled on all interface-output arcs; gated by a refcount of
   * installed MIRROR_EGRESS actions so the arc cost is only paid while the
   * feature is in use (HLD 12.4). */
  u8 egress_mirror_arc_enabled;
  u32 active_egress_mirror_actions;

  /* SAI TTL_ERROR trap installed? Read by sonic_ext_ttl_error_should_
   * punt(), the dlsym hook core VPP's ip4-rewrite calls on every
   * genuinely-transiting TTL-expired packet (see copp_ttl_punt_node.c
   * and vppbld/patches/0021-ip4-redirect-ttl-expired-to-copp-punt-
   * hook.patch). A single global flag, not a per-ethertype table entry
   * like copp-ifout -- TTL_ERROR is one SAI trap type with one binary
   * "is it installed" question at this call site. */
  u8 copp_ttl_punt_enabled;

  /* Everflow mirror encap fixup: per-interface (mirror GRE tunnel
   * sw_if_index) outer TTL + GRE protocol override, applied on the
   * ethernet-output arc after GRE encap.  vec indexed by sw_if_index. */
  sonic_ext_mirror_encap_cfg_t *mirror_encap_cfg;

  /* Counters (per-feature, per-thread accounting kept in node
   * registrations; these are summary counters for `show sonic-ext`). */
  u64 captures;
  u64 aggr_tap_redirects;
  u64 glean_redirects;
  u64 host_xc_direct;
  u64 l2_trap_fixups;
  u64 ip2me_hits;

  /* ---- packet trimming ---- */
  u16 copp_msg_id_base;   /* sonic_ext_copp binary API base id */
  u16 trim_msg_id_base;   /* binary API base id (setup_message_id_table) */
  u8 trim_configured;     /* any global trim policy pushed yet */

  /* Global trim policy (SAI SWITCH_ATTR_PACKET_TRIM_*). */
  u16 trim_size;          /* bytes retained; 0 => unset */
  u8 trim_dscp_mode;      /* sonic_ext_trim_dscp_mode_t */
  u8 trim_dscp_value;     /* symmetric DSCP (0..63) */
  u8 trim_tc;             /* TC used for FROM_TC egress-map lookup */
  u8 trim_queue;          /* static trim queue index (0..7) */

  /*
   * Global DSCP(0..63) -> egress queue(0..7) resolution used by the
   * admission shim to classify a packet to its original queue.  Pushed
   * by SAI-VPP as the composition of the SONiC DSCP_TO_TC and
   * TC_TO_QUEUE maps, so the plugin does not replicate both maps.
   */
  u8 dscp_to_queue[64];

  /*
   * Per-egress-port admission state, a vec indexed by sw_if_index; each
   * element holds one token bucket per queue.  NULL/short entries mean
   * "no trim admission configured on that port" (feature is a no-op).
   */
  sonic_ext_trim_port_t *trim_ports;

  /* Summary counters surfaced by `show sonic-ext trim` and SAI stats. */
  u64 trim_sent;         /* truncated copies transmitted */
  u64 trim_drop;         /* eligible but trim queue also congested */
  u64 trim_admit_fail;   /* original-queue admission failures observed */
  u64 mirror_encap_fixups;

  /* sonic-ext-copp-ifout: ethertype -> policer binding table. */
  sonic_ext_copp_ifout_entry_t copp_ifout_entries[SONIC_EXT_COPP_IFOUT_MAX_ENTRIES];
  u32 copp_ifout_n_entries;
  u32 copp_neighbor_miss_entry_index;
  u64 copp_ifout_conform_packets[SONIC_EXT_COPP_IFOUT_MAX_ENTRIES];
  u64 copp_ifout_exceed_packets[SONIC_EXT_COPP_IFOUT_MAX_ENTRIES];
  u64 copp_ifout_violate_packets[SONIC_EXT_COPP_IFOUT_MAX_ENTRIES];

  sonic_ext_copp_ip2me_addr_t copp_ip2me_addrs[SONIC_EXT_COPP_IP2ME_MAX_ADDRS];
  u32 copp_ip2me_n_addrs;
  sonic_ext_copp_ip2me_policer_t copp_ip2me_policers[SONIC_EXT_COPP_IP2ME_MAX_POLICERS];
  u32 copp_ip2me_n_policers;
} sonic_ext_main_t;

extern sonic_ext_main_t sonic_ext_main;

extern vlib_node_registration_t sonic_ext_capture_node;
extern vlib_node_registration_t sonic_ext_aggr_tap_redirect_node;
extern vlib_node_registration_t sonic_ext_glean_redirect_node;
extern vlib_node_registration_t sonic_ext_host_xc_node;
extern vlib_node_registration_t sonic_ext_l2_trap_fixup_node;
extern vlib_node_registration_t sonic_ext_l2_vlan_filter_node;
extern vlib_node_registration_t sonic_ext_ip2me_ip4_node;
extern vlib_node_registration_t sonic_ext_ip2me_ip6_node;
extern vlib_node_registration_t sonic_ext_egress_mirror_node;
extern vlib_node_registration_t sonic_ext_mirror_encap_fixup_node;
extern vlib_node_registration_t sonic_ext_copp_ifout_node;
extern vlib_node_registration_t sonic_ext_copp_ip2me_node;
extern vlib_node_registration_t sonic_ext_copp_udld_node;
extern vlib_node_registration_t sonic_ext_copp_ttl_punt_node;
extern vlib_node_registration_t sonic_ext_ip4_loopback_node;
extern vlib_node_registration_t sonic_ext_ip6_loopback_node;

/* Enable / disable sonic-ext-capture on a given interface.  No-op if
 * the capture sidecar is not yet initialized. */
void sonic_ext_capture_enable_disable (u32 sw_if_index, int enable);

/* Enable / disable sonic-ext-host-xc on a given interface. */
void sonic_ext_host_xc_enable_disable (u32 sw_if_index, int enable);

/* Enable / disable sonic-ext-aggr-tap-redirect on a given sw_if_index
 * (always the LCP host tap of an aggregate phy -- BVI today, bond
 * tomorrow).  Driven from the LCP pair add/del callback. */
void sonic_ext_aggr_tap_redirect_enable_disable (u32 sw_if_index, int enable);

/* Enable / disable sonic-ext-glean-redirect.  Global (the ip4-drop /
 * ip6-drop arcs dispatch with sw_if_index 0), so there is no
 * per-interface argument; the node scopes itself per packet via the
 * capture cookie and the glean/arp adjacency check. */
void sonic_ext_glean_redirect_enable_disable (int enable);

/* Redirect a captured packet to the LCP host tap of its ingress phy.
 * Restores the original L2 header and outer VLAN tag before updating
 * VLIB_TX.  excluded_tap prevents redirecting back to the current tap. */
int sonic_ext_redirect_to_ingress_tap (vlib_buffer_t *b, u32 orig_rx,
				       u32 excluded_tap, u32 *host_tap,
				       u16 *pushed_tpid,
				       u16 *pushed_vlan_id);

/* Enable / disable the "receive-DPO check before ACL" feature on a
 * given L2 port sw_if_index.  Registers sonic-ext-ip2me-ip4/ip6 on the
 * l2-input-ip4 / l2-input-ip6 feature arcs, ahead of the ACL plugin, so
 * that ip2me (destined-to-this-switch) traffic bypasses any ingress drop
 * ACL that would otherwise discard it.  Driven from the SAI-VPP layer
 * wherever such a drop ACL is bound. */
void sonic_ext_ip2me_enable_disable (u32 sw_if_index, int enable);

/* Enable / disable post-VLAN-admission DHCP interception on one L2 port. */
void sonic_ext_dhcp_enable_disable (u32 sw_if_index, int enable);

/* Suppress a DHCP COPY's later BVI receive copy on an LCP host tap. */
void sonic_ext_dhcp_copy_suppress_enable_disable (u32 sw_if_index,
					  int enable);

/* Toggle accessors used by CLI and node fast paths. */
void sonic_ext_set_punt_via_member (u8 is_enable);
void sonic_ext_neighbor_miss_enable_redirect (void);
/* Attach capture and the global drop-arc redirect used by semantic CoPP
 * traps whose packet would otherwise be consumed before the punt arcs. */
void sonic_ext_copp_enable_drop_redirect (void);
void sonic_ext_set_host_xc (u8 is_enable);
void sonic_ext_set_nd_punt (u8 is_enable);

/* Enable sonic-ext-capture on every existing LCP pair's wire phy.  Capture
 * has no toggle of its own: it is enabled whenever one of the features that
 * consumes its per-buffer cookie is. */
void sonic_ext_capture_enable_all (void);

/* Enable capture plus the DHCP l2-input-ip4/ip6 features on every existing
 * interface; the sw-interface callback handles interfaces created later. */
void sonic_ext_copp_dhcp_enable_all (void);

/* Returns non-zero if phy_sw_if_index is an "aggregate" parent whose
 * LCP host tap should have the aggr-tap-redirect feature enabled --
 * today that means a BVI, a bond / port-channel master, or a routed
 * sub-interface of a bond.  Used by the LCP pair add callback. */
int sonic_ext_phy_is_aggregate (u32 phy_sw_if_index);

/* Returns non-zero iff phy_sw_if_index is a BVI (bridge-virtual
 * interface).  Distinct from is_aggregate so future bond support
 * can opt out of bvi-specific features (bcast-redirect runs on
 * the BVI's own ip4-unicast arc; today only BVIs need it). */
int sonic_ext_phy_is_bvi (u32 phy_sw_if_index);

/* Returns non-zero iff phy_sw_if_index is a bond (port-channel) master
 * or a sub-interface whose parent hw is a bond master.  Used to detect
 * port-channel aggregates and to funnel tap-less bonded sub-interface
 * punts to the bond master host tap. */
int sonic_ext_phy_is_bond (u32 phy_sw_if_index);
/* ---- packet trimming (trim_admission_node.c / trim_node.c / trim_api.c) ---- */

extern vlib_node_registration_t sonic_ext_trim_admission_node;
extern vlib_node_registration_t sonic_ext_trim_node;

/* Enable/disable the sonic-ext-trim-admission feature on an egress
 * interface-output arc.  Driven from the binary API when a port gains or
 * loses trim-eligible queues. */
int sonic_ext_trim_enable_disable (u32 sw_if_index, int enable);

/* Get (optionally allocate) the per-port admission state for sw_if_index.
 * Returns NULL when create==0 and the port has no state. */
sonic_ext_trim_port_t *sonic_ext_trim_port_get (u32 sw_if_index, int create);

/* Program one egress (port, queue) admission slot and (re)evaluate whether
 * the sonic-ext-trim-admission feature should be enabled on the port.
 * Shared by the binary API handler and the debug CLI. Returns 0 on success or
 * a nonzero vnet API error if the feature-arc enable/disable transition failed,
 * so the caller can surface the failure (the queue state itself is always
 * programmed). */
int sonic_ext_trim_queue_program (u32 sw_if_index, u32 queue, int eligible,
                                  u64 rate_bytes_per_sec, u64 capacity_bytes);

/* Software token-bucket admission decision for one packet of `len` bytes
 * on queue `q`.  Refills from rate_bytes_per_sec, then either debits and
 * returns 1 (admitted) or leaves the bucket and returns 0 (rejected).  A
 * queue with rate_bytes_per_sec==0 always rejects once drained. */
int sonic_ext_trim_bucket_admit (vlib_main_t *vm, sonic_ext_trim_queue_t *q,
                                 u32 len);

/* Register the trim binary API message table.  Called from
 * sonic_ext_init(); sets sonic_ext_main.trim_msg_id_base. */
clib_error_t *sonic_ext_trim_api_hookup (vlib_main_t *vm);

/* Register the compatibility-isolated CoPP binary API message table. Called
 * from sonic_ext_init(); sets sonic_ext_main.copp_msg_id_base. */
clib_error_t *sonic_ext_copp_api_hookup (vlib_main_t *vm);

/* Policy is programmed on physical ports, while VLAN egress keeps the
 * sub-interface in VLIB_TX. Do not change VLIB_TX: the rewrite/TX path needs it. */
static_always_inline u32
sonic_ext_trim_egress_port_sw_if_index (vlib_buffer_t *b)
{
  vnet_main_t *vnm = vnet_get_main ();
  u32 sw_if_index = vnet_buffer (b)->sw_if_index[VLIB_TX];

  if (PREDICT_FALSE (
	sw_if_index == ~0 ||
	vnet_get_sw_interface_or_null (vnm, sw_if_index) == 0))
    return ~0;

  return vnet_get_sup_hw_interface (vnm, sw_if_index)->sw_if_index;
}

/*
 * ------------------------------------------------------------------
 * Shared L3 classification / DSCP rewrite inlines used by both the
 * admission node (classify the packet to its egress queue) and the
 * trim action node (rewrite the trimmed copy's DSCP).
 *
 * Both nodes run on the interface-output arc, where the buffer's
 * current data is positioned at the start of the (rewritten) L2
 * ethernet header for normal transit/routed traffic.  We parse the
 * ethertype, skip up to two VLAN tags, and locate the IPv4/IPv6
 * header.  Non-IP frames have no resolvable queue (sentinel
 * SONIC_EXT_TRIM_QUEUE_NONE) and are never trimmed.
 * ------------------------------------------------------------------
 */

/* Locate the L3 header after L2 + up to two VLAN tags.  Returns a
 * pointer to the L3 header and writes the (host-order) L3 ethertype to
 * *etype.  Caller must ensure the bytes are present (trim_size and real
 * MTU packets always carry the IP header). */
static_always_inline u8 *
sonic_ext_trim_find_l3 (vlib_buffer_t *b, u16 *etype)
{
  u8 *eth = vlib_buffer_get_current (b);
  u16 et = clib_net_to_host_u16 (*(u16 *) (eth + 12));
  u32 o = 14;
  int n = 0;

  while ((et == ETHERNET_TYPE_VLAN || et == ETHERNET_TYPE_DOT1AD ||
	  et == ETHERNET_TYPE_VLAN_9100) &&
	 n < 2)
    {
      et = clib_net_to_host_u16 (*(u16 *) (eth + o + 2));
      o += 4;
      n++;
    }

  *etype = et;
  return eth + o;
}

/* Extract the 6-bit DSCP from an IPv4/IPv6 packet.  Returns 1 and sets
 * *dscp for IP packets, 0 for non-IP (caller treats as queue 0). */
static_always_inline int
sonic_ext_trim_packet_dscp (vlib_buffer_t *b, u8 *dscp)
{
  u16 et;
  u8 *l3 = sonic_ext_trim_find_l3 (b, &et);

  if (et == ETHERNET_TYPE_IP4)
    {
      ip4_header_t *ip = (ip4_header_t *) l3;
      *dscp = ip->tos >> 2;
      return 1;
    }
  if (et == ETHERNET_TYPE_IP6)
    {
      ip6_header_t *ip = (ip6_header_t *) l3;
      u32 v =
	clib_net_to_host_u32 (ip->ip_version_traffic_class_and_flow_label);
      *dscp = (v >> 22) & 0x3f;
      return 1;
    }
  return 0;
}

/* Resolve the egress queue (0..7) for a packet from its DSCP via the
 * SAI-pushed dscp_to_queue table.  Non-IP packets (ARP/LACP/LLDP/...)
 * have no DSCP and therefore no resolvable egress queue: they return the
 * SONIC_EXT_TRIM_QUEUE_NONE sentinel so the admission node bypasses them
 * unconditionally and never trims a control frame (which also keeps LAG
 * control-plane traffic intact when queue 0 is trim-eligible). */
static_always_inline u8
sonic_ext_trim_packet_queue (sonic_ext_main_t *sem, vlib_buffer_t *b)
{
  u8 dscp;

  if (!sonic_ext_trim_packet_dscp (b, &dscp))
    return SONIC_EXT_TRIM_QUEUE_NONE;
  return sem->dscp_to_queue[dscp & 0x3f];
}

/* Rewrite the DSCP of an (already truncated) trimmed copy to new_dscp,
 * fixing the IPv4 header checksum.  IPv6 carries no header checksum. */
static_always_inline void
sonic_ext_trim_rewrite_dscp (vlib_buffer_t *b, u8 new_dscp)
{
  u16 et;
  u8 *l3 = sonic_ext_trim_find_l3 (b, &et);

  if (et == ETHERNET_TYPE_IP4)
    {
      ip4_header_t *ip = (ip4_header_t *) l3;
      ip->tos = (u8) ((new_dscp << 2) | (ip->tos & 0x3));
      ip->checksum = 0;
      ip->checksum = ip4_header_checksum (ip);
    }
  else if (et == ETHERNET_TYPE_IP6)
    {
      ip6_header_t *ip = (ip6_header_t *) l3;
      u32 v =
	clib_net_to_host_u32 (ip->ip_version_traffic_class_and_flow_label);
      v = (v & ~(0x3full << 22)) | (((u32) (new_dscp & 0x3f)) << 22);
      ip->ip_version_traffic_class_and_flow_label = clib_host_to_net_u32 (v);
    }
}

/* Deferred egress mirror refcount toggle.  enable=1 installs a
 * MIRROR_EGRESS action (enabling sonic-ext-egress-mirror on every
 * interface-output arc on the 0->1 transition); enable=0 removes one
 * (disabling on the 1->0 transition).  Driven from the SAI-VPP layer per
 * Everflow egress mirror session. */
int sonic_ext_egress_mirror_enable_disable (u8 enable);

/* Enable / disable the Everflow mirror encap fixup on a mirror GRE tunnel
 * sw_if_index.  enable=1 records {gre_protocol, ttl} and turns on the
 * sonic-ext-mirror-encap-fixup feature on that interface's ethernet-output
 * arc; calling it again while enabled just updates the values; enable=0
 * removes the feature.  Driven from the SAI-VPP layer per Everflow ERSPAN
 * session. */
int sonic_ext_mirror_encap_fixup_enable_disable (u32 sw_if_index,
						 u16 gre_protocol, u8 ttl,
						 int enable);

/* Registered with the ACL plugin as its deferred mirror stamper: records the
 * mirror destination in the sonic_ext cookie for the late interface-output
 * clone.  Always accepts, so returns 1. */
int sonic_ext_acl_deferred_mirror_stamp (vlib_buffer_t *b, u32 rx_sw_if_index,
					 u32 mirror_sw_if_index);

/*
 * enable/disable the interface-output CoPP policer feature on a given
 * TAP sw_if_index.
 */
void sonic_ext_copp_ifout_enable_disable (u32 sw_if_index, int enable);
void sonic_ext_copp_udld_enable (vlib_main_t *vm);

/*
 * look up an entry by wire ethertype/length value.
 * Returns -1 if no matching in-use entry exists.
 */
int sonic_ext_copp_ifout_find_entry (sonic_ext_main_t *sem, u16 ethertype);

/*
 * bind (or unbind) an ethertype -> policer-name
 */
int sonic_ext_copp_ifout_bind (u16 ethertype, const char *policer_name,
			       int is_bind, int match_ip4_ttl_expiring);

/* Bind the semantic NEIGHBOR_MISS reason to a copp-ifout policer row. */
int sonic_ext_copp_neighbor_miss_bind (const char *policer_name, int is_bind);
int sonic_ext_copp_neighbor_miss_is_bound (sonic_ext_main_t *sem);
/* Bind one family of the semantic DHCP trap to copp-ifout. */
int sonic_ext_copp_dhcp_bind (const char *policer_name, int is_ipv6,
			      int is_copy, int is_bind);
int sonic_ext_copp_dhcp_is_bound (sonic_ext_main_t *sem, int is_ipv6);
int sonic_ext_copp_dhcp_is_copy (sonic_ext_main_t *sem, int is_ipv6);
int sonic_ext_copp_ip2me_addr_add_del (u32 addr, int is_add);
int sonic_ext_copp_ip2me_bind (const char *policer_name, int is_bind);
int sonic_ext_copp_ip2me_bind_condition (
  const char *policer_name, const sonic_ext_copp_ip2me_condition_t *condition,
  int is_bind);
int sonic_ext_copp_ttl_punt_bind (int is_bind);

/* Set the per-interface RIF loopback (hairpin) packet action and enable or
 * disable the sonic-ext-ip4/ip6-loopback output-arc nodes accordingly.
 * Returns 0 on success, a VNET_API_ERROR_* on failure. Honours the
 * iface_loopback feature toggle. */
int sonic_ext_iface_loopback_set_action (u32 sw_if_index, u8 action);

#endif /* __included_sonic_ext_h__ */
