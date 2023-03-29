#!/bin/bash -x
# Copyright (c) 2023 Cisco and/or its affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at:
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

function error()
{
    echo "Error: $*" 1>&2
    exit 3
}

#if [ $# -lt 1 ]; then
#    error "Usage: $0 num_interfaces"
#fi

function veth_intfs_up
{
    SERVERS=$1

    for srv in `seq 0 $((SERVERS-1))`; do
	ip link set up dev eth$((srv+1))
    done
}

function init_veth_intfs
{
    SERVERS=$1
    SWNAME="cntr"
    for srv in `seq 0 $((SERVERS-1))`; do
	SRV="$SWNAME-srv$srv"
	ip netns add $SRV
	NSS="ip netns exec $SRV"
	#NSE="nsenter -t $pid -n"

	$NSS ip addr add 127.0.0.1/8 dev lo
	$NSS ip addr add ::1/128 dev lo
	$NSS ip link set dev lo up

	# add virtual link between neighbor and the virtual switch docker

	IF="eth$((srv+1))"

	ip link add ${SRV}eth0 type veth peer name $SWNAME-$IF
	ip link set ${SRV}eth0 netns $SRV
	ip link set $SWNAME-$IF netns ${pid}
	$NSE ip link set dev $SWNAME-$IF name $IF

	echo "Bring ${SRV}eth0 up"
	$NSS ip link set dev ${SRV}eth0 name eth0
	$NSS ip link set dev eth0 up

	echo "Bring $IF up in the virtual switch docker"
	$NSE ip link set dev $IF up

    done
}

function init_veth_intfs_dummy
{
    SERVERS=$1
    for srv in `seq 0 $((SERVERS-1))`; do

	IF="eth$((srv+1))"

	ip link add ${IF} type dummy

	echo "Bring $IF up in the virtual switch docker"
	ip link set dev ${IF} up

    done
}

function create_if_mapping()
{
    IFS=","
    read -a portlist <<< """$VPP_DPDK_PORTS"""
    unset IFS

    PATH=$PATH:/usr/sbin
    DPDK_IFNAME="bobm"
    SONIC_VPP_IFMAP=/usr/share/sonic/hwsku/sonic_vpp_ifmap.ini
    local IDX=0
    local DPDK_IDX=0
    for port in ${portlist[@]};
    do
	if eval ip link show type veth dev $port >& /dev/null; then
	    port_name="host-$port"
	else
	    port_name="$DPDK_IFNAME$DPDK_IDX"
	    DPDK_IDX=$((DPDK_IDX + 1))
	fi
	echo "Ethernet$IDX $port_name" >> $SONIC_VPP_IFMAP
	IDX=$((IDX + 1))
    done

}

NUM_INTFS=$SONIC_NUM_PORTS

[ $NUM_INTFS -ge 1 ] || error "Invalid number of ports $NUM_INTFS specified"

ln -sf /usr/share/sonic/device/$PLATFORM /usr/share/sonic/platform
ln -sf /usr/share/sonic/device/$PLATFORM/$HWSKU /usr/share/sonic/hwsku

DEL_INTF=$NUM_INTFS
PLATFORM_JSON="/usr/share/sonic/platform/platform.json"
HWSKU_JSON="/usr/share/sonic/hwsku/hwsku.json"
CONFIG_JSON="/etc/sonic/config_db.json"
INITCFG_JSON="/etc/sonic/init_cfg.json"

#PLATFORM_JSON="./platform.json"
#HWSKU_JSON="./hwsku.json"

MATCH_INTF=$(for idx in `seq ${DEL_INTF} 1 31 `; do jq -e ".interfaces.Ethernet${idx}" ${PLATFORM_JSON} >& /dev/null && echo found ;done)

if [ "${MATCH_INTF}" != "" ]; then
    # init_veth_intfs ${NUM_INTFS}
    
    INTF_FILTER=$(SEP=""; for idx in `seq ${DEL_INTF} 1 31 `; do echo -n "${SEP}del(.interfaces.Ethernet${idx})"; SEP="|";done)

    cp ${PLATFORM_JSON} ${PLATFORM_JSON}.orig || error "Error copying ${PLATFORM_JSON} file"
    cp ${HWSKU_JSON} ${HWSKU_JSON}.orig || error "Error copying ${HWSKU_JSON} file"

    JSONTMP=$(tempfile -p sonic)

    jq "${INTF_FILTER}" ${PLATFORM_JSON} > ${JSONTMP} || error "jq failed to modify ${PLATFORM_JSON}"
    mv ${JSONTMP} ${PLATFORM_JSON}

    jq "${INTF_FILTER}" ${HWSKU_JSON} > ${JSONTMP} || error "jq failed to modify ${HWSKU_JSON}"
    mv ${JSONTMP} ${HWSKU_JSON}

#    jq '{"FLEX_COUNTER_TABLE": {"PORT": {"FLEX_COUNTER_STATUS": "enable"}}} + .' ${INITCFG_JSON} > ${JSONTMP} || \
#	error "FLEX COUNTER jq failed to modify ${INITCFG_JSON}"
#    mv ${JSONTMP} ${INITCFG_JSON}

    rm -f ${JSONTMP}
    # Cleanup old config so that new config_db gets created
    rm -f ${CONFIG_JSON}

fi

# veth_intfs_up $NUM_INTFS
create_if_mapping

# Sonic start.sh will complain
rm -f /usr/share/sonic/platform
rm -f /usr/share/sonic/hwsku

exec /usr/local/bin/supervisord
