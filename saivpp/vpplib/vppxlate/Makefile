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

VPP_LIBS        = -lvapiclient
VPP_LIBS        += -lvppapiclient
VPP_LIBS        += -lvlibmemoryclient
VPP_LIBS        += -lsvm
VPP_LIBS        += -lvppinfra
VPP_LIBS        += -lvlib
VPP_LIBS        += -lvatplugin

SaiVppXlate.o: SaiVppXlate.c
	$(CC) -g -I/usr/include/vpp_plugins -fPIC -o$@ -c SaiVppXlate.c

SaiVppStats.o: SaiVppStats.c SaiVppStats.h
	$(CC) -g -fPIC -o$@ -c SaiVppStats.c

SaiIntfStats.o: SaiIntfStats.c SaiVppStats.h SaiIntfStats.h
	$(CC) -g -fPIC -o$@ -c SaiIntfStats.c

SaiAclStats.o: SaiAclStats.c SaiVppStats.h SaiAclStats.h
	$(CC) -g -fPIC -o$@ -c SaiAclStats.c

libvppxlate.a: SaiVppXlate.o SaiVppStats.o SaiIntfStats.o SaiAclStats.o
	ar rc $@ SaiVppXlate.o SaiVppStats.o SaiIntfStats.o SaiAclStats.o

vppst: SaiIntfStats.c SaiVppStats.c
	$(CC) -DMAIN -g -o$@ $(VPP_LIBS) SaiVppStats.c SaiIntfStats.c

all: libvppxlate.a

.PHONY: install clean

install:

clean:
	rm -f *.o libvppxlate.a
