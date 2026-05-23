#!/bin/bash
# Copyright (c) 2026 Cisco and/or its affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at:
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# vpp_promisc.sh -- keep DPDK bobm* interfaces in promiscuous mode.
#
# VPP DPDK virtio ports default to "unicast off all-multicast on", which
# drops broadcast frames (e.g. ARP requests targeting newly-created L3
# sub-ports). Real ASICs are implicitly promiscuous for routed ports, so
# we mirror that behavior here.
#
# VPP's startup-config directive runs CLI commands before the DPDK
# plugin has finished initialising its ports, so "set interface
# promiscuous on bobm*" lines emitted into the startup config silently
# fail. This script polls until bobm* interfaces exist, then enables
# promisc; it keeps watching so the property is re-applied if VPP is
# restarted under supervisord.

set -u

# Optionally pull DPDK_DISABLE from the syncd env file so we can early-
# exit on DPDK-less configurations. docker-sonic-vpp may run without
# this file (env passed via "docker run -e ..."), so sourcing is best-
# effort.
VPP_ENV_FILE=/etc/sonic/vpp/syncd_vpp_env
[ -r "$VPP_ENV_FILE" ] && source "$VPP_ENV_FILE"
[ "${DPDK_DISABLE:-n}" == "y" ] && exit 0

WAIT_TIMEOUT=${VPP_PROMISC_WAIT:-180}
RECHECK_INTERVAL=${VPP_PROMISC_INTERVAL:-2}

# Phase 1: wait for VPP CLI to come up and at least one bobm* interface
# to appear. Fail loudly on timeout so supervisord surfaces the problem.
deadline=$((SECONDS + WAIT_TIMEOUT))
ready=0
while [ $SECONDS -lt $deadline ]; do
    if vppctl show interface 2>/dev/null \
        | awk '/^bobm[0-9]+ /{f=1} END{exit !f}'; then
        ready=1
        break
    fi
    sleep 1
done

if [ "$ready" != 1 ]; then
    # bobm* never appeared. Most likely DPDK is not configured on this
    # platform (DPDK_DISABLE may not have been set explicitly). Log once
    # and exit cleanly so supervisord does not enter a restart storm.
    # autorestart=unexpected (see vpp_promisc.conf) then leaves the
    # program in EXITED state instead of cycling.
    echo "vpp_promisc.sh: timed out after ${WAIT_TIMEOUT}s waiting for bobm* interfaces; exiting (no DPDK ports to manage)" >&2
    exit 0
fi

# Phase 2: keep promiscuous mode on for every discovered bobm* port.
# Re-apply if a new VPP instance comes up under the same supervisord.
while true; do
    intfs=$(vppctl show interface 2>/dev/null \
        | awk '/^bobm[0-9]+ /{print $1}')
    for intf in $intfs; do
        # Allow trivial whitespace and format variation across VPP
        # releases (e.g. extra spaces, key reordering). The previous
        # fixed-substring check was fragile against minor format drift.
        if vppctl show hardware-interfaces "$intf" 2>/dev/null \
            | grep -qE 'promiscuous:[[:space:]]+unicast[[:space:]]+on'; then
            continue
        fi
        if vppctl set interface promiscuous on "$intf" >/dev/null 2>&1; then
            echo "vpp_promisc.sh: enabled promisc on $intf"
        fi
    done
    sleep "$RECHECK_INTERVAL"
done
