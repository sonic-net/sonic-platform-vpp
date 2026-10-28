#!/usr/bin/env python3
"""SONiC-ext L2 punt tests.

Covers the two nodes that move a punted L2 frame onto the *member*
interface's Linux host tap:

  sonic-ext-capture
      device-input feature on every non-aggregate phy with an LCP pair.
      Stamps the buffer with a magic cookie, the ingress sw_if_index and
      the outermost VLAN tag as seen on the wire.

  sonic-ext-aggr-tap-redirect
      interface-output feature on the host tap of an aggregate phy (a BVI
      here).  Recovers the cookie and rewrites VLIB_TX from the BVI's tap
      to the ingress member's tap, re-pushing the captured VLAN tag.

Topology.  The host taps are real Linux taps created by linux-cp in a
dedicated netns, so a punted frame is observed where SONiC observes it:

    pg0           physical bridge member, untagged     -> tap hpg0
    pg1           trunk phy, not itself a BD member    -> tap hpg1
    pg1.100       vlan sub-interface of pg1, tagged BD member, NO LCP pair
    bvi0          BVI of the bridge domain             -> tap hbvi0

pg1.100 deliberately has no LCP pair: a bridged sub-interface never has one
in SONiC, which is the whole reason the trap-fixup and the aggregate-tap
redirect exist.  A tagged frame arriving on pg1.100 must come out of hpg1,
the tap of its *parent* phy.

The classify tables installed here mirror l2_punt_classify_init() in
sonic-sairedis (vslib/vpp/SwitchVppFdb.cpp).  Only the tables SONiC binds by
default are installed: the untagged IP4 slot (DHCPv4), the untagged OTHER
slot (LLDP) and the tagged OTHER slot (DHCPv4 over .1Q).  The broadcast-ARP
tables are deliberately *not* installed -- SONiC only binds those for VLANs
with broadcast flood control disabled -- so ARP floods to the BVI and is
punted there.

That BVI punt relies on "lcp ethertype enable 0x0806", which SONiC issues in
init_vpp_client().  linux-cp-arp-phy, the arp-feature-arc node, only copies
ARP *replies* to the host, so without the ethertype registration a flooded
ARP request dies in error-drop.  With it, ethernet-input's L3 demux on the
BVI hands the frame to linux-cp-punt-xc, which targets the BVI's own tap --
exactly where sonic-ext-aggr-tap-redirect is hooked.

LLDP is covered on both punt paths.  On the access phy it is a bridged frame
and is punted by the classify table.  On the trunk phy it never enters the
bridge -- the phy is not a BD member -- so it takes the L3 path through
ethernet-input's ethertype demux, which requires "lcp ethertype enable".
"""

import time
import unittest

from scapy.contrib.lldp import (
    LLDPDUChassisID,
    LLDPDUEndOfLLDPDU,
    LLDPDUPortID,
    LLDPDUTimeToLive,
)
from scapy.layers.dhcp import BOOTP, DHCP
from scapy.layers.inet import IP, UDP
from scapy.layers.inet6 import IPv6, IPv6ExtHdrHopByHop
from scapy.layers.l2 import ARP, Dot1Q, Ether
from scapy.packet import Raw

from asfframework import VppTestRunner, get_testcase_dirname
from config import config
from framework import VppTestCase
from host_interface import HostInterface
from vpp_bvi_interface import VppBviInterface
from vpp_l2 import L2_PORT_TYPE, VppBridgeDomain, VppBridgeDomainPort
from vpp_papi import VppEnum
from vpp_ip_route import VppIpRoute, VppRoutePath
from vpp_policer import PolicerAction, VppPolicer
from vpp_qemu_utils import create_namespace, delete_all_namespaces, set_interface_up
from vpp_sub_interface import L2_VTR_OP, VppDot1QSubint

NO_INDEX = 0xFFFFFFFF

BCAST_MAC = "ff:ff:ff:ff:ff:ff"
LLDP_MCAST_MAC = "01:80:c2:00:00:0e"
LLDP_ETHERTYPE = 0x88CC
ARP_ETHERTYPE = 0x0806
DHCP_CLIENT_PORT = 68
DHCP_SERVER_PORT = 67
DHCP6_CLIENT_PORT = 546
DHCP6_SERVER_PORT = 547

REDIRECTED = (
    "/err/sonic-ext-aggr-tap-redirect/aggregate tap punt redirected to member tap"
)
NOT_REDIRECTED = (
    "/err/sonic-ext-aggr-tap-redirect/punt-via-member disabled -- left on aggregate tap"
)

QOS = VppEnum.vl_api_sse2_qos_action_type_t
RATE = VppEnum.vl_api_sse2_qos_rate_type_t
DROP = PolicerAction(QOS.SSE2_QOS_ACTION_API_DROP, 0)


@unittest.skipIf("linux-cp" in config.excluded_plugins, "Exclude linux-cp plugin tests")
@unittest.skipIf(
    "sonic_ext" in config.excluded_plugins, "Exclude sonic-ext plugin tests"
)
@unittest.skipIf(config.skip_netns_tests, "netns not available or disabled from cli")
class SonicExtL2PuntBase(VppTestCase):
    """SONiC-ext L2 punt test base"""

    vpp_worker_count = 0

    # Both plugins are .default_disabled, so ask for them explicitly.
    extra_vpp_plugin_config = [
        "plugin",
        "linux_cp_plugin.so",
        "{",
        "enable",
        "}",
        "plugin",
        "sonic_ext_plugin.so",
        "{",
        "enable",
        "}",
        "plugin",
        "policer_plugin.so",
        "{",
        "enable",
        "}",
    ]

    BD_ID = 1
    VLAN_ID = 100
    # Turning punt-via-member on at runtime wires it, so a class testing it
    # off in startup.conf must not.
    PUNT_VIA_MEMBER = True

    #
    # netns plumbing -- the namespace has to exist before VPP starts
    #

    @classmethod
    def setUpNetNS(cls):
        cls.ns_history_name = (
            f"{config.tmp_dir}/{get_testcase_dirname(cls.__name__)}/history_ns.txt"
        )
        delete_all_namespaces(cls.ns_history_name)
        cls.ns_name = create_namespace(cls.ns_history_name)
        cls.vpp_cmdline.extend(["linux-cp", "{", "default", "netns", cls.ns_name, "}"])

    @classmethod
    def attach_vpp(cls):
        cls.setUpNetNS()
        super().attach_vpp()

    @classmethod
    def run_vpp(cls):
        cls.setUpNetNS()
        super().run_vpp()

    @classmethod
    def tearDownClass(cls):
        delete_all_namespaces(cls.ns_history_name)
        super().tearDownClass()

    def setUp(self):
        super(SonicExtL2PuntBase, self).setUp()

        self.create_pg_interfaces(range(2))
        for i in self.pg_interfaces:
            i.admin_up()

        self.access_phy = self.pg0
        self.trunk_phy = self.pg1

        # Tagged bridge member on the *other* phy.  pop-1 so the bridge sees
        # an untagged frame, exactly as SONiC configures a trunk member.
        self.vlan_if = VppDot1QSubint(self, self.trunk_phy, self.VLAN_ID)
        self.vlan_if.admin_up()
        self.vlan_if.set_vtr(L2_VTR_OP.L2_POP_1)

        # VppBviInterface creates itself in __init__.
        self.bvi = VppBviInterface(self)
        self.bvi.admin_up()

        # arp_term=0: ARP must flood to the BVI rather than be answered by the
        # bridge domain, otherwise the redirect path is never reached.
        self.bd = VppBridgeDomain(self, self.BD_ID, arp_term=0).add_vpp_config()
        self.bd_ports = [
            VppBridgeDomainPort(self, self.bd, self.access_phy).add_vpp_config(),
            VppBridgeDomainPort(self, self.bd, self.vlan_if).add_vpp_config(),
            VppBridgeDomainPort(
                self, self.bd, self.bvi, port_type=L2_PORT_TYPE.BVI
            ).add_vpp_config(),
        ]

        # LCP pairs must come *after* the BVI has been bound into the bridge
        # domain: sonic_ext_lcp_pair_add_cb() uses l2_input_is_bvi() to decide
        # whether a pair gets sonic-ext-capture (member) or
        # sonic-ext-aggr-tap-redirect (aggregate).
        self.lcp_pairs = []
        self.access_tap = self._lcp_create(self.access_phy, "hpg0")
        self.trunk_tap = self._lcp_create(self.trunk_phy, "hpg1")
        self.aggr_tap = self._lcp_create(self.bvi, "hbvi0")
        self.logger.info("lcp:\n%s", self.vapi.cli("show lcp"))

        # LLDP on the trunk phy never enters the bridge -- the phy itself is
        # not a BD member -- so it is not reachable by an l2-input-classify
        # table.  It takes the L3 path instead: registering the ethertype
        # points ethernet-input's L3 demux at linux-cp-punt-xc, which sends
        # the frame to the RX phy's own LCP host.  A bridged interface goes
        # to l2-input before that demux, so this does not disturb the
        # classify-driven LLDP punt on the access phy.
        #
        # ARP is registered for the same reason, and SONiC does the same in
        # init_vpp_client() (SaiVppXlate.c).  Without it a flooded ARP
        # *request* reaching the BVI is delivered to arp-input, where
        # linux-cp-arp-phy passes it along the arp feature arc and it ends up
        # in error-drop: that node only copies ARP *replies* to the host.
        # Registering 0x0806 makes the BVI copy take linux-cp-punt-xc to the
        # BVI's own tap, which is what sonic-ext-aggr-tap-redirect hooks.
        for ethertype in (LLDP_ETHERTYPE, ARP_ETHERTYPE):
            self.vapi.cli("lcp ethertype enable %d" % ethertype)
        self.logger.info("lcp ethertype:\n%s", self.vapi.cli("show lcp ethertype"))

        if self.PUNT_VIA_MEMBER:
            self.vapi.cli("sonic-ext punt-via-member on")

        self._install_punt_classifiers()
        self.logger.info("sonic-ext:\n%s", self.vapi.cli("show sonic-ext"))

    def tearDown(self):
        for tap in (self.access_tap, self.trunk_tap, self.aggr_tap):
            tap.disable_capture()

        self._remove_punt_classifiers()

        for itf in reversed(self.lcp_pairs):
            self.vapi.cli("lcp delete %s" % itf)

        for p in self.bd_ports:
            p.remove_vpp_config()
        self.bd.remove_vpp_config()
        self.bvi.remove_vpp_config()
        self.vlan_if.admin_down()
        self.vlan_if.remove_vpp_config()

        for i in self.pg_interfaces:
            i.admin_down()
        super(SonicExtL2PuntBase, self).tearDown()

    def _lcp_create(self, itf, host_if_name):
        """Create an LCP pair and return a HostInterface for its Linux tap."""
        self.vapi.cli(
            "lcp create %s host-if %s netns %s" % (itf, host_if_name, self.ns_name)
        )
        self.lcp_pairs.append(itf)
        set_interface_up(self.ns_name, host_if_name)
        return HostInterface(self, self.ns_name, host_if_name)

    #
    # classify plumbing -- mirrors l2_punt_classify_init()
    #

    def _node_next(self, node_name, next_name):
        """Resolve a hit-next graph slot, registering next_name as a next node
        of node_name so the session's hit_next_index is a real edge."""
        r = self.vapi.add_node_next(node_name=node_name, next_name=next_name)
        self.assertNotEqual(r.next_index, NO_INDEX)
        return r.next_index

    def _add_table(self, mask, match_n_vectors, next_table_index=NO_INDEX):
        r = self.vapi.classify_add_del_table(
            is_add=1,
            nbuckets=8,
            memory_size=4 * 1024,
            skip_n_vectors=0,
            match_n_vectors=match_n_vectors,
            next_table_index=next_table_index,
            miss_next_index=NO_INDEX,
            mask=mask,
            mask_len=len(mask),
        )
        self.tables.append(r.new_table_index)
        return r.new_table_index

    def _add_session(self, table_index, hit_next_index, match):
        self.vapi.classify_add_del_session(
            is_add=1,
            table_index=table_index,
            hit_next_index=hit_next_index,
            match=match,
            match_len=len(match),
        )

    @staticmethod
    def _vec(size, *byte_maps):
        """Build a match/mask vector of *size* bytes from {offset: value} maps."""
        v = bytearray(size)
        for byte_map in byte_maps:
            for offset, value in byte_map.items():
                v[offset] = value
        return bytes(v)

    def _install_punt_classifiers(self):
        self.tables = []

        # Only LLDP remains classify-driven in this focused harness. DHCP is
        # parsed on device-input by sonic-ext-capture, where the live semantic
        # binding decides absent/TRAP/COPY without fixed transport offsets.
        punt_next = self._node_next("l2-input-classify", "linux-cp-punt")
        mask = self._vec(16, {12: 0xFF, 13: 0xFF})
        self.untag_other_table = self._add_table(mask, 1)
        match = self._vec(16, {12: 0x88, 13: 0xCC})
        self._add_session(self.untag_other_table, punt_next, match)

        self.vapi.classify_set_interface_l2_tables(
            sw_if_index=self.access_phy.sw_if_index,
            ip4_table_index=NO_INDEX,
            ip6_table_index=NO_INDEX,
            other_table_index=self.untag_other_table,
            is_input=1,
        )
        self.vapi.classify_set_interface_l2_tables(
            sw_if_index=self.vlan_if.sw_if_index,
            ip4_table_index=NO_INDEX,
            ip6_table_index=NO_INDEX,
            other_table_index=NO_INDEX,
            is_input=1,
        )

    def _remove_punt_classifiers(self):
        for sw_if_index in (self.access_phy.sw_if_index, self.vlan_if.sw_if_index):
            self.vapi.classify_set_interface_l2_tables(
                sw_if_index=sw_if_index,
                ip4_table_index=NO_INDEX,
                ip6_table_index=NO_INDEX,
                other_table_index=NO_INDEX,
                is_input=1,
            )
        # Classify tables are global; drop them so repeated test methods do not
        # accumulate them in the shared VPP instance.  The API handler checks
        # mask_len == match_n_vectors * sizeof(u32x4) before it even looks at
        # is_add, and match_n_vectors defaults to 1, so a bare delete would be
        # rejected with INVALID_VALUE.  Send an explicit empty mask.
        for table_index in self.tables:
            self.vapi.classify_add_del_table(
                is_add=0,
                table_index=table_index,
                match_n_vectors=0,
                mask=b"",
                mask_len=0,
            )
        self.tables = []

    #
    # packet builders
    #

    def _arp_request(self, phy, tagged=False):
        eth = Ether(dst=BCAST_MAC, src=phy.remote_mac)
        payload = ARP(
            op="who-has",
            hwsrc=phy.remote_mac,
            psrc="10.0.0.2",
            pdst="10.0.0.1",
        )
        if tagged:
            return eth / Dot1Q(vlan=self.VLAN_ID) / payload
        return eth / payload

    def _dhcp_request(self, phy, tagged=False):
        eth = Ether(dst=BCAST_MAC, src=phy.remote_mac)
        payload = (
            IP(src="0.0.0.0", dst="255.255.255.255")
            / UDP(sport=DHCP_CLIENT_PORT, dport=DHCP_SERVER_PORT)
            / BOOTP(op=1, chaddr=bytes.fromhex(phy.remote_mac.replace(":", "")))
            / DHCP(options=[("message-type", "request"), "end"])
        )
        if tagged:
            return eth / Dot1Q(vlan=self.VLAN_ID) / payload
        return eth / payload

    def _dhcp6_request(self, phy, tagged=False, extension=False):
        eth = Ether(dst="33:33:00:01:00:02", src=phy.remote_mac)
        l4 = UDP(sport=DHCP6_CLIENT_PORT, dport=DHCP6_SERVER_PORT)
        if extension:
            l4 = IPv6ExtHdrHopByHop(options=[]) / l4
        payload = IPv6(src="::1", dst="ff02::1:2", hlim=64) / l4 / Raw(b"dhcp6-client")
        if tagged:
            return eth / Dot1Q(vlan=self.VLAN_ID) / payload
        return eth / payload

    def _lldp(self, phy):
        return (
            Ether(dst=LLDP_MCAST_MAC, src=phy.remote_mac, type=LLDP_ETHERTYPE)
            / LLDPDUChassisID(subtype=4, id="01:02:03:04:05:06")
            / LLDPDUPortID(subtype=3, id="07:08:09:0a:0b:0c")
            / LLDPDUTimeToLive(ttl=120)
            / LLDPDUEndOfLLDPDU()
        )

    #
    # capture filters -- the taps also carry kernel-generated traffic
    #

    @staticmethod
    def _not_arp(p):
        return not p.haslayer(ARP)

    @staticmethod
    def _not_dhcp(p):
        return not (p.haslayer(UDP) and p[UDP].dport == DHCP_SERVER_PORT)

    @staticmethod
    def _not_dhcp6(p):
        return not (p.haslayer(UDP) and p[UDP].dport == DHCP6_SERVER_PORT)

    @staticmethod
    def _not_lldp(p):
        if p.haslayer(Dot1Q):
            return p[Dot1Q].type != LLDP_ETHERTYPE
        return p[Ether].type != LLDP_ETHERTYPE

    @staticmethod
    def _not_from(src_mac):
        """Drop everything not sourced by *src_mac*.

        The Linux side of the LCP taps emits its own traffic -- IPv6 DAD
        neighbour solicitations for the link-local address, MLD reports, ...
        -- which VPP receives on the tap and floods through the bridge
        domain out of the pg members.  is_ipv6_misc(), the pg default
        filter, does not cover neighbour discovery, so filter on the source
        MAC of the frame under test instead.
        """

        def filter_out_fn(p):
            return p[Ether].src != src_mac

        return filter_out_fn

    #
    # helpers
    #

    def _err(self, name):
        return self.statistics.get_err_counter(name)

    def _bind_dhcp(self, is_ipv6=False, is_copy=False):
        self.vapi.sonic_ext_copp_dhcp_bind(
            policer_name="",
            is_ipv6=is_ipv6,
            is_copy=is_copy,
            is_bind=True,
        )
        self.addCleanup(
            self.vapi.sonic_ext_copp_dhcp_bind,
            policer_name="",
            is_ipv6=is_ipv6,
            is_copy=is_copy,
            is_bind=False,
        )

    def _punt(self, phy, pkt, tap, filter_out_fn, flood_to=()):
        """Inject one frame on *phy* and return the copy seen on *tap*.

        *flood_to* is an iterable of (pg_interface, expect_tagged) for the
        other bridge-domain members that must also receive the frame.  A
        punted broadcast is still a broadcast: punting the BVI copy must not
        consume the flood copies.
        """
        tap.enable_capture()
        self.pg_enable_capture(self.pg_interfaces)
        phy.add_stream([pkt])
        self.pg_start(trace=True)
        try:
            rxs = tap.get_capture(
                expected_count=1, timeout=5, filter_out_fn=filter_out_fn
            )
            for member, expect_tagged in flood_to:
                flooded = member.get_capture(
                    1, filter_out_fn=self._not_from(pkt[Ether].src)
                )
                self._assert_flooded(pkt, flooded[0], member, expect_tagged)
        finally:
            # Nothing else dumps the trace for us: the send_and_expect helpers
            # do it, but we drive the PG directly because the frame leaves via
            # a Linux tap rather than a pg interface.
            self.logger.debug(self.vapi.cli("show trace max 50"))
        self.assertEqual(len(rxs), 1)
        return rxs[0]

    def _assert_flooded(self, sent, rx, member, expect_tagged):
        """Check the bridge flooded *sent* out of *member* with the right tag."""
        if expect_tagged:
            self.assertTrue(
                rx.haslayer(Dot1Q),
                "flood copy on %s lost its VLAN tag" % member.name,
            )
            self.assertEqual(rx[Dot1Q].vlan, self.VLAN_ID)
        else:
            self.assertFalse(
                rx.haslayer(Dot1Q),
                "flood copy on %s unexpectedly carries a VLAN tag" % member.name,
            )
        self.assertEqual(rx[Ether].dst, sent[Ether].dst)
        self.assertEqual(rx[Ether].src, sent[Ether].src)
        if sent.haslayer(ARP):
            self.assertEqual(rx[ARP].psrc, sent[ARP].psrc)
            self.assertEqual(rx[ARP].pdst, sent[ARP].pdst)
        elif sent.haslayer(UDP):
            self.assertEqual(rx[UDP].sport, sent[UDP].sport)
            self.assertEqual(rx[UDP].dport, sent[UDP].dport)

    def _assert_untagged(self, rx):
        self.assertFalse(
            rx.haslayer(Dot1Q), "punted frame unexpectedly carries a VLAN tag"
        )

    def _assert_tagged(self, rx):
        self.assertTrue(
            rx.haslayer(Dot1Q),
            "punted frame lost its VLAN tag on the way to the member tap",
        )
        self.assertEqual(rx[Dot1Q].vlan, self.VLAN_ID)


class TestSonicExtL2Punt(SonicExtL2PuntBase):
    """SONiC-ext L2 punt: capture and aggregate-tap redirect"""

    #
    # scenario 1 -- untagged, received on the physical bridge member
    #

    def test_untagged_arp_to_member_tap(self):
        """Untagged ARP floods to the BVI and is redirected to the member tap"""
        before = self._err(REDIRECTED)

        rx = self._punt(
            self.access_phy,
            self._arp_request(self.access_phy),
            self.access_tap,
            self._not_arp,
            # The tagged member gets the flood copy with the VTR-pushed tag.
            flood_to=[(self.trunk_phy, True)],
        )

        self._assert_untagged(rx)
        self.assertEqual(rx[ARP].pdst, "10.0.0.1")
        # Proves the frame left the aggregate tap for the member tap.
        self.assertEqual(self._err(REDIRECTED), before + 1)

    def test_untagged_dhcp_to_member_tap(self):
        """Untagged DHCP request is classified and punted to the member tap"""
        self._bind_dhcp()
        rx = self._punt(
            self.access_phy,
            self._dhcp_request(self.access_phy),
            self.access_tap,
            self._not_dhcp,
        )

        self._assert_untagged(rx)
        self.assertEqual(rx[UDP].dport, DHCP_SERVER_PORT)

    def test_lldp_to_member_tap(self):
        """LLDP is classified and punted to the member tap"""
        rx = self._punt(
            self.access_phy,
            self._lldp(self.access_phy),
            self.access_tap,
            self._not_lldp,
        )

        self._assert_untagged(rx)
        self.assertEqual(rx[Ether].dst, LLDP_MCAST_MAC)

    #
    # scenario 2 -- tagged, received on the vlan sub-interface of another phy
    #

    def test_tagged_arp_to_parent_member_tap(self):
        """Tagged ARP is redirected to the parent phy's tap with the tag intact"""
        before = self._err(REDIRECTED)

        rx = self._punt(
            self.trunk_phy,
            self._arp_request(self.trunk_phy, tagged=True),
            self.trunk_tap,
            self._not_arp,
            # The access member gets the flood copy with the tag popped.
            flood_to=[(self.access_phy, False)],
        )

        self._assert_tagged(rx)
        self.assertEqual(rx[ARP].pdst, "10.0.0.1")
        self.assertEqual(self._err(REDIRECTED), before + 1)

    def test_tagged_dhcp_to_parent_member_tap(self):
        """Tagged DHCP is trap-fixed up and punted to the parent phy's tap"""
        self._bind_dhcp()
        rx = self._punt(
            self.trunk_phy,
            self._dhcp_request(self.trunk_phy, tagged=True),
            self.trunk_tap,
            self._not_dhcp,
        )

        self._assert_tagged(rx)
        self.assertEqual(rx[UDP].dport, DHCP_SERVER_PORT)

    def test_unbound_dhcp6_preserves_vlan_flooding(self):
        """Without a DHCPv6 trap, a client multicast stays in the bridge."""
        pkt = self._dhcp6_request(self.access_phy, extension=True)
        self.access_tap.enable_capture()
        self.pg_enable_capture(self.pg_interfaces)
        self.access_phy.add_stream([pkt])
        self.pg_start(trace=True)

        self.access_tap.get_capture(
            expected_count=0, timeout=1, filter_out_fn=self._not_dhcp6
        )
        flooded = self.trunk_phy.get_capture(
            1, filter_out_fn=self._not_from(pkt[Ether].src)
        )[0]
        self._assert_flooded(pkt, flooded, self.trunk_phy, True)

    def test_unpoliced_dhcp6_copy_preserves_vlan_flooding(self):
        """COPY sends one CPU copy and preserves the original flood path."""
        self._bind_dhcp(is_ipv6=True, is_copy=True)
        pkt = self._dhcp6_request(self.access_phy, extension=True)
        rx = self._punt(
            self.access_phy,
            pkt,
            self.access_tap,
            self._not_dhcp6,
            flood_to=[(self.trunk_phy, True)],
        )
        self._assert_untagged(rx)
        self.assertEqual(rx[UDP].dport, DHCP6_SERVER_PORT)

    def test_tagged_dhcp6_extension_trap_suppresses_flood(self):
        """TRAP parses an extension chain, punts tagged, and consumes L2."""
        self._bind_dhcp(is_ipv6=True)
        pkt = self._dhcp6_request(self.trunk_phy, tagged=True, extension=True)
        self.trunk_tap.enable_capture()
        self.pg_enable_capture(self.pg_interfaces)
        self.trunk_phy.add_stream([pkt])
        self.pg_start(trace=True)

        rx = self.trunk_tap.get_capture(
            expected_count=1, timeout=5, filter_out_fn=self._not_dhcp6
        )[0]
        self.access_phy.get_capture(
            0, timeout=1, filter_out_fn=self._not_from(pkt[Ether].src)
        )
        self._assert_tagged(rx)
        self.assertEqual(rx[UDP].dport, DHCP6_SERVER_PORT)

    def test_lldp_on_trunk_to_own_tap(self):
        """LLDP on the trunk phy is punted via the L3 ethertype path to its tap"""
        # Untagged, and the trunk phy is not a BD member, so this bypasses the
        # bridge entirely: ethernet-input -> linux-cp-punt-xc -> hpg1.
        rx = self._punt(
            self.trunk_phy,
            self._lldp(self.trunk_phy),
            self.trunk_tap,
            self._not_lldp,
        )

        self._assert_untagged(rx)
        self.assertEqual(rx[Ether].dst, LLDP_MCAST_MAC)
        self.assertEqual(rx[Ether].src, self.trunk_phy.remote_mac)

    #
    # negative control
    #

    def test_punt_via_member_disabled(self):
        """With punt-via-member off the punt stays on the aggregate tap"""
        self.vapi.cli("sonic-ext punt-via-member off")
        try:
            before = self._err(NOT_REDIRECTED)

            rx = self._punt(
                self.access_phy,
                self._arp_request(self.access_phy),
                self.aggr_tap,
                self._not_arp,
                flood_to=[(self.trunk_phy, True)],
            )

            self.assertEqual(rx[ARP].pdst, "10.0.0.1")
            self.assertEqual(self._err(NOT_REDIRECTED), before + 1)
        finally:
            self.vapi.cli("sonic-ext punt-via-member on")

    def _assert_neighbor_miss_semantic_policing(self, prefix, l3):
        pkt = (
            Ether(src=self.access_phy.remote_mac, dst=self.bvi.local_mac)
            / l3
            / Raw(b"neighbor-miss")
        )
        pol = VppPolicer(
            self,
            "copp-neighbor-miss-%s" % ("ip6" if isinstance(l3, IPv6) else "ip4"),
            1000,
            0,
            100000,
            0,
            rate_type=RATE.SSE2_QOS_RATE_API_KBPS,
            conform_action=DROP,
            exceed_action=DROP,
            violate_action=DROP,
        )

        self.vapi.sw_interface_add_del_address(
            sw_if_index=self.bvi.sw_if_index, prefix=prefix
        )
        try:
            pol.add_vpp_config()
            # Unbound control: the existing neighbor-resolution redirect
            # remains functional and byte-preserving.
            rx = self._punt(
                self.access_phy,
                pkt,
                self.access_tap,
                self._not_from(pkt[Ether].src),
            )
            self.assertEqual(bytes(rx), bytes(pkt))

            # The glean redirect has a per-adjacency ~1 ms throttle. Wait
            # beyond it so this checks CoPP rather than the earlier throttle.
            time.sleep(0.02)
            self.vapi.sonic_ext_copp_neighbor_miss_bind(
                policer_name=pol.name, is_bind=True
            )

            before = pol.get_stats()["conform_packets"]
            self.access_tap.enable_capture()
            self.access_phy.add_stream([pkt])
            self.pg_start(trace=True)
            self.access_tap.get_capture(
                expected_count=0,
                timeout=1,
                filter_out_fn=self._not_from(pkt[Ether].src),
            )
            self.assertEqual(pol.get_stats()["conform_packets"], before + 1)
        finally:
            self.vapi.sonic_ext_copp_neighbor_miss_bind(
                policer_name=pol.name, is_bind=False
            )
            if pol.query_vpp_config():
                pol.remove_vpp_config()
            self.vapi.sw_interface_add_del_address(
                sw_if_index=self.bvi.sw_if_index, prefix=prefix, is_add=0
            )

    def test_ip4_neighbor_miss_semantic_row_polices_only_bound_bvi_glean(self):
        """An IPv4 BVI glean is policed only while NEIGHBOR_MISS is bound."""
        self._assert_neighbor_miss_semantic_policing(
            "10.10.10.1/24", IP(src="10.10.10.2", dst="10.10.10.99")
        )

    def test_ip6_neighbor_miss_semantic_row_polices_only_bound_bvi_glean(self):
        """An IPv6 BVI glean is policed only while NEIGHBOR_MISS is bound."""
        self._assert_neighbor_miss_semantic_policing(
            "2001:db8:10::1/64",
            IPv6(src="2001:db8:10::2", dst="2001:db8:10::99"),
        )

    def test_unpoliced_neighbor_miss_bypasses_legacy_throttle(self):
        """An active row with no policer admits a burst to the CPU."""
        prefix = "10.20.20.1/24"
        pkt = (
            Ether(src=self.access_phy.remote_mac, dst=self.bvi.local_mac)
            / IP(src="10.20.20.2", dst="10.20.20.99")
            / Raw(b"unpoliced-neighbor-miss")
        )
        self.vapi.sw_interface_add_del_address(
            sw_if_index=self.bvi.sw_if_index, prefix=prefix
        )
        try:
            self.vapi.sonic_ext_copp_neighbor_miss_bind(policer_name="", is_bind=True)
            self.access_tap.enable_capture()
            self.access_phy.add_stream([pkt, pkt, pkt])
            self.pg_start(trace=True)
            rxs = self.access_tap.get_capture(
                expected_count=3,
                timeout=5,
                filter_out_fn=self._not_from(pkt[Ether].src),
            )
            self.assertEqual([bytes(rx) for rx in rxs], [bytes(pkt)] * 3)
        finally:
            self.vapi.sonic_ext_copp_neighbor_miss_bind(policer_name="", is_bind=False)
            self.vapi.sw_interface_add_del_address(
                sw_if_index=self.bvi.sw_if_index, prefix=prefix, is_add=0
            )

    def test_neighbor_miss_survives_runtime_punt_via_member_off(self):
        """The semantic trap owns glean redirect independently at runtime."""
        prefix = "10.30.30.1/24"
        pkt = (
            Ether(src=self.access_phy.remote_mac, dst=self.bvi.local_mac)
            / IP(src="10.30.30.2", dst="10.30.30.99")
            / Raw(b"neighbor-miss-punt-via-member-off")
        )
        self.vapi.sw_interface_add_del_address(
            sw_if_index=self.bvi.sw_if_index, prefix=prefix
        )
        try:
            self.vapi.sonic_ext_copp_neighbor_miss_bind(policer_name="", is_bind=True)
            self.vapi.cli("sonic-ext punt-via-member off")
            rx = self._punt(
                self.access_phy,
                pkt,
                self.access_tap,
                self._not_from(pkt[Ether].src),
            )
            self.assertEqual(bytes(rx), bytes(pkt))
        finally:
            self.vapi.sonic_ext_copp_neighbor_miss_bind(policer_name="", is_bind=False)
            self.vapi.cli("sonic-ext punt-via-member on")
            self.vapi.sw_interface_add_del_address(
                sw_if_index=self.bvi.sw_if_index, prefix=prefix, is_add=0
            )

    def test_ip6_incomplete_neighbor_miss_preserves_and_policies_packet(self):
        """An IPv6 recursive next-hop miss punts the untouched packet."""
        prefix = "2001:db8:20::1/64"
        next_hop = "2001:db8:20::2"
        destination = "2001:db8:30::99"
        pkt = (
            Ether(src=self.access_phy.remote_mac, dst=self.bvi.local_mac)
            / IPv6(src="2001:db8:40::2", dst=destination)
            / Raw(b"ip6-incomplete-neighbor-miss")
        )
        pol = VppPolicer(
            self,
            "copp-neighbor-miss-ip6-incomplete",
            1000,
            0,
            100000,
            0,
            rate_type=RATE.SSE2_QOS_RATE_API_KBPS,
            conform_action=DROP,
            exceed_action=DROP,
            violate_action=DROP,
        )
        route = VppIpRoute(
            self,
            destination,
            128,
            [VppRoutePath(next_hop, self.bvi.sw_if_index)],
        )

        self.vapi.sw_interface_add_del_address(
            sw_if_index=self.bvi.sw_if_index, prefix=prefix
        )
        try:
            route.add_vpp_config()
            pol.add_vpp_config()
            self.vapi.sonic_ext_copp_neighbor_miss_bind(
                policer_name=pol.name, is_bind=True
            )

            before = pol.get_stats()["conform_packets"]
            self.access_tap.enable_capture()
            self.access_phy.add_stream([pkt])
            self.pg_start(trace=True)
            self.access_tap.get_capture(
                expected_count=0,
                timeout=1,
                filter_out_fn=self._not_from(pkt[Ether].src),
            )
            self.assertEqual(pol.get_stats()["conform_packets"], before + 1)

            # Prove the same incomplete-adjacency path preserves the packet
            # when admitted, including the original routed destination.
            self.vapi.sonic_ext_copp_neighbor_miss_bind(
                policer_name=pol.name, is_bind=False
            )
            rx = self._punt(
                self.access_phy,
                pkt,
                self.access_tap,
                self._not_from(pkt[Ether].src),
            )
            self.assertEqual(bytes(rx), bytes(pkt))
        finally:
            self.vapi.sonic_ext_copp_neighbor_miss_bind(
                policer_name=pol.name, is_bind=False
            )
            if pol.query_vpp_config():
                pol.remove_vpp_config()
            if route.query_vpp_config():
                route.remove_vpp_config()
            self.vapi.sw_interface_add_del_address(
                sw_if_index=self.bvi.sw_if_index, prefix=prefix, is_add=0
            )


class TestSonicExtL2PuntStartupOff(SonicExtL2PuntBase):
    """SONiC-ext L2 punt with punt-via-member off in startup.conf"""

    extra_vpp_config = ["sonic-ext", "{", "punt-via-member", "off", "}"]
    PUNT_VIA_MEMBER = False

    def test_punt_via_member_startup_off(self):
        """punt-via-member off at startup never wires the aggregate-tap redirect"""
        self.assertFalse(
            self.vapi.sonic_ext_feature_get(feature="punt-via-member").enabled
        )
        for itf in self.vapi.sw_interface_dump():
            output = self.vapi.cli("show interface features %s" % itf.interface_name)
            features = (line.strip() for line in output.splitlines())
            self.assertFalse(
                "sonic-ext-aggr-tap-redirect" in features,
                "sonic-ext-aggr-tap-redirect on %s" % itf.interface_name,
            )

        redirected = self._err(REDIRECTED)
        not_redirected = self._err(NOT_REDIRECTED)

        rx = self._punt(
            self.access_phy,
            self._arp_request(self.access_phy),
            self.aggr_tap,
            self._not_arp,
            flood_to=[(self.trunk_phy, True)],
        )

        self.assertEqual(rx[ARP].pdst, "10.0.0.1")
        # Unlike a runtime off, the redirect node is not on the arc to count.
        self.assertEqual(self._err(REDIRECTED), redirected)
        self.assertEqual(self._err(NOT_REDIRECTED), not_redirected)


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
