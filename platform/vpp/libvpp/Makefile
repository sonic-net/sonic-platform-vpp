#  Copyright (c) 2024 Cisco and/or its affiliates.
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at:

#      http://www.apache.org/licenses/LICENSE-2.0

#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.

.ONESHELL:
SHELL = /bin/bash
.SHELLFLAGS += -e

MAIN_TARGET = $(VPPINFRA)
DERIVED_TARGETS = $(VPP_MAIN) $(VPP_PLUGIN_CORE) $(VPP_PLUGIN_DPDK) \
				  $(VPP_PLUGIN_DEV) $(VPP_DEV) $(VPPINFRA_DEV) $(VPPDBG)

$(addprefix $(DEST)/, $(MAIN_TARGET)): $(DEST)/% :
	# Obtaining the vpp
	rm -rf $(VPP_REPO_DIR)
	git clone https://gerrit.fd.io/r/vpp $(VPP_REPO_DIR)
	# optionally checking out a specific tag/commit
	pushd $(VPP_REPO_DIR)
	echo 'v1.0-0-sonic' > src/scripts/.version
	# 1. use installed libnl from sonic. 
	# 2. use libbpf pulled by vpp
	git apply ../vpp.patch
	make UNATTENDED=y PLATFORM=vpp -j$(SONIC_CONFIG_MAKE_JOBS) install-dep
	make UNATTENDED=y PLATFORM=vpp -j$(SONIC_CONFIG_MAKE_JOBS) build
	make UNATTENDED=y PLATFORM=vpp -j$(SONIC_CONFIG_MAKE_JOBS) pkg-deb
	mv $(foreach target,$(DERIVED_TARGETS) $(MAIN_TARGET),$(addprefix build-root/,$(target))) $(DEST)/
	popd

$(addprefix $(DEST)/, $(DERIVED_TARGETS)): $(DEST)/% : $(DEST)/$(MAIN_TARGET)
