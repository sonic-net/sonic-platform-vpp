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

# sonic version yml file

sonic_version=$(SONIC_GET_VERSION)
sonic_asic_platform=$(CONFIGURED_PLATFORM)
sonic_os_version=$(SONIC_OS_VERSION)

export sonic_version
export sonic_asic_platform
export sonic_os_version

SONIC_VERSION = sonic_version.yml
$(SONIC_VERSION)_SRC_PATH = $(PLATFORM_PATH)/sonic-version
SONIC_MAKE_FILES += $(SONIC_VERSION)

SONIC_PHONY_TARGETS += $(addprefix $(FILES_PATH)/, $(SONIC_VERSION))
