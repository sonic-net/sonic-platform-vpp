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

# libvpp package

VPP_VERSION = 24.06
VPP_VERSION_FULL = 24.06-release
VPP_PKG_SERVER = "https://packagecloud.io/fdio/release/packages/debian/$(BLDENV)"
VPP_DIST_VER="/download.deb?distro_version_id=207"


VPPINFRA = libvppinfra_$(VPP_VERSION_FULL)_$(CONFIGURED_ARCH).deb
$(VPPINFRA)_URL = $(VPP_PKG_SERVER)/libvppinfra_$(VPP_VERSION_FULL)_$(CONFIGURED_ARCH).deb$(VPP_DIST_VER)
SONIC_ONLINE_DEBS += $(VPPINFRA)


VPP_MAIN = vpp_$(VPP_VERSION_FULL)_$(CONFIGURED_ARCH).deb
$(VPP_MAIN)_URL = $(VPP_PKG_SERVER)/vpp_$(VPP_VERSION_FULL)_$(CONFIGURED_ARCH).deb$(VPP_DIST_VER)
SONIC_ONLINE_DEBS += $(VPP_MAIN)
$(VPP_MAIN)_DEPENDS += $(VPPINFRA)
$(VPP_MAIN)_RDEPENDS += $(VPPINFRA)


LIBNUMA := $(shell apt-get download --print-uris libnuma1 | cut -d " " -f2)
$(LIBNUMA)_URL := $(shell apt-get download --print-uris libnuma1 | cut -d " " -f1)
SONIC_ONLINE_DEBS += $(LIBNUMA)


LIBMBEDCRYPTO3 := $(shell apt-get download --print-uris libmbedcrypto3 | cut -d " " -f2)
$(LIBMBEDCRYPTO3)_URL := $(shell apt-get download --print-uris libmbedcrypto3 | cut -d " " -f1)
SONIC_ONLINE_DEBS += $(LIBMBEDCRYPTO3)


LIBMBEDX509 := $(shell apt-get download --print-uris libmbedx509-0 | cut -d " " -f2)
$(LIBMBEDX509)_URL := $(shell apt-get download --print-uris libmbedx509-0 | cut -d " " -f1)
SONIC_ONLINE_DEBS += $(LIBMBEDX509)
$(LIBMBEDX509)_DEPENDS += $(LIBMBEDCRYPTO3)
$(LIBMBEDX509)_RDEPENDS += $(LIBMBEDCRYPTO3)


LIBMBEDTLS12 := $(shell apt-get download --print-uris libmbedtls12 | cut -d " " -f2)
$(LIBMBEDTLS12)_URL := $(shell apt-get download --print-uris libmbedtls12 | cut -d " " -f1)
SONIC_ONLINE_DEBS += $(LIBMBEDTLS12)
$(LIBMBEDTLS12)_DEPENDS += $(LIBMBEDCRYPTO3) $(LIBMBEDX509)
$(LIBMBEDTLS12)_RDEPENDS += $(LIBMBEDCRYPTO3) $(LIBMBEDX509)


VPP_PLUGIN_CORE = vpp-plugin-core_$(VPP_VERSION_FULL)_$(CONFIGURED_ARCH).deb
$(VPP_PLUGIN_CORE)_URL = $(VPP_PKG_SERVER)/vpp-plugin-core_$(VPP_VERSION_FULL)_$(CONFIGURED_ARCH).deb$(VPP_DIST_VER)
SONIC_ONLINE_DEBS += $(VPP_PLUGIN_CORE)
$(VPP_PLUGIN_CORE)_DEPENDS += $(LIBMBEDCRYPTO3) $(LIBMBEDX509) $(LIBMBEDTLS12) $(VPP_MAIN)
$(VPP_PLUGIN_CORE)_RDEPENDS += $(LIBMBEDCRYPTO3) $(LIBMBEDX509) $(LIBMBEDTLS12) $(VPP_MAIN)


VPP_PLUGIN_DPDK = vpp-plugin-dpdk_$(VPP_VERSION_FULL)_$(CONFIGURED_ARCH).deb
$(VPP_PLUGIN_DPDK)_URL = $(VPP_PKG_SERVER)/vpp-plugin-dpdk_$(VPP_VERSION_FULL)_$(CONFIGURED_ARCH).deb$(VPP_DIST_VER)
SONIC_ONLINE_DEBS += $(VPP_PLUGIN_DPDK)
$(VPP_PLUGIN_DPDK)_DEPENDS += $(LIBNUMA) $(VPP_MAIN)
$(VPP_PLUGIN_DPDK)_RDEPENDS += $(LIBNUMA) $(VPP_MAIN)


VPP_PLUGIN_DEV = vpp-plugin-devtools_$(VPP_VERSION_FULL)_$(CONFIGURED_ARCH).deb
$(VPP_PLUGIN_DEV)_URL = $(VPP_PKG_SERVER)/vpp-plugin-devtools_$(VPP_VERSION_FULL)_$(CONFIGURED_ARCH).deb$(VPP_DIST_VER)
SONIC_ONLINE_DEBS += $(VPP_PLUGIN_DEV)
$(VPP_PLUGIN_DEV)_DEPENDS += $(VPP_MAIN) $(VPPINFRA)
$(VPP_PLUGIN_DEV)_RDEPENDS += $(VPP_MAIN) $(VPPINFRA)


VPPINFRA_DEV = libvppinfra-dev_$(VPP_VERSION_FULL)_$(CONFIGURED_ARCH).deb
$(VPPINFRA_DEV)_URL = $(VPP_PKG_SERVER)/libvppinfra-dev_$(VPP_VERSION_FULL)_$(CONFIGURED_ARCH).deb$(VPP_DIST_VER)
SONIC_ONLINE_DEBS += $(VPPINFRA_DEV)


VPP_DEV = vpp-dev_$(VPP_VERSION_FULL)_$(CONFIGURED_ARCH).deb
$(VPP_DEV)_URL = $(VPP_PKG_SERVER)/vpp-dev_$(VPP_VERSION_FULL)_$(CONFIGURED_ARCH).deb$(VPP_DIST_VER)
SONIC_ONLINE_DEBS += $(VPP_DEV)
$(VPP_DEV)_DEPENDS += $(VPPINFRA_DEV)
$(VPP_DEV)_RDEPENDS += $(VPPINFRA_DEV)


VPPDBG = vpp-dbg_$(VPP_VERSION_FULL)_$(CONFIGURED_ARCH).deb
$(VPPDBG)_URL = $(VPP_PKG_SERVER)/vpp-dbg_$(VPP_VERSION_FULL)_$(CONFIGURED_ARCH).deb$(VPP_DIST_VER)
SONIC_ONLINE_DEBS += $(VPPDBG)


export VPP_MAIN VPP_PLUGIN_CORE VPP_PLUGIN_DPDK VPP_PLUGIN_DEV VPP_DEV VPPINFRA_DEV VPPDBG
