VxLAN EVPN
==========

## Setup

Build the following virtual setup in your favorite way (e.g. in GNS3).
Leaf-1 and Leaf-2 are VM's running sonic_vm (sonic-platform-vpp one).
Host-1 and Host-2 are VM's of your choice. 

```
                            Ethernet1             Ethernet3               Ethernet3          Ethernet1
 Host-1    <------------------------>   Leaf-1    <------------------------------->   Leaf-2  <-------------------------->  Host-2
           192.168.1.1                            10.10.10.1             10.10.10.2                                         192.168.1.1
```

## Configuration

### Leaf-1

```
config hostname Leaf-1
config save -y


config interface ip add Ethernet3 10.10.10.1/24
config interface ip add Loopback0 1.1.1.1/32
config route add prefix 1.1.1.2/32 nexthop 10.10.10.2

config vlan add 100
config vlan member add -u 100 Ethernet1
config vlan member add -u 100 Ethernet2

config vxlan add vtep0 1.1.1.1
config vxlan evpn_nvo add nvo vtep0
config vxlan map add vtep0 100 7000



# Configure BGP (IPv4 + EVPN)

vtysh
!
configure
hostname Leaf-1
log syslog informational
log facility local4
agentx
no service integrated-vtysh-config
!
password zebra
enable password zebra
!
router bgp 65100
 bgp router-id 1.1.1.1
 bgp log-neighbor-changes
 no bgp ebgp-requires-policy
 bgp bestpath as-path multipath-relax
 neighbor 10.10.10.2 remote-as 65100
 !
 address-family ipv4 unicast
  network 1.1.1.1/32
  maximum-paths 64
 exit-address-family
 !
 address-family l2vpn evpn
  neighbor 10.10.10.2 activate
  advertise-all-vni
 exit-address-family
exit
!
end
write
exit
```

### Leaf-2

```
config hostname Leaf-2
config save -y


config interface ip add Ethernet3 10.10.10.2/24
config interface ip add Loopback0 1.1.1.2/32
config route add prefix 1.1.1.1/32 nexthop 10.10.10.1

config vlan add 100
config vlan member add -u 100 Ethernet1
config vlan member add -u 100 Ethernet2

config vxlan add vtep0 1.1.1.2
config vxlan evpn_nvo add nvo vtep0
config vxlan map add vtep0 100 7000


# Configure BGP (IPv4 + EVPN)

vtysh
!
configure
hostname Leaf-2
log syslog informational
log facility local4
agentx
no service integrated-vtysh-config
!
password zebra
enable password zebra
!
router bgp 65100
 bgp router-id 1.1.1.2
 bgp log-neighbor-changes
 no bgp ebgp-requires-policy
 bgp bestpath as-path multipath-relax
 neighbor 10.10.10.1 remote-as 65100
 !
 address-family ipv4 unicast
  network 1.1.1.2/32
  maximum-paths 64
 exit-address-family
 !
 address-family l2vpn evpn
  neighbor 10.10.10.1 activate
  advertise-all-vni
 exit-address-family
exit
!
end
write
exit

```

## Test

BGP needs some time to update VxLan endpoints so wait 1-2 minutes (or less).
Then try to ping.
From Host-1:
```
ping 192.168.1.2
```
Ping should work

If you capture traffic between Lefa-1 and Leaf-2 you can  observe encapsulated ICMP packets.
