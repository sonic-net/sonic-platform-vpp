# Copilot Instructions for sonic-platform-vpp

## Project Overview

sonic-platform-vpp provides VPP (Vector Packet Processing) data plane integration for SONiC. Instead of using hardware ASICs via SAI, this platform uses fd.io VPP as a software-based packet processing engine. It enables SONiC to run as a high-performance software router/switch using standard x86 hardware with DPDK-accelerated networking.

## Architecture

```
sonic-platform-vpp/
├── saivpp/                          # SAI implementation backed by VPP
│   ├── vpplib/                      # VPP-SAI bridge library
│   └── ...                          # SAI object implementations
├── docker-syncd-vpp/                # Syncd container with VPP backend
├── docker-sonic-vpp/                # Single-container SONiC-VPP image
├── docker-gbsyncd-vpp/              # Gearbox syncd for VPP
├── sonic-platform-modules-vpp/      # Platform modules
├── platform/                        # Platform configuration files
├── rules/                           # Build rules for sonic-buildimage
├── rules.mk                         # Top-level Make rules
├── rules.dep                        # Build dependencies
├── scripts/                         # Build and setup scripts
├── docs/                            # Documentation
│   ├── README.getting-started.md    # Getting started guide
│   └── ...                          # Additional docs
├── platform.conf                    # Platform configuration
├── azure-pipelines.yml              # CI pipeline
└── sonic-gns3a.sh                   # GNS3 appliance setup
```

### Key Concepts
- **VPP (fd.io)**: High-performance software packet processing framework
- **SAI-VPP**: SAI API implementation that translates SAI calls to VPP API calls
- **DPDK**: Data Plane Development Kit for kernel-bypass networking
- **Syncd-VPP**: Modified syncd that uses VPP instead of hardware ASIC SDK
- **Two build targets**: Single Docker container (`docker-sonic-vpp.gz`) or full VM image

## Language & Style

- **Primary languages**: C++ (SAI-VPP), Python (scripts), Shell (build), Dockerfile
- **C++ standard**: C++14/17
- **Indentation**: 4 spaces
- **SAI conventions**: Follow SAI header naming and API patterns
- **VPP API**: Use VPP's C API for data plane programming

## Build Instructions

```bash
# Clone sonic-buildimage with submodules
git clone --recurse-submodules https://github.com/sonic-net/sonic-buildimage.git
cd sonic-buildimage

# Initialize and configure for VPP platform
make init
make configure PLATFORM=vpp

# Build single-container image
NOBULLSEYE=1 NOBUSTER=1 make SONIC_BUILD_JOBS=4 target/docker-sonic-vpp.gz

# Build full VM image
NOBULLSEYE=1 NOBUSTER=1 make SONIC_BUILD_JOBS=4 target/sonic-vpp.img.gz
```

## Testing

- Run the single-container image in Docker for functional testing
- Use GNS3 for multi-node topology testing
- Refer to `docs/README.getting-started.md` for test topology setup

## PR Guidelines

- **Signed-off-by**: Required on all commits
- **CLA**: Sign Linux Foundation EasyCLA
- **Testing**: Verify changes build and basic packet forwarding works
- **SAI compliance**: SAI-VPP changes must maintain SAI API compliance
- **CI**: Azure pipeline checks must pass

## Dependencies

- **VPP (fd.io)**: Core packet processing engine
- **DPDK**: Kernel bypass networking
- **sonic-sairedis**: SAI Redis interface framework
- **SAI headers**: Switch Abstraction Interface definitions
- **sonic-buildimage**: Build framework

## Gotchas

- **DPDK hugepages**: VPP requires hugepage memory configuration on the host
- **NIC support**: Not all NICs are supported by DPDK — check compatibility
- **SAI feature coverage**: VPP-SAI doesn't implement all SAI features — check saivpp for coverage
- **Performance tuning**: VPP performance depends heavily on CPU pinning, hugepages, and DPDK config
- **First build is slow**: The backend build (Docker images) takes significant time on first run
- **Container vs VM**: Single-container has a subset of features; VM image is more complete
- **CPU architecture**: Primarily x86_64 — ARM support may be limited
