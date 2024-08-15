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
VPP_SHA =
VPP_LAG = 21
USER = $(shell id -un)
UID = $(shell id -u)
GUID = $(shell id -g)

VPP_REPO_DIR = repo
MAIN_TARGET = $(VPPINFRA)
DERIVED_TARGETS = $(VPP_MAIN) $(VPP_PLUGIN_CORE) $(VPP_PLUGIN_DPDK) \
				  $(VPP_PLUGIN_DEV) $(VPP_DEV) $(VPPINFRA_DEV) $(VPPDBG)

VPP_DOCKER_DEBS = libnl-3-200_$(LIBNL3_VERSION)_$(CONFIGURED_ARCH).deb \
                  libnl-3-dev_$(LIBNL3_VERSION)_$(CONFIGURED_ARCH).deb \
				  libnl-route-3-200_$(LIBNL3_VERSION)_$(CONFIGURED_ARCH).deb \
				  libnl-route-3-dev_$(LIBNL3_VERSION)_$(CONFIGURED_ARCH).deb

VPP_DOCKER_BUILD = docker build --no-cache \
		    -t vppbld_$(BLDENV):latest \
		    --build-arg http_proxy=$(http_proxy) \
		    --build-arg https_proxy=$(https_proxy) \
		    --build-arg no_proxy=$(no_proxy) \
			--build-arg user=$(USER) \
			--build-arg uid=$(UID) \
			--build-arg guid=$(GUID) \
			-f ./Dockerfile-$(BLDENV) .

VPP_DOCKER_RUN = docker run --rm=true \
			-v /sonic/platform/vpp/vppbld/repo:/vpp \
    		-e "http_proxy=$(http_proxy)" \
    		-e "https_proxy=$(https_proxy)" \
    		-e "no_proxy=$(no_proxy)" \
			--network=host \
			vppbld_$(BLDENV):latest

VPP_DOCKER_RUN_IT = docker run -it --rm=true \
			-v /sonic/platform/vpp/vppbld/repo:/vpp \
    		-e "http_proxy=$(http_proxy)" \
    		-e "https_proxy=$(https_proxy)" \
    		-e "no_proxy=$(no_proxy)" \
			--network=host \
			vppbld_$(BLDENV):latest bash

docker_shell:
	$(VPP_DOCKER_RUN_IT)

build_vpp:
	 $(VPP_DOCKER_RUN) /run_build.sh

.PHONY: repo_clone
repo_clone:
	rm -rf $(VPP_REPO_DIR)
	git clone $(VPP_URL) $(VPP_REPO_DIR)
	pushd $(VPP_REPO_DIR)
	@VPP_SHA=`git log --until "$(VPP_LAG) days ago" -1 --format="%h"`
	git checkout $$VPP_SHA
	# 1. use installed libnl from sonic. 
	# 2. use libbpf pulled by vpp
	git apply ../vpp.patch
	popd

vppbld-docker/Dockerfile-$(BLDENV): vppbld-docker/Dockerfile-$(BLDENV).j2
	export VPP_DOCKER_DEBS="$(VPP_DOCKER_DEBS)"
	j2 vppbld-docker/Dockerfile-$(BLDENV).j2 > vppbld-docker/Dockerfile-$(BLDENV)

.PHONY: docker_build
docker_build:  vppbld-docker/Dockerfile-$(BLDENV)
	pushd vppbld-docker
	rm -rf debs
	mkdir debs
	cp $(foreach target,$(VPP_DOCKER_DEBS),$(addprefix $(DEST)/,$(target))) debs/
	$(VPP_DOCKER_BUILD)
	rm -rf debs
	popd

$(addprefix $(DEST)/, $(MAIN_TARGET)): $(DEST)/% : docker_build repo_clone
	$(VPP_DOCKER_RUN) /run_build.sh
	pushd $(VPP_REPO_DIR)/build-root
	@for target in *_$(CONFIGURED_ARCH).deb; do \
		new_name=$$(echo $$target | sed -E 's/^([^_]+)_[^_]+_($(CONFIGURED_ARCH).deb)$$/\1_$(VPP_VERSION_FULL)_\2/')
		echo "$$target -> $$new_name"
		cp $$target $(DEST)/$$new_name
	done
	popd

$(addprefix $(DEST)/, $(DERIVED_TARGETS)): $(DEST)/% : $(DEST)/$(MAIN_TARGET)
