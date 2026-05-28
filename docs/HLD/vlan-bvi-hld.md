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

### 2.1 ASIC_DB Representation

The CONFIG_DB configuration above flows through orchagent and surfaces in ASIC_DB as the following SAI objects. The sonic-vpp SAI layer translates each of these into VPP bridge-domain, sub-interface, BVI, LCP, and classify operations.

#### Bridge object (SAI 1Q bridge)

```
ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE:oid:0x39000000000001
    SAI_BRIDGE_ATTR_TYPE = SAI_BRIDGE_TYPE_1Q
```

Created implicitly at boot. The default 1Q bridge is the container for VLANs.

#### VLAN object

```
ASIC_STATE:SAI_OBJECT_TYPE_VLAN:oid:0x2600000000064f
    SAI_VLAN_ATTR_VLAN_ID = 10
```

→ sonic-vpp creates VPP bridge-domain id 10 and BVI `bvi10`.

#### VLAN member objects (one per port)

```
ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER:oid:0x270000000007a0
    SAI_VLAN_MEMBER_ATTR_VLAN_ID            = oid:0x2600000000064f
    SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID     = oid:0x3a00000000079f   # bridge port for Ethernet0
    SAI_VLAN_MEMBER_ATTR_VLAN_TAGGING_MODE  = SAI_VLAN_TAGGING_MODE_TAGGED

ASIC_STATE:SAI_OBJECT_TYPE_VLAN_MEMBER:oid:0x270000000007a1
    SAI_VLAN_MEMBER_ATTR_VLAN_ID            = oid:0x2600000000064f
    SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID     = oid:0x3a00000000079e   # bridge port for Ethernet4
    SAI_VLAN_MEMBER_ATTR_VLAN_TAGGING_MODE  = SAI_VLAN_TAGGING_MODE_UNTAGGED
```

→ sonic-vpp action:
- **Tagged** (`Ethernet0`): create `GigabitEthernet0/8/0.10` dot1q sub-interface, set `l2 tag-rewrite pop 1`, add to BD 10.
- **Untagged** (`Ethernet4`): add the parent hardware interface `GigabitEthernet0/8/1` directly to BD 10.
- Install l2-input-classify sessions on the member's `sw_if_index` (per the protocol matrix in §3.3).

#### Bridge port objects (one per port that participates in any bridge)

```
ASIC_STATE:SAI_OBJECT_TYPE_BRIDGE_PORT:oid:0x3a00000000079f
    SAI_BRIDGE_PORT_ATTR_TYPE          = SAI_BRIDGE_PORT_TYPE_PORT
    SAI_BRIDGE_PORT_ATTR_PORT_ID       = oid:0x10000000000004        # Ethernet0
    SAI_BRIDGE_PORT_ATTR_BRIDGE_ID     = oid:0x39000000000001
    SAI_BRIDGE_PORT_ATTR_ADMIN_STATE   = true
    SAI_BRIDGE_PORT_ATTR_FDB_LEARNING_MODE = SAI_BRIDGE_PORT_FDB_LEARNING_MODE_HW
```

→ sonic-vpp records the port-to-BD relationship; actual `set int l2 bridge` is deferred until VLAN_MEMBER object resolves which BD and which sub-interface to use.

#### Router interface object (the SVI / BVI)

```
ASIC_STATE:SAI_OBJECT_TYPE_ROUTER_INTERFACE:oid:0x600000000064d
    SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID = oid:0x3000000000040
    SAI_ROUTER_INTERFACE_ATTR_TYPE              = SAI_ROUTER_INTERFACE_TYPE_VLAN
    SAI_ROUTER_INTERFACE_ATTR_VLAN_ID           = oid:0x2600000000064f
    SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS   = 1C:23:CD:51:EB:00
    SAI_ROUTER_INTERFACE_ATTR_MTU               = 9100
```

→ sonic-vpp action: This is the trigger for BVI creation.
- Create `bvi10` (loopback interface with `bvi 1`) in BD 10.
- Set MAC address from `SAI_ROUTER_INTERFACE_ATTR_SRC_MAC_ADDRESS`.
- Create LCP pair `bvi10` ↔ `tap_Vlan10`; set up tc filter `tap_Vlan10` ↔ kernel `Vlan10`.
- Enable IP on the BVI; subsequent `SAI_OBJECT_TYPE_ROUTER_INTERFACE` IP-address attribute updates and `SAI_OBJECT_TYPE_NEIGHBOR_ENTRY` / `SAI_OBJECT_TYPE_ROUTE_ENTRY` operations use this BVI's `sw_if_index`.

#### Hostif object (for the SVI Linux netdev)

```
ASIC_STATE:SAI_OBJECT_TYPE_HOSTIF:oid:0xd000000000a5e
    SAI_HOSTIF_ATTR_TYPE      = SAI_HOSTIF_TYPE_NETDEV
    SAI_HOSTIF_ATTR_OBJ_ID    = oid:0x600000000064d                  # the VLAN RIF
    SAI_HOSTIF_ATTR_NAME      = "Vlan10"
    SAI_HOSTIF_ATTR_OPER_STATUS = true
```

→ sonic-vpp action: The `Vlan10` netdev already exists in the kernel (created by orchagent's host-interface manager); sonic-vpp ensures the LCP tap is wired to it via tc redirect.

#### Putting it together

```
config_db.json
   │
   ▼
orchagent (VlanMgr, IntfMgr, ...)
   │
   ▼
ASIC_DB objects (above)
   │
   ▼
sonic-vpp SAI layer
   ├── SAI_OBJECT_TYPE_VLAN              → create BD 10
   ├── SAI_OBJECT_TYPE_VLAN_MEMBER (tagged)   → dot1q sub-if + tag-rewrite + add to BD + classify sessions
   ├── SAI_OBJECT_TYPE_VLAN_MEMBER (untagged) → parent hwif + add to BD + classify sessions
   ├── SAI_OBJECT_TYPE_ROUTER_INTERFACE  → bvi10 + LCP pair + tc redirect to Vlan10
   └── SAI_OBJECT_TYPE_HOSTIF            → ensure Vlan10 netdev ↔ tap_Vlan10 tc link
```

ASIC_DB does **not** explicitly model:
- Which L2 protocols are punted (ARP/LLDP/LACP/DHCP) — that comes from CoPP / hostif-trap objects (`SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP`, `SAI_OBJECT_TYPE_HOSTIF_TRAP`) which orchagent installs once at boot; sonic-vpp translates them into the `l2-input-classify` sessions described in §3.3.
- BVI vs. non-BVI semantics — that is a property of `SAI_ROUTER_INTERFACE_TYPE_VLAN` (BVI) vs. `SAI_ROUTER_INTERFACE_TYPE_PORT` / `_SUB_PORT` (no BVI).

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

**Table: untag_ip6** — matches IPv6 next-header + ICMPv6 type (skip=2, match=1)

This table catches IPv6 Neighbor Discovery (NS/NA/RS/RA/Redirect) frames
among IPv6 packets (ethertype `0x86DD`). Without this, IPv6 SVI operation
breaks once the BVI is removed from the BD flood group (see §3.7).

| Byte Offset | Field | Mask |
|-------------|-------|------|
| 20 | IPv6 Next-Header | `0xFF` |
| 54 | ICMPv6 Type | `0xFF` |

Sessions (all `opaque_index=1`, clone + punt):

| Protocol | Next-Hdr | ICMPv6 Type |
|----------|----------|-------------|
| RS  | 0x3A | 133 |
| RA  | 0x3A | 134 |
| NS  | 0x3A | 135 |
| NA  | 0x3A | 136 |
| Redirect | 0x3A | 137 |

Miss: continue L2 feature chain.

DHCPv6 (UDP 546/547) is **not** included in v1 — DHCPv6 relay/server on the
SVI is out of scope; if enabled later, add UDP-dport sessions analogous to
the DHCPv4 entry in `untag_ip4`.

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

**Table: tag_ip6** — matches IPv6 next-header + ICMPv6 type with a 4-byte
802.1Q shift (skip=2, match=1)

Same fields as `untag_ip6` but shifted by 4 bytes to account for the outer
802.1Q header (the classifier runs before VTR, so the tag is still present).

| Byte Offset | Field | Mask |
|-------------|-------|------|
| 24 | IPv6 Next-Header | `0xFF` |
| 58 | ICMPv6 Type | `0xFF` |

Sessions: same five ND types as `untag_ip6`, `opaque_index=1`.

#### 3.3.3 Table Attachment

When a BD member is added:
- **Tagged member** (sub-interface, e.g., `bobm0.10`):
  `classify_set_interface_l2_tables(bobm0.10, ip4=~0, ip6=tag_ip6, other=tag_other)`
- **Untagged member** (parent phy, e.g., `bobm0`):
  `classify_set_interface_l2_tables(bobm0, ip4=untag_ip4, ip6=untag_ip6, other=untag_other)`

#### 3.3.4 Adding a New Punted L2-Multicast / Broadcast Protocol

Because the BVI is excluded from the BD flood group (§3.7), **every** new
control protocol whose frames are L2-multicast or broadcast must be punted
by an explicit classifier session. There is no implicit fallback through
the BVI. To add a protocol (e.g., IGMP, MLD, DHCPv6, VRRP):

1. Pick the table slot based on ethertype:
   - `0x0800` (IPv4) → `untag_ip4` / future `tag_ip4`
   - `0x86DD` (IPv6) → `untag_ip6` / `tag_ip6`
   - other / link-local → `untag_other` / `tag_other`
2. If the table's existing mask covers your match fields, just add a session.
   If not, create a new chained table (`next_table_index`) with the right
   mask and link the prior table's miss to it.
3. Choose `opaque_index`:
   - `0` = consume (frame stops in classifier, only the punted copy survives)
     — use only for link-local protocols that must not be L2-forwarded.
   - `1` = clone-on-hit — use for everything else so BD members still see
     the frame.
4. Register the session in `l2_punt_classify_init()` so it is installed
   before any BD member is attached.

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

### 3.7 BVI Excluded from BD Flood Group

By default, VPP adds every BD member — including the BVI — to the BD's
flood group, so broadcast / unknown-unicast / L2-multicast (BUM) frames
from any member are replicated to the BVI and then handed to `ip4-input` /
`ip6-input`. For an SVI port this is wasted work:

- ARP broadcasts (`who-has 10.0.0.1`) are already cloned to the kernel via
  the `l2-input-classify` punt path described in §3.3. The kernel's ARP
  stack on `Vlan10` answers; the BVI does not need to see the flood copy.
- DHCP broadcasts are handled the same way (clone + punt).
- Unknown-unicast and L2-multicast inside the BD have no business reaching
  the L3 SVI — they are pure CPU cost and can confuse the IP input path
  (martian source logs, unwanted multicast joins, etc.).

To prevent this, the BVI is added to the BD with **flood and
unknown-unicast flood disabled**:

| BD-member flag | Normal member | BVI member |
|----------------|---------------|------------|
| `port_type`    | `NORMAL`      | `BVI`      |
| `enable_flood` (broadcast + L2-mcast) | `true` | **`false`** |
| `enable_uu_flood` (unknown unicast)   | `true` | **`false`** |
| `enable_bvi`   | n/a           | `true`     |

Known-unicast frames whose destination MAC matches the BVI's MAC are still
delivered to the BVI via FDB lookup — those are L3-bound packets and must
reach `ip4-input`. Only the flood and UU-flood replications are suppressed.

This is configured via the `sw_interface_set_l2_bridge` binary API with the
`enable_flood` and `enable_uu_flood` bits cleared for the BVI's `sw_if_index`.

**Implication — every kernel-visible BUM protocol needs an explicit
classifier session.** With the BVI no longer in the flood group, the BD has
no catch-all path to the host stack. Any L2-broadcast or L2-multicast frame
the kernel needs to see (ARP, DHCPv4, IPv6 ND, IGMP, MLD, DHCPv6, VRRP,
etc.) must be punted by a session in one of the `l2-input-classify` tables
(§3.3). Frames not matched by a session are flooded only to the other BD
members and **never reach** `Vlan10` or any other SVI tap.

v1 ships sessions for: ARP, DHCPv4, LLDP, LACP, and IPv6 ND
(RS/RA/NS/NA/Redirect). Other protocols are out of scope for v1 and follow
the recipe in §3.3.4 when they are enabled. Link-local L2 protocols (LLDP,
LACP, STP) that travel without an 802.1Q header on tagged members continue
to use the parent-phy LCP path and do not need classifier sessions on the
sub-interface (see §3.2).

---

## 4. Implementation Changes

### 4.1 BVI LCP Pair + TC Filter

**File**: `SwitchVppFdb.cpp` — `vpp_create_bvi_interface()`

When a VLAN SVI is created (SAI `ROUTER_INTERFACE_TYPE_VLAN`):
1. Create BVI: `create_bvi_interface(mac, vlan_id)`
2. Add BVI to BD **with flood disabled** (see §3.7):
   `set_sw_interface_l2_bridge(bvi10, vlan_id, port_type=BVI,
   enable=true, enable_flood=false, enable_uu_flood=false)`
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
                  → l2-learn → l2-fwd → l2-flood
                    (flood group excludes bvi10 — see §3.7 —
                     so the BVI does NOT receive a copy)
                    → replicated only to other BD members
```

ARP reaches the kernel **once**, via the classifier clone on the per-port
`Ethernet0` tap. The BVI is excluded from the BD flood group (§3.7), so the
broadcast is not replicated to `bvi10` and does **not** appear on `Vlan10`.

> **Note on ARP replies for the SVI IP (`10.0.0.1`).** Because the cloned
> ARP request lands on `Ethernet0` rather than `Vlan10`, the kernel must
> still be able to answer for `Vlan10`'s IP. With default sysctls
> (`arp_ignore=0`), Linux will reply for any locally-owned IP regardless
> of the receiving interface; the reply egresses `Ethernet0` and is then
> bridged by VPP back into BD 10 via the LCP path. Implementations should
> validate this path end-to-end and, if needed, either (a) keep the BVI in
> the flood group for broadcast (set `enable_flood=true`, `enable_uu_flood=false`)
> so the kernel sees the request directly on `Vlan10`, or (b) install
> static BD `arp-entry` records and let VPP answer ARP for the SVI IP
> from `bvi10`'s MAC without involving the kernel.

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
                → l2-learn → l2-fwd → l2-flood
                  (flood group excludes bvi10 — see §3.7)
                  → replicated only to other BD members
```

As in §5.1, the kernel sees the ARP exactly once (via the classifier clone
on `Ethernet4`). The BVI does not receive the flood copy.

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
