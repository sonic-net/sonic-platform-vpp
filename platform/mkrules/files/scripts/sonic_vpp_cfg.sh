#!/bin/bash

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

set -e

function error()
{
    echo "Error: $*" 1>&2
    exit 255
}

function usage()
{
    echo """
$0 -i [ -p -d ]
-i - Network Interfaces(ports) seperated by ,
-d - Dry run
-p - Don't ask while updating configuration
"""
}

VPP_ROOT_CFG=/etc/sonic/vpp
VPP_ENV_FILE=$VPP_ROOT_CFG/syncd_vpp_env
VPP_PLATFORM="x86_64-kvm_x86_64-r0"
VPP_HWSKU="$VPP_PLATFORM/Force10-S6000"
SONIC_VPP_IFMAP=/usr/share/sonic/device/$VPP_HWSKU/sonic_vpp_ifmap.ini

function setup_ports_list()
{
    portlist=$1
    netns=$2

    echo -n "Injecting Ports "
    for port in ${portlist[@]};
    do
	echo -n "$port "
	if ! eval echo "$port" | grep "[0-9a-fA-F]\+:[0-9a-fA-F]\+:[0-9a-fA-F]\+\.[0-9a-fA-F]\+" > /dev/null; then
	    if eval ip link show type veth dev $port | grep "$port" >& /dev/null; then
		PORTS_BUS="${PORTS_BUS}${SEP}${port}"
	    else
		PCI_ID=$(ethtool -i $port | grep bus-info | awk '{ print $2}')
		if [ "$PCI_ID" == "" ]; then
		    error "Interface $port does not exist"
		fi

		PORTS_BUS="${PORTS_BUS}${SEP}${PCI_ID}"
		DPDK_DISABLE="n"
	    fi
	else
	    PORTS_BUS="${PORTS_BUS}${SEP}${port}"
	    DPDK_DISABLE="n"
	fi
	SEP=","
    done
    echo "DPDK_DISABLE=$DPDK_DISABLE" >> $VPP_ENV_FILE
    echo "VPP_DPDK_PORTS=$PORTS_BUS" >> $VPP_ENV_FILE
    echo "SONIC_NUM_PORTS=${#portlist[@]}" >> $VPP_ENV_FILE

    echo ""
}

function create_if_mapping()
{
    IFS=","
    read -a portlist <<< "$1"
    unset IFS

    PATH=$PATH:/usr/sbin
    DPDK_IFNAME="bobm"

    cp /dev/null $SONIC_VPP_IFMAP

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
	echo "Ethernet$((IDX * LANES)) $port_name" >> $SONIC_VPP_IFMAP
	IDX=$((IDX + 1))
    done

}

PRIV=$(id -u)
if [ $PRIV -ne 0 ]; then
    echo "Run this script as root"
    error "operation not permitted"
fi
mkdir -p $VPP_ROOT_CFG
NETPORTS=""
DRY_RUN=n
DONT_ASK=n
LANES=${LANES:=1}

SONIC_CFG_FILE=/etc/sonic/config_db.json
SONIC_CFG_FILE_ORIG=/etc/sonic/config_db.json.orig
OUTPUT_FILE=$SONIC_CFG_FILE

while true
do
    case "$1" in
        "-i")
            shift 1
	    NETPORTS=$1
	    shift 1
            ;;
        "-v")
            shift 1
            VERBOSE=$1
            ;;
        "-d")
            shift 1
            DRY_RUN=y
	    VPP_ENV_FILE=/dev/stdout
	    OUTPUT_FILE=/dev/stdout
	    SONIC_VPP_IFMAP=/dev/stdout
            ;;

        "-p")
            shift 1
	    DONT_ASK=y
            ;;
        *)
            break
            ;;
    esac
done      

if [ "$NETPORTS" == "" ]; then
    echo "Please input the list of interfaces to argument -i"
    usage
fi

IFS=','

read -a portlist <<< $NETPORTS
unset IFS

if [ ! -s $SONIC_CFG_FILE_ORIG ]; then
    cp $SONIC_CFG_FILE $SONIC_CFG_FILE_ORIG || error "SONiC config file '$SONIC_CFG_FILE' missing"
fi

[ x$DRY_RUN != "xy" ] && cp $SONIC_CFG_FILE ${SONIC_CFG_FILE}.orig.$(date +"%s")

TMPCFG1=$(mktemp)
TMPCFG2=$(mktemp)
jq 'del(.INTERFACE)|del(.LOOPBACK_INTERFACE)|del(.DEVICE_NEIGHBOR)|del(.BGP_NEIGHBOR)|del(.PORT)' $SONIC_CFG_FILE_ORIG > $TMPCFG1

NUM_INTFS=${#portlist[@]}
for i in `seq 0 1 $((NUM_INTFS - 1))`
do
	ETHVAL=$(jq ".PORT.Ethernet$((i * 4))" $SONIC_CFG_FILE_ORIG)
	jq ".PORT |= . + { \"Ethernet$((i * LANES))\": $ETHVAL }" $TMPCFG1 > $TMPCFG2
	cp $TMPCFG2 $TMPCFG1
done

echo "Writing syncd_vpp_env contents"
echo "VPP_PORT_LIST=$NETPORTS" > $VPP_ENV_FILE

if [ "$NO_LINUX_NL" == "y" ]; then
    echo "NO_LINUX_NL=y" >> $VPP_ENV_FILE
fi

if [ "$VPP_CONF_DB" == "y" ]; then
    echo "VPP_CONF_DB=y" >> $VPP_ENV_FILE
fi

if [ "x$PERPORT_BUF" != "x" ]; then
    echo "PERPORT_BUF=$PERPORT_BUF" >> $VPP_ENV_FILE
fi

if [ "x$SONIC_VPP_SWITCH" != "x" ]; then
    echo "SONIC_VPP_SWITCH=$SONIC_VPP_SWITCH" >> $VPP_ENV_FILE
fi

setup_ports_list $portlist

echo "Creating vpp to sonic interface mapping file"

create_if_mapping $PORTS_BUS

rm -f $TMPCFG2
if [ x$DRY_RUN != "xy" ] && [ x$DONT_ASK != "xy" ]; then
    echo -n "Continue with updating the $SONIC_CFG_FILE ? (y/n) "
    read input
    [ x"$input" != "xy" ] && exit 0
fi
echo "Updating $SONIC_CFG_FILE"

systemctl stop syncd || true
docker rm syncd || true

cp $TMPCFG1 $OUTPUT_FILE

if [ x$DRY_RUN != "xy" ] && [ x$DONT_ASK != "xy" ]; then
    echo -n "Continue with rebooting the system (configuration changes are effective only after reboot) ? (y/n) "
    read input
    [ x"$input" != "xy" ] && exit 0
fi
echo "Rebooting ... "

/sbin/shutdown -r now

