SUBDIRS = vppxlate

AM_CXXFLAGS = $(SAIINC) -I$(top_srcdir)/lib

lib_LTLIBRARIES = libsaivpp.la

noinst_LIBRARIES = libSaiVPP.a

libSaiVPP_a_SOURCES = \
					  Buffer.cpp \
					  ContextConfigContainer.cpp \
					  ContextConfig.cpp \
					  Context.cpp \
					  CorePortIndexMapContainer.cpp \
					  CorePortIndexMap.cpp \
					  CorePortIndexMapFileParser.cpp \
					  Event.cpp \
					  EventPayloadNetLinkMsg.cpp \
					  EventPayloadNotification.cpp \
					  EventPayloadPacket.cpp \
					  EventQueue.cpp \
					  FdbInfo.cpp \
					  HostInterfaceInfo.cpp \
					  LaneMapContainer.cpp \
					  LaneMap.cpp \
					  LaneMapFileParser.cpp \
					  MACsecAttr.cpp \
					  MACsecFilterStateGuard.cpp \
					  MACsecEgressFilter.cpp \
					  MACsecFilter.cpp \
					  MACsecForwarder.cpp \
					  MACsecIngressFilter.cpp \
					  MACsecManager.cpp \
					  NetMsgRegistrar.cpp \
					  RealObjectIdManager.cpp \
					  ResourceLimiterContainer.cpp \
					  ResourceLimiter.cpp \
					  ResourceLimiterParser.cpp \
					  SaiAttrWrap.cpp \
					  Sai.cpp \
					  SaiEventQueue.cpp \
					  SaiFdbAging.cpp \
					  SaiUnittests.cpp \
					  SelectableFd.cpp \
					  Signal.cpp \
					  SwitchBCM56850.cpp \
					  SwitchBCM56971B0.cpp \
					  SwitchBCM81724.cpp \
					  SwitchConfigContainer.cpp \
					  SwitchConfig.cpp \
					  SwitchContainer.cpp \
					  Switch.cpp \
					  SwitchMLNX2700.cpp \
					  SwitchStateBase.cpp \
					  SwitchStateBaseFdb.cpp \
					  SwitchStateBaseHostif.cpp \
					  SwitchStateBaseRif.cpp \
					  SwitchStateBaseNbr.cpp \
					  SwitchStateBaseRoute.cpp \
					  SwitchStateBaseNexthop.cpp \
					  SwitchStateBaseAcl.cpp \
					  SwitchStateBaseMACsec.cpp \
					  SwitchState.cpp \
					  TrafficFilterPipes.cpp \
					  TrafficForwarder.cpp \
					  VirtualSwitchSaiInterface.cpp \
					  VirtualSwitchSaiInterfaceFdb.cpp \
					  VirtualSwitchSaiInterfacePort.cpp \
					  SaiObjectDB.cpp \
					  TunnelManager.cpp

BUILT_SOURCES = sai_vpp.cpp

sai_vpp.cpp: $(SRC_PATH)/sonic-sairedis/stub.pl $(top_srcdir)/SAI/meta/saimetadata.c
	$(SRC_PATH)/sonic-sairedis/stub.pl -d ../SAI/ -c Sai -n saivpp -f sai_vpp.cpp -s stub

clean-local:
	rm -f sai_vpp.cpp

libsaivpp_la_SOURCES = sai_vpp.cpp

VPP_LIBS        = -lvapiclient
VPP_LIBS        += -lvppapiclient
VPP_LIBS        += -lvlibmemoryclient
VPP_LIBS        += -lsvm
VPP_LIBS        += -lvppinfra
VPP_LIBS        += -lvlib
VPP_LIBS        += -lvatplugin

libSaiVPP_a_CPPFLAGS = $(CODE_COVERAGE_CPPFLAGS)
libSaiVPP_a_CXXFLAGS = $(DBGFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS_COMMON) $(CODE_COVERAGE_CXXFLAGS)

libsaivpp_la_CPPFLAGS = $(CODE_COVERAGE_CPPFLAGS)
libsaivpp_la_CXXFLAGS = $(DBGFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS_COMMON) $(CODE_COVERAGE_CXXFLAGS)
libsaivpp_la_LIBADD = -lhiredis -lswsscommon libSaiVPP.a $(CODE_COVERAGE_LIBS) ./vppxlate/libvppxlate.a $(VPP_LIBS)

bin_PROGRAMS = tests

tests_SOURCES = tests.cpp
tests_CXXFLAGS = $(DBGFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS_COMMON)
tests_LDADD = -lhiredis -lswsscommon -lpthread libsaivpp.la -L$(top_srcdir)/meta/.libs -lsaimetadata -lsaimeta -lzmq

TESTS = tests
