#!/usr/bin/env python3
"""OSPF and FRR BFD delivery through the existing linux-cp paths."""

import unittest

from asfframework import VppTestRunner
from config import config
from framework import VppTestCase
from scapy.layers.inet import IP, UDP
from scapy.layers.inet6 import IPv6
from scapy.layers.l2 import Ether
from scapy.packet import Raw
from vpp_ip_route import FibPathProto, FibPathType, VppIpMRoute, VppMRoutePath
from vpp_papi import VppEnum

OSPF_PROTOCOL = 89
BFD_PORTS = (3784, 3785)


@unittest.skipIf("linux-cp" in config.excluded_plugins, "Exclude linux-cp tests")
class TestLinuxCpOspfBfdPunt(VppTestCase):
    """OSPF and local FRR BFD reach Linux through stock LCP delivery."""

    vpp_worker_count = 0
    extra_vpp_plugin_config = [
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
    ]

    @classmethod
    def setUpClass(cls):
        super(TestLinuxCpOspfBfdPunt, cls).setUpClass()
        cls.create_pg_interfaces(range(2))

    def setUp(self):
        super(TestLinuxCpOspfBfdPunt, self).setUp()
        self.phy, self.host = self.pg0, self.pg1
        self.phy.admin_up()
        self.host.admin_up()
        self.phy.config_ip4()
        self.phy.config_ip6()
        self.host.config_ip6()
        self.vapi.cli("test lcp add phy %s host %s" % (self.phy, self.host))
        self._install_linux_cp_multicast_routes()

    def _install_linux_cp_multicast_routes(self):
        """Reproduce routes installed by linux-cp Netlink address events.

        The unittest LCP-pair helper installs the pair and punt redirects but
        does not generate Linux address events. Production lcp_router.c uses
        those events to add an ingress ACCEPT path and a table-local path for
        224.0.0.0/24 and ff00::/8.
        """
        entry_flags = VppEnum.vl_api_mfib_entry_flags_t
        itf_flags = VppEnum.vl_api_mfib_itf_flags_t

        VppIpMRoute(
            self,
            "0.0.0.0",
            "224.0.0.0",
            24,
            entry_flags.MFIB_API_ENTRY_FLAG_NONE,
            [
                VppMRoutePath(
                    self.phy.sw_if_index,
                    itf_flags.MFIB_API_ITF_FLAG_ACCEPT,
                ),
                VppMRoutePath(
                    0xFFFFFFFF,
                    itf_flags.MFIB_API_ITF_FLAG_FORWARD,
                    type=FibPathType.FIB_PATH_TYPE_LOCAL,
                ),
            ],
        ).add_vpp_config()

        VppIpMRoute(
            self,
            "::",
            "ff00::",
            8,
            entry_flags.MFIB_API_ENTRY_FLAG_NONE,
            [
                VppMRoutePath(
                    self.phy.sw_if_index,
                    itf_flags.MFIB_API_ITF_FLAG_ACCEPT,
                    proto=FibPathProto.FIB_PATH_NH_PROTO_IP6,
                ),
                VppMRoutePath(
                    0xFFFFFFFF,
                    itf_flags.MFIB_API_ITF_FLAG_FORWARD,
                    proto=FibPathProto.FIB_PATH_NH_PROTO_IP6,
                    type=FibPathType.FIB_PATH_TYPE_LOCAL,
                ),
            ],
        ).add_vpp_config()

    def tearDown(self):
        self.vapi.cli("test lcp del phy %s host %s" % (self.phy, self.host))
        self.host.unconfig_ip6()
        self.phy.unconfig_ip6()
        self.phy.unconfig_ip4()
        self.host.admin_down()
        self.phy.admin_down()
        super(TestLinuxCpOspfBfdPunt, self).tearDown()

    def test_ospfv2_multicast_punts_to_host(self):
        """linux-cp's local 224/24 route delivers OSPFv2 to Linux."""
        packet = (
            Ether(src=self.phy.remote_mac, dst="01:00:5e:00:00:05")
            / IP(
                src=self.phy.remote_ip4,
                dst="224.0.0.5",
                proto=OSPF_PROTOCOL,
                ttl=1,
            )
            / Raw(b"\xa5" * 64)
        )
        self.send_and_expect(self.phy, [packet], self.host)

    def test_ospfv3_multicast_punts_to_host(self):
        """linux-cp's local ff00::/8 route delivers OSPFv3 to Linux."""
        packet = (
            Ether(src=self.phy.remote_mac, dst="33:33:00:00:00:05")
            / IPv6(
                src=self.phy.remote_ip6,
                dst="ff02::5",
                nh=OSPF_PROTOCOL,
                hlim=1,
            )
            / Raw(b"\xa5" * 64)
        )
        self.send_and_expect(self.phy, [packet], self.host)

    def test_bfd_ipv4_unknown_udp_punts_to_host(self):
        """Local IPv4 BFD control and echo use linux-cp unknown-UDP punt."""
        packets = [
            Ether(src=self.phy.remote_mac, dst=self.phy.local_mac)
            / IP(src=self.phy.remote_ip4, dst=self.phy.local_ip4, ttl=255)
            / UDP(sport=49152, dport=port)
            / Raw(b"\x20\x40\x03\x18" + b"\x00" * 20)
            for port in BFD_PORTS
        ]
        self.send_and_expect(self.phy, packets, self.host)

    def test_bfd_ipv6_unknown_udp_punts_to_host(self):
        """Local IPv6 BFD control and echo use linux-cp unknown-UDP punt."""
        packets = [
            Ether(src=self.phy.remote_mac, dst=self.phy.local_mac)
            / IPv6(src=self.phy.remote_ip6, dst=self.phy.local_ip6, hlim=255)
            / UDP(sport=49152, dport=port)
            / Raw(b"\x20\x40\x03\x18" + b"\x00" * 20)
            for port in BFD_PORTS
        ]
        self.send_and_expect(self.phy, packets, self.host)


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
