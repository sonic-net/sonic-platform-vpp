#include "sai_vpp.h"

VPP_GENERIC_QUAD(METER_BUCKET, meter_bucket);
VPP_BULK_CREATE(METER_BUCKET, meter_buckets);
VPP_BULK_REMOVE(METER_BUCKET, meter_buckets);

VPP_GENERIC_QUAD(METER_BUCKET, meter_policy);
VPP_BULK_CREATE(METER_BUCKET, meter_policys);
VPP_BULK_REMOVE(METER_BUCKET, meter_policys);

VPP_GENERIC_QUAD(METER_BUCKET, meter_rule);
VPP_BULK_CREATE(METER_BUCKET, meter_rules);
VPP_BULK_REMOVE(METER_BUCKET, meter_rules);

const sai_dash_meter_api_t vpp_dash_meter_api = {

    VPP_GENERIC_QUAD_API(meter_bucket)
    vpp_bulk_create_meter_buckets,
    vpp_bulk_remove_meter_buckets,

    VPP_GENERIC_QUAD_API(meter_policy)
    vpp_bulk_create_meter_policys,
    vpp_bulk_remove_meter_policys,

    VPP_GENERIC_QUAD_API(meter_rule)
    vpp_bulk_create_meter_rules,
    vpp_bulk_remove_meter_rules,
};
