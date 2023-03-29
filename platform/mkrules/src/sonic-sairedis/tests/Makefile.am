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

AM_CXXFLAGS = $(SAIINC) -I$(top_srcdir)/lib -I$(top_srcdir)/vpplib

bin_PROGRAMS = vssyncd tests testclient

SAILIB=-L$(top_srcdir)/vpplib/.libs -lsaivpp

vssyncd_SOURCES = main.cpp
vssyncd_CPPFLAGS = $(CODE_COVERAGE_CPPFLAGS)
vssyncd_CXXFLAGS = $(DBGFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS_COMMON) $(CODE_COVERAGE_CXXFLAGS)
vssyncd_LDADD = $(top_srcdir)/syncd/libSyncd.a $(top_srcdir)/lib/libSaiRedis.a \
				-lhiredis -lswsscommon $(SAILIB) -lpthread \
				-L$(top_srcdir)/meta/.libs -lsaimetadata -lsaimeta -ldl -lzmq $(CODE_COVERAGE_LIBS)

if SAITHRIFT
vssyncd_LDADD += -lrpcserver -lthrift
endif

tests_SOURCES = tests.cpp
tests_CXXFLAGS = $(DBGFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS_COMMON)
tests_LDADD = -lhiredis -lswsscommon -lpthread \
			  $(top_srcdir)/lib/libsairedis.la $(top_srcdir)/syncd/libSyncd.a \
			  -L$(top_srcdir)/meta/.libs -lsaimetadata -lsaimeta -lzmq $(CODE_COVERAGE_LIBS)

testclient_SOURCES = testclient.cpp
testclient_CXXFLAGS = $(DBGFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS_COMMON)
testclient_LDADD = -lhiredis -lswsscommon -lpthread \
				   $(top_srcdir)/lib/libsairedis.la $(top_srcdir)/syncd/libSyncd.a \
				   -L$(top_srcdir)/meta/.libs -lsaimetadata -lsaimeta -lzmq $(CODE_COVERAGE_LIBS)

TESTS = aspellcheck.pl conflictnames.pl swsslogentercheck.sh checkwhitespace.sh tests BCM56850.pl MLNX2700.pl BCM56971B0.pl
