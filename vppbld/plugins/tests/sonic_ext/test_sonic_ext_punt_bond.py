#!/usr/bin/env python3
"""SONiC-ext L3 punt tests over a bond (port-channel).

Companion to test_sonic_ext.py, which covers the L2/bridge-domain case.
Here the aggregate is a bond master rather than a BVI, and the punt paths
are pure L3:

  sonic-ext-capture
      device-input feature on each bond *member*.  It must run before
      bond-input, because bond-input overwrites VLIB_RX with the bond
      master and the whole point of the cookie is to remember which
      physical member the frame actually arrived on.

  sonic-ext-aggr-tap-redirect
      interface-output feature on the host tap of the bond master and of
      every routed sub-interface of the bond (sonic_ext_phy_is_bond()
      resolves the super hw, so it is true for both).  It rewrites
      VLIB_TX from the aggregate tap to the ingress member's tap.

Topology:

    pg0, pg1                bond members, each with its own LCP pair
    BondEthernet0           bond master, 10.10.10.1/24   -> tap hbond0
    BondEthernet0.100       routed sub-interface, 10.10.100.1/24
                                                         -> tap hbond0.100

Scenario 1 -- ARP.  A broadcast ARP request for the bond address arrives on
a member.  "lcp ethertype enable 0x0806" (which SONiC issues in
init_vpp_client(), see SaiVppXlate.c) points ethernet-input's L3 demux at
linux-cp-punt-xc, which targets the LCP tap of the RX interface -- the bond
tap.  aggr-tap-redirect then moves it to the member's tap.

Scenario 2 -- ICMP.  A unicast ICMP echo request for the bond address
reaches ip4-local, is punted to ip4-punt, and linux-cp's punt-redirect
(installed per phy by lcp_itf_pair_create) DVR-forwards it out the bond
tap, where aggr-tap-redirect moves it to the member's tap.

Both scenarios are then repeated tagged, against BondEthernet0.100.  The
punted copy must still surface on the *member* tap -- a routed
sub-interface of a bond has its own LCP pair, but that tap is an aggregate
tap just like the master's.

Every test injects on both members in turn and asserts the punt lands on
the tap of the member that received it; that is the property the two nodes
exist to provide.
"""

import unittest

from scapy.layers.inet import ICMP, IP
from scapy.layers.l2 import ARP, Dot1Q, Ether
from scapy.packet import Raw

from asfframework import VppTestRunner, get_testcase_dirname
from config import config
from framework import VppTestCase
from host_interface import HostInterface
from vpp_bond_interface import VppBondInterface
from vpp_papi import VppEnum
from vpp_qemu_utils import create_namespace, delete_all_namespaces, set_interface_up
from vpp_sub_interface import VppDot1QSubint

BCAST_MAC = "ff:ff:ff:ff:ff:ff"
ARP_ETHERTYPE = 0x0806

REDIRECTED = (
    "/err/sonic-ext-aggr-tap-redirect/aggregate tap punt redirected to member tap"
)
NO_LCP = (
    "/err/sonic-ext-aggr-tap-redirect/"
    "no LCP pair for original phy -- left on aggregate tap"
)
NO_COOKIE = (
    "/err/sonic-ext-aggr-tap-redirect/no capture cookie -- left on aggregate tap"
)
DISABLED = (
    "/err/sonic-ext-aggr-tap-redirect/"
    "punt-via-member disabled -- left on aggregate tap"
)


@unittest.skipIf("linux-cp" in config.excluded_plugins, "Exclude linux-cp plugin tests")
@unittest.skipIf(
    "sonic_ext" in config.excluded_plugins, "Exclude sonic-ext plugin tests"
)
@unittest.skipIf(config.skip_netns_tests, "netns not available or disabled from cli")
class TestSonicExtBondPunt(VppTestCase):
    """SONiC-ext L3 punt over a bond: capture and aggregate-tap redirect"""

    extra_vpp_plugin_config = [
        # Both sonic-ext and linux-cp are .default_disabled.
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
        # The ping plugin registers ICMP4 echo-request/echo-reply handlers on
        # ip4-icmp-input (ping.c: ip4_icmp_register_type).  With it loaded VPP
        # answers the echo itself and the frame never reaches ip4-punt, so
        # there is nothing to redirect.  Unregistered ICMP types fall through
        # to ip4-punt, which is the path under test.
        "plugin",
        "ping_plugin.so",
        "{",
        "disable",
        "}",
    ]

    VLAN_ID = 100

    BOND_IP4 = "10.10.10.1"
    BOND_REMOTE_IP4 = "10.10.10.2"
    VLAN_IP4 = "10.10.100.1"
    VLAN_REMOTE_IP4 = "10.10.100.2"
    PREFIX_LEN = 24

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
        super(TestSonicExtBondPunt, self).setUp()

        self.create_pg_interfaces(range(2))
        for i in self.pg_interfaces:
            i.admin_up()

        self.lcp_pairs = []

        # Member LCP pairs first, mirroring SONiC: the host netdev for a
        # physical port exists before the port-channel is built.  Creating the
        # pair is what enables sonic-ext-capture on the member, since
        # sonic_ext_phy_is_aggregate() is false for a plain pg interface.
        self.member_taps = [
            self._lcp_create(self.pg0, "hpg0"),
            self._lcp_create(self.pg1, "hpg1"),
        ]

        self.bond = VppBondInterface(
            self,
            mode=VppEnum.vl_api_bond_mode_t.BOND_API_MODE_XOR,
            lb=VppEnum.vl_api_bond_lb_algo_t.BOND_API_LB_ALGO_L34,
        )
        self.bond.add_vpp_config()
        self.bond.admin_up()
        for i in self.pg_interfaces:
            self.bond.add_member_vpp_bond_interface(sw_if_index=i.sw_if_index)

        self.vapi.sw_interface_add_del_address(
            sw_if_index=self.bond.sw_if_index,
            prefix="%s/%d" % (self.BOND_IP4, self.PREFIX_LEN),
        )

        # Routed sub-interface of the bond.  No VTR: this is L3, so
        # ethernet-input's sub-interface classification pops the tag.
        self.vlan_if = VppDot1QSubint(self, self.bond, self.VLAN_ID)
        self.vlan_if.admin_up()
        self.vapi.sw_interface_add_del_address(
            sw_if_index=self.vlan_if.sw_if_index,
            prefix="%s/%d" % (self.VLAN_IP4, self.PREFIX_LEN),
        )

        # Aggregate LCP pairs.  The bond master's pair must exist before the
        # sub-interface's: lcp_itf_pair_create() looks up the parent pair to
        # find the Linux netdev it should hang the VLAN device off.
        self.bond_tap = self._lcp_create(self.bond, "hbond0")
        self.vlan_tap = self._lcp_create(self.vlan_if, "hbond0.100")
        self.logger.info("lcp:\n%s", self.vapi.cli("show lcp"))

        # Without this ARP goes to arp-input and VPP answers it locally;
        # linux-cp-arp-phy only copies ARP *replies* to the host.  SONiC
        # registers the same ethertype in init_vpp_client().
        self.vapi.cli("lcp ethertype enable %d" % ARP_ETHERTYPE)
        self.logger.info("lcp ethertype:\n%s", self.vapi.cli("show lcp ethertype"))

        self.vapi.cli("sonic-ext punt-via-member on")
        self.logger.info("sonic-ext:\n%s", self.vapi.cli("show sonic-ext"))

    def tearDown(self):
        for tap in self.member_taps + [self.bond_tap, self.vlan_tap]:
            tap.disable_capture()

        for itf in reversed(self.lcp_pairs):
            self.vapi.cli("lcp delete %s" % itf)

        self.vapi.sw_interface_add_del_address(
            sw_if_index=self.vlan_if.sw_if_index,
            prefix="%s/%d" % (self.VLAN_IP4, self.PREFIX_LEN),
            is_add=0,
        )
        self.vlan_if.admin_down()
        self.vlan_if.remove_vpp_config()

        self.vapi.sw_interface_add_del_address(
            sw_if_index=self.bond.sw_if_index,
            prefix="%s/%d" % (self.BOND_IP4, self.PREFIX_LEN),
            is_add=0,
        )
        for i in self.pg_interfaces:
            self.bond.detach_vpp_bond_interface(sw_if_index=i.sw_if_index)
        self.bond.admin_down()
        self.bond.remove_vpp_config()

        for i in self.pg_interfaces:
            i.admin_down()
        super(TestSonicExtBondPunt, self).tearDown()

    def show_commands_at_teardown(self):
        self.logger.info(self.vapi.cli("show interface"))
        self.logger.info(self.vapi.cli("show lcp"))
        self.logger.info(self.vapi.cli("show sonic-ext"))

    def _lcp_create(self, itf, host_if_name):
        """Create an LCP pair and return a HostInterface for its Linux tap."""
        self.vapi.cli(
            "lcp create %s host-if %s netns %s" % (itf, host_if_name, self.ns_name)
        )
        self.lcp_pairs.append(itf)
        set_interface_up(self.ns_name, host_if_name)
        return HostInterface(self, self.ns_name, host_if_name)

    #
    # packet builders
    #

    def _arp_request(self, member, tagged=False):
        pkt = Ether(src=member.remote_mac, dst=BCAST_MAC)
        if tagged:
            pkt /= Dot1Q(vlan=self.VLAN_ID)
        return pkt / ARP(
            op="who-has",
            hwsrc=member.remote_mac,
            psrc=self.VLAN_REMOTE_IP4 if tagged else self.BOND_REMOTE_IP4,
            pdst=self.VLAN_IP4 if tagged else self.BOND_IP4,
        )

    def _icmp_request(self, member, tagged=False):
        # Unicast to the bond's own MAC: the sub-interface shares it.
        pkt = Ether(src=member.remote_mac, dst=self.bond.local_mac)
        if tagged:
            pkt /= Dot1Q(vlan=self.VLAN_ID)
        return (
            pkt
            / IP(
                src=self.VLAN_REMOTE_IP4 if tagged else self.BOND_REMOTE_IP4,
                dst=self.VLAN_IP4 if tagged else self.BOND_IP4,
            )
            / ICMP(type="echo-request", id=0x4242, seq=1)
            / Raw(b"\xa5" * 64)
        )

    #
    # capture filters -- the taps also carry kernel-generated traffic
    #

    @staticmethod
    def _not_arp(p):
        return not p.haslayer(ARP)

    @staticmethod
    def _not_icmp(p):
        return not p.haslayer(ICMP)

    #
    # helpers
    #

    def _err(self, name):
        return self.statistics.get_err_counter(name)

    def _punt_on_member(self, member, tap, pkt, filter_out_fn):
        """Inject on *member*, expect exactly one redirected copy on *tap*."""
        before = self._err(REDIRECTED)
        no_lcp_before = self._err(NO_LCP)
        no_cookie_before = self._err(NO_COOKIE)

        tap.enable_capture()
        member.add_stream([pkt])
        self.pg_start(trace=True)
        try:
            rxs = tap.get_capture(
                expected_count=1, timeout=5, filter_out_fn=filter_out_fn
            )
        finally:
            # We drive the packet generator directly because the frame leaves
            # via a Linux tap, so nothing else dumps the trace for us.
            self.logger.debug(self.vapi.cli("show trace max 50"))

        self.assertEqual(len(rxs), 1)
        # A single injected frame that was redirected exactly once, and that
        # surfaced on this member's tap, cannot also have gone anywhere else.
        self.assertEqual(
            self._err(REDIRECTED),
            before + 1,
            "punt was not redirected from the aggregate tap to %s" % tap,
        )
        # These would fire if sonic-ext-capture ran after bond-input (the
        # cookie would name the bond, whose tap is the excluded aggregate tap)
        # or if it never ran at all.
        self.assertEqual(self._err(NO_LCP), no_lcp_before)
        self.assertEqual(self._err(NO_COOKIE), no_cookie_before)
        return rxs[0]

    def _punt_on_each_member(self, build_pkt, filter_out_fn, tagged):
        """Run the same punt from every bond member and check tap selection."""
        results = []
        for member, tap in zip(self.pg_interfaces, self.member_taps):
            rx = self._punt_on_member(
                member, tap, build_pkt(member, tagged=tagged), filter_out_fn
            )
            if tagged:
                self.assertTrue(
                    rx.haslayer(Dot1Q),
                    "punted frame lost its VLAN tag on the way to %s" % tap,
                )
                self.assertEqual(rx[Dot1Q].vlan, self.VLAN_ID)
            else:
                self.assertFalse(
                    rx.haslayer(Dot1Q),
                    "punted frame unexpectedly carries a VLAN tag on %s" % tap,
                )
            self.assertEqual(rx[Ether].src, member.remote_mac)
            results.append((member, rx))
        return results

    #
    # scenario 1 -- ARP for the bond address
    #

    def test_untagged_arp_to_member_tap(self):
        """Untagged ARP for the bond IP is punted to the ingress member's tap"""
        for member, rx in self._punt_on_each_member(
            self._arp_request, self._not_arp, tagged=False
        ):
            self.assertEqual(rx[ARP].pdst, self.BOND_IP4)
            self.assertEqual(rx[ARP].psrc, self.BOND_REMOTE_IP4)
            self.assertEqual(rx[Ether].dst, BCAST_MAC)

    def test_tagged_arp_to_member_tap(self):
        """Tagged ARP for the sub-interface IP is punted to the member's tap"""
        for member, rx in self._punt_on_each_member(
            self._arp_request, self._not_arp, tagged=True
        ):
            self.assertEqual(rx[ARP].pdst, self.VLAN_IP4)
            self.assertEqual(rx[ARP].psrc, self.VLAN_REMOTE_IP4)
            self.assertEqual(rx[Ether].dst, BCAST_MAC)

    #
    # scenario 2 -- ICMP for the bond address
    #

    def test_untagged_icmp_to_member_tap(self):
        """Untagged ICMP to the bond IP is punted to the ingress member's tap"""
        for member, rx in self._punt_on_each_member(
            self._icmp_request, self._not_icmp, tagged=False
        ):
            self.assertEqual(rx[IP].dst, self.BOND_IP4)
            self.assertEqual(rx[IP].src, self.BOND_REMOTE_IP4)
            self.assertEqual(rx[Ether].dst, self.bond.local_mac)

    def test_tagged_icmp_to_member_tap(self):
        """Tagged ICMP to the sub-interface IP is punted to the member's tap"""
        for member, rx in self._punt_on_each_member(
            self._icmp_request, self._not_icmp, tagged=True
        ):
            self.assertEqual(rx[IP].dst, self.VLAN_IP4)
            self.assertEqual(rx[IP].src, self.VLAN_REMOTE_IP4)
            self.assertEqual(rx[Ether].dst, self.bond.local_mac)

    #
    # negative control
    #

    def test_punt_via_member_disabled(self):
        """With punt-via-member off the punt stays on the bond's own tap"""
        self.vapi.cli("sonic-ext punt-via-member off")
        try:
            before = self._err(DISABLED)

            self.bond_tap.enable_capture()
            self.pg0.add_stream([self._arp_request(self.pg0)])
            self.pg_start(trace=True)
            try:
                rxs = self.bond_tap.get_capture(
                    expected_count=1, timeout=5, filter_out_fn=self._not_arp
                )
            finally:
                self.logger.debug(self.vapi.cli("show trace max 50"))

            self.assertEqual(len(rxs), 1)
            self.assertEqual(rxs[0][ARP].pdst, self.BOND_IP4)
            self.assertEqual(self._err(DISABLED), before + 1)
        finally:
            self.vapi.cli("sonic-ext punt-via-member on")


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
