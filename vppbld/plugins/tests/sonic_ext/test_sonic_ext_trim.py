#!/usr/bin/env python3
"""sonic-ext packet-trim configuration, lifecycle, and dataplane tests."""

import unittest

from framework import VppTestCase
from asfframework import VppTestRunner
from vpp_ip_route import VppIpMRoute, VppMRoutePath
from vpp_neighbor import VppNeighbor
from vpp_papi import VppEnum
from vpp_papi_provider import CliFailedCommandError
from vpp_sub_interface import VppDot1QSubint

from scapy.layers.inet import IP, UDP
from scapy.layers.l2 import Dot1Q, Ether
from scapy.packet import Raw


class SonicExtTrimBase(VppTestCase):
    """sonic-ext packet-trim feature-toggle test base"""

    # sonic-ext's nodes name linux-cp's, so sonic-ext cannot load without it;
    # both plugins are default-disabled and must be enabled explicitly.
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

    def trim_feature_enabled(self):
        return bool(self.vapi.sonic_ext_feature_get(feature="trim").enabled)


class TestSonicExtTrim(SonicExtTrimBase):
    """sonic-ext packet-trim feature registry"""

    def test_trim_registered_default_on(self):
        """trim is a known sonic-ext feature and defaults enabled

        Without the foreach_sonic_ext_feature registration, feature_get would
        return the unknown-feature default (disabled). Since the admission node
        gates every packet on this toggle, it must be registered and default on
        so SAI-driven trimming still activates with no startup stanza.
        """
        self.assertTrue(self.trim_feature_enabled())


class TestSonicExtTrimStartupOff(SonicExtTrimBase):
    """sonic-ext trim off in startup.conf"""

    extra_vpp_config = ["sonic-ext", "{", "trim", "off", "}"]

    def test_trim_startup_off(self):
        """trim off at startup clears the toggle the admission node reads"""
        self.assertFalse(self.trim_feature_enabled())


class SonicExtTrimDataplaneBase(SonicExtTrimBase):
    """Three routed ports, with queue 0 blocked and trim-eligible on pg1."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.create_pg_interfaces(range(3))

    def setUp(self):
        super().setUp()
        for interface in self.pg_interfaces:
            interface.admin_up()
            interface.config_ip4()
            self.vapi.sw_interface_set_mtu(
                sw_if_index=interface.sw_if_index, mtu=[9216, 0, 0, 0]
            )
            VppNeighbor(
                self,
                interface.sw_if_index,
                interface.remote_mac,
                interface.remote_ip4,
                is_static=True,
            ).add_vpp_config()
        self.set_global()
        self.vapi.sonic_ext_trim_dscp_map_set(dscp_to_queue=bytes(64))
        self.set_queue(queue=6, eligible=False, rate=1_000_000, capacity=1_000_000)
        self.set_queue()

    def tearDown(self):
        self.set_queue(eligible=False)
        self.set_queue(queue=6, eligible=False)
        self.vapi.sonic_ext_trim_global_set(is_enable=False)
        super().tearDown()
        for interface in self.pg_interfaces:
            interface.unconfig_ip4()
            interface.admin_down()

    def set_global(self, size=256, queue=6):
        return self.vapi.sonic_ext_trim_global_set(
            is_enable=True,
            trim_size=size,
            dscp_mode=0,
            dscp_value=48,
            tc_value=0,
            trim_queue=queue,
        )

    def set_queue(self, sw_if_index=None, queue=0, eligible=True, rate=0, capacity=0):
        if sw_if_index is None:
            sw_if_index = self.pg1.sw_if_index
        return self.vapi.sonic_ext_trim_queue_set(
            sw_if_index=sw_if_index,
            queue=queue,
            eligible=eligible,
            rate_bytes_per_sec=rate,
            capacity_bytes=capacity,
        )

    def packet(self, size=512, dst=None):
        packet = (
            Ether(src=self.pg0.remote_mac, dst=self.pg0.local_mac)
            / IP(src=self.pg0.remote_ip4, dst=dst or self.pg1.remote_ip4, tos=40)
            / UDP(sport=1234, dport=4321)
        )
        return packet / Raw(b"x" * (size - len(packet)))

    def assert_trimmed(self, packet, size=256):
        self.assertEqual(len(packet), size)
        self.assertEqual(packet[IP].tos >> 2, 48)


class TestSonicExtTrimDataplane(SonicExtTrimDataplaneBase):
    """sonic-ext trim dataplane regressions"""

    def test_interface_index_reuse(self):
        """a reused interface index has clean buckets and can enable trim"""
        original = self.vapi.create_loopback().sw_if_index
        try:
            self.set_queue(sw_if_index=original)
        finally:
            self.vapi.delete_loopback(sw_if_index=original)

        replacement = self.vapi.create_loopback().sw_if_index
        try:
            self.assertEqual(replacement, original)
            name = self.vapi.sw_interface_dump(sw_if_index=replacement)[
                0
            ].interface_name
            self.assertNotIn(f"port {name}:", self.vapi.cli("show sonic-ext trim"))
            self.set_queue(sw_if_index=replacement)
            self.assertIn(
                "sonic-ext-trim-admission",
                self.vapi.cli(f"show interface features {name}"),
            )
        finally:
            self.vapi.delete_loopback(sw_if_index=replacement)

    def test_invalid_dscp_queue_map(self):
        """unsupported queues reject the entire map without changing it"""
        for queue in (8, 9):
            with self.subTest(queue=queue):
                mapping = [1] * 64
                mapping[-1] = queue
                with self.vapi.assert_negative_api_retval():
                    self.vapi.sonic_ext_trim_dscp_map_set(dscp_to_queue=bytes(mapping))
                rx = self.send_and_expect(self.pg0, [self.packet()], self.pg1)
                self.assert_trimmed(rx[0])

    def test_invalid_trim_queue(self):
        """invalid API and CLI trim queues preserve the previous policy"""
        for queue in (8, 9):
            with self.subTest(queue=queue):
                with self.vapi.assert_negative_api_retval():
                    self.set_global(size=512, queue=queue)
                with self.assertRaises(CliFailedCommandError):
                    self.vapi.cli(
                        f"sonic-ext trim global size 512 dscp 12 queue {queue}"
                    )
                rx = self.send_and_expect(self.pg0, [self.packet()], self.pg1)
                self.assert_trimmed(rx[0])

    def test_vlan_egress(self):
        """VLAN egress uses the physical port for both admission buckets"""
        subif = VppDot1QSubint(self, self.pg1, 100)
        self.addCleanup(subif.remove_vpp_config)
        subif.admin_up()
        subif.config_ip4()
        try:
            VppNeighbor(
                self,
                subif.sw_if_index,
                subif.remote_mac,
                subif.remote_ip4,
                is_static=True,
            ).add_vpp_config()
            packet = self.packet(dst=subif.remote_ip4)
            rx = self.send_and_expect(self.pg0, [packet], self.pg1)
            self.assert_trimmed(rx[0])
            self.assertEqual(rx[0][Dot1Q].vlan, 100)

            self.set_queue(queue=6, eligible=False)
            self.send_and_assert_no_replies(self.pg0, [packet])
        finally:
            subif.unconfig_ip4()
            subif.admin_down()

    def test_jumbo_chain(self):
        """trimming retains the requested prefix across buffer segments"""
        self.set_global(size=3000)
        rx = self.send_and_expect(self.pg0, [self.packet(size=5000)], self.pg1)
        self.assert_trimmed(rx[0], size=3000)

    def test_shared_multicast_chain(self):
        """trimming a replicated jumbo never shortens its untrimmed sibling"""
        # Keep the sibling in the output graph until trimming runs; otherwise
        # pg2 can serialize it and release its shared references first.
        for feature in ("sonic-ext-trim-admission", "sonic-ext-egress-mirror"):
            self.vapi.feature_enable_disable(
                sw_if_index=self.pg2.sw_if_index,
                arc_name="interface-output",
                feature_name=feature,
                enable=True,
            )
            self.addCleanup(
                self.vapi.feature_enable_disable,
                sw_if_index=self.pg2.sw_if_index,
                arc_name="interface-output",
                feature_name=feature,
                enable=False,
            )
        itf_flags = VppEnum.vl_api_mfib_itf_flags_t
        entry_flags = VppEnum.vl_api_mfib_entry_flags_t
        group = "232.1.1.1"
        VppIpMRoute(
            self,
            "0.0.0.0",
            group,
            32,
            entry_flags.MFIB_API_ENTRY_FLAG_NONE,
            [
                VppMRoutePath(self.pg0.sw_if_index, itf_flags.MFIB_API_ITF_FLAG_ACCEPT),
                VppMRoutePath(
                    self.pg1.sw_if_index, itf_flags.MFIB_API_ITF_FLAG_FORWARD
                ),
                VppMRoutePath(
                    self.pg2.sw_if_index, itf_flags.MFIB_API_ITF_FLAG_FORWARD
                ),
            ],
        ).add_vpp_config()
        packet = self.packet(size=5000, dst=group)
        for size in (384, 3000):
            with self.subTest(trim_size=size):
                self.set_global(size=size)
                self.pg0.add_stream([packet] * 3)
                self.pg_enable_capture([self.pg1, self.pg2])
                self.pg_start()
                trimmed = self.pg1.get_capture(3)
                untrimmed = self.pg2.get_capture(3)
                for copy in trimmed:
                    self.assert_trimmed(copy, size=size)
                for copy in untrimmed:
                    self.assertEqual(len(copy), len(packet))
                    self.assertEqual(copy[IP].tos, packet[IP].tos)
                    self.assertEqual(bytes(copy[Raw]), bytes(packet[Raw]))


class TestSonicExtTrimDataplaneStartupOff(SonicExtTrimDataplaneBase):
    """sonic-ext trim override with a fully programmed admission policy"""

    extra_vpp_config = ["sonic-ext", "{", "trim", "off", "}"]

    def test_startup_off_bypasses_policy(self):
        """operator force-off bypasses even a blocked eligible queue"""
        before = self.vapi.sonic_ext_trim_counters_get()
        packet = self.packet()
        rx = self.send_and_expect(self.pg0, [packet], self.pg1)
        self.assertEqual(len(rx[0]), len(packet))
        self.assertEqual(rx[0][IP].tos, packet[IP].tos)
        after = self.vapi.sonic_ext_trim_counters_get()
        self.assertEqual(after.trim_admit_fail, before.trim_admit_fail)
        self.assertEqual(after.trim_sent, before.trim_sent)


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
