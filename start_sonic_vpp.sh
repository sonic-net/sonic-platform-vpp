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

#set -x

function error()
{
    echo "Error: $*" 1>&2
    exit 255
}

function usage()
{
    echo """
    Usage: $1 command -n net_ports [-v]
    command - start or stop or status
    -i netports - comma separate listed of network ports (virtual interfaces, physical network interfaces or PCI bus address of a network inteface)
    (Use ethtool -i port_name | grep bus-info | awk '{ print $2}' command to find out PCI address of a network interface)
    -n container_name
    -v - verbose
    """
    exit 3
}

function cntr_running()
{
    if [ "$(docker ps --filter="name=$1$" -q)" == "" ]; then
	return 1
    else
	return 0
    fi
}

function create_netns()
{
    netns=$1

    if ! eval cntr_running $netns; then
	docker run -d --rm --name $netns $SONIC_NSS_IMG > /dev/null || error "Failed to create netnss container"
	return $?
    fi
    return 1
}

function start_sonic_vpp_cntr()
{
    netns="container:$1"
    PORTS="$(cat $NETPORTS_PATH)"
    read -a portlist < $NETPORTS_PATH

    if ! eval cntr_running $SONIC_VPP; then
	echo "Starting $SONIC_VPP container"
	docker run --rm --init --privileged -e "VPP_DPDK_PORTS=$PORTS" -e "SONIC_NUM_PORTS=${#portlist[@]}" \
	       -e "DPDK_DISABLE=$DPDK_DISABLE" -e "NO_LINUX_NL=$NO_LINUX_NL" --network $netns --name $SONIC_VPP -d $SONIC_VPP_IMG > /dev/null
#	docker create --rm --init --privileged -e "VPP_DPDK_PORTS=$PORTS" -e "SONIC_NUM_PORTS=${#portlist[@]}" \
#		-e "DPDK_DISABLE=$DPDK_DISABLE" --network $netns --name $SONIC_VPP $SONIC_VPP_IMG
    fi
}

function restore_dpdk_ports()
{
    portlist=$1
    for port in ${portlist[@]};
    do
	if eval echo "$port" | grep "[0-9a-fA-F]\+:[0-9a-fA-F]\+:[0-9a-fA-F]\+\.[0-9a-fA-F]\+" > /dev/null; then
	    CUR_DRIVER=$(lspci -k -s $port | grep "Kernel driver in use:" | cut -d ':' -f2 | xargs)
	    DRIVER=$(lspci -k -s $port | grep "Kernel modules:" | cut -d ':' -f2 | xargs)
	    
	    if [ "$CUR_DRIVER" != "" ] && [ "$CUR_DRIVER" != "$DRIVER" ]; then
		echo "Un-binding port $port from driver $CUR_DRIVER"
		echo "$port" > /sys/bus/pci/drivers/$CUR_DRIVER/unbind
	    fi

	    echo "Binding port $port to driver $DRIVER"
	    echo "$port" > /sys/bus/pci/drivers/$DRIVER/bind
	fi
    done
}

function pci_bind_unbind()
{
    PCI_ID=$1

    CUR_DRIVER=$(lspci -k -s $PCI_ID | grep "Kernel driver in use:" | cut -d ':' -f2 | xargs)
    if [ "$CUR_DRIVER" != "" ] && [ "$CUR_DRIVER" != "$UIO_DRV" ]; then
	echo "Un-binding port $port($PCI_ID) from driver $CUR_DRIVER"
	echo "$PCI_ID" > /sys/bus/pci/drivers/$CUR_DRIVER/unbind
    fi
    echo "Binding port $port($PCI_ID) to driver $UIO_DRV"
    NEWID=$(lspci -s $PCI_ID -n | awk '{ print $3}' | awk -F":" '{ printf "%s %s", $1, $2}')
    echo "$NEWID" > /sys/bus/pci/drivers/$UIO_DRV/new_id
    echo "$PCI_ID" > /sys/bus/pci/drivers/$UIO_DRV/bind
}

function inject_ports_in_cntr()
{
    portlist=$1
    netns=$2

    if [ "$UIO_DRV" == "vfio-pci" ]; then
	modprobe vfio_pci
    else
	modprobe $UIO_DRV
    fi
    echo "Injecting Ports "
    for port in ${portlist[@]};
    do
	if ! eval echo "$port" | grep "[0-9a-fA-F]\+:[0-9a-fA-F]\+:[0-9a-fA-F]\+\.[0-9a-fA-F]\+" > /dev/null; then
	    if eval ip link show type veth dev $port | grep "$port" >& /dev/null; then
		ip link set netns $netns dev $port
		PORTS_BUS="${PORTS_BUS}${SEP}${port}"
	    else
		PCI_ID=$(ethtool -i $port | grep bus-info | awk '{ print $2}')
		if [ "$PCI_ID" == "" ]; then
		    error "Interface $port does not exist"
		fi
		pci_bind_unbind $PCI_ID
		PORTS_BUS="${PORTS_BUS}${SEP}${PCI_ID}"
		DPDK_DISABLE="n"
	    fi
	else
	    pci_bind_unbind $port
	    PORTS_BUS="${PORTS_BUS}${SEP}${port}"
	    DPDK_DISABLE="n"
	fi
	SEP=","
    done
    echo -n "$PORTS_BUS" > $NETPORTS_PATH
    echo ""
}

PRIV=$(id -u)
if [ $PRIV -ne 0 ]; then
    echo "Run this script as root"
    error "operation not permitted"
fi

SONIC_NSS_IMG=${SONIC_NSS_IMG:="k8s.gcr.io/pause"}
SONIC_VPP_IMG=${SONIC_VPP_IMG:="docker-sonic-vpp:latest"}
SONIC_VPP=""

CMD=$1
shift 1

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
        "-n")
            shift 1
            SONIC_VPP=$1
            shift 1
            ;;

        "-t")
            shift 1
            TICKET_PATH=$1
            shift 1
            ;;
        *)
            break
            ;;
    esac
done      

if [ "$SONIC_VPP" == "" ]; then
    echo "Please specify a name for the sonic instance"
    usage
fi

SONIC_NSS="$SONIC_VPP-nss"
NETPORTS_PATH="/run/sonic-vpp/$SONIC_VPP-netports_bus"
DPDK_DISABLE="y"

UIO_DRV=${UIO_DRV:=vfio-pci}
NO_LINUX_NL=${NO_LINUX_NL:="n"}

if [ "$CMD" == "start" ]; then
    if [ "$NETPORTS" == "" ]; then
	echo "Please input the list of interfaces to argument -i"
	usage
    fi
    if eval cntr_running "$SONIC_VPP"; then
	echo "A container is already running with the name $SONIC_VPP"
	exit 0
    fi

    IFS=','

    read -a portlist <<< $NETPORTS

    if eval create_netns $SONIC_NSS; then
	mkdir -p /run/sonic-vpp/

	echo -n "$NETPORTS" > /run/sonic-vpp/$SONIC_VPP-netports_name

	inject_ports_in_cntr ${portlist} "$(docker inspect --format '{{.State.Pid}}' $SONIC_NSS)"
    else
	echo "Error creating network namespace container"
	exit 3
    fi

    start_sonic_vpp_cntr "$SONIC_NSS"

elif [ "$CMD" == "stop" ]; then
    if ! eval cntr_running "$SONIC_VPP"; then
	echo "No container running with name $SONIC_VPP"
	exit 0
    fi
    docker exec -it $SONIC_VPP supervisorctl stop all >& /dev/null
    docker stop $SONIC_VPP
    docker stop $SONIC_NSS
    IFS=","
    read -a portlist < $NETPORTS_PATH > /dev/null
    restore_dpdk_ports ${portlist}
    echo "Container $SONIC_VPP stopped"
elif [ "$CMD" == "status" ]; then
    if ! eval cntr_running "$SONIC_VPP"; then
	echo "No container running with name $SONIC_VPP"
	exit 0
    fi
    echo "Container $SONIC_VPP is running (Network interfaces $(cat /run/sonic-vpp/$SONIC_VPP-netports_name))"
else
    echo "Invalid command"
    usage
fi
