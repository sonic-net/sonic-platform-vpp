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
 * Per-interface feature enable helpers.  All three live in this file
 * (rather than in the per-node files) so that the LCP pair add/del
 * callback, the sw_if_index add/del callback and the CLI all share the
 * same code path.
 */
void
sonic_ext_capture_enable_disable (u32 sw_if_index, int enable)
{
  vnet_feature_enable_disable ("device-input", "sonic-ext-capture",
			       sw_if_index, enable, 0, 0);
}

void
sonic_ext_host_xc_enable_disable (u32 sw_if_index, int enable)
{
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

void
sonic_ext_bcast_redirect_enable_disable (u32 sw_if_index, int enable)
{
  vnet_feature_enable_disable ("ip4-unicast",
			       "sonic-ext-bcast-redirect", sw_if_index,
			       enable, 0, 0);
}

/*
 * Is `phy_sw_if_index` a BVI (bridge-virtual interface)?  Used both
 * by the aggregate-detection helper and by the bcast-redirect
 * enable path.  Distinct from is_aggregate so that future bond
 * support can be added to is_aggregate without dragging
 * bvi-specific features along for the ride.
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
 * Is `phy_sw_if_index` an "aggregate" parent whose linux-cp host tap
 * should receive aggr-tap-redirect?  Today that means BVI (loop /
 * bridge-virtual interface); tomorrow it will also mean bond /
 * port-channel masters.  Sub-interfaces are never aggregates; their
 * parent might be, but the parent has its own LCP pair which will
 * have produced its own callback.
 */
int
sonic_ext_phy_is_aggregate (u32 phy_sw_if_index)
{
  if (sonic_ext_phy_is_bvi (phy_sw_if_index))
    return 1;

  /* TODO: bond / port-channel detection -- compare hw->dev_class
   * name to "bond" so we don't have to link against the bonding
   * plugin. */

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
 * On new interface creation, we don't enable any sonic-ext features
 * directly: capture, host-xc and aggr-tap-redirect all need LCP pair
 * context (we want capture only on real wire phys, and the other two
 * only on host taps).  The LCP pair add callback is the single point
 * that wires every per-interface feature.  Keeping this hook around
 * (as a no-op stub) leaves space for future bookkeeping that doesn't
 * need LCP context.
 */
static clib_error_t *
sonic_ext_sw_interface_add_del (vnet_main_t *vnm, u32 sw_if_index, u32 is_add)
{
  return 0;
}

VNET_SW_INTERFACE_ADD_DEL_FUNCTION (sonic_ext_sw_interface_add_del);

/*
 * LCP pair add/del: when a new linux-cp pair appears, enable the per-
 * interface sonic-ext features that apply.
 *
 *   - sonic-ext-capture on the phy   -- only if phy is a real wire
 *     port (not BVI / bond master) and punt-via-member is enabled.
 *   - sonic-ext-host-xc on the host  -- only if host-xc is enabled.
 *   - sonic-ext-aggr-tap-redirect on the host  -- only if phy is an
 *     aggregate (BVI today, bond tomorrow) and punt-via-member is on.
 *   - sonic-ext-bcast-redirect on the phy  -- only if phy is a
 *     BVI and punt-via-member is on; catches limited-broadcast
 *     IPv4 frames that flooded into the BVI from a bridge member
 *     and feeds them to linux-cp-punt -> bvi-host-tap
 *     interface-output -> aggr-tap-redirect -> member-host-tap (so
 *     the kernel observes the broadcast on the original member's
 *     netdev).
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
  if (sem->capture_enabled
      && sonic_ext_phy_is_bvi (lip->lip_phy_sw_if_index))
    sonic_ext_bcast_redirect_enable_disable (lip->lip_phy_sw_if_index, 1);
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
  if (sem->capture_enabled
      && sonic_ext_phy_is_bvi (lip->lip_phy_sw_if_index))
    sonic_ext_bcast_redirect_enable_disable (lip->lip_phy_sw_if_index, 0);
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
