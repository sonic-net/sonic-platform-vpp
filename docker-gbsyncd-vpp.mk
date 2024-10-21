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

# docker image for vpp gbsyncd

DOCKER_GBSYNCD_PLATFORM_CODE = vpp
include $(PLATFORM_PATH)/../template/docker-gbsyncd-bookworm.mk

$(DOCKER_GBSYNCD_BASE)_DEPENDS += $(LIBSAIVPP) \
                                $(SYNCD)

$(DOCKER_GBSYNCD_BASE)_DBG_DEPENDS += $(SYNCD_VPP_DBG) \
                                $(LIBSWSSCOMMON_DBG) \
                                $(LIBSAIMETADATA_DBG) \
                                $(LIBSAIREDIS_DBG) \
                                $(LIBSAIVPP_DBG)

$(DOCKER_GBSYNCD_BASE)_VERSION = 1.0.0
$(DOCKER_GBSYNCD_BASE)_PACKAGE_NAME = gbsyncd

$(DOCKER_GBSYNCD_BASE)_RUN_OPT += -v /host/warmboot:/var/warmboot
