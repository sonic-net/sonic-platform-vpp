# Sonic-Mgmt ACL testing

The Sonic-VPP ACL implementation has been updated to support testing with sonic-mgmt.

The test cases in [`test_acl.py`](https://github.com/sonic-net/sonic-mgmt/blob/master/tests/acl/test_acl.py) can be executed on the T1 topology, subjet to the following limitations:

1. Empty ACL tables cannot be applied on a port before the ACL table under test is configured.

2. After config reload, the test must wait for the interfaces to come up before proceeding.

These VPP limitations are codified in the following sonic-mgmt PR: https://github.com/sonic-net/sonic-mgmt/pull/18313

Future support on sonic-mgmt will include:

- T1-lag / PortChannel support
- Incremental, Reload and PortToggle variations (800 tests)

The following results summarize the current status of the ACL test runs on the T1 topology:

```
========= 188 passed, 612 skipped, 663 warnings in 1039.06s (0:17:19) ==========
SKIPPED [4] acl/test_acl.py:1000: Only run for egress
SKIPPED [600] acl/test_acl.py: Phase 2 enablement
SKIPPED [4] acl/test_acl.py:992: Only run for ingress
SKIPPED [4] acl/test_acl.py: Failure on VPP
PASSED test_ingress_unmatched_blocked[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_source_ip_match_forwarded[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_rules_priority_forwarded[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_rules_priority_dropped[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_dest_ip_match_forwarded[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_dest_ip_match_dropped[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_source_ip_match_dropped[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_udp_source_ip_match_forwarded[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_udp_source_ip_match_dropped[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_icmp_source_ip_match_dropped[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_icmp_source_ip_match_forwarded[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_l4_dport_match_forwarded[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_l4_sport_match_forwarded[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_l4_dport_range_match_forwarded[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_l4_sport_range_match_forwarded[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_l4_dport_range_match_dropped[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_l4_sport_range_match_dropped[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_ip_proto_match_forwarded[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_tcp_flags_match_forwarded[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_l4_dport_match_dropped[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_l4_sport_match_dropped[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_ip_proto_match_dropped[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_tcp_flags_match_dropped[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_icmp_match_forwarded[ipv4-ingress-downlink->uplink-default-no_vlan]
PASSED test_ingress_unmatched_blocked[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_source_ip_match_forwarded[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_rules_priority_forwarded[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_rules_priority_dropped[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_dest_ip_match_forwarded[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_dest_ip_match_dropped[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_source_ip_match_dropped[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_udp_source_ip_match_forwarded[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_udp_source_ip_match_dropped[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_icmp_source_ip_match_dropped[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_icmp_source_ip_match_forwarded[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_l4_dport_match_forwarded[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_l4_sport_match_forwarded[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_l4_dport_range_match_forwarded[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_l4_sport_range_match_forwarded[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_l4_dport_range_match_dropped[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_l4_sport_range_match_dropped[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_ip_proto_match_forwarded[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_tcp_flags_match_forwarded[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_l4_dport_match_dropped[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_l4_sport_match_dropped[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_ip_proto_match_dropped[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_tcp_flags_match_dropped[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_icmp_match_forwarded[ipv4-ingress-uplink->downlink-default-no_vlan]
PASSED test_source_ip_match_forwarded[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_rules_priority_forwarded[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_rules_priority_dropped[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_dest_ip_match_forwarded[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_dest_ip_match_dropped[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_source_ip_match_dropped[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_udp_source_ip_match_forwarded[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_udp_source_ip_match_dropped[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_icmp_source_ip_match_dropped[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_icmp_source_ip_match_forwarded[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_l4_dport_match_forwarded[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_l4_sport_match_forwarded[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_l4_dport_range_match_forwarded[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_l4_sport_range_match_forwarded[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_l4_dport_range_match_dropped[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_l4_sport_range_match_dropped[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_ip_proto_match_forwarded[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_tcp_flags_match_forwarded[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_l4_dport_match_dropped[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_l4_sport_match_dropped[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_ip_proto_match_dropped[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_tcp_flags_match_dropped[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_icmp_match_forwarded[ipv4-egress-downlink->uplink-default-no_vlan]
PASSED test_source_ip_match_forwarded[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_rules_priority_forwarded[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_rules_priority_dropped[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_dest_ip_match_forwarded[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_dest_ip_match_dropped[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_source_ip_match_dropped[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_udp_source_ip_match_forwarded[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_udp_source_ip_match_dropped[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_icmp_source_ip_match_dropped[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_icmp_source_ip_match_forwarded[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_l4_dport_match_forwarded[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_l4_sport_match_forwarded[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_l4_dport_range_match_forwarded[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_l4_sport_range_match_forwarded[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_l4_dport_range_match_dropped[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_l4_sport_range_match_dropped[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_ip_proto_match_forwarded[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_tcp_flags_match_forwarded[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_l4_dport_match_dropped[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_l4_sport_match_dropped[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_ip_proto_match_dropped[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_tcp_flags_match_dropped[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_icmp_match_forwarded[ipv4-egress-uplink->downlink-default-no_vlan]
PASSED test_source_ip_match_forwarded[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_rules_priority_forwarded[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_rules_priority_dropped[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_dest_ip_match_forwarded[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_dest_ip_match_dropped[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_source_ip_match_dropped[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_udp_source_ip_match_forwarded[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_udp_source_ip_match_dropped[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_icmp_source_ip_match_dropped[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_icmp_source_ip_match_forwarded[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_l4_dport_match_forwarded[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_l4_sport_match_forwarded[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_l4_dport_range_match_forwarded[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_l4_sport_range_match_forwarded[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_l4_dport_range_match_dropped[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_l4_sport_range_match_dropped[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_ip_proto_match_forwarded[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_tcp_flags_match_forwarded[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_l4_dport_match_dropped[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_l4_sport_match_dropped[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_ip_proto_match_dropped[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_tcp_flags_match_dropped[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_icmp_match_forwarded[ipv6-egress-downlink->uplink-default-no_vlan]
PASSED test_source_ip_match_forwarded[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_rules_priority_forwarded[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_rules_priority_dropped[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_dest_ip_match_forwarded[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_dest_ip_match_dropped[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_source_ip_match_dropped[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_udp_source_ip_match_forwarded[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_udp_source_ip_match_dropped[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_icmp_source_ip_match_dropped[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_icmp_source_ip_match_forwarded[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_l4_dport_match_forwarded[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_l4_sport_match_forwarded[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_l4_dport_range_match_forwarded[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_l4_sport_range_match_forwarded[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_l4_dport_range_match_dropped[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_l4_sport_range_match_dropped[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_ip_proto_match_forwarded[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_tcp_flags_match_forwarded[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_l4_dport_match_dropped[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_l4_sport_match_dropped[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_ip_proto_match_dropped[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_tcp_flags_match_dropped[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_icmp_match_forwarded[ipv6-egress-uplink->downlink-default-no_vlan]
PASSED test_ingress_unmatched_blocked[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_source_ip_match_forwarded[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_rules_priority_forwarded[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_rules_priority_dropped[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_dest_ip_match_forwarded[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_dest_ip_match_dropped[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_source_ip_match_dropped[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_udp_source_ip_match_forwarded[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_udp_source_ip_match_dropped[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_icmp_source_ip_match_dropped[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_icmp_source_ip_match_forwarded[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_l4_dport_match_forwarded[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_l4_sport_match_forwarded[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_l4_dport_range_match_forwarded[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_l4_sport_range_match_forwarded[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_l4_dport_range_match_dropped[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_l4_sport_range_match_dropped[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_ip_proto_match_forwarded[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_tcp_flags_match_forwarded[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_l4_dport_match_dropped[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_l4_sport_match_dropped[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_ip_proto_match_dropped[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_tcp_flags_match_dropped[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_icmp_match_forwarded[ipv6-ingress-downlink->uplink-default-no_vlan]
PASSED test_ingress_unmatched_blocked[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_source_ip_match_forwarded[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_rules_priority_forwarded[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_rules_priority_dropped[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_dest_ip_match_forwarded[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_dest_ip_match_dropped[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_source_ip_match_dropped[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_udp_source_ip_match_forwarded[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_udp_source_ip_match_dropped[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_icmp_source_ip_match_dropped[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_icmp_source_ip_match_forwarded[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_l4_dport_match_forwarded[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_l4_sport_match_forwarded[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_l4_dport_range_match_forwarded[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_l4_sport_range_match_forwarded[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_l4_dport_range_match_dropped[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_l4_sport_range_match_dropped[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_ip_proto_match_forwarded[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_tcp_flags_match_forwarded[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_l4_dport_match_dropped[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_l4_sport_match_dropped[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_ip_proto_match_dropped[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_tcp_flags_match_dropped[ipv6-ingress-uplink->downlink-default-no_vlan]
PASSED test_icmp_match_forwarded[ipv6-ingress-uplink->downlink-default-no_vlan]
```

# Simple Topology Testing

You can use a simple routing topology as below to demonstrate SONiC VPP traffic filtering using ingress Access List

Host1 --------------------- Sonic-VPP-Router1 -------------- Sonic-VPP-Router2 ---------------- Host2

    172.16.1.2/24       172.16.1.1/24   172.16.3.1/24  172.16.3.2/24   172.16.2.1/24        172.16.2.2/24

Pre-requisites for testing this out
    Make sure the docker is installed on the Linux system. iproute2 and sudo packages should be installed.
    
## A brief about Access list and expected behavior

The ACL is configured in the sonic-vpp1 router and ACL is processed in context of interface with address 172.16.1.1/24. The ACL has two rules
 - allow only SSH sessions going from 172.16.1.0 subnet to 172.16.2.0 subnet with source port range of 1024-6K, destination port as 22 and protocol is being TCP
 - Deny all other traffic

The expected result as per the rules is to see only SSH traffic flowing.


Load container images
```
docker load < docker-sonic-vpp.gz
```

Check if images are listed 
```
docker images | grep "sonic-vpp\|pause"
```

Get the script to create and run the sonic container from the sonic-platform-vpp repo if you have not cloned the repo.
```
wget https://raw.githubusercontent.com/sonic-net/sonic-platform-vpp/main/start_sonic_vpp.sh 
```
Set the execute bits of the downloaded script file
```
chmod +x start_sonic_vpp.sh

```
Else if you have already cloned the sonic-platform-repo you can copy the script to the local directory and use it.

On the host create veth interface pairs for vpp and host 

```
sudo ip link add name veth_vpp1 type veth peer name vpp1
sudo ip link add name veth_vpp2 type veth peer name vpp2
sudo ip link add name ir_vpp1 type veth peer name ir_vpp2
```

Create network namespace for end hosts and inject one end of veth pair

```
sudo ip netns add host-1.0
sudo ip netns add host-2.0

sudo ip link set dev veth_vpp1 netns host-1.0
sudo ip link set dev veth_vpp2 netns host-2.0
```

Configure IP addresses inside the host net namepaces

```
sudo ip netns exec host-1.0 bash
ip link set dev veth_vpp1 up
ip addr add 172.16.1.2/24 dev veth_vpp1
ip route add 172.16.2.0/24 via 172.16.1.1
ip address show
ip route show
exit

sudo ip netns exec host-2.0 bash
ip link set dev veth_vpp2 up
ip addr add 172.16.2.2/24 dev veth_vpp2
ip route add 172.16.1.0/24 via 172.16.2.1
ip address show
ip route show
exit
```

Now start the sonic container and pass the veth interfaces to sonic-vpp
```
sudo ./start_sonic_vpp.sh start -n sonic-vpp1 -i vpp1,ir_vpp1
```

start the second sonic container and pass the veth interfaces to sonic-vpp
```
sudo ./start_sonic_vpp.sh start -n sonic-vpp2 -i vpp2,ir_vpp2
```

Copy the ACL rules file to sonic-vpp1 instance
```
wget https://raw.githubusercontent.com/sonic-net/sonic-platform-vpp/main/docs/test_2rules.json
docker cp test_2rules.json sonic-vpp1:/tmp/
```

Get into the first sonic container and configure network addresses and routes
```
docker exec -it sonic-vpp1 /bin/bash
ip link show

config interface ip add Ethernet0 172.16.1.1/24
config interface ip add Ethernet1 172.16.3.1/24
config interface startup Ethernet0
config interface startup Ethernet1
config route add prefix 172.16.2.0/24 nexthop 172.16.3.2

ip route show
config acl add table TEST_IPV4 L3 -p Ethernet0 -s ingress
config load -y /tmp/test_2rules.json
counterpoll acl enable

exit
```

Get into the second sonic container and configure network addresses and routes
```
docker exec -it sonic-vpp2 /bin/bash
ip link show

config interface ip add Ethernet0 172.16.2.1/24
config interface ip add Ethernet1 172.16.3.2/24
config interface startup Ethernet0
config interface startup Ethernet1
config route add prefix 172.16.1.0/24 nexthop 172.16.3.1

ip route show
exit
```

Test the Sonic ACL by generating some traffic

```
sudo ip netns exec host-1.0 bash
ping -c5  172.16.2.2
for i in $(seq 1 5); do ssh 172.16.2.2; done

exit
```
You should see the ping failure to the host in the other network namespace and ssh connection being reset.

Now check the ACL stats in the sonic-vpp1 instance
```
show acl table
show acl rule
aclshow
```

You should see acl table, rules and number of packets/bytes per rules printed(see below).

```
# show acl table
sudo: docker: command not found
Name       Type    Binding    Description    Stage    Status
---------  ------  ---------  -------------  -------  --------
TEST_IPV4  L3      Ethernet0  TEST_IPV4      ingress  Active
# show acl rule 
sudo: docker: command not found
Table      Rule     Priority    Action    Match                          Status
---------  -------  ----------  --------  -----------------------------  --------
TEST_IPV4  ALLOW    85          FORWARD   DST_IP: 172.16.2.0/24          Active
                                          IP_PROTOCOL: 6
                                          L4_DST_PORT: 22
                                          L4_SRC_PORT_RANGE: 1024-65535
                                          SRC_IP: 172.16.1.0/24
TEST_IPV4  TheDrop  80          DROP      IP_TYPE: IPv4ANY               Active
# aclshow
RULE NAME    TABLE NAME      PRIO    PACKETS COUNT    BYTES COUNT
-----------  ------------  ------  ---------------  -------------
ALLOW        TEST_IPV4         85                5            300
TheDrop      TEST_IPV4         80                5            420

```

Use below command to stop the sonic-vpp containers
```
sudo ./start_sonic_vpp.sh stop -n sonic-vpp1
sudo ./start_sonic_vpp.sh stop -n sonic-vpp2
```

