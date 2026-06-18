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
} sonic_ext_buffer_opaque_t;

STATIC_ASSERT (sizeof (sonic_ext_buffer_opaque_t) <=
                 sizeof (((vnet_buffer_opaque2_t *) 0)->unused),
               "sonic-ext per-buffer metadata too large for opaque2->unused");

static_always_inline sonic_ext_buffer_opaque_t *
sonic_ext_buffer (vlib_buffer_t *b)
{
  return (sonic_ext_buffer_opaque_t *) vnet_buffer2 (b)->unused;
}

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
  u64 bcast_punts;
} sonic_ext_main_t;

extern sonic_ext_main_t sonic_ext_main;

extern vlib_node_registration_t sonic_ext_capture_node;
extern vlib_node_registration_t sonic_ext_aggr_tap_redirect_node;
extern vlib_node_registration_t sonic_ext_host_xc_node;
extern vlib_node_registration_t sonic_ext_bcast_redirect_node;

/* Enable / disable sonic-ext-capture on a given interface.  No-op if
 * the capture sidecar is not yet initialized. */
void sonic_ext_capture_enable_disable (u32 sw_if_index, int enable);

/* Enable / disable sonic-ext-host-xc on a given interface. */
void sonic_ext_host_xc_enable_disable (u32 sw_if_index, int enable);

/* Enable / disable sonic-ext-aggr-tap-redirect on a given sw_if_index
 * (always the LCP host tap of an aggregate phy -- BVI today, bond
 * tomorrow).  Driven from the LCP pair add/del callback. */
void sonic_ext_aggr_tap_redirect_enable_disable (u32 sw_if_index, int enable);

/* Enable / disable sonic-ext-bcast-redirect on a given sw_if_index.
 * Today driven from the LCP pair add/del callback gated on
 * sonic_ext_phy_is_bvi(); the node itself is generic and could be
 * extended to non-BVI aggregates in the future. */
void sonic_ext_bcast_redirect_enable_disable (u32 sw_if_index, int enable);

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

#endif /* __included_sonic_ext_h__ */
