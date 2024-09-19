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

# libsaivpp package
LIBSAIVPP_VERSION = 1.0.0
LIBSAIVPP_NAME = libsaivpp
LIBSAIVPP = $(LIBSAIVPP_NAME)_$(LIBSAIVPP_VERSION)_$(CONFIGURED_ARCH).deb
$(LIBSAIVPP)_SRC_PATH = platform/vpp/saivpp
$(LIBSAIVPP)_VERSION = $(LIBSAIVPP_VERSION)
# It seems libsairedis is built without libsai in the dependencies. Can we still use meta from sairedis?
$(LIBSAIVPP)_DEPENDS += $(LIBSWSSCOMMON_DEV) $(LIBSAIMETADATA_DEV) $(LIBSAIREDIS_DEV) $(VPP_MAIN) $(VPP_PLUGIN_CORE) $(VPP_PLUGIN_DPDK) \
	$(VPP_PLUGIN_DEV) $(VPP_DEV) $(VPPINFRA_DEV) $(VPPDBG)
$(LIBSAIVPP)_RDEPENDS += $(LIBSWSSCOMMON) $(LIBSAIMETADATA) $(LIBSAIREDIS) $(VPP_MAIN) $(VPP_PLUGIN_CORE) $(VPP_PLUGIN_DPDK) \
	$(VPP_PLUGIN_DEV) $(VPP_DEV) $(VPPINFRA_DEV) $(VPPDBG)
$(LIBSAIVPP)_DEB_BUILD_OPTIONS = nocheck
SONIC_DPKG_DEBS += $(LIBSAIVPP)

LIBSAIVPP_DEV = $(LIBSAIVPP_NAME)-dev_$(LIBSAIVPP_VERSION)_$(CONFIGURED_ARCH).deb
$(eval $(call add_derived_package,$(LIBSAIVPP),$(LIBSAIVPP_DEV)))

