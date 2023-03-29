# SONiC-VPP VM bringup

## A brief about the setup
The sonic-vpp.img.gz is found at sonic-buildimage/build/target/ directory. This is a qemu qcow disk image with storage prebuilt for some directories.

Testing this image is simple.
. Make sure the packages mentioned in the Prerequisites sections are installed
. Fireup qemu-system (kvm) with the sonic-vpp.img as disk image
. Connect to the running vm either by serial port or by vncviewer
. Configure an IP address on eth0 of the SONiC so that you can access it from a linux system.
. Configure the SONiC-VPP using sonic_vpp_cfg.sh script to pass the required interfaces list.
. Reboot the SONiC.
. Configure the network addresses on the host and on the SONiC VM.
. Run a ping. Voila!!! the sonic-vpp is up and running

## Prerequisites
* Run a latest Linux distribution Ubuntu 18 or higher on the Host

* install following packages
```
sudo apt-get install -y qemu-system-x86 qemu-kvm
```

> Note : Make sure the host Linux OS enables the SSE4.3 features by passing the right CPU flags to the qemu-kvm command. In this case "-cpu host" option is used and the host is running on a latest SandyBridge or above intel processor. The VPP process in syncd container crashes if SSE instructions are not available.

## Procedure
1. Decompress the image by
```
gzip -d sonic-vpp.img.gz
```

2. Start the vm with below command.
```
sudo qemu-system-x86_64 -name sonic-simulator_1 -m 4096M -cpu host -smp cpus=4 -drive file=./sonic-vpp.img,index=0,media=disk,id=drive0 -serial telnet:127.0.0.1:5001,server,nowait -monitor tcp:127.0.0.1:44001,server,nowait -device e1000,netdev=net0 -netdev tap,ifname=tap0,id=net0 -device e1000,netdev=net1 -netdev tap,ifname=sonic-tap1,id=net1 -device e1000,netdev=net2 -netdev tap,ifname=sonic-tap2,id=net2 -display vnc=:4 -daemonize -enable-kvm

```
> Note: Three network interfaces are passed to the VM. These show up as eth0, eth1 and eth2. eth0 is for management in SONiC and the counterpart o f eth0 on the host is tap0. eth1 and eth2 are for  data path usage. The host side these appear as sonic-tap1, sonic-tap2
> Note: Enable netsted virtualization for the host (if host itself is a VM) for better performance

Now make sure the qemu-system proces is running
```
ps -ax | grep qemu-system
```

Wait for few minutes (3 minutes) for the VM to boot SONiC.

3. Connect to the serial port of VM and login as admin/YourPaSsWoRd
```
telnet localhost 5001
```
Once logged in configure an IP address for the management interface eth0. In my case I used 10.0.0.1/24
```
sudo config interface ip add eth0 10.0.0.1/24 10.0.0.2
sudo config save -y
```
Come out of telnet session (Ctrl-] followed by quit command and enter) and configure an Ip address on the host's tap0 interface

```
sudo ip addr add 10.0.0.2/24 dev tap0
```

Now you should be able ssh into the VM using credentials admin/YourPaSsWoRd.

4. Time to configure the vpp. In the SSH session run
```
sudo sonic_vpp_cfg.sh -p -i eth1,eth2
```
The above command will generate required configuration of SONiC and syncd-vpp. It will prompt user for confirmation unless -p option is chosen.

> Note: The mgmt eth0 address is saved to config_db(in step 3) so there is no need to reconfigure the ip address of mgmt interface most of the time. After reboot if mgmt interface address is lost  rerun the step 3.

5. After reboot you should be able to see syncd container running and the "ip link" should list Ethernet0, Ethernet1 interfaces. Connect the SONiC either using serial port or ssh of mgmt IP address.
```
docker ps | grep syncd
ip link | grep Ethernet

```

6. Now we are ready to configure the IP addresses for data path interfaces

```
sudo config interface ip add Ethernet0 20.0.0.2/24
sudo config interface ip add Ethernet1 30.0.0.2/24
sudo config interface startup Ethernet0
sudo config interface startup Ethernet1
sudo config save -y
```
7. On the host configure ip addresses on the host side of interfaces

```
sudo ip link set dev sonic-tap1 up
sudo ip link set dev sonic-tap2 up
sudo ip addr add 20.0.0.1/24 dev sonic-tap1
sudo ip addr add 30.0.0.1/24 dev sonic-tap2

```

Ping 20.0.0.2 to from the host. If ping succeeds SONiC-VPP data path is functioning.

> Note: Once the SONiC qemu VM instace is terminated the tap0, sonic-tap1, sonic-tap2 interfaces on the host get deleted by the qemu. So after a frest start of SONiC VM instance you need to reconfigure the ip addreses on the newly created interfaces as above. Where as the Ip addresses configured on the interfaces of the SONIC are effective after the reboot.

> Note: To avoid manual configuration you can write a script to automate IP address configuration. Just pass this script to qemu "-netdev" paramater as "-netdev tap,ifname=tap0,id=net0,script=SCRIPT_ABSOLUTE_PATH" for each network interface. Check "man qemu-system-x86_64" for details. OR try the below bridge method to setup once for the life of the host.

## Setup once host bridge network for SONiC
The below setup does not require writing of any script. Instead it relies on default /etc/qemu-ifup script to add the host side tap interfaces to user configured sonic bridge. The interfaces on the host side are part of the bridge and have ip addresses configured. These interfaces and their ip addresses remain intact even after the SONiC VM is terminated.
```
sudo brctl addbr sonic-br
sudo ip link add name veth_sonic1 type veth peer name sonic1
sudo ip link add name veth_sonic2 type veth peer name sonic2
sudo ip link add name veth_sonic_mgmt type veth peer name sonic_mgmt
sudo brctl addif sonic-br veth_sonic1
sudo brctl addif sonic-br veth_sonic2
sudo brctl addif sonic-br veth_sonic_mgmt

sudo ip link set dev sonic1 up
sudo ip link set dev sonic2 up
sudo ip link set dev sonic_mgmt up

sudo ip link set dev veth_sonic1 up
sudo ip link set dev veth_sonic2 up
sudo ip link set dev veth_sonic_mgmt up

sudo ip addr add 10.0.0.2/24 dev sonic_mgmt
sudo ip addr add 20.0.0.1/24 dev sonic1
sudo ip addr add 30.0.0.1/24 dev sonic2

sudo ip link set dev sonic-br up
sudo ip addr add 192.168.1.2/24 dev sonic-br
sudo ip route add 0.0.0.0/0 via 192.168.1.1 metric 255
sudo iptables -A FORWARD -i sonic-br -o sonic-br -j ACCEPT

```

After the SONiC VM starts running the bridge shows entries of the interfaces connected to VM.

```
$ brctl show sonic-br
bridge name     bridge id               STP enabled     interfaces
sonic-br                8000.1e208661f9ec       no              sonic-tap1
                                                        sonic-tap2
                                                        tap0
                                                        veth_sonic1
                                                        veth_sonic2
                                                        veth_sonic_mgmt
```