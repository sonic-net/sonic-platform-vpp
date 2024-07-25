# SONIC VPP VxLAN VNET High Level Design
Rev v0.1

<br/>
<br/>

## Table of Contents

1. [Revisions](#item-1)
2. [Scope](#item-2)
3. [Definitions/Abbreviations](#item-3)
4. [Introduction](#item-4)
5. [VNET Peering Example](#item-5)
6. [Baremetal to VM](#item-6)
7. [VM to Baremetal](#item-7)
<br/>
<br/>

<a id="item-1"></a>
## Revisions

| Rev | Date | Author(s) |
|-----|------|-----------|
|v0.1 | 08/07/2024 | Yue Gao (Cisco) |


<br/>
<br/>

<a id="item-2"></a>
## Scope
This document describes the high level design of implementing VxLAN tunnel to support VNET peering between VMs and baremetal servers.

<br/>

<a id="item-3"></a>
## Definitions/Abbreviations
###### Table 1: Abbreviations
|                          |                                |
|--------------------------|--------------------------------|
| VNI                      | Vxlan Network Identifier       |
| VTEP                     | Vxlan Tunnel End Point         |
| VM                       | Virtual Machine                |
| VRF                      | Virtual Routing and Forwarding |
| VNet                     | Virtual Network                |
| VPP                      | Vector Packet Processing       |

<br/>
<br/>

<a id="item-4"></a>
## Introduction
SONiC VxLAN HLD (https://github.com/sonic-net/SONiC/blob/master/doc/vxlan/Vxlan_hld.md) describes using VxLAN tunnel to support 
VNET peering between VMs and baremetal servers. Traffic from baremeta server is routed to VxLAN tunnel through vnet tunnel route.
VxLAN traffic from VM to baremeta server will be decapsulated first then map to a VR from VNI. DST IP lookup within the VR will
router the traffic to the baremetal server.

VPP supports bidirectional VxLAN tunnel. We need to build the forwarding chain properly to support above VNET peering using VxLAN
tunnel.

<a id="item-5"></a>
## VNET Peering Example
Below is one simple example to show VM and Baremetal server VNET peering.
### Topology
	Vnet 1 
		□ VNI - 2000
		□ VMs
			VM1. CA: 100.100.1.1/32, PA: 10.10.10.1, MAC: 00:00:00:00:01:02
		□ BM1 
			Connected on Ethernet1 
			Ip: 100.100.3.2/24
			MAC: 00:00:AA:AA:AA:01
Below diagram shows the connectivity between VM1 and BM1. VxLAN tunnel will be created between SONiC and VM host.
```
                                                                         +-----------------+
    +-------+                                  +-------------+           |                 |
    |       | 100.100.3.2/24    100.100.3.1/24 |             |           | +-------------+ |
    |  BM1  +----------------------------------+    SONiC    +----...----+ |    VM1      | | 
    |       |                                  | 10.10.10.10 |           | | 100.100.1.1 | |
    +-------+                                  +-------------+           | +-------------+ |
                                                                         | host 10.10.10.1 |
                                                                         +-----------------+
```
### ConfigDB objects
```
{ 
    "VXLAN_TUNNEL": {
        "tunnel1": {
            "src_ip": "10.10.10.10"
        }
    },

    "VNET": {
        "Vnet_2000": {
            "vxlan_tunnel": "tunnel1",
            "vni": "2000",
            "peer_list": ""
        }
    },

    "INTERFACE": {
        "Ethernet1": { 
            "vnet_name": "Vnet_2000"
        }
    },
     
    "INTERFACE": {
        "Ethernet1|100.100.3.1/24": {}
    },
   
    "NEIGH": {
        "Ethernet1|100.100.3.2": {
            "family": "IPv4"
     }
}
```
### AppDB objects
```
{  
    "VNET_ROUTE_TABLE:Vnet_2000:100.100.3.0/24": { 
        "ifname": "Ethernet1", 
    }, 

    "VNET_ROUTE_TUNNEL_TABLE:Vnet_2000:100.100.1.1/32": { 
        "endpoint": "10.10.10.1", 
    }, 
}
```

<a id="item-6"></a>
## Baremetal to VM

From baremetal to VM, SONiC needs to encapsulate incoming packets in VxLAN tunnel. With the above example, traffic is from 
BM1 and to VM1. SONiC needs to encapulate incoming IP packet in VxLAN in DIP 10.10.10.1, SIP 10.10.10.10 and VNI 2000.
The inner DMAC is VxLAN MAC (SAI attribute SAI_SWITCH_ATTR_VXLAN_DEFAULT_ROUTER_MAC). To achieve this using VPP dataplane,
we need to create a dummy IP neighbour with MAC of the VxLAN MAC and interface of VxLAN tunnel. Then the IP route for the 
prefix in VNET_ROUTE_TUNNEL_TABLE points to the neighbour. Here is the equivalent VPP dataplane programming using VPP debug
CLI for illustration. 

```
create vxlan tunnel src 10.10.10.10 dst 10.10.10.1 vni 2000 instance 0
ip neighbor vxlan_tunnel0 10.10.10.1 00:00:00:00:00:01 no-fib-entry
ip route add 100.100.1.1/32 table 100 via 10.10.10.1 vxlan_tunnel0
```
MAC 00:00:00:00:00:01 is the VxLAN MAC mentioned above. Table 100 is the table ID assigned to vnet_2000. 
no-fib-entry is needed to create IP neighbour to avoid the dummy nexthop messing up forwarding.


<a id="item-7"></a>
## VM to Baremetal
From VM to Baremetal, SONiC needs to decapsulate incoming VxLAN packets, map to the VNET from VNI then forward to the baremetal
server using corresponding VNET route.
In VPP, this can be achieved through bridge domain and BVI interface. The VxLAN tunnel interface needs to be added to a bridge
domain along with a BVI interface. BVI interface is set with VxLAN MAC and added to the IP table corresponding to the VNET. 
Incoming VxLAN matching the VxLAN tunnel (SIP, DIP and VNI) will be decapsulated and sent to the BD. The inner DMAC after 
decapsulation matches BVI MAC. VPP will strip inner Ethernet header and forward the inner IP packet using routes of the 
IP table corresponding to the VNET.
Here is the equivalent VPP dataplane programming illustrated with VPP debug CLI.
```
bvi create mac 00:00:00:00:00:01 
set interface state bvi0 up
set interface ip table bvi0 100
set interface ip address bvi0 0.0.0.2/32
set interface l2 bridge vxlan_tunnel0 2000 1
set interface l2 bridge bvi0 2000 bvi
```
MAC 00:00:00:00:00:01 is the VxLAN MAC mentioned above. Table 100 is the table ID assigned to vnet_2000.
Non-routable IP 0.0.0.2 from reserved range is assigned to BVI interface. Otherwise, VPP won't enable IP protocol on the
interface. If there are multiple VxLAN tunnels terminated by the same VNET, there will be one bridge domain and BVI per 
tunnel. Each BVI interface in the VNET will have an unique IP address.
## References

[SONiC system architecture](https://github.com/sonic-net/SONiC/wiki/Architecture)\
[SONiC VxLAN High Level Design](https://github.com/sonic-net/SONiC/blob/master/doc/vxlan/Vxlan_hld.md)(#VxLAN-HLD)\
[What is VPP](https://s3-docs.fd.io/vpp/23.06/)