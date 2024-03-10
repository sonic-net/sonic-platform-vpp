## Some common queries
 * What features are supported now ?

   In Phase 0 below features are supported
   * Interface configuration, IP address configuration
   * IPv4,IPv6 routing, static routes, BGP routing protocol
   * sub-interfaces, VRF

   Please check the [TODO.md](TODO.md) on list of features to be implemented in the near future.

> Note - interface queuing, buffering, WRED, policing features not supported yet.

 * Why some of the config and show commands don't run in the single container SONiC-VPP ?

   The single container SONiC-VPP supports subset of functionality supported in the full SONiC image (VM image or hardware form factor image). Features like SNMP, telemetry, lldp, platform-monitoring are missing in single container image. Please use qemu-kvm VM SONiC-VPP image for full functionality.

 * Any configuration examples ?

   Please check some routing configuration examples given in the docs/README.xxx.md

 * /etc/sonic/config_db.json lists EthernetX interfaces with lanes values

   Please ignore the lanes in the configuration. The system does not have SERDES connected switch ports. This will be removed in the future fixes. BREAKOUT_CFG is not applicable for now so that also will be removed from configuration file.

 * What kind network ports supported ?

   Broadly Virtual interfaces and PCI based network interfaces are supported.
   * Virtual interfaces
     * veth pair interface, One is passed to the SONiC-VPP container and the other end is in the host
   * PCI based ports
     * 10G,25G,40G PCI-express based network ports
     * SR-IOV PF and VF ports

 * Packet IO stops after syncd or vpp processes crash ?

   Its a known limitation. The syncd does not yet support resyncing of configuration.
   The only way is to restart the container (in case of single container) or restart the VM (in case of VM image)

 * How to configure BGP ?

   Please check the [simple BGP configuration](docs/README.simple.bgp.md) document. SONiC seems to not support configuration of full BGP configuration. Configuration is only supported through vtysh if FRR is used.
