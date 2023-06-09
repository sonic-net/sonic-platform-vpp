# Use a simple routing topology as below to demonstrate SONiC VPP routing

Host1 --------------------- Sonic-VPP-Router1 -------------- Sonic-VPP-Router2 ---------------- Host2

    172.16.1.2/24       172.16.1.1/24   172.16.3.1/24  172.16.3.2/24   172.16.2.1/24        172.16.2.2/24

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

Get into the first sonic container and configure network addresses and routes
```
docker exec -it sonic-vpp1 /bin/bash
ip link show

config interface ip add Ethernet0 172.16.1.1/24
config interface ip add Ethernet1 172.16.3.1/24
config interface startup Ethernet0
config interface startup Ethernet1
ip route add 172.16.2.0/24 via 172.16.3.2

ip route show
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
ip route add 172.16.1.0/24 via 172.16.3.1

ip route show
exit
```

Test the Sonic routing

```
sudo ip netns exec host-1.0 bash
ping -c5  172.16.1.1
ping -c5  172.16.2.2
exit
```

You should see the ping reachability to sonic router and to the host in the other network namespace.

Use below command to stop the sonic-vpp containers
```
sudo ./start_sonic_vpp.sh stop -n sonic-vpp1
sudo ./start_sonic_vpp.sh stop -n sonic-vpp2
```

