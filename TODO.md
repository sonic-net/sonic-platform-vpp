
# Areas of Work
This sections lists pending areas of work for SONiC-VPP. At a high level the following are the areas of work. Section below describes how we phase the execution.

1. Support all SAI APIs and make changes to VPP (if necessary)
2. New features to be added to SONiC or VPP (e.g., IKE, Stateful firewall, DPI, etc)   
3. VPP configurability via configDB  
3. Debuggability of SONiC VPP  (logging, telemetry, counters)
4. Hardening
   - Restart of VPP,
   - Support warm restart in VPP (state reconciliation)
5. Performance Optimizations
   - Route convergence
   - Forwarding performance
6. Support various environments
   -AWS (container/VM)
   -K8S
   -KVM  (VM)

 

# Execution Phasing
Note- Phases 2 and beyond needs agreement between the various stateholders. The list may change accordingly 

- **Phase 1**\
  Initial code supports the following functionality
  - v4
  - v6
  - adjacency
  - sub interfaces
  - BGP
  - ARP
  - Build system for a single container (SONiC + VPP)
  - Support for following SAI APIs 
    - SAI_OBJECT_TYPE_PORT = 1,
    - SAI_OBJECT_TYPE_VIRTUAL_ROUTER = 3,
    - SAI_OBJECT_TYPE_NEXT_HOP = 4,
    - SAI_OBJECT_TYPE_ROUTER_INTERFACE = 6,
    - SAI_OBJECT_TYPE_HOSTIF = 13,
    - SAI_OBJECT_TYPE_NEIGHBOR_ENTRY = 36,
    - SAI_OBJECT_TYPE_ROUTE_ENTRY = 37,
    - SAI_OBJECT_TYPE_HOSTIF_PACKET = 40,
    - SAI_OBJECT_TYPE_SWITCH = 33,
  - Test framework and basic tests


- **Phase 2**\
  Routing focus
  - VPP Configurability
  - Debug ability
  - Various Environments (AWS, K8S, etc)
  - Loopback interface create/delete support (SAI_ROUTER_INTERFACE_TYPE_LOOPBACK)
  - syncd-vpp container and docker-sonic-vpp container migration to debian/bullseye
  - Upgrade VPP to upcoming 23.xx release
  - Support for following SAI APIs 
    - SAI_OBJECT_TYPE_NEXT_HOP_GROUP = 5
    - SAI_OBJECT_TYPE_ACL_TABLE = 7,
    - SAI_OBJECT_TYPE_ACL_ENTRY = 8,
    - SAI_OBJECT_TYPE_ACL_COUNTER = 9,
    - SAI_OBJECT_TYPE_ACL_RANGE = 10,
    - SAI_OBJECT_TYPE_ACL_TABLE_GROUP = 11,
    - SAI_OBJECT_TYPE_ACL_TABLE_GROUP_MEMBER = 12,
    - SAI_OBJECT_TYPE_COUNTER = 84,
    - SAI_OBJECT_TYPE_DEBUG_COUNTER = 85,
    - SAI_OBJECT_TYPE_VLAN = 38,
    - SAI_OBJECT_TYPE_VLAN_MEMBER = 39,
    - SAI_OBJECT_TYPE_TUNNEL_MAP = 41,
    - SAI_OBJECT_TYPE_TUNNEL = 42,
    - SAI_OBJECT_TYPE_TUNNEL_TERM_TABLE_ENTRY = 43,
    - SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY = 59,
    - SAI_OBJECT_TYPE_BFD_SESSION = 69,
    - SAI_OBJECT_TYPE_RPF_GROUP = 47,
    - SAI_OBJECT_TYPE_RPF_GROUP_MEMBER = 48,
    - SAI_OBJECT_TYPE_IPMC_GROUP = 51,
    - SAI_OBJECT_TYPE_IPMC_GROUP_MEMBER = 52,
    - SAI_OBJECT_TYPE_IPMC_ENTRY = 54,
    - SAI_OBJECT_TYPE_TAM_MATH_FUNC = 72,
    - SAI_OBJECT_TYPE_TAM_REPORT = 73,
    - SAI_OBJECT_TYPE_TAM_EVENT_THRESHOLD = 74,
    - SAI_OBJECT_TYPE_TAM_TEL_TYPE = 75,
    - SAI_OBJECT_TYPE_TAM_TRANSPORT = 76,
    - SAI_OBJECT_TYPE_TAM_TELEMETRY = 77,
    - SAI_OBJECT_TYPE_TAM_COLLECTOR = 78,
    - SAI_OBJECT_TYPE_TAM_EVENT_ACTION = 79,
    - SAI_OBJECT_TYPE_TAM_EVENT = 80,
    - SAI_OBJECT_TYPE_LAG = 2

- **Phase 3**\
  L2 and QoS focus 
  - Support for following SAI APIs
    - SAI_OBJECT_TYPE_POLICER = 18,
    - SAI_OBJECT_TYPE_WRED = 19,
    - SAI_OBJECT_TYPE_QOS_MAP = 20,
    - SAI_OBJECT_TYPE_QUEUE = 21,
    - SAI_OBJECT_TYPE_SCHEDULER = 22,
    - SAI_OBJECT_TYPE_SCHEDULER_GROUP = 23,
    - SAI_OBJECT_TYPE_BUFFER_POOL = 24,
    - SAI_OBJECT_TYPE_BUFFER_PROFILE = 25,
    - SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP = 26,
    - SAI_OBJECT_TYPE_BRIDGE = 57,
    - SAI_OBJECT_TYPE_BRIDGE_PORT = 58
    - SAI_OBJECT_TYPE_STP_PORT = 46,


- **Phase 4**\
  Advanced features like IPSec, Stateful firewall, DPI, SRv6, NAT, MACSec etc
  - Support for following SAI APIs
    - SAI_OBJECT_TYPE_IPSEC = 99,
    - SAI_OBJECT_TYPE_IPSEC_PORT = 100,
    - SAI_OBJECT_TYPE_IPSEC_SA = 101,
    - SAI_OBJECT_TYPE_MACSEC = 88,
    - SAI_OBJECT_TYPE_MACSEC_PORT = 89,
    - SAI_OBJECT_TYPE_MACSEC_FLOW = 90,
    - SAI_OBJECT_TYPE_MACSEC_SC = 91,
    - SAI_OBJECT_TYPE_MACSEC_SA = 92,
    - SAI_OBJECT_TYPE_NAT_ZONE_COUNTER = 81,
    - SAI_OBJECT_TYPE_NAT_ENTRY = 82,
    - SAI_OBJECT_TYPE_MIRROR_SESSION = 14,
    - SAI_OBJECT_TYPE_SAMPLEPACKET = 15,

