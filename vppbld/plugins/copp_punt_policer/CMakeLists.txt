# Copyright (c) 2026 SONiC-VPP contributors
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

# Linked against linux-cp's `lcp` library so the data-path node can look
# up the phy->host (TAP) interface pairing and punt conforming,
# CoPP-matched packets directly to the mapped TAP -- bypassing
# ethernet-input/linux-cp's own protocol-dispatch nodes entirely, so
# delivery does not depend on whichever downstream path (arp-input,
# linux-cp-punt-xc, ip4-punt-redirect, ...) would otherwise have
# handled it.
add_vpp_plugin(copp_punt_policer
  SOURCES
  copp_punt_policer.c
  copp_punt_policer_node.c

  API_FILES
  copp_punt_policer.api

  LINK_LIBRARIES
  lcp
)
