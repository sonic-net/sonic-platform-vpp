SUBDIRS = vppxlate

SAI_PATH = /sonic/src/sonic-sairedis/SAI
AM_CXXFLAGS = $(SAIINC) -I/usr/include/sai -I/usr/include/libnl3
export AM_CXXFLAGS
lib_LTLIBRARIES = libsai.la

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
					  SwitchStateBaseBfd.cpp \
					  SwitchState.cpp \
					  TrafficFilterPipes.cpp \
					  TrafficForwarder.cpp \
					  VirtualSwitchSaiInterface.cpp \
					  VirtualSwitchSaiInterfaceFdb.cpp \
					  VirtualSwitchSaiInterfacePort.cpp \
					  SaiObjectDB.cpp \
					  TunnelManager.cpp \
					  SwitchStateBaseSRv6.cpp

BUILT_SOURCES = sai_vpp.cpp

sai_vpp.cpp: /sonic/src/sonic-sairedis/stub.pl $(SAI_PATH)/meta/saimetadata.c
	/sonic/src/sonic-sairedis/stub.pl -d $(SAI_PATH) -c Sai -n saivpp -f sai_vpp.cpp -s stub
	sed -i 's|#include "meta/SaiInterface.h"|#include "SaiInterface.h"|' sai_vpp.cpp


$(SAI_PATH)/meta/saimetadata.c $(SAI_PATH)/meta/saimetadata.h: 
	$(MAKE) -C $(SAI_PATH)/meta saimetadata.c

clean-local:
	rm -f sai_vpp.cpp

libsai_la_SOURCES = sai_vpp.cpp

VPP_LIBS        = -lvlibapi
VPP_LIBS        += -lvapiclient
VPP_LIBS        += -lvppapiclient
VPP_LIBS        += -lvlibmemoryclient
VPP_LIBS        += -lsvm
VPP_LIBS        += -lvppinfra
VPP_LIBS        += -lvlib
VPP_LIBS        += -lvatplugin

libSaiVPP_a_CPPFLAGS = $(CODE_COVERAGE_CPPFLAGS)
libSaiVPP_a_CXXFLAGS = $(DBGFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS_COMMON) $(CODE_COVERAGE_CXXFLAGS)

libsai_la_CPPFLAGS = $(CODE_COVERAGE_CPPFLAGS)
libsai_la_CXXFLAGS = $(DBGFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS_COMMON) $(CODE_COVERßAGE_CXXFLAGS)
libsai_la_LIBADD = -lhiredis -lswsscommon libSaiVPP.a $(CODE_COVERAGE_LIBS) -lnl-route-3 -lsaimetadata -lsaimeta ./vppxlate/libvppxlate.a $(VPP_LIBS)

bin_PROGRAMS = tests

tests_SOURCES = tests.cpp
tests_CXXFLAGS = $(DBGFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS_COMMON)
tests_LDADD = -lhiredis -lswsscommon -lpthread libsai.la -lsaimetadata -lsaimeta -lzmq

TESTS = tests
