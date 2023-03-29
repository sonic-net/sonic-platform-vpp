# SONiC VPP routing with subinterfaces+Vrf configuration

Host1 -------------------------- Sonic-VPP-Router ---------------------- Host2

 172.16.1.2/24   (Vrfblue) 172.16.1.1/24     (Vrfred) 172.16.1.1/24        172.16.1.2/24

Pre-requisites for testing this out
    Make sure the docker is installed on the Linux system. iproute2 and sudo packages should be installed.
   Make sure you have four PCI based ethernet interfaces, in the current testbed  ens161,ens256,ens192,ens224 are the four ethernet interfaces. In your case they may be named differently.
    

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

Create network namespace for end hosts and inject one each interface belonging to the vlan subinterface.

```
sudo ip netns add host-1.0
sudo ip netns add host-2.0

sudo ip link set dev ens161 netns host-1.0
sudo ip link set dev ens256 netns host-2.0

```

Configure IP addresses inside the host net namepaces

```
sudo ip netns exec host-1.0 bash
ip link set dev ens161 up
ip link add link ens161 name ens161.10 type vlan id 10
ip link set dev ens161.10 up
ip addr add 172.16.1.2/24 dev ens161.10
ip address show
ip route show
exit

sudo ip netns exec host-2.0 bash
ip link set dev ens256 up
ip link add link ens256 name ens256.20 type vlan id 20
ip link set dev ens256.20 up
ip addr add 172.16.1.2/24 dev ens256.20
ip address show
ip route show
exit
```

Now start the sonic container and pass the veth interfaces to sonic-vpp
```
sudo ./start_sonic_vpp.sh start -n sonic-vpp -i ens192,ens224
```
> Note: For VRF to function correctly the corresponding interfaces also should be in different L2 domain. In this case ens161 and ens192 are in one L2 domain, ens256 and ens224 are in a different L2 domain.
> Note: If you use VMware vswitch create two port groups with different VLAN ids and place ens161,ens192 in one port group and ens256,ens224 in another.

Get into the sonic container and configure network addresses
```
docker exec -it sonic-vpp /bin/bash
ip link show

config interface startup Ethernet0
config interface startup Ethernet1

config vrf add Vrfblue
config vrf add Vrfred

config subinterface add Ethernet0.10
config subinterface add Ethernet1.20

config interface startup Ethernet0.10
config interface startup Ethernet1.20

config interface vrf bind Ethernet0.10 Vrfblue
config interface vrf bind Ethernet1.20 Vrfred

config interface shutdown Ethernet0.10
config interface startup Ethernet0.10

config interface shutdown Ethernet1.20
config interface startup Ethernet1.20

config interface ip add Ethernet0.10 172.16.1.1/24
config interface ip add Ethernet1.20 172.16.1.1/24

ip route show
exit
```

Test the Sonic routing

```
sudo ip netns exec host-1.0 bash
ping -c5  172.16.1.1
sudo ip netns exec host-2.0 bash
ping -c5  172.16.1.1
exit
```

You should see the ping reachability to sonic router and to the host in the other network namespace.

To stop the sonic-vpp container run
```
sudo ./start_sonic_vpp.sh stop -n sonic-vpp
```
