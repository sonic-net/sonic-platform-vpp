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

# docker image for virtual switch based sonic docker image

DOCKER_SONIC_VPP = docker-sonic-vpp.gz
$(DOCKER_SONIC_VPP)_PATH = $(PLATFORM_PATH)/docker-sonic-vpp
$(DOCKER_SONIC_VPP)_DEPENDS += $(SYNCD_VPP) \
                              $(LIBVPP) \
                              $(PYTHON_SWSSCOMMON) \
                              $(PYTHON3_SWSSCOMMON) \
                              $(LIBTEAMDCTL) \
                              $(LIBTEAM_UTILS) \
                              $(SONIC_DEVICE_DATA) \
                              $(LIBYANG) \
                              $(LIBYANG_CPP) \
                              $(LIBYANG_PY3) \
                              $(SONIC_UTILITIES_DATA) \
                              $(SONIC_HOST_SERVICES_DATA) \
			                  $(FRR) $(FRR_SNMP) \
			                  $(LIBYANG2) \
                              $(IPROUTE2)

$(DOCKER_SONIC_VPP)_PYTHON_WHEELS += $(SWSSSDK_PY3) \
                                    $(SONIC_PY_COMMON_PY3) \
                                    $(SONIC_PLATFORM_COMMON_PY3) \
                                    $(SONIC_YANG_MODELS_PY3) \
                                    $(SONIC_YANG_MGMT_PY3) \
                                    $(SONIC_UTILITIES_PY3) \
                                    $(SONIC_HOST_SERVICES_PY3) \
				    $(SONIC_BGPCFGD) \
				    $(SONIC_VPPCFGD) \
				    $(SONIC_FRR_MGMT_FRAMEWORK)

ifeq ($(INSTALL_DEBUG_TOOLS), y)
$(DOCKER_SONIC_VPP)_DEPENDS += $(LIBSWSSCOMMON_DBG) \
                              $(LIBSAIREDIS_DBG) \
                              $(LIBSAIVPP_DBG) \
                              $(SYNCD_VPP_DBG)
endif

$(DOCKER_SONIC_VPP)_FILES += $(CONFIGDB_LOAD_SCRIPT) \
                            $(ARP_UPDATE_SCRIPT) \
                            $(ARP_UPDATE_VARS_TEMPLATE) \
                            $(BUFFERS_CONFIG_TEMPLATE) \
                            $(QOS_CONFIG_TEMPLATE) \
                            $(SONIC_VERSION) \
                            $(UPDATE_CHASSISDB_CONFIG_SCRIPT) \
                            $(COPP_CONFIG_TEMPLATE)

$(DOCKER_SONIC_VPP)_LOAD_DOCKERS += $(DOCKER_SWSS_LAYER_BOOKWORM)
SONIC_DOCKER_IMAGES += $(DOCKER_SONIC_VPP)

SONIC_BOOKWORM_DOCKERS += $(DOCKER_SONIC_VPP)
