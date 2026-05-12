# VLAN Bridge Domain with BVI — High-Level Design

## 1. Problem Statement

SONiC on VPP needs to support L2 bridging with L3 SVI (VLAN interfaces) for both
tagged and untagged VLAN members. The standard SONiC data model creates:

- A VLAN (e.g., Vlan10)
- An SVI with an IP address (e.g., 10.0.0.1/24 on Vlan10)
- Member ports with tagging mode (tagged or untagged)

VPP's bridge-domain (BD) and BVI (Bridge Virtual Interface) constructs map to this
model, but require explicit wiring for two distinct traffic classes:

1. **Control-plane protocols (ARP, LLDP, LACP, DHCP)** — must be punted directly
   from BD member interfaces to the Linux control plane so that per-port services
   (LLDP/LACP agents, DHCP relay) receive frames on the correct interface.

2. **L3 traffic over the VLAN SVI IP** — IP unicast destined to the SVI address
   (e.g., ping, SSH, routing protocols) must traverse the BVI and be punted to
   the kernel `Vlan10` interface via the BVI LCP pair.

### Challenges

1. **BVI ↔ Linux connectivity**: VPP's BVI has no automatic path to the Linux
   network stack. IP packets destined to the SVI must reach the kernel `Vlan10`
   interface.

2. **Per-port punt for control protocols**: ARP, LLDP, LACP, and DHCP must be
   punted directly from the ingress BD member interface so that Linux daemons
   (lldpd, teamd, dhcrelay) receive frames on the correct port. Relying on
   BVI-path flooding would deliver these frames on `Vlan10` without port context.

3. **Clone vs. consume**: ARP and DHCP require clone-on-hit — a copy is punted
   to the control plane while the original continues through the BD for normal
   L2 flooding and BVI processing. LLDP and LACP are consumed (punt only, no
   clone) because they are link-local and must not be forwarded in the BD.

4. **Tagged member dispatch**: VPP must create dot1q sub-interfaces for tagged
   members, strip the tag on ingress to the BD, and push it back on egress.

5. **Untagged member handling**: The physical interface itself joins the BD
   directly — no sub-interface or VLAN tag rewrite needed.

6. **Promiscuous mode**: VPP's virtio/DPDK backend filters tagged frames at
   the device level unless promiscuous mode is enabled on the parent interface.

---

## 2. SONiC Configuration Example

```json
// config_db.json excerpts

"VLAN": {
    "Vlan10": { "vlanid": "10" }
}

"VLAN_INTERFACE": {
    "Vlan10": {},
    "Vlan10|10.0.0.1/24": {}
}

"VLAN_MEMBER": {
    "Vlan10|Ethernet0": { "tagging_mode": "tagged" },
    "Vlan10|Ethernet4": { "tagging_mode": "untagged" }
}
```

CLI equivalents:
```bash
config vlan add 10
config vlan member add 10 Ethernet0    # tagged by default
config vlan member add -u 10 Ethernet4 # untagged
config interface ip add Vlan10 10.0.0.1/24
```

---

## 3. Design Principles

### 3.1 BVI LCP Pair (L3 SVI Access)

The BVI (`bvi10`) is the L3 endpoint of the bridge domain. An LCP (Linux Control
Plane) pair is created between `bvi10` and a Linux tap device (`tap_Vlan10`). A tc
filter redirects traffic between `tap_Vlan10` and the kernel `Vlan10` netdev.

The BVI LCP pair handles **L3 traffic destined to the SVI IP**:
- **IP unicast** to the SVI IP (ping, SSH, routing) reaches the BVI via L2
  forwarding, transitions to L3 via `bvi-input`, and is punted to Vlan10.
- **Routed traffic** from the kernel exits through Vlan10 → tap → BVI → BD.
- **Inter-VLAN routing** uses the BVI as the L3 gateway.

### 3.2 L2 Classifier-Based Punt (Control Protocols)

Control-plane protocols (ARP, LLDP, LACP, DHCP) are punted **directly from BD
member interfaces** using VPP's `l2-input-classify` feature with the
`linux-cp-punt` node as the target. This ensures per-port delivery to the
Linux control plane.

Key design decisions:

- **No new VPP graph nodes** — the implementation uses existing VPP
  infrastructure: `l2-input-classify` (built-in L2 feature, bit 18) with
  `linux-cp-punt` as hit-next target.

- **Clone-on-hit** — a VPP patch adds clone support to `l2-input-classify`.
  When a classify session has `opaque_index = 1`, the buffer is cloned via
  `vlib_buffer_copy()`: the clone is sent to `linux-cp-punt` and the original
  continues through the L2 feature chain (VTR → learn → fwd → flood).

- **Punt vs. clone per protocol**:
  - **LLDP/LACP** (`opaque_index=0`): punt/consume — the original is
    redirected to `linux-cp-punt` and does not continue in the BD. These are
    link-local protocols that must not be L2-forwarded. Note: this only
    applies to **untagged** members (see below).
  - **ARP/DHCP** (`opaque_index=1`): clone-on-hit — a copy is punted for
    the control plane while the original continues through the BD for normal
    flooding/forwarding/BVI processing.

- **LLDP/LACP on tagged members follow the regular LCP path** —
  `ethernet-input` dispatches frames by outer ethertype. LLDP (0x88CC) and
  LACP (0x8809) are **not** 0x8100, so they are **not** dispatched to the
  dot1q sub-interface. Instead, they stay on the parent physical interface
  (which is not in the BD) and follow the normal LCP punt path:
  `dpdk-input → ethernet-input → linux-cp-punt-xc → tap`. The classifier
  on the sub-interface never sees these frames. Therefore, the tagged
  classifier tables only need sessions for ARP and DHCP.

- **Classifier runs before VTR** — `l2-input-classify` (bit 18) has higher
  priority than `l2-input-vtr` (bit 11) in the L2 feature bitmap. For tagged
  members, the 802.1Q header is still present when the classifier matches.

### 3.3 Classifier Table Design

Four shared classify tables are created lazily on the first BD member add and
persist for the lifetime of the process. A single `linux-cp-punt` next-index
is resolved via `vpp_add_node_next("l2-input-classify", "linux-cp-punt")`.

#### 3.3.1 Untagged Member Tables

Untagged frames have the ethertype at byte offset 12 (standard Ethernet).

**Table: untag_other** — matches ethertype directly (skip=0, match=1)

| Byte Offset | Field | Mask |
|-------------|-------|------|
| 12–13 | Ethertype | `0xFFFF` |

Sessions:

| Protocol | Ethertype | opaque_index | Action |
|----------|-----------|-------------|--------|
| LLDP | `0x88CC` | 0 | punt (consume) |
| LACP | `0x8809` | 0 | punt (consume) |
| ARP | `0x0806` | 1 | clone + punt |

Miss: continue L2 feature chain (to VTR, learn, fwd).

**Table: untag_ip4** — matches IP protocol + UDP dport (skip=1, match=2)

This table catches DHCP among IPv4 packets (ethertype `0x0800`).

| Byte Offset | Field | Mask |
|-------------|-------|------|
| 23 | IP Protocol | `0xFF` |
| 36–37 | UDP Dest Port | `0xFFFF` |

Sessions:

| Protocol | Proto | DPort | opaque_index | Action |
|----------|-------|-------|-------------|--------|
| DHCP | 0x11 (UDP) | 67 (0x0043) | 1 | clone + punt |

Miss: continue L2 feature chain.

VPP session match data for skip=1 tables includes 16 bytes of skip padding
at the start: `match_len = (skip + match) × 16 = 48 bytes`.

#### 3.3.2 Tagged Member Tables

Tagged frames have the outer ethertype `0x8100` at byte 12, so VPP's
`l2-input-classify` dispatches all tagged frames to the **other** table slot.
The inner (payload) ethertype is at byte offset 16.

**Table: tag_other** — matches inner ethertype (skip=1, match=1)

Chained to `tag_dhcp` table on miss.

| Byte Offset | Field | Mask |
|-------------|-------|------|
| 16–17 | Inner Ethertype | `0xFFFF` |

Sessions:

| Protocol | Inner Etype | opaque_index | Action |
|----------|-------------|-------------|--------|
| ARP | `0x0806` | 1 | clone + punt |

Note: LLDP/LACP sessions are **not** needed in the tagged tables because
`ethernet-input` does not dispatch LLDP (0x88CC) or LACP (0x8809) frames
to the dot1q sub-interface — they stay on the parent and follow the regular
LCP punt path.

Miss: chain to `tag_dhcp` table.

**Table: tag_dhcp** — matches inner etype + IP protocol + UDP dport (skip=1, match=2)

| Byte Offset | Field | Mask |
|-------------|-------|------|
| 16–17 | Inner Ethertype | `0xFFFF` |
| 27 | IP Protocol | `0xFF` |
| 40–41 | UDP Dest Port | `0xFFFF` |

Sessions:

| Protocol | Inner Etype | Proto | DPort | opaque_index | Action |
|----------|-------------|-------|-------|-------------|--------|
| DHCP | `0x0800` | 0x11 | 67 | 1 | clone + punt |

Miss: continue L2 feature chain.

#### 3.3.3 Table Attachment

When a BD member is added:
- **Tagged member** (sub-interface, e.g., `bobm0.10`):
  `classify_set_interface_l2_tables(bobm0.10, ip4=~0, ip6=~0, other=tag_other)`
- **Untagged member** (parent phy, e.g., `bobm0`):
  `classify_set_interface_l2_tables(bobm0, ip4=untag_ip4, ip6=~0, other=untag_other)`

### 3.4 Frames in the BD Are Always Untagged

All frames inside the bridge domain are **untagged**:
- Tagged members: VPP sub-interface pops the outer tag on ingress (`pop 1`)
  and pushes it back on egress (symmetric VTR)
- Untagged members: frames arrive without a tag and stay untagged
- BVI: exchanges untagged frames with the BD (no VTR on BVI)

This matches the Linux kernel model where the `Vlan10` SVI sees untagged frames.

### 3.5 Promiscuous Mode on Physical Interfaces

VPP's virtio/DPDK backend implements `VIRTIO_NET_F_CTRL_VLAN` which filters
tagged frames at the device level. Promiscuous mode is enabled on every
physical interface at LCP pair creation time so that tagged frames pass
through to VPP's `ethernet-input` for sub-interface dispatch.

### 3.6 Auto Sub-Interface (lcp-auto-subint)

VPP's linux-cp plugin `lcp-auto-subint` feature is enabled. When SAI creates
a VPP sub-interface (e.g., `bobm0.10`), linux-cp automatically creates the
corresponding Linux VLAN device (`Ethernet0.10`) on the host side. This
eliminates the need for explicit `configure_lcp_interface()` calls for
sub-interfaces.

---

## 4. Implementation Changes

### 4.1 BVI LCP Pair + TC Filter

**File**: `SwitchVppFdb.cpp` — `vpp_create_bvi_interface()`

When a VLAN SVI is created (SAI `ROUTER_INTERFACE_TYPE_VLAN`):
1. Create BVI: `create_bvi_interface(mac, vlan_id)`
2. Add BVI to BD: `set_sw_interface_l2_bridge(bvi10, vlan_id, true, BVI)`
3. Create LCP pair: `configure_lcp_interface("bvi10", "tap_Vlan10", true)`
4. Bring up the tap: `interface_set_state("tap_Vlan10", true)`
5. TC redirect: `add_tc_filter_redirect("tap_Vlan10", "Vlan10")`

Teardown mirrors creation in reverse order.

### 4.2 Tagged Member: Sub-Interface + VTR Pop-1 + Classifier

**File**: `SwitchVppFdb.cpp` — `vpp_create_vlan_member()` (TAGGED path)

1. Create VPP sub-interface: `create_sub_interface(bobm0, 10, 10)`
   - `lcp-auto-subint` automatically creates `Ethernet0.10` on the host
2. Add sub-interface to BD: `set_sw_interface_l2_bridge(bobm0.10, 10, true, NORMAL)`
3. Set VTR pop-1: `set_l2_interface_vlan_tag_rewrite(bobm0.10, 10, ~0, DOT1Q, POP_1)`
4. Admin up: `interface_set_state(bobm0.10, true)`
5. Attach classifier: `l2_punt_classify_apply(bobm0.10, true /*tagged*/)`

### 4.3 Untagged Member: Parent Interface in BD + Classifier

**File**: `SwitchVppFdb.cpp` — `vpp_create_vlan_member()` (UNTAGGED path)

1. Add parent phy directly to BD: `set_sw_interface_l2_bridge(bobm0, 10, true, NORMAL)`
2. Attach classifier: `l2_punt_classify_apply(bobm0, false /*untagged*/)`

No sub-interface or VTR needed — wire frames are already untagged.

### 4.4 Classifier Initialization

**File**: `SwitchVppFdb.cpp` — `l2_punt_classify_init()`

Lazily creates the four shared classify tables and sessions described in
Section 3.3. Called automatically on the first BD member add.

### 4.5 Clone-on-Hit VPP Patch

**File**: `platform/vpp/vppbld/patches/0010-l2-input-classify-clone-on-hit.patch`

Adds clone support to `l2-input-classify`. When a session match has
`opaque_index == L2_CLASSIFY_OPAQUE_CLONE (1)`:
1. Clone the buffer via `vlib_buffer_copy()`
2. Send clone to `hit_next_index` (linux-cp-punt)
3. Original buffer continues through the L2 feature chain

### 4.6 VPP API Wrappers

**File**: `SaiVppXlate.c`

New functions for the classify binary API:
- `vpp_classify_table_create()` — create table with mask
- `vpp_classify_table_delete()` — delete table
- `vpp_classify_session_add()` — add session with match/opaque
- `vpp_classify_session_del()` — delete session
- `vpp_classify_set_interface_l2_tables()` — attach/detach tables on interface
- `vpp_add_node_next()` — resolve next-node index

All use the `M`/`M22` macros for socket-aware message allocation.

### 4.7 Promiscuous Mode on Every Physical Interface

**File**: `SwitchVppHostif.cpp`

At LCP pair creation for each physical port:
```cpp
configure_lcp_interface(hwif_name, dev, true);
interface_set_promiscuous(hwif_name, true);  // <-- added
```

---

## 5. Packet Flow

### 5.1 ARP Request from Tagged Member

A host on tagged member Ethernet0 (bobm0.10) sends an ARP broadcast:

```
Wire (802.1Q tag=10, ARP who-has 10.0.0.1, dst ff:ff:ff:ff:ff:ff)
  → bobm0 (dpdk-input, promisc on)
    → ethernet-input: dot1q tag=10 → dispatches to bobm0.10
      → l2-input (BD 10)
        → l2-input-classify: outer etype=0x8100 → "other" table
          → tag_other: inner etype=0x0806 (ARP) → HIT, opaque=1
            → clone-on-hit:
              CLONE → linux-cp-punt → Ethernet0 tap (with 802.1Q tag)
                → kernel receives ARP on Ethernet0 with VLAN tag
                  → kernel ARP daemon replies (if IP is local)
              ORIGINAL → continues L2 feature chain:
                → l2-input-vtr: POP tag
                  → l2-learn → l2-fwd → l2-flood → bvi10
                    → bvi-input → arp-input → linux-cp (Vlan10)
```

ARP reaches the kernel **twice**: once via the clone (on `Ethernet0` with
VLAN tag, for per-port visibility) and once via the BVI path (on `Vlan10`,
untagged).

### 5.2 ARP Request from Untagged Member

A host on untagged member Ethernet4 (bobm1) sends an ARP broadcast:

```
Wire (no tag, ARP who-has 10.0.0.1, dst ff:ff:ff:ff:ff:ff)
  → bobm1 (dpdk-input, promisc on)
    → ethernet-input: no tag → parent sw_if_index (bobm1)
      → l2-input (BD 10)
        → l2-input-classify: etype=0x0806 → "other" table
          → untag_other: etype=0x0806 (ARP) → HIT, opaque=1
            → clone-on-hit:
              CLONE → linux-cp-punt → Ethernet4 tap (untagged)
                → kernel receives ARP on Ethernet4
              ORIGINAL → continues L2 feature chain:
                → l2-learn → l2-fwd → l2-flood → bvi10
                  → bvi-input → arp-input → linux-cp (Vlan10)
```

### 5.3 LLDP/LACP from Tagged Member

LLDP and LACP use link-local ethertypes (0x88CC, 0x8809) — **not** 0x8100.
When a tagged member port receives an LLDP frame, `ethernet-input` sees
ethertype 0x88CC and does **not** dispatch it to the dot1q sub-interface.
The frame stays on the parent physical interface, which has an LCP pair
but is not in the BD. It follows the regular LCP punt path:

```
Wire (LLDP 0x88CC, no 802.1Q encapsulation — LLDP is always untagged)
  → bobm0 (dpdk-input, promisc on)
    → ethernet-input: etype=0x88CC, hw-if-index=1, sw-if-index=1 (parent)
      → linux-cp-punt-xc: sw_if_index 1 → tap (Ethernet0)
        → kernel lldpd/teamd processes the frame on Ethernet0
```

The classifier on the sub-interface (`bobm0.10`) never sees LLDP/LACP.
This is the standard behavior — LLDP is a link-layer protocol that is
not VLAN-tagged on the wire.

### 5.4 LLDP/LACP from Untagged Member

```
Wire (no tag, LLDP 0x88CC)
  → bobm1 → ethernet-input → bobm1
    → l2-input (BD 10)
      → l2-input-classify: etype=0x88CC → "other" table
        → untag_other: etype=0x88CC → HIT, opaque=0
          → punt (consume): → linux-cp-punt → Ethernet4 tap
            → kernel lldpd/teamd processes the frame
```

### 5.5 DHCP Request from Untagged Member

DHCP discover from a client on untagged member Ethernet4:

```
Wire (no tag, IPv4, UDP src=68 dst=67, 0.0.0.0 → 255.255.255.255)
  → bobm1 → ethernet-input → bobm1
    → l2-input (BD 10)
      → l2-input-classify: etype=0x0800 → "ip4" table
        → untag_ip4: proto=0x11, dport=67 → HIT, opaque=1
          → clone-on-hit:
            CLONE → linux-cp-punt → Ethernet4 tap (untagged)
              → kernel receives DHCP on Ethernet4
                → Linux bridge floods to all member taps
                  → dhcrelay on Vlan10 picks up the broadcast
                    → relay adds option 82, forwards to DHCP server
            ORIGINAL → continues L2 feature chain:
              → l2-learn → l2-fwd → l2-flood → bvi10
                → (IP broadcast to 255.255.255.255, handled by BD flood)
```

The DHCP relay process (`dhcrelay -id Vlan10 -iu docker0 <server-ip>`)
listens for DHCP broadcasts. The clone delivers the packet into the Linux
network stack where it reaches the relay via the Linux bridge that connects
member taps and `Vlan10`.

### 5.6 DHCP Request from Tagged Member

```
Wire (802.1Q tag=10, IPv4, UDP src=68 dst=67)
  → bobm0 → ethernet-input → bobm0.10
    → l2-input (BD 10)
      → l2-input-classify: outer etype=0x8100 → "other" table
        → tag_other: inner etype=0x0800 → MISS → chain to tag_dhcp
          → tag_dhcp: inner etype=0x0800, proto=0x11, dport=67 → HIT, opaque=1
            → clone-on-hit:
              CLONE → linux-cp-punt → Ethernet0 tap (with 802.1Q tag)
                → kernel receives tagged DHCP on Ethernet0
              ORIGINAL → continues L2 feature chain:
                → l2-input-vtr: POP tag → l2-learn → l2-flood → bvi10
```
Note: SONiC control plane doesn't support DHCP over tagged member interface.

### 5.7 IP Unicast to SVI (e.g., ping to Vlan10 IP)

L3 traffic destined to the SVI IP does **not** use the classifier — it flows
through normal L2 forwarding to the BVI, then through the BVI LCP pair.

```
Wire (tagged, dst-MAC = BVI MAC, dst-IP = 10.0.0.1) → bobm0 (dpdk-input)
  → ethernet-input → bobm0.10
    → l2-input (BD 10)
      → l2-input-classify: etype=0x0800, ip4 → untag_ip4 or tag_dhcp
        → no session match (not DHCP) → MISS → continue
      → l2-input-vtr: POP tag
        → l2-learn → l2-fwd: dst MAC known on bvi10
          → l2-output → bvi10
            → bvi-input: L2→L3 transition
              → ip4-input → ip4-lookup → ip4-local
                → linux-cp-punt-xc → tap_Vlan10 → tc → Vlan10 (kernel)
```

For an **untagged** member the path is the same except no VTR pop.

### 5.8 Routed Traffic with BVI as Next-Hop (Inter-VLAN / External)

When a host in BD 10 sends IP traffic to a destination on a **different
subnet**, the host's default gateway is the BVI IP. The frame has dst-MAC =
BVI MAC and is L2-forwarded to the BVI.

```
Wire (dst-MAC = BVI MAC, dst-IP = 10.0.20.1) → bobm0.10
  → l2-input (BD 10) → classifier miss → VTR POP
    → l2-learn → l2-fwd: dst MAC = BVI MAC → bvi10
      → bvi-input: L2→L3
        → ip4-input → ip4-lookup:
          dst 10.0.20.1 → next-hop via bvi20 (inter-VLAN)
                        → or next-hop via bobm2 (L3 routed port)
```

### 5.9 L2 Unicast Forwarding Between Members

Normal L2 forwarding is unaffected by the classifier (miss path):

```
bobm0.10 (ingress, tagged) → l2-input → classifier miss → POP tag
  → l2-learn + l2-fwd → dst MAC on bobm1 (untagged):
    → l2-output → no VTR → bobm1 → wire (untagged)

bobm1 (ingress, untagged) → l2-input → classifier miss
  → l2-learn + l2-fwd → dst MAC on bobm0.10 (tagged):
    → l2-output → l2-output-vtr: PUSH tag 10 → bobm0.10 → wire (tagged)
```

---

## 6. VPP Configuration Summary (Runtime State)

After SONiC applies the configuration above, VPP state looks like:

```
# Bridge domain 10 with BVI, tagged member, untagged member
vppctl show bridge-domain 10 detail
  BD-ID 10, flood, learn
    bvi10        (BVI, sw_if_index 25)
    bobm0.10     (tagged member, sw_if_index 23, vtr pop-1)
    bobm1        (untagged member, sw_if_index 3)

# LCP pairs
vppctl show lcp
  bvi10     → tap_Vlan10
  bobm0     → Ethernet0      (physical)
  bobm0.10  → Ethernet0.10   (auto-subint)
  bobm1     → Ethernet4      (physical)

# Classifier tables
vppctl show classify tables
  Table 0 (untag_other): skip=0 match=1 sessions=3
  Table 1 (untag_ip4):   skip=1 match=2 sessions=1
  Table 2 (tag_dhcp):    skip=1 match=2 sessions=1
  Table 3 (tag_other):   skip=1 match=1 sessions=3, next_table=2

# Classifier attachment
vppctl show classify interface
  bobm0.10: ip4=~0 ip6=~0 other=3 (tag_other)
  bobm1:    ip4=1 (untag_ip4) ip6=~0 other=0 (untag_other)

# Promiscuous mode
vppctl show interface bobm0
  flags: ... promisc ...
```

---

## 7. IPv6 Neighbor Discovery (Future Work)

IPv6 Neighbor Discovery (ND) punt is not covered in this design and will be
addressed in a separate document. Two options are under consideration:

1. **VPP built-in ND handling**: VPP's `ip6-neighbor-discovery` and linux-cp
   plugin can handle ND natively without involving the SONiC control plane.
   VPP would respond to NS/NA on the BVI and program neighbor entries directly.

2. **Classifier-based punt**: Extend the classifier tables to match IPv6 ND
   packets (Next Header=0x3A ICMPv6, Type=0x87 NS / 0x88 NA) and punt/clone
   them to the Linux control plane, similar to the ARP approach. This would
   require additional ip6 table slots.

---

## 8. Files Modified

| File | Change |
|------|--------|
| `platform/vpp/docker-syncd-vpp/conf/startup.conf.tmpl` | Enable `lcp-auto-subint` |
| `platform/vpp/docker-sonic-vpp/conf/startup.conf.tmpl` | Enable `lcp-auto-subint` |
| `platform/vpp/vppbld/patches/0010-l2-input-classify-clone-on-hit.patch` | Clone-on-hit for `l2-input-classify` |
| `src/sonic-sairedis/vslib/vpp/vppxlate/SaiVppXlate.c` | Classify API wrappers, `interface_set_promiscuous()` |
| `src/sonic-sairedis/vslib/vpp/vppxlate/SaiVppXlate.h` | Extern declarations |
| `src/sonic-sairedis/vslib/vpp/SwitchVppFdb.cpp` | BVI LCP pair, classifier init/apply/remove, tagged/untagged member handling |
| `src/sonic-sairedis/vslib/vpp/SwitchVppRif.cpp` | SUB_PORT RIF: rely on auto-subint |
| `src/sonic-sairedis/vslib/vpp/SwitchVppHostif.cpp` | Promisc on every phy at LCP creation |
| `src/sonic-sairedis/vslib/vpp/SwitchVpp.h` | `m_bvi_vlan_lcp_map` member |
