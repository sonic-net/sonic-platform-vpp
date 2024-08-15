# Copyright 2016 Microsoft, Inc.
# Modifications copyright (c) 2023 Cisco and/or its affiliates.
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

# sairedis package

LIBSAIREDIS_VERSION = 1.0.0
LIBSAIREDIS_NAME = libsairedis

LIBSAIREDIS = $(LIBSAIREDIS_NAME)_$(LIBSAIREDIS_VERSION)_$(CONFIGURED_ARCH).deb
$(LIBSAIREDIS)_SRC_PATH = $(SRC_PATH)/sonic-sairedis
$(LIBSAIREDIS)_VERSION = $(LIBSAIREDIS_VERSION)
$(LIBSAIREDIS)_NAME = $(LIBSAIREDIS_NAME)
$(LIBSAIREDIS)_DEPENDS += $(LIBSWSSCOMMON_DEV) $(VPP_MAIN) $(VPP_PLUGIN_CORE) $(VPP_PLUGIN_DPDK) \
	$(VPP_PLUGIN_DEV) $(VPP_DEV) $(VPPINFRA_DEV) $(VPPDBG) 
$(LIBSAIREDIS)_RDEPENDS += $(LIBSWSSCOMMON)
$(LIBSAIREDIS)_DEB_BUILD_OPTIONS = nocheck
SONIC_DPKG_DEBS += $(LIBSAIREDIS)

LIBSAIREDIS_DEV = $(LIBSAIREDIS_NAME)-dev_$(LIBSAIREDIS_VERSION)_$(CONFIGURED_ARCH).deb
$(eval $(call add_derived_package,$(LIBSAIREDIS),$(LIBSAIREDIS_DEV)))

LIBSAIVPP = libsaivpp_$(LIBSAIREDIS_VERSION)_$(CONFIGURED_ARCH).deb
$(eval $(call add_derived_package,$(LIBSAIREDIS),$(LIBSAIVPP)))
$(LIBSAIVPP)_DEPENDS += $(VPP_MAIN) $(VPP_PLUGIN_CORE) $(VPP_PLUGIN_DPDK) \
	$(VPP_PLUGIN_DEV) $(VPP_DEV) $(VPPINFRA_DEV) $(VPPDBG) 
$(LIBSAIVPP)_RDEPENDS += $(VPP_MAIN) $(VPP_PLUGIN_CORE) $(VPP_PLUGIN_DPDK) \
	$(VPP_PLUGIN_DEV) $(VPP_DEV) $(VPPINFRA_DEV) $(VPPDBG) 

LIBSAIVPP_DEV = libsaivpp-dev_$(LIBSAIREDIS_VERSION)_$(CONFIGURED_ARCH).deb
$(eval $(call add_derived_package,$(LIBSAIREDIS),$(LIBSAIVPP_DEV)))
$(LIBSAIVPP_DEV)_DEPENDS += $(LIBSAIVPP)

LIBSAIMETADATA = libsaimetadata_$(LIBSAIREDIS_VERSION)_$(CONFIGURED_ARCH).deb
$(eval $(call add_derived_package,$(LIBSAIREDIS),$(LIBSAIMETADATA)))

LIBSAIMETADATA_DEV = libsaimetadata-dev_$(LIBSAIREDIS_VERSION)_$(CONFIGURED_ARCH).deb
$(LIBSAIMETADATA_DEV)_DEPENDS += $(LIBSAIMETADATA)
$(eval $(call add_derived_package,$(LIBSAIREDIS),$(LIBSAIMETADATA_DEV)))

LIBSAIREDIS_DBGSYM = $(LIBSAIREDIS_NAME)-dbgsym_$(LIBSAIREDIS_VERSION)_$(CONFIGURED_ARCH).deb
$(LIBSAIREDIS_DBGSYM)_DEPENDS += $(LIBSAIREDIS)
$(LIBSAIREDIS_DBGSYM)_RDEPENDS += $(LIBSAIREDIS)
$(eval $(call add_derived_package,$(LIBSAIREDIS),$(LIBSAIREDIS_DBGSYM)))

LIBSAIVPP_DBGSYM = libsaivpp-dbgsym_$(LIBSAIREDIS_VERSION)_$(CONFIGURED_ARCH).deb
$(LIBSAIVPP_DBGSYM)_DEPENDS += $(LIBSAIVPP)
$(LIBSAIVPP_DBGSYM)_RDEPENDS += $(LIBSAIVPP)
$(eval $(call add_derived_package,$(LIBSAIREDIS),$(LIBSAIVPP_DBGSYM)))

LIBSAIMETADATA_DBGSYM = libsaimetadata-dbgsym_$(LIBSAIREDIS_VERSION)_$(CONFIGURED_ARCH).deb
$(LIBSAIMETADATA_DBGSYM)_DEPENDS += $(LIBSAIMETADATA)
$(LIBSAIMETADATA_DBGSYM)_RDEPENDS += $(LIBSAIMETADATA)
$(eval $(call add_derived_package,$(LIBSAIREDIS),$(LIBSAIMETADATA_DBGSYM)))

ifeq ($(ENABLE_PY2_MODULES), n)
    $(LIBSAIREDIS)_DEB_BUILD_PROFILES += nopython2
endif

# The .c, .cpp, .h & .hpp files under src/{$DBG_SRC_ARCHIVE list}
# are archived into debug one image to facilitate debugging.
#
DBG_SRC_ARCHIVE += sonic-sairedis
