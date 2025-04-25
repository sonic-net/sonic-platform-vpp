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

VPP_URL = https://gerrit.fd.io/r/vpp
VPP_DEB_URL = https://packages.buildkite.com/sonic-vpp/vpp/any/pool/any/main/v/vpp
USER = $(shell id -un)
UID = $(shell id -u)
GUID = $(shell id -g)

VPP_REPO_DIR = repo
VPP_CUSTOM_PLUGINS_DIR = plugins
MAIN_TARGET = $(VPPINFRA)
DERIVED_TARGETS = $(VPP_MAIN) $(VPP_PLUGIN_CORE) $(VPP_PLUGIN_DPDK) \
				  $(VPP_PLUGIN_DEV) $(VPP_DEV) $(VPPINFRA_DEV) $(VPPDBG)
ALL_TARGETS = $(MAIN_TARGET) $(DERIVED_TARGETS)
.PHONY: repo_clone build_locally
repo_clone:
	rm -rf $(VPP_REPO_DIR)
	git clone $(VPP_URL) $(VPP_REPO_DIR)
	cp -r $(VPP_CUSTOM_PLUGINS_DIR)/* $(VPP_REPO_DIR)/src/plugins/
	pushd $(VPP_REPO_DIR)
	@VPP_VERSION=`cat ../vpp_version`
	git checkout $$VPP_VERSION
	# 1. use installed libnl from sonic. 
	# 2. use libbpf pulled by vpp
	# 3. export vxlan main
	git apply ../vpp.patch
	popd

$(addprefix $(DEST)/, $(MAIN_TARGET)):
	@echo "Downloading all .deb files to $(DEST)..."
	@( \
		set -e; \
		$(foreach deb, $(ALL_TARGETS), \
			echo "Attempting to download $(DEST)/$(deb)..."; \
			curl -f -o $(DEST)/$(deb) $(VPP_DEB_URL)/$(deb); \
		) \
	) || (echo "One or more downloads failed..." && $(MAKE) build_locally -f Makefile)

build_locally: repo_clone
	pushd $(VPP_REPO_DIR)
	make UNATTENDED=y PLATFORM=vpp install-dep && CUSTOM_VERSION=$(VPP_VERSION_SONIC) make UNATTENDED=y PLATFORM=vpp -j4 pkg-deb
	pushd build-root
	@for target in *_$(CONFIGURED_ARCH).deb; do \
		cp $$target $(DEST)/
	done
	popd

$(addprefix $(DEST)/, $(DERIVED_TARGETS)): $(DEST)/% : $(DEST)/$(MAIN_TARGET)
