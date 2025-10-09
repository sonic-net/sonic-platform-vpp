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

# docker image for docker-ptf

DOCKER_PTF = docker-ptf.gz
$(DOCKER_PTF)_PYTHON_WHEELS += $(PTF_PY3)
$(DOCKER_PTF)_PATH = $(DOCKERS_PATH)/docker-ptf
$(DOCKER_PTF)_DEPENDS += $(LIBTHRIFT) $(PYTHON_THRIFT) $(PTF) $(PYTHON_SAITHRIFT)
SONIC_DOCKER_IMAGES += $(DOCKER_PTF)
SONIC_BUSTER_DOCKERS += $(DOCKER_PTF)
