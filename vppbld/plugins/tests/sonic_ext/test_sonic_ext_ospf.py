#!/usr/bin/env python3
"""OSPFv2/OSPFv3 punt-to-linux tests for sonic-ext."""

import unittest

from asfframework import VppTestRunner
from config import config
from framework import VppTestCase
from scapy.layers.inet import IP
from scapy.layers.inet6 import IPv6
from scapy.layers.l2 import Ether
from scapy.packet import Raw

OSPF_PROTOCOL = 89


@unittest.skipIf("linux-cp" in config.excluded_plugins, "Exclude linux-cp tests")
@unittest.skipIf(
    "sonic_ext" in config.excluded_plugins, "Exclude sonic-ext plugin tests"
)
class TestSonicExtOspfPunt(VppTestCase):
    """Protocol 89 reaches the normal LCP host path without CoPP."""

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
        "plugin",
        "sonic_ext_plugin.so",
        "{",
        "enable",
        "}",
    ]
    extra_vpp_config = ["sonic-ext", "{", "copp", "off", "}"]

    @classmethod
    def setUpClass(cls):
        super(TestSonicExtOspfPunt, cls).setUpClass()
        cls.create_pg_interfaces(range(2))

    def setUp(self):
        super(TestSonicExtOspfPunt, self).setUp()
        self.phy, self.host = self.pg0, self.pg1
        self.phy.admin_up()
        self.host.admin_up()
        self.phy.config_ip4()
        self.phy.config_ip6()
        self.host.config_ip6()
        self.vapi.cli("test lcp add phy %s host %s" % (self.phy, self.host))

    def tearDown(self):
        self.vapi.cli("test lcp del phy %s host %s" % (self.phy, self.host))
        self.host.unconfig_ip6()
        self.phy.unconfig_ip6()
        self.phy.unconfig_ip4()
        self.host.admin_down()
        self.phy.admin_down()
        super(TestSonicExtOspfPunt, self).tearDown()

    def test_ospfv2_multicast_punts_to_host(self):
        """224.0.0.5 protocol 89 is redirected before MFIB lookup."""
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
        """ff02::5 next-header 89 is redirected before MFIB lookup."""
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


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
