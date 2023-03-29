- [Overview](#overview)
    - [Scope](#scope)
    - [Supported topologies](#supported-topologies)
- [General test flow](#general-test-flow)
- [Test cases](#test-cases)
    - [Test case #1](#test-case-1)
    - [Test case #2](#test-case-2)
    - [Test case #3](#test-case-3)
    - [Test case #4](#test-case-4)
    - [Test case #5](#test-case-5)

#### Overview
The purpose is to validate SONiC container and VM image with VPP based software data path.

#### Scope
Test Plan covers test cases to validate:
- Data flow through VPP data plane of SONiC VPP Images, data flow is simulated using ping of IP Addresses across SONiC VPP DUT
- Routing (Staic Route and BGP) of SONiC VPP
- SONiC functional/feature (defined under sonic-mgmt) for SONiC VPP images


#### Supported topologies:
```
Standalone DUTs
t0
ptf32
```

#### Test cases
Please refer to the test case for detailed description.

| Test case ID| Description | Area|
|-------------|-------------|-----------|
| 1 | SONiC VPP Container Image Routing validation |Static Route |
| 2 | SONiC VPP VM Image Routing validation|Static Route|
| 3 | SONiC VPP Container Image BGP Routing  |BGP|
| 4 | SONiC VPP VM Image BGP Routing  |BGP|
| 5 | SONiC VPP VM PTF on T0 Topology  |To Identified|


#### General test flow

##### step #1 - DUT VM
Before test suite run, have Ubuntu 20.4 VM ready with required packages installed and SONiC VM image staged under required folder.

##### step #2 - Execute test scenarios
Test scenarios can be executed by running test script defined for each scenario.

##### step #3 - Validate result
Check output of test script to validate test result. In case of success, test script will tear down test toplogy and leave set up in clean state for execution of next test scenario. In case of failure, test set up is left as is for troubleshooting. 

#### Test cases
Each test case can be executed using test script. Test script will perform vaidation and return pass or fail based on result of test. Test script creates required toplogy before executing tests.

#### Test case #1
##### Test objective

Verify basic routing (staic route) of SONiC VPP Container image in sigle-hop and multi-hop topology.

##### Test steps
Run test script `validate-sonic-vpp-docker-routing`:
   ```
    python validate-sonic-vpp-docker-routing.py --topology singleHop --host <VM IP Address> --username <SSH Username> --password <SSH User Password>
    python validate-sonic-vpp-docker-routing.py --topology multiHop --routing static --host <VM IP Address> --username <SSH Username> --password <SSH User Password>
   ```
#### Test case #2
##### Test objective

Verify basic routing (staic route) of SONiC VPP VM image

##### Test steps
Run test script `validate-sonic-vpp-vm-routing`:
   ```
    python validate-sonic-vpp-vm-routing.py --host <VM IP Address> --username <SSH Username> --password <SSH User Password>
   ```
#### Test case #3
##### Test objective

Verify BGP routing of SONiC VPP Container Image in multi-hop topology

##### Test steps
Run test script `validate-sonic-vpp-docker-routing`:
   ```
    python validate-sonic-vpp-docker-routing.py --topology multiHop --routing bgp --host <VM IP Address> --username <SSH Username> --password <SSH User Password>
   ```
#### Test case #4
##### Test objective

Verify BGP routing of SONiC VPP VM image in multi-hop topology

##### Test steps
Run test script `validate-sonic-vpp-vm-routing`:
   ```
    python validate-sonic-vpp-vm-routing.py --host <VM IP Address> --username <SSH Username> --password <SSH User Password>
   ```
#### Test case #5
##### Test objective

Verify SONiC VPP VM image with T0 topology configuration for existing feature/functional test cases using sonic-mgmt.

##### Test steps
- Build T0 topology using script `create_sonic_t0_topo.py`
- Run feature/functional test cases using script `run_tests.py <text-file-containing-list-of-tests>`

