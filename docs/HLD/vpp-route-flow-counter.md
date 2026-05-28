# VPP Route Flow Counter HLD

## Table of Contents

1. [Revisions](#revisions)
2. [Scope](#scope)
3. [Definitions and Abbreviations](#definitions-and-abbreviations)
4. [Background](#background)
5. [Problem Statement](#problem-statement)
6. [Proposed Solution](#proposed-solution)
7. [Design Details](#design-details)
8. [Code References](#code-references)
9. [Expected Behavior](#expected-behavior)
10. [Risks and Open Questions](#risks-and-open-questions)
11. [Review Focus](#review-focus)

## Revisions

| Rev | Date | Author(s) |
|-----|------|-----------|
| v0.1 | 05/25/2026 | Chenyang Wang |

## Scope

This document describes the high level design for route flow counter support on the SONiC VPP platform.

The change is implemented in the VPP virtual switch implementation in `sonic-sairedis`, under `vslib/vpp`. It enables SONiC route flow counters to be backed by VPP route statistics so tests and operators can verify that counters increment when traffic hits matched routes.

### In scope

- Capture VPP route `stats_index` when routes are programmed.
- Track route-to-counter and counter-to-route bindings for `SAI_ROUTE_ENTRY_ATTR_COUNTER_ID`.
- Read VPP route packet and byte counters for SAI counters bound to routes.
- Support clear and read-and-clear behavior using stored counter base values.
- Refresh route stats index tracking when route programming changes, including next-hop group member add/remove.
- Build the new route stats helper into the VPP syncd image.

### Out of scope

- Orchagent route flow counter design changes.
- SONiC CLI or database schema changes.
- Non-VPP ASIC behavior.
- sonic-mgmt test implementation changes.

## Definitions and Abbreviations

**VPP**: Vector Packet Processing.

**SAI**: Switch Abstraction Interface.

**syncd-vpp**: SONiC syncd container using the VPP-backed SAI implementation.

**Route flow counter**: SONiC feature that attaches counters to routes matching configured route patterns.

**VPP stats index**: Per-route index returned by VPP route programming APIs and used to query route statistics from VPP stats.

## Background

SONiC route flow counter is configured through the existing control plane flow:

1. User enables route flow counter polling with `counterpoll flowcnt-route enable`.
2. User configures route patterns with `config flowcnt-route pattern add ...`.
3. Orchagent attaches SAI counters to routes matching the configured patterns.
4. Flex counter polling reads SAI counter stats.
5. `show flowcnt-route stats` displays packet and byte counters for matched routes.

For VPP, the SAI implementation must translate the SAI counter read path into VPP route statistics. The latest sonic-mgmt test gap fix in [sonic-net/sonic-mgmt#24553](https://github.com/sonic-net/sonic-mgmt/pull/24553) now sends traffic and verifies that the packet counter increments, so logical counter creation alone is not sufficient.

## Problem Statement

Before this change, VPP route flow counters were not fully backed by VPP dataplane route stats. The main gaps were:

- Route programming did not keep the VPP route stats index needed for later stats lookup.
- Route `COUNTER_ID` binding was not tracked in the VPP SAI layer.
- SAI counter stats reads were not redirected to VPP route stats for route-bound counters.
- Clear and read-and-clear behavior did not have a VPP route counter base mechanism.

This could allow route flow counter objects to exist without providing traffic-backed packet and byte increments.

## Proposed Solution

The VPP SAI layer now treats a SAI counter bound to a route as a view over the corresponding VPP route statistics.

At route add time, VPP returns a `stats_index`. The VPP SAI layer stores that index by serialized route key. When orchagent binds a SAI counter to the route via `SAI_ROUTE_ENTRY_ATTR_COUNTER_ID`, the implementation records the route/counter relationship. When flex counter polling reads the SAI counter, the VPP SAI layer resolves the counter back to the route, finds the route stats index, queries VPP route stats, and returns packet and byte counters.

Clear and read-and-clear are implemented by storing a base snapshot. Returned values are calculated as:

```text
carried delta + (current VPP route stat - stored base stat)
```

The carried delta is normally zero. It is increased before route or next-hop-group reprogramming refreshes the VPP route stats index, so the logical SAI counter does not drop when the new VPP stats index starts from a lower value. Clear and read-and-clear reset carried delta for the requested stats; counter unbind, counter removal, and route removal reset all carried delta for the binding.

## Design Details

### Route programming and stats index capture

The route add/delete helper was extended to optionally return a VPP route stats index. Route add paths call the new helper and cache the returned stats index per serialized route.

If a route has a non-null counter binding but VPP does not return a valid stats index, the route counter binding is rejected with `SAI_STATUS_NOT_SUPPORTED`. This avoids silently accepting a counter that cannot be read later.

### Route counter binding lifecycle

The implementation maintains two maps:

```cpp
std::map<sai_object_id_t, std::string> m_counterToRouteMap;
std::map<std::string, sai_object_id_t> m_routeToCounterMap;
```

These maps support both directions:

- Given a route, find the bound counter during route update or removal.
- Given a counter, find the bound route during stats read.

The binding logic also prevents the same counter from being attached to multiple routes.

### Route stats reads

When `getStatsExt()` receives a stats read for a SAI counter object, VPP checks whether that counter is route-bound. If yes, the read is handled by route counter logic instead of the default virtual switch counter logic.

The route counter read path:

1. Looks up the route from `m_counterToRouteMap`.
2. Looks up the VPP stats index from `m_routeStatsIndexMap`.
3. Queries VPP stats path `/net/route/to`.
4. Returns `SAI_COUNTER_STAT_PACKETS` and `SAI_COUNTER_STAT_BYTES`.

### Counter clear and read-and-clear

The implementation stores a base value per route-bound counter:

```cpp
std::map<sai_object_id_t, std::map<sai_stat_id_t, uint64_t>> m_routeCounterStatsBaseMap;
std::map<sai_object_id_t, std::map<sai_stat_id_t, uint64_t>> m_routeCounterStatsCarryMap;
```

When a counter is bound to a route, the current VPP route stats are captured as the base and the carried delta is cleared. Normal reads return the carried delta plus the delta from that base. If a requested stat does not have a base entry yet, the read seeds the base from the current VPP value and returns only the carried value for that stat. Clear and read-and-clear modes update the base to the current VPP value and clear the carried delta for the requested stats.

The code also handles a possible VPP route stats reset by treating a large wrap/reset delta as zero and updating the base.

### Route updates

Route updates can reprogram the route in VPP and may produce a new stats index. Before deleting the old VPP route, the update path reads the current stats for any bound route counter and adds the visible delta into the carried delta. It then adds the new VPP route, refreshes the cached stats index, and refreshes the base for the new stats index.

Rollback is attempted if reprogramming or internal state update fails.

### Next-hop group member changes

Routes using next-hop groups can be affected when a next-hop group member is added or removed. For impacted routes with a bound counter, the VPP SAI layer reads the current stats before updating the path, carries the visible delta forward, refreshes the route stats index after the path update, and resets the base to the current VPP stats for the new stats index.

NHG member changes are handled best-effort per impacted route. If one route fails to update or refresh its counter base, the code logs the failure and continues processing the remaining routes. When VPP has already accepted the path update but the new stats base cannot be read, the cached stats index remains on the new VPP index and the stale base is cleared, so later reads do not use a base from the old stats index.

### Counter and route removal

Counter unbind is an explicit reset boundary. When `SAI_ROUTE_ENTRY_ATTR_COUNTER_ID` is set to `SAI_NULL_OBJECT_ID`, the route-to-counter binding, base snapshot, and carried delta are removed. A later rebind starts from a fresh base snapshot instead of inheriting traffic from the previous binding lifecycle.

When a route is removed, the route counter binding and stats index entry are removed. When a counter is removed, any route binding for that counter is removed before deleting the counter state.

## Code References

The implementation is in PR: https://github.com/sonic-net/sonic-sairedis/pull/1894

| Area | Reference |
|------|-----------|
| Route add/delete stats index plumbing | [`SwitchVppRoute.cpp`](https://github.com/cyw233/sonic-sairedis/blob/vpp-route-flow-counter/vslib/vpp/SwitchVppRoute.cpp#L104-L230) |
| Route counter binding helpers | [`SwitchVppRoute.cpp`](https://github.com/cyw233/sonic-sairedis/blob/vpp-route-flow-counter/vslib/vpp/SwitchVppRoute.cpp#L232-L308) |
| Route create/update/remove counter handling | [`SwitchVppRoute.cpp`](https://github.com/cyw233/sonic-sairedis/blob/vpp-route-flow-counter/vslib/vpp/SwitchVppRoute.cpp#L370-L770) |
| Route-bound counter stats read path | [`SwitchVpp.cpp`](https://github.com/cyw233/sonic-sairedis/blob/vpp-route-flow-counter/vslib/vpp/SwitchVpp.cpp#L753-L875) |
| `getStatsExt()` route counter dispatch | [`SwitchVpp.cpp`](https://github.com/cyw233/sonic-sairedis/blob/vpp-route-flow-counter/vslib/vpp/SwitchVpp.cpp#L925-L957) |
| Route counter state maps and helper declarations | [`SwitchVpp.h`](https://github.com/cyw233/sonic-sairedis/blob/vpp-route-flow-counter/vslib/vpp/SwitchVpp.h#L92-L111), [`SwitchVpp.h`](https://github.com/cyw233/sonic-sairedis/blob/vpp-route-flow-counter/vslib/vpp/SwitchVpp.h#L659-L707) |
| NHG member stats index refresh | [`SwitchVppNexthop.cpp`](https://github.com/cyw233/sonic-sairedis/blob/vpp-route-flow-counter/vslib/vpp/SwitchVppNexthop.cpp#L318-L454) |
| VPP route stats index capture from API reply | [`SaiVppXlate.c`](https://github.com/cyw233/sonic-sairedis/blob/vpp-route-flow-counter/vslib/vpp/vppxlate/SaiVppXlate.c#L853-L870), [`SaiVppXlate.c`](https://github.com/cyw233/sonic-sairedis/blob/vpp-route-flow-counter/vslib/vpp/vppxlate/SaiVppXlate.c#L2036-L2167) |
| VPP route stats query helper | [`SaiRouteStats.c`](https://github.com/cyw233/sonic-sairedis/blob/vpp-route-flow-counter/vslib/vpp/vppxlate/SaiRouteStats.c), [`SaiRouteStats.h`](https://github.com/cyw233/sonic-sairedis/blob/vpp-route-flow-counter/vslib/vpp/vppxlate/SaiRouteStats.h) |
| VPP build integration | [`Makefile.am`](https://github.com/cyw233/sonic-sairedis/blob/vpp-route-flow-counter/vslib/Makefile.am#L76-L98) |

Related test gap fix:

- [sonic-net/sonic-mgmt#24553](https://github.com/sonic-net/sonic-mgmt/pull/24553)

## Expected Behavior

With the updated `docker-syncd-vpp.gz`:

1. `counterpoll flowcnt-route enable` enables route flow counter polling.
2. `config flowcnt-route pattern add ...` causes matched routes to receive counters.
3. Static routes matching the configured pattern get route flow counters.
4. Traffic sent to those routes increments VPP route stats.
5. `show flowcnt-route stats` reports increasing packet and byte counters.
6. Pattern updates and max-match-count updates continue to create and remove route counters as expected.

This should allow the updated `tests/route/test_route_flow_counter.py` test from sonic-mgmt PR 24553 to pass on the VPP platform, assuming the new syncd-vpp container is deployed and traffic reaches the matched VPP routes.

## Risks and Open Questions

| Area | Risk or Question |
|------|------------------|
| VPP stats index lifecycle | Need to ensure cached stats indexes remain valid after all route reprogramming paths. |
| Multipath routes | Need review of VPP stats index behavior for ECMP and NHG-backed routes. |
| Counter base reset | Incorrect base refresh could hide real increments or produce unexpected deltas. |
| VPP stats source | Need confirmation that `/net/route/to` is the correct source for both IPv4 and IPv6 route counters. |
| Rollback path | Need review of partial failure handling when route reprogramming succeeds but state update fails. |
| Test traffic path | The sonic-mgmt traffic-based test depends on traffic actually hitting the matched route in VPP. |

## Review Focus

Please review:

1. Whether the stats index lifecycle is correct for route create, update, delete, and NHG member changes.
2. Whether `SAI_ROUTE_ENTRY_ATTR_COUNTER_ID` behavior matches SAI expectations.
3. Whether one counter should be rejected when already bound to a different route.
4. Whether base snapshot logic is the right way to implement clear and read-and-clear for VPP route stats.
5. Whether the selected SAI status codes are appropriate for unsupported stats index cases and VPP stats read failures.
6. Whether additional tests are needed for route update, counter unbind, counter remove, and NHG route cases.
