# VLAN BRIDGING Test

<img src="vlan-bridge-topo.png" alt="VLAN BRIDGING Simple Test Topology" width="1024" height="600" title="VLAN BRIDGING Simple Test Topology">


Host1-10.0.1.1/24|-------Ethernet0|_______________________________________________________|Ethernet0-------|Host1-10.0.1.2/24
Host2-10.0.2.1/24|-------Ethernet1|SONiC-BR1|Ethernet3------Trunk------Ethernet3|SONiC-BR2|Ethernet1-------|Host2-10.0.2.2/24
Host3-10.0.3.1/24|-------Ethernet2|_______________________________________________________|Ethernet2-------|Host3-10.0.3.2/24

Pre-requisites for testing this out
    Make sure the docker is installed on the Linux system. iproute2 and sudo packages should be installed.
    
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
sudo ip link add name veth_ac1 type veth peer name ac1
sudo ip link add name veth_ac2 type veth peer name ac2
sudo ip link add name veth_ac3 type veth peer name ac3
sudo ip link add name veth_ac4 type veth peer name ac4
sudo ip link add name veth_ac5 type veth peer name ac5
sudo ip link add name veth_ac6 type veth peer name ac6
sudo ip link add name veth_trunk type veth peer name trunk

```

Create network namespace for end hosts and inject one end of veth pair

```
sudo ip netns add host-1.0
sudo ip netns add host-2.0
sudo ip netns add host-3.0
sudo ip netns add host-4.0
sudo ip netns add host-5.0
sudo ip netns add host-6.0
sudo ip link set dev veth_ac1 netns host-1.0
sudo ip link set dev veth_ac2 netns host-2.0
sudo ip link set dev veth_ac3 netns host-3.0
sudo ip link set dev veth_ac4 netns host-4.0
sudo ip link set dev veth_ac5 netns host-5.0
sudo ip link set dev veth_ac6 netns host-6.0
```

Configure IP addresses inside the host net namepaces

```
sudo ip netns exec host-1.0 bash
ip link set dev veth_ac1 up
ip addr add 10.0.1.1/24 dev veth_ac1
ip address show
ip route show
exit
sudo ip netns exec host-2.0 bash
ip link set dev veth_ac2 up
ip addr add 10.0.2.1/24 dev veth_ac2
ip address show
ip route show
exit
sudo ip netns exec host-3.0 bash
ip link set dev veth_ac3 up
ip addr add 10.0.3.1/24 dev veth_ac3
ip address show
ip route show
exit
sudo ip netns exec host-4.0 bash
ip link set dev veth_ac4 up
ip addr add 10.0.1.2/24 dev veth_ac4
ip address show
ip route show
exit
sudo ip netns exec host-5.0 bash
ip link set dev veth_ac5 up
ip addr add 10.0.2.2/24 dev veth_ac5
ip address show
ip route show
exit
sudo ip netns exec host-6.0 bash
ip link set dev veth_ac6 up
ip addr add 10.0.3.2/24 dev veth_ac6
ip address show
ip route show
exit
```

Now start the sonic container and pass the veth interfaces to sonic-vpp
```
sudo ./start_sonic_vpp.sh start -n sonic-br1 -i ac1,ac2,ac3,trunk
```

start the second sonic container and pass the veth interfaces to sonic-vpp
```
sudo ./start_sonic_vpp.sh start -n sonic-br2 -i ac3,ac4,ac5,veth_trunk
```

Get into sonic-br1 container and configure vlan and vlan bridging
sonic-br1:
```
docker exec -it sonic-br2 /bin/bash
show interface status
config interface startup Ethernet0
config interface startup Ethernet1
config interface startup Ethernet2
config interface startup Ethernet3

config vlan add 10
config vlan member add -u 10 Ethernet0
config vlan member add  10 Ethernet3

config vlan add 20
config vlan member add -u 20 Ethernet1
config vlan member add  20 Ethernet3

config vlan add 30
config vlan member add -u 30 Ethernet2
config vlan member add  30 Ethernet3
```

Get into sonic-br2 container and configure vlan , vlan bridging
sonic-br2:
```
docker exec -it sonic-br2 /bin/bash
show interface status
config interface startup Ethernet0
config interface startup Ethernet1
config interface startup Ethernet2
config interface startup Ethernet3

config vlan add 10
config vlan member add -u 10 Ethernet0
config vlan member add  10 Ethernet3

config vlan add 20
config vlan member add -u 20 Ethernet1
config vlan member add  20 Ethernet3

config vlan add 30
config vlan member add -u 30 Ethernet2
config vlan member add  30 Ethernet3
```
Test the SONiC vlan briding function across 3 VLAN bridge domains

```
sudo ip netns exec host-1.0 bash
ping -c5  10.0.1.2
exit

sudo ip netns exec host-2.0 bash
ping -c5  10.0.2.2
exit

sudo ip netns exec host-3.0 bash
ping -c5  10.0.2.2
exit
```

You should see the ping reachability from host1 to host4, host2 to host5 and host3 to host6. 

Use below command to stop the sonic-vpp containers
```
sudo ./start_sonic_vpp.sh stop -n sonic-br1
sudo ./start_sonic_vpp.sh stop -n sonic-br2
```

Show command outputs
```
show interface status
```
```
  Interface        Lanes    Speed    MTU    FEC          Alias    Vlan    Oper    Admin    Type    Asym PFC
  +--------------------------------------------------------------------------------------------------------+
  Ethernet0  25,26,27,28     100G   9100    N/A   fortyGigE0/0  routed      up       up     N/A         N/A
  Ethernet1  29,30,31,32     100G   9100    N/A   fortyGigE0/4  routed      up     down     N/A         N/A
  Ethernet2  33,34,35,36     100G   9100    N/A   fortyGigE0/8  routed      up     down     N/A         N/A
  Ethernet3  37,38,39,40     100G   9100    N/A  fortyGigE0/12  routed      up     down     N/A         N/A
```
```
show vlan brief
```
```
+-----------+--------------+-----------+----------------+-------------+
|   VLAN ID | IP Address   | Ports     | Port Tagging   | Proxy ARP   |
+===========+==============+===========+================+=============+
|        10 |              | Ethernet0 | untagged       | disabled    |
|           |              | Ethernet3 | tagged         |             |
+-----------+--------------+-----------+----------------+-------------+
|        20 |              | Ethernet1 | untagged       | disabled    |
|           |              | Ethernet3 | tagged         |             |
+-----------+--------------+-----------+----------------+-------------+
|        30 |              | Ethernet2 | untagged       | disabled    |
|           |              | Ethernet3 | tagged         |             |
+-----------+--------------+-----------+----------------+-------------+
```
```
show vlan config
```
```
Name      VID  Member     Mode
------  -----  ---------  --------
Vlan10     10  Ethernet0  untagged
Vlan10     10  Ethernet3  tagged
Vlan20     20  Ethernet1  untagged
Vlan20     20  Ethernet3  tagged
Vlan30     30  Ethernet2  untagged
Vlan30     30  Ethernet3  tagged
```
```
vppctl show bridge
```
```
  BD-ID   Index   BSN  Age(min)  Learning  U-Forwrd   UU-Flood   Flooding  ARP-Term  arp-ufwd Learn-co Learn-li   BVI-Intf 
   10       1      0     off        on        on       flood        on       off       off        0    16777216     N/A    
   20       2      0     off        on        on       flood        on       off       off        0    16777216     N/A    
   30       3      0     off        on        on       flood        on       off       off        1    16777216     N/A    
```
```
vppctl show bridge 10 detail
```
```
  BD-ID   Index   BSN  Age(min)  Learning  U-Forwrd   UU-Flood   Flooding  ARP-Term  arp-ufwd Learn-co Learn-li   BVI-Intf 
   10       1      0     off        on        on       flood        on       off       off        0    16777216     N/A    
span-l2-input l2-input-classify l2-input-feat-arc l2-policer-classify l2-input-acl vpath-input-l2 l2-ip-qos-record l2-input-vtr l2-learn l2-rw l2-fwd l2-flood l2-flood l2-output 

           Interface           If-idx ISN  SHG  BVI  TxFlood        VLAN-Tag-Rewrite       
           host-ac4              1     1    0    -      *            push-1 dot1q 10       
      host-veth_trunk.10         13    1    0    -      *                 none             
```
```
vppctl show bridge 20 detail
```
```
  BD-ID   Index   BSN  Age(min)  Learning  U-Forwrd   UU-Flood   Flooding  ARP-Term  arp-ufwd Learn-co Learn-li   BVI-Intf 
   20       2      0     off        on        on       flood        on       off       off        0    16777216     N/A    
span-l2-input l2-input-classify l2-input-feat-arc l2-policer-classify l2-input-acl vpath-input-l2 l2-ip-qos-record l2-input-vtr l2-learn l2-rw l2-fwd l2-flood l2-flood l2-output 

           Interface           If-idx ISN  SHG  BVI  TxFlood        VLAN-Tag-Rewrite       
           host-ac5              2     1    0    -      *            push-1 dot1q 20       
      host-veth_trunk.20         11    1    0    -      *                 none             
```
```
vppctl show bridge 30 detail
```
```
  BD-ID   Index   BSN  Age(min)  Learning  U-Forwrd   UU-Flood   Flooding  ARP-Term  arp-ufwd Learn-co Learn-li   BVI-Intf 
   30       3      0     off        on        on       flood        on       off       off        1    16777216     N/A    
span-l2-input l2-input-classify l2-input-feat-arc l2-policer-classify l2-input-acl vpath-input-l2 l2-ip-qos-record l2-input-vtr l2-learn l2-rw l2-fwd l2-flood l2-flood l2-output 

           Interface           If-idx ISN  SHG  BVI  TxFlood        VLAN-Tag-Rewrite       
           host-ac6              3     1    0    -      *            push-1 dot1q 30       
      host-veth_trunk.30         9     1    0    -      *                 none             
```

Verification of packet on VPP data plane for vlan tagging
Start the ping and then do show trace

```
vppctl
trace add af-packet-input 10
show trace
```
