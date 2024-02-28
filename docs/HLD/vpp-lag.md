# SONIC VPP LAG Bridging support
Rev v0.1

<br/>
<br/>

## Table of Contents

1. [Revisions](#item-1)
2. [Scope](#item-2)
3. [Definitions/Abbreviations](#item-3)
4. [Introduction](#item-4)
5. [LAG and Bridging](#item-5)

<br/>
<br/>

<a id="item-1"></a>
## Revisions

| Rev | Date | Author(s) |
|-----|------|-----------|
|v0.1 | 25/02/2024 | Bendrapu Balareddy (Cisco), Sameer Nanajkar (Cisco) |


<br/>
<br/>

<a id="item-2"></a>
## Scope
This document describes the high level design of integrating LAG and bridging functionality between SONIC and VPP. It provides
 - LAG 
 - 802.1Q bridging

<br/>

<a id="item-3"></a>
## Definitions/Abbreviations
**VPP**: Vector Packet Processing (VPP) technology based high performance packet processing stack that can run on commodity CPUs. For more details see [What is VPP](https://wiki.fd.io/view/VPP/What_is_VPP%3F)

**LAG**:  Ling Aggregation Group


<br/>
<br/>

<a id="item-4"></a>
## Introduction
LAG functionality is supported in SONiC with port channel interfaces. 

 
<a id="item-5"></a>
![LAG Bridging](../LAG-Dot1q-Bridging-Topo.png) 

SONiC supports port channel interfaces. These interfaces can be configured for layer routing or as members of a vlan
 - Access port
   config vlan member add -u <portname> creates an un tagged bridge port. This will be translated to vpp in two steps
   1. creates a bridge with given vlan id as bridge id if the bridge with given id doesnot exist.
   2. set the port as member of bridge with appropriate tag rewrite
 - Trunk port
   config vlan member add  <portname> creates a tagged port bridge port. This will be translated to vpp in three steps
   1. creates a bridge with given vlan id as bridge id if the bridge with given aid does not exist.
   2. vpp trunk port operation is achieved through vlan subinterface. A subinterface is created for this vlan id. Note
      that this subinterface is only specific to vpp and does not appear in SONiC control plane data bases.
   3. set the vlan subinterface as bridge port


## References

[SONiC system architecture](https://github.com/sonic-net/SONiC/wiki/Architecture)\
[What is VPP](https://s3-docs.fd.io/vpp/23.06/)

