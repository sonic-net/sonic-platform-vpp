# SONiC VPP Deployment Automation

This repository contains scripts to automate build, deployment and validation of a SONiC VPP image. 

> Note - The current implementation uses Ansible. This will be deprecated soon in favor of PTF. 

## Prerequisites

 * VM running Ubuntu 20.4 release
 * Install pip, jinja, ansible, sshpass on VM

```
sudo apt install -y python3-pip
sudo pip3 install j2cli
sudo python3 -m pip install ansible
sudo apt-get install -y sshpass
sudo apt-get install -y expect
```
 * Install [Docker](https://docs.docker.com/engine/install/) and configure your system to allow running the 'docker' command without 'sudo':
    * Add current user to the docker group: `sudo gpasswd -a ${USER} docker`
    * Log out and log back in so that your group membership is re-evaluated

## SONiC VPP Docker image Deployment and Validation

Perform following steps to deploy and validate SONiC VPP docker image.
1. Downalod SONiC-VPP docker image ("docker-sonic-vpp.gz") and script "start_sonic_vpp.sh" to VM in directory "/home/\<user-name\>/sonic-vpp". Script "start_sonic_vpp.sh" is available at https://github.com/sonic-net/sonic-platform-vpp/blob/main/start_sonic_vpp.sh
2. Clone repository https://github.com/sonic-net/sonic-platform-vpp
   ```
    git clone https://github.com/sonic-net/sonic-platform-vpp 
   ```
   Repository contains python script validate-sonic-vpp-docker-routing.py and two playbooks:

    sonic-sw-dp-bring-up.yaml:  Bring up SONiC-VPP container, set up validation topology and perform ping test. 
    sonic-sw-dp-bring-down.yaml: Tear down topology and stop SONiC-VPP container to bring VM into original state.
    
   
3. From 'sonic-platform-vpp/tests' directory, run python script validate-sonic-vpp-docker-routing.py using following arguments to deploy and validate image with single-hop topology:
   ```
    python validate-sonic-vpp-docker-routing.py --topology singleHop --host <VM IP Address> --username <SSH Username> --password <SSH User Password>
   ```
4. From 'sonic-platform-vpp/tests' directory, run python script validate-sonic-vpp-docker-routing.py using following arguments to deploy and validate image with multi-hop topology:
   ```
    python validate-sonic-vpp-docker-routing.py --topology multiHop --host <VM IP Address> --username <SSH Username> --password <SSH User Password>
   ```
   For multiHop topology, BGP routing protocol is used by default. In order to test multiHop topology with static route, use option `--routing static`.
   Option `--routing` support both static route and BGP routing protocol (`--routing <static | bgp>`)
   
> Note : 
  * Python script can be executed on your local machine (i.e Mac) or directly on VM (Where SONIC-VPP container is being deployed for validation).
  * Run "python validate-sonic-vpp-docker-routing.py -h" to see additional optional argumnets. Additional arguments allows you to overwrite defualt IP Addresses being used to setup routing topology.

## SONiC VPP VM image Deployment and Validation

Perform following steps to deploy and validate SONiC VPP VM image.
1. Downalod SONiC-VPP VM image ("sonic-vpp.img.gz") and script "sonic_vpp_cfg.sh" to VM in directory "/home/\<user-name\>/sonic-vpp". Script "sonic_vpp_cfg.sh" is available at https://github.com/sonic-net/sonic-platform-vpp/blob/main/platform/mkrules/files/scripts/sonic_vpp_cfg.sh. For VM VxLAN test, download the VxLAN file tests/vxlan-configs/vnet_tunnel.json.j2 to "/home/\<user-name\>/sonic-vpp" as well.

2. Clone repository https://github.com/sonic-net/sonic-platform-vpp 
   ```
    git clone https://github.com/sonic-net/sonic-platform-vpp 
   ```
   Repository contains python script validate-sonic-vpp-vm-routing.py, validate-sonic-vpp-vm-vxlan-routing.py, and playbooks:

    sonic-vpp-vm-bring-up.yaml:  Bring/tear up/down SONiC-VPP VM, set up validation topology and perform ping test. 
    sonic-vpp-vm-vxlan-bring-up.yaml:  Bring/tear up/down SONiC-VPP VM, set up validation topology and VxLAN tunnels and perform ping test.
    
   
3. From 'sonic-platform-vpp/tests' directory, run python script validate-sonic-vpp-vm-routing.py using following arguments to deploy and validate image for multi-hop topology with BGP routing:
   ```
    python validate-sonic-vpp-vm-routing.py --host <VM IP Address> --username <SSH Username> --password <SSH User Password>
   ```

4. From 'sonic-platform-vpp/tests' directory, run python script validate-sonic-vpp-vm-vxlan-routing.py using following arguments to deploy and validate VxLAN encap and decap for multi-hop topology:
   ```
    python validate-sonic-vpp-vm-vxlan-routing.py --host <VM IP Address> --username <SSH Username> --password <SSH User Password>
   ```
> Note : 
  * Python script can be executed on your local machine (i.e Mac) or directly on VM (Where SONIC-VPP VM is being deployed for validation). On your Ubuntu VM, need to enable Hardware virtualization on ESXi host running Ubuntu VM. On ESXI host, Edit the VM configuration in the VMware, select CPU section. You will see “Expose hardware assisted virtualization to the guest OS” , press the tick box and save the configuration. Reboot you VM. 

  * Run "python validate-sonic-vpp-vm-routing.py -h" to see additional optional argumnets. Additional arguments allows you to overwrite defualt IP Addresses being used to setup routing topology.

