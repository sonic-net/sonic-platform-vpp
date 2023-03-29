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

.PHONY: _pysairedis.la

SWIG_SRC=../pysairedis.i
SOURCES=../pysairedis.h ../pysairedis.cpp

pkgpython3_PYTHON = pysairedis.py __init__.py
pkgpy3exec_LTLIBRARIES = _pysairedis.la

AM_CXXFLAGS = $(SAIINC) -I$(top_srcdir)/lib -I..

BUILT_SOURCES = pysairedis_wrap.cpp

_pysairedis_la_SOURCES = pysairedis_wrap.cpp $(SOURCES)
_pysairedis_la_CXXFLAGS = -I/usr/include/python$(PYTHON3_VERSION) $(AM_CXXFLAGS) $(CXXFLAGS_COMMON) \
						  -Wno-cast-qual -Wno-shadow -Wno-redundant-decls -Wno-conversion $(NO_CAST_FUNCTION_TYPE)

_pysairedis_la_LDFLAGS = -module \
		-lhiredis -lswsscommon -lpthread \
		-L$(top_srcdir)/lib/.libs -lsairedis \
		-L$(top_srcdir)/meta/.libs -lsaimetadata -lsaimeta \
		-lzmq

_pysairedis_la_LIBADD = $(PYTHON3_BLDLIBRARY)

SWIG_FLAG = -Wall -c++ -python -keyword
if ARCH64
SWIG_FLAG += -DSWIGWORDSIZE64
endif

pysairedis_wrap.cpp: $(SWIG_SRC) $(SOURCES)
	$(SWIG) $(SWIG_FLAG) $(AM_CXXFLAGS) -o $@ $<

CLEANFILES = pysairedis_wrap.cpp
