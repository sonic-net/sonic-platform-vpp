# SONiC-VPP LAG Member Egress Disable HLD

Rev v0.1

## Table of Contents

1. [Revisions](#1-revisions)
2. [Scope](#2-scope)
3. [Definitions](#3-definitions)
4. [Background](#4-background)
5. [Problem Statement](#5-problem-statement)
6. [Goals and Non-Goals](#6-goals-and-non-goals)
7. [High Level Design](#7-high-level-design)
8. [Behavior Matrix](#8-behavior-matrix)
9. [Call Flows](#9-call-flows)
10. [Error Handling](#10-error-handling)
11. [Limitations and Future Work](#11-limitations-and-future-work)
12. [References](#12-references)

## 1. Revisions

| Rev | Date | Author(s) | Changes |
|---|---|---|---|
| v0.1 | 2026-06-05 | Chenyang Wang | Initial HLD for VPP LAG member egress disable support |

## 2. Scope

This document describes how SONiC-VPP handles `SAI_LAG_MEMBER_ATTR_EGRESS_DISABLE` for LAG members in the VPP-backed virtual switch path.

The design covers:

- Creating a LAG member while egress is disabled.
- Dynamically disabling egress on an existing LAG member.
- Dynamically enabling egress on a previously disabled LAG member.
- Keeping VPP bond membership aligned with SONiC SAI state.
- Preserving the existing delayed LCP/tap creation behavior for VPP LAGs.

This document is a focused extension of the broader [SONiC VPP LAG Support](vpp-lag.md) HLD.

## 3. Definitions

| Term | Meaning |
|---|---|
| LAG | Link Aggregation Group, exposed in SONiC as a PortChannel |
| LAG member | A physical port that belongs to a LAG |
| Bond | VPP BondEthernet interface used to model a SONiC PortChannel |
| LCP | VPP Linux Control Plane plugin, used to pair VPP interfaces with Linux interfaces |
| Egress disable | SAI LAG member attribute that prevents a member from being used for egress traffic |

## 4. Background

SONiC represents a PortChannel as a SAI LAG object and each member port as a SAI LAG member object. In SONiC-VPP, the SAI-VPP layer translates these objects into VPP bond configuration:

```text
SONiC PortChannel
    -> SAI LAG
        -> VPP BondEthernet

SONiC PortChannel member
    -> SAI LAG member
        -> VPP bond member
```

The existing VPP LAG design creates the VPP LAG bond first, then adds member ports to the bond. For L3 PortChannel host-path support, SONiC-VPP also creates an LCP/tap pair for the bond interface.

One important detail is that LCP/tap creation is intentionally delayed until the first enabled member is added. Creating the tap too early, while the VPP bond has no active member, can leave the tap with a stale generated MAC address instead of the member or bond MAC address.

## 5. Problem Statement

Before this feature, SONiC-VPP created and removed VPP bond members, but it did not react to updates of `SAI_LAG_MEMBER_ATTR_EGRESS_DISABLE`.

That caused two gaps:

1. If a LAG member was created with egress disabled, SONiC-VPP could still add it to the VPP bond.
2. If SONiC later changed the egress-disable state of a LAG member, VPP bond membership did not change accordingly.

As a result, SONiC control-plane state and VPP dataplane state could diverge.

## 6. Goals and Non-Goals

### Goals

- Honor `SAI_LAG_MEMBER_ATTR_EGRESS_DISABLE` in the VPP dataplane.
- Keep disabled LAG members out of VPP bond egress selection.
- Add a member back to the VPP bond when egress is re-enabled.
- Treat a missing stored `EGRESS_DISABLE` attribute as the SAI default value, `false`.
- Avoid unnecessary VPP operations when the requested egress-disable state does not change.
- Keep delayed LCP/tap creation intact, so the bond LCP pair is created only when an enabled member exists.

### Non-Goals

- No change to SONiC CLI, Config DB, APP DB, or Orchagent behavior.
- No change to LACP protocol handling.
- No change to VPP bond mode or load-balancing algorithm selection.
- No support for additional SAI LAG member attributes beyond this feature.
- No redesign of the existing LAG LCP/tap and `tc` redirect model.

## 7. High Level Design

### 7.1 Design Summary

SONiC-VPP maps the SAI egress-disable state to VPP bond membership:

| SAI egress-disable state | VPP action |
|---|---|
| `true` | Keep the LAG member object in SAI state, but remove the port from the VPP bond |
| `false` | Add the port to the VPP bond if it is not already present |

This keeps the object lifecycle and dataplane programming separate:

- The SAI LAG member object can exist even when egress is disabled.
- The VPP bond member exists only when the member is eligible for egress traffic.

VPP does not expose a SONiC-VPP helper for an egress-only disable flag on an existing bond member. This design therefore models `EGRESS_DISABLE=true` by fully detaching the member from the VPP bond dataplane. That means the member does not participate in VPP bond egress selection, and it also no longer participates as a VPP bond member for dataplane ingress until it is added back. LACP ownership and packet punt/inject behavior remain unchanged: SONiC continues to run LACP, and this feature only changes VPP bond membership.

### 7.2 Create Path

When a LAG member is created:

1. Store the LAG member in the normal SAI-VPP object state.
2. Check whether `SAI_LAG_MEMBER_ATTR_EGRESS_DISABLE` is present and set to `true`.
3. If egress is disabled, stop after storing the object. Do not add the member to the VPP bond.
4. If egress is not disabled, add the member to the VPP bond.
5. Ensure LCP/tap setup for the bond only after adding an enabled member.

`create_internal()` only records the SAI object in the virtual switch state. VPP bond programming happens later through `vpp_create_lag_member()`, and that step is skipped for an initially disabled member.

```text
create LAG member
    |
    v
store SAI object state
    |
    v
egress_disable == true?
    | yes
    v
defer VPP bond member add

egress_disable == false or absent?
    |
    v
add port to VPP bond
    |
    v
ensure bond LCP/tap exists
```

### 7.3 Set Path

`SwitchVpp::set()` routes `SAI_OBJECT_TYPE_LAG_MEMBER` updates to `SwitchVpp::setLagMember()`.

`setLagMember()` handles only `SAI_LAG_MEMBER_ATTR_EGRESS_DISABLE` specially. All other attributes continue to use the existing generic `set_internal()` path.

The set path follows this order:

1. Validate that the LAG member object exists.
2. Read the current stored `EGRESS_DISABLE` value.
3. Decide whether the requested value requires no action, detach, or add.
4. Program VPP first.
5. Update the stored SAI object state only after the VPP operation succeeds.

This order prevents SONiC-VPP from recording a new SAI state when the corresponding VPP programming fails.

### 7.4 Action Selection

The action selector uses three inputs:

- Requested `egress_disable` value from the new SAI set operation.
- Whether the current stored attribute exists.
- Current stored `egress_disable` value, if present.

The result is one of:

| Action | Meaning |
|---|---|
| `NONE` | No VPP bond membership change is needed |
| `DETACH` | Remove the member from the VPP bond |
| `ADD` | Add the member back to the VPP bond |

If the current stored attribute is missing, SONiC-VPP treats it as the SAI default `false`.

### 7.5 Detach from VPP Bond

When egress is disabled:

```text
set EGRESS_DISABLE=true
    |
    v
remove member from VPP bond
    |
    v
update stored SAI attr to true
```

The SAI object remains present. Only the VPP dataplane membership changes.

### 7.6 Add Back to VPP Bond

When egress is re-enabled:

```text
set EGRESS_DISABLE=false
    |
    v
read stored LAG_ID and PORT_ID
    |
    v
add member to VPP bond
    |
    v
ensure bond LCP/tap exists
    |
    v
update stored SAI attr to false
```

The add operation reconstructs the minimum attribute list needed by the existing VPP LAG member creation helper:

- `SAI_LAG_MEMBER_ATTR_LAG_ID`
- `SAI_LAG_MEMBER_ATTR_PORT_ID`

This reuses the existing VPP bond-member creation logic instead of adding a parallel path.

### 7.7 LCP/Tap Handling

LCP/tap setup remains tied to enabled VPP bond membership.

`vpp_ensure_lag_lcp()` creates the LCP/tap pair only if it has not already been created for the LAG. This helper is called after adding an enabled member to the VPP bond.

This preserves the existing design requirement:

- Do not create the bond LCP/tap pair for an empty bond.
- Create it when the first enabled member is added.
- Reuse the existing pair for later member additions.

If all enabled members are later disabled after the LCP/tap pair already exists, this feature does not remove the LCP/tap pair immediately. The LAG can temporarily have an existing LCP/tap pair and no enabled VPP bond members. Re-enabling a member reuses the existing LCP/tap pair, and normal LAG removal remains responsible for LAG-level cleanup.

### 7.8 Remove Path and Lifecycle Edge Cases

The LAG member remove path is not redesigned by this feature. The expected lifecycle behavior is:

| Case | Expected behavior |
|---|---|
| Remove an enabled member | Remove it from the VPP bond, then remove the SAI object state |
| Remove a disabled member | Remove the SAI object state; VPP bond membership is already absent and should be treated as a no-op or tolerated by the remove path |
| Disable the last enabled member | Remove the member from the VPP bond; keep the LAG object and any existing LCP/tap state |
| Re-enable a member after all members were disabled | Add the member back to the VPP bond and reuse or create LCP/tap state as needed |

Deleting a disabled member is an important regression scenario because the member may already be absent from the VPP bond when the remove operation arrives.

## 8. Behavior Matrix

| Current stored attr exists? | Current value | Requested value | VPP action | Stored state update |
|---|---|---|---|---|
| Yes | `false` | `false` | None | Set attr to `false` |
| Yes | `false` | `true` | Detach member from VPP bond | Set attr to `true` after detach succeeds |
| Yes | `true` | `false` | Add member to VPP bond | Set attr to `false` after add succeeds |
| Yes | `true` | `true` | None | Set attr to `true` |
| No | default `false` | `false` | None | Set attr to `false` |
| No | default `false` | `true` | Detach member from VPP bond | Set attr to `true` after detach succeeds |

## 9. Call Flows

### 9.1 LAG Member Created with Egress Enabled

```text
syncd
  |
  v
SwitchVpp::createLagMember()
  |
  v
create_internal(SAI_OBJECT_TYPE_LAG_MEMBER)
  |
  v
vpp_create_lag_member()
  |
  v
add member to VPP BondEthernet
  |
  v
vpp_ensure_lag_lcp()
```

### 9.2 LAG Member Created with Egress Disabled

```text
syncd
  |
  v
SwitchVpp::createLagMember()
  |
  v
create_internal(SAI_OBJECT_TYPE_LAG_MEMBER)
  |
  v
detect EGRESS_DISABLE=true
  |
  v
return without adding member to VPP bond
```

### 9.3 Existing LAG Member Disabled

```text
syncd
  |
  v
SwitchVpp::set(SAI_OBJECT_TYPE_LAG_MEMBER)
  |
  v
SwitchVpp::setLagMember(EGRESS_DISABLE=true)
  |
  v
getLagMemberEgressDisableAction() -> DETACH
  |
  v
vpp_remove_lag_member()
  |
  v
set_internal(EGRESS_DISABLE=true)
```

### 9.4 Existing LAG Member Re-Enabled

```text
syncd
  |
  v
SwitchVpp::set(SAI_OBJECT_TYPE_LAG_MEMBER)
  |
  v
SwitchVpp::setLagMember(EGRESS_DISABLE=false)
  |
  v
getLagMemberEgressDisableAction() -> ADD
  |
  v
read stored LAG_ID and PORT_ID
  |
  v
vpp_create_lag_member()
  |
  v
vpp_ensure_lag_lcp()
  |
  v
set_internal(EGRESS_DISABLE=false)
```

## 10. Error Handling

| Error case | Handling |
|---|---|
| Null set attribute | Return `SAI_STATUS_INVALID_PARAMETER` |
| LAG member object not found | Return `SAI_STATUS_ITEM_NOT_FOUND` |
| Unsupported LAG member attribute | Use existing generic `set_internal()` behavior |
| VPP detach fails | Return the VPP failure status and do not update stored `EGRESS_DISABLE` state |
| VPP add fails | Return the VPP failure status and do not update stored `EGRESS_DISABLE` state |
| Stored `LAG_ID` missing while re-adding | Return `SAI_STATUS_FAILURE` |
| Stored `PORT_ID` missing while re-adding | Return `SAI_STATUS_FAILURE` |
| Stored `LAG_ID` or `PORT_ID` has the wrong object type | Return `SAI_STATUS_FAILURE` |
| Port tap or VPP hardware interface lookup fails | Return `SAI_STATUS_FAILURE` |
| Bond interface name lookup fails during LCP setup | Return `SAI_STATUS_FAILURE` |
| VPP bond member API returns an error | Return `SAI_STATUS_FAILURE` |

## 11. Limitations and Future Work

- This design models egress disable by removing the port from the VPP bond. It does not use a VPP-native per-member egress-disable flag.
- This feature depends on stored SAI object state to recover `LAG_ID` and `PORT_ID` when adding a member back to the VPP bond.
- Additional integration coverage can be added after the related SONiC-VPP test topology supports direct validation of this attribute transition.

## 12. References

- [SONiC VPP LAG Support](vpp-lag.md)
- [SONiC VPP Platform HLD](SONICVPP-HLD.md)
- [sonic-sairedis PR #1928](https://github.com/sonic-net/sonic-sairedis/pull/1928)
