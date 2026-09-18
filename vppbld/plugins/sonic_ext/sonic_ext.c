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
#include <vnet/l2/l2_in_out_feat_arc.h>
#include <vpp/app/version.h>
#include <vlib/unix/plugin.h>
#include <plugins/linux-cp/lcp_interface.h>
#include <plugins/acl/acl.h>

sonic_ext_main_t sonic_ext_main;

/*
 * Default-disabled, like linux_cp_plugin.so which this plugin depends on:
 * sonic-ext nodes name linux-cp nodes directly in their next-node arcs
 * (e.g. sonic-ext-l2-trap-fixup -> "linux-cp-punt"), so loading sonic-ext
 * without linux-cp makes vlib_node_main_init() fail to resolve them and
 * aborts VPP startup.  Both SONiC startup.conf templates enable this
 * plugin explicitly.
 */
VLIB_PLUGIN_REGISTER () = {
  .version = SONIC_EXT_PLUGIN_BUILD_VER,
  .description = "SONiC VPP extensions: punt-via-member, host-xc",
  .default_disabled = 1,
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

int
sonic_ext_redirect_to_ingress_tap (vlib_buffer_t *b, u32 orig_rx,
				   u32 excluded_tap, u32 *host_tap,
				   u16 *pushed_tpid, u16 *pushed_vlan_id)
{
  vnet_main_t *vnm = vnet_get_main ();
  sonic_ext_buffer_opaque_t *seb = sonic_ext_buffer (b);
  vnet_sw_interface_t *swo;
  const lcp_itf_pair_t *lip;
  index_t lipi;
  u32 phy_sw;
  u32 saved_vlan_tag = seb->orig_vlan_tag;
  i32 adv;

  *host_tap = ~0;
  *pushed_tpid = 0;
  *pushed_vlan_id = 0;

  swo = vnet_get_sw_interface_or_null (vnm, orig_rx);
  if (!swo)
    return 0;

  if (swo->type == VNET_SW_INTERFACE_TYPE_SUB)
    phy_sw = swo->sup_sw_if_index;
  else
    phy_sw = orig_rx;

  lipi = lcp_itf_pair_find_by_phy (phy_sw);
  if (lipi == INDEX_INVALID)
    return 0;

  lip = lcp_itf_pair_get (lipi);
  *host_tap = lip->lip_host_sw_if_index;
  if (*host_tap == excluded_tap)
    return 0;

  seb->magic = 0;

  /* Restore the original wire L2 position.  ethernet-input may have
   * advanced past the Ethernet header before either redirect node runs. */
  adv = (i32) vnet_buffer (b)->l2_hdr_offset - (i32) b->current_data;
  vlib_buffer_advance (b, adv);

  /* Re-push the captured outer tag only if the restored frame does not
   * already contain one.  The raw TPID+TCI snapshot preserves the exact
   * ingress tag without relying on sub-interface configuration. */
  if (saved_vlan_tag && b->current_data >= 4)
    {
      u8 *cur = vlib_buffer_get_current (b);
      u16 cur_etype = clib_net_to_host_u16 (*(u16 *) (cur + 12));

      if (cur_etype != ETHERNET_TYPE_VLAN &&
	  cur_etype != ETHERNET_TYPE_DOT1AD &&
	  cur_etype != ETHERNET_TYPE_VLAN_9100)
	{
	  u8 save_macs[12];
	  u8 *new_cur;

	  clib_memcpy_fast (save_macs, cur, 12);
	  vlib_buffer_advance (b, -4);
	  new_cur = vlib_buffer_get_current (b);
	  clib_memcpy_fast (new_cur, save_macs, 12);
	  clib_memcpy_fast (new_cur + 12, &saved_vlan_tag, 4);
	  vnet_buffer (b)->l2_hdr_offset -= 4;
	  *pushed_tpid = clib_net_to_host_u16 (*(u16 *) &saved_vlan_tag);
	  *pushed_vlan_id =
	    clib_net_to_host_u16 (*((u16 *) &saved_vlan_tag + 1)) & 0x0fff;
	}
    }

  vnet_buffer (b)->sw_if_index[VLIB_TX] = *host_tap;
  return 1;
}

/*
 * Enable / disable the "receive-DPO check before ACL" feature on an L2
 * port.  Both the ip4 and ip6 flavours are toggled together on the
 * l2-input-ip4 / l2-input-ip6 feature arcs (via the L2-specific
 * vnet_l2_feature_enable_disable, which also flips the port's
 * L2INPUT_FEAT_INPUT_FEAT_ARC bitmap so the sub-arc is dispatched).
 * VNET_FEATURE_INIT ordering (.runs_before acl-plugin-in-ip*-l2)
 * guarantees the ip2me node runs ahead of any ingress drop ACL.
 */
void
sonic_ext_ip2me_enable_disable (u32 sw_if_index, int enable)
{
  vnet_l2_feature_enable_disable ("l2-input-ip4", "sonic-ext-ip2me-ip4",
				  sw_if_index, enable, 0, 0);
  vnet_l2_feature_enable_disable ("l2-input-ip6", "sonic-ext-ip2me-ip6",
				  sw_if_index, enable, 0, 0);
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
  /* sonic-ext-copp-ifout: bind ONLY on a direct/member phy's own host
   * tap, never on an aggregate's (bond/BVI) own host tap. For a bond
   * member, ARP/LACP/LLDP/UDLD/TTL_ERROR is first punted to the
   * *aggregate's* tap (see sonic-ext-aggr-tap-redirect's header
   * comment), which redirects it onward to the member's own tap and
   * re-enters interface-output there. If this policer were also
   * bound on the aggregate tap, that single physical packet would be
   * policed twice (once per interface-output pass) against the same
   * shared CIR budget -- confirmed live: policer hit-count ran ~3x
   * the actual packets sent, starving real delivery. Binding only on
   * non-aggregate taps polices each packet exactly once, on its
   * final (member) tap. */
  if (!sonic_ext_phy_is_aggregate (lip->lip_phy_sw_if_index))
    sonic_ext_copp_ifout_enable_disable (lip->lip_host_sw_if_index, 1);
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
  if (!sonic_ext_phy_is_aggregate (lip->lip_phy_sw_if_index))
    sonic_ext_copp_ifout_enable_disable (lip->lip_host_sw_if_index, 0);
}


/*
 * lcp_itf_pair_walk callback: enable sonic-ext-copp-ifout on this
 * pair's host tap. Used at plugin-init time to catch pairs created
 * before the plugin's own VLIB_INIT_FUNCTION ran (shouldn't normally
 * happen given plugin init ordering, but mirrors the same
 * belt-and-suspenders pattern host-xc/aggr-tap-redirect already use).
 */
static walk_rc_t
sonic_ext_copp_ifout_walk_enable_cb (index_t lipi, void *ctx)
{
  const lcp_itf_pair_t *lip = lcp_itf_pair_get (lipi);
  if (lip && !sonic_ext_phy_is_aggregate (lip->lip_phy_sw_if_index))
    sonic_ext_copp_ifout_enable_disable (lip->lip_host_sw_if_index, 1);
  return WALK_CONTINUE;
}

/*
 * Deferred egress mirror (Everflow MIRROR_EGRESS).
 *
 * The ACL dataplane node stamps the mirror destination into the per-buffer
 * sonic_ext cookie and sets MIRROR_PENDING; the sonic-ext-egress-mirror
 * feature on interface-output does the late post-route/post-encap clone.
 * A mirrored data packet can egress any port, so the feature is enabled on
 * *every* interface -- but only while at least one MIRROR_EGRESS action is
 * installed, tracked by active_egress_mirror_actions so the arc cost is
 * paid only when the feature is in use (HLD 12.4).
 */
static int
sonic_ext_egress_mirror_arc_set (int enable)
{
  vnet_main_t *vnm = vnet_get_main ();
  vnet_interface_main_t *im = &vnm->interface_main;
  vnet_sw_interface_t *sw_if;
  u32 *changed = 0;
  int rv = 0;

  enable = !!enable;
  if (sonic_ext_main.egress_mirror_arc_enabled == enable)
    return 0;

  pool_foreach (sw_if, im->sw_interfaces)
    {
      rv = vnet_feature_enable_disable ("interface-output",
					"sonic-ext-egress-mirror",
					sw_if->sw_if_index, enable, 0, 0);
      if (rv)
	goto rollback;
      vec_add1 (changed, sw_if->sw_if_index);
    }

  sonic_ext_main.egress_mirror_arc_enabled = enable;
  vec_free (changed);
  return 0;

rollback:
  while (vec_len (changed) > 0)
    {
      u32 sw_if_index = vec_pop (changed);
      vnet_feature_enable_disable ("interface-output",
				   "sonic-ext-egress-mirror", sw_if_index,
				   !enable, 0, 0);
    }
  vec_free (changed);
  return rv;
}

int
sonic_ext_egress_mirror_enable_disable (u8 enable)
{
  sonic_ext_main_t *sem = &sonic_ext_main;

  /* Commit the refcount only after the arc transition succeeds. If the
   * feature enable/disable fails, leaving the count unchanged lets a later
   * retry re-attempt the transition instead of returning a false success. */
  if (enable)
    {
      if (sem->active_egress_mirror_actions == 0)
	{
	  int rv = sonic_ext_egress_mirror_arc_set (1);
	  if (rv)
	    return rv;
	}
      sem->active_egress_mirror_actions++;
      return 0;
    }

  if (sem->active_egress_mirror_actions == 0)
    return 0; /* balanced disable underflow guard */
  if (sem->active_egress_mirror_actions == 1)
    {
      int rv = sonic_ext_egress_mirror_arc_set (0);
      if (rv)
	return rv;
    }
  sem->active_egress_mirror_actions--;
  return 0;
}

/* New interfaces created while at least one MIRROR_EGRESS action is
 * installed must have the feature enabled too, since the mirrored data
 * packet could egress the new port. */
static clib_error_t *
sonic_ext_egress_mirror_sw_interface_add_del (vnet_main_t *vnm,
					      u32 sw_if_index, u32 is_add)
{
  int rv;
  (void) vnm;

  if (!is_add || !sonic_ext_main.egress_mirror_arc_enabled)
    return 0;

  rv = vnet_feature_enable_disable ("interface-output",
				    "sonic-ext-egress-mirror", sw_if_index, 1,
				    0, 0);
  if (rv)
    return clib_error_return (
      0, "sonic_ext: enable egress mirror on sw_if_index %u failed: %d",
      sw_if_index, rv);
  return 0;
}

VNET_SW_INTERFACE_ADD_DEL_FUNCTION (
  sonic_ext_egress_mirror_sw_interface_add_del);

/*
 * Everflow mirror encap fixup.
 *
 * A stock TEB GRE mirror tunnel does not carry the mirror ethertype and
 * lets the underlay decrement the outer TTL.  This records the desired
 * {gre_protocol, ttl} for the tunnel's sw_if_index and enables the
 * sonic-ext-mirror-encap-fixup feature on its ethernet-output arc, which
 * rewrites the encapped copy.  Re-calling with enable=1 while already
 * enabled only refreshes the values (used on a SAI SET), so it never
 * re-toggles the feature arc.
 */
int
sonic_ext_mirror_encap_fixup_enable_disable (u32 sw_if_index, u16 gre_protocol,
					     u8 ttl, int enable)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  int rv;

  if (enable)
    {
      u8 was_enabled;

      vec_validate (sem->mirror_encap_cfg, sw_if_index);
      was_enabled = sem->mirror_encap_cfg[sw_if_index].enabled;
      sem->mirror_encap_cfg[sw_if_index].gre_protocol = gre_protocol;
      sem->mirror_encap_cfg[sw_if_index].ttl = ttl;
      sem->mirror_encap_cfg[sw_if_index].enabled = 1;

      if (was_enabled)
	return 0;

      rv = vnet_feature_enable_disable ("ethernet-output",
					"sonic-ext-mirror-encap-fixup",
					sw_if_index, 1, 0, 0);
      if (rv)
	sem->mirror_encap_cfg[sw_if_index].enabled = 0;
      return rv;
    }

  rv = vnet_feature_enable_disable ("ethernet-output",
				    "sonic-ext-mirror-encap-fixup", sw_if_index,
				    0, 0, 0);
  if (sw_if_index < vec_len (sem->mirror_encap_cfg))
    {
      sem->mirror_encap_cfg[sw_if_index].enabled = 0;
      sem->mirror_encap_cfg[sw_if_index].gre_protocol = 0;
      sem->mirror_encap_cfg[sw_if_index].ttl = 0;
    }
  return rv;
}

/*
 * Claim the ACL plugin's deferred (egress-arc) mirror clone for Everflow
 * MIRROR_EGRESS. Resolved at runtime so sonic_ext neither links against nor
 * requires the ACL plugin: if it is not loaded, the ACL side simply clones
 * immediately instead.
 */
static void
sonic_ext_register_acl_deferred_mirror (void)
{
  void (*acl_register) (acl_deferred_mirror_stamp_fn);

  acl_register = vlib_get_plugin_symbol (
    "acl_plugin.so", "acl_register_deferred_mirror_stamp");

  if (acl_register == 0)
    {
      clib_warning ("sonic_ext: acl plugin has no deferred mirror hook; "
		    "Everflow egress mirroring will clone on the ACL arc");
      return;
    }

  acl_register (sonic_ext_acl_deferred_mirror_stamp);
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

  sonic_ext_register_acl_deferred_mirror ();

  /* sonic-ext-copp-ifout has no on/off toggle -- it always binds, and
   * relies on its own (initially empty) bind table to no-op until
   * something is actually bound via sonic_ext_copp_ifout_bind(). Walk
   * any pre-existing pairs for the same belt-and-suspenders reason as
   * above; going forward the LCP pair add/del callback keeps it in
   * sync, including across `config reload`. */
  lcp_itf_pair_walk (sonic_ext_copp_ifout_walk_enable_cb, NULL);

  /* sonic-ext-copp-ip2me is a single global feature on the ip4-punt
   * arc (dispatched with sw_if_index 0, same as glean-redirect on
   * ip4-drop/ip6-drop above) -- enable it once, unconditionally, at
   * init. No per-interface binding is needed or ever done: the node
   * self-scopes via its own (initially empty) tracked-address table
   * and no-ops until addresses are added via
   * sonic_ext_copp_ip2me_addr_add_del() / bound via
   * sonic_ext_copp_ip2me_bind(). Without this call the node is only
   * registered in the ip4-punt arc's graph (which is why `show
   * sonic-ext copp-ip2me` reports it as bound with addresses) but
   * never actually visited by any packet, silently letting all IP2ME/
   * SNMP/SSH/BGP traffic through completely unpoliced. */
  vnet_feature_enable_disable ("ip4-punt", "sonic-ext-copp-ip2me", 0, 1, 0,
			       0);

  return 0;
}

VLIB_INIT_FUNCTION (sonic_ext_init);


