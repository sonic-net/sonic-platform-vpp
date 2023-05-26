# Using a simple routing topology as below this demonstrates SONiC VPP Loopback

  |-------------------------------------------------------------|
  |                                                             |
Host1 --------------------- Sonic-VPP-Router ---------------- Loopback0

    172.16.1.2/24         172.16.1.1/24   172.16.2.1/24        10.1.0.30/32

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

Next on the host system create veth interface pairs for vpp and host

```
sudo ip link add name veth_vpp1 type veth peer name vpp1
sudo ip link add name veth_vpp2 type veth peer name vpp2
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

Now in the host start the sonic container and pass the veth interfaces to sonic-vpp
```
sudo ./start_sonic_vpp.sh start -n sonic-vpp -i vpp1,vpp2
```

Get into the sonic container and configure network addresses
add the ipv4range and ipv6range for loopback interfaces

```
docker exec -it sonic-vpp /bin/bash
ip link show

vi /usr/share/sonic/hwsku/sonic_vpp_lpb_range.ini
ipv4range 10.1.0.0-10.1.255.255
ipv6range FC00:1::0-FC00:1::FF

exit
```

Get into the sonic container and configure network addresses 
and Loopback intrfaces

```
docker exec -it sonic-vpp /bin/bash

config interface ip add Ethernet0 172.16.1.1/24
config interface ip add Ethernet1 172.16.2.1/24
config interface startup Ethernet0
config interface startup Ethernet1

ip route show

config interface ip add Loopback0 10.1.0.30/32

show ip interfaces
exit
```

Get into VPP, check the loopback interfaces, address and lcp
then get out of vpp and then get out of the container

```
vppctl

sh int
sh int addr
sh lcp

quit
exit
```

Add route to the Loopback interface inside the host net namepace

```
sudo ip netns exec host-1.0 bash
ip route add 10.1.0.30/32 via 172.16.1.1
ip address show
ip route show
exit
```

Test the Loopback interface

```
sudo ip netns exec host-1.0 bash
ping -c5  10.1.0.30
exit
```

You should see the ping reachability to the loopback interface and see the ping traffic being punted to the Host Loopback interface

To remove the loopback interface perfor the reverse as shown

Remove route to the Loopback interface inside the host net namepace

```
sudo ip netns exec host-1.0 bash
ip route del 10.1.0.30/32 via 172.16.1.1
ip address show
ip route show
exit
```

Removed the loopback configuration from the sonic container

```
docker exec -it sonic-vpp /bin/bash

config interface ip remove Loopback0 10.1.0.30/32

show ip interfaces
exit
```

To stop the sonic-vpp container run
```
sudo ./start_sonic_vpp.sh stop -n sonic-vpp
```
