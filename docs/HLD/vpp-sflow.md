# sFlow Support in SONiC-VPP — High Level Design

## 1. Objectives

Provide standards-conformant sFlow (RFC 3176 / sFlow v5) support on the
SONiC-VPP platform, consistent with the [SONiC sFlow
HLD](https://github.com/sonic-net/SONiC/blob/master/doc/sflow/sflow_hld.md):

- Per-port packet sampling on physical interfaces, programmable through the
  standard SONiC `SFLOW`/`SFLOW_SESSION`/`SFLOW_COLLECTOR` CONFIG_DB schema and
  `config sflow ...` CLI.
- Per-interface counter samples exported by `hsflowd` to one or more sFlow
  collectors.
- Optional drop-event notifications (sFlow drop monitor / dropmon).
- Operate end-to-end on SONiC-VPP without bypassing the standard SONiC sFlow
  stack (`sflowmgrd` → `sflowOrch` → SAI → host-sflowd).
- Pass `sonic-mgmt` sFlow regression suites against a SONiC-VPP DUT.

Non-objectives:

- Vendor-specific extensions (e.g. proprietary debug counters in flow samples).
- Re-implementing `hsflowd`; we reuse the standard `docker-sflow`.
- Per-flow accounting (sFlow is statistical sampling, by design).

## 2. Scope

In scope:

- `platform/vpp/` build glue (VPP plugin enablement, supervisord, configuration
  templates).
- `sonic-sairedis` vpplib / vslib SAI shim for sFlow object types
  (`SAI_OBJECT_TYPE_SAMPLEPACKET`, `SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE`,
  `SAI_HOSTIF_TRAP_TYPE_SAMPLEPACKET`, related hostif/policer plumbing).
- Local patches to VPP's `sflow` plugin where required (ifindex translation,
  per-port sub-sampling, drop-rate limiting) and will create fd.io PR to upstream
  the changes.
- Possible bump of `host-sflow` (`hsflowd`) version inside `docker-sflow` to
  pick up `mod_vpp` (optional, see Phase 1 alternative).
- Verification plan (unit tests in vpplib, integration tests against `vs`-style
  topologies, full sonic-mgmt sFlow suite).

Out of scope:

- Changes to SONiC's CONFIG_DB schema, YANG models, `sflowmgrd`, `sflowOrch`,
  or `sonic-utilities` CLI.
- IPv6 collector address support beyond what `hsflowd` already provides.

## 3. Feature Description

### 3.1 What SONiC sFlow expects from the platform

Per the SONiC sFlow HLD:

| Layer | Responsibility |
|---|---|
| `config sflow` CLI / sonic-cfggen | Populate `SFLOW`/`SFLOW_SESSION`/`SFLOW_COLLECTOR` tables in CONFIG_DB. |
| `sflowmgrd` (swss) | Translate CONFIG_DB → APPL_DB. |
| `sflowOrch` (orchagent) | Program SAI: create `SAMPLEPACKET` objects, set `SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE` per port, install `SAI_HOSTIF_TRAP_TYPE_SAMPLEPACKET`, set up policer/trap-group, install genetlink hostif `psample`. |
| Platform SAI | Generate per-port samples at the configured rate; deliver to the host via the `psample` genetlink family (multicast group `packets`). Drop notifications via `NET_DM` family (group `events`). |
| `docker-sflow` / `hsflowd` | Receive PSAMPLE / DROPMON messages, encode sFlow v5 datagrams (flow + counter + drop samples), send to collector(s). |

Per-port programmable: **enable**, **admin state**, **sample rate**, **trap
policer**. Counters (octets, ucast/mcast/bcast pkts, errors, discards) come from
COUNTERS_DB via `mod_sonic` in `hsflowd` — not from the SAI sFlow path.

#### 3.1.1 SONiC sFlow component diagram

```
                 +--------------------------+
   Operator ---> |  config sflow CLI /      |
                 |  sonic-cfggen / YANG     |
                 +-----------+--------------+
                             |
                             v
                  +----------+-----------+
                  |     CONFIG_DB        |
                  |  SFLOW               |
                  |  SFLOW_SESSION       |
                  |  SFLOW_COLLECTOR     |
                  +----------+-----------+
                             |
                             v
            +----------------+----------------+
            |          sflowmgrd  (swss)      |
            |   CONFIG_DB  ->  APPL_DB        |
            +----------------+----------------+
                             |
                             v
                  +----------+-----------+
                  |       APPL_DB        |
                  |  SFLOW_TABLE         |
                  |  SFLOW_SESSION_TABLE |
                  +----------+-----------+
                             |
                             v
            +----------------+----------------+
            |        sflowOrch (orchagent)    |
            |  SAMPLEPACKET / port attr /     |
            |  hostif trap / hostif / policer |
            +----------------+----------------+
                             | SAI ABI
                             v
                  +----------+-----------+         +--------------+
                  |        syncd         |<------->|  ASIC_DB     |
                  +----------+-----------+         +--------------+
                             | SAI ABI
                             v
            +-----------------+--------------------+
            |          Platform SAI driver         |
            | (vendor ASIC SDK or sonic-vpp shim)  |
            +-----------------+--------------------+
                              |
              programs sampling, traps, policers
                              |
                              v
                   +----------+-----------+
                   |     Dataplane        |
                   |  (ASIC or VPP)       |
                   +-----+----------+-----+
                         |          |
            flow samples |          | drop alerts
            (PSAMPLE /   |          | (NET_DM /
             genetlink)  |          |  genetlink)
                         v          v
                    +----+----------+----+         +-----------------+
                    |  kernel netlink     |<------>|  /etc/hsflowd   |
                    |  (psample, NET_DM)  |        |  /.conf         |
                    +----+----------------+        +-----------------+
                         |
                         v
   COUNTERS_DB ---->+----+-----------------------------+
   (per-port stats) |        docker-sflow              |
                    |  hsflowd (mod_psample,           |
                    |   mod_dropmon, mod_sonic)        |
                    +----+-----------------------------+
                         | sFlow v5 UDP
                         v
                  +------+--------+
                  | Collector(s)  |  e.g. sflowtool, InMon, ntop
                  +---------------+
```

Inputs to `hsflowd` on a SONiC switch:

- **PSAMPLE genetlink** — per-packet flow samples generated by the platform.
- **NET_DM genetlink** — drop alerts (packet-mode), generated by the platform.
- **COUNTERS_DB** (Redis) — per-port interface counters (octets, ucast,
  mcast, bcast, errors, discards) pulled by `mod_sonic` and packaged into
  sFlow counter samples. These do **not** flow through the SAI sFlow trap
  path; they originate from `syncd`'s normal counter polling.

`hsflowd` then composes sFlow v5 datagrams (flow + counter + drop samples)
and unicasts them to each configured collector over UDP/6343 (default).

### 3.2 What VPP's `sflow` plugin provides

VPP ships `src/plugins/sflow/` (already present in
`platform/vpp/vppbld/repo/src/plugins/sflow/`) with the following capabilities:

| Capability | Status |
|---|---|
| Random 1-in-N packet sampling on `device-input` arc per hw-interface | Yes, **global** sampling rate; per-interface enable/disable only |
| Optional egress sampling on `interface-output` arc | Yes |
| Sampling direction (ingress / egress / both) | Yes, but **global only** (`sflow direction <ingress\|egress\|both>` / `sflow_direction_set` API). `smp->samplingD` is a single global value applied to every enabled port. |
| Drop-event sampling on `error-drop` arc | Yes; **on/off only**, no rate limiter |
| PSAMPLE genetlink output (family `psample`, group `packets`) | Yes, with `IIFINDEX = VPP hw_if_index` |
| DROPMON genetlink output (family `NET_DM`, group `events`) | Yes, packet-mode (`NET_DM_CMD_PACKET_ALERT`) |
| USERSOCK output for per-port counters and OSINDEX (LCP-resolved) | Yes (proto 2, multicast group bit 29); consumer is `mod_vpp` in `hsflowd` ≥ 2.1.10 |
| Periodic counter polling (configurable `polling_S`) | Yes |
| Configurable header-bytes (max bytes captured per sample) | Yes (64–256, step 32, default 128) |
| Per-port sample-count counters | **No** (only global plugin error counters in `/err/sflow/*`) |
| Per-trap rate limiting on dropmon | **No** |
| API/CLI: `sflow enable hw <if>`, `sflow sampling-rate`, `sflow polling-interval`, `sflow header-bytes`, `sflow drop-monitoring on|off` | Yes |

### 3.3 Capability gap matrix

| SONiC requirement | VPP today | Gap | Resolution |
|---|---|---|---|
| Per-port sample rate | Global rate, per-port enable | Yes | GCD + per-port sub-sampling in slow path |
| Per-port enable | Yes | None | Direct mapping |
| Per-port sampling direction (ingress / egress / both) | Global only (`smp->samplingD`) | Yes | Per-port direction state in `sflow_per_interface_data_t`; in `sflow_enable_disable_interface()` decide independently per port whether to attach the `device-input` (ingress) and `interface-output` (egress) feature arcs based on the port's own direction. Add new VPP API `sflow_interface_direction_set { hw_if_index, direction }`. Programmed by vpplib from the SAI samplepacket object's `SAI_SAMPLEPACKET_ATTR_TYPE` plus the port-attribute distinction between `INGRESS_SAMPLEPACKET_ENABLE` and `EGRESS_SAMPLEPACKET_ENABLE`. |
| Sample rate runtime change | Yes | None | Direct mapping |
| Header-bytes runtime change | Yes | None | Direct mapping |
| Polling interval | Yes (global) | None | Direct mapping |
| PSAMPLE delivery to `hsflowd` | Yes, but ifindex = hw_if_index | Mismatch with Linux netdev / SONiC port name | (a) substitute LCP-resolved Linux ifindex into PSAMPLE message, or (b) use `mod_vpp` over usersock |
| `SAI_HOSTIF_TRAP_TYPE_SAMPLEPACKET` (psample genetlink hostif) | Family is kernel-provided + VPP plugin emits | None functional; SAI shim is bookkeeping no-op | SAI shim accepts trap creates and tracks state |
| Drop monitor on/off | Yes | None | Direct mapping |
| Trap policer (drop notifications) | No rate limit in plugin | Yes | Token-bucket in plugin slow path |
| SAI samplepacket attributes (`SAMPLE_RATE`, `TYPE = SLOW_PATH`) | n/a | None | Translate to VPP API calls |
| Per-port interface counters in COUNTERS_DB | Already provided by SAI shim → COUNTERS_DB → `mod_sonic` | None | No change |

### 3.4 Architecture overview

```
                +--------------------+
config CLI ---> |    CONFIG_DB       |
                +---------+----------+
                          |
                          v
                +---------+----------+
                |     sflowmgrd      |  (swss)
                +---------+----------+
                          |
                          v
                +---------+----------+
                |     APPL_DB        |
                +---------+----------+
                          |
                          v
                +---------+----------+
                |     sflowOrch      |  (orchagent)
                +---------+----------+
                          | sairedis client API
                          v
                +---------+----------+
                |     ASIC_DB        |   (Redis, sai_serialize'd ops:
                |                    |    create_samplepacket,
                |                    |    set port INGRESS_SAMPLEPACKET_ENABLE,
                |                    |    create_hostif_trap, etc.)
                +---------+----------+
                          |
                          v
                +---------+----------+
                |       syncd        |   subscribes to ASIC_DB,
                |                    |   replays ops via SAI ABI
                +---------+----------+
                          | SAI ABI (libsai.so == sairedis vpplib)
                          v
                +---------+--------------+
                |    sairedis vpplib     |   SAI -> VPP translation:
                |   (libsai for VPP)     |     create_samplepacket
                |                        |     set INGRESS_SAMPLEPACKET_ENABLE
                |                        |     create_hostif_trap (SAMPLEPACKET)
                |                        |     create_hostif (psample) [no-op]
                +---------+--------------+
                          | VPP binary API (vapi)
                          v
        +-----------------+----------------------+
        |          VPP sflow plugin              |
        |  per-port enable + GCD + sub-sample +  |
        |  linux-ifindex translation +           |
        |  drop-rate limiter                     |
        +---+--------------+-------------+-------+
            |              |             |
        PSAMPLE         NET_DM        USERSOCK (proto 2, mc grp 29)
       (genetlink)    (genetlink)         |
            |              |              v
            v              v          (consumed by mod_vpp in
        +---+--------------+--+        hsflowd >= 2.1.10; ignored
        |  hsflowd            |        on default sonic-vpp build)
        |  in docker-sflow:   |
        |   mod_psample       |<--- COUNTERS_DB (per-port stats,
        |   mod_dropmon       |     populated by syncd from VPP
        |   mod_sonic         |     stat segment via vpplib)
        +---------+-----------+
                  | sFlow v5 / UDP 6343
                  v
            +-----+-----+
            | Collector |
            +-----------+
```

Key points the diagram makes explicit:

- `sflowOrch` does not call SAI directly into the platform. It writes
  SAI-serialized operations to **ASIC_DB**, which `syncd` consumes and
  replays through its linked `libsai.so` — on sonic-vpp this `libsai.so`
  **is** the sairedis vpplib.
- vpplib is the single SAI shim layer that translates SAI calls into VPP
  binary-API calls. There is no separate "vpplib" object below `syncd`;
  `syncd` itself loads vpplib as `libsaivs.so`.
- The VPP sflow plugin is the only producer of PSAMPLE / NET_DM netlink
  messages. `hsflowd` is the only consumer in the SONiC sflow docker.
- COUNTERS_DB carries per-port interface counters; these are populated by
  `syncd` (via vpplib reading the VPP stat segment) and consumed by
  `mod_sonic`. They do **not** travel through the netlink sflow path.

## 4. Solution Proposal

### 4.1 SAI APIs to implement in vpplib

Add a vpplib `SaiVppSflow` translator. The following SAI handlers must be wired:

| SAI object / attribute | Action in vpplib |
|---|---|
| `SAI_OBJECT_TYPE_SAMPLEPACKET` create / set / remove | Allocate vpplib OID; persist `SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE`, `SAI_SAMPLEPACKET_ATTR_TYPE`, `SAI_SAMPLEPACKET_ATTR_MODE`. On any change, recompute the global VPP sample rate (GCD) and per-port sub-sampling factors for ports referencing this OID. |
| `SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE` set | If new value is `SAI_NULL_OBJECT_ID`: call VPP `sflow_enable_disable(sw_if_index, 0)`; clear per-port state. Else: bind port to the samplepacket OID, call `sflow_enable_disable(sw_if_index, 1)`, recompute GCD and per-port sub-sampling. |
| `SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE` set | (Optional, Phase 4) Same as above for egress arc; VPP supports it but mainline SONiC rarely uses it. |
| `SAI_HOSTIF_TRAP_TYPE_SAMPLEPACKET` create / set | Bookkeeping only. Track `PACKET_ACTION` (TRAP/COPY/DROP), `TRAP_GROUP`. If action becomes DROP, gate per-port enable off. |
| `SAI_HOSTIF_TRAP_GROUP` with `POLICER` | Translate policer `pir` (pps) and `cbs` (burst) → call new VPP API `sflow_drop_rate_limit_set(pps, burst)` if the trap is `SAMPLEPACKET` and applies to drop monitoring. For ingress sampling, VPP's intrinsic 1-in-N is the rate-limiter. |
| `SAI_OBJECT_TYPE_HOSTIF` of type `GENETLINK` with name `psample`, group `packets` | No-op: kernel module + VPP plugin own this channel. Just allocate OID and return success. |
| `SAI_OBJECT_TYPE_HOSTIF_TABLE_ENTRY` binding `SAMPLEPACKET` trap → psample hostif | No-op: binding is implicit in VPP. |

The slow-path `update_counters` and stat-segment integration require **no
new SAI work** — sFlow's per-port counter samples come from COUNTERS_DB via
`mod_sonic`, which is already populated for every SONiC-VPP port.

### 4.2 Mapping to VPP APIs

We will call VPP through the existing vpplib transport (vapi / direct binary
API), reusing the same path as ACL/route programming:

| SAI handler | VPP API/CLI |
|---|---|
| Per-port enable | `sflow_enable_disable { hw_if_index, enable_disable }` |
| Set sample rate (global) | `sflow_sampling_rate_set { sampling_N }` |
| Set polling interval | `sflow_polling_interval_set { polling_S }` |
| Set header bytes | `sflow_header_bytes_set { header_B }` |
| Drop monitoring on/off | New helper `sflow_drop_monitoring(bool)` (CLI exists, API trivial wrapper) |
| Drop rate limit | **New API** `sflow_drop_rate_limit_set { max_pps, burst }` (Phase 1.5) |
| Per-port sub-sampling factor | **New API** `sflow_interface_subsample_set { hw_if_index, sub_factor, desired_rate }` (Phase 3) |
| Per-port sampling direction | **New API** `sflow_interface_direction_set { hw_if_index, direction }`, where `direction ∈ { ingress, egress, both, none }`. Replaces (per port) what the global `sflow_direction_set` does today. (Phase 3) |

VPP's existing public APIs cover everything Phase 1 needs. Phases 2/3 add
three new APIs to the plugin (per-port sub-sampling, per-port direction,
drop-rate limit), all backward-compatible: the existing global
`sflow_direction_set` and `sflow_sampling_rate_set` remain functional and
take effect when no per-port override is set.

### 4.3 ifindex coherence — reporting Linux netdev ifindex

Default plugin behavior writes VPP `hw_if_index` into `PSAMPLE_ATTR_IIFINDEX`
and `NET_DM_ATTR_IN_PORT`. SONiC `mod_psample` and `mod_dropmon` will
fail to resolve those to SONiC port names. Two acceptable solutions:

**Option A (preferred): split responsibilities — `mod_sonic` for counters,
`mod_vpp` for ifIndex translation.**

`mod_vpp` (in `hsflowd` >= 2.1.10) accepts a `osIndex=on` config option that
tells it to operate purely as an ifIndex translator without taking over
counter polling. With this set:

- `mod_vpp` listens on the VPP plugin's USERSOCK channel and learns each
  port's `(VPP sw_if_index, Linux OSINDEX, name, mac)` tuple
  (LCP-resolved by VPP).
- `mod_vpp` registers / updates `SFLAdaptor` entries in the **Linux ifIndex**
  namespace and marks them `switchPort = YES`.
- When a PSAMPLE message arrives carrying `IIFINDEX = VPP hw_if_index`,
  `mod_vpp`'s `evt_psample` translates it to the Linux ifIndex of the
  corresponding SONiC port before the flow sample is keyed and emitted.
- `mod_vpp`'s counter / counter-sample / discard-sample event handlers are
  **not subscribed** when `osIndex=on` (guarded in upstream `mod_vpp.c`),
  so they cannot collide with `mod_sonic`.
- `mod_sonic` remains the sole counter authority — reads `COUNTERS_DB`
  (canonical SAI counters), drives the per-adaptor poller, emits
  `counter_sample` records keyed by the same Linux ifIndex.

Result: flow samples, drop alerts, and counter samples for the same SONiC
port all carry the same Linux ifIndex, with counters sourced from
`COUNTERS_DB` (matching SNMP / CLI / everything else in SONiC) and ifIndex
translation handled by `mod_vpp` natively — no VPP-side patch required.

Requirements:

- Bump `hsflowd` to >= 2.1.10 in `dockers/docker-sflow/buildinfo/versions/`
  and the matching host-sflow git pin.
- Add the following stanzas to `/etc/hsflowd.conf` template in
  `dockers/docker-sflow/`:

```text
sonic   { unixGroup=1 }
psample { group=1 }
dropmon { group=1 }
vpp     { osIndex = on }
```

- VPP linux-cp plugin loaded (already on in sonic-vpp).
- `docker-sflow` shares the netns where VPP runs (already true — host netns
  on SONiC).

Known caveat: order-of-init. If a PSAMPLE message arrives before `mod_vpp`
has learned `OSINDEX` for that port, the sample is keyed to an unmapped
ifIndex. `mod_vpp` resolves this on the next tick; in practice the first
port's first sample after a cold start may be misattributed for <= 1
second, which is acceptable.

**Option B (fallback): patch VPP plugin to substitute the LCP-resolved
Linux netdev ifindex.**

Used when bumping `hsflowd` is undesirable (e.g. an interim release where
the pin cannot move). Implemented in `sflow.c` send paths, gated by a new
global flag `smp->report_linux_ifindex` (default off; sonic-vpp sets it to
on at init).

```c
static inline u32
sflow_translate_ifindex(sflow_main_t *smp, u32 hw_if_index) {
    if (!smp->report_linux_ifindex || hw_if_index == 0 || hw_if_index == ~0)
        return hw_if_index;
    if (!smp->lcp_itf_pair_get_vif_index_by_phy)
        return hw_if_index;
    vnet_hw_interface_t *hw =
        vnet_get_hw_interface_safe(smp->vnet_main, hw_if_index);
    if (!hw) return hw_if_index;
    u32 lin = (*smp->lcp_itf_pair_get_vif_index_by_phy)(hw->sw_if_index);
    return (lin == INDEX_INVALID) ? hw_if_index : lin;
}
```

Substitute in `send_packet_sample()` for `IIFINDEX`/`OIFINDEX` and in
`send_packet_dropmon()` for `NET_DM_ATTR_IN_PORT`.

CLI: `sflow report-linux-ifindex { on | off }` and matching API.


### 4.4 Per-port sample rate

VPP's stock plugin uses a single global `samplingN`. To honor per-port rates
we add **GCD + per-port sub-sampling** (GCD = greatest common divisor; the
upstream plugin TODO calls this "HCF + sub-sampling" — same idea):

1. Each port that has `INGRESS_SAMPLEPACKET_ENABLE = <samplepacket_oid>` records
   `sfif->desired_rate` from the bound samplepacket object.
2. **Quantize** `desired_rate` to a small set of allowed values before using
   it in the GCD (see §4.4.1 below) so the GCD does not collapse to 1.
3. Compute `smp->samplingN = GCD(all enabled sfif->quantized_rate)` whenever
   the set changes; refresh worker threads via existing
   `sflow_set_worker_sampling_state()`.
4. In `send_packet_sample()` (slow path, after a candidate is drawn), apply
   per-port sub-sampling:

```c
u32 sub = sfif->quantized_rate / smp->samplingN;
if (sub > 1) {
    if ((++sfif->sub_counter % sub) != 0) return 0; /* drop candidate */
}
SFLOWPS_set_attr_int(pst, SFLOWPS_PSAMPLE_ATTR_SAMPLE_RATE, sfif->quantized_rate);
```

5. Reported `SAMPLE_RATE` in the PSAMPLE message uses the **port-quantized**
   rate so the collector applies the correct estimator (which is what the
   operator effectively configured — see §4.4.1).

This keeps the fastpath unchanged (one global rate, per-worker random skip).

#### 4.4.1 Avoiding a degenerate GCD

The naive scheme breaks when two configured rates are coprime. For example,
if port A is set to 4999 and port B to 5000:

$$
\gcd(4999, 5000) = 1
$$

A global rate of 1 would force the fastpath to treat every packet as a
sample candidate, then sub-sample by 4999× / 5000× in the slow path —
essentially negating the whole point of upstream 1-in-N filtering and
causing a CPU cliff.

We defend against this with **rate quantization** before the GCD is taken.
Each configured `desired_rate` is mapped to the nearest value in a fixed
allowed set, then the GCD is taken over the quantized values.

**Allowed set (default):** rates of the form `{1, 2, 5} × 10ᵏ` for `k = 0..6`,
plus exact powers of two:

```
  10, 20, 50,
  100, 200, 500,
  1000, 2000, 5000,
  10000, 20000, 50000,
  100000, 200000, 500000,
  1000000
```

This is the conventional sFlow rate ladder used by most collectors, and it
guarantees a useful GCD because every member shares factors of 10 (or 2 and
5) with every other member. Concrete consequences:

| Configured | Quantized | Notes |
|---|---|---|
| 4999 | 5000 | Snapped up to the nearest allowed value. |
| 5000 | 5000 | No change. |
| 1234 | 1000 | Snapped down. |
| 1500 | 2000 (or 1000) | Tie-breaker: round to nearest; on exact midpoint round to coarser. |
| 7 | 10 | Below practical minimum; clamped. |

With quantization, `gcd(4999_q, 5000_q) = gcd(5000, 5000) = 5000`, the
fastpath samples 1-in-5000, both ports end up with `sub = 1`, no slow-path
over-sampling, and the collector sees `SAMPLE_RATE = 5000` for both — which
is the operator's intent at any reasonable granularity.

**Rounding policy** is configurable per platform:

- `nearest` (default): minimal effective rate change;
- `down`: never under-sample (effective rate ≤ configured);
- `up`: never over-sample (effective rate ≥ configured).

**Operator visibility:** the per-port quantized rate is reported in
`show sflow interface` so the operator can confirm what is actually being
applied. If the configured rate snaps to a different value, vpplib logs a
NOTICE-level message ("port Ethernet0 sample-rate 4999 quantized to 5000")
at configuration time.

**Sanity bound:** the plugin enforces
`smp->samplingN >= SFLOW_MIN_GLOBAL_RATE` (default 100). If the GCD
computation ever yields a smaller value (which the quantization should
prevent), the plugin clamps the global rate up and adjusts per-port `sub`
factors accordingly, logging a WARN.

**Alternative considered — dynamic LCM-based bucketing:** instead of quantizing,
compute `samplingN = gcd(rates)` and accept any value, but cap the slow-path
sub-sample multiplier so that effective rate degradation is bounded
(e.g. `max sub <= 64`). Rejected because (a) it makes the effective rate
depend on the *combination* of ports configured (changing port C's rate
shifts port A's effective rate), which violates least-surprise, and
(b) the quantized scheme is what every commercial sFlow stack uses.

### 4.5 Per-port sampling direction

SONiC binds two attributes to a port — `INGRESS_SAMPLEPACKET_ENABLE` and
`EGRESS_SAMPLEPACKET_ENABLE` — each independently pointing to a
`SAMPLEPACKET` object. Today the VPP plugin tracks direction in a single
global `smp->samplingD` and applies it uniformly. The two sampling paths
are already wired as **separate** vnet feature arcs (`device-input`/`sflow`
for ingress, `interface-output`/`sflow-egress` for egress), so making
direction per-port is bookkeeping in the plugin's enable/disable logic
and requires no datapath rework.

**Plugin changes:**

- Add a `direction` field (`SFLOW_DIRN_INGRESS | EGRESS | BOTH`) to
  `sflow_per_interface_data_t`, defaulting to `INGRESS`.
- In `sflow_enable_disable_interface()`, derive `ingress_on` / `egress_on`
  from `sfif->direction` (falling back to the global `smp->samplingD`
  when no per-port direction has been set), then call
  `vnet_feature_enable_disable()` on each arc independently. The
  existing idempotent compare-and-flip logic already handles the
  attach/detach correctly when direction transitions at runtime.
- New plugin API + CLI:
  - `sflow_interface_direction_set { sw_if_index, direction }`
  - `sflow interface <intf> direction { ingress | egress | both }`
- The existing global `sflow_direction_set` API and CLI continue to work
  as a default for ports that have not had a per-port direction set,
  preserving backwards compatibility.

**vpplib (SAI) changes:** translate the four states of
`(INGRESS_SAMPLEPACKET_ENABLE, EGRESS_SAMPLEPACKET_ENABLE)` into one
direction call:

| ingress bound | egress bound | action |
|---|---|---|
| no  | no  | `sflow_enable_disable(sw_if_index, off)` |
| yes | no  | direction=`INGRESS`, then `enable_disable(on)` |
| no  | yes | direction=`EGRESS`,  then `enable_disable(on)` |
| yes | yes | direction=`BOTH`,    then `enable_disable(on)` |

Direction must be set **before** enabling the port to avoid briefly
attaching the wrong arc.

**Edge cases:**

- *Different sample rates per direction.* SONiC allows the ingress and
  egress bindings on the same port to point to two `SAMPLEPACKET` objects
  with different `SAMPLE_RATE` values. The VPP plugin keeps a single
  per-port rate (§4.4); vpplib will reject non-equal rates with
  `SAI_STATUS_NOT_SUPPORTED`. Documented as a known limitation.
- *Drop monitoring* fires on the `error-drop` arc and is direction-
  agnostic; per-port direction has no effect on drop samples.
- *Direction reporting.* No change to `node.c`/`sflow_psample.c` is
  needed: ingress samples already populate `IIFINDEX` from the
  `device-input` arc and egress samples populate `OIFINDEX` from the
  `interface-output` arc, so direction is implicit in the PSAMPLE
  message.

### 4.6 Drop notification rate limiting

Add a token-bucket guard in the plugin's drop-send slow path
(`send_packet_dropmon`):

```c
if (smp->drop_max_pps) {
    refill_tokens(smp);
    if (smp->drop_tokens <= 0) { smp->drop_rate_limited++; continue; }
    smp->drop_tokens--;
}
```

Programmed by vpplib from the SAI policer attached to the `SAMPLEPACKET` trap
group's policer (`SAI_POLICER_ATTR_PIR`, `SAI_POLICER_ATTR_CBS`).

### 4.7 docker-sflow / hsflowd integration

Two configurations supported, matching the two options in §4.3:

- **Preferred (Option A)**: bump `hsflowd` to >= 2.1.10 and run `mod_sonic`
  (counters from COUNTERS_DB) alongside `mod_vpp` with `osIndex=on`
  (ifIndex translation only). No VPP plugin patch required for ifIndex.
- **Fallback (Option B)**: keep `hsflowd 2.0.51-26`, patch the VPP sflow
  plugin to write Linux netdev ifIndex into PSAMPLE / NET_DM messages,
  use `mod_psample` + `mod_dropmon` + `mod_sonic` only.

`hsflowd.conf` template in `dockers/docker-sflow/` for **Option A**:

```text
sonic   { unixGroup=1 }
psample { group=1 }
dropmon { group=1 }
vpp     { osIndex = on }
```

`hsflowd.conf` template for **Option B**:

```text
sonic   { unixGroup=1 }
psample { group=1 }
dropmon { group=1 }
# vpp module not present in hsflowd 2.0.51
```

The `hsflowd` daemon must run in the same network namespace as VPP (the
`docker-sflow` container already shares `host` netns in SONiC).

Version bump (Option A) touches:

- `dockers/docker-sflow/buildinfo/versions/versions-deb-bookworm`
- `files/build/versions/dockers/docker-sflow/versions-deb-bookworm`
- `files/build/versions-public/dockers/docker-sflow/versions-deb-bookworm`
- `dockers/docker-sflow/buildinfo/versions/versions-git`
- The `host-sflow` submodule pin under `src/sflow/hsflowd/host-sflow`.

### 4.8 VPP plugin enablement

- Ensure the `sflow` plugin is built (already is — present under
  `vppbld/repo/src/plugins/sflow/`).
- Add `plugin sflow_plugin.so { enable }` to `startup.conf` template for VPP
  in `platform/vpp/`.
- Confirm the linux-cp plugin remains enabled (required for ifindex
  translation; already on in sonic-vpp).
- Ensure the kernel `psample` and `drop_monitor` modules are loaded in the VPP
  netns (host netns on SONiC). Add modprobe lines in
  `platform/vpp/docker-sonic-vpp/start.sh` if not already present.

## 5. Implementation Plan

### Phase 1 — Basic sFlow functionality

Goal: a single global sample rate, per-port enable/disable, working flow
samples and counters end-to-end with the standard SONiC sFlow stack on
sonic-vpp. Drop monitor optional in this phase.

Deliverables:

- vpplib `SaiVppSflow` skeleton:
  - `create/remove/set/get` for `SAI_OBJECT_TYPE_SAMPLEPACKET`.
  - `SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE` set handler that calls
    `sflow_enable_disable` and `sflow_sampling_rate_set` for the **global**
    rate (per-port rate honored only if all ports use the same rate; otherwise
    last-writer-wins, log a WARNING).
  - `SAI_HOSTIF_TRAP_TYPE_SAMPLEPACKET` create/remove as bookkeeping.
  - `SAI_HOSTIF` of type GENETLINK + table entry as bookkeeping.
- VPP startup.conf changes to load `sflow_plugin.so`.
- `docker-sonic-vpp/start.sh` ensures `psample` and `drop_monitor` kernel
  modules loaded.
- `docker-sflow` configuration enabling `mod_psample`, `mod_dropmon`,
  `mod_sonic`.
- Smoke test: single-port sFlow with `sflowtool` collector running in vs
  topology shows flow samples and counter samples for that port.

Exit criteria:

- `config sflow enable; config sflow interface enable Ethernet0; config sflow
  collector add c1 1.1.1.1 6343` results in flow samples reaching `sflowtool
  -p 6343`.
- `show sflow interface` reflects configured state.
- COUNTERS_DB-based counter samples appear at the configured polling interval.

### Phase 2 — ifindex coherence

Goal: PSAMPLE and DROPMON messages carry Linux netdev ifindex so flow samples
report against the correct SONiC port name in `sflowtool` and at the
collector.

Deliverables:

- VPP plugin patch: add `report_linux_ifindex` flag, helper
  `sflow_translate_ifindex()`, substitute in `send_packet_sample()` and
  `send_packet_dropmon()`.
- New API/CLI: `sflow_report_linux_ifindex_set { enable }` and
  `sflow report-linux-ifindex { on | off }`.
- vpplib enables it at init via the new API.
- Patch maintained under `platform/vpp/vppbld/patches/sflow-report-linux-ifindex.patch`,
  to be submitted upstream.

Exit criteria:

- `sflowtool -t` against vs topology shows interface names matching SONiC port
  names (`Ethernet0`, `Ethernet4`, …) instead of unresolved numeric ifindex.
- Drop alerts show the same Linux ifindex.

### Phase 3 — Per-port sample rate and direction

Goal: honor distinct sample rates **and** distinct sampling directions
configured on different SONiC ports.

Deliverables:

- VPP plugin patch (sample rate):
  - `sflow_per_interface_data_t` adds `desired_rate`, `sub_counter`.
  - GCD computation on per-port enable/rate change.
  - Per-port sub-sampling check in `send_packet_sample()`.
  - Reported `PSAMPLE_ATTR_SAMPLE_RATE` reflects per-port `desired_rate`.
- VPP plugin patch (direction):
  - `sflow_per_interface_data_t` adds `direction` (ingress / egress / both /
    none); when unset, falls back to the global `smp->samplingD`.
  - In `sflow_enable_disable_interface()`, decide attaching the
    `device-input` (ingress) and/or `interface-output` (egress) feature arcs
    independently per port, based on the port's own direction.
- New VPP APIs:
  - `sflow_interface_subsample_set { hw_if_index, desired_rate }`.
  - `sflow_interface_direction_set { hw_if_index, direction }`.
- vpplib `SaiSamplepacket` honors per-port `SAI_SAMPLEPACKET_ATTR_SAMPLE_RATE`
  by computing & pushing GCD + per-port factor.
- vpplib distinguishes `SAI_PORT_ATTR_INGRESS_SAMPLEPACKET_ENABLE` vs
  `SAI_PORT_ATTR_EGRESS_SAMPLEPACKET_ENABLE` and translates each to the
  corresponding direction bit on the port via
  `sflow_interface_direction_set`.
- Patches maintained under:
  - `platform/vpp/vppbld/patches/sflow-per-port-rate.patch`
  - `platform/vpp/vppbld/patches/sflow-per-port-direction.patch`

Optional sub-step (3.5): drop-rate limiter (token bucket in slow path),
programmed from `SAI_HOSTIF_TRAP_GROUP_ATTR_POLICER`.

Exit criteria:

- Configure two ports with different rates (e.g. 1000 and 5000); collector
  shows distinct effective rates per port, both reported correctly.
- Configure one port ingress-only and another port both-direction;
  collector shows ingress flow samples on the first and ingress + egress
  flow samples on the second; the global `sflow direction` setting does not
  override either.
- Sub-sampling counter visible in `show sflow` per-port.

### Phase 4 — sonic-mgmt regression

Goal: pass the upstream sonic-mgmt sFlow test suite against a sonic-vpp DUT.

Deliverables:

- sonic-mgmt topology updates needed (if any) for sonic-vpp.
- Test fixtures in `platform/vpp/tests/` to spin up the sonic-mgmt sFlow
  testbed against a sonic-vpp `vs` DUT.
- Bug fixes from running the suite (record, fix, re-run).
- CI integration: nightly job that runs the sFlow suite.

Exit criteria:

- `sonic-mgmt/tests/sflow/` passes against sonic-vpp with no test exclusions
  beyond those already excluded for non-sFlow reasons.

### Phase 5 (optional) — bump hsflowd / enable mod_vpp

Only pursued if Phase 2's substitution approach proves insufficient (e.g. need
sub-flow ifindex-on-LAG, or want the cleaner usersock counter export):

- Bump `hsflowd` pin in `dockers/docker-sflow/buildinfo/versions/versions-deb`
  to a release containing `mod_vpp`.
- Add `vpp { }` to `/etc/hsflowd.conf` template.
- Validate co-existence with `mod_sonic` (counters from COUNTERS_DB win).

## 6. SONiC sFlow Configuration Guide (sonic-vpp specifics)

Standard `config sflow` CLI is unchanged. Examples below assume a sonic-vpp
DUT with `Ethernet0..Ethernet32`.

### 6.1 Enable sFlow globally and add a collector

```bash
config sflow enable
config sflow collector add collector1 198.51.100.10 6343
config sflow polling-interval 20
config sflow agent-id add Loopback0
```

For on-box verification (no external collector required), add a **loopback
collector** so `hsflowd` ships datagrams to `127.0.0.1` where `sflowtool`
can decode them:

```bash
config sflow collector add local 127.0.0.1 --port 6343
```

Multiple collectors are allowed, so the loopback entry can coexist with a
remote collector during bring-up and be removed later with
`config sflow collector del local`.

### 6.2 Per-port configuration

```bash
# Enable on a specific port
config sflow interface enable Ethernet0

# Set a port-specific sample rate (Phase 3)
config sflow interface sample-rate Ethernet0 1000
config sflow interface sample-rate Ethernet8 5000

# Disable on a port
config sflow interface disable Ethernet4
```

Verify:

```bash
show sflow
show sflow interface
```

### 6.3 Drop monitor (Phase 1.5+)

If/when drop monitor is enabled in sonic-vpp:

```bash
# (illustrative; uses standard SONiC drop monitor CLI)
config dropmon enable
```

The trap group policer set by `sflowOrch` will be honored once Phase 3.5 ships.

### 6.4 Validation from inside the device — loopback collector

The quickest way to confirm that VPP is sampling and `hsflowd` is
encapsulating the samples correctly is to point `hsflowd` at the loopback
address (§6.1) and run `sflowtool` inside the `sflow` container:

```bash
# Decode flow / counter / drop samples in human-readable form
docker exec -it sflow sflowtool -p 6343

# One line per flow sample with parsed L2/L3/L4 fields
docker exec -it sflow sflowtool -p 6343 -l

# Write the sampled header bytes as a tcpdump-readable pcap
docker exec -it sflow sflowtool -p 6343 -t > /tmp/samples.pcap
```

Check the VPP-side state in parallel:

```bash
docker exec sonic vppctl show sflow
docker exec sonic vppctl show sflow interface HundredGigabitEthernet0/0/0
```

What to expect:

- With the preferred Option A (§4.3, `mod_vpp` osIndex=on), `sflowtool`
  prints `inputPort` / `outputPort` matching the **Linux netdev ifIndex**
  of the SONiC port — cross-check with `ip link show Ethernet0`.
- Counter samples come from `mod_sonic` (COUNTERS_DB) and key on the same
  ifIndex.
- `flowSample` records contain the first `header-bytes` (default 128) of
  the sampled packet; bump via `sflow header-bytes 256` if you need full
  L4 visibility on jumbo flows.
- Drop alerts surface as `discardSample` records when running a recent
  enough `sflowtool`; older builds will still show flow + counter samples
  while drops only appear in `dropwatch` / `mod_dropmon` logs.

For port-mirroring style debug (forward decoded samples to another tool),
`sflowtool` itself can re-emit:

```bash
docker exec -d sflow sflowtool -p 6343 -f 198.51.100.20/6343
```

### 6.5 Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| No flow samples at collector | psample kmod not loaded, or VPP plugin not enabled | `lsmod | grep psample`, `vppctl show plugin | grep sflow` |
| Samples arrive but interface name is numeric or wrong | ifindex mismatch | Check `vppctl show sflow report-linux-ifindex`; should be `on`. Verify `lcp pair` for the port. |
| Counter samples missing one port | SONiC not collecting that port's stats in COUNTERS_DB | Check `redis-cli -n 2 KEYS COUNTERS:oid:* | head` and `mod_sonic` log in `docker-sflow` |
| Drop alerts flooding the collector | No rate limit | Phase 3.5 not yet shipped, or policer not bound to trap group |
| `config sflow interface sample-rate Ethernet0 1000` ignored on some ports | Phase 1 only honors single rate | Use Phase 3 build |

## 7. Test Plan

### 7.1 vpplib unit tests (gtest, runs without VPP)

Cover the SAI vpplib translation layer for sFlow objects:

- Samplepacket object lifecycle (create / set rate / remove) and the
  resulting calls into the VPP binary-API mock.
- Port `INGRESS_SAMPLEPACKET_ENABLE` / `EGRESS_SAMPLEPACKET_ENABLE`
  attribute bind / unbind, including per-port direction transitions.
- Hostif trap of type `SAMPLEPACKET` with `TRAP_GROUP` + `POLICER`, and
  the translation of policer rate into the drop-monitor rate limit.
- Hostif of type `GENETLINK("psample", "packets")` and
  `GENETLINK("NET_DM", "events")` accepted as no-op success.
- GCD helper and rate-quantization (`{1, 2, 5} × 10^k` ladder, rounding
  policies, coprime-rate case from §4.4.1).

### 7.2 VPP plugin patch tests

Cover the per-port sub-sampling, per-port direction, drop rate limit, and
(if Option B in §4.3 is taken) the Linux ifIndex substitution:

- Per-port sub-sampling honors per-port rate within statistical tolerance
  while preserving the global rate sent on the wire.
- Per-port direction independently attaches `device-input` and
  `interface-output` arcs.
- Drop-monitor rate limiter caps NET_DM messages at the configured
  pps/burst and exposes a `drop_rate_limited` counter.
- (Option B only) PSAMPLE / NET_DM messages carry the LCP-resolved Linux
  netdev ifIndex when `report-linux-ifindex` is on.

### 7.3 sonic-mgmt regression (Phase 4)

Run the existing `sonic-mgmt/tests/sflow/test_sflow.py` suite against a
sonic-vpp DUT. The suite covers the following behaviors and all of them
must pass:

| Test class | Behaviors covered | Maps to HLD section |
|---|---|---|
| `TestSflowCollector.test_sflow_config` | Global enable + per-port enable + single collector; flow + counter samples reach the collector for the right port | §3, §4.1, §6.1 |
| `TestSflowCollector.test_collector_del_add` | Dynamic collector remove → samples stop; re-add → samples resume | §6.1 |
| `TestSflowCollector.test_two_collectors` | Two collectors with different UDP ports (6343, 6344); third collector rejected (cap = 2); samples sent to all configured collectors | §6.1 |
| `TestSflowPolling.testPolling` | `polling-interval = 20` produces one counter sample per port per interval | §3 (counters), §4.1 |
| `TestSflowPolling.testDisablePolling` | `polling-interval = 0` disables counter samples | §3, §4.1 |
| `TestSflowPolling.testDifferentPollingInt` | `polling-interval = 60` honored at runtime | §3 |
| `TestSflowInterface.testIntfRemoval` | Disabling sFlow on a subset of ports stops samples on those ports while leaving others active | §4.5 |
| `TestSflowInterface.testIntfSamplingRate` | Different per-port sample rates (256, 1024) honored | §4.4, §4.4.1 |
| `TestAgentId.testNonDefaultAgent` | Agent-id set to `Loopback0` reflected in samples | §6.1 |
| `TestAgentId.testDelAgent` | Agent-id deleted → hsflowd default heuristic chooses an IP, samples reflect it | §6.1 |
| `TestAgentId.testAddAgent` | Agent-id set to `eth0` reflected in samples | §6.1 |
| `TestReboot.testRebootSflowEnable` | Cold reboot with sFlow enabled — config persists, samples resume | §4 (config persistence) |
| `TestReboot.testRebootSflowDisable` | Cold reboot with sFlow disabled — stays disabled | §4 |
| `TestReboot.testFastreboot` | Fast reboot — config + samples preserved | §4 |
| `TestReboot.testWarmreboot` | Warm reboot — config + samples preserved | §4 |

Coverage gaps that the existing suite does **not** exercise and which we
will track separately:

- **Per-port sampling direction** (§4.5) — sonic-mgmt tests only ingress
  sampling; egress and bidirectional cases will need a sonic-vpp
  extension or an upstream addition.
- **Drop monitoring** (§4.6) — not exercised; drop-sample rate-limit and
  policer-driven configuration covered only by §7.2 plugin tests until a
  sonic-mgmt drop-monitor case is added.
- **ifIndex coherence** (§4.3) — sonic-mgmt verifies sample reception but
  does not assert that flow-sample `inputPort` equals the SONiC port's
  Linux ifIndex; this is verified by manual loopback-collector inspection
  (§6.4) until an explicit assertion is added.

### 7.4 Negative / robustness tests

- Remove a samplepacket OID while a port is still bound — graceful unbind,
  no VPP crash.
- Large fan-out: many ports with distinct rates — GCD stable, no
  performance cliff.
- `hsflowd` outage and restart — samples resume, VPP drop counter
  advances during outage.
- Interface down / up — cached per-port state remains consistent without
  re-enable.

## 8. References

- [SONiC sFlow HLD](https://github.com/sonic-net/SONiC/blob/master/doc/sflow/sflow_hld.md)
- VPP `sflow` plugin: `platform/vpp/vppbld/repo/src/plugins/sflow/`
- host-sflow / hsflowd: <https://github.com/sflow/host-sflow>
- RFC 3176: InMon Corporation's sFlow
- `platform/vpp/TODO.md` — original gap list including
  `SAI_OBJECT_TYPE_SAMPLEPACKET`
