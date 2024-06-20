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

.PHONY: vpp sonic init sonic_vm reset

build/.sonic_vpp_repo:
	mkdir -p ./build
	[ -d ./build/sonic-buildimage ] || git clone https://github.com/sonic-net/sonic-buildimage.git ./build/sonic-buildimage
	touch build/.sonic_vpp_repo

sonic_repo: build/.sonic_vpp_repo

build/.sonic_vpp_setup:
	./sonic-vpp-setup.sh && touch build/.sonic_vpp_setup

sonic_setup: build/.sonic_vpp_setup

build/.sonic_vpp_config:
	make -C ./build/sonic-buildimage PLATFORM=vpp configure && touch build/.sonic_vpp_config

sonic_config: build/.sonic_vpp_config

build/sonic-buildimage/target/docker-sonic-vpp.gz:
	make -C ./build/sonic-buildimage SONIC_BUILD_JOBS=4 target/docker-sonic-vpp.gz  BLDENV=bullseye NOBUSTER=1

sonic_build: build/sonic-buildimage/target/docker-sonic-vpp.gz

build/sonic-buildimage/target/sonic-vpp.img.gz:
	make -C ./build/sonic-buildimage SONIC_BUILD_JOBS=4 target/sonic-vpp.img.gz BLDENV=bullseye NOBUSTER=1

sonic_build_vm: build/sonic-buildimage/target/sonic-vpp.img.gz

sonic: sonic_repo sonic_setup sonic_config sonic_build

sonic_vm: sonic sonic_build_vm

init: sonic_repo sonic_setup

clean_syncd_dependencies:
	 make -C ./build/sonic-buildimage SONIC_BUILD_JOBS=4 target/docker-sonic-vpp.gz-clean BLDENV=bullseye NOBUSTER=1
	 make -C ./build/sonic-buildimage SONIC_BUILD_JOBS=4 target/debs/bullseye/libsaivpp_1.0.0_amd64.deb-clean BLDENV=bullseye NOBUSTER=1
	 make -C ./build/sonic-buildimage SONIC_BUILD_JOBS=4 target/debs/bullseye/vpp_23.06-release_amd64.deb-clean BLDENV=bullseye NOBUSTER=1

vpp_build:
	mkdir -p ./build
	[ -d ./build/vpp-dataplane/.git ] || git clone https://github.com/projectcalico/vpp-dataplane.git ./build/vpp-dataplane
	cp  ./vpp/vpp-manager/Makefile ./build/vpp-dataplane/vpp-manager/Makefile
	cd ./build/vpp-dataplane/vpp-manager && make DOCKER_BUILD_PROXY=http://proxy-wsa.esl.cisco.com:80 vpp

vpp: vpp_build

reset:
	make -C ./build/sonic-buildimage PLATFORM=vpp SONIC_BUILD_JOBS=4 reset
	rm -f ./build/.sonic_vpp_setup
	rm -f ./build/.sonic_vpp_config

all: sonic
