#!/usr/bin/env python3
"""Everflow dataplane tests (ACL PERMIT_MIRROR + sonic-ext deferred clone).

Covers the dataplane half of SONiC Everflow: patch 0019's PERMIT_MIRROR
action (is_permit == 3) and sonic-ext's deferred MIRROR_EGRESS stamper /
sonic-ext-egress-mirror node.  Bindings go through the ACL and sonic-ext
binary APIs the same way sairedis does, against packet-generator ports, so
the tests isolate the dataplane from SAI.

Topology:

    pg0  ingress (routed)          <- traffic injected here
    pg1  routed egress             <- original packet
    pg2  mirror destination        <- ACL clone (SPAN-style)

An immediate PERMIT_MIRROR clone is taken in the ACL node and is the
original L2 frame.  A deferred clone (ACL_MIRROR_F_DEFERRED, ingress ACL,
stamper registered) is taken later on interface-output and matches the
post-rewrite forwarded packet.  Non-matching traffic is forwarded by a
trailing permit-all rule and must not be cloned.
"""

import unittest
from ipaddress import IPv4Network

from scapy.layers.inet import IP, TCP, UDP
from scapy.layers.l2 import Ether
from scapy.packet import Raw

from asfframework import VppTestRunner
from config import config
from framework import VppTestCase
from vpp_acl import AclRule, VppAcl, VppAclInterface
from vpp_papi import VppEnum

# acl_types.api: ACL_ACTION_API_PERMIT / PERMIT_MIRROR.  Numeric fallbacks
# keep the file importable if the enum is missing for some reason.
try:
    ACL_PERMIT = int(VppEnum.vl_api_acl_action_t.ACL_ACTION_API_PERMIT)
except AttributeError:
    ACL_PERMIT = 1
try:
    ACL_PERMIT_MIRROR = int(
        VppEnum.vl_api_acl_action_t.ACL_ACTION_API_PERMIT_MIRROR
    )
except AttributeError:
    ACL_PERMIT_MIRROR = 3

# Packed mirror_action: dest sw_if_index in bits 27:0, flags in 31:28.
# Must match vppbld/patches/0019-acl-add-permit-mirror-action-for-everflow.patch.
ACL_MIRROR_SW_IF_INDEX_MASK = 0x0FFFFFFF
ACL_MIRROR_FLAGS_SHIFT = 28
ACL_MIRROR_F_DEFERRED = 1 << 0

PROTO_UDP = 17
PROTO_TCP = 6
MATCH_SPORT = 1234
MATCH_DPORT = 4321
PAYLOAD = b"everflow-ut"


def pack_mirror_action(sw_if_index, deferred=False):
    flags = ACL_MIRROR_F_DEFERRED if deferred else 0
    return (flags << ACL_MIRROR_FLAGS_SHIFT) | (
        sw_if_index & ACL_MIRROR_SW_IF_INDEX_MASK
    )


class MirrorAclRule(AclRule):
    """AclRule that encodes patch 0018/0019 fields VPP's helper does not know."""

    def __init__(
        self,
        is_permit,
        mirror_sw_if_index=0,
        deferred=False,
        in_sw_if_index=0,
        **kwargs
    ):
        super(MirrorAclRule, self).__init__(is_permit, **kwargs)
        self.mirror_sw_if_index = mirror_sw_if_index
        self.deferred = deferred
        self.in_sw_if_index = in_sw_if_index

    def encode(self):
        encoded = super(MirrorAclRule, self).encode()
        encoded["in_sw_if_index"] = self.in_sw_if_index
        encoded["mirror_action"] = pack_mirror_action(
            self.mirror_sw_if_index, self.deferred
        )
        return encoded


class EverflowAclTopoBase(VppTestCase):
    """pg0/pg1/pg2 routed topology used by every Everflow ACL suite."""

    vpp_worker_count = 0

    extra_vpp_plugin_config = [
        "plugin",
        "acl_plugin.so",
        "{",
        "enable",
        "}",
    ]

    @classmethod
    def setUpClass(cls):
        super(EverflowAclTopoBase, cls).setUpClass()
        cls.create_pg_interfaces(range(3))
        cls.ing, cls.fwd, cls.mirror = cls.pg0, cls.pg1, cls.pg2
        for pg in cls.pg_interfaces:
            pg.admin_up()
            pg.config_ip4()
        # Two neighbors so a dst-IP miss can still be a connected, resolvable
        # address on pg1 rather than an unroutable blackhole.
        cls.fwd.generate_remote_hosts(2)
        cls.fwd.configure_ipv4_neighbors()

    def setUp(self):
        super(EverflowAclTopoBase, self).setUp()
        self.ing, self.fwd, self.mirror = self.pg0, self.pg1, self.pg2
        # One ACL + bind per test: VppAclInterface.object_id() is only the
        # sw_if_index, so a second bind on pg0 is dropped by the registry and
        # tearDown then tries to acl_del a still-bound list (-142).
        self._acl = None
        self._acl_if = None

    def show_commands_at_teardown(self):
        self.logger.info(self.vapi.cli("show acl-plugin acl"))
        self.logger.info(self.vapi.cli("show acl-plugin interface"))
        self.logger.info(self.vapi.cli("show acl-plugin tables"))

    def dst_ip(self):
        return self.fwd.remote_hosts[0].ip4

    def routed_pkt(
        self,
        src=None,
        dst=None,
        proto="udp",
        sport=MATCH_SPORT,
        dport=MATCH_DPORT,
        ttl=64,
    ):
        if src is None:
            src = self.ing.remote_ip4
        if dst is None:
            dst = self.dst_ip()
        eth = Ether(dst=self.ing.local_mac, src=self.ing.remote_mac)
        ip = IP(src=src, dst=dst, ttl=ttl)
        if proto == "tcp":
            l4 = TCP(sport=sport, dport=dport)
        else:
            l4 = UDP(sport=sport, dport=dport)
        return eth / ip / l4 / Raw(PAYLOAD)

    def permit_all(self):
        # Same encoder as the mirror rule so in_sw_if_index / mirror_action
        # are present on the API struct (AclRule.encode() does not know them).
        return MirrorAclRule(is_permit=ACL_PERMIT)

    def mirror_rule(self, deferred=False, mirror_sw_if_index=None, **kwargs):
        if mirror_sw_if_index is None:
            mirror_sw_if_index = self.mirror.sw_if_index
        return MirrorAclRule(
            is_permit=ACL_PERMIT_MIRROR,
            mirror_sw_if_index=mirror_sw_if_index,
            deferred=deferred,
            **kwargs
        )

    def install_input_acl(self, rules, sw_if_index=None):
        if sw_if_index is None:
            sw_if_index = self.ing.sw_if_index
        if self._acl is None:
            self._acl = VppAcl(self, rules=rules)
            self._acl.add_vpp_config()
            self._acl_if = VppAclInterface(
                self, sw_if_index=sw_if_index, n_input=1, acls=[self._acl]
            )
            self._acl_if.add_vpp_config()
        else:
            self._acl.modify_vpp_config(rules)
        return self._acl

    def inject(self, pkts):
        self.pg_enable_capture(self.pg_interfaces)
        self.ing.add_stream(list(pkts))
        self.pg_start()

    def expect_mirrored(self, pkt, n=1):
        self.inject([pkt] if not isinstance(pkt, list) else pkt)
        fwd = self.fwd.get_capture(n)
        mir = self.mirror.get_capture(n)
        return fwd, mir

    def expect_forward_only(self, pkt):
        self.inject([pkt])
        fwd = self.fwd.get_capture(1)
        self.mirror.assert_nothing_captured()
        return fwd

    def assert_forwarded(self, rx, tx):
        self.assertEqual(rx[Ether].src, self.fwd.local_mac)
        self.assertEqual(rx[IP].src, tx[IP].src)
        self.assertEqual(rx[IP].dst, tx[IP].dst)
        self.assertEqual(rx[IP].ttl, tx[IP].ttl - 1)
        self.assertEqual(bytes(rx[Raw]), PAYLOAD)

    def assert_ingress_clone(self, rx, tx):
        """Immediate clone: original L2 frame, TTL not decremented."""
        self.assertEqual(rx[Ether].src, tx[Ether].src)
        self.assertEqual(rx[Ether].dst, tx[Ether].dst)
        self.assertEqual(rx[IP].src, tx[IP].src)
        self.assertEqual(rx[IP].dst, tx[IP].dst)
        self.assertEqual(rx[IP].ttl, tx[IP].ttl)
        self.assertEqual(bytes(rx[Raw]), PAYLOAD)

    def assert_egress_clone(self, rx, tx):
        """Deferred clone: post-rewrite form, same as the forwarded packet."""
        self.assert_forwarded(rx, tx)


@unittest.skipIf("acl" in config.excluded_plugins, "Exclude acl plugin tests")
@unittest.skipIf("linux-cp" in config.excluded_plugins, "Exclude linux-cp plugin tests")
@unittest.skipIf(
    "sonic_ext" in config.excluded_plugins, "Exclude sonic-ext plugin tests"
)
class SonicExtEverflowBase(EverflowAclTopoBase):
    """Everflow with sonic-ext loaded so the deferred stamper is registered."""

    extra_vpp_plugin_config = EverflowAclTopoBase.extra_vpp_plugin_config + [
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

    def enable_egress_mirror(self):
        self.vapi.sonic_ext_egress_mirror_enable_disable(enable=True)
        self.addCleanup(
            self.vapi.sonic_ext_egress_mirror_enable_disable, enable=False
        )

    def show_commands_at_teardown(self):
        super(SonicExtEverflowBase, self).show_commands_at_teardown()
        self.logger.info(self.vapi.cli("show sonic-ext"))


class TestSonicExtEverflowImmediate(SonicExtEverflowBase):
    """Ingress ACL PERMIT_MIRROR, clone taken in the ACL node."""

    def test_matching_packet_cloned_and_forwarded(self):
        """Match: original forwarded, immediate clone is the ingress L2 frame"""
        pkt = self.routed_pkt()
        self.install_input_acl([self.mirror_rule(), self.permit_all()])
        fwd, mir = self.expect_mirrored(pkt)
        self.assert_forwarded(fwd[0], pkt)
        self.assert_ingress_clone(mir[0], pkt)

    def test_non_matching_packet_forwarded_not_cloned(self):
        """Miss: trailing permit-all forwards; mirror dest is silent"""
        self.install_input_acl(
            [
                self.mirror_rule(
                    proto=PROTO_UDP,
                    sport_from=MATCH_SPORT,
                    sport_to=MATCH_SPORT,
                    dport_from=MATCH_DPORT,
                    dport_to=MATCH_DPORT,
                ),
                self.permit_all(),
            ]
        )
        miss = self.routed_pkt(sport=MATCH_SPORT + 1)
        fwd = self.expect_forward_only(miss)
        self.assert_forwarded(fwd[0], miss)

    def test_catchall_produces_single_clone(self):
        """A catch-all mirror rule still produces exactly one clone"""
        pkt = self.routed_pkt()
        self.install_input_acl([self.mirror_rule()])
        fwd, mir = self.expect_mirrored(pkt)
        self.assertEqual(len(fwd), 1)
        self.assertEqual(len(mir), 1)
        self.assert_forwarded(fwd[0], pkt)
        self.assert_ingress_clone(mir[0], pkt)

    def test_down_mirror_dest_still_forwards(self):
        """Clone is skipped when the dest is admin-down; original still goes"""
        self.install_input_acl([self.mirror_rule(), self.permit_all()])
        self.mirror.admin_down()
        try:
            pkt = self.routed_pkt()
            fwd = self.expect_forward_only(pkt)
            self.assert_forwarded(fwd[0], pkt)
        finally:
            self.mirror.admin_up()

    def test_invalid_mirror_dest_still_forwards(self):
        """A stale dest sw_if_index must not crash or drop the original"""
        self.install_input_acl(
            [
                self.mirror_rule(mirror_sw_if_index=0x00FFFFFE),
                self.permit_all(),
            ]
        )
        pkt = self.routed_pkt()
        fwd = self.expect_forward_only(pkt)
        self.assert_forwarded(fwd[0], pkt)

    def test_ipv4_five_tuple_qualifiers(self):
        """Src/dst IP, proto, and L4 ports each select the clone"""
        dst = str(self.dst_ip())
        src = str(self.ing.remote_ip4)
        miss_dst = str(self.fwd.remote_hosts[1].ip4)
        cases = [
            (
                "src ip",
                dict(src_prefix=IPv4Network(src + "/32")),
                self.routed_pkt(src=src),
                self.routed_pkt(src="198.51.100.1"),
            ),
            (
                "dst ip",
                dict(dst_prefix=IPv4Network(dst + "/32")),
                self.routed_pkt(dst=dst),
                self.routed_pkt(dst=miss_dst),
            ),
            (
                "udp proto",
                dict(proto=PROTO_UDP),
                self.routed_pkt(proto="udp"),
                self.routed_pkt(proto="tcp"),
            ),
            (
                "l4 src port",
                dict(
                    proto=PROTO_UDP,
                    sport_from=MATCH_SPORT,
                    sport_to=MATCH_SPORT,
                ),
                self.routed_pkt(sport=MATCH_SPORT),
                self.routed_pkt(sport=MATCH_SPORT + 1),
            ),
            (
                "l4 dst port",
                dict(
                    proto=PROTO_UDP,
                    dport_from=MATCH_DPORT,
                    dport_to=MATCH_DPORT,
                ),
                self.routed_pkt(dport=MATCH_DPORT),
                self.routed_pkt(dport=MATCH_DPORT + 1),
            ),
        ]
        for name, rule_kwargs, match, miss in cases:
            with self.subTest(qualifier=name):
                self.install_input_acl(
                    [self.mirror_rule(**rule_kwargs), self.permit_all()]
                )
                fwd, mir = self.expect_mirrored(match)
                self.assert_forwarded(fwd[0], match)
                self.assert_ingress_clone(mir[0], match)
                fwd = self.expect_forward_only(miss)
                self.assert_forwarded(fwd[0], miss)

    def test_in_port_scopes_the_rule(self):
        """in_sw_if_index (patch 0018) matches only the listed ingress port"""
        pkt = self.routed_pkt()
        self.install_input_acl(
            [
                self.mirror_rule(in_sw_if_index=self.ing.sw_if_index),
                self.permit_all(),
            ]
        )
        fwd, mir = self.expect_mirrored(pkt)
        self.assert_ingress_clone(mir[0], pkt)
        self.assert_forwarded(fwd[0], pkt)

        # Same packet, rule scoped to a different port: no clone.
        self.install_input_acl(
            [
                self.mirror_rule(in_sw_if_index=self.fwd.sw_if_index),
                self.permit_all(),
            ]
        )
        fwd = self.expect_forward_only(pkt)
        self.assert_forwarded(fwd[0], pkt)


class TestSonicExtEverflowDeferred(SonicExtEverflowBase):
    """ACL_MIRROR_F_DEFERRED on an ingress ACL, clone taken at egress."""

    def test_deferred_without_egress_node_does_not_clone(self):
        """Stamper claims the clone; with the egress node off, nothing is sent"""
        self.install_input_acl(
            [self.mirror_rule(deferred=True), self.permit_all()]
        )
        pkt = self.routed_pkt()
        fwd = self.expect_forward_only(pkt)
        self.assert_forwarded(fwd[0], pkt)

    def test_deferred_clone_is_post_rewrite(self):
        """After enabling sonic-ext-egress-mirror the clone matches egress L2"""
        self.enable_egress_mirror()
        self.install_input_acl(
            [self.mirror_rule(deferred=True), self.permit_all()]
        )
        pkt = self.routed_pkt()
        fwd, mir = self.expect_mirrored(pkt)
        self.assert_forwarded(fwd[0], pkt)
        self.assert_egress_clone(mir[0], pkt)


@unittest.skipIf("acl" in config.excluded_plugins, "Exclude acl plugin tests")
class TestAclPermitMirrorNoStamper(EverflowAclTopoBase):
    """Deferred flag with no sonic-ext plugin: ACL falls back to immediate clone."""

    def test_deferred_falls_back_to_immediate_clone(self):
        """No stamper registered: a deferred rule still clones in the ACL node"""
        self.install_input_acl(
            [self.mirror_rule(deferred=True), self.permit_all()]
        )
        pkt = self.routed_pkt()
        fwd, mir = self.expect_mirrored(pkt)
        self.assert_forwarded(fwd[0], pkt)
        self.assert_ingress_clone(mir[0], pkt)


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
