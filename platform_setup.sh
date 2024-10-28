#!/bin/bash
sed -i -e 's/vs/vpp/g' /sonic/device/virtual/x86_64-kvm_x86_64-r0/platform_asic

# copy VPP sai.profile to the sonic-device-data package (Or permanently add sai.vpp_profile to sonic-device-data and change syncd script)
sed -i -e 's/_VS_/_VPP_/g' /sonic/src/sonic-device-data/src/sai.vs_profile
