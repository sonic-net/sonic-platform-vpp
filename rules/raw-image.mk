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

# sonic vpp raw image installer

SONIC_RAW_IMAGE = sonic-vpp.raw
$(SONIC_RAW_IMAGE)_MACHINE = vpp
$(SONIC_RAW_IMAGE)_IMAGE_TYPE = raw
$(SONIC_RAW_IMAGE)_INSTALLS += $(SYSTEMD_SONIC_GENERATOR) $(VPP_PLATFORM_MODULE)
$(SONIC_RAW_IMAGE)_DOCKERS += $(SONIC_INSTALL_DOCKER_IMAGES)
SONIC_INSTALLERS += $(SONIC_RAW_IMAGE)
