#include "sai_vpp.h"

VPP_GENERIC_QUAD(DASH_ACL_GROUP, dash_acl_group);
VPP_BULK_CREATE(DASH_ACL_GROUP, dash_acl_groups);
VPP_BULK_REMOVE(DASH_ACL_GROUP, dash_acl_groups);

VPP_GENERIC_QUAD(DASH_ACL_RULE, dash_acl_rule);
VPP_BULK_CREATE(DASH_ACL_RULE, dash_acl_rules);
VPP_BULK_REMOVE(DASH_ACL_RULE, dash_acl_rules);

const sai_dash_acl_api_t vpp_dash_acl_api = {
    VPP_GENERIC_QUAD_API(dash_acl_group)
    vpp_bulk_create_dash_acl_groups,
    vpp_bulk_remove_dash_acl_groups,

    VPP_GENERIC_QUAD_API(dash_acl_rule)
    vpp_bulk_create_dash_acl_rules,
    vpp_bulk_remove_dash_acl_rules,
};
