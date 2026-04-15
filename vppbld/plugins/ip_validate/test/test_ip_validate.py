#!/usr/bin/env python3
"""
IP Packet Validation Plugin Tests

Tests for ip4-validate and ip6-validate VPP nodes.
These nodes drop packets with invalid source/destination addresses
and mismatched L2/L3 headers.
"""

import unittest
from framework import VppTestCase
from asfframework import VppTestRunner
from vpp_sub_interface import VppDot1QSubint

from scapy.layers.l2 import Ether, Dot1Q
from scapy.layers.inet import IP, UDP
from scapy.layers.inet6 import IPv6
from scapy.packet import Raw


class TestIpValidate(VppTestCase):
    """IP Packet Validation Test Case"""

    @classmethod
    def setUpClass(cls):
        super(TestIpValidate, cls).setUpClass()
        try:
            cls.create_pg_interfaces(range(2))
            for pg in cls.pg_interfaces:
                pg.admin_up()
                pg.config_ip4()
                pg.config_ip6()
                pg.resolve_arp()
                pg.resolve_ndp()

            # Enable ip-validate features on pg interfaces via API
            for pg in cls.pg_interfaces:
                cls.vapi.ip_validate_enable_disable(
                    sw_if_index=pg.sw_if_index, is_enable=True
                )
        except Exception:
            cls.tearDownClass()
            raise

    @classmethod
    def tearDownClass(cls):
        # Disable features before teardown
        for pg in cls.pg_interfaces:
            cls.vapi.ip_validate_enable_disable(
                sw_if_index=pg.sw_if_index, is_enable=False
            )
        super(TestIpValidate, cls).tearDownClass()

    def setUp(self):
        super(TestIpValidate, self).setUp()
        for pg in self.pg_interfaces:
            pg.enable_capture()

    def tearDown(self):
        super(TestIpValidate, self).tearDown()

    def show_commands_at_teardown(self):
        self.logger.info(self.vapi.cli("show interface"))
        self.logger.info(self.vapi.cli("show node counters"))

    def create_ipv4_packet(self, src_ip, dst_ip, src_mac=None, dst_mac=None):
        """Create an IPv4/UDP test packet."""
        pg0 = self.pg_interfaces[0]
        pg1 = self.pg_interfaces[1]
        if src_mac is None:
            src_mac = pg0.remote_mac
        if dst_mac is None:
            dst_mac = pg0.local_mac
        if dst_ip is None:
            dst_ip = pg1.remote_ip4
        return (
            Ether(src=src_mac, dst=dst_mac)
            / IP(src=src_ip, dst=dst_ip)
            / UDP(sport=10000, dport=20000)
            / Raw(b"\xa5" * 100)
        )

    def create_ipv6_packet(self, src_ip, dst_ip):
        """Create an IPv6/UDP test packet."""
        pg0 = self.pg_interfaces[0]
        pg1 = self.pg_interfaces[1]
        if dst_ip is None:
            dst_ip = pg1.remote_ip6
        return (
            Ether(src=pg0.remote_mac, dst=pg0.local_mac)
            / IPv6(src=src_ip, dst=dst_ip)
            / UDP(sport=10000, dport=20000)
            / Raw(b"\xa5" * 100)
        )

    def send_and_assert_dropped(self, pkt, error_counter_name=None):
        """Send a packet on pg0 and verify it is dropped (nothing on pg1)."""
        pg0 = self.pg_interfaces[0]
        pg1 = self.pg_interfaces[1]

        if error_counter_name:
            counter_before = self.statistics.get_err_counter(
                error_counter_name
            )

        pg0.add_stream([pkt])
        pg1.enable_capture()
        self.pg_start()

        pg1.assert_nothing_captured(remark="Packet should have been dropped")

        if error_counter_name:
            counter_after = self.statistics.get_err_counter(
                error_counter_name
            )
            self.assertGreater(
                counter_after,
                counter_before,
                "Error counter %s did not increment" % error_counter_name,
            )

    def send_and_assert_forwarded(self, pkt):
        """Send a packet on pg0 and verify it arrives on pg1."""
        pg0 = self.pg_interfaces[0]
        pg1 = self.pg_interfaces[1]

        pg0.add_stream([pkt])
        pg1.enable_capture()
        self.pg_start()

        capture = pg1.get_capture(1)
        self.assertEqual(len(capture), 1)

    # ================================================================
    # IPv4 L2 check tests
    # ================================================================

    def test_unicast_ip_incorrect_eth_dst_multicast(self):
        """IPv4: unicast IP dst with multicast ETH dst (01:00:5e) -> drop"""
        pg1 = self.pg_interfaces[1]
        pkt = self.create_ipv4_packet(
            src_ip="10.0.0.1",
            dst_ip=pg1.remote_ip4,
            dst_mac="01:00:5e:00:00:01",
        )
        self.send_and_assert_dropped(
            pkt, "/err/ip4-validate/unicast IP with multicast~broadcast L2 destination"
        )

    def test_unicast_ip_incorrect_eth_dst_broadcast(self):
        """IPv4: unicast IP dst with broadcast ETH dst (ff:ff:ff) -> drop"""
        pg1 = self.pg_interfaces[1]
        pkt = self.create_ipv4_packet(
            src_ip="10.0.0.1",
            dst_ip=pg1.remote_ip4,
            dst_mac="ff:ff:ff:ff:ff:ff",
        )
        self.send_and_assert_dropped(
            pkt, "/err/ip4-validate/unicast IP with multicast~broadcast L2 destination"
        )

    # ================================================================
    # IPv4 SRC address check tests
    # ================================================================

    def test_src_ip_is_loopback_addr(self):
        """IPv4: SRC=127.0.0.1 -> drop"""
        pkt = self.create_ipv4_packet(src_ip="127.0.0.1", dst_ip=None)
        self.send_and_assert_dropped(
            pkt, "/err/ip4-validate/source address is loopback"
        )

    def test_src_ip_is_multicast_addr_ipv4(self):
        """IPv4: SRC=224.1.1.1 -> drop"""
        pkt = self.create_ipv4_packet(src_ip="224.1.1.1", dst_ip=None)
        self.send_and_assert_dropped(
            pkt, "/err/ip4-validate/source address is multicast"
        )

    def test_src_ip_is_class_e(self):
        """IPv4: SRC=240.1.1.1 -> drop"""
        pkt = self.create_ipv4_packet(src_ip="240.1.1.1", dst_ip=None)
        self.send_and_assert_dropped(
            pkt, "/err/ip4-validate/source address is class E"
        )

    def test_ip_is_zero_addr_ipv4_src(self):
        """IPv4: SRC=0.0.0.0 -> drop"""
        pkt = self.create_ipv4_packet(src_ip="0.0.0.0", dst_ip=None)
        self.send_and_assert_dropped(
            pkt, "/err/ip4-validate/source address is unspecified"
        )

    def test_src_ip_link_local(self):
        """IPv4: SRC=169.254.1.1 -> drop"""
        pkt = self.create_ipv4_packet(src_ip="169.254.1.1", dst_ip=None)
        self.send_and_assert_dropped(
            pkt, "/err/ip4-validate/source address is link-local"
        )

    # ================================================================
    # IPv4 DST address check tests
    # ================================================================

    def test_dst_ip_is_loopback_addr(self):
        """IPv4: DST=127.0.0.1 -> drop"""
        pkt = self.create_ipv4_packet(src_ip="10.0.0.1", dst_ip="127.0.0.1")
        self.send_and_assert_dropped(
            pkt, "/err/ip4-validate/destination address is loopback"
        )

    def test_dst_ip_link_local(self):
        """IPv4: DST=169.254.1.1 -> drop"""
        pkt = self.create_ipv4_packet(
            src_ip="10.0.0.1", dst_ip="169.254.1.1"
        )
        self.send_and_assert_dropped(
            pkt, "/err/ip4-validate/destination address is link-local"
        )

    # ================================================================
    # IPv4 valid packet (positive control)
    # ================================================================

    def test_valid_ipv4_packet(self):
        """IPv4: valid SRC and DST -> forwarded"""
        pkt = self.create_ipv4_packet(src_ip="10.0.0.1", dst_ip=None)
        self.send_and_assert_forwarded(pkt)

    def test_valid_ipv4_feature_arc_passthrough(self):
        """IPv4: valid packets traverse node and increment VALID counter"""
        pg0 = self.pg_interfaces[0]
        pg1 = self.pg_interfaces[1]
        counter_path = "/err/ip4-validate/valid packets"
        counter_before = self.statistics.get_err_counter(counter_path)

        pkts = [
            self.create_ipv4_packet(src_ip="10.0.0.%d" % (i + 1), dst_ip=None)
            for i in range(10)
        ]
        pg0.add_stream(pkts)
        pg1.enable_capture()
        self.pg_start()

        capture = pg1.get_capture(10)
        self.assertEqual(len(capture), 10, "All 10 valid packets should be forwarded")

        counter_after = self.statistics.get_err_counter(counter_path)
        self.assertGreaterEqual(
            counter_after - counter_before,
            10,
            "VALID counter should increment by at least 10",
        )

    # ================================================================
    # IPv6 L2 check tests
    # ================================================================

    def test_unicast_ipv6_incorrect_eth_dst_multicast(self):
        """IPv6: unicast IP dst with multicast ETH dst -> drop"""
        pg1 = self.pg_interfaces[1]
        pg0 = self.pg_interfaces[0]
        pkt = (
            Ether(src=pg0.remote_mac, dst="33:33:00:00:00:01")
            / IPv6(src="2001:db8::1", dst=pg1.remote_ip6)
            / UDP(sport=10000, dport=20000)
            / Raw(b"\xa5" * 100)
        )
        self.send_and_assert_dropped(
            pkt, "/err/ip6-validate/unicast IP with multicast~broadcast L2 destination"
        )

    def test_unicast_ipv6_incorrect_eth_dst_broadcast(self):
        """IPv6: unicast IP dst with broadcast ETH dst -> drop"""
        pg1 = self.pg_interfaces[1]
        pg0 = self.pg_interfaces[0]
        pkt = (
            Ether(src=pg0.remote_mac, dst="ff:ff:ff:ff:ff:ff")
            / IPv6(src="2001:db8::1", dst=pg1.remote_ip6)
            / UDP(sport=10000, dport=20000)
            / Raw(b"\xa5" * 100)
        )
        self.send_and_assert_dropped(
            pkt, "/err/ip6-validate/unicast IP with multicast~broadcast L2 destination"
        )

    # ================================================================
    # IPv6 SRC address check tests
    # ================================================================

    def test_src_ip_is_multicast_addr_ipv6(self):
        """IPv6: SRC=ff02::1 -> drop"""
        pkt = self.create_ipv6_packet(src_ip="ff02::1", dst_ip=None)
        self.send_and_assert_dropped(
            pkt, "/err/ip6-validate/source address is multicast"
        )

    def test_ip_is_zero_addr_ipv6_src(self):
        """IPv6: SRC=:: -> drop"""
        pkt = self.create_ipv6_packet(src_ip="::", dst_ip=None)
        self.send_and_assert_dropped(
            pkt, "/err/ip6-validate/source address is unspecified"
        )

    def test_src_ip_is_loopback_addr_ipv6(self):
        """IPv6: SRC=::1 -> drop"""
        pkt = self.create_ipv6_packet(src_ip="::1", dst_ip=None)
        self.send_and_assert_dropped(
            pkt, "/err/ip6-validate/source address is loopback"
        )

    # ================================================================
    # IPv6 DST address check tests
    # ================================================================

    def test_ip_is_zero_addr_ipv6_dst(self):
        """IPv6: DST=:: -> drop"""
        pkt = self.create_ipv6_packet(src_ip="2001:db8::1", dst_ip="::")
        self.send_and_assert_dropped(
            pkt, "/err/ip6-validate/destination address is unspecified"
        )

    def test_dst_ip_is_loopback_addr_ipv6(self):
        """IPv6: DST=::1 -> drop"""
        pkt = self.create_ipv6_packet(src_ip="2001:db8::1", dst_ip="::1")
        self.send_and_assert_dropped(
            pkt, "/err/ip6-validate/destination address is loopback"
        )

    # ================================================================
    # IPv6 valid packet (positive control)
    # ================================================================

    def test_valid_ipv6_packet(self):
        """IPv6: valid SRC and DST -> forwarded"""
        pkt = self.create_ipv6_packet(src_ip="2001:db8::1", dst_ip=None)
        self.send_and_assert_forwarded(pkt)

    def test_valid_ipv6_feature_arc_passthrough(self):
        """IPv6: valid packets traverse node and increment VALID counter"""
        pg0 = self.pg_interfaces[0]
        pg1 = self.pg_interfaces[1]
        counter_path = "/err/ip6-validate/valid packets"
        counter_before = self.statistics.get_err_counter(counter_path)

        pkts = [
            self.create_ipv6_packet(src_ip="2001:db8::%d" % (i + 1), dst_ip=None)
            for i in range(10)
        ]
        pg0.add_stream(pkts)
        pg1.enable_capture()
        self.pg_start()

        capture = pg1.get_capture(10)
        self.assertEqual(len(capture), 10, "All 10 valid packets should be forwarded")

        counter_after = self.statistics.get_err_counter(counter_path)
        self.assertGreaterEqual(
            counter_after - counter_before,
            10,
            "VALID counter should increment by at least 10",
        )

    # ================================================================
    # VLAN-tagged traffic tests (validates ethernet_buffer_get_header fix)
    # ================================================================

    def test_vlan_tagged_mcast_eth_drop(self):
        """IPv4: VLAN-tagged unicast IP with multicast ETH dst -> drop"""
        pg0 = self.pg_interfaces[0]
        pg1 = self.pg_interfaces[1]

        # Create VLAN 100 sub-interface on pg0.
        # The VNET_SW_INTERFACE_ADD_DEL_FUNCTION callback auto-enables
        # the feature on the sub-interface.
        sub = VppDot1QSubint(self, pg0, 100)
        sub.admin_up()
        sub.config_ip4()
        sub.resolve_arp()

        counter_before = self.statistics.get_err_counter(
            "/err/ip4-validate/unicast IP with multicast~broadcast L2 destination"
        )

        pkt = (
            Ether(src=pg0.remote_mac, dst="01:00:5e:00:00:01")
            / Dot1Q(vlan=100)
            / IP(src="10.0.0.1", dst=pg1.remote_ip4)
            / UDP(sport=10000, dport=20000)
            / Raw(b"\xa5" * 100)
        )

        pg0.add_stream([pkt])
        pg1.enable_capture()
        self.pg_start()

        pg1.assert_nothing_captured(
            remark="VLAN-tagged packet with mcast ETH dst should be dropped"
        )

        counter_after = self.statistics.get_err_counter(
            "/err/ip4-validate/unicast IP with multicast~broadcast L2 destination"
        )
        self.assertGreater(
            counter_after,
            counter_before,
            "L2_MCAST_BCAST counter should increment for VLAN-tagged packet",
        )

        sub.unconfig_ip4()
        sub.remove_vpp_config()

    def test_vlan_tagged_valid_forwarded(self):
        """IPv4: VLAN-tagged valid packet -> forwarded"""
        pg0 = self.pg_interfaces[0]
        pg1 = self.pg_interfaces[1]

        sub = VppDot1QSubint(self, pg0, 200)
        sub.admin_up()
        sub.config_ip4()
        sub.resolve_arp()

        pkt = (
            Ether(src=pg0.remote_mac, dst=pg0.local_mac)
            / Dot1Q(vlan=200)
            / IP(src=sub.remote_ip4, dst=pg1.remote_ip4)
            / UDP(sport=10000, dport=20000)
            / Raw(b"\xa5" * 100)
        )

        pg0.add_stream([pkt])
        pg1.enable_capture()
        self.pg_start()

        capture = pg1.get_capture(1)
        self.assertEqual(len(capture), 1, "Valid VLAN-tagged packet should be forwarded")

        sub.unconfig_ip4()
        sub.remove_vpp_config()

    # ================================================================
    # Feature disable/re-enable tests
    # ================================================================

    def test_disable_allows_invalid_ipv4(self):
        """IPv4: after disabling feature, invalid packets are forwarded"""
        pg0 = self.pg_interfaces[0]

        # The feature has refcount=2 on pg0 (auto-enable + explicit API
        # enable in setUpClass). Disable twice to fully remove.
        for _ in range(2):
            self.vapi.ip_validate_enable_disable(
                sw_if_index=pg0.sw_if_index, is_enable=False
            )

        # Loopback-source packet should now be forwarded
        pkt = self.create_ipv4_packet(src_ip="127.0.0.1", dst_ip=None)
        self.send_and_assert_forwarded(pkt)

        # Re-enable twice to restore original refcount
        for _ in range(2):
            self.vapi.ip_validate_enable_disable(
                sw_if_index=pg0.sw_if_index, is_enable=True
            )

        # Loopback-source packet should be dropped again
        pkt = self.create_ipv4_packet(src_ip="127.0.0.1", dst_ip=None)
        self.send_and_assert_dropped(
            pkt, "/err/ip4-validate/source address is loopback"
        )

    def test_disable_allows_invalid_ipv6(self):
        """IPv6: after disabling feature, invalid packets are forwarded"""
        pg0 = self.pg_interfaces[0]

        for _ in range(2):
            self.vapi.ip_validate_enable_disable(
                sw_if_index=pg0.sw_if_index, is_enable=False
            )

        # Multicast-source packet should now be forwarded
        pkt = self.create_ipv6_packet(src_ip="ff02::1", dst_ip=None)
        self.send_and_assert_forwarded(pkt)

        for _ in range(2):
            self.vapi.ip_validate_enable_disable(
                sw_if_index=pg0.sw_if_index, is_enable=True
            )

        pkt = self.create_ipv6_packet(src_ip="ff02::1", dst_ip=None)
        self.send_and_assert_dropped(
            pkt, "/err/ip6-validate/source address is multicast"
        )

    # ================================================================
    # Mixed batch tests (valid + invalid in one stream)
    # ================================================================

    def test_mixed_batch_ipv4(self):
        """IPv4: mixed valid and invalid packets in one stream"""
        pg0 = self.pg_interfaces[0]
        pg1 = self.pg_interfaces[1]

        valid_pkts = [
            self.create_ipv4_packet(src_ip="10.0.0.%d" % (i + 1), dst_ip=None)
            for i in range(5)
        ]
        invalid_pkts = [
            self.create_ipv4_packet(src_ip="127.0.0.1", dst_ip=None),
            self.create_ipv4_packet(src_ip="224.1.1.1", dst_ip=None),
            self.create_ipv4_packet(src_ip="0.0.0.0", dst_ip=None),
        ]

        # Interleave: valid, invalid, valid, invalid, ...
        mixed = [
            valid_pkts[0], invalid_pkts[0],
            valid_pkts[1], invalid_pkts[1],
            valid_pkts[2], invalid_pkts[2],
            valid_pkts[3], valid_pkts[4],
        ]

        pg0.add_stream(mixed)
        pg1.enable_capture()
        self.pg_start()

        capture = pg1.get_capture(5)
        self.assertEqual(
            len(capture), 5,
            "Only the 5 valid packets should be forwarded",
        )

    def test_mixed_batch_ipv6(self):
        """IPv6: mixed valid and invalid packets in one stream"""
        pg0 = self.pg_interfaces[0]
        pg1 = self.pg_interfaces[1]

        valid_pkts = [
            self.create_ipv6_packet(src_ip="2001:db8::%d" % (i + 1), dst_ip=None)
            for i in range(5)
        ]
        invalid_pkts = [
            self.create_ipv6_packet(src_ip="ff02::1", dst_ip=None),
            self.create_ipv6_packet(src_ip="::", dst_ip=None),
            self.create_ipv6_packet(src_ip="2001:db8::99", dst_ip="::"),
        ]

        mixed = [
            valid_pkts[0], invalid_pkts[0],
            valid_pkts[1], invalid_pkts[1],
            valid_pkts[2], invalid_pkts[2],
            valid_pkts[3], valid_pkts[4],
        ]

        pg0.add_stream(mixed)
        pg1.enable_capture()
        self.pg_start()

        capture = pg1.get_capture(5)
        self.assertEqual(
            len(capture), 5,
            "Only the 5 valid packets should be forwarded",
        )


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
