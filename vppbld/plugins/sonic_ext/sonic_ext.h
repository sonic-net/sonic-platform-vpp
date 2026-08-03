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

typedef struct
{
  /* Global feature toggles. */
  u8 punt_via_member;
  u8 host_xc;

  /* Set once capture/host-xc have been enabled on all existing
   * interfaces, so that toggling on/off is idempotent. */
  u8 capture_enabled;
  u8 host_xc_enabled;

  /* Counters (per-feature, per-thread accounting kept in node
   * registrations; these are summary counters for `show sonic-ext`). */
  u64 captures;
  u64 aggr_tap_redirects;
  u64 host_xc_direct;
  u64 l2_trap_fixups;

  /* ---- packet trimming ---- */
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
} sonic_ext_main_t;

extern sonic_ext_main_t sonic_ext_main;

extern vlib_node_registration_t sonic_ext_capture_node;
extern vlib_node_registration_t sonic_ext_aggr_tap_redirect_node;
extern vlib_node_registration_t sonic_ext_host_xc_node;
extern vlib_node_registration_t sonic_ext_l2_trap_fixup_node;

/* Enable / disable sonic-ext-capture on a given interface.  No-op if
 * the capture sidecar is not yet initialized. */
void sonic_ext_capture_enable_disable (u32 sw_if_index, int enable);

/* Enable / disable sonic-ext-host-xc on a given interface. */
void sonic_ext_host_xc_enable_disable (u32 sw_if_index, int enable);

/* Enable / disable sonic-ext-aggr-tap-redirect on a given sw_if_index
 * (always the LCP host tap of an aggregate phy -- BVI today, bond
 * tomorrow).  Driven from the LCP pair add/del callback. */
void sonic_ext_aggr_tap_redirect_enable_disable (u32 sw_if_index, int enable);

/* Toggle accessors used by CLI and node fast paths. */
void sonic_ext_set_punt_via_member (u8 is_enable);
void sonic_ext_set_host_xc (u8 is_enable);

/* Returns non-zero if phy_sw_if_index is an "aggregate" parent whose
 * LCP host tap should have the aggr-tap-redirect feature enabled --
 * today that means BVI; in the future it will also cover bond /
 * port-channel master interfaces.  Used by the LCP pair add callback. */
int sonic_ext_phy_is_aggregate (u32 phy_sw_if_index);

/* Returns non-zero iff phy_sw_if_index is a BVI (bridge-virtual
 * interface).  Distinct from is_aggregate so future bond support
 * can opt out of bvi-specific features (bcast-redirect runs on
 * the BVI's own ip4-unicast arc; today only BVIs need it). */
int sonic_ext_phy_is_bvi (u32 phy_sw_if_index);

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

#endif /* __included_sonic_ext_h__ */
