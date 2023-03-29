# Use a simple routing topology as below to demonstrate SONiC VPP routing

Host1 --------------------- Sonic-VPP-Router ---------------- Host2

    172.16.1.2/24         172.16.1.1/24   172.16.2.1/24        172.16.2.2/24

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
ip link add link veth_vpp1 name veth_vpp1.10 type vlan id 10
ip link set dev veth_vpp1.10 up
ip addr add 172.16.1.2/24 dev veth_vpp1.10
ip route add 172.16.2.0/24 via 172.16.1.1
ip address show
ip route show
exit

sudo ip netns exec host-2.0 bash
ip link set dev veth_vpp2 up
ip link add link veth_vpp2 name veth_vpp2.10 type vlan id 10
ip link set dev veth_vpp2.10 up
ip addr add 172.16.2.2/24 dev veth_vpp2.10
ip route add 172.16.1.0/24 via 172.16.2.1
ip address show
ip route show
exit

```

Now start the sonic container and pass the veth interfaces to sonic-vpp
```
sudo ./start_sonic.sh start -n sonic-vpp -i vpp1,vpp2
```

Get into the sonic container and configure network addresses
```
docker exec -it sonic-vpp /bin/bash
ip link show

config interface startup Ethernet0
config interface startup Ethernet1

config subinterface add Ethernet0.10
config subinterface add Ethernet1.10

config interface startup Ethernet0.10
config interface startup Ethernet1.10

config interface ip add Ethernet0.10 172.16.1.1/24
config interface ip add Ethernet1.10 172.16.2.1/24

ip route show
exit
```
> Note: Ideally sub-interfaces should belong to differnet vlan so that L2 domains are different ARP broadcast. Here it works because veth are virtual and are part of separate network namespaces.

> Note : A VPP process crash is seen if below order of sub interface configuration is carried out. The crash is being investigated. So please do not use the below configuration order.
> ** DO NOT USE BELOW CONFIG **
```
config subinterface add Ethernet0.10
config subinterface add Ethernet1.10
config interface ip add Ethernet0.10 172.16.1.1/24
config interface ip add Ethernet1.10 172.16.2.1/24
config interface startup Ethernet0
config interface startup Ethernet1
config interface startup Ethernet0.10
config interface startup Ethernet1.10

```
Test the Sonic routing

```
sudo ip netns exec host-1.0 bash
ping -c5  172.16.1.1
ping -c5  172.16.2.2
exit
```

You should see the ping reachability to sonic router and to the host in the other network namespace.

To stop the sonic-vpp container run
```
sudo ./start_sonic_vpp.sh stop -n sonic-vpp
```
