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

AM_CXXFLAGS = $(SAIINC) -I$(top_srcdir)/meta -I$(top_srcdir)/lib

bin_PROGRAMS = saidiscovery

if SAIVPP
SAILIB=-L$(top_srcdir)/vpplib/.libs -lsaivpp
else
SAILIB=-lsai
endif

saidiscovery_SOURCES = saidiscovery.cpp

saidiscovery_CPPFLAGS = $(CODE_COVERAGE_CPPFLAGS)
saidiscovery_CXXFLAGS = $(DBGFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS_COMMON) $(CODE_COVERAGE_CXXFLAGS)
saidiscovery_LDADD = -lhiredis -lswsscommon $(top_srcdir)/syncd/libSyncd.a $(SAILIB) -lpthread \
					 -L$(top_srcdir)/meta/.libs -lsaimetadata -lsaimeta -lzmq $(CODE_COVERAGE_LIBS)
