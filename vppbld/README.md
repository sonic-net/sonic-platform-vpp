## Introduction
vppbld Makefile tries to download the specified vpp debian packages from buildkite. If it is not found there, it then builds it locally by pulling source code and applying vpp.patch.

The version of vpp packages is specified in sonic-buildimage/rules/vpp.mk. When we build target/sonic-vpp.img.gz or libvppinfra, for example target/debs/bookworm/libvppinfra_2506-0.2+b1sonic1_amd64.deb, it will trigger above Makefile. 

If we need to create a new vpp debian packages, either we have a new patch or we are upgrading vpp base version, we can specify a new version in sonic-buildimage/rules/vpp.mk then run one of the above make target.