#!/usr/bin/python3
# -*- coding: utf-8 -*-
import json
import sys
import os
import tempfile

def load_config():
    cfg = {}
    vpp = {}
    conf_db_set = os.getenv('VPP_CONF_DB', '')
    if not conf_db_set or conf_db_set != 'y':
        sys.exit(0)

    portlist = os.getenv('VPP_DPDK_PORTS', '')
    vports = {}
    dpdk_ports = {}
    index=0
    for port in portlist.split(','):
        rc = os.system('ip link show type veth dev {}'.format(port))
        if rc == 0:
            vports[port] = ''
        else:
            dpdk_ports[port] = {'name': 'bobm{}'.format(index)}
            index += 1

    if len(vports):
        vpp['vports'] = vports

    if len(dpdk_ports):
        dpdk = { 'dev': dpdk_ports }
        vpp['dpdk'] = dpdk

    cfg['PLATFORM'] = { 'vpp': vpp }
    print(cfg)
    fd, tmpfile = tempfile.mkstemp(prefix='vppcfg')

    with open(tmpfile, 'w') as fp:
        json.dump(cfg, fp, indent=4)

    rc = os.system('cat {}'.format(tmpfile))
    rc += os.system('config load -y {}'.format(tmpfile))
    os.close(fd)
    os.unlink(tmpfile)

    return rc

if __name__ == '__main__':
   sys.exit(load_config())
