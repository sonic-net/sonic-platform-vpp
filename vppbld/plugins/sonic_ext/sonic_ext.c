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
#include <sonic_ext/sonic_ext.h>

#include <vlib/vlib.h>
#include <vnet/plugin/plugin.h>
#include <vnet/feature/feature.h>
#include <vnet/interface.h>
#include <vnet/l2/l2_input.h>
#include <vnet/l2/l2_bvi.h>
#include <vpp/app/version.h>
#include <plugins/linux-cp/lcp_interface.h>

sonic_ext_main_t sonic_ext_main;

VLIB_PLUGIN_REGISTER () = {
  .version = SONIC_EXT_PLUGIN_BUILD_VER,
  .description = "SONiC VPP extensions: punt-via-member, host-xc",
};

/*
 * A "device-input" feature must never be toggled on a sub-interface.
 *
 * The driver input nodes start that arc from the *device's* own
 * sw_if_index -- e.g. virtio (all linux-cp taps) calls
 * vnet_feature_start_device_input (vif->sw_if_index, ...) and dpdk
 * calls it with xd->sw_if_index -- so a sub-interface never gets a
 * device-input dispatch of its own; it always rides its parent's.
 * Enabling there is therefore a no-op, but *disabling* is actively
 * destructive: vnet_config feature strings are interned and shared by
 * every interface with an identical feature set, so removing the
 * feature via the sub-interface rewrites the very config the parent is
 * still pointing at.  The parent silently loses the feature and its
 * input node falls back to the arc's end node (ethernet-input).
 *
 * Concretely: creating and then deleting a physical sub-port such as
 * Ethernet64.20 used to strip host-xc off Ethernet64's tap.  Every
 * later Linux-originated VLAN-tagged frame on that tap then reached
 * ethernet-input instead of host-xc, and -- the tap having no VLAN
 * sub-interface of its own -- was dropped as "unknown vlan".  That is
 * exactly the path a LAG sub-port's ARP takes once the port has been
 * enslaved (PortChannel1.20 -> team -> Ethernet64 tap), so the
 * neighbour never resolved and LAG sub-port forwarding died -- but
 * only when a physical sub-port had been created and removed earlier
 * in the same session.
 */
static int
sonic_ext_sw_is_sub (u32 sw_if_index)
{
  vnet_sw_interface_t *swi =
    vnet_get_sw_interface_or_null (vnet_get_main (), sw_if_index);

  return swi && swi->type == VNET_SW_INTERFACE_TYPE_SUB;
}

/*
 * Per-interface feature enable helpers.  All three live in this file
 * (rather than in the per-node files) so that the LCP pair add/del
 * callback, the sw_if_index add/del callback and the CLI all share the
 * same code path.
 */
void
sonic_ext_capture_enable_disable (u32 sw_if_index, int enable)
{
  if (sonic_ext_sw_is_sub (sw_if_index))
    return;

  vnet_feature_enable_disable ("device-input", "sonic-ext-capture",
			       sw_if_index, enable, 0, 0);
}

void
sonic_ext_host_xc_enable_disable (u32 sw_if_index, int enable)
{
  if (sonic_ext_sw_is_sub (sw_if_index))
    return;

  vnet_feature_enable_disable ("device-input", "sonic-ext-host-xc",
			       sw_if_index, enable, 0, 0);
}

void
sonic_ext_aggr_tap_redirect_enable_disable (u32 sw_if_index, int enable)
{
  vnet_feature_enable_disable ("interface-output",
			       "sonic-ext-aggr-tap-redirect", sw_if_index,
			       enable, 0, 0);
}

/*
 * Enable / disable sonic-ext-glean-redirect on the ip4-drop / ip6-drop
 * arcs.  Those arcs are dispatched with sw_if_index 0 (ip_drop_or_punt
 * hardcodes it), so this is a single global toggle -- not per phy.
 * The node scopes itself per packet: it only acts on buffers that
 * carry a capture cookie (i.e. ingressed on a real wire phy) whose
 * VLIB_TX adjacency is an unresolved glean / arp adjacency.
 */
void
sonic_ext_glean_redirect_enable_disable (int enable)
{
  vnet_feature_enable_disable ("ip4-drop", "sonic-ext-glean-redirect", 0,
			       enable, 0, 0);
  vnet_feature_enable_disable ("ip6-drop", "sonic-ext-glean-redirect", 0,
			       enable, 0, 0);
}

/*
 * Is `phy_sw_if_index` a BVI (bridge-virtual interface)?  Used by
 * the aggregate-detection helper.  Distinct from is_aggregate so
 * that future bond support can be added to is_aggregate without
 * dragging bvi-specific helpers along for the ride.
 */
int
sonic_ext_phy_is_bvi (u32 phy_sw_if_index)
{
  vnet_main_t *vnm = vnet_get_main ();
  l2input_main_t *l2im = &l2input_main;
  vnet_sw_interface_t *swi;

  if (phy_sw_if_index == ~0)
    return 0;
  swi = vnet_get_sw_interface_or_null (vnm, phy_sw_if_index);
  if (!swi || swi->type == VNET_SW_INTERFACE_TYPE_SUB)
    return 0;

  if (phy_sw_if_index < vec_len (l2im->configs))
    {
      l2_input_config_t *cfg = vec_elt_at_index (l2im->configs,
						 phy_sw_if_index);
      if (l2_input_is_bvi (cfg))
	return 1;
    }
  return 0;
}

/*
 * Is `phy_sw_if_index` a bond (port-channel) master, or a sub-interface
 * whose super (parent) hw is a bond master?  Used by the aggregate
 * detection helper and the sub-interface funnel.  We test the device
 * class name rather than linking against the bonding symbols so this
 * stays decoupled from the bonding implementation.
 */
int
sonic_ext_phy_is_bond (u32 phy_sw_if_index)
{
  vnet_main_t *vnm = vnet_get_main ();
  vnet_sw_interface_t *swi;
  vnet_hw_interface_t *hw;
  vnet_device_class_t *dc;

  if (phy_sw_if_index == ~0)
    return 0;
  swi = vnet_get_sw_interface_or_null (vnm, phy_sw_if_index);
  if (!swi)
    return 0;
  hw = vnet_get_sup_hw_interface (vnm, phy_sw_if_index);
  if (!hw)
    return 0;
  dc = vnet_get_device_class (vnm, hw->dev_class_index);
  if (dc && dc->name && !strcmp ((char *) dc->name, "bond"))
    return 1;
  return 0;
}

/*
 * Is `phy_sw_if_index` an "aggregate" parent whose linux-cp host tap
 * should receive aggr-tap-redirect?  Today that means a BVI (loop /
 * bridge-virtual interface), a bond / port-channel master, or a routed
 * sub-interface of a bond.  For the bond sub-interface case the host
 * sub-tap (be<id>.<vlan>) is where linux-cp-punt-xc / ip[46]-punt land
 * the punted frame, and aggr-tap-redirect steers it to the originating
 * member (with the wire VLAN tag re-pushed from the capture cookie).
 */
int
sonic_ext_phy_is_aggregate (u32 phy_sw_if_index)
{
  if (sonic_ext_phy_is_bvi (phy_sw_if_index))
    return 1;

  /* A bond master OR a sub-interface of a bond (is_bond resolves the
   * super hw, so it is true for both). */
  if (sonic_ext_phy_is_bond (phy_sw_if_index))
    return 1;

  return 0;
}

/*
 * lcp_itf_pair_walk callback: enable host-xc on this pair's host tap.
 * Used at toggle-on time to catch every pair that was created before
 * the operator flipped host_xc on; subsequent pair add/del go via the
 * LCP vft callbacks.
 */
static walk_rc_t
sonic_ext_host_xc_walk_enable_cb (index_t lipi, void *ctx)
{
  const lcp_itf_pair_t *lip = lcp_itf_pair_get (lipi);
  if (lip)
    sonic_ext_host_xc_enable_disable (lip->lip_host_sw_if_index, 1);
  return WALK_CONTINUE;
}

/*
 * lcp_itf_pair_walk callback: enable sonic-ext-capture on this pair's
 * phy if (and only if) the phy is a real wire port -- i.e. NOT an
 * "aggregate" pseudo-phy (BVI / bond master).  Capture's job is to
 * stamp the original wire-ingress sw_if_index + VLAN tag into the
 * buffer cookie before any L2 bridging mangles VLIB_RX, so it must
 * fire on the actual member port, not on synthetic interfaces.
 *
 * Sub-interfaces share their parent's device-input dispatch -- we
 * never enable on the sub directly, only on the parent phy.
 */
static walk_rc_t
sonic_ext_capture_walk_enable_cb (index_t lipi, void *ctx)
{
  const lcp_itf_pair_t *lip = lcp_itf_pair_get (lipi);
  if (lip && !sonic_ext_phy_is_aggregate (lip->lip_phy_sw_if_index))
    sonic_ext_capture_enable_disable (lip->lip_phy_sw_if_index, 1);
  return WALK_CONTINUE;
}

void
sonic_ext_set_punt_via_member (u8 is_enable)
{
  sonic_ext_main_t *sem = &sonic_ext_main;

  sem->punt_via_member = (is_enable != 0);

  /* Capture only fires on the wire phy side of LCP pairs (real ports,
   * not BVIs/bonds) so the original ingress sw_if_index + VLAN tag is
   * recorded before L2 bridging overwrites VLIB_RX with the BVI.  We
   * leave the capture feature enabled even after disabling
   * punt-via-member to avoid the per-interface enable/disable churn
   * (the downstream redirect node short-circuits via the cookie magic
   * check when the toggle is off). */
  if (is_enable && !sem->capture_enabled)
    {
      lcp_itf_pair_walk (sonic_ext_capture_walk_enable_cb, NULL);
      sem->capture_enabled = 1;
    }

  /* Glean-redirect is a single global feature on the ip4/ip6-drop
   * arcs (dispatched with sw_if_index 0).  Enable once; the node
   * self-scopes via the capture cookie + glean/arp adjacency check
   * and short-circuits when punt_via_member is off, so we never need
   * to disable it per-interface. */
  if (is_enable && !sem->glean_redirect_enabled)
    {
      sonic_ext_glean_redirect_enable_disable (1);
      sem->glean_redirect_enabled = 1;
    }

  /* The aggr-tap-redirect feature itself is wired per-interface from
   * the LCP pair add/del callback (sonic_ext_lcp_pair_add_cb) -- it
   * only needs to fire on the host tap of BVI/bond masters, never on
   * every phy.  No per-interface iteration here. */
}

void
sonic_ext_set_host_xc (u8 is_enable)
{
  sonic_ext_main_t *sem = &sonic_ext_main;

  sem->host_xc = (is_enable != 0);

  /* host-xc is only meaningful on LCP host taps -- it steers Linux-
   * originated traffic out the corresponding phy.  Enable on every
   * existing pair's host tap, and let the LCP pair add/del callback
   * keep the set in sync going forward.  Don't iterate every sw_if:
   * on phys / sub-ifs / BVIs the feature would always be a no-op
   * (lcp_itf_pair_find_by_host returns INDEX_INVALID) but still
   * costs a feature-arc dispatch per packet. */
  if (is_enable && !sem->host_xc_enabled)
    {
      lcp_itf_pair_walk (sonic_ext_host_xc_walk_enable_cb, NULL);
      sem->host_xc_enabled = 1;
    }
}

/*
 * LCP pair add/del: when a new linux-cp pair appears, enable the per-
 * interface sonic-ext features that apply.
 *
 *   - sonic-ext-capture on the phy   -- only if phy is a real wire
 *     port (not BVI / bond master) and punt-via-member is enabled.
 *   - sonic-ext-host-xc on the host  -- only if host-xc is enabled.
 *   - sonic-ext-aggr-tap-redirect on the host  -- only if phy is an
 *     aggregate (BVI today, bond tomorrow) and punt-via-member is on.
 *
 * DHCPv4 client broadcast is trapped at l2-input-classify by the
 * per-member classifier session installed in SwitchVppFdb.cpp; it
 * does not need any per-LCP-pair feature on the BVI.  See
 * l2_trap_fixup_node.c.
 *
 * This is the only place we know both (a) the host tap sw_if_index
 * and (b) which phy it shadows.
 */
static void
sonic_ext_lcp_pair_add_cb (lcp_itf_pair_t *lip)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  if (!lip)
    return;
  if (sem->capture_enabled
      && !sonic_ext_phy_is_aggregate (lip->lip_phy_sw_if_index))
    sonic_ext_capture_enable_disable (lip->lip_phy_sw_if_index, 1);
  if (sem->host_xc_enabled)
    sonic_ext_host_xc_enable_disable (lip->lip_host_sw_if_index, 1);
  if (sonic_ext_phy_is_aggregate (lip->lip_phy_sw_if_index))
    sonic_ext_aggr_tap_redirect_enable_disable (lip->lip_host_sw_if_index, 1);
}

static void
sonic_ext_lcp_pair_del_cb (lcp_itf_pair_t *lip)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  if (!lip)
    return;
  if (sem->capture_enabled
      && !sonic_ext_phy_is_aggregate (lip->lip_phy_sw_if_index))
    sonic_ext_capture_enable_disable (lip->lip_phy_sw_if_index, 0);
  if (sem->host_xc_enabled)
    sonic_ext_host_xc_enable_disable (lip->lip_host_sw_if_index, 0);
  if (sonic_ext_phy_is_aggregate (lip->lip_phy_sw_if_index))
    sonic_ext_aggr_tap_redirect_enable_disable (lip->lip_host_sw_if_index, 0);
}

static clib_error_t *
sonic_ext_init (vlib_main_t *vm)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  lcp_itf_pair_vft_t sonic_ext_lcp_vft = {
    .pair_add_fn = sonic_ext_lcp_pair_add_cb,
    .pair_del_fn = sonic_ext_lcp_pair_del_cb,
  };
  clib_memset (sem, 0, sizeof (*sem));
  lcp_itf_pair_register_vft (&sonic_ext_lcp_vft);

  /* Default-on: capture + aggr-tap-redirect (punt-via-member) and
   * host-xc.  At init time no LCP pairs exist yet, so the walks
   * inside set_*() are no-ops and just flip the global toggles; as
   * pairs are subsequently created, the LCP pair add callback wires
   * the features per-interface.  The CLI ("sonic-ext punt-via-member
   * disable" / "sonic-ext host-xc disable") can still flip them off
   * at runtime. */
  sonic_ext_set_punt_via_member (1);
  sonic_ext_set_host_xc (1);

  return 0;
}

VLIB_INIT_FUNCTION (sonic_ext_init);
