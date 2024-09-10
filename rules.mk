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

include $(PLATFORM_PATH)/syncd-vpp.mk
include $(PLATFORM_PATH)/sonic-version.mk
include $(PLATFORM_PATH)/docker-sonic-vpp.mk
include $(PLATFORM_PATH)/docker-syncd-vpp.mk
include $(PLATFORM_PATH)/docker-gbsyncd-vpp.mk
include $(PLATFORM_PATH)/docker-ptf.mk
include $(PLATFORM_PATH)/docker-ptf-sai.mk
include $(PLATFORM_PATH)/libsaithrift-dev.mk
include $(PLATFORM_PATH)/one-image.mk
include $(PLATFORM_PATH)/onie.mk
include $(PLATFORM_PATH)/kvm-image.mk
include $(PLATFORM_PATH)/raw-image.mk

SONIC_ALL += $(SONIC_ONE_IMAGE) $(SONIC_KVM_IMAGE) $(DOCKER_SONIC_VPP) $(SONIC_RAW_IMAGE)
