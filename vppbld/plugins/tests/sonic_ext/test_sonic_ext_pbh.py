#!/usr/bin/env python3
"""SONiC-ext PBH -- policy-based hashing on bridged ports.

What is under test
------------------
PBH overrides the ECMP and/or LAG hash of a tunnelled packet with a hash
computed over the *inner* 5-tuple, selected per rule.  SONiC binds a PBH
table to PORT and LAG bind points, and sonic-mgmt binds it to every member
of a VLAN -- i.e. to *bridged* ports.  A bridged member never traverses the
ip4-unicast / ip6-unicast arcs that PBH hooks, so sonic_ext_pbh_bvi_attach()
refcounts the bridge domain's BVI, enables the arcs there, and turns on
sonic-ext-capture; sonic_ext_pbh_shadow_admits() then narrows per packet
using the capture cookie's orig_rx_sw_if_index.

sonic_ext_capture_enable_all() walks *LCP pairs*, so the bridged members
must have linux-cp pairs or no cookie is stamped and the narrowing silently
fails open.  Hence the netns plumbing.

Topology
--------
  bridge domain 1 (untagged)
    pg0, pg1   L2 members, LCP pairs, PBH table ATTACHED  (BVI refcount 2)
    pg2        L2 member,  LCP pair,  no table            (narrowing control)
    loop0      BVI, 10.10.10.1/24 + 2001:db8:10::1/64 -- the PBH arcs run here

  BondEthernet0 = (pg3, pg4), mode XOR, lb L34
    10.99.99.1/24 + 2001:db8:99::1/64
    two static neighbours: .10 -> 00:...:01 and .11 -> 00:...:02
    ECMP route 20.0.0.0/24 (2001:db8:20::/64) with BOTH paths on the bond

The two next hops share one bond, which lets a single captured frame report
both decisions at once:

    dst MAC of the frame        == the ECMP decision
    which of pg3/pg4 caught it  == the LAG decision

Traffic is VXLAN or NVGRE with a *constant* outer 5-tuple, so without PBH
the stock outer-based hash collapses every flow onto one path.  Any spread
observed is therefore produced by PBH.

Profiles are deliberately disjoint -- ECMP hashes the inner addresses, LAG
hashes the inner L4 ports -- so the two can be shown to be independent.
Within each profile src and dst share a sequence_id, which the plugin
XOR-folds, giving order-independent (symmetric) hashing.
"""

import random
import re
import unittest

from scapy.layers.inet import IP, UDP
from scapy.layers.inet6 import IPv6
from scapy.layers.l2 import Ether, GRE
from scapy.layers.vxlan import VXLAN
from scapy.packet import Raw

from asfframework import VppTestRunner, get_testcase_dirname
from config import config
from framework import VppTestCase
from vpp_bond_interface import VppBondInterface
from vpp_bvi_interface import VppBviInterface
from vpp_ip_route import VppIpRoute, VppRoutePath
from vpp_l2 import L2_PORT_TYPE, VppBridgeDomain, VppBridgeDomainPort
from vpp_neighbor import VppNeighbor
from vpp_papi import VppEnum
from vpp_papi_exceptions import CliFailedCommandError
from vpp_qemu_utils import create_namespace, delete_all_namespaces, set_interface_up

NO_INDEX = 0xFFFFFFFF

# sonic_ext_pbh_hash_field_id_t -- pbh.h.  The .api carries a bare u8, so
# there is no VppEnum for these; the ordinals have to be mirrored here.
HF_INNER_IP_PROTOCOL = 0
HF_INNER_L4_SRC_PORT = 1
HF_INNER_L4_DST_PORT = 2
HF_INNER_SRC_IPV4 = 3
HF_INNER_DST_IPV4 = 4
HF_INNER_SRC_IPV6 = 5
HF_INNER_DST_IPV6 = 6

# sonic_ext_pbh_qualifier_t bitmap -- pbh.h.  Same story: bare u32.
Q_ETHER_TYPE = 1 << 0
Q_IP_PROTOCOL = 1 << 1
Q_IPV6_NEXT_HEADER = 1 << 2
Q_L4_DST_PORT = 1 << 3
Q_GRE_KEY = 1 << 4
Q_INNER_ETHER_TYPE = 1 << 5

ETHERTYPE_IP4 = 0x0800
ETHERTYPE_IP6 = 0x86DD
PROTO_UDP = 0x11
PROTO_GRE = 0x2F
GRE_PROTO_TEB = 0x6558

# SONiC's inner-hashing profile uses a non-standard VXLAN port; mirror it so
# the rule set matches what sonic-mgmt installs.
VXLAN_PORT = 13330
VXLAN_VNI = 1000
GRE_KEY = 0x2500
GRE_KEY_MASK = 0xFFFFFF00

MASK_V4 = b"\xff" * 4 + b"\x00" * 12
MASK_V6 = b"\xff" * 16
MASK_NONE = b"\x00" * 16

INNER_DMAC = "aa:bb:cc:dd:ee:00"
INNER_SMAC = "aa:bb:cc:dd:ee:01"

N_PKTS = 257
N_PAIRS = 16
TAG_LEN = 8

PBH_NODE4 = "sonic-ext-pbh-ip4"
PBH_NODE6 = "sonic-ext-pbh-ip6"
ERR_HIT = "packets matched a PBH rule"
ERR_MISS = "packets matched no PBH rule"
ERR_UNRESOLVED = "inner header could not be parsed"

# format_pbh_trace() -- pbh_node.c.  Longest action name first, the
# alternation is ordered.
PBH_TRACE_RE = re.compile(
    r"sonic-ext-pbh: table (\d+) "
    r"(?:(miss)"
    r"|rule (\d+) "
    r"(set-ecmp-hash set-lag-hash|set-ecmp-hash|set-lag-hash|none) "
    r"hash 0x([0-9a-f]{8})(?: dpo (\d+))?)"
)


def _rand_ipv4(rng):
    return "10.{}.{}.{}".format(
        rng.randint(1, 254), rng.randint(1, 254), rng.randint(1, 254)
    )


def _rand_ipv6(rng):
    return "2001:db8:{:x}:{:x}::{:x}".format(
        rng.randint(1, 0xFFFF), rng.randint(1, 0xFFFF), rng.randint(1, 0xFFFF)
    )


def _rand_addr(rng, inner_l):
    return _rand_ipv4(rng) if inner_l is IP else _rand_ipv6(rng)


def _rand_port(rng):
    return rng.randint(1024, 65535)


def _tag(i):
    return b"PBH%05d" % i


#
# packet builders
#


def _outer(outer_l, src, dst, proto):
    if outer_l is IP:
        return IP(src=src, dst=dst, proto=proto, ttl=64)
    return IPv6(src=src, dst=dst, nh=proto, hlim=64)


def _build_vxlan(outer_l, osrc, odst, osport, inner_l, isrc, idst, isport, idport):
    return (
        _outer(outer_l, osrc, odst, PROTO_UDP)
        / UDP(sport=osport, dport=VXLAN_PORT)
        / VXLAN(vni=VXLAN_VNI, flags=0x08)
        / Ether(dst=INNER_DMAC, src=INNER_SMAC)
        / inner_l(src=isrc, dst=idst)
        / UDP(sport=isport, dport=idport)
    )


def _build_nvgre(outer_l, osrc, odst, okey, inner_l, isrc, idst, isport, idport):
    return (
        _outer(outer_l, osrc, odst, PROTO_GRE)
        / GRE(proto=GRE_PROTO_TEB, key_present=1, key=okey)
        / Ether(dst=INNER_DMAC, src=INNER_SMAC)
        / inner_l(src=isrc, dst=idst)
        / UDP(sport=isport, dport=idport)
    )


@unittest.skipIf("linux-cp" in config.excluded_plugins, "Exclude linux-cp plugin tests")
@unittest.skipIf(
    "sonic_ext" in config.excluded_plugins, "Exclude sonic-ext plugin tests"
)
@unittest.skipIf(config.skip_netns_tests, "netns not available or disabled from cli")
class SonicExtPbhBase(VppTestCase):
    """Bridged-ingress PBH fixture.  No test_ methods -- see subclasses."""

    # Both plugins are .default_disabled, so ask for them explicitly.
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

    BD_ID = 1

    BVI_IP4 = "10.10.10.1"
    BVI_IP6 = "2001:db8:10::1"
    BOND_IP4 = "10.99.99.1"
    BOND_IP6 = "2001:db8:99::1"
    NH4 = ("10.99.99.10", "10.99.99.11")
    NH6 = ("2001:db8:99::10", "2001:db8:99::11")
    NH_MAC = ("00:00:00:aa:bb:01", "00:00:00:aa:bb:02")
    ECMP4 = ("20.0.0.0", 24)
    ECMP6 = ("2001:db8:20::", 64)
    OUTER_DST4 = "20.0.0.5"
    OUTER_DST6 = "2001:db8:20::5"
    OUTER_SRC4 = "192.0.2.1"
    OUTER_SRC6 = "2001:db8:dead::1"

    # Subclass knobs: which actions the rules carry.
    WANT_ECMP = True
    WANT_LAG = True

    #
    # netns plumbing -- the namespace has to exist before VPP starts
    #

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
        super(SonicExtPbhBase, self).setUp()

        self.create_pg_interfaces(range(5))
        for i in self.pg_interfaces:
            i.admin_up()

        self.attached = [self.pg0, self.pg1]
        self.unattached = self.pg2
        self.bridged = self.attached + [self.unattached]
        self.members = [self.pg3, self.pg4]

        self._setup_bridge()
        self._setup_bond()
        self._setup_lcp()

        self.assertTrue(
            self.vapi.sonic_ext_feature_get(feature="pbh").enabled,
            "pbh is disabled; the suite cannot configure anything",
        )

        self._setup_pbh()
        self.logger.info("sonic-ext pbh:\n%s", self.vapi.cli("show sonic-ext pbh"))

    def tearDown(self):
        # The framework calls show_commands_at_teardown() from its own
        # tearDown(), i.e. after this method has already dismantled the
        # topology -- by then "show bridge-domain 1" is an error, not a
        # diagnostic.  Snapshot the interesting state while it still exists.
        self._capture_teardown_state()

        self._teardown_pbh()

        for itf in reversed(self.lcp_pairs):
            self.vapi.cli("lcp delete %s" % itf)

        for r in self.routes:
            r.remove_vpp_config()
        for n in self.neighbors:
            n.remove_vpp_config()

        for p in self.bd_ports:
            p.remove_vpp_config()
        self.bd.remove_vpp_config()
        self.bvi.admin_down()
        self.bvi.remove_vpp_config()

        for i in self.members:
            self.bond.detach_vpp_bond_interface(sw_if_index=i.sw_if_index)
        self.bond.admin_down()
        self.bond.remove_vpp_config()

        for i in self.pg_interfaces:
            i.admin_down()
        super(SonicExtPbhBase, self).tearDown()

    def _capture_teardown_state(self):
        """Collect the per-suite show output before the topology is removed."""
        self._teardown_state = []
        for cmd in (
            "show interface",
            "show bond details",
            "show bridge-domain %d detail" % self.BD_ID,
            "show sonic-ext pbh",
            "show ip fib %s" % self.ECMP4[0],
            "show ip6 fib %s" % self.ECMP6[0],
        ):
            try:
                out = self.vapi.cli(cmd)
            except Exception as e:
                # A test may have deliberately removed the object being
                # shown; that must not mask the real failure.
                out = "<%s: %s>" % (type(e).__name__, e)
            self._teardown_state.append((cmd, out))

    def show_commands_at_teardown(self):
        for cmd, out in getattr(self, "_teardown_state", []):
            self.logger.info("%s:\n%s", cmd, out)

    #
    # topology
    #

    def _setup_bridge(self):
        # VppBviInterface creates itself in __init__.
        self.bvi = VppBviInterface(self)
        self.bvi.admin_up()

        # arp_term=0: nothing here relies on the bridge answering ARP, and
        # SONiC does not enable it for a plain VLAN.
        self.bd = VppBridgeDomain(self, self.BD_ID, arp_term=0).add_vpp_config()
        self.bd_ports = [
            VppBridgeDomainPort(self, self.bd, i).add_vpp_config() for i in self.bridged
        ]
        self.bd_ports.append(
            VppBridgeDomainPort(
                self, self.bd, self.bvi, port_type=L2_PORT_TYPE.BVI
            ).add_vpp_config()
        )

        for prefix in ("%s/24" % self.BVI_IP4, "%s/64" % self.BVI_IP6):
            self.vapi.sw_interface_add_del_address(
                sw_if_index=self.bvi.sw_if_index, prefix=prefix
            )

    def _setup_bond(self):
        # XOR + L34: the override is skipped outright for active-backup,
        # broadcast and round-robin bonds.
        self.bond = VppBondInterface(
            self,
            mode=VppEnum.vl_api_bond_mode_t.BOND_API_MODE_XOR,
            lb=VppEnum.vl_api_bond_lb_algo_t.BOND_API_LB_ALGO_L34,
        )
        self.bond.add_vpp_config()
        self.bond.admin_up()
        for i in self.members:
            self.bond.add_member_vpp_bond_interface(sw_if_index=i.sw_if_index)

        for prefix in ("%s/24" % self.BOND_IP4, "%s/64" % self.BOND_IP6):
            self.vapi.sw_interface_add_del_address(
                sw_if_index=self.bond.sw_if_index, prefix=prefix
            )

        # Static neighbours: the two ECMP next hops differ only by their
        # rewrite, which is what makes the egress dst MAC report the ECMP
        # bucket.
        self.neighbors = []
        for nh4, nh6, mac in zip(self.NH4, self.NH6, self.NH_MAC):
            for nh in (nh4, nh6):
                self.neighbors.append(
                    VppNeighbor(
                        self, self.bond.sw_if_index, mac, nh, is_static=True
                    ).add_vpp_config()
                )

        self.routes = []
        for (net, plen), nhs in ((self.ECMP4, self.NH4), (self.ECMP6, self.NH6)):
            paths = [VppRoutePath(nh, self.bond.sw_if_index) for nh in nhs]
            self.routes.append(VppIpRoute(self, net, plen, paths).add_vpp_config())

    def _setup_lcp(self):
        # The pairs must come after the BVI is bound into the bridge domain:
        # sonic_ext_lcp_pair_add_cb() uses l2_input_is_bvi() to decide whether
        # a pair is a member (gets sonic-ext-capture) or an aggregate.  And
        # they must come before the first PBH attach, because that is what
        # calls sonic_ext_capture_enable_all(), which walks the pairs that
        # exist at that moment.
        self.lcp_pairs = []
        for n, i in enumerate(self.bridged):
            self.vapi.cli("lcp create %s host-if hpg%d netns %s" % (i, n, self.ns_name))
            self.lcp_pairs.append(i)
            set_interface_up(self.ns_name, "hpg%d" % n)
        self.logger.info("lcp:\n%s", self.vapi.cli("show lcp"))

    #
    # PBH configuration
    #

    # papi does not derive the vector-length field of a VLA from the list it
    # counts -- VLAList.pack() looks up kwargs[length_field] and raises if it
    # is absent -- so every caller has to pass n_fields / n_rules.  Funnel the
    # calls through these two helpers rather than repeating that at each site.

    def _profile_add_del(self, **kwargs):
        kwargs.setdefault("fields", [])
        kwargs["n_fields"] = len(kwargs["fields"])
        return self.vapi.sonic_ext_pbh_profile_add_del(**kwargs)

    def _table_add_replace(self, **kwargs):
        kwargs.setdefault("rules", [])
        kwargs["n_rules"] = len(kwargs["rules"])
        return self.vapi.sonic_ext_pbh_table_add_replace(**kwargs)

    def _add_profile(self, fields):
        r = self._profile_add_del(is_add=True, profile_index=NO_INDEX, fields=fields)
        return r.profile_index

    @staticmethod
    def _addr_fields():
        """Inner addresses only.  src and dst share a sequence_id, so the
        plugin XOR-folds them and the hash is order independent."""
        return [
            {"field": HF_INNER_SRC_IPV4, "sequence_id": 1, "mask": MASK_V4},
            {"field": HF_INNER_DST_IPV4, "sequence_id": 1, "mask": MASK_V4},
            {"field": HF_INNER_SRC_IPV6, "sequence_id": 2, "mask": MASK_V6},
            {"field": HF_INNER_DST_IPV6, "sequence_id": 2, "mask": MASK_V6},
        ]

    @staticmethod
    def _port_fields():
        """Inner L4 ports only, likewise symmetric."""
        return [
            {"field": HF_INNER_L4_SRC_PORT, "sequence_id": 1, "mask": MASK_NONE},
            {"field": HF_INNER_L4_DST_PORT, "sequence_id": 1, "mask": MASK_NONE},
        ]

    @classmethod
    def _full_fields(cls):
        """The shape SONiC actually installs: protocol, ports, v4 and v6
        addresses, each in its own sequence group."""
        return [
            {"field": HF_INNER_IP_PROTOCOL, "sequence_id": 1, "mask": MASK_NONE},
            {"field": HF_INNER_L4_SRC_PORT, "sequence_id": 2, "mask": MASK_NONE},
            {"field": HF_INNER_L4_DST_PORT, "sequence_id": 2, "mask": MASK_NONE},
            {"field": HF_INNER_SRC_IPV4, "sequence_id": 3, "mask": MASK_V4},
            {"field": HF_INNER_DST_IPV4, "sequence_id": 3, "mask": MASK_V4},
            {"field": HF_INNER_SRC_IPV6, "sequence_id": 4, "mask": MASK_V6},
            {"field": HF_INNER_DST_IPV6, "sequence_id": 4, "mask": MASK_V6},
        ]

    def _rule(self, rule_id, priority, encap, outer_af, inner_af, ecmp, lag):
        """One rule of the 8-way {encap} x {outer af} x {inner af} matrix.

        ether_type is matched against the *node's* address family rather than
        the packet, so it has to agree with outer_af.  A VXLAN rule must carry
        both the protocol and the L4 dst port or sonic_ext_pbh_encap_from_match
        derives ENCAP_NONE and the add is rejected.
        """
        r = {
            "rule_id": rule_id,
            "priority": priority,
            "qualifiers": Q_ETHER_TYPE | Q_INNER_ETHER_TYPE,
            "ether_type": ETHERTYPE_IP4 if outer_af == 4 else ETHERTYPE_IP6,
            "inner_ether_type": ETHERTYPE_IP4 if inner_af == 4 else ETHERTYPE_IP6,
            "l4_dst_port": 0,
            "ip_protocol": 0,
            "ipv6_next_header": 0,
            "gre_key": 0,
            "gre_key_mask": 0,
            "ecmp_profile": ecmp,
            "lag_profile": lag,
            "flow_counter": True,
        }

        proto = PROTO_UDP if encap == "vxlan" else PROTO_GRE
        if outer_af == 4:
            r["qualifiers"] |= Q_IP_PROTOCOL
            r["ip_protocol"] = proto
        else:
            r["qualifiers"] |= Q_IPV6_NEXT_HEADER
            r["ipv6_next_header"] = proto

        if encap == "vxlan":
            r["qualifiers"] |= Q_L4_DST_PORT
            r["l4_dst_port"] = VXLAN_PORT
        else:
            r["qualifiers"] |= Q_GRE_KEY
            r["gre_key"] = GRE_KEY
            r["gre_key_mask"] = GRE_KEY_MASK

        return r

    @staticmethod
    def _sorted_positions(rules):
        """Mirror sonic_ext_pbh_rule_cmp(): descending priority, ties broken
        by ascending rule_id.  Returns {rule_id: position}."""
        order = sorted(rules, key=lambda r: (-r["priority"], r["rule_id"]))
        return {r["rule_id"]: pos for pos, r in enumerate(order)}

    def _build_rules(self, ecmp, lag):
        rules = []
        keys = []
        rule_id = 1
        priority = 80
        for encap in ("vxlan", "nvgre"):
            for outer_af in (4, 6):
                for inner_af in (4, 6):
                    rules.append(
                        self._rule(
                            rule_id, priority, encap, outer_af, inner_af, ecmp, lag
                        )
                    )
                    keys.append((encap, outer_af, inner_af))
                    rule_id += 1
                    priority -= 1
        return rules, keys

    def _setup_pbh(self):
        self.ecmp_profile = NO_INDEX
        self.lag_profile = NO_INDEX
        self.profiles = []

        if self.WANT_ECMP:
            self.ecmp_profile = self._add_profile(self._addr_fields())
            self.profiles.append(self.ecmp_profile)
        if self.WANT_LAG:
            self.lag_profile = self._add_profile(self._port_fields())
            self.profiles.append(self.lag_profile)

        rules, keys = self._build_rules(self.ecmp_profile, self.lag_profile)
        self.rules = rules
        positions = self._sorted_positions(rules)
        self.rule_id_of = dict(zip(keys, [r["rule_id"] for r in rules]))
        self.rule_pos = {k: positions[self.rule_id_of[k]] for k in keys}

        r = self._table_add_replace(table_index=NO_INDEX, name="pbh_table", rules=rules)
        self.table_index = r.table_index

        for i in self.attached:
            self.vapi.sonic_ext_pbh_interface_attach_detach(
                sw_if_index=i.sw_if_index,
                table_index=self.table_index,
                is_attach=True,
            )

    def _teardown_pbh(self):
        # A test may legitimately have deleted the table already.
        if getattr(self, "table_index", None) is not None:
            self.vapi.sonic_ext_pbh_table_del(table_index=self.table_index)
            self.table_index = None
        for p in getattr(self, "profiles", []):
            self._profile_add_del(is_add=False, profile_index=p)
        self.profiles = []

    #
    # traffic helpers
    #

    def _frame(self, l3):
        return Ether(dst=self.bvi.local_mac, src=self.pg0.remote_mac) / l3

    def _encap(self, encap, outer_af, inner_af, inner, osport=12345, okey=GRE_KEY):
        outer_l = IP if outer_af == 4 else IPv6
        inner_l = IP if inner_af == 4 else IPv6
        osrc = self.OUTER_SRC4 if outer_af == 4 else self.OUTER_SRC6
        odst = self.OUTER_DST4 if outer_af == 4 else self.OUTER_DST6
        isrc, idst, isport, idport = inner
        if encap == "vxlan":
            return _build_vxlan(
                outer_l, osrc, odst, osport, inner_l, isrc, idst, isport, idport
            )
        return _build_nvgre(
            outer_l, osrc, odst, okey, inner_l, isrc, idst, isport, idport
        )

    @staticmethod
    def _fixed_inner(inner_af):
        if inner_af == 4:
            return ("10.99.0.1", "10.99.0.2", 1234, 5678)
        return ("2001:db8:cafe::1", "2001:db8:cafe::2", 1234, 5678)

    def _stream(self, encap, outer_af, inner_af, vary, seed=1, n=N_PKTS):
        """Build n packets varying exactly one aspect.

        vary='inner'  -- whole inner 5-tuple random, outer constant
        vary='ports'  -- inner L4 ports random, inner addresses constant
        vary='addrs'  -- inner addresses random, inner ports constant
        vary='outer'  -- outer varies (udp sport / gre key low byte), inner
                         constant.  The GRE key low byte is below the rule's
                         0xffffff00 mask, so the rule still matches.
        """
        rng = random.Random(seed)
        fsrc, fdst, fsport, fdport = self._fixed_inner(inner_af)
        pkts = []
        for _ in range(n):
            osport, okey = 12345, GRE_KEY
            isrc, idst, isport, idport = fsrc, fdst, fsport, fdport
            if vary == "inner":
                isrc, idst = _rand_addr(rng, IP if inner_af == 4 else IPv6), _rand_addr(
                    rng, IP if inner_af == 4 else IPv6
                )
                isport, idport = _rand_port(rng), _rand_port(rng)
            elif vary == "ports":
                isport, idport = _rand_port(rng), _rand_port(rng)
            elif vary == "addrs":
                isrc, idst = _rand_addr(rng, IP if inner_af == 4 else IPv6), _rand_addr(
                    rng, IP if inner_af == 4 else IPv6
                )
            elif vary == "outer":
                osport = _rand_port(rng)
                okey = GRE_KEY | rng.randint(0, 0xFF)
            else:
                raise ValueError(vary)
            pkts.append(
                self._frame(
                    self._encap(
                        encap,
                        outer_af,
                        inner_af,
                        (isrc, idst, isport, idport),
                        osport=osport,
                        okey=okey,
                    )
                )
                / Raw(_tag(0))
            )
        return pkts

    def _symmetric_stream(self, encap, outer_af, inner_af, seed=7):
        """N_PAIRS random flows, each immediately followed by its mirror
        image (inner src/dst and sport/dport swapped).  Both members of a
        pair carry the same payload tag."""
        rng = random.Random(seed)
        inner_l = IP if inner_af == 4 else IPv6
        pkts = []
        for i in range(N_PAIRS):
            isrc = _rand_addr(rng, inner_l)
            idst = _rand_addr(rng, inner_l)
            isport, idport = _rand_port(rng), _rand_port(rng)
            for inner in ((isrc, idst, isport, idport), (idst, isrc, idport, isport)):
                pkts.append(
                    self._frame(self._encap(encap, outer_af, inner_af, inner))
                    / Raw(_tag(i))
                )
        return pkts

    def _send(self, src, pkts, trace=False):
        self.pg_enable_capture(self.pg_interfaces)
        src.add_stream(pkts)
        self.pg_start(trace=trace)
        return [pg._get_capture() or [] for pg in self.members]

    def _observe(self, src, pkts, trace=False):
        """Return (members, macs, total): the set of bond members that caught
        anything -- the LAG decision -- and the set of egress dst MACs -- the
        ECMP decision."""
        caps = self._send(src, pkts, trace=trace)
        members, macs, total = set(), set(), 0
        for idx, cap in enumerate(caps):
            if cap:
                members.add(idx)
            for p in cap:
                macs.add(p[Ether].dst)
                total += 1
        return members, macs, total

    def _observe_by_tag(self, src, pkts):
        """Return {tag: (member_index, dst_mac)} keyed off the payload tag.

        Always traces: the only caller pairs this with _pbh_traces() to
        compare the two hashes of a flow and its mirror image."""
        caps = self._send(src, pkts, trace=True)
        out = {}
        for idx, cap in enumerate(caps):
            for p in cap:
                out.setdefault(bytes(p)[-TAG_LEN:], []).append((idx, p[Ether].dst))
        return out

    def _pbh_traces(self):
        """Parse the PBH trace lines of the last pg run, in dispatch order."""
        out = []
        for m in PBH_TRACE_RE.finditer(self.vapi.cli("show trace max 1000")):
            table, miss, rule_id, action, pbh_hash, dpo = m.groups()
            out.append(
                {
                    "table": int(table),
                    "miss": miss is not None,
                    "rule_id": int(rule_id) if rule_id else None,
                    "action": action,
                    "hash": int(pbh_hash, 16) if pbh_hash else None,
                    "dpo": int(dpo) if dpo else None,
                }
            )
        return out

    #
    # counters
    #

    def _rule_packets(self, position):
        c = self.statistics["/sonic-ext/pbh/%u/matches" % self.table_index]
        return sum(thread[position]["packets"] for thread in c)

    def _err(self, outer_af, name):
        node = PBH_NODE4 if outer_af == 4 else PBH_NODE6
        return self.statistics.get_err_counter("/err/%s/%s" % (node, name))

    #
    # shared assertions
    #

    def _assert_spread_and_collapse(self, encap, af):
        """Varying the inner 5-tuple must spread across both the ECMP next
        hops and the bond members; varying only the outer must not move
        either, which is the control that proves PBH is doing the work."""
        members, macs, total = self._observe(
            self.pg0, self._stream(encap, af, af, "inner")
        )
        self.assertEqual(total, N_PKTS, "lost packets (members=%s)" % members)
        self.assertEqual(len(macs), 2, "ECMP did not spread: macs=%s" % macs)
        self.assertEqual(len(members), 2, "LAG did not spread: members=%s" % members)

        members, macs, total = self._observe(
            self.pg0, self._stream(encap, af, af, "outer")
        )
        self.assertEqual(total, N_PKTS, "lost packets (members=%s)" % members)
        self.assertEqual(len(macs), 1, "outer variation moved ECMP: macs=%s" % macs)
        self.assertEqual(
            len(members), 1, "outer variation moved LAG: members=%s" % members
        )

    def _assert_symmetric(self, encap, af):
        """A flow and its mirror image must hash identically: src and dst
        share a sequence_id, which the plugin XOR-folds."""
        pkts = self._symmetric_stream(encap, af, af)
        by_tag = self._observe_by_tag(self.pg0, pkts)
        traces = [t for t in self._pbh_traces() if not t["miss"]]

        self.assertEqual(
            len(traces), 2 * N_PAIRS, "unexpected PBH trace count: %s" % len(traces)
        )
        for i in range(N_PAIRS):
            fwd, rev = traces[2 * i], traces[2 * i + 1]
            self.assertEqual(
                fwd["hash"],
                rev["hash"],
                "pair %d: hash 0x%08x != 0x%08x" % (i, fwd["hash"], rev["hash"]),
            )
            self.assertEqual(fwd["dpo"], rev["dpo"], "pair %d: dpo differs" % i)

        for i in range(N_PAIRS):
            seen = by_tag.get(_tag(i), [])
            self.assertEqual(len(seen), 2, "pair %d: lost a packet" % i)
            self.assertEqual(
                seen[0],
                seen[1],
                "pair %d: forward and reverse diverged: %s" % (i, seen),
            )

    def _assert_independence(self, encap, af):
        """The two actions use disjoint profiles, so each must respond only to
        its own fields.  A single test with both actions set is the only way
        to catch one hash leaking into the other."""
        members, macs, total = self._observe(
            self.pg0, self._stream(encap, af, af, "ports")
        )
        self.assertEqual(total, N_PKTS)
        self.assertEqual(
            len(members), 2, "inner ports did not move the LAG member: %s" % members
        )
        self.assertEqual(
            len(macs), 1, "inner ports leaked into the ECMP hash: macs=%s" % macs
        )

        members, macs, total = self._observe(
            self.pg0, self._stream(encap, af, af, "addrs")
        )
        self.assertEqual(total, N_PKTS)
        self.assertEqual(
            len(macs), 2, "inner addresses did not move the ECMP bucket: %s" % macs
        )
        self.assertEqual(
            len(members),
            1,
            "inner addresses leaked into the LAG hash: members=%s" % members,
        )


class TestSonicExtPbhEcmpLag(SonicExtPbhBase):
    """SONiC-ext PBH: ECMP and LAG hash set together"""

    WANT_ECMP = True
    WANT_LAG = True

    def test_vxlan_v4_spread(self):
        """PBH VXLAN outer-v4/inner-v4: inner spreads, outer collapses"""
        self._assert_spread_and_collapse("vxlan", 4)

    def test_vxlan_v4_symmetric(self):
        """PBH VXLAN outer-v4/inner-v4: forward and reverse hash alike"""
        self._assert_symmetric("vxlan", 4)

    def test_vxlan_v4_independence(self):
        """PBH VXLAN outer-v4/inner-v4: ECMP and LAG hashes are independent"""
        self._assert_independence("vxlan", 4)

    def test_vxlan_v6_spread(self):
        """PBH VXLAN outer-v6/inner-v6: inner spreads, outer collapses"""
        self._assert_spread_and_collapse("vxlan", 6)

    def test_vxlan_v6_symmetric(self):
        """PBH VXLAN outer-v6/inner-v6: forward and reverse hash alike"""
        self._assert_symmetric("vxlan", 6)

    def test_vxlan_v6_independence(self):
        """PBH VXLAN outer-v6/inner-v6: ECMP and LAG hashes are independent"""
        self._assert_independence("vxlan", 6)

    def test_nvgre_v4_spread(self):
        """PBH NVGRE outer-v4/inner-v4: inner spreads, outer collapses"""
        self._assert_spread_and_collapse("nvgre", 4)

    def test_nvgre_v4_symmetric(self):
        """PBH NVGRE outer-v4/inner-v4: forward and reverse hash alike"""
        self._assert_symmetric("nvgre", 4)

    def test_nvgre_v4_independence(self):
        """PBH NVGRE outer-v4/inner-v4: ECMP and LAG hashes are independent"""
        self._assert_independence("nvgre", 4)

    def test_nvgre_v6_spread(self):
        """PBH NVGRE outer-v6/inner-v6: inner spreads, outer collapses"""
        self._assert_spread_and_collapse("nvgre", 6)

    def test_nvgre_v6_symmetric(self):
        """PBH NVGRE outer-v6/inner-v6: forward and reverse hash alike"""
        self._assert_symmetric("nvgre", 6)

    def test_nvgre_v6_independence(self):
        """PBH NVGRE outer-v6/inner-v6: ECMP and LAG hashes are independent"""
        self._assert_independence("nvgre", 6)

    def test_trace_reports_both_actions(self):
        """PBH trace names both actions and the chosen adjacency"""
        pkts = self._stream("vxlan", 4, 4, "inner", n=4)
        self._send(self.pg0, pkts, trace=True)
        traces = self._pbh_traces()
        self.assertEqual(len(traces), 4, "unexpected trace count")
        key = ("vxlan", 4, 4)
        for t in traces:
            self.assertFalse(t["miss"])
            self.assertEqual(t["table"], self.table_index)
            self.assertEqual(t["rule_id"], self.rule_id_of[key])
            self.assertEqual(t["action"], "set-ecmp-hash set-lag-hash")
            self.assertNotEqual(t["hash"], 0, "the hash is never the 0 sentinel")
            self.assertIsNotNone(t["dpo"], "ECMP steer did not resolve an adjacency")

    def test_rule_counters(self):
        """PBH per-rule counters follow the priority-sorted position"""
        key = ("vxlan", 4, 4)
        pos = self.rule_pos[key]
        other = self.rule_pos[("nvgre", 6, 6)]

        before = self._rule_packets(pos)
        before_other = self._rule_packets(other)
        before_hit = self._err(4, ERR_HIT)

        n = 10
        self._send(self.pg0, self._stream("vxlan", 4, 4, "inner", n=n))

        self.assertEqual(self._rule_packets(pos), before + n)
        self.assertEqual(self._rule_packets(other), before_other)
        self.assertEqual(self._err(4, ERR_HIT), before_hit + n)

    def test_replace_clears_counters(self):
        """Replacing a PBH table's rule set zeroes its counters"""
        pos = self.rule_pos[("vxlan", 4, 4)]
        self._send(self.pg0, self._stream("vxlan", 4, 4, "inner", n=10))
        self.assertEqual(self._rule_packets(pos), 10)

        self._table_add_replace(
            table_index=self.table_index, name="pbh_table", rules=self.rules
        )
        self.assertEqual(self._rule_packets(pos), 0)

    def test_full_profile(self):
        """PBH with the full SONiC hash-field set still steers both actions"""
        full = self._add_profile(self._full_fields())
        self.profiles.append(full)
        rules, _ = self._build_rules(full, full)
        self._table_add_replace(
            table_index=self.table_index, name="pbh_table", rules=rules
        )

        members, macs, total = self._observe(
            self.pg0, self._stream("vxlan", 4, 4, "inner")
        )
        self.assertEqual(total, N_PKTS)
        self.assertEqual(len(macs), 2, "ECMP did not spread: macs=%s" % macs)
        self.assertEqual(len(members), 2, "LAG did not spread: members=%s" % members)

        # The same profile now drives both actions, so a mirrored flow must
        # still land identically -- every group is src/dst symmetric.
        self._assert_symmetric("vxlan", 4)


class TestSonicExtPbhShadow(SonicExtPbhBase):
    """SONiC-ext PBH: bridge-domain shadow and per-port narrowing"""

    def test_capture_enabled_on_attach(self):
        """PBH attach on a bridged port turns sonic-ext-capture on"""
        # setUp already attached, so capture must be on by now.
        self.assertTrue(self.vapi.sonic_ext_feature_get(feature="capture").enabled)

    def test_show_interfaces_reports_shadow(self):
        """show sonic-ext pbh interfaces reports the bridge domain shadow"""
        out = self.vapi.cli("show sonic-ext pbh interfaces")
        self.assertIn(
            "%s: table %u (bridge domain shadow, 2 members)"
            % (self.bvi.name, self.table_index),
            out,
        )
        for i in self.attached:
            self.assertIn("%s: table %u" % (i.name, self.table_index), out)

    def test_arcs_enabled_on_bvi(self):
        """PBH feature arcs run on the BVI, not on the bridged member"""
        out = self.vapi.cli("show interface features %s" % self.bvi.name)
        self.assertIn(PBH_NODE4, out)
        self.assertIn(PBH_NODE6, out)

    def test_attached_members_are_steered(self):
        """Both attached bridged members reach the same PBH table"""
        for i in self.attached:
            members, macs, total = self._observe(
                i, self._stream("vxlan", 4, 4, "inner")
            )
            self.assertEqual(total, N_PKTS)
            self.assertEqual(len(macs), 2, "%s: ECMP did not spread" % i.name)
            self.assertEqual(len(members), 2, "%s: LAG did not spread" % i.name)

    def test_unattached_member_is_narrowed_out(self):
        """Traffic from an unattached bridged member is not steered

        pg2 shares the bridge domain -- and therefore the BVI the arcs run on
        -- with pg0 and pg1, so without sonic_ext_pbh_shadow_admits() it would
        be hashed by the same table.  The capture cookie's orig_rx_sw_if_index
        is what keeps it out.
        """
        before_miss = self._err(4, ERR_MISS)
        before_hit = self._err(4, ERR_HIT)

        members, macs, total = self._observe(
            self.unattached, self._stream("vxlan", 4, 4, "inner", n=10), trace=True
        )

        self.assertEqual(total, 10, "packets were dropped, not just un-steered")
        self.assertEqual(self._err(4, ERR_MISS), before_miss + 10)
        self.assertEqual(self._err(4, ERR_HIT), before_hit)

        traces = self._pbh_traces()
        self.assertEqual(len(traces), 10)
        for t in traces:
            self.assertTrue(t["miss"], "narrowing let a packet through: %s" % t)
            self.assertEqual(t["table"], NO_INDEX)

        # The outer 5-tuple is constant, so the stock hash collapses.
        self.assertEqual(len(macs), 1, "unattached port got PBH ECMP: %s" % macs)
        self.assertEqual(len(members), 1, "unattached port got PBH LAG: %s" % members)

    def test_second_table_does_not_displace_shadow(self):
        """A second PBH table cannot take over an already shadowed BVI"""
        rules, _ = self._build_rules(self.ecmp_profile, self.lag_profile)
        r = self._table_add_replace(
            table_index=NO_INDEX, name="pbh_table_b", rules=rules
        )
        other = r.table_index
        try:
            self.vapi.sonic_ext_pbh_interface_attach_detach(
                sw_if_index=self.unattached.sw_if_index,
                table_index=other,
                is_attach=True,
            )

            out = self.vapi.cli("show sonic-ext pbh interfaces")
            self.assertIn(
                "%s: table %u (bridge domain shadow"
                % (self.bvi.name, self.table_index),
                out,
                "the second table displaced the shadow",
            )

            # pg2 is attached to a table the BVI does not shadow, so its
            # traffic is still narrowed out.
            members, macs, _ = self._observe(
                self.unattached, self._stream("vxlan", 4, 4, "inner")
            )
            self.assertEqual(len(macs), 1)
            self.assertEqual(len(members), 1)
        finally:
            self.vapi.sonic_ext_pbh_table_del(table_index=other)

    def test_detach_drops_shadow(self):
        """Detaching the last member drops the BVI shadow"""
        for i in self.attached:
            self.vapi.sonic_ext_pbh_interface_attach_detach(
                sw_if_index=i.sw_if_index,
                table_index=self.table_index,
                is_attach=False,
            )

        out = self.vapi.cli("show sonic-ext pbh interfaces")
        self.assertNotIn("bridge domain shadow", out)
        self.assertNotIn("%s: table" % self.bvi.name, out)

        members, macs, total = self._observe(
            self.pg0, self._stream("vxlan", 4, 4, "inner")
        )
        self.assertEqual(total, N_PKTS)
        self.assertEqual(len(macs), 1, "still steering after detach: %s" % macs)
        self.assertEqual(len(members), 1, "still steering after detach: %s" % members)

        # Re-attach so tearDown's table_del drains a populated attachment list.
        for i in self.attached:
            self.vapi.sonic_ext_pbh_interface_attach_detach(
                sw_if_index=i.sw_if_index,
                table_index=self.table_index,
                is_attach=True,
            )


class TestSonicExtPbhEcmpOnly(SonicExtPbhBase):
    """SONiC-ext PBH: SET_ECMP_HASH alone leaves the LAG hash untouched"""

    WANT_ECMP = True
    WANT_LAG = False

    def _assert_ecmp_only(self, af):
        # Inner addresses move the ECMP bucket...
        members, macs, total = self._observe(
            self.pg0, self._stream("vxlan", af, af, "addrs")
        )
        self.assertEqual(total, N_PKTS)
        self.assertEqual(len(macs), 2, "ECMP did not spread: macs=%s" % macs)

        # ...but nothing inner may move the bond member.  With no side-band
        # registered the member follows hash-eth-l34 over the constant outer
        # header, and VXLAN is the clean control: the inner-aware peek in
        # patch 0011 deliberately skips it.
        members, macs, total = self._observe(
            self.pg0, self._stream("vxlan", af, af, "ports")
        )
        self.assertEqual(total, N_PKTS)
        self.assertEqual(len(macs), 1, "inner ports leaked into ECMP: %s" % macs)
        self.assertEqual(
            len(members),
            1,
            "LAG hash was overridden without SET_LAG_HASH: %s" % members,
        )

    def test_ecmp_only_v4(self):
        """PBH SET_ECMP_HASH only, outer-v4/inner-v4"""
        self._assert_ecmp_only(4)

    def test_ecmp_only_v6(self):
        """PBH SET_ECMP_HASH only, outer-v6/inner-v6"""
        self._assert_ecmp_only(6)

    def test_trace_names_one_action(self):
        """PBH trace names set-ecmp-hash alone"""
        self._send(self.pg0, self._stream("vxlan", 4, 4, "inner", n=4), trace=True)
        for t in self._pbh_traces():
            self.assertEqual(t["action"], "set-ecmp-hash")


class TestSonicExtPbhLagOnly(SonicExtPbhBase):
    """SONiC-ext PBH: SET_LAG_HASH alone leaves the ECMP hash untouched"""

    WANT_ECMP = False
    WANT_LAG = True

    def _assert_lag_only(self, af):
        # Inner ports move the bond member...
        members, macs, total = self._observe(
            self.pg0, self._stream("vxlan", af, af, "ports")
        )
        self.assertEqual(total, N_PKTS)
        self.assertEqual(len(members), 2, "LAG did not spread: members=%s" % members)

        # ...but nothing inner may move the ECMP bucket: without
        # SET_ECMP_HASH the packet falls through to ip4-lookup, which
        # recomputes from the constant outer header.
        members, macs, total = self._observe(
            self.pg0, self._stream("vxlan", af, af, "addrs")
        )
        self.assertEqual(total, N_PKTS)
        self.assertEqual(
            len(macs), 1, "ECMP was overridden without SET_ECMP_HASH: %s" % macs
        )
        self.assertEqual(
            len(members), 1, "inner addresses leaked into LAG: %s" % members
        )

    def test_lag_only_v4(self):
        """PBH SET_LAG_HASH only, outer-v4/inner-v4"""
        self._assert_lag_only(4)

    def test_lag_only_v6(self):
        """PBH SET_LAG_HASH only, outer-v6/inner-v6"""
        self._assert_lag_only(6)

    def test_trace_names_one_action(self):
        """PBH trace names set-lag-hash alone and resolves no adjacency"""
        self._send(self.pg0, self._stream("vxlan", 4, 4, "inner", n=4), trace=True)
        for t in self._pbh_traces():
            self.assertEqual(t["action"], "set-lag-hash")
            self.assertIsNone(t["dpo"], "a LAG-only rule must not steer ECMP")


class TestSonicExtPbhApi(SonicExtPbhBase):
    """SONiC-ext PBH: control-plane validation and lifecycle"""

    def test_profile_rejects_empty_field_set(self):
        """PBH rejects a hash profile with no fields"""
        with self.vapi.assert_negative_api_retval():
            self._profile_add_del(is_add=True, profile_index=NO_INDEX, fields=[])

    def test_profile_rejects_unknown_field(self):
        """PBH rejects an out-of-range hash field id"""
        with self.vapi.assert_negative_api_retval():
            self._profile_add_del(
                is_add=True,
                profile_index=NO_INDEX,
                fields=[{"field": 7, "sequence_id": 1, "mask": MASK_NONE}],
            )

    def test_profile_delete_rejects_unknown_index(self):
        """PBH rejects deleting a hash profile that does not exist"""
        with self.vapi.assert_negative_api_retval():
            self._profile_add_del(is_add=False, profile_index=4096, fields=[])

    def test_table_rejects_action_without_encap(self):
        """PBH rejects a rule that has an action but matches no encapsulation

        sonic_ext_pbh_encap_from_match() derives ENCAP_NONE when the rule
        names neither a UDP port nor a tunnel protocol, and there is then
        nothing to hash.
        """
        bad = self._rule(1, 10, "vxlan", 4, 4, self.ecmp_profile, self.lag_profile)
        bad["qualifiers"] &= ~(Q_IP_PROTOCOL | Q_L4_DST_PORT)
        bad["ip_protocol"] = 0
        bad["l4_dst_port"] = 0
        with self.vapi.assert_negative_api_retval():
            self._table_add_replace(table_index=NO_INDEX, name="bad", rules=[bad])

    def test_table_rejects_unknown_profile(self):
        """PBH rejects a rule naming a hash profile that does not exist"""
        bad = self._rule(1, 10, "vxlan", 4, 4, 4096, NO_INDEX)
        with self.vapi.assert_negative_api_retval():
            self._table_add_replace(table_index=NO_INDEX, name="bad", rules=[bad])

    def test_table_delete_rejects_unknown_index(self):
        """PBH rejects deleting a table that does not exist"""
        with self.vapi.assert_negative_api_retval():
            self.vapi.sonic_ext_pbh_table_del(table_index=4096)

    def test_attach_rejects_unknown_interface(self):
        """PBH rejects attaching a table to an interface that does not exist"""
        with self.vapi.assert_negative_api_retval():
            self.vapi.sonic_ext_pbh_interface_attach_detach(
                sw_if_index=4096, table_index=self.table_index, is_attach=True
            )

    def test_reattach_same_table_is_a_noop(self):
        """Re-attaching the same PBH table does not change the shadow"""
        before = self.vapi.cli("show sonic-ext pbh interfaces")
        self.vapi.sonic_ext_pbh_interface_attach_detach(
            sw_if_index=self.pg0.sw_if_index,
            table_index=self.table_index,
            is_attach=True,
        )
        self.assertEqual(before, self.vapi.cli("show sonic-ext pbh interfaces"))

    def test_rebind_detaches_the_previous_table(self):
        """Attaching a second PBH table to a bound port rebinds it"""
        rules, _ = self._build_rules(self.ecmp_profile, self.lag_profile)
        r = self._table_add_replace(
            table_index=NO_INDEX, name="pbh_table_b", rules=rules
        )
        other = r.table_index
        try:
            self.vapi.sonic_ext_pbh_interface_attach_detach(
                sw_if_index=self.pg0.sw_if_index,
                table_index=other,
                is_attach=True,
            )
            out = self.vapi.cli("show sonic-ext pbh interfaces")
            self.assertIn("%s: table %u" % (self.pg0.name, other), out)
            self.assertNotIn("%s: table %u" % (self.pg0.name, self.table_index), out)
        finally:
            self.vapi.sonic_ext_pbh_table_del(table_index=other)

    def test_table_del_drains_attachments(self):
        """Deleting a PBH table detaches every interface it was bound to"""
        self.vapi.sonic_ext_pbh_table_del(table_index=self.table_index)
        self.table_index = None

        out = self.vapi.cli("show sonic-ext pbh interfaces")
        for i in self.attached:
            self.assertNotIn("%s: table" % i.name, out)
        self.assertNotIn("bridge domain shadow", out)

    def test_show_pbh_sections(self):
        """show sonic-ext pbh accepts its sub-keywords and rejects others"""
        self.assertIn("hash profiles:", self.vapi.cli("show sonic-ext pbh profiles"))
        self.assertIn("tables:", self.vapi.cli("show sonic-ext pbh tables"))
        self.assertIn("interfaces:", self.vapi.cli("show sonic-ext pbh interfaces"))
        self.assertIn("hits", self.vapi.cli("show sonic-ext pbh"))
        with self.assertRaisesRegex(CliFailedCommandError, "unknown input `bogus'"):
            self.vapi.cli("show sonic-ext pbh bogus")


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
