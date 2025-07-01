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

UIO_DRV=${UIO_DRV:=uio_pci_generic}

function bind_ports_to_uio()
{
    portlist=$1

    if [ "$UIO_DRV" == "vfio-pci" ]; then
	modprobe vfio_pci
    else
	modprobe $UIO_DRV
    fi

    for port in ${portlist[@]};
    do
	echo "Binding port $port "
	PCI_ID=$(ethtool -i $port | grep bus-info | awk '{ print $2}')

	if eval echo "$port" | grep "[0-9a-fA-F]\+:[0-9a-fA-F]\+:[0-9a-fA-F]\+\.[0-9a-fA-F]\+" > /dev/null; then
	    PCI_ID=$port
	elif [ "$PCI_ID" == "" ]; then
	    continue
	fi
	CUR_DRIVER=$(lspci -k -s $PCI_ID | grep "Kernel driver in use:" | cut -d ':' -f2 | xargs)
	# Only skip mlx5_core devices as they need their native driver
	if [ "$CUR_DRIVER" == "mlx5_core" ]; then
			echo "Skipping device with mlx5_core driver: $pci_dev"
			continue
	fi

	if [ "$CUR_DRIVER" != "" ] && [ "$CUR_DRIVER" != "$UIO_DRV" ]; then
	    echo "Un-binding port $port($PCI_ID) from driver $CUR_DRIVER"
	    echo "$PCI_ID" > /sys/bus/pci/drivers/$CUR_DRIVER/unbind

		# Wait until the device is unbound
	    max_attempts=50  # Maximum number of attempts
	    attempt=0        # Current attempt count
	    while [ -e "/sys/bus/pci/drivers/$CUR_DRIVER/$PCI_ID" ]; do
	        sleep 0.1  # Check every 100ms
	        attempt=$((attempt + 1))
	        if [ "$attempt" -ge "$max_attempts" ]; then
	            echo "Timeout: Device unbind operation took too long"
	        fi
	    done
	fi
	echo "Binding port $port($PCI_ID) to driver $UIO_DRV"
	NEWID=$(lspci -s $PCI_ID -n | awk '{ print $3}' | awk -F":" '{ printf "%s %s", $1, $2}')
	echo "$NEWID" > /sys/bus/pci/drivers/$UIO_DRV/new_id
	echo "$PCI_ID" > /sys/bus/pci/drivers/$UIO_DRV/bind
    done
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
		echo "$port" > /sys/bus/pci/drivers/$CUR_DRIVER/unbind || true
	    fi

	    # unbind , maybe, will automatically trigger loading the default kernel driver
	    echo "Binding port $port to driver $DRIVER"
	    NEWID=$(lspci -s $port -n | awk '{ print $3}' | awk -F":" '{ printf "%s %s", $1, $2}')
	    echo "$NEWID" > /sys/bus/pci/drivers/$DRIVER/new_id || true
	    echo "$port" > /sys/bus/pci/drivers/$DRIVER/bind || true
	fi
    done
}

VPP_ROOT_CFG=/etc/sonic/vpp
VPP_ENV_FILE=$VPP_ROOT_CFG/syncd_vpp_env

source $VPP_ENV_FILE

IFS=","
read -a portlist <<< "$VPP_DPDK_PORTS"
unset IFS

cmd=$1
case "$cmd" in
    "start")
	bind_ports_to_uio $portlist
	;;
    "stop")
	restore_dpdk_ports $portlist
	;;
    *)
	echo "Invalid command $cmd"
	exit 255
	;;
esac

exit 0
