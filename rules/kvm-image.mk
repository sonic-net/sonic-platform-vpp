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

# sonic kvm image

SONIC_KVM_IMAGE = sonic-vpp.img.gz
$(SONIC_KVM_IMAGE)_INSTALLS += $(SYSTEMD_SONIC_GENERATOR) $(VPP_PLATFORM_MODULE)
$(SONIC_KVM_IMAGE)_MACHINE = vpp
$(SONIC_KVM_IMAGE)_IMAGE_TYPE = kvm
ifeq ($(INSTALL_DEBUG_TOOLS),y)
$(SONIC_KVM_IMAGE)_DOCKERS += $(SONIC_INSTALL_DOCKER_DBG_IMAGES)
$(SONIC_KVM_IMAGE)_DOCKERS += $(filter-out $(patsubst %-$(DBG_IMAGE_MARK).gz,%.gz, $(SONIC_INSTALL_DOCKER_DBG_IMAGES)), $(SONIC_INSTALL_DOCKER_IMAGES))
else
$(SONIC_KVM_IMAGE)_DOCKERS = $(SONIC_INSTALL_DOCKER_IMAGES)
endif
$(SONIC_KVM_IMAGE)_FILES = $(ONIE_RECOVERY_IMAGE) $(ONIE_RECOVERY_KVM_4ASIC_IMAGE) $(ONIE_RECOVERY_KVM_6ASIC_IMAGE)
SONIC_INSTALLERS += $(SONIC_KVM_IMAGE)
