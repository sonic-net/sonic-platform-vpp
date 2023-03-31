# Use a simple routing topology as below to demonstrate SONiC VPP routing

Host1 ------------------------------------ Sonic-VPP-Router ---------------- Host2

2001:0db8:85a3::0370:7334/64    2001:0db8:85a3::0370:7335/64   2001:0db8:85a4::0370:7334/64  2001:0db8:85a4::0370:7335/64

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
ip -6 addr add 2001:0db8:85a3::0370:7334/64 dev veth_vpp1
ip -6 route add 2001:0db8:85a4::0370:7335/64 via 2001:0db8:85a3::0370:7335
ip -6 address show
ip -6 route show
exit

sudo ip netns exec host-2.0 bash
ip link set dev veth_vpp2 up
ip -6 addr add 2001:0db8:85a4::0370:7335/64 dev veth_vpp2
ip -6 route add 2001:0db8:85a3::0370:7334/64 via 2001:0db8:85a4::0370:7334
ip -6 address show
ip -6 route show
exit
```

Now in the host start the sonic container and pass the veth interfaces to sonic-vpp
```
sudo ./start_sonic_vpp.sh start -n sonic-vpp -i vpp1,vpp2
```

Get into the sonic container and configure network addresses
```
docker exec -it sonic-vpp /bin/bash
ip link show

config interface ip add Ethernet0 2001:0db8:85a3::0370:7335/64
config interface ip add Ethernet1 2001:0db8:85a4::0370:7334/64
config interface startup Ethernet0
config interface startup Ethernet1

ip route show
exit
```

Test the Sonic routing

```
sudo ip netns exec host-1.0 bash
ping -6 -c5  2001:0db8:85a3::0370:7335
ping -6 -c5  2001:0db8:85a4::0370:7335
exit
```

You should see the ping reachability to sonic router and to the host in the other network namespace.

To stop the sonic-vpp container run
```
sudo ./start_sonic_vpp.sh stop -n sonic-vpp
```
