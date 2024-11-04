#!/bin/bash -e

# Generate configuration

# NOTE: 'PLATFORM' and 'HWSKU' environment variables are set
# in the Dockerfile so that they persist for the life of the container

ln -sf /usr/share/sonic/device/$PLATFORM /usr/share/sonic/platform
ln -sf /usr/share/sonic/device/$PLATFORM/$HWSKU /usr/share/sonic/hwsku

pushd /usr/share/sonic/hwsku

# filter available front panel ports in lanemap.ini
[ -f lanemap.ini.orig ] || cp lanemap.ini lanemap.ini.orig
for p in $(ip link show | grep -oE "eth[0-9]+" | grep -v eth0); do
    grep ^$p: lanemap.ini.orig
done > lanemap.ini

# filter available sonic front panel ports in port_config.ini
[ -f port_config.ini.orig ] || cp port_config.ini port_config.ini.orig
grep ^# port_config.ini.orig > port_config.ini
for lanes in $(awk -F ':' '{print $2}' lanemap.ini); do
    grep -E "\s$lanes\s" port_config.ini.orig
done >> port_config.ini

popd

[ -d /etc/sonic ] || mkdir -p /etc/sonic

# Note: libswsscommon requires a dabase_config file in /var/run/redis/sonic-db/
# Prepare this file before any dependent application, such as sonic-cfggen
mkdir -p /var/run/redis/sonic-db
cp /etc/default/sonic-db/database_config.json /var/run/redis/sonic-db/

SYSTEM_MAC_ADDRESS=$(ip link show eth0 | grep ether | awk '{print $2}')
sonic-cfggen -t /usr/share/sonic/templates/init_cfg.json.j2 -a "{\"system_mac\": \"$SYSTEM_MAC_ADDRESS\"}" > /etc/sonic/init_cfg.json

if [[ -f /usr/share/sonic/virtual_chassis/default_config.json ]]; then
    sonic-cfggen -j /etc/sonic/init_cfg.json -j /usr/share/sonic/virtual_chassis/default_config.json --print-data > /tmp/init_cfg.json
    mv /tmp/init_cfg.json /etc/sonic/init_cfg.json
fi

if [ -f /etc/sonic/config_db.json ]; then
    sonic-cfggen -j /etc/sonic/init_cfg.json -j /etc/sonic/config_db.json --print-data > /tmp/config_db.json
    mv /tmp/config_db.json /etc/sonic/config_db.json
else
    # generate and merge buffers configuration into config file
    sonic-cfggen -k $HWSKU -p /usr/share/sonic/device/$PLATFORM/platform.json -t /usr/share/sonic/hwsku/buffers.json.j2 > /tmp/buffers.json
    sonic-cfggen -j /etc/sonic/init_cfg.json -t /usr/share/sonic/hwsku/qos.json.j2 > /tmp/qos.json
    sonic-cfggen -p /usr/share/sonic/device/$PLATFORM/platform.json -k $HWSKU --print-data > /tmp/ports.json
    # change admin_status from up to down; Test cases dependent
    sed -i "s/up/down/g" /tmp/ports.json
    sonic-cfggen -j /etc/sonic/init_cfg.json -j /tmp/buffers.json -j /tmp/qos.json -j /tmp/ports.json --print-data > /etc/sonic/config_db.json
fi
sonic-cfggen -t /usr/share/sonic/templates/copp_cfg.j2 > /etc/sonic/copp_cfg.json

if [ "$HWSKU" == "Mellanox-SN2700" ]; then
    cp /usr/share/sonic/hwsku/sai_mlnx.profile /usr/share/sonic/hwsku/sai.profile
fi

mkdir -p /etc/swss/config.d/

rm -f /var/run/rsyslogd.pid

supervisorctl start rsyslogd

supervisord_cfg="/etc/supervisor/conf.d/supervisord.conf"
chassisdb_cfg_file="/usr/share/sonic/virtual_chassis/default_config.json"
chassisdb_cfg_file_default="/etc/default/sonic-db/default_chassis_cfg.json"
host_template="/usr/share/sonic/templates/hostname.j2"
db_cfg_file="/var/run/redis/sonic-db/database_config.json"
db_cfg_file_tmp="/var/run/redis/sonic-db/database_config.json.tmp"

if [ -r "$chassisdb_cfg_file" ]; then
   echo $(sonic-cfggen -j $chassisdb_cfg_file -t $host_template) >> /etc/hosts
else
   chassisdb_cfg_file="$chassisdb_cfg_file_default"
   echo "10.8.1.200 redis_chassis.server" >> /etc/hosts
fi

supervisorctl start redis-server

start_chassis_db=`sonic-cfggen -v DEVICE_METADATA.localhost.start_chassis_db -y $chassisdb_cfg_file`
if [[ "$HOSTNAME" == *"supervisor"* ]] || [ "$start_chassis_db" == "1" ]; then
   supervisorctl start redis-chassis
fi

conn_chassis_db=`sonic-cfggen -v DEVICE_METADATA.localhost.connect_to_chassis_db -y $chassisdb_cfg_file`
if [ "$start_chassis_db" != "1" ] && [ "$conn_chassis_db" != "1" ]; then
   cp $db_cfg_file $db_cfg_file_tmp
   update_chassisdb_config -j $db_cfg_file_tmp -d
   cp $db_cfg_file_tmp $db_cfg_file
fi

if [ "$conn_chassis_db" == "1" ]; then
   if [ -f /usr/share/sonic/virtual_chassis/coreportindexmap.ini ]; then
      cp /usr/share/sonic/virtual_chassis/coreportindexmap.ini /usr/share/sonic/hwsku/

      pushd /usr/share/sonic/hwsku

      # filter available front panel ports in coreportindexmap.ini
      [ -f coreportindexmap.ini.orig ] || cp coreportindexmap.ini coreportindexmap.ini.orig
      for p in $(ip link show | grep -oE "eth[0-9]+" | grep -v eth0); do
          grep ^$p: coreportindexmap.ini.orig
      done > coreportindexmap.ini

      popd
   fi
fi

/usr/bin/configdb-load.sh

if [ "$HWSKU" = "brcm_gearbox_vs" ]; then
    supervisorctl start gbsyncd
    supervisorctl start gearsyncd
fi

# Create vpp port configuration and load it in config_db
/usr/local/bin/vppcfg_load.py

supervisorctl start vppcfgd

supervisorctl start syncd

supervisorctl start portsyncd

supervisorctl start orchagent

supervisorctl start coppmgrd

supervisorctl start neighsyncd

supervisorctl start fdbsyncd

supervisorctl start teamsyncd

supervisorctl start fpmsyncd

supervisorctl start teammgrd

supervisorctl start vrfmgrd

supervisorctl start portmgrd

supervisorctl start intfmgrd

supervisorctl start vlanmgrd

if [ ! -f /etc/frr/bgpd.conf ]; then
    CFGGEN_PARAMS=" \
        -d \
        -y /etc/sonic/constants.yml \
        -t /usr/share/sonic/templates/bgpd/gen_bgpd.conf.j2,/etc/frr/bgpd.conf \
        -t /usr/share/sonic/templates/zebra/zebra.conf.j2,/etc/frr/zebra.conf \
        -t /usr/share/sonic/templates/staticd/gen_staticd.conf.j2,/etc/frr/staticd.conf \
    "
    while [ ! -f /etc/frr/bgpd.conf ]
    do
        sonic-cfggen $CFGGEN_PARAMS || true
        sleep 1
    done 
    echo "no service integrated-vtysh-config" > /etc/frr/vtysh.conf
    rm -f /etc/frr/frr.conf
    chown -R frr:frr /etc/frr/
fi

supervisorctl start zebra

supervisorctl start bgpd

supervisorctl start bgpcfgd

supervisorctl start bgpmon

supervisorctl start staticd

supervisorctl start buffermgrd

supervisorctl start nbrmgrd

supervisorctl start vxlanmgrd

supervisorctl start sflowmgrd

supervisorctl start natmgrd

supervisorctl start natsyncd

supervisorctl start tunnelmgrd

# Start arp_update when VLAN exists
VLAN=`sonic-cfggen -d -v 'VLAN.keys() | join(" ") if VLAN'`
if [ "$VLAN" != "" ]; then
    supervisorctl start arp_update
fi
