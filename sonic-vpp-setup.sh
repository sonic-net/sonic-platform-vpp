#!/bin/bash
# Copyright (c) 2023 Cisco and/or its affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at:
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -e

MKRULES_PATH=$PWD/platform/mkrules
SONIC_SRC=$MKRULES_PATH/src
SONIC_SAIREDIS=$SONIC_SRC/sonic-sairedis

# Link the vpplib and platform rules
cp -r $PWD/platform/vpp $PWD/build/sonic-buildimage/platform/
VPPLIBPATH=$PWD/platform/saivpp/vpplib

cd ./build/sonic-buildimage

#OLD_WORKING_LABEL=eba30ff26f
# Below is the build label information
# 254924	20230417.6	master	Azure.sonic-buildimage.official.vs	succeeded	2023-04-17T08:20:34	2023-04-17T13:39:10	183d0f2be7
WORKING_LABEL=183d0f2be7
SONIC_CHECKOUT_LABEL=${SONIC_CHECKOUT_LABEL:=$WORKING_LABEL}
git checkout $SONIC_CHECKOUT_LABEL

make init
#if [ "$http_proxy" != "" ]; then
#    make http_proxy=$http_proxy https_proxy=$https_proxy init
#else
#    make init
#fi
cp -r $VPPLIBPATH $PWD/src/sonic-sairedis/

# Replace vslib with vpplib which contains saivpp code.
sed -i -e 's/vslib/vpplib/g' ./src/sonic-sairedis/Makefile.am

#add vpp install flag
sed -i -e 's/dpkg -i/dpkg -E -i/g' ./build_debug_docker_j2.sh

#Fix the broken libnuma1 debian pkg installation problem during sonic-vpp.img creation
sed -i -e 's/apt-get install -y net-tools/apt-get -o "APT::Get::Fix-Broken=false" install -y net-tools/g' ./scripts/build_kvm_image.sh

# Change the Virtual platform to vpp. Otherwise the ONIE kvm install fails with error
# "The image you're trying to install is of a different ASIC type as the running platform's ASIC"
sed -i -e 's/vs/vpp/g' ./device/virtual/x86_64-kvm_x86_64-r0/platform_asic

# vpp specific syncd container start configuration change
git apply $MKRULES_PATH/files/build_templates/docker_image_ctl.j2.patch
git apply $MKRULES_PATH/files/build_templates/per_namespace/syncd.service.j2.patch
git apply $MKRULES_PATH/files/build_templates/sonic_debian_extension.j2.patch

# copy VPP sai.profile to the sonic-device-data package
cp $MKRULES_PATH/files/sai.profile ./src/sonic-device-data/src/sai.vs_profile

# Copy the sonic-vpp configuration scripts
cp $MKRULES_PATH/files/scripts/sonic_vpp_cfg.sh ./files/scripts/sonic_vpp_cfg.sh
cp $MKRULES_PATH/files/scripts/vpp_ports_setup.sh ./files/scripts/vpp_ports_setup.sh

# Setup the swss docker file template to ignore the vpp sysctl during installation
cp $MKRULES_PATH/dockers/docker-swss-layer-buster/Dockerfile.j2 ./dockers/docker-swss-layer-buster/Dockerfile.j2

# sairedis and swss explicitly use saivs in rules/*.mk. Replace saivs with saivpp
cp $MKRULES_PATH/rules/sairedis.mk ./rules/sairedis.mk
cp $MKRULES_PATH/rules/swss.mk  ./rules/swss.mk
cp $MKRULES_PATH/rules/vpp.mk ./rules/vpp.mk
cp $MKRULES_PATH/rules/vpp.dep  ./rules/vpp.dep

#Configure.ac explicitly uses LIBSAIVS. Use LIBSAIVPP instead.
cp $SONIC_SAIREDIS/configure.ac.vpp ./src/sonic-sairedis/configure.ac

#Replace the saivs debian package config/scripts with vpp ones
mv ./src/sonic-sairedis/debian ./src/sonic-sairedis/debian-vs
cp -r $SONIC_SAIREDIS/debian ./src/sonic-sairedis/debian

# Sets up args to syncd for the vpp platform
cp $SONIC_SAIREDIS/syncd/scripts/syncd_init_common.sh ./src/sonic-sairedis/syncd/scripts/syncd_init_common.sh

# Add saivpp lib instead of saivs
cp $SONIC_SAIREDIS/saidiscovery/Makefile.am ./src/sonic-sairedis/saidiscovery/Makefile.am
cp $SONIC_SAIREDIS/syncd/Makefile.am ./src/sonic-sairedis/syncd/Makefile.am
cp $SONIC_SAIREDIS/saisdkdump/Makefile.am ./src/sonic-sairedis/saisdkdump/Makefile.am
cp $SONIC_SAIREDIS/tests/Makefile.am ./src/sonic-sairedis/tests/Makefile.am
cp $SONIC_SAIREDIS/unittest/Makefile.am ./src/sonic-sairedis/unittest/Makefile.am
cp $SONIC_SAIREDIS/unittest/vslib/Makefile.am ./src/sonic-sairedis/unittest/vslib/Makefile.am
cp $SONIC_SAIREDIS/pyext/py3/Makefile.am ./src/sonic-sairedis/pyext/py3/Makefile.am

# Linking error with saivpp. Disable mock_tests for swss for now. Fix it later
# Also fix the fpmsync to configure routes in APPL_DB from the default routing table.
# https://github.com/sonic-net/sonic-swss/issues/2746

pushd . ; cd ./src/sonic-swss; git apply  $SONIC_SRC/sonic-swss/swss.patch; popd

