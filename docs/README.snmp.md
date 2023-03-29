# SONiC-VPP SNMP Howto use
SNMP is only supported in the SONiC-VPP VM qemu-kvm image. It is enabled by default. The default configuration of SNMP lets readonly access of system mibs. All that user has to do is install snmp manager on a host and run query to the SONIC-VPP VM. In the below commands 10.0.0.1 is the address of the SONiC-VPP VM running SNMP.

```
sudo apt-get install -y snmp
snmpwalk -Os -c public -v 1 10.0.0.1
```

More configuration details of SNMP are found [here](https://github.com/sonic-net/sonic-utilities/blob/master/doc/Command-Reference.md#snmp).