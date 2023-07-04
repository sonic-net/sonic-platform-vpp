# SONiC-VPP saivpp ACL provisioning

## ACL configuration in SONiC
The ACL configuration requires first to create a table using ACL cli and update the ACL with rules. The ACL rules are supposed to be in a file in SONiC json format.

Create an ACL and bind it to a port
```
config acl add table TEST_IPV4 L3 -p Ethernet0 -s ingress
```
The above command triggers a flurry of ACL related objects getting created in orchagent and then passed to syncd via SAI API. These SAI API objects represent the ACL in forward engine.

 - ACL_TABLE
 - ACL_TABLE_ENTRY
 - TABLE_GROUP
 - TABLE_GROUP_MEMBER
 - PORT

ACL_TABLE represents broader ACL and one or more ACL_TABLE_ENTRY objects point to ACL_TABLE.
There is an indirection to ACL_TABLE from one of the TABLE_GROUP_MEMBER. Now PORT object does not refer directly to either ACL_TABLE or ACL_GROUP_MEMBER, instead PORT object has binding with TABLE_GROUP. The TABLE_GROUP in turn has GROU_MEMBERs pointing back.

This object relation model makes it little difficult to implement the binding of ACL to a PORT. The relation of these objects are detailed in the figure 1. This is the reason for multiple maps being used to track and configure ACL in saivpp layer.

<img src="sai-acl.png" alt="SONIC-VPP SAI ACL Objects" title="SONIC-VPP ACL Objects Relation">

## VPP ACL support
VPP supports MAC, IPv4, IPv6 ACLs. One set of APIs for configuring ACLs and other set for binding the created ACLs to an interface.
VPP return an ACL index for an  ACL configured in VPP. This ACL index should be used for future updates(replace or delete).
 
