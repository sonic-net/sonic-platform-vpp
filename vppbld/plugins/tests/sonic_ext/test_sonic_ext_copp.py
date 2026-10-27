#!/usr/bin/env python3
"""sonic-ext CoPP dataplane tests.

Covers the CoPP enforcement added by sonic-platform-vpp PR #281
(issue #316): the nodes that meter host-bound control-plane traffic
against a named VPP policer and drop what the policer says to drop.

  sonic-ext-copp-ifout
      interface-output feature on each phy's linux-cp host tap.  Matches
      by ethertype (ARP/LACP/LLDP...), structurally for UDLD, and by
      "IPv4 with TTL <= 1" for TTL_ERROR. NEIGHBOR_MISS uses a dedicated
      semantic row selected only by sonic_ext's per-buffer CoPP reason.

  sonic-ext-copp-ip2me / sonic-ext-copp-ip2me-ip6
      ip4-punt / ip6-punt features.  IPv4 matches a destination-address
      set (IP2ME/SSH/SNMP) or a TCP port (BGP); IPv6 matches only the TCP
      port slot (BGPv6).  A port match wins over an address match.

  sonic-ext-copp-udld
      snap-input handler for the Cisco UDLD SNAP protocol: restores the
      Ethernet header and hands the frame to copp-ifout on the ingress
      phy's host tap.

  sonic-ext-copp-ttl-punt
      reached from ip4-rewrite (vppbld patch 0021) for transit packets
      whose TTL expires, when the TTL_ERROR trap is installed: redirects
      the original frame to the ingress phy's host tap instead of letting
      VPP answer with ICMP time-exceeded.

Topology, using the linux-cp unittest pairing so no real tap is needed:

    pg0   "phy", IPv4/IPv6 addressed           <- traffic injected here
    pg1   pg0's linux-cp host interface         <- punted traffic seen here
    pg2   routed egress for transit (TTL) traffic

Bindings are made directly through the sonic-ext binary API, exactly as
sairedis does, but against policers created by the test, so the tests
isolate the dataplane from SAI.  Each test asserts three things together:
what reached the host interface, what the native policer counters
(/net/policer/*) recorded, and what sonic-ext's own per-trap counters
recorded.
"""

import ipaddress
import unittest

from scapy.contrib.lldp import (
    LLDPDUChassisID,
    LLDPDUEndOfLLDPDU,
    LLDPDUPortID,
    LLDPDUTimeToLive,
)
from scapy.layers.inet import ICMP, IP, IPOption_Router_Alert, TCP
from scapy.layers.inet6 import IPv6
from scapy.layers.l2 import Dot1Q, Dot3, Ether, LLC, SNAP
from scapy.packet import Raw

from asfframework import VppTestRunner
from config import config
from framework import VppTestCase
from vpp_papi import VppEnum
from vpp_policer import PolicerAction, VppPolicer
from vpp_sub_interface import VppDot1QSubint

LLDP_ETHERTYPE = 0x88CC
LLDP_MCAST_MAC = "01:80:c2:00:00:0e"
LACP_ETHERTYPE = 0x8809
IP4_ETHERTYPE = 0x0800
# sonic-ext's internal bind-table key for UDLD (SONIC_EXT_COPP_UDLD_ETHERTYPE);
# UDLD is 802.3/LLC framed and has no real ethertype.
UDLD_KEY = 0x0067
UDLD_MCAST_MAC = "01:00:0c:cc:cc:cc"
CISCO_OUI = 0x00000C
UDLD_SNAP_PID = 0x0111
BGP_PORT = 179
# An IP protocol nothing in VPP registers, so a packet to a local address
# deterministically falls through ip4-local into ip4-punt.
UNREGISTERED_PROTO = 253
VLAN_ID = 100

QOS = VppEnum.vl_api_sse2_qos_action_type_t
RATE = VppEnum.vl_api_sse2_qos_rate_type_t
TRANSMIT = PolicerAction(QOS.SSE2_QOS_ACTION_API_TRANSMIT, 0)
DROP = PolicerAction(QOS.SSE2_QOS_ACTION_API_DROP, 0)

IFOUT_ERR = "/err/sonic-ext-copp-ifout/"
IP2ME_ERR = "/err/sonic-ext-copp-ip2me/"
IP2ME6_ERR = "/err/sonic-ext-copp-ip2me-ip6/"
UNRESOLVED = "packets dropped (policer name not yet resolvable)"
UNMATCHED_LLC = "packets dropped (LLC/SNAP dead-end, no UDLD match)"
TTL_REDIRECTED = (
    "/err/sonic-ext-copp-ttl-punt/transit TTL_ERROR punted to host for CoPP policing"
)
UDLD_REDIRECTED = (
    "/err/sonic-ext-copp-udld/UDLD (LLC/SNAP) redirected to copp-ifout for policing"
)
UDLD_NO_LCP = "/err/sonic-ext-copp-udld/UDLD dropped -- no LCP pair for ingress phy"
TTL_NO_LCP = (
    "/err/sonic-ext-copp-ttl-punt/"
    "TTL_ERROR punt dropped -- no LCP pair for ingress phy"
)

COLORS = ("conform", "exceed", "violate")


@unittest.skipIf("linux-cp" in config.excluded_plugins, "Exclude linux-cp plugin tests")
@unittest.skipIf("policer" in config.excluded_plugins, "Exclude policer plugin tests")
@unittest.skipIf(
    "sonic_ext" in config.excluded_plugins, "Exclude sonic-ext plugin tests"
)
class SonicExtCoppDataplaneBase(VppTestCase):
    """sonic-ext CoPP dataplane test base"""

    # Policer token buckets and the sonic-ext counters are shared plugin state;
    # keep the dataplane accounting assertions deterministic.
    vpp_worker_count = 0

    extra_vpp_plugin_config = [
        # linux-cp and sonic-ext are .default_disabled; the unittest plugin
        # provides "test lcp add", which pairs two pg interfaces without a
        # kernel tap.
        "plugin",
        "linux_cp_plugin.so",
        "{",
        "enable",
        "}",
        "plugin",
        "linux_cp_unittest_plugin.so",
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
        # The LLDP plugin owns ethertype 0x88cc; with it loaded linux-cp
        # cannot register LLDP for punting.  Same as upstream's
        # TestLinuxCPEthertype.
        "plugin",
        "lldp_plugin.so",
        "{",
        "disable",
        "}",
    ]

    N_PKTS = 40

    @classmethod
    def setUpClass(cls):
        super(SonicExtCoppDataplaneBase, cls).setUpClass()
        cls.create_pg_interfaces(range(3))
        # SONiC registers the CoPP ethertypes with linux-cp so ethernet-input
        # punts them to the host tap (init_vpp_client() in SaiVppXlate.c).
        # Registration is global and has no undo, so do it once per VPP.
        cls.vapi.cli("lcp ethertype enable %d" % LLDP_ETHERTYPE)

    def setUp(self):
        super(SonicExtCoppDataplaneBase, self).setUp()
        for i in self.pg_interfaces:
            i.admin_up()
        self.phy, self.host, self.egress = self.pg0, self.pg1, self.pg2

        self.phy.config_ip4()
        self.phy.config_ip6()
        # The linux-cp ip6 punt redirect is a DVR path out of the host
        # interface, which needs IPv6 enabled there.
        self.host.config_ip6()
        self.egress.config_ip4()
        self.egress.generate_remote_hosts(1)
        self.egress.configure_ipv4_neighbors()

        self.vapi.cli("test lcp add phy %s host %s" % (self.phy, self.host))
        self.n_policers = 0

    def tearDown(self):
        self.vapi.cli("test lcp del phy %s host %s" % (self.phy, self.host))
        self.vapi.ip_neighbor_flush(
            af=VppEnum.vl_api_address_family_t.ADDRESS_IP4,
            sw_if_index=self.egress.sw_if_index,
        )
        self.egress.unconfig_ip4()
        self.host.unconfig_ip6()
        self.phy.unconfig_ip6()
        self.phy.unconfig_ip4()
        for i in self.pg_interfaces:
            i.admin_down()
        super(SonicExtCoppDataplaneBase, self).tearDown()

    def show_commands_at_teardown(self):
        self.logger.info(self.vapi.cli("show sonic-ext copp-ifout"))
        self.logger.info(self.vapi.cli("show sonic-ext copp-ip2me"))
        self.logger.info(self.vapi.cli("show policer"))

    #
    # policers and bindings -- every one is undone by an addCleanup
    #

    def policer(
        self,
        conform=TRANSMIT,
        exceed=DROP,
        violate=DROP,
        cir=8,
        cb=9216,
        rate_type=RATE.SSE2_QOS_RATE_API_KBPS,
        name=None,
        add=True,
    ):
        """A single-rate (1R2C) policer.  The defaults admit about one
        9 KB burst and then drop: 8 kbps refills only ~1 byte/ms, so a
        back-to-back stream drains the bucket deterministically."""
        if name is None:
            self.n_policers += 1
            name = "copp-test-%s-%d" % (self._testMethodName, self.n_policers)
        pol = VppPolicer(
            self,
            name,
            cir,
            0,
            cb,
            0,
            rate_type=rate_type,
            conform_action=conform,
            exceed_action=exceed,
            violate_action=violate,
        )
        if add:
            pol.add_vpp_config()
            self.addCleanup(self._remove_policer, pol)
        return pol

    def _remove_policer(self, pol):
        if pol.query_vpp_config():
            pol.remove_vpp_config()

    def bind_ifout(self, ethertype, pol, ttl_expiring=False):
        self.vapi.sonic_ext_copp_ifout_bind(
            ethertype=ethertype,
            policer_name=pol.name,
            is_bind=True,
            match_ip4_ttl_expiring=ttl_expiring,
        )
        self.addCleanup(
            self.vapi.sonic_ext_copp_ifout_bind,
            ethertype=ethertype,
            policer_name=pol.name,
            is_bind=False,
            match_ip4_ttl_expiring=ttl_expiring,
        )

    def bind_neighbor_miss(self, pol):
        self.vapi.sonic_ext_copp_neighbor_miss_bind(policer_name=pol.name, is_bind=True)
        self.addCleanup(
            self.vapi.sonic_ext_copp_neighbor_miss_bind,
            policer_name=pol.name,
            is_bind=False,
        )

    def add_ip2me_addr(self, addr):
        # sonic-ext keeps the address in network byte order and PAPI sends a
        # u32 big-endian, so the integer form of the address is what to pass.
        addr = int(ipaddress.IPv4Address(addr))
        self.vapi.sonic_ext_copp_ip2me_addr_add_del(addr=addr, is_add=True)
        self.addCleanup(
            self.vapi.sonic_ext_copp_ip2me_addr_add_del, addr=addr, is_add=False
        )

    def bind_ip2me(self, pol):
        self.vapi.sonic_ext_copp_ip2me_bind(policer_name=pol.name, is_bind=True)
        self.addCleanup(
            self.vapi.sonic_ext_copp_ip2me_bind, policer_name=pol.name, is_bind=False
        )

    def bind_ip2me_port(self, pol, port=BGP_PORT):
        self.vapi.sonic_ext_copp_ip2me_bind_condition(
            policer_name=pol.name, tcp_port=port, is_bind=True
        )
        self.addCleanup(
            self.vapi.sonic_ext_copp_ip2me_bind_condition,
            policer_name=pol.name,
            tcp_port=port,
            is_bind=False,
        )

    def bind_ttl_punt(self):
        self.vapi.sonic_ext_copp_ttl_punt_bind(is_bind=True)
        self.addCleanup(self.vapi.sonic_ext_copp_ttl_punt_bind, is_bind=False)

    #
    # counters
    #

    @staticmethod
    def native(pol):
        """Packets per color from the policer plugin's /net/policer/*."""
        s = pol.get_stats()
        return {c: s["%s_packets" % c] for c in COLORS}

    def ifout_counters(self, ethertype):
        r = self.vapi.sonic_ext_copp_ifout_get_counters(ethertype=ethertype)
        return {c: getattr(r, "%s_packets" % c) for c in COLORS}

    def ip2me_counters(self):
        r = self.vapi.sonic_ext_copp_ip2me_get_counters()
        return {c: getattr(r, "%s_packets" % c) for c in COLORS}

    @staticmethod
    def delta(after, before):
        return {c: after[c] - before[c] for c in COLORS}

    def err(self, name):
        return self.statistics.get_err_counter(name)

    #
    # traffic
    #

    def send_and_capture_host(self, pkts):
        """Send on the phy; return whatever reached the host interface.
        Unlike send_and_expect, the count is not known in advance: it is
        what the policer decided, and is checked against the counters."""
        self.pg_send(self.phy, pkts)
        rx = self.host._get_capture()
        self.logger.debug(self.vapi.cli("show trace max 20"))
        return [] if rx is None else list(rx)

    def assert_policed(self, pkts, pol, sonic_counters):
        """Send a burst against a "conform transmits, the rest drops"
        policer and check that the host saw exactly the conforming packets,
        and that the native and sonic-ext counters agree with each other
        and with the wire."""
        native_before = self.native(pol)
        sonic_before = sonic_counters()

        rx = self.send_and_capture_host(pkts)

        native = self.delta(self.native(pol), native_before)
        sonic = self.delta(sonic_counters(), sonic_before)

        self.assertEqual(sum(native.values()), len(pkts), "every packet metered once")
        self.assertEqual(native, sonic, "sonic-ext counters match /net/policer")
        self.assertEqual(len(rx), native["conform"], "only conforming packets pass")
        self.assertGreater(native["conform"], 0, "the burst fits nothing")
        self.assertGreater(
            native["exceed"] + native["violate"], 0, "the burst was never limited"
        )
        return rx

    def assert_unpoliced(self, pkts, pol, sonic_counters):
        """Every packet reaches the host and nothing is metered."""
        native_before = self.native(pol)
        sonic_before = sonic_counters()

        rx = self.send_and_capture_host(pkts)

        self.assertEqual(len(rx), len(pkts))
        self.assertEqual(self.native(pol), native_before)
        self.assertEqual(sonic_counters(), sonic_before)
        return rx

    def assert_dropped(self, pkts, err_name=None):
        before = self.err(err_name) if err_name else 0
        self.send_and_assert_no_replies(self.phy, pkts)
        if err_name:
            self.assertEqual(self.err(err_name) - before, len(pkts))

    #
    # packet builders
    #

    def lldp(self, n=None, tag=None):
        if tag is None:
            eth = Ether(
                src=self.phy.remote_mac, dst=LLDP_MCAST_MAC, type=LLDP_ETHERTYPE
            )
        else:
            eth = Ether(src=self.phy.remote_mac, dst=LLDP_MCAST_MAC) / Dot1Q(
                vlan=tag, type=LLDP_ETHERTYPE
            )
        # scapy's LLDP layers insist on sitting directly on Ether/Dot1Q.
        p = (
            eth
            / LLDPDUChassisID(subtype=4, id="01:02:03:04:05:06")
            / LLDPDUPortID(subtype=3, id="07:08:09:0a:0b:0c")
            / LLDPDUTimeToLive(ttl=120)
            / LLDPDUEndOfLLDPDU()
            / Raw(b"\x00" * 400)
        )
        return p * (n or self.N_PKTS)

    def udld(self, n=None, oui=CISCO_OUI, pid=UDLD_SNAP_PID):
        return (
            Dot3(src=self.phy.remote_mac, dst=UDLD_MCAST_MAC)
            / LLC(dsap=0xAA, ssap=0xAA, ctrl=3)
            / SNAP(OUI=oui, code=pid)
            / Raw(b"\x21" + b"\x00" * 300)
        ) * (n or self.N_PKTS)

    def ip4_to_me(self, n=None, proto=UNREGISTERED_PROTO, l4=None, size=400, **kw):
        ip = IP(src=self.phy.remote_ip4, dst=self.phy.local_ip4, **kw)
        if l4 is None:
            ip.proto = proto
            payload = ip / Raw(b"\xa5" * size)
        else:
            payload = ip / l4 / Raw(b"\xa5" * size)
        return (Ether(src=self.phy.remote_mac, dst=self.phy.local_mac) / payload) * (
            n or self.N_PKTS
        )

    def ip6_to_me(self, l4, n=None, size=400):
        return (
            Ether(src=self.phy.remote_mac, dst=self.phy.local_mac)
            / IPv6(src=self.phy.remote_ip6, dst=self.phy.local_ip6)
            / l4
            / Raw(b"\xa5" * size)
        ) * (n or self.N_PKTS)

    def ip4_transit(self, ttl, n=None):
        return (
            Ether(src=self.phy.remote_mac, dst=self.phy.local_mac)
            / IP(src=self.phy.remote_ip4, dst=self.egress.remote_hosts[0].ip4, ttl=ttl)
            / Raw(b"\xa5" * 200)
        ) * (n or self.N_PKTS)


class TestSonicExtCoppIfout(SonicExtCoppDataplaneBase):
    """sonic-ext CoPP: interface-output (ethertype) policing"""

    def test_ethertype_policed(self):
        """A bound ethertype is policed on the way to the host"""
        pol = self.policer()
        self.bind_ifout(LLDP_ETHERTYPE, pol)

        rx = self.assert_policed(
            self.lldp(), pol, lambda: self.ifout_counters(LLDP_ETHERTYPE)
        )
        for p in rx:
            self.assertEqual(p[Ether].type, LLDP_ETHERTYPE)
            self.assertEqual(p[Ether].src, self.phy.remote_mac)

    def test_unbound_ethertype_passes(self):
        """An ethertype with no binding reaches the host unpoliced"""
        # Bind something else to an all-drop policer: LLDP must not hit it.
        pol = self.policer(conform=DROP)
        self.bind_ifout(LACP_ETHERTYPE, pol)

        self.assert_unpoliced(
            self.lldp(), pol, lambda: self.ifout_counters(LACP_ETHERTYPE)
        )

    def test_neighbor_miss_row_does_not_match_wire_traffic(self):
        """The semantic reason row must not classify an ordinary frame"""
        pol = self.policer(conform=DROP)
        self.bind_neighbor_miss(pol)

        native_before = self.native(pol)
        rx = self.send_and_capture_host(self.lldp())

        self.assertEqual(len(rx), self.N_PKTS)
        self.assertEqual(self.native(pol), native_before)
        self.assertIn("neighbor-miss", self.vapi.cli("show sonic-ext copp-ifout"))

    def test_neighbor_miss_bind_replace_unbind_reuses_one_slot(self):
        """The singleton semantic row is replaced and its slot is reusable"""
        first = self.policer(conform=DROP, name="copp-neighbor-miss-first")
        second = self.policer(conform=DROP, name="copp-neighbor-miss-second")

        self.vapi.sonic_ext_copp_neighbor_miss_bind(
            policer_name=first.name, is_bind=True
        )
        self.addCleanup(
            self.vapi.sonic_ext_copp_neighbor_miss_bind,
            policer_name=first.name,
            is_bind=False,
        )
        self.vapi.sonic_ext_copp_neighbor_miss_bind(
            policer_name=second.name, is_bind=True
        )
        show = self.vapi.cli("show sonic-ext copp-ifout")
        self.assertNotIn(first.name, show)
        rows = [line for line in show.splitlines() if second.name in line]
        self.assertEqual(len(rows), 1)
        self.assertIn("neighbor-miss", rows[0])

        for _ in range(17):
            self.vapi.sonic_ext_copp_neighbor_miss_bind(
                policer_name=second.name, is_bind=False
            )
            self.vapi.sonic_ext_copp_neighbor_miss_bind(
                policer_name=second.name, is_bind=True
            )

        self.vapi.sonic_ext_copp_neighbor_miss_bind(
            policer_name=second.name, is_bind=False
        )
        self.assertNotIn("neighbor-miss", self.vapi.cli("show sonic-ext copp-ifout"))

    def test_drop_action_honored_for_conforming(self):
        """The configured per-color action decides, not the color itself"""
        pol = self.policer(conform=DROP, cb=10**6)
        self.bind_ifout(LLDP_ETHERTYPE, pol)

        before = self.native(pol)
        self.assert_dropped(self.lldp())
        d = self.delta(self.native(pol), before)
        self.assertEqual(d["conform"], self.N_PKTS)

    def test_unbind_restores_passthrough(self):
        """Unbinding removes policing; rebinding restores it"""
        pol = self.policer(conform=DROP)
        self.vapi.sonic_ext_copp_ifout_bind(
            ethertype=LLDP_ETHERTYPE, policer_name=pol.name, is_bind=True
        )
        self.assert_dropped(self.lldp())

        self.vapi.sonic_ext_copp_ifout_bind(
            ethertype=LLDP_ETHERTYPE, policer_name=pol.name, is_bind=False
        )
        with self.vapi.assert_negative_api_retval():
            self.vapi.sonic_ext_copp_ifout_get_counters(ethertype=LLDP_ETHERTYPE)
        self.send_and_expect(self.phy, self.lldp(), self.host)

        self.bind_ifout(LLDP_ETHERTYPE, pol)
        self.assert_dropped(self.lldp())

    def test_unbind_reuses_table_slots(self):
        """Repeated trap churn does not exhaust the 16-entry bind table"""
        pol = self.policer(conform=DROP, cb=10**6)

        for _ in range(17):
            self.vapi.sonic_ext_copp_ifout_bind(
                ethertype=LLDP_ETHERTYPE, policer_name=pol.name, is_bind=True
            )
            self.vapi.sonic_ext_copp_ifout_bind(
                ethertype=LLDP_ETHERTYPE, policer_name=pol.name, is_bind=False
            )

        self.bind_ifout(LLDP_ETHERTYPE, pol)
        before = self.native(pol)
        self.assert_dropped(self.lldp())
        self.assertEqual(self.delta(self.native(pol), before)["conform"], self.N_PKTS)

    def test_unresolved_policer_drops(self):
        """A binding whose policer does not exist yet drops, then resolves
        lazily once the policer is created, and drops again once deleted"""
        pol = self.policer(add=False)
        self.bind_ifout(LLDP_ETHERTYPE, pol)

        self.assert_dropped(self.lldp(), IFOUT_ERR + UNRESOLVED)

        pol.add_vpp_config()
        self.addCleanup(self._remove_policer, pol)
        self.assert_policed(
            self.lldp(), pol, lambda: self.ifout_counters(LLDP_ETHERTYPE)
        )

        pol.remove_vpp_config()
        self.assert_dropped(self.lldp(), IFOUT_ERR + UNRESOLVED)

    def test_cached_policer_slot_is_validated_by_name(self):
        """A recycled VPP pool slot is not mistaken for the bound policer"""
        original = self.policer(conform=DROP, cb=10**6)
        self.bind_ifout(LLDP_ETHERTYPE, original)

        # Resolve and cache original's VPP pool index in the bind entry.
        self.assert_dropped(self.lldp())
        original_index = original.policer_index
        original.remove_vpp_config()

        replacement = self.policer(conform=TRANSMIT, cb=10**6)
        self.assertEqual(replacement.policer_index, original_index)
        replacement_before = self.native(replacement)

        self.assert_dropped(self.lldp(), IFOUT_ERR + UNRESOLVED)
        self.assertEqual(self.native(replacement), replacement_before)

    def test_vlan_tagged(self):
        """The ethertype behind an 802.1Q tag is what is matched"""
        phy_sub = VppDot1QSubint(self, self.phy, VLAN_ID)
        host_sub = VppDot1QSubint(self, self.host, VLAN_ID)
        for s in (phy_sub, host_sub):
            s.admin_up()
        self.vapi.cli("test lcp add phy %s host %s" % (phy_sub, host_sub))
        try:
            self.assertIn(
                "sonic-ext-copp-ifout",
                self.vapi.cli("show interface features %s" % host_sub),
            )

            pol = self.policer()
            self.bind_ifout(LLDP_ETHERTYPE, pol)

            rx = self.assert_policed(
                self.lldp(tag=VLAN_ID),
                pol,
                lambda: self.ifout_counters(LLDP_ETHERTYPE),
            )
            for p in rx:
                self.assertEqual(p[Dot1Q].vlan, VLAN_ID)
                self.assertEqual(p[Dot1Q].type, LLDP_ETHERTYPE)
        finally:
            # The pair must go before its sub-interfaces do.
            self.vapi.cli("test lcp del phy %s host %s" % (phy_sub, host_sub))
            for s in (phy_sub, host_sub):
                s.admin_down()
                s.remove_vpp_config()


class TestSonicExtCoppUdld(SonicExtCoppDataplaneBase):
    """sonic-ext CoPP: UDLD (802.3 LLC/SNAP) policing"""

    def test_udld_policed(self):
        """Real Cisco SNAP UDLD reaches the host intact and is policed"""
        pol = self.policer()
        self.bind_ifout(UDLD_KEY, pol)
        redirected = self.err(UDLD_REDIRECTED)

        pkts = self.udld()
        rx = self.assert_policed(pkts, pol, lambda: self.ifout_counters(UDLD_KEY))

        self.assertEqual(self.err(UDLD_REDIRECTED) - redirected, len(pkts))
        for p in rx:
            # sonic-ext-copp-udld rewound past LLC/SNAP: the full 802.3 frame
            # is what the host sees.
            self.assertEqual(bytes(p), bytes(pkts[0]))

    def test_udld_unbound_dropped(self):
        """With no UDLD binding the frame is dropped, not leaked unpoliced"""
        self.assert_dropped(self.udld(), IFOUT_ERR + UNMATCHED_LLC)

    def test_length_field_is_not_an_ethertype(self):
        """An 802.3 length that happens to equal the UDLD key is not UDLD"""
        pol = self.policer(conform=DROP)
        self.bind_ifout(UDLD_KEY, pol)
        before = self.native(pol)

        # Non-SNAP LLC with an 802.3 length of exactly UDLD_KEY: llc-input has
        # no handler for this SAP, so it never reaches the host at all.
        pkt = (
            Dot3(src=self.phy.remote_mac, dst=UDLD_MCAST_MAC, len=UDLD_KEY)
            / LLC(dsap=0x30, ssap=0x30, ctrl=3)
            / Raw(b"0" * (UDLD_KEY - 3))
        )
        self.send_and_assert_no_replies(self.phy, pkt * self.N_PKTS)
        self.assertEqual(self.native(pol), before)

    def test_non_cisco_snap_is_not_udld(self):
        """Only the registered Cisco OUI and UDLD PID reach the CoPP node"""
        pol = self.policer(conform=DROP)
        self.bind_ifout(UDLD_KEY, pol)
        before = self.native(pol)
        redirected = self.err(UDLD_REDIRECTED)

        self.send_and_assert_no_replies(self.phy, self.udld(oui=0x000001))

        self.assertEqual(self.native(pol), before)
        self.assertEqual(self.err(UDLD_REDIRECTED), redirected)

    def test_udld_without_lcp_pair_drops(self):
        """A genuine UDLD frame fails closed when ingress has no host pair"""
        before = self.err(UDLD_NO_LCP)
        self.vapi.cli("test lcp del phy %s host %s" % (self.phy, self.host))
        try:
            self.send_and_assert_no_replies(self.phy, self.udld())
            self.assertEqual(self.err(UDLD_NO_LCP) - before, self.N_PKTS)
        finally:
            self.vapi.cli("test lcp add phy %s host %s" % (self.phy, self.host))


class TestSonicExtCoppTtl(SonicExtCoppDataplaneBase):
    """sonic-ext CoPP: TTL_ERROR punt and policing"""

    def test_ttl_expired_icmp_without_trap(self):
        """Without the TTL_ERROR trap VPP answers with time-exceeded"""
        # The reply goes back out the phy, so it needs the remote's MAC.  One
        # packet only: VPP rate-limits ICMP errors.
        self.phy.resolve_arp()
        rx = self.send_and_expect(self.phy, self.ip4_transit(ttl=1, n=1), self.phy)
        self.assertEqual(rx[0][ICMP].type, 11)
        self.host.assert_nothing_captured()

    def test_ttl_expired_punted_and_policed(self):
        """With the trap, expired transit IPv4 goes to the ingress host
        interface instead of out the egress, and is policed there"""
        self.bind_ttl_punt()
        pol = self.policer()
        self.bind_ifout(IP4_ETHERTYPE, pol, ttl_expiring=True)
        redirected = self.err(TTL_REDIRECTED)

        pkts = self.ip4_transit(ttl=1)
        rx = self.assert_policed(pkts, pol, lambda: self.ifout_counters(IP4_ETHERTYPE))

        self.assertEqual(self.err(TTL_REDIRECTED) - redirected, len(pkts))
        for p in rx:
            self.assertEqual(p[IP].src, self.phy.remote_ip4)
            self.assertEqual(p[IP].dst, self.egress.remote_hosts[0].ip4)
        self.phy.assert_nothing_captured()
        self.egress.assert_nothing_captured()

    def test_ttl_expired_without_lcp_pair_drops(self):
        """TTL_ERROR fails closed when the ingress phy has no host pair"""
        self.bind_ttl_punt()
        before = self.err(TTL_NO_LCP)
        self.vapi.cli("test lcp del phy %s host %s" % (self.phy, self.host))
        try:
            self.send_and_assert_no_replies(self.phy, self.ip4_transit(ttl=1))
            self.assertEqual(self.err(TTL_NO_LCP) - before, self.N_PKTS)
            self.egress.assert_nothing_captured()
        finally:
            self.vapi.cli("test lcp add phy %s host %s" % (self.phy, self.host))

    def test_live_ttl_not_matched(self):
        """The TTL_ERROR binding ignores host-bound IPv4 with a live TTL"""
        pol = self.policer(conform=DROP)
        self.bind_ifout(IP4_ETHERTYPE, pol, ttl_expiring=True)

        self.assert_unpoliced(
            self.ip4_to_me(ttl=64), pol, lambda: self.ifout_counters(IP4_ETHERTYPE)
        )

    def test_local_ttl_one_does_not_take_transit_punt(self):
        """A locally addressed TTL=1 packet never enters the transit hook"""
        self.bind_ttl_punt()
        redirected = self.err(TTL_REDIRECTED)

        self.send_and_expect(self.phy, self.ip4_to_me(n=1, ttl=1), self.host)

        self.assertEqual(self.err(TTL_REDIRECTED), redirected)

    def test_live_transit_forwarded(self):
        """With the trap installed, transit traffic with a live TTL is
        forwarded normally"""
        self.bind_ttl_punt()
        self.send_and_expect(self.phy, self.ip4_transit(ttl=5), self.egress)
        self.host.assert_nothing_captured()


class TestSonicExtCoppIp2me(SonicExtCoppDataplaneBase):
    """sonic-ext CoPP: IPv4/IPv6 punt-path (ip2me/BGP) policing"""

    def test_ip2me_addr_policed(self):
        """IPv4 to a registered local address is policed"""
        pol = self.policer()
        self.add_ip2me_addr(self.phy.local_ip4)
        self.bind_ip2me(pol)

        rx = self.assert_policed(self.ip4_to_me(), pol, self.ip2me_counters)
        for p in rx:
            self.assertEqual(p[IP].dst, self.phy.local_ip4)

    def test_ip2me_unregistered_addr_passes(self):
        """Local IPv4 to an address not in the IP2ME set passes unpoliced"""
        pol = self.policer(conform=DROP)
        self.bind_ip2me(pol)

        self.assert_unpoliced(self.ip4_to_me(), pol, self.ip2me_counters)

    def test_ip2me_unresolved_policer_drops(self):
        """A bound but missing IP2ME policer drops until it is created"""
        pol = self.policer(add=False)
        self.add_ip2me_addr(self.phy.local_ip4)
        self.bind_ip2me(pol)

        self.assert_dropped(self.ip4_to_me(), IP2ME_ERR + UNRESOLVED)

        pol.add_vpp_config()
        self.addCleanup(self._remove_policer, pol)
        self.assert_policed(self.ip4_to_me(), pol, self.ip2me_counters)

    def test_ip2me_address_slots_are_reused(self):
        """More than 256 add/delete cycles do not exhaust the address table"""
        addr = int(ipaddress.IPv4Address(self.phy.local_ip4))
        for _ in range(257):
            self.vapi.sonic_ext_copp_ip2me_addr_add_del(addr=addr, is_add=True)
            self.vapi.sonic_ext_copp_ip2me_addr_add_del(addr=addr, is_add=False)

        pol = self.policer(conform=DROP, cb=10**6)
        self.add_ip2me_addr(self.phy.local_ip4)
        self.bind_ip2me(pol)
        before = self.native(pol)
        self.assert_dropped(self.ip4_to_me())
        self.assertEqual(self.delta(self.native(pol), before)["conform"], self.N_PKTS)

    def test_ip2me_policer_slots_are_reused(self):
        """Repeated bind churn does not exhaust the eight policer slots"""
        for i in range(9):
            name = "copp-gone-%d" % i
            self.vapi.sonic_ext_copp_ip2me_bind(policer_name=name, is_bind=True)
            self.vapi.sonic_ext_copp_ip2me_bind(policer_name=name, is_bind=False)

        pol = self.policer(conform=DROP, cb=10**6)
        self.add_ip2me_addr(self.phy.local_ip4)
        self.bind_ip2me(pol)
        before = self.native(pol)
        self.assert_dropped(self.ip4_to_me())
        self.assertEqual(self.delta(self.native(pol), before)["conform"], self.N_PKTS)

    def test_bgp_port_wins_over_addr(self):
        """BGP (TCP/179 either direction) uses its own slot even to an
        IP2ME address; other TCP to that address uses the IP2ME slot"""
        ip2me_pol = self.policer(conform=DROP, cb=10**6)
        bgp_pol = self.policer(conform=TRANSMIT, cb=10**6)
        self.add_ip2me_addr(self.phy.local_ip4)
        self.bind_ip2me(ip2me_pol)
        self.bind_ip2me_port(bgp_pol)

        for l4 in (TCP(sport=40000, dport=BGP_PORT), TCP(sport=BGP_PORT, dport=40000)):
            before = self.native(bgp_pol)
            self.send_and_expect(self.phy, self.ip4_to_me(l4=l4), self.host)
            self.assertEqual(
                self.delta(self.native(bgp_pol), before)["conform"], self.N_PKTS
            )

        before = self.native(ip2me_pol)
        self.assert_dropped(self.ip4_to_me(l4=TCP(sport=40000, dport=22)))
        self.assertEqual(
            self.delta(self.native(ip2me_pol), before)["conform"], self.N_PKTS
        )

    def test_unbinding_bgp_preserves_ip2me_slot(self):
        """Removing the port slot leaves the independent address slot intact"""
        ip2me_pol = self.policer(conform=DROP, cb=10**6)
        bgp_pol = self.policer(conform=TRANSMIT, cb=10**6)
        self.add_ip2me_addr(self.phy.local_ip4)
        self.bind_ip2me(ip2me_pol)
        self.bind_ip2me_port(bgp_pol)

        self.vapi.sonic_ext_copp_ip2me_bind_condition(
            policer_name=bgp_pol.name, tcp_port=BGP_PORT, is_bind=False
        )
        ip2me_before = self.native(ip2me_pol)
        bgp_before = self.native(bgp_pol)

        self.assert_dropped(self.ip4_to_me(l4=TCP(sport=40000, dport=BGP_PORT)))

        self.assertEqual(
            self.delta(self.native(ip2me_pol), ip2me_before)["conform"],
            self.N_PKTS,
        )
        self.assertEqual(self.native(bgp_pol), bgp_before)

    def test_same_policer_name_keeps_bgp_and_ip2me_slots_independent(self):
        """The API operation, not only the name, identifies each slot."""
        pol = self.policer(conform=DROP, cb=10**6, name="shared-copp")
        self.add_ip2me_addr(self.phy.local_ip4)
        self.bind_ip2me(pol)
        self.bind_ip2me_port(pol)

        self.vapi.sonic_ext_copp_ip2me_bind_condition(
            policer_name=pol.name, tcp_port=BGP_PORT, is_bind=False
        )
        before = self.native(pol)
        self.assert_dropped(self.ip4_to_me(l4=TCP(sport=40000, dport=22)))
        self.assertEqual(
            self.delta(self.native(pol), before)["conform"], self.N_PKTS
        )

    def test_rebinding_bgp_replaces_the_previous_owner(self):
        """One semantic BGP slot cannot retain a superseded policer name."""
        old_pol = self.policer(conform=DROP, cb=10**6, name="old-bgp")
        new_pol = self.policer(conform=TRANSMIT, cb=10**6, name="new-bgp")
        self.bind_ip2me_port(old_pol)
        self.bind_ip2me_port(new_pol)

        old_before = self.native(old_pol)
        new_before = self.native(new_pol)
        self.send_and_expect(
            self.phy,
            self.ip4_to_me(l4=TCP(sport=40000, dport=BGP_PORT)),
            self.host,
        )
        self.assertEqual(self.native(old_pol), old_before)
        self.assertEqual(
            self.delta(self.native(new_pol), new_before)["conform"], self.N_PKTS
        )

    def test_bgp_port_found_after_ip_options(self):
        """The TCP port is located from the IHL, not a fixed offset"""
        pol = self.policer(conform=DROP, cb=10**6)
        self.bind_ip2me_port(pol)

        # A non-IGMP packet with options is sent by ip4-options straight to
        # ip4-punt, so this also covers that entry into the punt arc.
        pkts = self.ip4_to_me(
            l4=TCP(sport=40000, dport=BGP_PORT), options=[IPOption_Router_Alert()]
        )
        self.assertGreater(Ether(bytes(pkts[0]))[IP].ihl, 5)
        before = self.native(pol)
        self.assert_dropped(pkts)
        self.assertEqual(self.delta(self.native(pol), before)["conform"], self.N_PKTS)

    def test_bgp6_policed(self):
        """BGPv6 (TCP/179) to a local IPv6 address is policed"""
        pol = self.policer()
        self.bind_ip2me_port(pol)

        rx = self.assert_policed(
            self.ip6_to_me(TCP(sport=40000, dport=BGP_PORT)), pol, self.ip2me_counters
        )
        for p in rx:
            self.assertEqual(p[IPv6].dst, self.phy.local_ip6)

    def test_non_bgp6_passes(self):
        """Other IPv6 punted TCP is not policed by the BGP slot"""
        pol = self.policer(conform=DROP)
        self.bind_ip2me_port(pol)

        self.assert_unpoliced(
            self.ip6_to_me(TCP(sport=40000, dport=22)), pol, self.ip2me_counters
        )

    def test_bgp6_unresolved_policer_drops(self):
        """A bound but missing BGPv6 policer drops"""
        pol = self.policer(add=False)
        self.bind_ip2me_port(pol)

        self.assert_dropped(
            self.ip6_to_me(TCP(sport=40000, dport=BGP_PORT)), IP2ME6_ERR + UNRESOLVED
        )

    def test_pps_policer_meters_fixed_size(self):
        """A PPS policer charges every packet as 256 bytes, a KBPS policer
        charges the real length"""
        pkts = self.ip4_to_me(n=80, size=1000)
        ip_len = len(pkts[0][IP])
        self.assertNotEqual(ip_len, 256)
        self.add_ip2me_addr(self.phy.local_ip4)

        # Realistic CoPP rates.  Very low PPS rates (single digits) make the
        # policer pick scale 31, where VPP's (len << scale) overflows u32 and
        # every packet conforms -- an upstream policer limit, avoided here.
        # PPS takes cir in packets/s and cb in ms.
        for rate_type, cir, cb, metered in (
            (RATE.SSE2_QOS_RATE_API_PPS, 100, 100, 256),
            (RATE.SSE2_QOS_RATE_API_KBPS, 205, 2560, ip_len),
        ):
            pol = self.policer(cir=cir, cb=cb, rate_type=rate_type)
            self.bind_ip2me(pol)

            before = pol.get_stats()
            rx = self.send_and_capture_host(pkts)
            after = pol.get_stats()
            conform = after["conform_packets"] - before["conform_packets"]
            metered_bytes = sum(
                after["%s_bytes" % c] - before["%s_bytes" % c] for c in COLORS
            )

            msg = "rate_type %d" % rate_type
            # The length each packet was charged: 256 for PPS, real for KBPS.
            self.assertEqual(metered_bytes, len(pkts) * metered, msg)
            self.assertEqual(len(rx), conform, msg)
            self.assertGreater(conform, 0, msg)
            self.assertLess(conform, len(pkts), msg)

            self.vapi.sonic_ext_copp_ip2me_bind(policer_name=pol.name, is_bind=False)


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
