#!/usr/bin/env python3
"""sonic-ext isolation-group (SAI isolation group) tests

A frame received on an interface bound to an isolation group is not bridged
out of any member of the group. Frames from anywhere else are bridged as
usual, members included.

That is the MC-LAG peer-link rule: a frame that comes across the peer link has
already been delivered to the dual-homed port-channels by the other switch of
the pair, so it must not go out of them again, while a frame from a dual-homed
port-channel must still reach the peer link and the other port-channels.

Topology, one bridge domain:

    pg0   peer link, bound to the group
    pg1   member (dual-homed port-channel)
    pg2   member (dual-homed port-channel)
    pg3   orphan port, not in the group
"""

import unittest

from framework import VppTestCase
from asfframework import VppTestRunner
from vpp_l2 import VppBridgeDomain, VppBridgeDomainPort
from vpp_papi_provider import CliFailedCommandError

from scapy.layers.l2 import ARP, Ether
from scapy.layers.inet import IP, UDP

GROUP = 7
BD_ID = 1
NO_GROUP = 0xFFFFFFFF
BCAST_MAC = "ff:ff:ff:ff:ff:ff"

ISOLATED_IP4 = "/err/sonic-ext-l2-isolation-ip4/frames dropped by an isolation group"
ISOLATED_NONIP = (
    "/err/sonic-ext-l2-isolation-nonip/frames dropped by an isolation group"
)


class SonicExtIsolationBase(VppTestCase):
    """sonic-ext isolation-group test base"""

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
        super(SonicExtIsolationBase, cls).setUpClass()
        cls.create_pg_interfaces(range(4))

    def setUp(self):
        super(SonicExtIsolationBase, self).setUp()
        for i in self.pg_interfaces:
            i.admin_up()
        self.peer_link = self.pg0
        self.members = [self.pg1, self.pg2]
        self.orphan = self.pg3

        self.bd = VppBridgeDomain(self, BD_ID).add_vpp_config()
        self.bd_ports = [
            VppBridgeDomainPort(self, self.bd, i).add_vpp_config()
            for i in self.pg_interfaces
        ]

    def tearDown(self):
        # A test may have left any part of the group configured, or none.
        cleanup = [f"unbind {self.peer_link.name}"]
        cleanup += [f"{GROUP} member {i.name} del" for i in self.members]
        cleanup += [f"{GROUP} del"]
        for cmd in cleanup:
            try:
                self.vapi.cli(f"sonic-ext isolation-group {cmd}")
            except CliFailedCommandError:
                pass
        super(SonicExtIsolationBase, self).tearDown()
        for i in self.pg_interfaces:
            i.admin_down()

    # -- helpers -------------------------------------------------------------

    def feature_enabled(self):
        return self.vapi.sonic_ext_feature_get(feature="isolation-group").enabled

    def isolate(self):
        """The MC-LAG shape: the peer link isolated from both members."""
        self.vapi.sonic_ext_isolation_group_add_del(group_id=GROUP, is_add=True)
        for i in self.members:
            self.vapi.sonic_ext_isolation_group_member_add_del(
                group_id=GROUP, sw_if_index=i.sw_if_index, is_add=True
            )
        self.vapi.sonic_ext_isolation_group_bind(
            sw_if_index=self.peer_link.sw_if_index, group_id=GROUP
        )

    def bcast_ip4(self, src):
        return (
            Ether(dst=BCAST_MAC, src=src.remote_mac)
            / IP(src=src.remote_ip4, dst="255.255.255.255")
            / UDP(sport=1234, dport=4321)
            / b"isolation"
        )

    def bcast_arp(self, src):
        return Ether(dst=BCAST_MAC, src=src.remote_mac) / ARP(
            hwsrc=src.remote_mac, psrc=src.remote_ip4, pdst=src.local_ip4
        )

    def unicast_to(self, src, dst_mac):
        return (
            Ether(dst=dst_mac, src=src.remote_mac)
            / IP(src=src.remote_ip4, dst="10.0.0.1")
            / UDP(sport=1234, dport=4321)
            / b"isolation"
        )

    def flood(self, src, pkts, expect, silent):
        """Send pkts on src; each of expect gets them all, silent gets none."""
        src.add_stream(pkts)
        self.pg_enable_capture(self.pg_interfaces)
        self.pg_start()
        for i in expect:
            i.get_capture(len(pkts))
        for i in silent:
            i.assert_nothing_captured(remark=f"isolated from {src.name}")

    def counter(self, name):
        return self.statistics.get_err_counter(name)


class TestSonicExtIsolation(SonicExtIsolationBase):
    """sonic-ext isolation-group"""

    def test_feature_default_enabled(self):
        """isolation-group is a SAIVPP-owned feature, on by default"""
        self.assertTrue(self.feature_enabled())

    def test_no_group_floods_everywhere(self):
        """without a group, the peer link floods to every port"""
        self.flood(
            self.peer_link,
            [self.bcast_ip4(self.peer_link)],
            self.members + [self.orphan],
            [],
        )

    def test_peer_link_isolated_from_members(self):
        """a broadcast from the peer link reaches only the orphan port"""
        self.isolate()
        before = self.counter(ISOLATED_IP4)
        n = 3
        self.flood(
            self.peer_link,
            n * [self.bcast_ip4(self.peer_link)],
            [self.orphan],
            self.members,
        )
        self.assertEqual(self.counter(ISOLATED_IP4), before + n * len(self.members))

    def test_non_ip_isolated(self):
        """a non-IP frame (ARP) from the peer link is isolated too"""
        self.isolate()
        before = self.counter(ISOLATED_NONIP)
        self.flood(
            self.peer_link,
            [self.bcast_arp(self.peer_link)],
            [self.orphan],
            self.members,
        )
        self.assertEqual(self.counter(ISOLATED_NONIP), before + len(self.members))

    def test_members_still_reach_everything(self):
        """a member still floods to the peer link, the other member and the orphan"""
        self.isolate()
        self.flood(
            self.pg1,
            [self.bcast_ip4(self.pg1)],
            [self.peer_link, self.pg2, self.orphan],
            [],
        )

    def test_known_unicast_isolated(self):
        """known unicast from the peer link to a member's MAC is dropped,
        to the orphan's MAC it is delivered"""
        self.isolate()
        for i in [self.pg1, self.orphan]:
            self.vapi.l2fib_add_del(
                mac=i.remote_mac,
                bd_id=BD_ID,
                sw_if_index=i.sw_if_index,
                is_add=1,
                static_mac=1,
            )
        self.flood(
            self.peer_link,
            [self.unicast_to(self.peer_link, self.pg1.remote_mac)],
            [],
            [self.pg1],
        )
        self.flood(
            self.peer_link,
            [self.unicast_to(self.peer_link, self.orphan.remote_mac)],
            [self.orphan],
            [],
        )

    def test_unbind_restores_flooding(self):
        """unbinding the peer link floods to the members again"""
        self.isolate()
        self.vapi.sonic_ext_isolation_group_bind(
            sw_if_index=self.peer_link.sw_if_index, group_id=NO_GROUP
        )
        self.flood(
            self.peer_link,
            [self.bcast_ip4(self.peer_link)],
            self.members + [self.orphan],
            [],
        )

    def test_member_removed_is_flooded_to(self):
        """a member taken out of the group gets the peer link's floods again"""
        self.isolate()
        self.vapi.sonic_ext_isolation_group_member_add_del(
            group_id=GROUP, sw_if_index=self.pg2.sw_if_index, is_add=False
        )
        self.flood(
            self.peer_link,
            [self.bcast_ip4(self.peer_link)],
            [self.pg2, self.orphan],
            [self.pg1],
        )

    def test_member_added_twice_stays_isolated_after_one_removal(self):
        """members are a set: adding one twice and removing it once removes it"""
        self.isolate()
        self.vapi.sonic_ext_isolation_group_member_add_del(
            group_id=GROUP, sw_if_index=self.pg1.sw_if_index, is_add=True
        )
        self.vapi.sonic_ext_isolation_group_member_add_del(
            group_id=GROUP, sw_if_index=self.pg1.sw_if_index, is_add=False
        )
        self.flood(
            self.peer_link,
            [self.bcast_ip4(self.peer_link)],
            [self.pg1, self.orphan],
            [self.pg2],
        )

    def test_member_rejoining_the_bridge_domain(self):
        """VPP clears the L2 output features of an interface that leaves the
        bridge domain: adding it again as a member isolates it again"""
        self.isolate()
        self.bd_ports[1].remove_vpp_config()
        self.bd_ports[1] = VppBridgeDomainPort(self, self.bd, self.pg1).add_vpp_config()
        self.vapi.sonic_ext_isolation_group_member_add_del(
            group_id=GROUP, sw_if_index=self.pg1.sw_if_index, is_add=True
        )
        self.flood(
            self.peer_link,
            [self.bcast_ip4(self.peer_link)],
            [self.orphan],
            self.members,
        )

    def test_group_in_use_is_not_deleted(self):
        """a group with members or a bound interface can't be deleted"""
        self.isolate()
        with self.vapi.assert_negative_api_retval():
            self.vapi.sonic_ext_isolation_group_add_del(group_id=GROUP, is_add=False)

    def test_group_errors(self):
        """duplicate groups, unknown groups and bad interfaces are rejected"""
        self.vapi.sonic_ext_isolation_group_add_del(group_id=GROUP, is_add=True)
        with self.vapi.assert_negative_api_retval():
            self.vapi.sonic_ext_isolation_group_add_del(group_id=GROUP, is_add=True)
        with self.vapi.assert_negative_api_retval():
            self.vapi.sonic_ext_isolation_group_bind(
                sw_if_index=self.peer_link.sw_if_index, group_id=GROUP + 1
            )
        with self.vapi.assert_negative_api_retval():
            self.vapi.sonic_ext_isolation_group_member_add_del(
                group_id=GROUP + 1, sw_if_index=self.pg1.sw_if_index, is_add=True
            )
        with self.vapi.assert_negative_api_retval():
            self.vapi.sonic_ext_isolation_group_member_add_del(
                group_id=GROUP, sw_if_index=1000, is_add=True
            )

    def test_show(self):
        """show sonic-ext isolation-group lists bindings and members"""
        self.isolate()
        out = self.vapi.cli("show sonic-ext isolation-group")
        self.assertIn(f"isolation-group {GROUP}", out)
        for i in [self.peer_link] + self.members:
            self.assertIn(i.name, out)


class TestSonicExtIsolationStartupOff(SonicExtIsolationBase):
    """sonic-ext isolation-group off in startup.conf"""

    extra_vpp_config = ["sonic-ext", "{", "isolation-group", "off", "}"]

    def test_startup_off(self):
        """isolation-group off at startup rejects groups"""
        self.assertFalse(self.feature_enabled())
        with self.vapi.assert_negative_api_retval():
            self.vapi.sonic_ext_isolation_group_add_del(group_id=GROUP, is_add=True)


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
