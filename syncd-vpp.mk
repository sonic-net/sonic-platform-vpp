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

$(LIBSAIREDIS)_DEB_BUILD_PROFILES += syncd vs

# VPP uses the virtual switch syncd since it operates in software dataplane mode
SYNCD_VPP = syncd-vs_1.0.0_$(CONFIGURED_ARCH).deb
SYNCD_VPP_DBG = syncd-vs-dbgsym_1.0.0_$(CONFIGURED_ARCH).deb

SYNCD_VS = syncd-vs_1.0.0_$(CONFIGURED_ARCH).deb
$(SYNCD_VS)_RDEPENDS += $(LIBSAIREDIS) $(LIBSAIMETADATA) $(LIBSAIVS)
$(eval $(call add_derived_package,$(LIBSAIREDIS),$(SYNCD_VS)))

SYNCD_VS_DBG = syncd-vs-dbgsym_1.0.0_$(CONFIGURED_ARCH).deb
$(SYNCD_VS_DBG)_DEPENDS += $(SYNCD_VS)
$(SYNCD_VS_DBG)_RDEPENDS += $(SYNCD_VS)
$(eval $(call add_derived_package,$(LIBSAIREDIS),$(SYNCD_VS_DBG)))
