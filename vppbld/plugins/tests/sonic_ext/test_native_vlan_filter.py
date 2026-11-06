#!/usr/bin/env python3
"""Native-VLAN/QinQ regressions for VPP's test framework.

The platform's ``make ci-test`` discovers this module through EXTERN_TESTS.
For a focused run from vppbld/repo, use ``make test TEST=native_vlan_filter
EXTERN_TESTS="$(realpath ../plugins/tests)"``. The framework manages VPP,
API connections, CPU allocation and retained logs/PCAPs. Only
packet-generator interfaces and a simulated LCP host endpoint are used;
no kernel TAP or namespace is created.
"""

import sys

# Keep container validation from creating root-owned workspace bytecode.
sys.dont_write_bytecode = True

import os
from pathlib import Path
import time
import unittest

from asfframework import VppAsfTestCase, VppTestRunner

from scapy.layers.inet import IP, UDP
from scapy.layers.dhcp import BOOTP, DHCP
from scapy.layers.inet6 import IPv6, ICMPv6ND_NS
from scapy.layers.l2 import ARP, Dot1Q, Ether
from scapy.packet import Raw
from scapy.utils import rdpcap, wrpcap


NO_INDEX = 0xFFFFFFFF
VLAN = 200
ACCEPTED = "/err/sonic-ext-l2-vlan-filter/tagged frame VID matched access VLAN; tag popped and bridged"
BAD_VID = "/err/sonic-ext-l2-vlan-filter/tagged frame VID not configured on access member; dropped"


class NativeVlanFilterBase(VppAsfTestCase):
    """Framework-managed VPP with native and tagged VLAN members."""

    vpp_worker_count = 0
    pg_count = 3
    extra_vpp_plugin_config = [
        "plugin", "default", "{", "disable", "}",
        "plugin", "acl_plugin.so", "{", "enable", "}",
        "plugin", "linux_cp_plugin.so", "{", "enable", "}",
        "plugin", "sonic_ext_plugin.so", "{", "enable", "}",
    ]

    @classmethod
    def setUpClass(cls):
        # run_tests.py assigns suite CPUs. Also support a direct invocation
        # with VppTestRunner, which does not pass through that scheduler.
        if not cls.cpus:
            cls.assign_cores([[min(os.sched_getaffinity(0))]])
        super().setUpClass()

    def setUp(self):
        super().setUp()
        self.api = self.vapi.papi
        self.stats = self.statistics
        self.path = Path(self.tempdir) / self._testMethodName
        self.path.mkdir(exist_ok=True)
        self.sequence = 0
        self.cli("set acl-plugin use-hash-acl-matching 1")
        self.addCleanup(self.cli, "set acl-plugin use-hash-acl-matching 1")
        self.rpc("acl_stats_intf_counters_enable", enable=1)
        self.addCleanup(self.rpc, "acl_stats_intf_counters_enable", enable=0)
        self.pgs = []
        for i in range(self.pg_count):
            pg = self.rpc("pg_create_interface", interface_id=i).sw_if_index
            self.addCleanup(self.rpc, "pg_delete_interface", sw_if_index=pg)
            self.rpc("sw_interface_set_flags", sw_if_index=pg, flags=1)
            self.pgs.append(pg)

        # pg0/pg2 are native members; pg1.<VID> is the tagged member.
        self.child = self.rpc("create_subif", sw_if_index=self.pgs[1], sub_id=VLAN,
                              outer_vlan_id=VLAN, sub_if_flags=2).sw_if_index
        self.addCleanup(self.rpc, "delete_subif", sw_if_index=self.child)
        self.rpc("sw_interface_set_flags", sw_if_index=self.child, flags=1)
        self.rpc("bridge_domain_add_del_v2", bd_id=VLAN, is_add=1,
                 flood=1, uu_flood=1, learn=0, forward=1)
        self.addCleanup(self.rpc, "bridge_domain_add_del_v2", bd_id=VLAN, is_add=0)
        for interface in (self.pgs[0], self.child, self.pgs[2]):
            self.rpc("sw_interface_set_l2_bridge", rx_sw_if_index=interface,
                     bd_id=VLAN, enable=1)
            self.addCleanup(self.rpc, "sw_interface_set_l2_bridge",
                            rx_sw_if_index=interface, bd_id=VLAN, enable=0)
        self.rpc("l2_interface_vlan_tag_rewrite", sw_if_index=self.child,
                 vtr_op=3, push_dot1q=1)

        next_index = self.rpc("add_node_next", node_name="l2-input-classify",
                              next_name="sonic-ext-l2-vlan-filter").next_index
        mask = bytearray(16)
        mask[12:14] = b"\xff\xff"
        self.native_table = self.rpc("classify_add_del_table", is_add=1, nbuckets=8,
                         memory_size=4096, skip_n_vectors=0, match_n_vectors=1,
                         next_table_index=NO_INDEX, miss_next_index=NO_INDEX,
                         mask=bytes(mask), mask_len=16).new_table_index
        self.addCleanup(self.delete_table, self.native_table)
        match = bytearray(16)
        match[12:14] = b"\x81\x00"
        self.rpc("classify_add_del_session", is_add=1, table_index=self.native_table,
                 hit_next_index=next_index, match=bytes(match), match_len=16)
        for interface in (self.pgs[0], self.pgs[2]):
            self.set_classifiers(interface, other=self.native_table)

    def set_classifiers(self, interface, ip4=NO_INDEX, ip6=NO_INDEX, other=NO_INDEX):
        self.rpc("classify_set_interface_l2_tables", sw_if_index=interface,
                 ip4_table_index=ip4, ip6_table_index=ip6,
                 other_table_index=other, is_input=1)
        self.addCleanup(self.rpc, "classify_set_interface_l2_tables",
                        sw_if_index=interface, ip4_table_index=NO_INDEX,
                        ip6_table_index=NO_INDEX, other_table_index=NO_INDEX,
                        is_input=1)

    def delete_table(self, table):
        self.rpc("classify_add_del_table", is_add=0, table_index=table,
                 match_n_vectors=0, mask=b"", mask_len=0)

    def rpc(self, name, **kwargs):
        reply = getattr(self.api, name)(**kwargs)
        self.assertEqual(reply.retval, 0, (name, kwargs, reply))
        return reply

    def cli(self, command):
        return self.rpc("cli_inband", cmd=command).reply

    @staticmethod
    def packet(tags=(), ipv6=False):
        pkt = Ether(src="02:00:00:00:10:01", dst="02:00:00:00:10:02")
        for tag in tags:
            pkt /= Dot1Q(vlan=tag)
        pkt /= (IPv6(src="2001:db8:1::2", dst="2001:db8:2::2") if ipv6 else
                IP(src="192.0.2.2", dst="198.51.100.2"))
        return pkt / UDP(sport=12345, dport=23456) / Raw(b"native VLAN QinQ regression" * 2)

    @staticmethod
    def rule(vlan=0, action=1, ipv6=False):
        return dict(is_permit=action, src_prefix="::/0" if ipv6 else "0.0.0.0/0",
                    dst_prefix="::/0" if ipv6 else "0.0.0.0/0", proto=0,
                    srcport_or_icmptype_last=65535, dstport_or_icmpcode_last=65535,
                    outer_vlan_id=vlan, outer_vlan_id_mask=0xFFF if vlan else 0)

    def bind(self, interface, rules):
        acl = self.rpc("acl_add_replace", acl_index=NO_INDEX, count=len(rules),
                       r=rules, tag="native-vlan-regression").acl_index
        self.addCleanup(self.rpc, "acl_del", acl_index=acl)
        self.rpc("acl_interface_set_acl_list", sw_if_index=interface,
                 n_input=1, count=1, acls=[acl])
        self.addCleanup(self.rpc, "acl_interface_set_acl_list",
                        sw_if_index=interface, n_input=0, count=0, acls=[])
        return acl

    def acl_counter(self, acl, rule=0):
        return self.stats[f"/acl/{acl}/matches"][:, rule].sum_packets()

    def error_counter(self, name):
        return self.stats[name].sum()

    def transmit_captures(self, packet, expected, source_pg=0):
        """Capture every named output, including outputs expected to be empty."""
        self.sequence += 1
        source = self.path / f"input{self.sequence}.pcap"
        targets = {pg: self.path / f"output{self.sequence}-pg{pg}.pcap"
                   for pg in expected}
        wrpcap(str(source), [packet])
        for pg, target in targets.items():
            self.cli(f"packet-generator capture pg{pg} pcap {target}")
        self.cli(f"packet-generator new pcap {source} source pg{source_pg} "
                 f"name stream{self.sequence}")
        self.addCleanup(self.cli, f"packet-generator delete stream{self.sequence}")
        self.cli(f"packet-generator enable-stream stream{self.sequence}")
        for _ in range(100):
            if "Yes" not in self.cli("show packet-generator"):
                break
            time.sleep(0.01)
        captures = {}
        for pg, target in targets.items():
            self.cli(f"packet-generator capture pg{pg} pcap {target} disable")
            captures[pg] = (rdpcap(str(target)) if target.exists() and
                            target.stat().st_size > 24 else [])
            self.assertEqual(len(captures[pg]), expected[pg],
                             f"pg{pg}: " + self.cli("show errors"))
        return captures

    def transmit(self, packet, source_pg=0, target_pg=1, expect=1):
        return self.transmit_captures(packet, {target_pg: expect}, source_pg)[target_pg]


class TestNativeVlanFilter(NativeVlanFilterBase):
    """Native VLAN admission, QinQ preservation and ACL enforcement."""

    def test_native_qinq_preserved_and_acl_runs_once(self):
        """Native QinQ validates only service VID, preserving different/same inner VID."""
        for ipv6 in (False, True):
            for hashed in (1, 0):
                self.cli(f"set acl-plugin use-hash-acl-matching {hashed}")
                acl = self.bind(self.pgs[0], [self.rule(10, action=0, ipv6=ipv6),
                                            self.rule(VLAN, ipv6=ipv6),
                                            self.rule(action=0, ipv6=ipv6)])
                for inner in (10, VLAN):
                    before = self.error_counter(ACCEPTED)
                    packet = self.packet((VLAN, inner), ipv6)
                    received = self.transmit(packet)[0]
                    self.assertEqual(bytes(received), bytes(packet))
                    self.assertEqual(self.error_counter(ACCEPTED) - before, 1)
                self.assertEqual(self.acl_counter(acl, 0), 0)
                self.assertEqual(self.acl_counter(acl, 1), 2)
                self.assertEqual(self.acl_counter(acl, 2), 0)

    def test_native_untagged_and_single_tag(self):
        """Existing native untagged/single-tag behavior and one-pass ACL counters."""
        for ipv6 in (False, True):
            acl = self.bind(self.pgs[0], [self.rule(VLAN, ipv6=ipv6)])
            before = self.error_counter(ACCEPTED)
            received = self.transmit(self.packet(ipv6=ipv6))[0]
            self.assertEqual(bytes(received), bytes(self.packet((VLAN,), ipv6)))
            self.assertEqual(self.error_counter(ACCEPTED), before)
            packet = self.packet((VLAN,), ipv6)
            self.assertEqual(bytes(self.transmit(packet)[0]), bytes(packet))
            self.assertEqual(self.error_counter(ACCEPTED) - before, 1)
            self.assertEqual(self.acl_counter(acl), 2)

    def test_wrong_outer_vlan_drops_before_acl(self):
        """An inner access VID cannot override a wrong outer VID."""
        for ipv6 in (False, True):
            acl = self.bind(self.pgs[0], [self.rule(ipv6=ipv6)])
            for tags in ((300,), (300, VLAN)):
                before = self.error_counter(BAD_VID)
                self.transmit(self.packet(tags, ipv6), expect=0)
                # The node and error-drop both account its existing drop
                # counter. Packet absence and zero ACL hits prove filtering.
                self.assertGreater(self.error_counter(BAD_VID), before)
            self.assertEqual(self.acl_counter(acl), 0)

    def test_native_qinq_acl_drop(self):
        """Continuing after classify still enforces a service-VID DROP rule."""
        for ipv6 in (False, True):
            acl = self.bind(self.pgs[0], [self.rule(VLAN, action=0, ipv6=ipv6)])
            before = self.error_counter(ACCEPTED)
            self.transmit(self.packet((VLAN, 10), ipv6), expect=0)
            self.assertEqual(self.error_counter(ACCEPTED) - before, 1)
            self.assertEqual(self.acl_counter(acl), 1)

    def test_tagged_member_qinq_and_single_tag(self):
        """Tagged members keep VTR behavior, with no native filter invocation."""
        for ipv6 in (False, True):
            acl = self.bind(self.child, [self.rule(VLAN, ipv6=ipv6)])
            before = self.error_counter(ACCEPTED)
            for tags in ((VLAN,), (VLAN, 10)):
                received = self.transmit(self.packet(tags, ipv6),
                                         source_pg=1, target_pg=2)[0]
                self.assertEqual(bytes(received), bytes(self.packet(tags[1:], ipv6)))
            self.assertEqual(self.error_counter(ACCEPTED), before)
            self.assertEqual(self.acl_counter(acl), 2)


class NativeVlanControlBase(NativeVlanFilterBase):
    """Use an LCP pg host endpoint; no kernel TAP or namespace is needed."""

    extra_vpp_plugin_config = NativeVlanFilterBase.extra_vpp_plugin_config + [
        "plugin", "linux_cp_unittest_plugin.so", "{", "enable", "}",
    ]
    pg_count = 4

    def setUp(self):
        super().setUp()
        self.cli("test lcp add phy pg0 host pg3")
        self.addCleanup(self.cli, "test lcp del phy pg0 host pg3")
        self.punt_next = self.rpc("add_node_next", node_name="l2-input-classify",
                                  next_name="linux-cp-punt").next_index
        bcast = {i: 0xFF for i in range(6)}
        self.dhcp_table = self.classifier(
            48, {**bcast, 12: 0xFF, 13: 0xFF, 23: 0xFF, 36: 0xFF, 37: 0xFF},
            {**bcast, 12: 0x08, 13: 0x00, 23: 17, 36: 0, 37: 67})
        self.arp_table = self.classifier(
            16, {**bcast, 12: 0xFF, 13: 0xFF},
            {**bcast, 12: 0x08, 13: 0x06}, next_table=self.native_table)
        self.ip6mc_table = self.classifier(16, {0: 0xFF, 1: 0xFF},
                                          {0: 0x33, 1: 0x33})
        self.control_classifiers()

    def classifier(self, size, mask_bytes, match_bytes, next_table=NO_INDEX):
        # The same masks/slots used by SwitchVppFdb.cpp's native-member
        # DHCP trap and optional ARP/IPv6 multicast flood suppression.
        mask, match = bytearray(size), bytearray(size)
        for offset, value in mask_bytes.items():
            mask[offset] = value
        for offset, value in match_bytes.items():
            match[offset] = value
        table = self.rpc("classify_add_del_table", is_add=1, nbuckets=8,
                         memory_size=4096, skip_n_vectors=0,
                         match_n_vectors=size // 16, next_table_index=next_table,
                         miss_next_index=NO_INDEX, mask=bytes(mask),
                         mask_len=size).new_table_index
        self.addCleanup(self.delete_table, table)
        self.rpc("classify_add_del_session", is_add=1, table_index=table,
                 hit_next_index=self.punt_next, match=bytes(match), match_len=size)
        return table

    def control_classifiers(self, arp=True, ip6mc=True):
        self.set_classifiers(self.pgs[0], ip4=self.dhcp_table,
                             ip6=self.ip6mc_table if ip6mc else NO_INDEX,
                             other=self.arp_table if arp else self.native_table)

    @staticmethod
    def control_packet(kind, tagged):
        eth = Ether(src="02:00:00:00:10:01", dst="ff:ff:ff:ff:ff:ff")
        if tagged:
            eth /= Dot1Q(vlan=VLAN)
        if kind == "dhcp":
            return (eth / IP(src="0.0.0.0", dst="255.255.255.255") /
                    UDP(sport=68, dport=67) / BOOTP(chaddr=b"\x02\x00\x00\x00\x10\x01") /
                    DHCP(options=[("message-type", "discover"), "end"]))
        if kind == "arp":
            return (eth / ARP(op=1, hwsrc=eth.src, psrc="192.0.2.2", pdst="192.0.2.1") /
                    Raw(b"\x00" * 18))
        eth.dst = "33:33:ff:00:00:01"
        return (eth / IPv6(src="2001:db8::2", dst="ff02::1:ff00:1", hlim=255) /
                ICMPv6ND_NS(tgt="2001:db8::1"))

    def assert_trapped(self, kind):
        acl = self.bind(self.pgs[0], [self.rule(VLAN, ipv6=kind == "ip6mc")])
        for tagged in (False, True):
            before = self.error_counter(ACCEPTED)
            received = self.transmit_captures(self.control_packet(kind, tagged),
                                              {1: 0, 2: 0, 3: 1})[3][0]
            self.assertEqual(bytes(received), bytes(self.control_packet(kind, False)))
            self.assertEqual(self.error_counter(ACCEPTED) - before, int(tagged))
        # These terminal classifiers precede the ACL, just as they do for
        # an originally untagged frame. No peer member gets a flood copy.
        self.assertEqual(self.acl_counter(acl), 0)


class TestNativeVlanControl(NativeVlanControlBase):
    """Native control-packet trapping and configured flood suppression."""

    def test_native_single_tag_dhcp_trap_without_flood(self):
        """POP exposes DHCP to its IP4 classifier and the parent's LCP host."""
        self.assert_trapped("dhcp")
        # Ordinary single-tag IPv4 still reaches ACL and bridge forwarding.
        acl = self.bind(self.pgs[0], [self.rule(VLAN)])
        packet = self.packet((VLAN,))
        received = self.transmit_captures(packet, {1: 1, 3: 0})[1][0]
        self.assertEqual(bytes(received), bytes(packet))
        self.assertEqual(self.acl_counter(acl), 1)

    def test_native_single_tag_arp_flood_suppression(self):
        """Configured broadcast-ARP suppression applies after the access POP."""
        self.assert_trapped("arp")
        self.control_classifiers(arp=False)
        captures = self.transmit_captures(self.control_packet("arp", True),
                                           {1: 1, 2: 1, 3: 0})
        self.assertEqual(bytes(captures[1][0]), bytes(self.control_packet("arp", True)))
        self.assertEqual(bytes(captures[2][0]), bytes(self.control_packet("arp", False)))

    def test_native_single_tag_ipv6_multicast_flood_suppression(self):
        """Configured IPv6 multicast suppression selects the IP6 slot after POP."""
        self.assert_trapped("ip6mc")
        self.control_classifiers(ip6mc=False)
        captures = self.transmit_captures(self.control_packet("ip6mc", True),
                                           {1: 1, 2: 1, 3: 0})
        self.assertEqual(bytes(captures[1][0]), bytes(self.control_packet("ip6mc", True)))
        self.assertEqual(bytes(captures[2][0]), bytes(self.control_packet("ip6mc", False)))


class TestNativeVlanCopp(NativeVlanControlBase):
    """Native ARP trapping through the simulated LCP host CoPP path."""

    extra_vpp_plugin_config = NativeVlanControlBase.extra_vpp_plugin_config + [
        "plugin", "policer_plugin.so", "{", "enable", "}",
    ]

    def test_native_single_tag_arp_reaches_host_copp(self):
        """The reclassified ARP punt traverses the host output CoPP feature."""
        for action, host_count in (("transmit", 1), ("drop", 0)):
            name = f"native-arp-{action}"
            self.cli(f"policer add name {name} type 1r2c cir 1000000 cb 1000000 "
                     f"rate kbps conform-action {action} exceed-action {action} "
                     f"violate-action {action}")
            self.addCleanup(self.cli, f"policer del name {name}")
            self.rpc("sonic_ext_copp_ifout_bind", ethertype=0x0806,
                     policer_name=name, is_bind=True)
            self.addCleanup(self.rpc, "sonic_ext_copp_ifout_bind", ethertype=0x0806,
                            policer_name=name, is_bind=False)
            before = self.rpc("sonic_ext_copp_ifout_get_counters", ethertype=0x0806)
            captures = self.transmit_captures(self.control_packet("arp", True),
                                               {1: 0, 2: 0, 3: host_count})
            after = self.rpc("sonic_ext_copp_ifout_get_counters", ethertype=0x0806)
            self.assertEqual(after.conform_packets - before.conform_packets, 1)
            self.assertEqual(after.exceed_packets - before.exceed_packets, 0)
            self.assertEqual(after.violate_packets - before.violate_packets, 0)
            if host_count:
                self.assertEqual(bytes(captures[3][0]),
                                 bytes(self.control_packet("arp", False)))


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
