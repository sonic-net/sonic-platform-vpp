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
	# Apply patches from series file
	@while IFS= read -r line; do \
		case "$$line" in \
			''|\#*) continue ;; \
		esac; \
		echo "Applying patch: $$line"; \
		git apply ../patches/$$line; \
	done < ../patches/series
	popd

$(addprefix $(DEST)/, $(MAIN_TARGET)):
	@echo "Downloading all .deb files to $(DEST)..."
	@( \
		set -e; \
		$(foreach deb, $(ALL_TARGETS), \
			echo "Attempting to download $(DEST)/$(deb)..."; \
			curl -L -f -o $(DEST)/$(deb) $(VPP_DEB_URL)/$(deb); \
		) \
	) || (echo "One or more downloads failed..." && $(MAKE) build_locally -f Makefile)

build_locally: repo_clone
	pushd $(VPP_REPO_DIR)
	make UNATTENDED=y PLATFORM=vpp DPDK_MLX4_PMD=n DPDK_MLX5_PMD=n DPDK_MLX5_COMMON_PMD=n install-dep && make PLATFORM=vpp DPDK_MLX4_PMD=n DPDK_MLX5_PMD=n DPDK_MLX5_COMMON_PMD=n install-ext-deps && CUSTOM_VERSION=$(VPP_VERSION_SONIC) make UNATTENDED=y PLATFORM=vpp -j4 DPDK_MLX4_PMD=n DPDK_MLX5_PMD=n DPDK_MLX5_COMMON_PMD=n pkg-deb
	pushd build-root
	@for target in *_$(CONFIGURED_ARCH).deb; do \
		cp $$target $(DEST)/
	done
	popd

CONFIGURED_ARCH ?= amd64
CI_DEST ?= $(CURDIR)/artifacts
CI_BUILD_STAMP = $(CURDIR)/.ci-build.stamp
CI_TEST_JOBS ?= 4

# Anything that changes what gets built must invalidate the stamp.
CI_BUILD_INPUTS = vpp_version patches/series $(wildcard patches/*.patch) \
                  $(shell find $(VPP_CUSTOM_PLUGINS_DIR) -type f 2>/dev/null)

.PHONY: ci-build
ci-build: $(CI_BUILD_STAMP)

# Stamped so that a second invocation (e.g. from ci-test, which runs as a
# separate CI step) does not re-clone the repo and rebuild from scratch.
# Remove the stamp with "make ci-clean" to force a full rebuild.
$(CI_BUILD_STAMP): $(CI_BUILD_INPUTS)
	rm -f $(CI_BUILD_STAMP)
	$(MAKE) -f Makefile repo_clone
	mkdir -p $(CI_DEST)
	pushd $(VPP_REPO_DIR)
	make UNATTENDED=y PLATFORM=vpp DPDK_MLX4_PMD=n DPDK_MLX5_PMD=n DPDK_MLX5_COMMON_PMD=n install-dep && \
	make PLATFORM=vpp DPDK_MLX4_PMD=n DPDK_MLX5_PMD=n DPDK_MLX5_COMMON_PMD=n install-ext-deps && \
	make UNATTENDED=y PLATFORM=vpp -j4 DPDK_MLX4_PMD=n DPDK_MLX5_PMD=n DPDK_MLX5_COMMON_PMD=n pkg-deb
	pushd build-root
	@for target in *_$(CONFIGURED_ARCH).deb; do \
		cp $$target $(CI_DEST)/; \
	done
	popd
	popd
	touch $(CI_BUILD_STAMP)

# Dependencies needed only by "make test". tshark is deliberately dropped from
# VPP's DEB_DEPENDS by patches/0001-remove-libnl-and-tshark-dependencies.patch
# so that it is not pulled into the regular build, so install it here instead.
CI_TEST_DEPS ?= tshark

# Apt state is container state, not workspace state: ci-build may have been
# satisfied by the stamp from an earlier container (or an earlier CI step),
# in which case install-dep never ran here and the toolchain is missing.
# Always re-assert the build deps before testing; apt is a no-op when they
# are already present.
.PHONY: ci-test-deps
ci-test-deps: ci-build
	# Preseed so the wireshark-common postinst does not prompt.
	echo "wireshark-common wireshark-common/install-setuid boolean false" | \
		sudo debconf-set-selections
	sudo -E DEBIAN_FRONTEND=noninteractive apt-get update
	sudo -E DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $(CI_TEST_DEPS)
	pushd $(VPP_REPO_DIR)
	make UNATTENDED=y PLATFORM=vpp DPDK_MLX4_PMD=n DPDK_MLX5_PMD=n DPDK_MLX5_COMMON_PMD=n install-dep
	make PLATFORM=vpp DPDK_MLX4_PMD=n DPDK_MLX5_PMD=n DPDK_MLX5_COMMON_PMD=n install-ext-deps
	popd

# Where to leave logs from failed test cases so that CI can publish them.
#
# "make test" drops each failing test case's tempdir (log.txt, vpp_stderr.txt
# and the kept pcaps) into FAILED_DIR, but the entries there are symlinks into
# /tmp/vpp-unittest-*, which an artifact upload cannot follow.  Setting
# COMPRESS_FAILED_TEST_LOGS=yes together with WORKSPACE makes VPP's
# test/scripts/compress_failed.sh gzip those files and copy real (non-symlink)
# copies into $(WORKSPACE)/archives, which is publishable.  Both happen inside
# the "test" recipe itself, before it propagates the failure, so the logs
# survive even though .SHELLFLAGS has -e.
CI_TEST_LOGS ?= $(CURDIR)/test-logs

.PHONY: ci-test
ci-test: ci-build ci-test-deps
	# Start clean so a passing run cannot republish a previous run's failures.
	rm -rf $(CI_TEST_LOGS)
	mkdir -p $(CI_TEST_LOGS)
	pushd $(VPP_REPO_DIR)
	env WORKSPACE=$(CI_TEST_LOGS) \
		make UNATTENDED=y PLATFORM=vpp TEST_JOBS=$(CI_TEST_JOBS) \
		     COMPRESS_FAILED_TEST_LOGS=yes test
	popd

.PHONY: ci-clean
ci-clean:
	rm -f $(CI_BUILD_STAMP)
	rm -rf $(CI_DEST)
	rm -rf $(CI_TEST_LOGS)
