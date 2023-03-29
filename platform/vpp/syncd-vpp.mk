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

$(LIBSAIREDIS)_DPKG_TARGET = binary-syncd-vpp

SYNCD_VPP = syncd-vpp_1.0.0_amd64.deb
$(SYNCD_VPP)_RDEPENDS += $(LIBSAIREDIS) $(LIBSAIMETADATA) $(LIBSAIVPP)
$(eval $(call add_derived_package,$(LIBSAIREDIS),$(SYNCD_VPP)))

SYNCD_VPP_DBG = syncd-vpp-dbg_1.0.0_amd64.deb
$(SYNCD_VPP_DBG)_DEPENDS += $(SYNCD_VPP)
$(SYNCD_VPP_DBG)_RDEPENDS += $(SYNCD_VPP)
$(eval $(call add_derived_package,$(LIBSAIREDIS),$(SYNCD_VPP_DBG)))
