#!/usr/bin/env python3
"""sonic-ext drop-reason-stats tests

sonic-ext-drop-reason-stats runs on the error-drop arc.  It maps each dropped
buffer's b->error to a SAI drop reason and counts it against the wire ingress
port in /sonic-ext/drop-reason/<REASON>, indexed by sw_if_index.  SAI-VPP
reads those counters to back SAI debug (configurable drop) counters.

The drops under test come from ip4-validate, which stamps b->error on every
packet it drops.
"""

import unittest

from asfframework import VppTestRunner, get_testcase_dirname
from config import config
from framework import VppTestCase
from vpp_bond_interface import VppBondInterface
from vpp_papi import MACAddress, VppEnum
from vpp_qemu_utils import create_namespace, delete_all_namespaces, set_interface_up

from scapy.layers.inet import IP, UDP
from scapy.layers.l2 import Ether
from scapy.packet import Raw

NODE = "sonic-ext-drop-reason-stats"
COUNTED = "/err/%s/drop counted against a SAI drop reason" % NODE
DIP_LL_ERR = "/err/ip4-validate/destination address is link-local"
SIP_LL_ERR = "/err/ip4-validate/source address is link-local"
SIP_LOOPBACK_ERR = "/err/ip4-validate/source address is loopback"

REASONS = ("SIP_LINK_LOCAL", "DIP_LINK_LOCAL")
LINK_LOCAL_IP4 = "169.254.0.1"
N_PKTS = 7

PLUGINS = [
    # sonic-ext, linux-cp and ip_validate are all .default_disabled.
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
    "ip_validate_plugin.so",
    "{",
    "enable",
    "}",
]


class SonicExtDropReasonMixin:
    """Counter and packet helpers shared by the test classes"""

    def reason_count(self, reason, sw_if_index):
        """Drops of `reason` charged to `sw_if_index`, summed over threads"""
        c = self.statistics["/sonic-ext/drop-reason/%s" % reason]
        return sum(t[sw_if_index] for t in c if sw_if_index < len(t))

    def reason_counts(self, sw_if_indices):
        return {
            (r, i): self.reason_count(r, i) for r in REASONS for i in sw_if_indices
        }

    def err(self, name):
        return self.statistics.get_err_counter(name)

    def stat_exists(self, name):
        try:
            self.statistics[name]
        except Exception:
            return False
        return True

    def udp4(self, rx, dst_mac, src_ip, dst_ip):
        return [
            Ether(src=rx.remote_mac, dst=dst_mac)
            / IP(src=src_ip, dst=dst_ip)
            / UDP(sport=10000 + i, dport=20000)
            / Raw(b"\xa5" * 64)
            for i in range(N_PKTS)
        ]

    def send_dropped(self, rx, pkts, others):
        rx.add_stream(pkts)
        self.pg_enable_capture(self.pg_interfaces)
        self.pg_start()
        for itf in others:
            itf.assert_nothing_captured(remark="packet should have been dropped")

    def assert_only_changed(self, before, after, changed):
        """Every counter equals its value before, except those in `changed`"""
        for key, value in after.items():
            self.assertEqual(
                value,
                before[key] + changed.get(key, 0),
                "drop-reason counter %s/%s" % key,
            )


@unittest.skipIf(
    "sonic_ext" in config.excluded_plugins, "Exclude sonic-ext plugin tests"
)
class TestSonicExtDropReason(SonicExtDropReasonMixin, VppTestCase):
    """sonic-ext drop-reason-stats on routed ports"""

    extra_vpp_plugin_config = PLUGINS

    @classmethod
    def setUpClass(cls):
        super(TestSonicExtDropReason, cls).setUpClass()
        try:
            cls.create_pg_interfaces(range(2))
            for i in cls.pg_interfaces:
                i.admin_up()
                i.config_ip4()
                i.resolve_arp()
        except Exception:
            cls.tearDownClass()
            raise

    @classmethod
    def tearDownClass(cls):
        for i in cls.pg_interfaces:
            i.unconfig_ip4()
            i.admin_down()
        super(TestSonicExtDropReason, cls).tearDownClass()

    def show_commands_at_teardown(self):
        self.logger.info(self.vapi.cli("show sonic-ext drop-reasons"))
        self.logger.info(self.vapi.cli("show errors"))

    def test_dip_link_local(self):
        """DIP link-local drops count against the ingress port only"""
        idx = [self.pg0.sw_if_index, self.pg1.sw_if_index]
        before = self.reason_counts(idx)
        err_before = self.err(DIP_LL_ERR)
        counted_before = self.err(COUNTED)

        pkts = self.udp4(
            self.pg0, self.pg0.local_mac, self.pg0.remote_ip4, LINK_LOCAL_IP4
        )
        self.send_dropped(self.pg0, pkts, [self.pg1])

        self.assert_only_changed(
            before,
            self.reason_counts(idx),
            {("DIP_LINK_LOCAL", self.pg0.sw_if_index): N_PKTS},
        )
        # b->error is stamped and the drop node counts it, exactly once.
        self.assertEqual(self.err(DIP_LL_ERR), err_before + N_PKTS)
        self.assertEqual(self.err(COUNTED), counted_before + N_PKTS)

    def test_sip_link_local(self):
        """SIP link-local drops count against the ingress port only"""
        idx = [self.pg0.sw_if_index, self.pg1.sw_if_index]
        before = self.reason_counts(idx)
        err_before = self.err(SIP_LL_ERR)

        pkts = self.udp4(
            self.pg1, self.pg1.local_mac, LINK_LOCAL_IP4, self.pg0.remote_ip4
        )
        self.send_dropped(self.pg1, pkts, [self.pg0])

        self.assert_only_changed(
            before,
            self.reason_counts(idx),
            {("SIP_LINK_LOCAL", self.pg1.sw_if_index): N_PKTS},
        )
        self.assertEqual(self.err(SIP_LL_ERR), err_before + N_PKTS)

    def test_unmapped_drop_not_counted(self):
        """A drop with no SAI reason mapping changes no reason counter"""
        idx = [self.pg0.sw_if_index, self.pg1.sw_if_index]
        before = self.reason_counts(idx)
        err_before = self.err(SIP_LOOPBACK_ERR)
        counted_before = self.err(COUNTED)

        pkts = self.udp4(
            self.pg0, self.pg0.local_mac, "127.0.0.1", self.pg1.remote_ip4
        )
        self.send_dropped(self.pg0, pkts, [self.pg1])

        self.assert_only_changed(before, self.reason_counts(idx), {})
        self.assertEqual(self.err(SIP_LOOPBACK_ERR), err_before + N_PKTS)
        self.assertEqual(self.err(COUNTED), counted_before)

    def test_forwarded_not_counted(self):
        """Forwarded traffic changes no reason counter"""
        idx = [self.pg0.sw_if_index, self.pg1.sw_if_index]
        before = self.reason_counts(idx)

        pkts = self.udp4(
            self.pg0, self.pg0.local_mac, self.pg0.remote_ip4, self.pg1.remote_ip4
        )
        self.pg0.add_stream(pkts)
        self.pg_enable_capture(self.pg_interfaces)
        self.pg_start()
        self.pg1.get_capture(N_PKTS)

        self.assert_only_changed(before, self.reason_counts(idx), {})

    def test_cli_and_arc(self):
        """CLI reports the toggle and mapping, and the node is on error-drop"""
        out = self.vapi.cli("show sonic-ext drop-reasons")
        self.assertIn("drop-reason-stats: on", out)
        for reason in REASONS:
            self.assertRegex(
                out, r"%s\s+yes\s+/sonic-ext/drop-reason/%s" % (reason, reason)
            )

        self.assertRegex(
            self.vapi.cli("show sonic-ext"), r"drop-reason-stats\s+:\s+on"
        )
        features = self.vapi.cli("show interface features %s" % self.pg0.name)
        self.assertIn(NODE, features)


@unittest.skipIf(
    "sonic_ext" in config.excluded_plugins, "Exclude sonic-ext plugin tests"
)
class TestSonicExtDropReasonStartupOff(SonicExtDropReasonMixin, VppTestCase):
    """sonic-ext drop-reason-stats off in startup.conf"""

    extra_vpp_plugin_config = PLUGINS
    extra_vpp_config = ["sonic-ext", "{", "drop-reason-stats", "off", "}"]

    @classmethod
    def setUpClass(cls):
        super(TestSonicExtDropReasonStartupOff, cls).setUpClass()
        try:
            cls.create_pg_interfaces(range(2))
            for i in cls.pg_interfaces:
                i.admin_up()
                i.config_ip4()
                i.resolve_arp()
        except Exception:
            cls.tearDownClass()
            raise

    @classmethod
    def tearDownClass(cls):
        for i in cls.pg_interfaces:
            i.unconfig_ip4()
            i.admin_down()
        super(TestSonicExtDropReasonStartupOff, cls).tearDownClass()

    def test_startup_off(self):
        """With the toggle off, nothing is wired or counted"""
        self.assertIn(
            "drop-reason-stats: off", self.vapi.cli("show sonic-ext drop-reasons")
        )
        self.assertNotIn(
            NODE, self.vapi.cli("show interface features %s" % self.pg0.name)
        )
        reply = self.vapi.sonic_ext_feature_get(feature="drop-reason-stats")
        self.assertFalse(reply.enabled)

        counted_before = self.err(COUNTED)
        pkts = self.udp4(
            self.pg0, self.pg0.local_mac, self.pg0.remote_ip4, LINK_LOCAL_IP4
        )
        self.send_dropped(self.pg0, pkts, [self.pg1])
        self.assertEqual(self.err(COUNTED), counted_before)
        # The counters are only created when the feature is on.
        for reason in REASONS:
            self.assertFalse(self.stat_exists("/sonic-ext/drop-reason/%s" % reason))


@unittest.skipIf("linux-cp" in config.excluded_plugins, "Exclude linux-cp plugin tests")
@unittest.skipIf(
    "sonic_ext" in config.excluded_plugins, "Exclude sonic-ext plugin tests"
)
@unittest.skipIf(config.skip_netns_tests, "netns not available or disabled from cli")
class TestSonicExtDropReasonBond(SonicExtDropReasonMixin, VppTestCase):
    """sonic-ext drop-reason-stats charges bond drops to the member port"""

    extra_vpp_plugin_config = PLUGINS

    # Without a custom MAC the bond takes its first member's MAC when that
    # member is added, so the address read at creation would be stale and
    # ethernet-input would drop the test traffic as an L3 MAC mismatch.
    BOND_MAC = "02:fe:38:30:59:3c"
    BOND_IP4 = "10.10.10.1"
    BOND_REMOTE_IP4 = "10.10.10.2"
    PREFIX_LEN = 24

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
        super(TestSonicExtDropReasonBond, self).setUp()

        self.create_pg_interfaces(range(2))
        for i in self.pg_interfaces:
            i.admin_up()
        self.members = (self.pg0, self.pg1)

        # Member LCP pairs first, as in SONiC.  The pair is what enables
        # sonic-ext-capture on the member, which records the wire port in the
        # buffer cookie before bond-input rewrites VLIB_RX to the bond.
        self.lcp_pairs = []
        for i, host in ((self.pg0, "hpg0"), (self.pg1, "hpg1")):
            self.vapi.cli(
                "lcp create %s host-if %s netns %s" % (i, host, self.ns_name)
            )
            self.lcp_pairs.append(i)
            set_interface_up(self.ns_name, host)

        self.bond = VppBondInterface(
            self,
            mode=VppEnum.vl_api_bond_mode_t.BOND_API_MODE_XOR,
            lb=VppEnum.vl_api_bond_lb_algo_t.BOND_API_LB_ALGO_L34,
            use_custom_mac=1,
            mac_address=MACAddress(self.BOND_MAC).packed,
        )
        self.bond.add_vpp_config()
        self.bond.admin_up()
        for i in self.members:
            self.bond.add_member_vpp_bond_interface(sw_if_index=i.sw_if_index)
        self.vapi.sw_interface_add_del_address(
            sw_if_index=self.bond.sw_if_index,
            prefix="%s/%d" % (self.BOND_IP4, self.PREFIX_LEN),
        )

    def tearDown(self):
        self.vapi.sw_interface_add_del_address(
            sw_if_index=self.bond.sw_if_index,
            prefix="%s/%d" % (self.BOND_IP4, self.PREFIX_LEN),
            is_add=0,
        )
        for i in self.members:
            self.bond.detach_vpp_bond_interface(sw_if_index=i.sw_if_index)
        self.bond.admin_down()
        self.bond.remove_vpp_config()
        for itf in reversed(self.lcp_pairs):
            self.vapi.cli("lcp delete %s" % itf)
        for i in self.pg_interfaces:
            i.admin_down()
        super(TestSonicExtDropReasonBond, self).tearDown()

    def show_commands_at_teardown(self):
        self.logger.info(self.vapi.cli("show sonic-ext drop-reasons"))
        self.logger.info(self.vapi.cli("show interface"))

    def test_bond_member_attribution(self):
        """A drop received on a bond member counts against the member"""
        idx = [self.pg0.sw_if_index, self.pg1.sw_if_index, self.bond.sw_if_index, 0]
        before = self.reason_counts(idx)
        err_before = self.err(DIP_LL_ERR)

        pkts = self.udp4(self.pg0, self.BOND_MAC, self.BOND_REMOTE_IP4, LINK_LOCAL_IP4)
        self.send_dropped(self.pg0, pkts, [self.pg1])

        self.assertEqual(self.err(DIP_LL_ERR), err_before + N_PKTS)
        self.assert_only_changed(
            before,
            self.reason_counts(idx),
            {("DIP_LINK_LOCAL", self.pg0.sw_if_index): N_PKTS},
        )


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
