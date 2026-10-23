#!/usr/bin/env python3
"""sonic-ext plugin tests"""

import re
import unittest
from contextlib import contextmanager
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


class SonicExtAllFeaturesBase(VppTestCase):
    """sonic-ext feature table test base"""

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

    # foreach_sonic_ext_feature (sonic_ext.h) with each feature's owner.  The
    # startup.conf stanza needs the names before VPP runs; show sonic-ext,
    # generated from the same table, then checks that none is missing.
    FEATURES = {
        "punt-via-member": "vpp",
        "host-xc": "vpp",
        "drop-member-stats": "vpp",
        "nd-punt": "vpp",
        "copp": "vpp",
        "ip2me": "saivpp",
        "l2-trap-fixup": "saivpp",
        "l2-vlan-filter": "saivpp",
        "pbh": "saivpp",
        "iface-loopback": "saivpp",
    }

    def show_features(self):
        """{feature: owner} as listed by show sonic-ext"""
        features = {}
        owner = "vpp"
        for line in self.vapi.cli("show sonic-ext").splitlines():
            line = line.strip()
            if line == "-- saivpp-wired --":
                owner = "saivpp"
            elif line == "-- counters --":
                break
            else:
                m = re.match(r"([a-z0-9-]+)\s+:\s+(on|off)$", line)
                if m:
                    features[m.group(1)] = owner
        return features

    def assert_all_features_state(self, enabled):
        self.assertEqual(self.show_features(), self.FEATURES)
        # capture has no keyword: it follows punt-via-member and drop-member-stats
        for feature in list(self.FEATURES) + ["capture"]:
            reply = self.vapi.sonic_ext_feature_get(feature=feature)
            self.assertEqual(
                bool(reply.enabled), enabled, "sonic_ext_feature_get %s" % feature
            )


class TestSonicExtAllFeaturesStartupOff(SonicExtAllFeaturesBase):
    """every sonic-ext feature off in one startup.conf stanza"""

    extra_vpp_config = (
        ["sonic-ext", "{"]
        + [
            word
            for feature in SonicExtAllFeaturesBase.FEATURES
            for word in (feature, "off")
        ]
        + ["}"]
    )

    def test_all_features_startup_off(self):
        """every feature keyword is accepted in one stanza and reported off"""
        self.assert_all_features_state(False)


class TestSonicExtAllFeaturesDefault(SonicExtAllFeaturesBase):
    """sonic-ext features default"""

    def test_all_features_defaults_on(self):
        """every feature is on by default, which an unknown name never is"""
        self.assert_all_features_state(True)


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


class SonicExtCoppBase(VppTestCase):
    """sonic-ext copp config test base"""

    extra_vpp_plugin_config = SonicExtNdPuntBase.extra_vpp_plugin_config + [
        "plugin",
        "linux_cp_unittest_plugin.so",
        "{",
        "enable",
        "}",
    ]

    @classmethod
    def setUpClass(cls):
        super(SonicExtCoppBase, cls).setUpClass()
        cls.create_pg_interfaces(range(2))

    def assert_feature(self, output, name, enabled):
        features = (line.strip() for line in output.splitlines())
        self.assertEqual(name in features, enabled)

    def assert_copp_state(self, enabled):
        self.assertEqual(
            bool(self.vapi.sonic_ext_feature_get(feature="copp").enabled), enabled
        )

        features = self.vapi.cli("show interface features local0")
        self.assert_feature(features, "sonic-ext-copp-ip2me", enabled)
        self.assert_feature(features, "sonic-ext-copp-ip2me-ip6", enabled)

        self.vapi.cli("test lcp add phy pg0 host pg1")
        try:
            features = self.vapi.cli("show interface features pg1")
            self.assert_feature(features, "sonic-ext-copp-ifout", enabled)
        finally:
            self.vapi.cli("test lcp del phy pg0 host pg1")

        self.assertEqual(
            "sonic-ext-copp-udld" in self.vapi.cli("show node snap-input"),
            enabled,
        )


class TestSonicExtCoppStartupOff(SonicExtCoppBase):
    """sonic-ext copp off in startup.conf"""

    extra_vpp_config = ["sonic-ext", "{", "copp", "off", "}"]

    def test_copp_startup_off(self):
        self.assert_copp_state(False)


class TestSonicExtCoppDefault(SonicExtCoppBase):
    """sonic-ext copp default"""

    def test_copp_defaults_on(self):
        self.assert_copp_state(True)


class SonicExtArcBase(VppTestCase):
    """sonic-ext feature arc test base"""

    extra_vpp_plugin_config = SonicExtCoppBase.extra_vpp_plugin_config

    @classmethod
    def setUpClass(cls):
        super(SonicExtArcBase, cls).setUpClass()
        cls.create_pg_interfaces(range(2))

    def assert_feature_get(self, feature, enabled):
        reply = self.vapi.sonic_ext_feature_get(feature=feature)
        self.assertEqual(
            bool(reply.enabled), enabled, "sonic_ext_feature_get %s" % feature
        )

    def arc_features(self, itf):
        """{arc: [feature node, ...]} from show interface features"""
        arcs = {}
        arc = None
        for line in self.vapi.cli("show interface features %s" % itf).splitlines():
            if line and not line[0].isspace() and line.endswith(":"):
                arc = arcs.setdefault(line[:-1], [])
            elif arc is not None and line.strip():
                arc.append(line.strip())
        return arcs

    def assert_feature(self, itf, arc, name, enabled):
        arcs = self.arc_features(itf)
        # a misspelt arc would otherwise pass every "not attached" check
        self.assertIn(arc, arcs, "no %s arc on %s" % (arc, itf))
        attached = name in arcs[arc]
        self.assertEqual(attached, enabled, "%s on %s %s" % (name, itf, arc))

    @contextmanager
    def lcp_pair(self):
        """pg1 stands in for pg0's linux-cp host tap"""
        self.vapi.cli("test lcp add phy pg0 host pg1")
        try:
            yield
        finally:
            self.vapi.cli("test lcp del phy pg0 host pg1")


class SonicExtHostXcBase(SonicExtArcBase):
    """sonic-ext host-xc test base"""

    def assert_host_xc_state(self, enabled):
        self.assert_feature_get("host-xc", enabled)
        with self.lcp_pair():
            self.assert_feature("pg1", "device-input", "sonic-ext-host-xc", enabled)


class TestSonicExtHostXcStartupOff(SonicExtHostXcBase):
    """sonic-ext host-xc off in startup.conf"""

    extra_vpp_config = ["sonic-ext", "{", "host-xc", "off", "}"]

    def test_host_xc_startup_off(self):
        """host-xc off at startup is never attached to a host tap"""
        self.assert_host_xc_state(False)


class TestSonicExtHostXcDefault(SonicExtHostXcBase):
    """sonic-ext host-xc default"""

    def test_host_xc_defaults_on(self):
        """host-xc is on by default and attached to a new host tap"""
        self.assert_host_xc_state(True)


class SonicExtDropMemberStatsBase(SonicExtArcBase):
    """sonic-ext drop-member-stats test base"""

    def assert_drop_member_stats_state(self, enabled):
        self.assert_feature_get("drop-member-stats", enabled)
        # local0 exists before the plugin's interface hook, pg0 is created after
        for itf in ("local0", "pg0"):
            self.assert_feature(
                itf, "error-drop", "sonic-ext-drop-member-stats", enabled
            )
        # capture keeps running either way, for punt-via-member
        self.assert_feature_get("capture", True)
        with self.lcp_pair():
            self.assert_feature("pg0", "device-input", "sonic-ext-capture", True)


class TestSonicExtDropMemberStatsStartupOff(SonicExtDropMemberStatsBase):
    """sonic-ext drop-member-stats off in startup.conf"""

    extra_vpp_config = ["sonic-ext", "{", "drop-member-stats", "off", "}"]

    def test_drop_member_stats_startup_off(self):
        """drop-member-stats off at startup stays off the error-drop arc"""
        self.assert_drop_member_stats_state(False)


class TestSonicExtDropMemberStatsDefault(SonicExtDropMemberStatsBase):
    """sonic-ext drop-member-stats default"""

    def test_drop_member_stats_defaults_on(self):
        """drop-member-stats is on by default on every interface"""
        self.assert_drop_member_stats_state(True)


class SonicExtPuntViaMemberBase(SonicExtArcBase):
    """sonic-ext punt-via-member test base"""

    def assert_punt_via_member_state(self, punt_via_member, capture):
        self.assert_feature_get("punt-via-member", punt_via_member)
        # capture has no keyword: it runs while punt-via-member or
        # drop-member-stats, the two consumers of its cookie, is on
        self.assert_feature_get("capture", capture)
        for arc in ("ip4-drop", "ip6-drop"):
            self.assert_feature(
                "local0", arc, "sonic-ext-glean-redirect", punt_via_member
            )
        with self.lcp_pair():
            self.assert_feature("pg0", "device-input", "sonic-ext-capture", capture)


class TestSonicExtPuntViaMemberStartupOff(SonicExtPuntViaMemberBase):
    """sonic-ext punt-via-member off in startup.conf"""

    extra_vpp_config = ["sonic-ext", "{", "punt-via-member", "off", "}"]

    def test_punt_via_member_startup_off(self):
        """punt-via-member off at startup leaves only capture, for drop-member-stats"""
        self.assert_punt_via_member_state(False, True)


class TestSonicExtCaptureStartupOff(SonicExtPuntViaMemberBase):
    """sonic-ext punt-via-member and drop-member-stats off in startup.conf"""

    extra_vpp_config = [
        "sonic-ext",
        "{",
        "punt-via-member",
        "off",
        "drop-member-stats",
        "off",
        "}",
    ]

    def test_capture_startup_off(self):
        """with both of its consumers off at startup, capture never runs"""
        self.assert_punt_via_member_state(False, False)


class TestSonicExtPuntViaMemberDefault(SonicExtPuntViaMemberBase):
    """sonic-ext punt-via-member default"""

    def test_punt_via_member_defaults_on(self):
        """punt-via-member is on by default with capture and glean redirect"""
        self.assert_punt_via_member_state(True, True)


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
