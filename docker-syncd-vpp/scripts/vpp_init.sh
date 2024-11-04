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

VPP_ENV_FILE=/etc/sonic/vpp/syncd_vpp_env
source $VPP_ENV_FILE

function error()
{
    echo "Error : $*" 1>&2
    exit 255
}

function upd_startup()
{
    [ "$DPDK_DISABLE" != "y" ] && echo "$*" >> $TMP_FILE
}

if [ "$VPP_CONF_DB" == "y" ]; then
    /usr/bin/vpp -c /etc/sonic/vpp/startup.conf
    exit $?
fi

STARTUP_TMPL="/etc/vpp/startup.conf.tmpl"
HWIFNAME="bobm"

[ -f $STARTUP_TMPL ] || error "$STARTUP_TMPL file missing"

IFS=","
read -a portlist <<< """$VPP_DPDK_PORTS"""
unset IFS

[ ${#portlist[@]} -ge 1 ] || error "DPDK ports list empty"

TMP_FILE=$(tempfile -p vpp -s .conf)

STARTUP_CFG=$(tempfile -p vpp -s .cfg)

cp $STARTUP_TMPL $TMP_FILE || error "Failed to copy template file"

[ "$DPDK_DISABLE" == "y" ] && sed -i -e 's/plugin dpdk_plugin/#plugin dpdk_plugin/g' $TMP_FILE
[ "$NO_LINUX_NL" == "y" ] && sed -i -e 's/plugin linux_nl_plugin/#plugin linux_nl_plugin/g' $TMP_FILE

IDX=0
upd_startup "dpdk {"
for port in ${portlist[@]};
do
    if eval ip link show type veth dev $port >& /dev/null; then
	echo "create host-interface name $port" >> $STARTUP_CFG
	continue
    fi
    upd_startup """
	dev $port {
	    name ${HWIFNAME}${IDX}
	}
	"""
    IDX=$((IDX + 1))
done
upd_startup "}"

sed -i -e "s,VPP_STARTUP_CONFIG,$STARTUP_CFG,g" $TMP_FILE

PERPORT_BUF=${PERPORT_BUF:=2048}
TOTBUF=$((PERPORT_BUF * SONIC_NUM_PORTS))

echo "buffers {" >> $TMP_FILE
echo "    buffers-per-numa $TOTBUF" >> $TMP_FILE
[ "$DPDK_DISABLE" != "y" ] && echo "    page-size default-hugepage" >> $TMP_FILE
echo "}" >> $TMP_FILE

if [ "$DPDK_DISABLE" != "y" ]; then
    if [ $IDX -le 6 ]; then
	NHPG=12
    else
	NHPG=$((IDX * 2))
    fi
    echo $NHPG > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
fi

cat $TMP_FILE

/usr/bin/vpp -c ${TMP_FILE}

rm -f $TMP_FILE $STARTUP_CFG
