# VLAN BRIDGING Test

<img src="vlan-bridge-topo.png" alt="VLAN BRIDGING Simple Test Topology" width="1024" height="600" title="VLAN BRIDGING Simple Test Topology">


Host1-10.0.1.1/24|-------Ethernet0|SONiC-BR1|Ethernet3------Trunk------Ethernet3|SONic-BR2|Ethernet0-------|Host1-10.0.1.2/24
Host2-10.0.2.1/24|-------Ethernet1|SONiC-BR1|Ethernet3------Trunk------Ethernet3|SONiC-BR2|Ethernet1-------|Host2-10.0.2.2/24
Host3-10.0.3.1/24|-------Ethernet2|SONiC-BR1|Ethernet3------Trunk------Ethernet3|SONiC-BR2|Ethernet2-------|Host3-10.0.3.2/24

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
sudo ./start_sonic_vpp.sh start -n sonic-br2 -i ac4,ac5,ac6,veth_trunk
```

Get into sonic-br1 container and configure vlan and vlan bridging
sonic-br1:
```
docker exec -it sonic-br1 /bin/bash
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
ping -c5  10.0.3.2
exit

sudo ip netns exec host-4.0 bash
ping -c5  10.0.1.1
exit

sudo ip netns exec host-5.0 bash
ping -c5  10.0.2.1
exit

sudo ip netns exec host-6.0 bash
ping -c5  10.0.3.1
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

## FDB_ENTRY 
Considering the same topology as above, with 3 hosts configured on each switch, fdb entries of types STATIC and DYNAMIC can be added via config file through swssconfig command. The expectation is to see these entries on VPP end (data plane side).

From here on refer to the terms, **FDB - Forwading Data Base is on control plane SONiC** and **FIB - Fowarding Information Base on data plane VPP**.

Examining the FDB table below using 'fdbshow' command, it is evident that no entries have been appended to the SONiC Control plane. Conversely, upon executing ping operations between hosts 1 to 6, certain FIB entries are added. Each switch maintains its own FIB, with the respective MAC addresses of the hosts learned on that particular switch. For the purpose of evaluating the feature, consider below scenario invloving SONiC Switch 1 configured as sonic-br1 container.

```
fdbshow
```

```
No.    Vlan    MacAddress    Port    Type
-----  ------  ------------  ------  ------
Total number of entries 0
```

```
vppctl show l2fib all
```
```
    Mac-Address     BD-Idx If-Idx BSN-ISN Age(min) static filter bvi         Interface-Name        
 16:d8:51:a3:16:16    2      2      0/1      -       -      -     -             host-ac2           
 16:92:ef:71:03:e6    2      11     0/1      -       -      -     -           host-trunk.20        
 46:ee:fd:8b:c0:1e    3      13     0/1      -       -      -     -           host-trunk.30        
 1a:1e:10:97:35:a5    1      1      0/1      -       -      -     -             host-ac1           
 ea:5f:5a:6e:06:5d    3      3      0/1      -       -      -     -             host-ac3           
 f6:d2:03:81:f1:75    1      9      0/1      -       -      -     -           host-trunk.10        
L2FIB total/learned entries: 6/9  Last scan time: 0.0000e0sec  Learn limit: 16777216 

```
For FDB Entry addition on SONiC via config file, an fdbadd.json file with Sample MAC and VLAN ID are created, where one of the entry looks like below, please note for **ADD : "OP" = "SET"**, for **DEL "OP" = "DEL"**

```
  {
      "FDB_TABLE:Vlan10:10-10-10-10-10-10": {
          "port": "Ethernet0",
          "type": "static"
      },
      "OP": "SET"
  }

  {
    "FDB_TABLE:Vlan10:10-10-10-10-10-10": {
        "port": "Ethernet0",
        "type": "static"
    },
    "OP": "DEL"
  }
```

To add the entries we've created in fdbadd.json onto SONiC FDB, run the below command. The FDB table on SONiC and VPP L2 FIB table should look similar to the below which contains the newly added entries plus dynamically learnt entries on data plane( by running pings across the hosts 1-6 ).

```
swssconfig ./fdbadd.json 
```

```
fdbshow
```

`FDB_TABLE:`
```
  No.    Vlan  MacAddress         Port       Type
-----  ------  -----------------  ---------  -------
    1      10  10:10:10:10:10:10  Ethernet0  Static
    2      10  40:40:40:40:40:40  Ethernet3  Static
    3      10  70:70:70:70:70:70  Ethernet0  Dynamic
    4      20  20:20:20:20:20:20  Ethernet1  Static
    5      30  30:30:30:30:30:30  Ethernet2  Static
    6      30  60:60:60:60:60:60  Ethernet2  Dynamic
    7      30  50:50:50:50:50:50  Ethernet2  Dynamic
Total number of entries 7
```
```
vppctl show l2fib all
```
`VPP FIB_TABLE :`
```
    Mac-Address     BD-Idx If-Idx BSN-ISN Age(min) static filter bvi         Interface-Name        
 16:d8:51:a3:16:16    2      2      0/1      -       -      -     -             host-ac2           
 30:30:30:30:30:30    3      3      0/0      no      *      -     -             host-ac3           
 16:92:ef:71:03:e6    2      11     0/1      -       -      -     -           host-trunk.20        
 46:ee:fd:8b:c0:1e    3      13     0/1      -       -      -     -           host-trunk.30        
 50:50:50:50:50:50    3      3      0/0      no      -      -     -             host-ac3           
 1a:1e:10:97:35:a5    1      1      0/1      -       -      -     -             host-ac1           
 60:60:60:60:60:60    3      3      0/0      no      -      -     -             host-ac3           
 ea:5f:5a:6e:06:5d    3      3      0/1      -       -      -     -             host-ac3           
 70:70:70:70:70:70    1      1      0/0      no      -      -     -             host-ac1           
 f6:d2:03:81:f1:75    1      9      0/1      -       -      -     -           host-trunk.10        
 10:10:10:10:10:10    1      1      0/0      no      *      -     -             host-ac1           
 20:20:20:20:20:20    2      2      0/0      no      *      -     -             host-ac2           
L2FIB total/learned entries: 12/3  Last scan time: 0.0000e0sec  Learn limit: 16777216 
```

For deletion of previously added FDB Entries, it is necessary to create an additional config file fdbdel.json. This file must include the previously added FDB_ENTRY information with the "OP" set to "DEL," signifying the deletion operation from the SONiC perspective. Execution of the provided command will yield the below tables on the SONiC and VPP sides.

```
swssconfig ./fdbdel.json
```
```
fdbshow
```
```
No.    Vlan    MacAddress    Port    Type
-----  ------  ------------  ------  ------
Total number of entries 0
```
```
vppctl show l2fib all
```
```
    Mac-Address     BD-Idx If-Idx BSN-ISN Age(min) static filter bvi         Interface-Name        
 16:d8:51:a3:16:16    2      2      0/1      -       -      -     -             host-ac2           
 16:92:ef:71:03:e6    2      11     0/1      -       -      -     -           host-trunk.20        
 46:ee:fd:8b:c0:1e    3      13     0/1      -       -      -     -           host-trunk.30        
 1a:1e:10:97:35:a5    1      1      0/1      -       -      -     -             host-ac1           
 ea:5f:5a:6e:06:5d    3      3      0/1      -       -      -     -             host-ac3           
 f6:d2:03:81:f1:75    1      9      0/1      -       -      -     -           host-trunk.10        
L2FIB total/learned entries: 6/3  Last scan time: 0.0000e0sec  Learn limit: 16777216 
```


Evidently, all STATIC and DYNAMIC FDB Entries previously added via the configuration file are now deleted, both on SONiC and VPP sides. VPP may still retain valid entries since learning functionality is enabled on the VPP side.

## FDB_FLUSH
Flushing of FDB Entries can be accomplished through three methods as outlined below. Presently, SONiC supports the following two modes for flushing dynamically learned entries on the VPP side.
- Flush by Interface/Port ( Example: Flush all entries on Ethernet0 )
- Flush by Bridge/VLAN ID ( Example: Flush all entries on bridge with particular VLAN ID)
- Flush all entries
\* Please note that VPP Flushes only Dynamically learnt entries as STATIC type entries can only be deleted.


1. To illustrate FLUSH by Port/Interface, revisit the configuration file fdbadd.json to append sample FDB entries using the swssconfig command which should result in the FDB and FIB tables as depicted below.

```
fdbshow
```
```
  No.    Vlan  MacAddress         Port       Type
-----  ------  -----------------  ---------  -------
    1      10  10:10:10:10:10:10  Ethernet0  Static
    2      10  40:40:40:40:40:40  Ethernet3  Static
    3      10  70:70:70:70:70:70  Ethernet0  Dynamic
    4      20  20:20:20:20:20:20  Ethernet1  Static
    5      30  30:30:30:30:30:30  Ethernet2  Static
    6      30  60:60:60:60:60:60  Ethernet2  Dynamic
    7      30  50:50:50:50:50:50  Ethernet2  Dynamic
Total number of entries 7
```
```
vppctl show l2fib all
```
```
    Mac-Address     BD-Idx If-Idx BSN-ISN Age(min) static filter bvi         Interface-Name        
 16:d8:51:a3:16:16    2      2      0/1      -       -      -     -             host-ac2           
 30:30:30:30:30:30    3      3      0/0      no      *      -     -             host-ac3           
 16:92:ef:71:03:e6    2      11     0/1      -       -      -     -           host-trunk.20        
 46:ee:fd:8b:c0:1e    3      13     0/1      -       -      -     -           host-trunk.30        
 50:50:50:50:50:50    3      3      0/0      no      -      -     -             host-ac3           
 1a:1e:10:97:35:a5    1      1      0/1      -       -      -     -             host-ac1           
 60:60:60:60:60:60    3      3      0/0      no      -      -     -             host-ac3           
 ea:5f:5a:6e:06:5d    3      3      0/2      -       -      -     -             host-ac3           
 70:70:70:70:70:70    1      1      0/0      no      -      -     -             host-ac1           
 f6:d2:03:81:f1:75    1      9      0/1      -       -      -     -           host-trunk.10        
 10:10:10:10:10:10    1      1      0/0      no      *      -     -             host-ac1           
 20:20:20:20:20:20    2      2      0/0      no      *      -     -             host-ac2 
 L2FIB total/learned entries: 12/2  Last scan time: 1.3616e-2sec  Learn limit: 16777216 
```
In the above FIB table, MAC address `"ea:5f:5a:6e:06:5d"` is learnt dynamically by VPP which will now be deleted via below SONiC command as part of FLUSH by port on Ethernet2 interface by passing corresponding interface name using *"-p"* flag.


```
fdbclear -p Ethernet2
```

```
fdbshow
```
```
  No.    Vlan  MacAddress         Port       Type
-----  ------  -----------------  ---------  -------
    1      10  10:10:10:10:10:10  Ethernet0  Static
    2      10  40:40:40:40:40:40  Ethernet3  Static
    3      10  70:70:70:70:70:70  Ethernet0  Dynamic
    4      20  20:20:20:20:20:20  Ethernet1  Static
    5      30  30:30:30:30:30:30  Ethernet2  Static
Total number of entries 5
```
```
vppctl show l2fib all
    Mac-Address     BD-Idx If-Idx BSN-ISN Age(min) static filter bvi         Interface-Name        
 16:d8:51:a3:16:16    2      2      0/1      -       -      -     -             host-ac2           
 30:30:30:30:30:30    3      3      0/0      no      *      -     -             host-ac3           
 16:92:ef:71:03:e6    2      11     0/1      -       -      -     -           host-trunk.20        
 46:ee:fd:8b:c0:1e    3      13     0/1      -       -      -     -           host-trunk.30        
 50:50:50:50:50:50    3      3      0/0      no      -      -     -             host-ac3           
 1a:1e:10:97:35:a5    1      1      0/1      -       -      -     -             host-ac1           
 60:60:60:60:60:60    3      3      0/0      no      -      -     -             host-ac3           
 70:70:70:70:70:70    1      1      0/0      no      -      -     -             host-ac1           
 f6:d2:03:81:f1:75    1      9      0/1      -       -      -     -           host-trunk.10        
 10:10:10:10:10:10    1      1      0/0      no      *      -     -             host-ac1           
 20:20:20:20:20:20    2      2      0/0      no      *      -     -             host-ac2           
L2FIB total/learned entries: 11/5  Last scan time: 1.3651e-2sec  Learn limit: 16777216 
```
The MAC address learned on the Interface ``host-ac3``, corresponding to **"Ethernet2"** is flushed which can be observed from the above table .


2. If FIB entries need to be cleared irrespective of Port and Bridge IDs, below SONiC command helps in flushing out all the dynamic entries learnt on VPP side. Here fdbclear has no parameters passed which by default flushes all of dynamically learnt entries on FIB table.

```
fdbclear
```
```
FDB entries are cleared.
```
```
fdbshow
```
```
  No.    Vlan  MacAddress         Port       Type
-----  ------  -----------------  ---------  ------
    1      10  10:10:10:10:10:10  Ethernet0  Static
    2      10  40:40:40:40:40:40  Ethernet3  Static
    3      20  20:20:20:20:20:20  Ethernet1  Static
    4      30  30:30:30:30:30:30  Ethernet2  Static
Total number of entries 4
```
```
vppctl show l2fib all
```
```
    Mac-Address     BD-Idx If-Idx BSN-ISN Age(min) static filter bvi         Interface-Name        
 30:30:30:30:30:30    3      3      0/0      no      *      -     -             host-ac3           
 50:50:50:50:50:50    3      3      0/0      no      -      -     -             host-ac3           
 60:60:60:60:60:60    3      3      0/0      no      -      -     -             host-ac3           
 70:70:70:70:70:70    1      1      0/0      no      -      -     -             host-ac1           
 10:10:10:10:10:10    1      1      0/0      no      *      -     -             host-ac1           
 20:20:20:20:20:20    2      2      0/0      no      *      -     -             host-ac2           
L2FIB total/learned entries: 6/0  Last scan time: 1.3682e-2sec  Learn limit: 16777216 
```
Concluding the above all, SONiC+VPP now supports FDB_ENTRY with ADD/DEL operation along with FDB_FLUSH of Dynamic Entries are now supported with these features.
