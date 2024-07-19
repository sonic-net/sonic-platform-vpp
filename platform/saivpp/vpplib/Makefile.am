SUBDIRS = vppxlate

AM_CXXFLAGS = $(SAIINC) -I$(top_srcdir)/lib -I/usr/include/libnl3

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
					  VirtualSwitchSaiInterfacePort.cpp

libsaivpp_la_SOURCES = \
					  sai_vpp_acl.cpp \
					  sai_vpp_ars.cpp \
					  sai_vpp_ars_profile.cpp \
					  sai_vpp_twamp.cpp \
					  sai_vpp_bfd.cpp \
					  sai_vpp_bmtor.cpp \
					  sai_vpp_bridge.cpp \
					  sai_vpp_buffer.cpp \
					  sai_vpp_counter.cpp \
					  sai_vpp_dash_vip.cpp \
					  sai_vpp_dash_pa_validation.cpp \
					  sai_vpp_dash_vnet.cpp \
					  sai_vpp_dash_outbound_routing.cpp \
					  sai_vpp_dash_outbound_ca_to_pa.cpp \
					  sai_vpp_dash_inbound_routing.cpp \
					  sai_vpp_dash_meter.cpp \
					  sai_vpp_dash_eni.cpp \
					  sai_vpp_dash_direction_lookup.cpp \
					  sai_vpp_dash_acl.cpp \
					  sai_vpp_debug_counter.cpp\
					  sai_vpp_dtel.cpp \
					  sai_vpp_fdb.cpp \
					  sai_vpp_genericprogrammable.cpp \
					  sai_vpp_hash.cpp \
					  sai_vpp_hostif.cpp \
					  sai_vpp_interfacequery.cpp \
					  sai_vpp_ipmc.cpp \
					  sai_vpp_ipmc_group.cpp \
					  sai_vpp_isolation_group.cpp \
					  sai_vpp_l2mc.cpp \
					  sai_vpp_l2mcgroup.cpp \
					  sai_vpp_lag.cpp \
					  sai_vpp_macsec.cpp \
					  sai_vpp_mcastfdb.cpp \
					  sai_vpp_mirror.cpp \
					  sai_vpp_mpls.cpp \
					  sai_vpp_nat.cpp \
					  sai_vpp_neighbor.cpp \
					  sai_vpp_nexthop.cpp \
					  sai_vpp_nexthopgroup.cpp \
					  sai_vpp_policer.cpp \
					  sai_vpp_port.cpp \
					  sai_vpp_qosmap.cpp \
					  sai_vpp_queue.cpp \
					  sai_vpp_route.cpp \
					  sai_vpp_router_interface.cpp \
					  sai_vpp_rpfgroup.cpp \
					  sai_vpp_samplepacket.cpp \
					  sai_vpp_scheduler.cpp \
					  sai_vpp_schedulergroup.cpp \
					  sai_vpp_srv6.cpp \
					  sai_vpp_stp.cpp \
					  sai_vpp_switch.cpp \
					  sai_vpp_system_port.cpp \
					  sai_vpp_tam.cpp \
					  sai_vpp_tunnel.cpp \
					  sai_vpp_udf.cpp \
					  sai_vpp_virtual_router.cpp \
					  sai_vpp_vlan.cpp \
					  sai_vpp_wred.cpp \
					  sai_vpp_my_mac.cpp \
					  sai_vpp_ipsec.cpp \
                                          sai_vpp_poe.cpp

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
