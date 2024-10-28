# Description
SONiC-VPP is a platform under SONIC that supports VPP data plane. For more details about VPP see [What is VPP](https://s3-docs.fd.io/vpp/23.06/) 

This repo contains  scripts, Makefiles, configuration file, Docker files etc to build a SONIC-VPP image. It also pulls some extra git repos and makes these repos part of SONiC build. In this process it will prompt for user/password while cloning. There are some VPP platform specific Makefile, config files which are applied to the sonic-buildimage repo to facilitate the successful build of the SONiC-VPP image.

> Note: The first build takes good amount of time. The SONiC image build has two parts to it, [backend and frontend](https://github.com/sonic-net/sonic-buildimage/blob/master/README.buildsystem.md). The backend consumes a lot of time when the build is done first time. The follow on builds only do the frontend.

**Caveat**- *The documentation for this project is a work in progress and a bit terse. We hope to address this in coming weeks.* 

# Prerequisites
 * Linux host machine running ubuntu 18.x or later distribution (Ubuntu 20.x is preferred).
 * Install the following packages 

```
sudo apt-get install -y make automake autoconf
sudo apt install -y python3-pip
sudo pip3 install j2cli
```

 * Install [Docker](https://docs.docker.com/engine/install/) and configure your system to allow running the 'docker' command without 'sudo':
    * Add current user to the docker group: `sudo gpasswd -a ${USER} docker`
    * Log out and log back in so that your group membership is re-evaluated

# SONiC VPP image build
There are two types of sonic-vpp image build targets supported
 * Single docker container image (*docker-sonic-vpp.gz*) with a subset of SONIC features
 * Comprehensive SONiC-VPP qemu VM image

## Building a single container image

1. git clone --recurse-submodules https://github.com/sonic-net/sonic-buildimage.git
2. git submodule add https://github.com/sonic-platform-vpp.git platform/vpp
3. Apply the diff in https://github.com/sonic-net/sonic-sairedis/pull/1424 if the PR hasn't been merged.
4. make init
5. make configure PLATFORM=vpp
6. NOBULLSEYE=1 NOBUSTER=1 make SONIC_BUILD_JOBS=4 target/docker-sonic-vpp.gz
Note: this is not tested and could be broken, which will be fixed later. In the meantime, use branch prior-to-build-change for single container image build.

### Testing the single container image

Refer to the [Getting started](docs/README.getting-started.md) document in docs directory for details. 


## Building a KVM VM image 
1. git clone --recurse-submodules https://github.com/sonic-net/sonic-buildimage.git
2. git submodule add https://github.com/sonic-platform-vpp.git platform/vpp
3. Apply the diff in https://github.com/sonic-net/sonic-sairedis/pull/1424 if the PR hasn't been merged.
4. make init
5. make configure PLATFORM=vpp
6. NOBULLSEYE=1 NOBUSTER=1 make SONIC_BUILD_JOBS=4 target/sonic-vpp.img.gz
Note: the first 3 steps are needed until the PR to upstream sonic is merged. 
### Testing the qemu VM image

Refer to the [document](docs/README.sonic_vm.md) in docs directory for details. 


# Troubleshooting

http(s)_proxy driven make fails sometimes with package download failure. It says hash mismatch. This may be caused
by some bug in the proxy which corrupts some bytes or downloaded package is incomplete. The fix is to run the
make sonic_build again.

```
E: Failed to fetch http://debian-archive.trafficmanager.net/debian/pool/main/p/perl/libperl5.32_5.32.1-4%2bdeb11u2_amd64.deb  Hash Sum mismatch
   Hashes of expected file:
    - SHA256:224cafe65968deb83168113b74dff2d2f13b115a41d99eb209ed3b8f981df0b3
    - MD5Sum:4a5dab70f461660deab6fd03b2f991f6 [weak]
    - Filesize:4106072 [weak]
    - SHA512:6f65c014dc3d71538eeff28499e857315b001edf34b7d27f9ba63ba4ae84296b23a03cf4d3684f048d2cf3ae6591f961e808041015c8cbf87993b37fceae21fc
    - SHA1:70b59e8890820d4b278dee5bea5d6c09e94a5c6a [weak]
   Hashes of received file:
    - SHA512:62f8a2073aab62c19e5233c527057365e928d4c27eb37e0cb5081fd2982cda265ff7088baf4539af754333fa0625370e1b38e02715fba056e1f8bad1d589c2b1
    - SHA256:f7b2db46905d4209ffea09a103f8a1b6e836075a547d1cd173ea49a12ce2f6d0
    - SHA1:988a27690b32fafa9bed3a20574554bd27599e71 [weak]
    - MD5Sum:0816cefaa91ea671925de07760480d86 [weak]
    - Filesize:4106072 [weak]
   Last modification reported: Thu, 30 Sep 2021 21:42:11 +0000
E: Unable to fetch some archives, maybe run apt-get update or try with --fix-missing?

```
