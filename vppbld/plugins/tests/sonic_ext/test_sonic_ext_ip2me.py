#!/usr/bin/env python3
"""sonic-ext ip2me: IPv6 Neighbor Discovery past an ingress drop ACL"""

import unittest
from ipaddress import IPv6Network
from socket import AF_INET6, inet_ntop, inet_pton

from framework import VppTestCase
from asfframework import VppTestRunner
from util import mk_ll_addr
from vpp_acl import AclRule, VppAcl, VppAclInterface
from vpp_bvi_interface import VppBviInterface
from vpp_ip import VppIpPuntRedirect
from vpp_l2 import L2_PORT_TYPE, VppBridgeDomain, VppBridgeDomainPort
from vpp_neighbor import VppNeighbor

from scapy.layers.dhcp6 import DHCP6_Solicit
from scapy.layers.inet import UDP
from scapy.layers.inet6 import (
    IPv6,
    ICMPv6EchoRequest,
    ICMPv6ND_NA,
    ICMPv6ND_NS,
    ICMPv6ND_RA,
    ICMPv6ND_RS,
    ICMPv6NDOptDstLLAddr,
    ICMPv6NDOptSrcLLAddr,
)
from scapy.layers.l2 import Ether
from scapy.packet import Raw
from scapy.utils6 import in6_getnsma, in6_getnsmac

ND_BYPASSED = "/err/sonic-ext-ip2me-ip6/IPv6 ND frames bypassed the ACL"


class TestSonicExtIp2meNd(VppTestCase):
    """sonic-ext ip2me lets IPv6 ND past an ingress drop ACL"""

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
        super(TestSonicExtIp2meNd, cls).setUpClass()
        cls.create_pg_interfaces(range(2))

    def setUp(self):
        super(TestSonicExtIp2meNd, self).setUp()
        for i in self.pg_interfaces:
            i.admin_up()

        # pg0 is a standby mux port: a bridge member whose ingress ACL
        # denies IPv4 and IPv6, as saivpp programs MuxOrch's drop rule.  The
        # BVI is the switch's VLAN interface.
        self.bvi = VppBviInterface(self)
        self.bvi.admin_up()
        self.bvi.config_ip6()
        self.bd = VppBridgeDomain(self, 1, arp_term=0).add_vpp_config()
        VppBridgeDomainPort(self, self.bd, self.pg0).add_vpp_config()
        VppBridgeDomainPort(
            self, self.bd, self.bvi, port_type=L2_PORT_TYPE.BVI
        ).add_vpp_config()

        self.vapi.acl_stats_intf_counters_enable(enable=1)
        self.acl = VppAcl(
            self,
            [
                AclRule(is_permit=0),
                AclRule(
                    is_permit=0,
                    src_prefix=IPv6Network("::/0"),
                    dst_prefix=IPv6Network("::/0"),
                ),
            ],
        ).add_vpp_config()
        VppAclInterface(
            self, self.pg0.sw_if_index, [self.acl], n_input=1
        ).add_vpp_config()

        # pg1 stands in for the BVI's linux-cp host tap: with nd-punt, ND
        # that reaches the BVI is punted there.  The neighbor is static
        # because with nd-punt VPP cannot resolve one itself.
        self.pg1.config_ip6()
        VppNeighbor(
            self,
            self.pg1.sw_if_index,
            self.pg1.remote_mac,
            self.pg1.remote_ip6,
            is_static=True,
        ).add_vpp_config()
        VppIpPuntRedirect(
            self, self.bvi.sw_if_index, self.pg1.sw_if_index, self.pg1.remote_ip6
        ).add_vpp_config()

        # The server behind pg0
        self.host_mac = self.pg0.remote_mac
        self.host_ip = self.bvi.remote_ip6
        self.host_ll = mk_ll_addr(self.host_mac)

    def tearDown(self):
        super(TestSonicExtIp2meNd, self).tearDown()
        self.vapi.sonic_ext_ip2me_enable_disable(
            sw_if_index=self.pg0.sw_if_index, enable=False
        )
        self.vapi.acl_stats_intf_counters_enable(enable=0)
        self.bvi.remove_vpp_config()
        self.pg1.unconfig_ip6()
        for i in self.pg_interfaces:
            i.admin_down()

    def enable_ip2me(self):
        self.vapi.sonic_ext_ip2me_enable_disable(
            sw_if_index=self.pg0.sw_if_index, enable=True
        )

    def ipv6_denied(self):
        """packets that matched the ACL's IPv6 deny"""
        return self.statistics["/acl/%d/matches" % self.acl.acl_index][
            :, 1
        ].sum_packets()

    def solicited_node(self, addr):
        """(MAC, address) of the solicited-node multicast group of addr"""
        nsma = in6_getnsma(inet_pton(AF_INET6, addr))
        return in6_getnsmac(nsma), inet_ntop(AF_INET6, nsma)

    def nd_from_host(self):
        """ND from the server to the switch, one of each type"""
        bvi_mac, bvi_ll = self.bvi.local_mac, self.bvi.local_ip6_ll
        ns_mac, ns_ip = self.solicited_node(self.bvi.local_ip6)
        return [
            # The NA answering the switch's link-local NS, which the
            # standby port dropped.
            Ether(src=self.host_mac, dst=bvi_mac)
            / IPv6(src=self.host_ip, dst=bvi_ll, hlim=255)
            / ICMPv6ND_NA(tgt=self.host_ip, R=0, S=1, O=1)
            / ICMPv6NDOptDstLLAddr(lladdr=self.host_mac),
            Ether(src=self.host_mac, dst=ns_mac)
            / IPv6(src=self.host_ip, dst=ns_ip, hlim=255)
            / ICMPv6ND_NS(tgt=self.bvi.local_ip6)
            / ICMPv6NDOptSrcLLAddr(lladdr=self.host_mac),
            Ether(src=self.host_mac, dst=bvi_mac)
            / IPv6(src=self.host_ll, dst=bvi_ll, hlim=255)
            / ICMPv6ND_NS(tgt=bvi_ll)
            / ICMPv6NDOptSrcLLAddr(lladdr=self.host_mac),
            Ether(src=self.host_mac, dst="33:33:00:00:00:02")
            / IPv6(src=self.host_ll, dst="ff02::2", hlim=255)
            / ICMPv6ND_RS()
            / ICMPv6NDOptSrcLLAddr(lladdr=self.host_mac),
            # Unicast: the default capture filter drops multicast RAs.
            Ether(src=self.host_mac, dst=bvi_mac)
            / IPv6(src=self.host_ll, dst=bvi_ll, hlim=255)
            / ICMPv6ND_RA()
            / ICMPv6NDOptSrcLLAddr(lladdr=self.host_mac),
            # An unsolicited NA to all nodes
            Ether(src=self.host_mac, dst="33:33:00:00:00:01")
            / IPv6(src=self.host_ip, dst="ff02::1", hlim=255)
            / ICMPv6ND_NA(tgt=self.host_ip, R=0, S=0, O=1)
            / ICMPv6NDOptDstLLAddr(lladdr=self.host_mac),
            # Duplicate address detection
            Ether(src=self.host_mac, dst=ns_mac)
            / IPv6(src="::", dst=ns_ip, hlim=255)
            / ICMPv6ND_NS(tgt=self.bvi.local_ip6),
        ]

    def test_nd_bypasses_acl(self):
        """RS/RA/NS/NA from an ip2me port skip its IPv6 deny"""
        self.enable_ip2me()
        pkts = self.nd_from_host()
        bypassed = self.statistics.get_err_counter(ND_BYPASSED)

        rx = self.send_and_expect_only(self.pg0, pkts, self.pg1)

        self.assertEqual(
            sorted(p[IPv6].payload.type for p in rx),
            sorted(p[IPv6].payload.type for p in pkts),
        )
        self.assertEqual(
            self.statistics.get_err_counter(ND_BYPASSED) - bypassed, len(pkts)
        )
        self.assertEqual(self.ipv6_denied(), 0)

    def test_nd_denied_without_ip2me(self):
        """without ip2me the IPv6 deny drops ND"""
        pkts = self.nd_from_host()

        self.send_and_assert_no_replies(self.pg0, pkts)

        self.assertEqual(self.ipv6_denied(), len(pkts))

    def test_only_valid_nd_bypasses_acl(self):
        """ip2me leaves non-ND IPv6 and ND a receiver discards to the ACL"""
        self.enable_ip2me()
        bvi_mac, bvi_ll = self.bvi.local_mac, self.bvi.local_ip6_ll
        ns_mac, ns_ip = self.solicited_node(self.bvi.local_ip6)
        ns = ICMPv6ND_NS(tgt=self.bvi.local_ip6, cksum=0)
        pkts = [
            # The switch answers this ping if it gets past the ACL.
            Ether(src=self.host_mac, dst=bvi_mac)
            / IPv6(src=self.host_ll, dst=bvi_ll)
            / ICMPv6EchoRequest(),
            Ether(src=self.host_mac, dst=ns_mac)
            / IPv6(src=self.host_ip, dst=ns_ip, hlim=254)
            / ICMPv6ND_NS(tgt=self.bvi.local_ip6)
            / ICMPv6NDOptSrcLLAddr(lladdr=self.host_mac),
            Ether(src=self.host_mac, dst=bvi_mac)
            / IPv6(src=self.host_ip, dst=bvi_ll, hlim=255)
            / ICMPv6ND_NA(code=1, tgt=self.host_ip, R=0, S=1, O=1)
            / ICMPv6NDOptDstLLAddr(lladdr=self.host_mac),
            # An NS cut off before its target address
            Ether(src=self.host_mac, dst=ns_mac)
            / IPv6(src=self.host_ip, dst=ns_ip, hlim=255, nh=58, plen=len(ns))
            / Raw(bytes(ns)[:8]),
            # An empty IPv6 payload, then padding that looks like an NS
            Ether(src=self.host_mac, dst=ns_mac)
            / IPv6(src=self.host_ip, dst=ns_ip, hlim=255, nh=58, plen=0)
            / Raw(bytes(ns)),
            # Multicast targets
            Ether(src=self.host_mac, dst=ns_mac)
            / IPv6(src=self.host_ip, dst=ns_ip, hlim=255)
            / ICMPv6ND_NS(tgt="ff02::1")
            / ICMPv6NDOptSrcLLAddr(lladdr=self.host_mac),
            Ether(src=self.host_mac, dst=bvi_mac)
            / IPv6(src=self.host_ip, dst=bvi_ll, hlim=255)
            / ICMPv6ND_NA(tgt="ff02::1", R=0, S=1, O=1)
            / ICMPv6NDOptDstLLAddr(lladdr=self.host_mac),
            # An NS from the unspecified address, not to a solicited-node group
            Ether(src=self.host_mac, dst=bvi_mac)
            / IPv6(src="::", dst=bvi_ll, hlim=255)
            / ICMPv6ND_NS(tgt=bvi_ll),
            # A solicited NA to all nodes
            Ether(src=self.host_mac, dst="33:33:00:00:00:01")
            / IPv6(src=self.host_ip, dst="ff02::1", hlim=255)
            / ICMPv6ND_NA(tgt=self.host_ip, R=0, S=1, O=1)
            / ICMPv6NDOptDstLLAddr(lladdr=self.host_mac),
            # An RA from a global address
            Ether(src=self.host_mac, dst=bvi_mac)
            / IPv6(src=self.host_ip, dst=bvi_ll, hlim=255)
            / ICMPv6ND_RA()
            / ICMPv6NDOptSrcLLAddr(lladdr=self.host_mac),
            Ether(src=self.host_mac, dst="33:33:00:01:00:02")
            / IPv6(src=self.host_ll, dst="ff02::1:2")
            / UDP(sport=546, dport=547)
            / DHCP6_Solicit(),
        ]
        bypassed = self.statistics.get_err_counter(ND_BYPASSED)

        self.send_and_assert_no_replies(self.pg0, pkts)

        self.assertEqual(self.statistics.get_err_counter(ND_BYPASSED), bypassed)
        self.assertEqual(self.ipv6_denied(), len(pkts))


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
