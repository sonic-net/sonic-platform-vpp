#!/usr/bin/env python3
"""sonic-ext packet-trim configuration, lifecycle, and dataplane tests."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from config import config
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
        "plugin",
        "bufmon_plugin.so",
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


class TestSonicExtTrimSideband(SonicExtTrimBase):
    """Production sideband accessors with deterministic buffer reuse."""

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.accessor_tempdir = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.accessor_tempdir.cleanup)
        cls.binary = str(Path(cls.accessor_tempdir.name) / "sideband-accessors")
        install_dir = Path(config.vpp_install_dir) / "vpp"
        vppinfra = next(install_dir.glob("lib*/**/libvppinfra.so"), None)
        if vppinfra is None:
            raise FileNotFoundError(f"libvppinfra.so not found under {install_dir}")
        subprocess.check_call(
            shlex.split(os.environ.get("CC", "cc"))
            + [
                "-O2",
                "-Wall",
                "-Werror",
                "-I" + str(Path(config.vpp_ws_dir) / "src" / "plugins"),
                "-I" + str(install_dir / "include"),
                str(Path(__file__).with_name("test_sonic_ext_vnet_buf.c")),
                "-o",
                cls.binary,
                "-Wl,-rpath," + str(vppinfra.parent),
                str(vppinfra),
            ]
        )

    def test_first_claim_clears_dirty_slot(self):
        """A new incarnation cannot retain stale validity or payload."""
        subprocess.check_call([self.binary, "first-claim"])

    def test_repeated_claim_and_release(self):
        """Repeated claims preserve the PBH hash; release hides the field."""
        subprocess.check_call([self.binary, "repeated-claim"])

    def test_unclaimed_and_clone_slots_hide_stale_fields(self):
        """Unclaimed buffers and fresh clone slots expose no source fields."""
        subprocess.check_call([self.binary, "unclaimed-clone"])


class TestSonicExtTrimStartupOff(SonicExtTrimBase):
    """sonic-ext trim off in startup.conf"""

    extra_vpp_config = ["sonic-ext", "{", "trim", "off", "}"]

    def test_trim_startup_off(self):
        """trim off at startup clears the toggle the admission node reads"""
        self.assertFalse(self.trim_feature_enabled())


class TestSonicExtTrimConfig(SonicExtTrimBase):
    """Global configuration validation and disable semantics."""

    def setUp(self):
        super().setUp()
        self.policy = dict(
            is_enable=True,
            trim_size=256,
            dscp_mode=0,
            dscp_value=48,
            tc_value=0,
            trim_queue=6,
        )
        self.addCleanup(self.vapi.sonic_ext_trim_global_set, is_enable=False)
        self.vapi.sonic_ext_trim_global_set(**self.policy)

    def test_cli_global_validation_is_atomic(self):
        """Reject narrowing, invalid modes, and explicit all-ones inputs."""
        before = self.vapi.cli("show sonic-ext trim")
        for invalid in (
            "size 65536",
            "size 4294967295",
            "dscp 64",
            "dscp 4294967295",
            "tc 256",
            "tc 4294967295",
            "queue 4294967295",
            "mode from-tc",
        ):
            with self.subTest(invalid=invalid):
                with self.assertRaises(CliFailedCommandError):
                    self.vapi.cli(
                        "sonic-ext trim global size 512 dscp 12 tc 3 queue 5 " + invalid
                    )
                self.assertEqual(before, self.vapi.cli("show sonic-ext trim"))

    def test_api_global_validation_is_atomic(self):
        """Invalid DSCP values and unsupported modes preserve every field."""
        before = self.vapi.cli("show sonic-ext trim")
        for field, values in (("dscp_value", (64, 255)), ("dscp_mode", (1, 255))):
            for value in values:
                with self.subTest(field=field, value=value):
                    candidate = dict(
                        self.policy,
                        trim_size=512,
                        dscp_value=12,
                        tc_value=3,
                        trim_queue=5,
                    )
                    candidate[field] = value
                    with self.vapi.assert_negative_api_retval():
                        self.vapi.sonic_ext_trim_global_set(**candidate)
                    self.assertEqual(before, self.vapi.cli("show sonic-ext trim"))

    def test_limits_partial_update_and_disable(self):
        """Boundary values survive partial updates; disable ignores fields."""
        self.vapi.cli(
            "sonic-ext trim global size 65535 dscp 63 tc 255 queue 7 mode symmetric"
        )
        self.vapi.cli("sonic-ext trim global dscp 0")
        state = self.vapi.cli("show sonic-ext trim")
        for label, value in (
            ("trim size", 65535),
            ("dscp value", 0),
            ("trim tc", 255),
            ("trim queue", 7),
        ):
            self.assertRegex(state, rf"{label}\s*:\s*{value}\b")
        self.vapi.cli(
            "sonic-ext trim global disable size 4294967295 dscp 4294967295 "
            "tc 4294967295 queue 4294967295 mode from-tc"
        )
        self.assertRegex(self.vapi.cli("show sonic-ext trim"), r"configured\s*:\s*no")
        self.vapi.sonic_ext_trim_global_set(
            is_enable=True,
            trim_size=65535,
            dscp_mode=0,
            dscp_value=63,
            tc_value=255,
            trim_queue=7,
        )
        self.vapi.sonic_ext_trim_global_set(
            is_enable=False,
            trim_size=65535,
            dscp_mode=255,
            dscp_value=255,
            tc_value=255,
            trim_queue=255,
        )
        state = self.vapi.cli("show sonic-ext trim")
        self.assertRegex(state, r"configured\s*:\s*no")
        self.assertRegex(state, r"trim size\s*:\s*0\b")
        self.assertRegex(state, r"dscp value\s*:\s*63\b")


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

    def test_deferred_egress_mirror(self):
        """Downstream mirroring runs once after trimming or policy bypass."""
        acl = self.vapi.acl_add_replace(
            acl_index=0xFFFFFFFF,
            tag="trim-egress-mirror",
            count=1,
            r=[
                dict(
                    is_permit=3,
                    src_prefix="0.0.0.0/0",
                    dst_prefix="0.0.0.0/0",
                    proto=17,
                    srcport_or_icmptype_first=0,
                    srcport_or_icmptype_last=65535,
                    dstport_or_icmpcode_first=0,
                    dstport_or_icmpcode_last=65535,
                    in_sw_if_index=self.pg0.sw_if_index,
                    mirror_action=(1 << 28) | self.pg2.sw_if_index,
                )
            ],
        ).acl_index
        self.addCleanup(self.vapi.acl_del, acl_index=acl)
        self.vapi.acl_interface_set_acl_list(
            sw_if_index=self.pg0.sw_if_index, count=1, n_input=1, acls=[acl]
        )
        self.addCleanup(
            self.vapi.acl_interface_set_acl_list,
            sw_if_index=self.pg0.sw_if_index,
            count=0,
            n_input=0,
            acls=[],
        )
        self.vapi.feature_enable_disable(
            sw_if_index=self.pg1.sw_if_index,
            arc_name="interface-output",
            feature_name="sonic-ext-egress-mirror",
            enable=True,
        )
        self.addCleanup(
            self.vapi.feature_enable_disable,
            sw_if_index=self.pg1.sw_if_index,
            arc_name="interface-output",
            feature_name="sonic-ext-egress-mirror",
            enable=False,
        )

        for count in (8, 9, 64):
            for enabled in (True, False, True):
                with self.subTest(packet_count=count, trim_enabled=enabled):
                    if enabled:
                        self.set_global()
                    else:
                        self.vapi.sonic_ext_trim_global_set(is_enable=False)
                    before = self.vapi.sonic_ext_trim_counters_get()
                    self.pg0.add_stream([self.packet()] * count)
                    self.pg_enable_capture([self.pg1, self.pg2])
                    self.pg_start()
                    forwarded = self.pg1.get_capture(count)
                    mirrored = self.pg2.get_capture(count)
                    for packet, copy in zip(forwarded, mirrored):
                        if enabled:
                            self.assert_trimmed(packet)
                        else:
                            self.assertEqual(len(packet), 512)
                            self.assertEqual(packet[IP].tos, 40)
                        self.assertEqual(bytes(copy), bytes(packet))
                    after = self.vapi.sonic_ext_trim_counters_get()
                    expected = count if enabled else 0
                    self.assertEqual(
                        after.trim_admit_fail - before.trim_admit_fail, expected
                    )
                    self.assertEqual(after.trim_sent - before.trim_sent, expected)

    def test_single_pass_trace_and_counters(self):
        """One admission trace carries the trim outcome and both counters."""
        counter = "/err/sonic-ext-trim-admission/trimmed copies transmitted"
        before = self.statistics.get_err_counter(counter)
        self.vapi.cli("clear trace")
        self.vapi.cli("trace add pg-input 1")
        self.assert_trimmed(
            self.send_and_expect(self.pg0, [self.packet()], self.pg1)[0]
        )
        trace = self.vapi.cli("show trace")
        self.assertEqual(trace.count("SONIC-EXT-TRIM-ADMISSION:"), 1)
        self.assertIn("trim-queue 6 size 256 dscp 48 SENT", trace)
        self.assertEqual(self.statistics.get_err_counter(counter), before + 1)

    def test_drop_counter_is_attributed_once(self):
        """A blocked trim queue counts each dropped packet exactly once."""
        self.set_queue(queue=6, eligible=False)
        counter = (
            "/err/sonic-ext-trim-admission/"
            "trimmed copies dropped (trim queue congested)"
        )
        for count in (1, 9, 64):
            with self.subTest(packet_count=count):
                before = self.vapi.sonic_ext_trim_counters_get()
                before_error = self.statistics.get_err_counter(counter)
                self.send_and_assert_no_replies(self.pg0, [self.packet()] * count)
                after = self.vapi.sonic_ext_trim_counters_get()
                self.assertEqual(after.trim_admit_fail - before.trim_admit_fail, count)
                self.assertEqual(after.trim_drop - before.trim_drop, count)
                self.assertEqual(after.trim_sent, before.trim_sent)
                self.assertEqual(
                    self.statistics.get_err_counter(counter) - before_error,
                    count,
                )

    def test_trim_independent_of_sideband_callback(self):
        """Trimming can enable, forward and delete while bufmon owns the hook."""
        self.set_queue(eligible=False)
        self.vapi.cli("set buffer traces on")
        try:
            self.set_queue()
            self.assertIn(
                "sonic-ext-trim-admission",
                self.vapi.cli(f"show interface features {self.pg1.name}"),
            )
            interface = self.vapi.create_loopback().sw_if_index
            try:
                self.set_queue(sw_if_index=interface)
            finally:
                self.vapi.delete_loopback(sw_if_index=interface)
            self.assert_trimmed(
                self.send_and_expect(self.pg0, [self.packet()], self.pg1)[0]
            )
        finally:
            self.vapi.cli("set buffer traces off")

    def test_pbh_sideband_independent_of_trim(self):
        """Only PBH retains the sideband across trim enable/disable."""
        profile = self.vapi.sonic_ext_pbh_profile_add_del(
            is_add=True,
            profile_index=0xFFFFFFFF,
            n_fields=1,
            fields=[dict(field=1, sequence_id=1, mask=bytes(16))],
        ).profile_index
        table = None
        try:
            table = self.vapi.sonic_ext_pbh_table_add_replace(
                table_index=0xFFFFFFFF,
                name="trim-sideband",
                n_rules=1,
                rules=[
                    dict(
                        rule_id=1,
                        qualifiers=(1 << 1) | (1 << 3),
                        ip_protocol=17,
                        l4_dst_port=13330,
                        ecmp_profile=0xFFFFFFFF,
                        lag_profile=profile,
                    )
                ],
            ).table_index
            self.set_queue(eligible=False)
            self.set_queue()
            with self.assertRaises(CliFailedCommandError):
                self.vapi.cli("set buffer traces on")
            self.vapi.sonic_ext_pbh_table_del(table_index=table)
            table = None
            self.assert_trimmed(
                self.send_and_expect(self.pg0, [self.packet()], self.pg1)[0]
            )
        finally:
            if table is not None:
                self.vapi.sonic_ext_pbh_table_del(table_index=table)
            self.vapi.sonic_ext_pbh_profile_add_del(
                is_add=False, profile_index=profile, n_fields=0, fields=[]
            )
        self.vapi.cli("set buffer traces on")
        self.vapi.cli("set buffer traces off")

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
        """Private trimmed chains retain the cursor for downstream features."""
        # Keep the sibling in the output graph until trimming runs; otherwise
        # pg2 can serialize it and release its shared references first.
        for interface, feature in (
            (self.pg1, "sonic-ext-egress-mirror"),
            (self.pg2, "sonic-ext-trim-admission"),
            (self.pg2, "sonic-ext-egress-mirror"),
        ):
            self.vapi.feature_enable_disable(
                sw_if_index=interface.sw_if_index,
                arc_name="interface-output",
                feature_name=feature,
                enable=True,
            )
            self.addCleanup(
                self.vapi.feature_enable_disable,
                sw_if_index=interface.sw_if_index,
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
