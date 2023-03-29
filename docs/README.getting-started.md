## Introduction
There are two form factors of SONiC-VPP
 * Single docker container image running subset of functionality of SONiC
 * VM image running full functionality of SONiC

This document explains single container image. Please refer to [README.sonic_vm.md](README.sonic_vm.md) on how to run a VM image.

The flow goes like this
 * Fullfill the requirements in pre-requisites
 * Find out the network interfaces to use
 * Check the start_sonic_vpp.sh script usage
 * Start a SONiC-VPP container

## Pre-requisites to run the container image
 * Linux host running Ubuntu18 or later distribution (other Linux distrubutions should also work)
 * Access to docker-sonic-vpp.gz container image and start_sonic_vpp.sh script
 * docker package installed and running on the system
 * iproute2 package

```
sudo apt-get install -y iproute2 docker-ce docker.io
```
> Note - start_sonic_vpp.sh is found in the sonic-platform-vpp repository root directory

## Network interfaces
Before invoking the container you need to find out how many interfaces to pass to the container for SONiC-VPP data path. Two types of interfaces are supported

 * Virtual veth interfaces
 * PCI based network interfaces

Virtual interface is a veth pair, one end is in the host and the other end is passed to the SONiC-VPP container.
> Note - The Virtual interfaces are deleted once the container is stopped.

The PCI based interfaces are configured/managed by the VPP-DPDK and these interfaces get yanked out from the Linux host system once the container runs. The interfaces are restored to the host system once the container is stoped. The ethtool or lspci can be used to find out physical interfaces.
```
ethtool -i INTERFACE_NAME
lspci -t -v
```
> Disclaimer: The SR-IOV PF and VF interfaces should also work just like any other PCI based ports. SR-IOV ports testing is still not done fully. These tests will be covered in the coming days.

## start_sonic_vpp.sh script usage
```
sudo ../start_sonic_vpp.sh 
Please specify a name for the sonic instance

    Usage: start_sonic_vpp.sh command -n -i net_ports [-v]
    command - start or stop or status
    -i netports - comma separate listed of network ports (virtual interfaces, physical network interfaces or PCI bus address of a network inteface)
    (Use ethtool -i port_name | grep bus-info | awk '{ print }' command to find out PCI address of a network interface)
    -n container_name
    -v - verbose

```
The script is for instantiating, stopping or checking status of a sonic-vpp container. The script should be run with root privilege.
The container name('-n'), network interfaces list('-i') are mandatory parameters. The name should be
unique, since the script allows starting of multiple instances of the sonic-vpp container.
To stop the container only name is mandatory.

The interface list can be mix of physical interfaces, virtual interfaces (veth). The physical
interfaces PCI ids are determined and passed as PCI-id to the sonic-vpp container. The DPDK
plugin of VPP owns these PCI bound network interfaces. Mixed set of the interfaces is supported.

> Note: If interfaces list contains only virtual interfaces then the sonic-vpp container disables DPDK and the virtual interfaces are handled through AF_PACKET mechanism.

The correspondance of network interfaces to the SONiC ports name is one-to-one and read from left to right.
-i "eth2,eth3,veth1,veth2" map to "Ethernet0, Ethernet1, Ethernet2, Ethernet4" i.e.
```
eth2 -> Ethernet0
eth3 -> Ethernet1
veth1 -> Ethernet2
veth2 -> Ethernet3
```

Below are the commands supported by the script

'start' - instantiates the sonic-vpp container. Name and interface list are mandatory paramaters
'stop' - stop the sonic-vpp container mentioned in the name
'status' - print the status of the named container

> Note: *docker ps* will list two containers, one with user specified CONTAINER_NAME and the other will be named "CONTAINER-NAME-nss". The second container is just a helper container to bootstrap the interface ports. It does not have any functional processes in it.

## Simple SONiC-VPP Router(container) in action
[Simple topology](README.simple.topo.md) shows procedure to run a SONiC-VPP container with virtual interfaces. The same applies to PCI based interfaces except that the interfaces passed are physical interfaces(PCI).

