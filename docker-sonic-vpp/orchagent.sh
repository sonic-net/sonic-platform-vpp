#!/usr/bin/env bash

#This is required since we have platform based checks in orchagent

if [ "$HWSKU" == "Mellanox-SN2700" ]; then
    export platform="mellanox"
else
    export platform=vs
fi

SWSS_VARS_FILE=/usr/share/sonic/templates/swss_vars.j2

# Retrieve SWSS vars from sonic-cfggen
SWSS_VARS=$(sonic-cfggen -d -y /etc/sonic/sonic_version.yml -t $SWSS_VARS_FILE) || exit 1

MAC_ADDRESS=$(echo $SWSS_VARS | jq -r '.mac')
if [ "$MAC_ADDRESS" == "None" ] || [ -z "$MAC_ADDRESS" ]; then
    MAC_ADDRESS=$(ip link show eth0 | grep ether | awk '{print $2}')
    logger "Mac address not found in Device Metadata, Falling back to eth0"
fi

# Create a folder for SwSS record files
mkdir -p /var/log/swss
# P4Orch unconditionally binds a ZMQ server on ipc:///zmq_swss/... ; in the
# multi-container/VM image this dir is a host bind-mount, but the single
# container must create it itself or orchagent aborts (zmq_bind ENOENT).
mkdir -p /zmq_swss
ORCHAGENT_ARGS="-d /var/log/swss "

# Set orchagent pop batch size to 8192
ORCHAGENT_ARGS+="-b 8192 "

# Set synchronous mode if it is enabled in CONFIG_DB
SYNC_MODE=$(echo $SWSS_VARS | jq -r '.synchronous_mode')
if [ "$SYNC_MODE" == "enable" ]; then
    ORCHAGENT_ARGS+="-s "
fi

# Pass FIB suppression flag when enabled in CONFIG_DB
SUPPRESS_FIB_CONFIG=$(sonic-cfggen -d -v "DEVICE_METADATA['localhost'].get('suppress-fib-pending', '')")
if [ "$SUPPRESS_FIB_CONFIG" == "enabled" ]; then
    ORCHAGENT_ARGS+="-F "
fi

# Set mac address
ORCHAGENT_ARGS+="-m $MAC_ADDRESS"

exec /usr/bin/orchagent ${ORCHAGENT_ARGS}
