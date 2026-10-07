#!/usr/bin/env python3
"""sonic-ext iface-loopback (RIF loopback packet action) tests

A "hairpin" is a routed packet whose resolved next hop egresses the very
interface it arrived on (VLIB_RX == VLIB_TX). The iface-loopback feature drops
and counts such packets per interface when the action is DROP, and forwards
them (the default) when it is FORWARD.

Each test forges a hairpin by installing a /32 route whose single path points
back out the ingress interface, then injects a routed packet for that prefix.
"""

import unittest

from framework import VppTestCase
from asfframework import VppTestRunner
from vpp_papi import VppEnum
from vpp_papi_provider import CliFailedCommandError
from vpp_ip_route import VppIpRoute, VppRoutePath
from vpp_neighbor import VppNeighbor

from scapy.layers.l2 import Ether
from scapy.layers.inet import IP, UDP

# Must match SONIC_EXT_LOOPBACK_ACTION_* in sonic_ext.h.
ACTION_FORWARD = 0
ACTION_DROP = 1

# A destination that the forged route sends straight back out the ingress RIF.
HAIRPIN_DST = "10.10.10.10"


class SonicExtLoopbackBase(VppTestCase):
    """sonic-ext iface-loopback test base"""

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
        super(SonicExtLoopbackBase, cls).setUpClass()
        cls.create_pg_interfaces(range(1))

    def setUp(self):
        super(SonicExtLoopbackBase, self).setUp()
        self.pg0.admin_up()
        self.pg0.config_ip4()

        # Static neighbor for the next hop, then a /32 route whose only path
        # egresses pg0 via that neighbor -> a packet for HAIRPIN_DST routes
        # back out its ingress interface.
        self.nbr = VppNeighbor(
            self,
            self.pg0.sw_if_index,
            self.pg0.remote_mac,
            self.pg0.remote_ip4,
            is_static=True,
        )
        self.nbr.add_vpp_config()

        self.route = VppIpRoute(
            self,
            HAIRPIN_DST,
            32,
            [VppRoutePath(self.pg0.remote_ip4, self.pg0.sw_if_index)],
        )
        self.route.add_vpp_config()

    def tearDown(self):
        super(SonicExtLoopbackBase, self).tearDown()
        # Clear the action so a recycled sw_if_index starts clean, then undo
        # the routing config.
        self.set_action(ACTION_FORWARD)
        self.route.remove_vpp_config()
        self.nbr.remove_vpp_config()
        self.pg0.unconfig_ip4()
        self.pg0.admin_down()

    # -- helpers -------------------------------------------------------------

    def feature_enabled(self):
        return self.vapi.sonic_ext_feature_get(feature="iface-loopback").enabled

    def set_action(self, action, sw_if_index=None):
        if sw_if_index is None:
            sw_if_index = self.pg0.sw_if_index
        return self.vapi.sonic_ext_iface_loopback_set_action(
            sw_if_index=sw_if_index, action=action
        )

    def hairpin_pkt(self):
        """A routed unicast packet (eth_dst = router MAC) for HAIRPIN_DST."""
        return (
            Ether(dst=self.pg0.local_mac, src=self.pg0.remote_mac)
            / IP(src=self.pg0.remote_ip4, dst=HAIRPIN_DST, ttl=64)
            / UDP(sport=1234, dport=4321)
            / b"hairpin"
        )

    def drops(self):
        return self.statistics.get_err_counter(
            "/err/sonic-ext-ip4-loopback/hairpin packets dropped"
        )


class TestSonicExtLoopback(SonicExtLoopbackBase):
    """sonic-ext iface-loopback"""

    def test_feature_default_enabled(self):
        """iface-loopback is a SAIVPP-owned feature, on by default"""
        self.assertTrue(self.feature_enabled())

    def test_forward_default(self):
        """default action FORWARD: the hairpin egresses its ingress RIF"""
        before = self.drops()
        rx = self.send_and_expect(self.pg0, [self.hairpin_pkt()], self.pg0)
        self.assertEqual(len(rx), 1)
        # Routed, so TTL is decremented and it leaves the same port.
        self.assertEqual(rx[0][IP].dst, HAIRPIN_DST)
        self.assertEqual(rx[0][IP].ttl, 63)
        self.assertEqual(self.drops(), before)

    def test_drop(self):
        """action DROP: the hairpin is dropped and counted"""
        self.set_action(ACTION_DROP)
        before = self.drops()
        n = 5
        self.send_and_assert_no_replies(self.pg0, n * [self.hairpin_pkt()])
        self.assertEqual(self.drops(), before + n)

    def test_drop_then_forward(self):
        """DROP then FORWARD restores forwarding and stops counting"""
        self.set_action(ACTION_DROP)
        self.send_and_assert_no_replies(self.pg0, [self.hairpin_pkt()])

        self.set_action(ACTION_FORWARD)
        before = self.drops()
        rx = self.send_and_expect(self.pg0, [self.hairpin_pkt()], self.pg0)
        self.assertEqual(len(rx), 1)
        self.assertEqual(self.drops(), before)

    def test_idempotent_set(self):
        """repeated DROP sets are a no-op (ref-counted arc stays balanced)"""
        self.set_action(ACTION_DROP)
        self.set_action(ACTION_DROP)
        self.send_and_assert_no_replies(self.pg0, [self.hairpin_pkt()])
        # A single FORWARD must fully re-enable forwarding despite two DROP sets.
        self.set_action(ACTION_FORWARD)
        rx = self.send_and_expect(self.pg0, [self.hairpin_pkt()], self.pg0)
        self.assertEqual(len(rx), 1)

    def test_bad_action_rejected(self):
        """an action that is neither FORWARD nor DROP is rejected"""
        with self.assertRaises(Exception):
            self.set_action(2)

class TestSonicExtLoopbackStartupOff(SonicExtLoopbackBase):
    """sonic-ext iface-loopback off in startup.conf"""

    extra_vpp_config = ["sonic-ext", "{", "iface-loopback", "off", "}"]

    def test_startup_off(self):
        """iface-loopback off at startup leaves hairpins forwarded"""
        self.assertFalse(self.feature_enabled())
        self.set_action(ACTION_DROP)
        rx = self.send_and_expect(self.pg0, [self.hairpin_pkt()], self.pg0)
        self.assertEqual(len(rx), 1)


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
