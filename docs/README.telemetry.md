# SONiC-VPP telemetry
Telemetry is only supported in sonic-vpp qemu-kvm VM image. The telemetry implementation details are [here](https://github.com/sonic-net/sonic-gnmi).
configuration parameters for telemetry are documented [here](https://github.com/sonic-net/sonic-utilities/blob/master/doc/Command-Reference.md).

telemetry configuration requires TLS certificates for its operation. To generate the require certificates follow below procedure.

Download the script [telemetry_cert.gen.sh](https://github.com/sonic-net/sonic-platform-vpp/scripts/telemetry_cert.gen.sh)
the running SONiC-VPP VM and generate private keys and certificates for telemetry.

```
chmod +x telemetry_cert.gen.sh
sudo DNS_NAME=your-sonic-vpp-vm-dns-name ./telemetry_cert.gen.sh
```

Now restart the telemetry container
```
sudo systemctl restart telemetry
```

Check telemetry process is running
```
show feature status

ps -ax | grep telemetry
```