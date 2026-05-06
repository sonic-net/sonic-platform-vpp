# VLAN Bridge Domain with BVI — High-Level Design

## 1. Problem Statement

SONiC on VPP needs to support L2 bridging with L3 SVI (VLAN interfaces) for both
tagged and untagged VLAN members. The standard SONiC data model creates:

- A VLAN (e.g., Vlan10)
- An SVI with an IP address (e.g., 10.0.0.1/24 on Vlan10)
- Member ports with tagging mode (tagged or untagged)

VPP's bridge-domain (BD) and BVI (Bridge Virtual Interface) constructs map to this
model, but require explicit wiring to connect VPP's data plane to the Linux control
plane for ARP resolution and IP forwarding.

### Challenges

1. **BVI ↔ Linux connectivity**: VPP's BVI has no automatic path to the Linux
   network stack. ARP requests/replies and IP packets destined to the SVI must
   reach the kernel `Vlan10` interface.

2. **ARP resolution**: Hosts in the BD send ARP requests (broadcast) to resolve
   the gateway (BVI IP). The BD must flood these to the BVI, and the BVI must
   punt them to the kernel for the ARP daemon to reply.

3. **Tagged member dispatch**: VPP must create dot1q sub-interfaces for tagged
   members, strip the tag on ingress to the BD, and push it back on egress.

4. **Untagged member handling**: The physical interface itself joins the BD
   directly — no sub-interface or VLAN tag rewrite needed.

5. **Promiscuous mode**: VPP's virtio/DPDK backend filters tagged frames at
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

### 3.1 BVI LCP Pair

The BVI (`bvi10`) is the L3 endpoint of the bridge domain. An LCP (Linux Control
Plane) pair is created between `bvi10` and a Linux tap device (`tap_Vlan10`). A tc
filter redirects traffic between `tap_Vlan10` and the kernel `Vlan10` netdev.

This allows:
- **ARP requests** flooded in the BD reach the BVI, which punts to kernel
- **ARP replies** from kernel travel back through tap → BVI → BD → member
- **IP packets** destined to the SVI IP are routed to `ip4-local` and punted
- **Routed traffic** from kernel exits through BVI into the BD

### 3.2 Frames in the BD Are Always Untagged

All frames inside the bridge domain are **untagged**:
- Tagged members: VPP sub-interface pops the outer tag on ingress (`pop 1`)
  and pushes it back on egress (symmetric VTR)
- Untagged members: frames arrive without a tag and stay untagged
- BVI: exchanges untagged frames with the BD (no VTR on BVI)

This matches the Linux kernel model where the `Vlan10` SVI sees untagged frames.

### 3.3 ARP Handling via BVI

ARP in a bridge domain with BVI works like below:

**BUM flood to BVI**: ARP requests (broadcast, dst ff:ff:ff:ff:ff:ff) are
   flooded to all BD members including the BVI. When the frame reaches the BVI:
   - `bvi-input` transitions from L2 to L3
   - The ARP request is punted to the host via
     `linux-cp-punt-xc → tap_Vlan10 → tc → Vlan10`
   - The kernel's ARP daemon generates a reply

ARP is **not** punted directly from member interface. The broadcast ARP naturally
floods to the BVI through the bridge domain's normal BUM flooding path.

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
3. Create LCP pair: `configure_lcp_interface("bvi10", "tap_Vlan10", true)`
4. Bring up the tap: `interface_set_state("tap_Vlan10", true)`
5. TC redirect: `add_tc_filter_redirect("tap_Vlan10", "Vlan10")`

Teardown mirrors creation in reverse order.

### 4.2 Tagged Member: Sub-Interface + VTR Pop-1

**File**: `SwitchVppFdb.cpp` — `vpp_create_vlan_member()` (TAGGED path)

1. Create VPP sub-interface: `create_sub_interface(bobm0, 10, 10)`
   - `lcp-auto-subint` automatically creates `Ethernet0.10` on the host
2. Add sub-interface to BD: `set_sw_interface_l2_bridge(bobm0.10, 10, true, NORMAL)`
3. Set VTR pop-1: `set_l2_interface_vlan_tag_rewrite(bobm0.10, 10, ~0, DOT1Q, POP_1)`
4. Admin up: `interface_set_state(bobm0.10, true)`

### 4.3 Untagged Member: Parent Interface in BD

**File**: `SwitchVppFdb.cpp` — `vpp_create_vlan_member()` (UNTAGGED path)

1. Add parent phy directly to BD: `set_sw_interface_l2_bridge(bobm0, 10, true, NORMAL)`

No sub-interface or VTR needed — wire frames are already untagged.

### 4.4 Promiscuous Mode on Every Physical Interface

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

### 5.1 ARP Request from Tagged Member

A host on tagged member Ethernet0 (bobm0.10) sends an ARP request to resolve
the gateway 10.0.0.1 (BVI IP):

```
Wire (802.1Q tag=10, ARP who-has 10.0.0.1, dst ff:ff:ff:ff:ff:ff)
  → bobm0 (dpdk-input, promisc on)
    → ethernet-input: dot1q tag=10 → dispatches to bobm0.10
      → l2-input (BD 10)
        → l2-input-vtr: POP tag (frame now untagged in BD)
          → l2-learn: learn src MAC on bobm0.10
            → l2-fwd: dst ff:ff:ff:ff:ff:ff → broadcast
              → l2-flood: flood to all BD members except source intf + BVI
                └─ bvi10 (BVI):
                    → linux-cp-punt-xc
                      → tap_Vlan10 → tc → Vlan10 (kernel)
                        → kernel ARP daemon sends reply
```

### 5.2 ARP Request from Untagged Member

A host on untagged member Ethernet4 (bobm0) sends an ARP request:

```
Wire (no tag, ARP who-has 10.0.0.1, dst ff:ff:ff:ff:ff:ff)
  → bobm0 (dpdk-input, promisc on)
    → ethernet-input: no tag → parent sw_if_index (bobm0)
      → l2-input (BD 10)
        → l2-input-vtr: no-op
          → l2-learn: learn src MAC on bobm0
            → l2-fwd: dst ff:ff:ff:ff:ff:ff → broadcast
              → l2-flood: flood to all BD members except source intf + BVI
                └─ bvi10 (BVI):
                    → bvi-input: L2→L3 transition
                      → ip4-punt → linux-cp-punt-xc
                        → tap_Vlan10 → tc → Vlan10 (kernel)
                          → kernel ARP daemon sends reply
```

### 5.3 ARP Reply from Kernel to Member

The kernel ARP daemon generates a unicast reply on `Vlan10`. The reply does
**not** go through VPP's bridge domain — it exits through the Linux bridge
directly to the member's LCP tap:

```
Kernel ARP reply (src MAC = BVI MAC, dst MAC = requester's MAC)
  → Vlan10 → Linux bridge FDB lookup for dst MAC:
    ├─ If learned on Ethernet8: unicast → Ethernet8 tap
    └─ If unknown: flood to all bridge member taps
  → Ethernet8 tap (tap-input, hw_if_index 18)
    → linux-cp-punt-xc: 18 → 2 (bobm1)
      → bobm1-output → bobm1-tx → wire (untagged)
```

For a **tagged** member:
```
  → Ethernet0.10 tap (tap-input)
    → linux-cp-punt-xc → bobm0.10
      → bobm0.10-output → wire (tagged, 802.1Q tag added by sub-if)
```

> **Note — Linux bridge FDB flooding issue:**
>
> The Linux bridge does not learn remote MACs on the correct member port
> because all L2 forwarding happens in VPP's bridge domain — no traffic from
> remote hosts ever arrives on member LCP taps to trigger kernel FDB learning.
> As a result, the first ARP reply (and all subsequent kernel-originated unicast
> frames) are **flooded** by the Linux bridge to all member taps as unknown
> unicast. This is functionally correct (the requester receives the reply) but
> wasteful.
>
> **Planned fix:** Implement SAI FDB event notifications (`SAI_FDB_EVENT_LEARNED`,
> `SAI_FDB_EVENT_AGED`, `SAI_FDB_EVENT_MOVE`) in VPP SAI. When VPP's `l2-learn`
> adds a MAC to its l2fib, the SAI layer generates a learned event. SONiC's
> fdborch processes the event and fdbsyncd programs the entry into the kernel
> bridge FDB. After that, the Linux bridge forwards unicast frames directly to
> the correct member tap without flooding.

### 5.4 Unicast Destined to BVI (e.g., ping to Vlan10 SVI IP)

Once ARP is resolved, the host sends IP packets with dst-MAC = BVI MAC. The
L2 forwarding table has learned the BVI MAC on the BVI port:

```
Wire (tagged, dst-MAC = BVI MAC, dst-IP = 10.0.0.1) → bobm0 (dpdk-input)
  → ethernet-input → bobm0.10
    → l2-input (BD 10)
      → l2-input-vtr: POP tag
        → l2-learn (src MAC on bobm0.10)
          → l2-fwd: dst MAC known on bvi10 (BVI port)
            → l2-output → bvi10
              → bvi-input: L2→L3 transition
                → ip4-input → ip4-lookup → ip4-local (dest is local SVI IP)
                  → linux-cp-punt-xc → tap_Vlan10 → tc → Vlan10 (kernel)
                    → kernel processes (ICMP reply, TCP, etc.)
```

For an **untagged** member the path is the same except no VTR pop:

```
Wire (untagged, dst-MAC = BVI MAC) → bobm0 (dpdk-input)
  → ethernet-input → bobm0 (parent sw_if_index)
    → l2-input (BD 10)
      → l2-learn (src MAC on bobm0)
        → l2-fwd: dst MAC known on bvi10
          → l2-output → bvi10
            → bvi-input: L2→L3 transition
              → ip4-input → ip4-lookup → ip4-local
                → linux-cp-punt-xc → tap_Vlan10 → tc → Vlan10 (kernel)
```

The key insight: unlike BUM traffic (which reaches the BVI via `l2-flood`),
known-unicast to the BVI MAC reaches it via normal `l2-fwd` lookup. In both
cases, once the packet enters `bvi-input`, it is treated as an L3 packet.

### 5.5 Routed Traffic with BVI as Next-Hop (Inter-VLAN / External)

When a host in BD 10 sends IP traffic to a destination on a **different
subnet** (e.g., another VLAN or an external route), the host's default
gateway is the BVI IP (Vlan10 SVI). The host resolves the gateway MAC via
ARP and sends the frame with dst-MAC = BVI MAC.

**Ingress (L2 → BVI → L3 routing):**

```
Wire (dst-MAC = BVI MAC, dst-IP = 10.0.20.1) → bobm0.10
  → l2-input (BD 10) → l2-input-vtr: POP tag
    → l2-learn → l2-fwd: dst MAC = BVI MAC → bvi10
      → bvi-input: L2→L3 transition
        → ip4-input → ip4-lookup:
          dst 10.0.20.1 → next-hop via bvi20 (another VLAN SVI)
                        → or next-hop via bobm2 (L3 routed port)
                        → or next-hop via default route (upstream)
```

**Case A: Inter-VLAN routing (destination in BD 20):**

```
        → ip4-lookup → next-hop 10.0.20.1 reachable via bvi20
          → ip4-rewrite: rewrite dst-MAC to target host MAC, src-MAC to bvi20 MAC
            → bvi-output (bvi20): L3→L2 transition into BD 20
              → l2-input (BD 20, from BVI port)
                → l2-fwd: dst MAC known on member in BD 20
                  → l2-output → [VTR if tagged member] → wire
```

**Case B: Routing to an L3 port (no bridge domain):**

```
        → ip4-lookup → next-hop via bobm2 (L3 interface)
          → ip4-rewrite: rewrite MACs
            → interface-output → bobm2 → wire
```

### 5.6 L2 Unicast Forwarding Between Members

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
  BD-ID 10, flood, learn
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
```

---

## 7. Related Documents

- [VLAN BVI L2 Punt HLD](vlan-bvi-l2-punt-hld.md) — LLDP/LACP punt via
  `lcp-punt-l2-ethertype` for bridged members

---

## 8. Files Modified

| File | Change |
|------|--------|
| `platform/vpp/docker-syncd-vpp/conf/startup.conf.tmpl` | Enable `lcp-auto-subint` |
| `platform/vpp/docker-sonic-vpp/conf/startup.conf.tmpl` | Enable `lcp-auto-subint` |
| `src/sonic-sairedis/vslib/vpp/vppxlate/SaiVppXlate.c` | `interface_set_promiscuous()` wrapper |
| `src/sonic-sairedis/vslib/vpp/vppxlate/SaiVppXlate.h` | Extern declarations |
| `src/sonic-sairedis/vslib/vpp/SwitchVppFdb.cpp` | BVI LCP pair, tagged/untagged member handling |
| `src/sonic-sairedis/vslib/vpp/SwitchVppRif.cpp` | SUB_PORT RIF: rely on auto-subint |
| `src/sonic-sairedis/vslib/vpp/SwitchVppHostif.cpp` | Promisc on every phy at LCP creation |
| `src/sonic-sairedis/vslib/vpp/SwitchVpp.h` | `m_bvi_vlan_lcp_map` member |
