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

debug=1

function dbg()
{
    if [ ${debug} -eq 1 ]; then
	echo $* >> /tmp/vpp_cfg.log
    fi
}

function create_intf()
{
    ifname=$1
    hwif=$2

    dbg "Creating lcp interface ${ifname}"
    if ! eval vppctl show lcp phy ${hwif} | grep ${ifname} >& /dev/null; then
	# docker exec  ubuntu-vpp vppctl set state ${hwif} up || echo "Command failed with status $?"

	vppctl lcp create ${hwif} host-if ${ifname} || dbg "lcp create command failed with status $?"
	sleep 2
    else
	dbg "${hwif} already configured"
    fi
}

function set_state()
{
    ifname=$1
    state=$2

    dbg "Setting state of ${ifname} to ${state}"
    vppctl set int state host-${ifname} up
}

function set_addr_mask()
{
    ifname=$1
    addr=$2
    mask=$3
    del=$4

    dbg "Setting address ${addr}/${mask} to ${ifname}"
    vppctl set int ip address ${del} host-${ifname} ${addr}/${mask}
}

intfmap=(Ethernet0 Ethernet4 Ethernet8 Ethernet12 Ethernet16 Ethernet20 Ethernet24 Ethernet28)
hwifmap=(bobm0 bobm1 bobm2 bobm3 bobm4 bobm5 bobm6 bobm7)

function map_name_to_hwif()
{
    search=$1
    for idx in ${!intfmap[@]};
    do
	if [ "${intfmap[$idx]}" == "${search}" ]; then
	    echo -n "${hwifmap[$idx]}"
	fi
    done
}

function process_sonic_msg()
{
    cmd=$1
    type=$2
    msg_data=$3

    dbg "Processing ${cmd} ${type} ${msg_data}"
    if [ "${cmd}" == "create" ]; then
	if [ "${type}" == "interface" ]; then
	    read dev <<< """${msg_data}"""
	    hwdev=$(map_name_to_hwif "$dev")
	    dbg "Parsed data ${dev} hwif ${hwdev}"
	    [ "${hwdev}" != "" ] && create_intf "${dev}" "${hwdev}"
	fi
    elif [ "${cmd}" == "stats" ]; then
	if [ "${type}" == "interface" ]; then
	    read dev <<< """${msg_data}"""
	    hwdev=$(map_name_to_hwif "$dev")
	    dbg "stats msg Parsed data ${dev} hwif ${hwdev}"
	    {
		echo "start $dev"
		read pkt bytes <<< """$(vpp_get_stats dump bobm0 | grep "/interfaces/bobm0/rx$" | awk '{ printf "%s %s", $4, $6}')"""
		echo "rxp $pkt"
		echo "rxb $bytes"
		read pkt bytes <<< """$(vpp_get_stats dump bobm0 | grep "/interfaces/bobm0/tx$" | awk '{ printf "%s %s", $4, $6}')"""
		echo "txp $pkt"
		echo "txb $bytes"
		read pkt <<< """$(vpp_get_stats dump bobm0 | grep "/interfaces/bobm0/drops$" | awk '{ printf "%s", $4}')"""
		echo "rxd $pkt"
		echo "end $dev"
	    } > /tmp/vpp_to_sonic.pipe
	fi
    fi
}

while true
do
    echo "Listening for messages on fifo /tmp/vpp_sonic.pipe"
    while true
    do
	read cmd type sonic_msg
	echo $sonic_msg
	if [ "${sonic_msg}" == "" ]; then
	    dbg "EOF received"
	    break
	fi
	dbg "${cmd} ${type} Received msg '${sonic_msg}'"
	process_sonic_msg ${cmd} ${type} "${sonic_msg}"
    done < /tmp/vpp_sonic.pipe
done
