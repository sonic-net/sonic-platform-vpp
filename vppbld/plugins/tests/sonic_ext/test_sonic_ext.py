#!/usr/bin/env python3
"""sonic-ext plugin tests"""

import unittest
from socket import AF_INET6, inet_ntop, inet_pton

from framework import VppTestCase
from asfframework import VppTestRunner
from vpp_ip import VppIpPuntRedirect
from vpp_neighbor import VppNeighbor, find_nbr
from vpp_papi import VppEnum
from vpp_papi_provider import CliFailedCommandError

from scapy.layers.l2 import Ether
from scapy.layers.inet6 import (
    IPv6,
    ICMPv6ND_NA,
    ICMPv6ND_NS,
    ICMPv6ND_RA,
    ICMPv6ND_RS,
    ICMPv6NDOptDstLLAddr,
    ICMPv6NDOptSrcLLAddr,
)
from scapy.utils6 import in6_getnsma, in6_getnsmac


class SonicExtNdPuntBase(VppTestCase):
    """sonic-ext nd-punt test base"""

    # Both plugins are .default_disabled, and sonic-ext's nodes name
    # linux-cp's, so sonic-ext cannot load without it.
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
        super(SonicExtNdPuntBase, cls).setUpClass()
        cls.create_pg_interfaces(range(2))

    def setUp(self):
        super(SonicExtNdPuntBase, self).setUp()
        for i in self.pg_interfaces:
            i.admin_up()
            i.config_ip6()
        self.vapi.ip_neighbor_flush(
            af=VppEnum.vl_api_address_family_t.ADDRESS_IP6,
            sw_if_index=self.pg0.sw_if_index,
        )

        # pg1 stands in for pg0's linux-cp host tap: linux-cp redirects each
        # port's punted IPv6 to its tap the same way.  The neighbor is static
        # because with nd-punt VPP cannot resolve one itself.
        VppNeighbor(
            self,
            self.pg1.sw_if_index,
            self.pg1.remote_mac,
            self.pg1.remote_ip6,
            is_static=True,
        ).add_vpp_config()
        VppIpPuntRedirect(
            self, self.pg0.sw_if_index, self.pg1.sw_if_index, self.pg1.remote_ip6
        ).add_vpp_config()

    def tearDown(self):
        super(SonicExtNdPuntBase, self).tearDown()
        for i in self.pg_interfaces:
            i.unconfig_ip6()
            i.admin_down()

    def nd_punt_enabled(self):
        return self.vapi.sonic_ext_feature_get(feature="nd-punt").enabled

    def remote_learned(self):
        return find_nbr(self, self.pg0.sw_if_index, self.pg0.remote_ip6)

    def ns(self, multicast=False):
        """NS from pg0's remote host for pg0's address"""
        if multicast:
            nsma = in6_getnsma(inet_pton(AF_INET6, self.pg0.local_ip6))
            dmac, dst = in6_getnsmac(nsma), inet_ntop(AF_INET6, nsma)
        else:
            dmac, dst = self.pg0.local_mac, self.pg0.local_ip6
        return (
            Ether(dst=dmac, src=self.pg0.remote_mac)
            / IPv6(src=self.pg0.remote_ip6, dst=dst, hlim=255)
            / ICMPv6ND_NS(tgt=self.pg0.local_ip6)
            / ICMPv6NDOptSrcLLAddr(lladdr=self.pg0.remote_mac)
        )

    def assert_vpp_answers_ns(self):
        rx = self.send_and_expect_only(self.pg0, [self.ns()], self.pg0)
        self.assertEqual(rx[0][ICMPv6ND_NA].tgt, self.pg0.local_ip6)
        self.assertTrue(self.remote_learned())


class TestSonicExtNdPunt(SonicExtNdPuntBase):
    """sonic-ext nd-punt"""

    def test_nd_punted(self):
        """RS/RA/NS/NA go to the host tap; VPP neither answers nor learns"""
        self.assertTrue(self.nd_punt_enabled())

        remote = self.pg0.remote_ip6
        pkts = [
            self.ns(),
            self.ns(multicast=True),
            Ether(dst=self.pg0.local_mac, src=self.pg0.remote_mac)
            / IPv6(src=remote, dst=self.pg0.local_ip6, hlim=255)
            / ICMPv6ND_NA(tgt=remote, R=0, S=1, O=1)
            / ICMPv6NDOptDstLLAddr(lladdr=self.pg0.remote_mac),
            Ether(dst="33:33:00:00:00:02", src=self.pg0.remote_mac)
            / IPv6(src=remote, dst="ff02::2", hlim=255)
            / ICMPv6ND_RS()
            / ICMPv6NDOptSrcLLAddr(lladdr=self.pg0.remote_mac),
            # Unicast: the default capture filter drops multicast RAs.
            Ether(dst=self.pg0.local_mac, src=self.pg0.remote_mac)
            / IPv6(src=self.pg0.remote_ip6_ll, dst=self.pg0.local_ip6, hlim=255)
            / ICMPv6ND_RA()
            / ICMPv6NDOptSrcLLAddr(lladdr=self.pg0.remote_mac),
        ]

        rx = self.send_and_expect_only(self.pg0, pkts, self.pg1)
        self.assertEqual(
            sorted(p[IPv6].payload.type for p in rx),
            sorted(p[IPv6].payload.type for p in pkts),
        )
        for p in rx:
            # Linux drops ND whose hop limit is not 255.
            self.assertEqual(p[IPv6].hlim, 255)
        self.assertFalse(self.remote_learned())

    def test_nd_punt_toggle(self):
        """nd-punt off hands ND back to VPP; on takes it away again"""
        self.vapi.cli("sonic-ext nd-punt off")
        try:
            self.assertFalse(self.nd_punt_enabled())
            self.assert_vpp_answers_ns()
        finally:
            self.vapi.cli("sonic-ext nd-punt on")

        self.vapi.ip_neighbor_flush(
            af=VppEnum.vl_api_address_family_t.ADDRESS_IP6,
            sw_if_index=self.pg0.sw_if_index,
        )
        self.assertTrue(self.nd_punt_enabled())
        self.send_and_expect_only(self.pg0, [self.ns()], self.pg1)
        self.assertFalse(self.remote_learned())

    def test_nd_punt_cli_bad_input(self):
        """sonic-ext nd-punt rejects an unknown word and names it"""
        with self.assertRaisesRegex(CliFailedCommandError, "unknown input `bogus'"):
            self.vapi.cli("sonic-ext nd-punt bogus")
        self.assertTrue(self.nd_punt_enabled())


class TestSonicExtNdPuntStartupOff(SonicExtNdPuntBase):
    """sonic-ext nd-punt off in startup.conf"""

    extra_vpp_config = ["sonic-ext", "{", "nd-punt", "off", "}"]

    def test_nd_punt_startup_off(self):
        """nd-punt off at startup leaves ND to VPP"""
        self.assertFalse(self.nd_punt_enabled())
        self.assert_vpp_answers_ns()


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
