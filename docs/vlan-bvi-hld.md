# VLAN Bridge Domain on VPP — High-Level Design

## 1. Problem Statement

SONiC on VPP needs to support L2 bridging with L3 SVI (VLAN interfaces) for both
tagged and untagged VLAN members. The standard SONiC data model creates:

- A VLAN (e.g., Vlan10)
- An SVI with an IP address (e.g., 10.0.0.1/24 on Vlan10)
- Member ports with tagging mode (tagged or untagged)

VPP's bridge-domain (BD) and BVI (Bridge Virtual Interface) constructs map to this
model, but require explicit wiring to connect VPP's data plane to the Linux control
plane (for ARP, DHCP, LLDP, LACP, routing protocols, etc.).

### Challenges

1. **BVI ↔ Linux connectivity**: VPP's BVI has no automatic path to the Linux
   network stack. Punted traffic (ARP, DHCP) from the BD/BVI must reach the
   kernel `Vlan10` interface.

2. **Tagged member dispatch**: VPP must create dot1q sub-interfaces for tagged
   members, strip the tag on ingress to the BD, and push it back on egress.
   The host must also see a corresponding VLAN sub-interface.

3. **Untagged member handling**: The physical interface itself joins the BD
   directly. Control protocols (LLDP, LACP, ARP) arriving untagged must be
   punted to the host without being consumed by the BD flood path.

4. **Promiscuous mode**: VPP's virtio/DPDK backend filters tagged frames at
   the device level unless promiscuous mode is enabled on the parent interface.

---

## 2. SONiC Configuration Example

```json
// config_db.json excerpts

// Create VLAN
"VLAN": {
    "Vlan10": {
        "vlanid": "10"
    }
}

// Assign IP to VLAN interface (SVI)
"VLAN_INTERFACE": {
    "Vlan10": {},
    "Vlan10|10.0.0.1/24": {}
}

// Add members
"VLAN_MEMBER": {
    "Vlan10|Ethernet0": {
        "tagging_mode": "tagged"
    },
    "Vlan10|Ethernet4": {
        "tagging_mode": "untagged"
    }
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

### 3.1 BVI LCP Pair

The BVI (`bvi10`) is the L3 endpoint of the bridge domain. An LCP (Linux Control
Plane) pair is created between `bvi10` and a Linux tap device (`tap_Vlan10`). A tc
filter redirects traffic between `tap_Vlan10` and the kernel `Vlan10` netdev.

This allows:
- **ARP/DHCP punted via BVI** reach the kernel as untagged Ethernet frames
- **Routing decisions** made by the kernel are sent back through the tap → BVI → BD
- The BD floods BUM (Broadcast/Unknown-unicast/Multicast) to all members
- Known unicast is forwarded based on the MAC table

### 3.2 Frames in the BD Are Always Untagged

All frames inside the bridge domain are **untagged**:
- Tagged members: VPP sub-interface pops the outer tag on ingress (`pop 1`)
  and pushes it back on egress (symmetric VTR)
- Untagged members: frames arrive without a tag and stay untagged
- BVI: exchanges untagged frames with the BD (no VTR on BVI)

This matches the Linux kernel model where the `Vlan10` SVI sees untagged frames.

### 3.3 Control Protocol Punt (`lcp-punt-l2-ethertype`)

#### Problem

When a physical interface has an LCP pair (e.g., `bobm0 → Ethernet0`) and
operates in **L3 mode**, VPP's existing `linux-cp-punt-xc` node handles
control traffic naturally: packets arriving on `bobm0` are cross-connected
to the host tap `Ethernet0` via the LCP punt path. The kernel receives
LLDP, LACP, ARP, etc. directly.

However, when the interface is placed into a **bridge domain** as an untagged
member (`set interface l2 bridge bobm0 10`), the interface transitions from
L3 mode to L2 mode. At this point:

1. VPP's `ip4-input`/`ip6-input` arc is **no longer in the path** — the
   interface enters `l2-input` instead.
2. `linux-cp-punt-xc` operates on the **L3 punt path** (ip4-punt, ip6-punt),
   not the L2 path. It never sees L2-only protocols.
3. The L2 bridge treats LLDP/LACP destination MACs (`01:80:c2:00:00:0e`,
   `01:80:c2:00:00:02`) as regular multicast and **floods them** to all BD
   members plus the BVI — but these are link-local protocols that must NOT
   be forwarded beyond the directly-connected link.
4. Even if the frame reaches the BVI and gets punted to the host, it arrives
   on `Vlan10` (the SVI) instead of `Ethernet0` (the physical port) — the
   control plane (lldpd, teamd) cannot associate the protocol message with
   the correct physical interface.

#### Solution: `lcp-punt-l2-ethertype`

A new VPP feature node is inserted on the **L2 input arc** (between
`l2-input` and `l2-learn`) that matches frames by ethertype:

```
l2-input → [lcp-punt-l2-ethertype] → l2-learn → l2-fwd → ...
```

When a frame matches a configured ethertype, the node:
- **Punt mode (1)**: Redirects the frame to the interface's LCP host tap
  and drops it from the L2 flood path. Used for LLDP and LACP — these must
  reach the host but never be flooded to other BD members.
- **Punt-copy mode (2)**: Clones the frame — the copy goes to the LCP host
  tap while the original continues through the L2 bridge for normal flooding.
  Used for ARP — the host needs it for the control plane, but other BD
  members also need the broadcast for L2 reachability.

The punt delivers the frame to the **LCP tap of the ingress interface**
(e.g., `Ethernet0` for untagged member `bobm0`, or `Ethernet0.10` for tagged
sub-interface `bobm0.10`), preserving the correct per-port association that
the control plane expects.

#### Configuration

Ethertypes are configured globally at VPP client init:

| Ethertype | Protocol | Mode | Behavior |
|-----------|----------|------|----------|
| 0x88cc | LLDP | punt (1) | To host only, not flooded |
| 0x8809 | LACP | punt (1) | To host only, not flooded |

Per-interface enable/disable controls which BD members participate:
- Untagged members: explicitly enabled via `vpp_lcp_punt_l2_ethertype_set()`
- Tagged members (sub-interfaces): inherit from parent's LCP pair

#### Why Not Use `group_fwd_mask` on Linux Bridge?

The Linux kernel bridge's `group_fwd_mask` only applies to the **kernel**
bridge (`Bridge` in SONiC). In the VPP data path, bridging happens entirely
in VPP's bridge domain — frames never traverse the kernel bridge for
forwarding decisions. The kernel bridge only sees traffic that has already
been punted/redirected via LCP taps. Therefore, `group_fwd_mask` cannot
solve the VPP-side LLDP/LACP flooding problem.

### 3.4 Promiscuous Mode on Physical Interfaces

VPP's virtio/DPDK backend implements `VIRTIO_NET_F_CTRL_VLAN` which filters
tagged frames at the device level. Promiscuous mode is enabled on every
physical interface at LCP pair creation time so that tagged frames pass
through to VPP's `ethernet-input` for sub-interface dispatch.

### 3.5 Auto Sub-Interface (lcp-auto-subint)

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
3. Enable arp-term on BD: `set_bridge_domain_flags(bd_id, ARP_TERM, true)`
4. Create LCP pair: `configure_lcp_interface("bvi10", "tap_Vlan10", true)`
5. Bring up the tap: `interface_set_state("tap_Vlan10", true)`
6. TC redirect: `add_tc_filter_redirect("tap_Vlan10", "Vlan10")`

Teardown mirrors creation in reverse order.

### 4.2 Tagged Member: Sub-Interface + VTR Pop-1

**File**: `SwitchVppFdb.cpp` — `vpp_create_vlan_member()` (TAGGED path)

1. Create VPP sub-interface: `create_sub_interface(bobm0, 10, 10)`
   - `lcp-auto-subint` automatically creates `Ethernet0.10` on the host
2. Add sub-interface to BD: `set_sw_interface_l2_bridge(bobm0.10, 10, true, NORMAL)`
3. Set VTR pop-1: `set_l2_interface_vlan_tag_rewrite(bobm0.10, 10, ~0, DOT1Q, POP_1)`
4. Admin up: `interface_set_state(bobm0.10, true)`

### 4.3 Punt L2 Ethertype for BD Members

**File**: `SaiVppXlate.c` — `init_vpp_client()`

Global mode-set at VPP client initialization:
- `vpp_lcp_punt_l2_ethertype_mode_set(0x8809, 1)` — LACP: punt (no flood)
- `vpp_lcp_punt_l2_ethertype_mode_set(0x88cc, 1)` — LLDP: punt (no flood)
- `vpp_lcp_punt_l2_ethertype_mode_set(0x0806, 2)` — ARP: punt-copy (clone + flood)

**VPP patch**: `0009-linux-cp-punt-l2-ethertype.patch` adds the L2-arc punt
node that intercepts matching ethertypes on interfaces in L2 mode.

### 4.4 Untagged Member: Parent Interface in BD

**File**: `SwitchVppFdb.cpp` — `vpp_create_vlan_member()` (UNTAGGED path)

1. Add parent phy directly to BD: `set_sw_interface_l2_bridge(bobm0, 10, true, NORMAL)`
2. Enable per-interface punt: `vpp_lcp_punt_l2_ethertype_set(bobm0, true)`
   - Ensures LLDP/LACP/ARP reach the host tap for the parent

No sub-interface or VTR needed — wire frames are already untagged.

### 4.5 Promiscuous Mode on Every Physical Interface

**File**: `SwitchVppHostif.cpp`

At LCP pair creation for each physical port:
```cpp
configure_lcp_interface(hwif_name, dev, true);
interface_set_promiscuous(hwif_name, true);  // <-- added
```

**File**: `SaiVppXlate.c` — new `interface_set_promiscuous()` wrapper using
VPP's `sw_interface_set_promisc` API.

---

## 5. Packet Flow

### 5.1 Ingress: Tagged Member (e.g., Ethernet0 in Vlan10, tagged)

```
Wire (802.1Q tag=10) → bobm0 (dpdk-input, promisc on)
  → ethernet-input: matches dot1q → dispatches to bobm0.10
    → l2-input (BD 10)
      → l2-input-vtr: POP outer tag (frame now untagged in BD)
        → l2-learn: learn src MAC on bobm0.10
          → l2-fwd:
            ├─ Known unicast → output to destination member
            └─ BUM → l2-flood → all BD members + BVI
              → BVI (bvi10) → linux-cp-punt-xc → tap_Vlan10 → tc → Vlan10 (kernel)
```

### 5.2 Ingress: Untagged Member (e.g., Ethernet4 in Vlan10, untagged)

```
Wire (no tag) → bobm0 (dpdk-input, promisc on)
  → ethernet-input: no tag → parent sw_if_index
    → l2-input (BD 10)
      → l2-input-vtr: no-op (no rewrite configured)
        → l2-learn: learn src MAC on bobm0
          → [LLDP/LACP?] → lcp-punt-l2-ethertype: punt to host (no flood)
          → l2-fwd:
            ├─ Known unicast → output to destination member
            └─ BUM → l2-flood → all BD members + BVI
              → BVI (bvi10) → linux-cp-punt-xc → tap_Vlan10 → tc → Vlan10 (kernel)
```

### 5.3 Egress: Kernel → BD (e.g., ARP reply from Vlan10 SVI)

```
Kernel (Vlan10) → tc redirect → tap_Vlan10
  → linux-cp-punt-xc → bvi10
    → l2-input (BD 10, BVI port)
      → l2-fwd:
        ├─ Known unicast MAC on bobm0.10 (tagged member):
        │   → l2-output → l2-output-vtr: PUSH tag 10 → bobm0.10 → wire (tagged)
        ├─ Known unicast MAC on bobm0 (untagged member):
        │   → l2-output → no VTR → bobm0 → wire (untagged)
        └─ BUM (broadcast ARP request):
            → l2-flood → all members:
              → bobm0.10: PUSH tag 10 → wire (tagged)
              → bobm0: no VTR → wire (untagged)
```

### 5.4 Egress: Unicast Forwarding Between Members

```
bobm0.10 (ingress, tagged member) → l2-input → POP tag
  → l2-learn + l2-fwd
    → destination MAC known on bobm0 (untagged member):
      → l2-output → no VTR → bobm0 → wire (untagged)

bobm0 (ingress, untagged member) → l2-input → no VTR
  → l2-learn + l2-fwd
    → destination MAC known on bobm0.10 (tagged member):
      → l2-output → l2-output-vtr: PUSH tag 10 → bobm0.10 → wire (tagged)
```

---

## 6. VPP Configuration Summary (Runtime State)

After SONiC applies the configuration above, VPP state looks like:

```
# Bridge domain 10 with BVI, tagged member, untagged member
vppctl show bridge-domain 10 detail
  BD-ID 10, flood, learn, arp-term
    bvi10        (BVI, sw_if_index 25)
    bobm0.10     (tagged member, sw_if_index 23, vtr pop-1)
    bobm0        (untagged member, sw_if_index 1)

# LCP pairs
vppctl show lcp
  bvi10     → tap_Vlan10
  bobm0     → Ethernet0      (physical)
  bobm0.10  → Ethernet0.10   (auto-subint)

# Promiscuous mode
vppctl show interface bobm0
  flags: ... promisc ...

# Punt L2 ethertypes (global)
  LACP (0x8809): punt
  LLDP (0x88cc): punt
```

---

## 7. Files Modified

| File | Change |
|------|--------|
| `platform/vpp/docker-syncd-vpp/conf/startup.conf.tmpl` | Enable `lcp-auto-subint` |
| `platform/vpp/docker-sonic-vpp/conf/startup.conf.tmpl` | Enable `lcp-auto-subint` |
| `platform/vpp/vppbld/patches/0009-linux-cp-punt-l2-ethertype.patch` | VPP plugin: L2 punt by ethertype |
| `src/sonic-sairedis/vslib/vpp/vppxlate/SaiVppXlate.c` | `interface_set_promiscuous()`, punt mode-set API, sub-if cache |
| `src/sonic-sairedis/vslib/vpp/vppxlate/SaiVppXlate.h` | Extern declarations |
| `src/sonic-sairedis/vslib/vpp/SwitchVppFdb.cpp` | BVI LCP pair, tagged/untagged member handling, delete fix |
| `src/sonic-sairedis/vslib/vpp/SwitchVppRif.cpp` | SUB_PORT RIF: rely on auto-subint |
| `src/sonic-sairedis/vslib/vpp/SwitchVppHostif.cpp` | Promisc on every phy at LCP creation |
| `src/sonic-sairedis/vslib/vpp/SwitchVpp.h` | `m_bvi_vlan_lcp_map` member |
