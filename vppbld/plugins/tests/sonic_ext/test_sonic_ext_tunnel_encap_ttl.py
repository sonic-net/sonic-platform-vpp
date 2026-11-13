#!/usr/bin/env python3
"""sonic-ext tunnel-encap-ttl (SAI pipe TTL model) tests

A stock IPIP tunnel builds its outer header with TTL / hop limit 64, and the
underlay ip4-rewrite decrements an IPv4 outer TTL to 63.
sonic_ext_tunnel_encap_ttl_set gives a tunnel a fixed outer TTL / hop limit,
which the underlay leaves alone.

pg0 is the underlay of an IPv4 and an IPv6 transport tunnel. Packets come in
on pg1 and are routed into a tunnel.
"""

import unittest

from framework import VppTestCase
from asfframework import VppTestRunner
from vpp_ip_route import VppIpRoute, VppRoutePath, FibPathProto
from vpp_ipip_tun_interface import VppIpIpTunInterface
from vpp_neighbor import VppNeighbor

from scapy.layers.l2 import Ether
from scapy.layers.inet import IP, UDP
from scapy.layers.inet6 import IPv6

N_PKTS = 3
INNER_TTL = 34

# Destinations routed via the IPv4 transport tunnel (4o4, 6o4) and via the
# IPv6 transport tunnel (4o6).
DST4_VIA_TUN4 = "130.67.0.1"
DST6_VIA_TUN4 = "dead:0::1"
DST4_VIA_TUN6 = "130.67.1.1"


class SonicExtTunnelEncapTtlBase(VppTestCase):
    """sonic-ext tunnel-encap-ttl test base"""

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
    ]

    @classmethod
    def setUpClass(cls):
        super(SonicExtTunnelEncapTtlBase, cls).setUpClass()
        cls.create_pg_interfaces(range(2))

    def setUp(self):
        super(SonicExtTunnelEncapTtlBase, self).setUp()
        # Static neighbors for the remote hosts: sonic-ext nd-punt, on by
        # default, hands neighbor discovery to the host tap, which the tests
        # don't have.
        for i in self.pg_interfaces:
            i.admin_up()
            i.config_ip4()
            i.config_ip6()
            i.disable_ipv6_ra()
            for remote_ip in (i.remote_ip4, i.remote_ip6):
                VppNeighbor(
                    self, i.sw_if_index, i.remote_mac, remote_ip, is_static=True
                ).add_vpp_config()

        self.tun4 = self.add_tunnel(self.pg0.local_ip4, self.pg0.remote_ip4)
        self.tun6 = self.add_tunnel(self.pg0.local_ip6, self.pg0.remote_ip6)
        self.add_route(DST4_VIA_TUN4, 32, self.tun4, FibPathProto.FIB_PATH_NH_PROTO_IP4)
        self.add_route(
            DST6_VIA_TUN4, 128, self.tun4, FibPathProto.FIB_PATH_NH_PROTO_IP6
        )
        self.add_route(DST4_VIA_TUN6, 32, self.tun6, FibPathProto.FIB_PATH_NH_PROTO_IP4)

    def tearDown(self):
        # The base tearDown removes the tunnels and routes added in setUp, which
        # are in the test object registry.
        super(SonicExtTunnelEncapTtlBase, self).tearDown()
        for i in self.pg_interfaces:
            i.unconfig_ip4()
            i.unconfig_ip6()
            i.admin_down()

    # -- helpers -------------------------------------------------------------

    def add_tunnel(self, src, dst):
        tun = VppIpIpTunInterface(self, self.pg0, src, dst)
        tun.add_vpp_config()
        tun.admin_up()
        self.vapi.sw_interface_set_unnumbered(
            sw_if_index=self.pg0.sw_if_index, unnumbered_sw_if_index=tun.sw_if_index
        )
        return tun

    def add_route(self, prefix, length, tun, proto):
        nh = "::" if proto == FibPathProto.FIB_PATH_NH_PROTO_IP6 else "0.0.0.0"
        route = VppIpRoute(
            self, prefix, length, [VppRoutePath(nh, tun.sw_if_index, proto=proto)]
        )
        route.add_vpp_config()
        return route

    def feature_enabled(self):
        return self.vapi.sonic_ext_feature_get(feature="tunnel-encap-ttl").enabled

    def set_ttl(self, sw_if_index, ttl):
        return self.vapi.sonic_ext_tunnel_encap_ttl_set(
            sw_if_index=sw_if_index, ttl=ttl
        )

    def pkt4(self, dst):
        return (
            Ether(dst=self.pg1.local_mac, src=self.pg1.remote_mac)
            / IP(src=self.pg1.remote_ip4, dst=dst, ttl=INNER_TTL)
            / UDP(sport=1234, dport=4321)
            / (b"\xa5" * 64)
        )

    def pkt6(self, dst):
        return (
            Ether(dst=self.pg1.local_mac, src=self.pg1.remote_mac)
            / IPv6(src=self.pg1.remote_ip6, dst=dst, hlim=INNER_TTL)
            / UDP(sport=1234, dport=4321)
            / (b"\xa5" * 64)
        )

    def send_4o4(self):
        """Outer TTLs of IPv4 packets encapsulated by tun4"""
        rx = self.send_and_expect(
            self.pg1, N_PKTS * [self.pkt4(DST4_VIA_TUN4)], self.pg0
        )
        for p in rx:
            self.assert_packet_checksums_valid(p)
            self.assertEqual(p[IP].dst, self.pg0.remote_ip4)
            self.assertEqual(p[IP].payload[IP].ttl, INNER_TTL - 1)
        return {p[IP].ttl for p in rx}

    def send_6o4(self):
        """Outer TTLs of IPv6 packets encapsulated by tun4"""
        rx = self.send_and_expect(
            self.pg1, N_PKTS * [self.pkt6(DST6_VIA_TUN4)], self.pg0
        )
        for p in rx:
            self.assert_packet_checksums_valid(p)
            self.assertEqual(p[IP].dst, self.pg0.remote_ip4)
            self.assertEqual(p[IPv6].hlim, INNER_TTL - 1)
        return {p[IP].ttl for p in rx}

    def send_4o6(self):
        """Outer hop limits of IPv4 packets encapsulated by tun6"""
        rx = self.send_and_expect(
            self.pg1, N_PKTS * [self.pkt4(DST4_VIA_TUN6)], self.pg0
        )
        for p in rx:
            self.assert_packet_checksums_valid(p)
            self.assertEqual(p[IPv6].dst, self.pg0.remote_ip6)
            self.assertEqual(p[IPv6].payload[IP].ttl, INNER_TTL - 1)
        return {p[IPv6].hlim for p in rx}

    def stamped(self, af):
        return self.statistics.get_err_counter(
            "/err/sonic-ext-%s-tunnel-encap-ttl/outer TTL set" % af
        )

    def not_encap(self, af):
        return self.statistics.get_err_counter(
            "/err/sonic-ext-%s-tunnel-encap-ttl/not tunnel encapsulated, left unchanged"
            % af
        )


class TestSonicExtTunnelEncapTtl(SonicExtTunnelEncapTtlBase):
    """sonic-ext tunnel-encap-ttl"""

    def test_feature_default_enabled(self):
        """tunnel-encap-ttl is a SAIVPP-owned feature, on by default"""
        self.assertTrue(self.feature_enabled())

    def test_stock_ttl(self):
        """without a TTL the tunnels keep the stock outer TTL"""
        self.assertEqual(self.send_4o4(), {63})
        self.assertEqual(self.send_6o4(), {63})
        self.assertEqual(self.send_4o6(), {64})

    def test_ttl_ip4_transport(self):
        """a TTL on an IPv4 transport tunnel is set, undecremented"""
        self.set_ttl(self.tun4.sw_if_index, 255)
        ip4_before, ip6_before = self.stamped("ip4"), self.stamped("ip6")

        self.assertEqual(self.send_4o4(), {255})
        self.assertEqual(self.send_6o4(), {255})

        # Inner IPv4 packets go through the ip4-output arc, inner IPv6 ones
        # through ip6-output.
        self.assertEqual(self.stamped("ip4"), ip4_before + N_PKTS)
        self.assertEqual(self.stamped("ip6"), ip6_before + N_PKTS)

        # The other tunnel keeps its stock hop limit
        self.assertEqual(self.send_4o6(), {64})

    def test_ttl_ip6_transport(self):
        """a TTL on an IPv6 transport tunnel sets the outer hop limit"""
        self.set_ttl(self.tun6.sw_if_index, 200)
        self.assertEqual(self.send_4o6(), {200})
        self.assertEqual(self.send_4o4(), {63})

    def test_update_and_remove(self):
        """a new TTL replaces the old one, and TTL 0 restores the stock one"""
        self.set_ttl(self.tun4.sw_if_index, 255)
        self.set_ttl(self.tun4.sw_if_index, 255)
        self.set_ttl(self.tun4.sw_if_index, 128)
        self.assertEqual(self.send_4o4(), {128})

        # A single 0 removes it, after several non-zero sets
        self.set_ttl(self.tun4.sw_if_index, 0)
        before = self.stamped("ip4")
        self.assertEqual(self.send_4o4(), {63})
        self.assertEqual(self.stamped("ip4"), before)

        # Removing it again, or from a tunnel without one, is a no-op
        self.set_ttl(self.tun4.sw_if_index, 0)
        self.set_ttl(self.tun6.sw_if_index, 0)
        self.assertEqual(self.send_4o4(), {63})

        self.set_ttl(self.tun4.sw_if_index, 255)
        self.assertEqual(self.send_4o4(), {255})

    def test_not_tunnel_encapsulated(self):
        """on an interface that does not encapsulate, packets are unchanged"""
        self.set_ttl(self.pg0.sw_if_index, 255)
        self.addCleanup(self.set_ttl, self.pg0.sw_if_index, 0)
        before = self.not_encap("ip4")

        pkt = (
            Ether(dst=self.pg1.local_mac, src=self.pg1.remote_mac)
            / IP(src=self.pg1.remote_ip4, dst=self.pg0.remote_ip4, ttl=INNER_TTL)
            / UDP(sport=1234, dport=4321)
            / (b"\xa5" * 64)
        )
        rx = self.send_and_expect(self.pg1, N_PKTS * [pkt], self.pg0)
        for p in rx:
            self.assertEqual(p[IP].ttl, INNER_TTL - 1)
            self.assert_packet_checksums_valid(p)
        self.assertEqual(self.not_encap("ip4"), before + N_PKTS)

    def test_invalid_sw_if_index(self):
        """a TTL for an interface that does not exist is rejected"""
        with self.assertRaises(Exception):
            self.set_ttl(0xFFFF, 255)

    def test_deleted_tunnel(self):
        """a tunnel that reuses a deleted one's sw_if_index has no TTL"""
        tun = self.add_tunnel(self.pg0.local_ip4, self.pg1.remote_ip4)
        sw_if_index = tun.sw_if_index
        self.set_ttl(sw_if_index, 255)
        tun.remove_vpp_config()

        tun = self.add_tunnel(self.pg0.local_ip4, self.pg1.remote_ip4)
        self.assertEqual(tun.sw_if_index, sw_if_index)
        route = self.add_route(
            "130.67.2.1", 32, tun, FibPathProto.FIB_PATH_NH_PROTO_IP4
        )

        rx = self.send_and_expect(
            self.pg1, N_PKTS * [self.pkt4("130.67.2.1")], self.pg1
        )
        self.assertEqual({p[IP].ttl for p in rx}, {63})

        route.remove_vpp_config()


class TestSonicExtTunnelEncapTtlStartupOff(SonicExtTunnelEncapTtlBase):
    """sonic-ext tunnel-encap-ttl off in startup.conf"""

    extra_vpp_config = ["sonic-ext", "{", "tunnel-encap-ttl", "off", "}"]

    def test_startup_off(self):
        """tunnel-encap-ttl off at startup leaves the stock outer TTL"""
        self.assertFalse(self.feature_enabled())
        self.set_ttl(self.tun4.sw_if_index, 255)
        self.assertEqual(self.send_4o4(), {63})


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
