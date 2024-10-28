#!/usr/bin/env python3

import unittest
from framework import VppTestCase
from asfframework import VppTestRunner

from scapy.layers.l2 import Ether, Dot1Q
from scapy.packet import Raw
from scapy.layers.inet import IP, UDP
from scapy.layers.inet6 import IPv6
from scapy.layers.vxlan import VXLAN

from vpp_ip_route import VppRoutePath
from vpp_vxlan_tunnel import VppVxlanTunnel
from vpp_l2 import L2_PORT_TYPE

###########################
# Test Configuration variables
###########################

# Scale
# Min 2 each if negative testing (or 1 each)
# Max around 100 ingress (as x2 for vxlan and test infra doesn't support sw_if_index > 255)
NUM_INGRESS_PGS = 2
NUM_EGRESS_PGS = 8

# Debug
# Set to True to run tests individually (slow)
# Generates test teardown traces (including packet captures) for each test
RUN_TESTS_INDIVIDUALLY = False


class TestTuntermAcl(VppTestCase):
    """Tunnel Termination ACL Test Case"""

    def __init__(self, *args):
        VppTestCase.__init__(self, *args)

    @classmethod
    def configTuntermACL(cls, port=4789):
        cls.dport = port

        cls.single_tunnel_vni = 0x12345
        cls.single_tunnel_bd = 1

        # Create VXLAN tunnels on ingress interfaces
        for i in range(NUM_INGRESS_PGS):
            ingress_pg = cls.pg_interfaces[NUM_EGRESS_PGS + i]
            r = VppVxlanTunnel(
                cls,
                src=ingress_pg.local_ip4,
                dst=ingress_pg.remote_ip4,
                src_port=cls.dport,
                dst_port=cls.dport,
                vni=cls.single_tunnel_vni,
                is_l3=True,
            )
            r.add_vpp_config()
            cls.vapi.sw_interface_set_l2_bridge(
                rx_sw_if_index=r.sw_if_index, bd_id=cls.single_tunnel_bd
            )

        # Create loopback/BVI interface
        cls.create_loopback_interfaces(1)
        cls.loop0 = cls.lo_interfaces[0]
        cls.loop0.admin_up()
        cls.vapi.sw_interface_set_mac_address(
            cls.loop0.sw_if_index, "00:00:00:00:00:02"
        )
        cls.loop0.config_ip4()
        cls.vapi.sw_interface_set_l2_bridge(
            rx_sw_if_index=cls.loop0.sw_if_index,
            bd_id=cls.single_tunnel_bd,
            port_type=L2_PORT_TYPE.BVI,
        )

        # Enable v6 on BVI interface (for negative test to forward based on inner dst IP)
        cls.vapi.sw_interface_ip6_enable_disable(cls.loop0.sw_if_index, enable=1)

        # Add default ACL permit per sonic-vpp use-case
        from vpp_acl import AclRule, VppAcl, VppAclInterface
        from ipaddress import IPv6Network

        rule_1 = AclRule(is_permit=1)
        rule_2 = AclRule(is_permit=1)
        rule_2.src_prefix = IPv6Network("0::0/0")
        rule_2.dst_prefix = IPv6Network("0::0/0")
        acl_1 = VppAcl(cls, rules=[rule_1, rule_2])
        acl_1.add_vpp_config()

        for i in range(NUM_INGRESS_PGS):
            ingress_pg = cls.pg_interfaces[NUM_EGRESS_PGS + i]
            acl_if = VppAclInterface(
                cls, sw_if_index=ingress_pg.sw_if_index, n_input=1, acls=[acl_1]
            )
            acl_if.add_vpp_config()

        # TunTerm plugin Call
        DST_IPs_v4 = [f"4.3.2.{i}" for i in range(NUM_EGRESS_PGS)]
        DST_IPs_v6 = [f"2001:db8::{i+1}" for i in range(NUM_EGRESS_PGS)]
        paths_v4 = [
            VppRoutePath(
                cls.pg_interfaces[i].remote_ip4, cls.pg_interfaces[i].sw_if_index
            ).encode()
            for i in range(NUM_EGRESS_PGS)
        ]
        paths_v6 = [
            VppRoutePath(
                cls.pg_interfaces[i].remote_ip6, cls.pg_interfaces[i].sw_if_index
            ).encode()
            for i in range(NUM_EGRESS_PGS)
        ]

        cls.rules_v4 = [
            {"dst": dst_ip, "path": path} for dst_ip, path in zip(DST_IPs_v4, paths_v4)
        ]
        cls.rules_v6 = [
            {"dst": dst_ip, "path": path} for dst_ip, path in zip(DST_IPs_v6, paths_v6)
        ]
        reply_v4 = cls.vapi.tunterm_acl_add_replace(
            0xFFFFFFFF, False, len(cls.rules_v4), cls.rules_v4
        )
        reply_v6 = cls.vapi.tunterm_acl_add_replace(
            0xFFFFFFFF, True, len(cls.rules_v6), cls.rules_v6
        )
        cls.tunterm_acl_index_v4 = reply_v4.tunterm_acl_index
        cls.tunterm_acl_index_v6 = reply_v6.tunterm_acl_index

        cls.vapi.tunterm_acl_add_replace(
            cls.tunterm_acl_index_v4, False, len(cls.rules_v4), cls.rules_v4
        )
        cls.vapi.tunterm_acl_add_replace(
            cls.tunterm_acl_index_v6, True, len(cls.rules_v6), cls.rules_v6
        )

        for i in range(NUM_INGRESS_PGS):
            ingress_pg = cls.pg_interfaces[NUM_EGRESS_PGS + i]
            cls.vapi.tunterm_acl_interface_add_del(
                True, ingress_pg.sw_if_index, cls.tunterm_acl_index_v4
            )
            cls.vapi.tunterm_acl_interface_add_del(
                True, ingress_pg.sw_if_index, cls.tunterm_acl_index_v6
            )

    @classmethod
    def setUpClass(cls):
        super(TestTuntermAcl, cls).setUpClass()

        try:
            cls.flags = 0x8

            # Create pg interfaces.
            cls.create_pg_interfaces(range(NUM_EGRESS_PGS + NUM_INGRESS_PGS))
            for pg in cls.pg_interfaces:
                pg.admin_up()
                pg.config_ip4()
                pg.config_ip6()
                pg.resolve_arp()
                pg.resolve_ndp()

            cls.configTuntermACL()

        except Exception:
            cls.tearDownClass()
            raise

    @classmethod
    def tearDownClass(cls):
        for i in range(NUM_INGRESS_PGS):
            ingress_pg = cls.pg_interfaces[NUM_EGRESS_PGS + i]
            cls.vapi.tunterm_acl_interface_add_del(
                False, ingress_pg.sw_if_index, cls.tunterm_acl_index_v4
            )
            cls.vapi.tunterm_acl_interface_add_del(
                False, ingress_pg.sw_if_index, cls.tunterm_acl_index_v6
            )

        cls.vapi.tunterm_acl_del(cls.tunterm_acl_index_v4)
        cls.vapi.tunterm_acl_del(cls.tunterm_acl_index_v6)

        super(TestTuntermAcl, cls).tearDownClass()

    def setUp(self):
        super(TestTuntermAcl, self).setUp()

    def tearDown(self):
        super(TestTuntermAcl, self).tearDown()

    def show_commands_at_teardown(self):
        self.logger.info(self.vapi.cli("show bridge-domain 1 detail"))
        self.logger.info(self.vapi.cli("show vxlan tunnel"))
        self.logger.info(self.vapi.cli("show node counters"))
        self.logger.info(self.vapi.cli("show classify table verbose"))
        self.logger.info(self.vapi.cli("show tunterm interfaces"))
        self.logger.info(self.vapi.cli("show acl-plugin acl"))
        self.logger.info(self.vapi.cli("show acl-plugin tables"))

    def encapsulate(self, pkt, vni, src_mac, dst_mac, src_ip, dst_ip):
        return (
            Ether(src=src_mac, dst=dst_mac)
            / IP(src=src_ip, dst=dst_ip)
            / UDP(sport=self.dport, dport=self.dport, chksum=0)
            / VXLAN(vni=vni, flags=self.flags)
            / pkt
        )

    def create_frame_request(
        self, src_mac, dst_mac, src_ip, dst_ip, is_ipv6=False, add_vlan=False
    ):
        ether_layer = Ether(src=src_mac, dst=dst_mac)
        if add_vlan:
            ether_layer /= Dot1Q(vlan=100)
        return (
            ether_layer
            / (IPv6(src=src_ip, dst=dst_ip) if is_ipv6 else IP(src=src_ip, dst=dst_ip))
            / UDP(sport=10000, dport=20000)
            / Raw("\xa5" * 100)
        )

    def assert_eq_pkts(self, pkt1, pkt2):
        """Verify the Ether, IP, UDP, payload are equal in both packets"""
        self.assertEqual(pkt1[Ether].src, pkt2[Ether].src)
        self.assertEqual(pkt1[Ether].dst, pkt2[Ether].dst)
        if IP in pkt1 or IP in pkt2:
            self.assertEqual(pkt1[IP].src, pkt2[IP].src)
            self.assertEqual(pkt1[IP].dst, pkt2[IP].dst)
        elif IPv6 in pkt1 or IPv6 in pkt2:
            self.assertEqual(pkt1[IPv6].src, pkt2[IPv6].src)
            self.assertEqual(pkt1[IPv6].dst, pkt2[IPv6].dst)
        self.assertEqual(pkt1[UDP].sport, pkt2[UDP].sport)
        self.assertEqual(pkt1[UDP].dport, pkt2[UDP].dport)
        self.assertEqual(pkt1[Raw], pkt2[Raw])

    def verify_packet_forwarding(self, out_pg, frame_request):
        out = out_pg.get_capture(1)
        pkt = out[0]

        frame_forwarded = frame_request.copy()
        frame_forwarded[Ether].src = out_pg.local_mac
        frame_forwarded[Ether].dst = out_pg.remote_mac

        self.assert_eq_pkts(pkt, frame_forwarded)

    def _test_decap(self, in_pg, out_pg, input_frame, is_v6=False):
        encapsulated_pkt = self.encapsulate(
            input_frame,
            self.single_tunnel_vni,
            in_pg.remote_mac,
            in_pg.local_mac,
            in_pg.remote_ip4,
            in_pg.local_ip4,
        )

        in_pg.add_stream([encapsulated_pkt])
        out_pg.enable_capture()
        self.pg_start()

        self.verify_packet_forwarding(out_pg, input_frame)

        self.logger.info(
            "test_tunterm: Passed %s with input intf #%d (%s) and output %s"
            % (
                "v6" if is_v6 else "v4",
                in_pg.pg_index - NUM_EGRESS_PGS + 1,
                in_pg.name,
                out_pg.name,
            )
        )

    def _test_add_remove(self, pg):
        reply_v4 = self.vapi.tunterm_acl_add_replace(
            0xFFFFFFFF, False, 1, [self.rules_v4[0]]
        )
        reply_v6 = self.vapi.tunterm_acl_add_replace(
            0xFFFFFFFF, True, 1, [self.rules_v6[0]]
        )
        tunterm_acl_index_v4 = reply_v4.tunterm_acl_index
        tunterm_acl_index_v6 = reply_v6.tunterm_acl_index

        self.vapi.tunterm_acl_interface_add_del(
            True, pg.sw_if_index, tunterm_acl_index_v4
        )
        self.vapi.tunterm_acl_interface_add_del(
            True, pg.sw_if_index, tunterm_acl_index_v6
        )

        # Shouldn't be able to delete the tunterm entry as it's still in use
        with self.assertRaises(Exception):
            self.vapi.tunterm_acl_del(tunterm_acl_index_v4)
        with self.assertRaises(Exception):
            self.vapi.tunterm_acl_del(tunterm_acl_index_v6)

        # Shouldn't be able to switch table AF during replace
        with self.assertRaises(Exception):
            self.vapi.tunterm_acl_add_del(
                tunterm_acl_index_v4, True, 1, [self.rules_v6[0]]
            )
        with self.assertRaises(Exception):
            self.vapi.tunterm_acl_add_del(
                tunterm_acl_index_v6, False, 1, [self.rules_v4[0]]
            )

        # Test a proper replace
        if len(self.rules_v4) > 1:
            reply = self.vapi.tunterm_acl_add_replace(
                tunterm_acl_index_v4, False, 1, [self.rules_v4[1]]
            )
            tunterm_acl_index_v4_2 = reply.tunterm_acl_index
            self.assertEqual(tunterm_acl_index_v4, tunterm_acl_index_v4_2)
        if len(self.rules_v6) > 1:
            reply = self.vapi.tunterm_acl_add_replace(
                tunterm_acl_index_v6, True, 1, [self.rules_v6[1]]
            )
            tunterm_acl_index_v6_2 = reply.tunterm_acl_index
            self.assertEqual(tunterm_acl_index_v6, tunterm_acl_index_v6_2)

        self.vapi.tunterm_acl_interface_add_del(
            False, pg.sw_if_index, tunterm_acl_index_v4
        )
        self.vapi.tunterm_acl_interface_add_del(
            False, pg.sw_if_index, tunterm_acl_index_v6
        )

        self.vapi.tunterm_acl_del(tunterm_acl_index_v4)
        self.vapi.tunterm_acl_del(tunterm_acl_index_v6)

        # Shouldn't be able to detach the tunterm as it's already been detached
        with self.assertRaises(Exception):
            self.vapi.tunterm_acl_interface_add_del(
                False, pg.sw_if_index, tunterm_acl_index_v4
            )
        with self.assertRaises(Exception):
            self.vapi.tunterm_acl_interface_add_del(
                False, pg.sw_if_index, tunterm_acl_index_v6
            )

        # Test empty acls
        reply_v4 = self.vapi.tunterm_acl_add_replace(0xFFFFFFFF, False, 0, [])
        reply_v4 = self.vapi.tunterm_acl_add_replace(
            reply_v4.tunterm_acl_index, False, 0, []
        )
        reply_v6 = self.vapi.tunterm_acl_add_replace(0xFFFFFFFF, True, 0, [])
        reply_v6 = self.vapi.tunterm_acl_add_replace(
            reply_v6.tunterm_acl_index, True, 0, []
        )
        self.vapi.tunterm_acl_del(reply_v4.tunterm_acl_index)
        self.vapi.tunterm_acl_del(reply_v6.tunterm_acl_index)

    def _run_decap_test(
        self, ingress_index, egress_index, is_ipv6=False, add_vlan=False
    ):
        self.remove_configured_vpp_objects_on_tear_down = False
        out_pg = self.pg_interfaces[egress_index]
        in_pg = self.pg_interfaces[NUM_EGRESS_PGS + ingress_index]

        src_ip = "2001:db9::1" if is_ipv6 else "1.2.3.4"
        dst_ip = f"2001:db8::{egress_index + 1}" if is_ipv6 else f"4.3.2.{egress_index}"

        frame_request = self.create_frame_request(
            "00:00:00:00:00:01", "00:00:00:00:00:02", src_ip, dst_ip, is_ipv6, add_vlan
        )

        self._test_add_remove(out_pg)  # exercise the add/remove (no-op)
        self._test_decap(in_pg, out_pg, frame_request, is_ipv6)

    #################
    # Test Creator
    # - For every ingress and egress pair, create a decap/redirect test
    #################
    @classmethod
    def add_dynamic_tests(cls):
        if RUN_TESTS_INDIVIDUALLY:
            for ingress_index in range(NUM_INGRESS_PGS):
                for egress_index in range(NUM_EGRESS_PGS):

                    def test_v4(
                        self, ingress_index=ingress_index, egress_index=egress_index
                    ):
                        self._run_decap_test(ingress_index, egress_index, is_ipv6=False)

                    def test_v6(
                        self, ingress_index=ingress_index, egress_index=egress_index
                    ):
                        self._run_decap_test(ingress_index, egress_index, is_ipv6=True)

                    setattr(
                        cls,
                        f"test_decap_v4_ingress_{ingress_index}_egress_{egress_index}",
                        test_v4,
                    )
                    setattr(
                        cls,
                        f"test_decap_v6_ingress_{ingress_index}_egress_{egress_index}",
                        test_v6,
                    )
        else:

            def test_v4(self):
                for ingress_index in range(NUM_INGRESS_PGS):
                    for egress_index in range(NUM_EGRESS_PGS):
                        self._run_decap_test(ingress_index, egress_index, is_ipv6=False)

            def test_v6(self):
                for ingress_index in range(NUM_INGRESS_PGS):
                    for egress_index in range(NUM_EGRESS_PGS):
                        self._run_decap_test(ingress_index, egress_index, is_ipv6=True)

            setattr(cls, "test_decap_v4", test_v4)
            setattr(cls, "test_decap_v6", test_v6)

    def test_v4_inner_vlan(self):
        """Test for inner vlan tag (v4)"""
        for ingress_index in range(NUM_INGRESS_PGS):
            for egress_index in range(NUM_EGRESS_PGS):
                self._run_decap_test(
                    ingress_index, egress_index, is_ipv6=False, add_vlan=True
                )

    def test_v6_inner_vlan(self):
        """Test for inner vlan tag (v6)"""
        for ingress_index in range(NUM_INGRESS_PGS):
            for egress_index in range(NUM_EGRESS_PGS):
                self._run_decap_test(
                    ingress_index, egress_index, is_ipv6=True, add_vlan=True
                )

    #################
    # Negative Tests
    # - Decap with unmatched inner DST IP (v4/v6)
    # - Normal IP forwarding without encapsulation (v4/v6)
    # - VXLAN packet without decap
    #################

    def _test_negative_decap(self, is_ipv6=False):
        out_pg = self.pg_interfaces[0]
        in_pg = self.pg_interfaces[NUM_EGRESS_PGS]

        frame_request = self.create_frame_request(
            "00:00:00:00:00:01",
            "00:00:00:00:00:02",
            "2001:db9::1" if is_ipv6 else "1.2.3.4",
            out_pg.remote_ip6 if is_ipv6 else out_pg.remote_ip4,
            is_ipv6,
        )

        encapsulated_pkt = self.encapsulate(
            frame_request,
            self.single_tunnel_vni,
            in_pg.remote_mac,
            in_pg.local_mac,
            in_pg.remote_ip4,
            in_pg.local_ip4,
        )

        in_pg.add_stream([encapsulated_pkt])
        out_pg.enable_capture()
        self.pg_start()

        self.verify_packet_forwarding(out_pg, frame_request)
        self.logger.info(f"test_negative_decap_v{'6' if is_ipv6 else '4'}: Passed")

    def _test_negative_normal_ip(self, is_ipv6=False):
        for src_pg in [
            self.pg_interfaces[NUM_EGRESS_PGS + i] for i in range(NUM_INGRESS_PGS)
        ]:
            for dst_pg in self.pg_interfaces[:NUM_EGRESS_PGS]:
                frame_request = self.create_frame_request(
                    src_pg.remote_mac,
                    src_pg.local_mac,
                    src_pg.remote_ip6 if is_ipv6 else src_pg.remote_ip4,
                    dst_pg.remote_ip6 if is_ipv6 else dst_pg.remote_ip4,
                    is_ipv6,
                )

                src_pg.add_stream([frame_request])
                dst_pg.enable_capture()
                self.pg_start()

                self.verify_packet_forwarding(dst_pg, frame_request)
                self.logger.info(
                    f"test_negative_normal_ip{'6' if is_ipv6 else '4'}: Passed with {src_pg.name} to {dst_pg.name}"
                )

    def test_negative_decap_v4(self):
        """Negative test for VXLAN decap with unmatched inner DST IPv4"""
        self._test_negative_decap(is_ipv6=False)
        self.remove_configured_vpp_objects_on_tear_down = False

    def test_negative_decap_v6(self):
        """Negative test for VXLAN decap with unmatched inner DST IPv6"""
        self._test_negative_decap(is_ipv6=True)
        self.remove_configured_vpp_objects_on_tear_down = False

    def test_negative_normal_ip4(self):
        """Negative test for non-encaped IPv4 packet"""
        self._test_negative_normal_ip(is_ipv6=False)
        self.remove_configured_vpp_objects_on_tear_down = False

    def test_negative_normal_ip6(self):
        """Negative test for non-encaped IPv6 packet"""
        self._test_negative_normal_ip(is_ipv6=True)
        self.remove_configured_vpp_objects_on_tear_down = False

    def test_negative_vxlan_no_decap(self):
        """Negative test for non-terminated VXLAN packet"""
        in_pg = self.pg_interfaces[NUM_EGRESS_PGS]
        out_pg = self.pg_interfaces[NUM_EGRESS_PGS + 1]
        frame_request = self.create_frame_request(
            "00:00:00:00:00:01", "00:00:00:00:00:02", "1.2.3.4", "4.3.2.1"
        )

        encapsulated_pkt = self.encapsulate(
            frame_request,
            self.single_tunnel_vni + 1,
            in_pg.remote_mac,
            in_pg.local_mac,
            in_pg.remote_ip4,
            out_pg.remote_ip4,
        )

        in_pg.add_stream([encapsulated_pkt])
        out_pg.enable_capture()
        self.pg_start()

        self.verify_packet_forwarding(out_pg, encapsulated_pkt)
        self.logger.info(
            f"test_negative_vxlan_no_decap: Passed from {in_pg.name} to {out_pg.name}"
        )

        self.remove_configured_vpp_objects_on_tear_down = False


TestTuntermAcl.add_dynamic_tests()

if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
