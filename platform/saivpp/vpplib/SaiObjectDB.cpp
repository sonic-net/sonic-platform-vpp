#include <memory>
#include <map>

#include "swss/logger.h"
#include "meta/sai_serialize.h"
#include "SwitchStateBase.h"
#include "SwitchStateBaseUtils.h"
#include "SaiObjectDB.h"

using namespace saivpp;
typedef struct _SaiChildRelation
{
    sai_object_type_t parent_type;
    // the attribute id in child object pointing to Parent object
    sai_attr_id_t child_link_attr;
    sai_attr_value_type_t child_link_attr_type;
} SaiChildRelation;

std::map<sai_object_type_t, std::vector<SaiChildRelation>> sai_child_relation_defs = {
    {SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, {{SAI_OBJECT_TYPE_TUNNEL_MAP, SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP, SAI_ATTR_VALUE_TYPE_OBJECT_ID}}},
    {SAI_OBJECT_TYPE_TUNNEL,           {{SAI_OBJECT_TYPE_TUNNEL_MAP, SAI_TUNNEL_ATTR_DECAP_MAPPERS, SAI_ATTR_VALUE_TYPE_OBJECT_LIST},
                                        {SAI_OBJECT_TYPE_TUNNEL_MAP, SAI_TUNNEL_ATTR_ENCAP_MAPPERS, SAI_ATTR_VALUE_TYPE_OBJECT_LIST}}}
};

static std::vector<std::string>
get_parent_oids(const sai_attribute_value_t *attr_val, const SaiChildRelation& child_def) 
{
    std::vector<std::string> parent_ids;
    
    switch (child_def.child_link_attr_type) {
        case SAI_ATTR_VALUE_TYPE_OBJECT_ID:
            parent_ids.push_back(sai_serialize_object_id(attr_val->oid));
            break;
        case SAI_ATTR_VALUE_TYPE_OBJECT_LIST:
        {
            auto& linked_obj_list = attr_val->objlist;
            for (uint32_t i = 0; i < linked_obj_list.count; i++) {
                parent_ids.push_back(sai_serialize_object_id(linked_obj_list.list[i]));       
            }
            break;
        }
        default:
            break;
    }
    return parent_ids;
}

static std::vector<std::string>
get_parent_oids(SwitchStateBase* switch_db, sai_object_type_t child_type, const std::string& child_oid, const SaiChildRelation& child_def) 
{
    std::vector<std::string> parent_ids;
    sai_status_t status;
    sai_attribute_t attr;
    sai_object_id_t obj_list[MAX_OBJLIST_LEN];
    attr.id = child_def.child_link_attr;
    if (child_def.child_link_attr_type == SAI_ATTR_VALUE_TYPE_OBJECT_LIST) {
        attr.value.objlist.count = MAX_OBJLIST_LEN;
        attr.value.objlist.list = obj_list;
    }
    
    status = switch_db->get(child_type, child_oid, 1, &attr);
    if (status != SAI_STATUS_SUCCESS) {
        SWSS_LOG_WARN("get_parent_oids: the child object is not found in switch_db %s", child_oid.c_str());
        return parent_ids;
    }    
    switch (child_def.child_link_attr_type) {
        case SAI_ATTR_VALUE_TYPE_OBJECT_ID:
            parent_ids.push_back(sai_serialize_object_id(attr.value.oid));
            break;
        case SAI_ATTR_VALUE_TYPE_OBJECT_LIST:
        {
            sai_object_list_t& linked_obj_list = attr.value.objlist;
            for (uint32_t i = 0; i < linked_obj_list.count; i++) {
                parent_ids.push_back(sai_serialize_object_id(linked_obj_list.list[i]));       
            }
            break;
        }
        default:
            break;
    }
    return parent_ids;
}

sai_status_t 
SaiObjectDB::add(
                _In_ sai_object_type_t object_type,
                _In_ const std::string& id,
                _In_ sai_object_id_t switch_id,
                _In_ uint32_t attr_count,
                _In_ const sai_attribute_t *attr_list)
{
    sai_status_t status;
    auto child_def_it = sai_child_relation_defs.find(object_type);
    if (child_def_it == sai_child_relation_defs.end()) {
        // Not an interesting type
        return SAI_STATUS_SUCCESS;
    }
    sai_object_id_t oid;
    sai_deserialize_object_id(id, oid);
    //Get Parent object type and OID
    const sai_attribute_value_t *attr_val;
    uint32_t attr_index;
    //iterate multiple child relation for the given parent type
    auto& child_defs = child_def_it->second;
    for (auto child_def: child_defs) {
        status = find_attrib_in_list(attr_count, attr_list, child_def.child_link_attr,
                                    &attr_val, &attr_index);
        if (status != SAI_STATUS_SUCCESS) {
            // attribute not found
            return SAI_STATUS_SUCCESS;
        }

        auto sai_parents = m_sai_parent_objs[child_def.parent_type];
        std::shared_ptr<SaiDBObject> sai_parent;
        
        std::vector<std::string> parent_ids = get_parent_oids(attr_val, child_def);
        for (auto parent_id: parent_ids) {
            auto parent_it = sai_parents.find(parent_id);
            if (parent_it == sai_parents.end()) {
                //The Parent object hasn't been created. create the Parent object
                sai_parent = std::make_shared<SaiDBObject>(m_switch_db, child_def.parent_type, parent_id);
                sai_parents[parent_id] = sai_parent; 
            } else {
                sai_parent = parent_it->second;
            }
            m_sai_parent_objs[child_def.parent_type] = sai_parents;
            auto sai_child = std::make_shared<SaiDBObject>(m_switch_db, object_type, id);
            sai_parent->add_child(sai_child);
            SWSS_LOG_DE("Add child %s to parent %s of type %s", id.c_str(), parent_id.c_str(), 
                    sai_serialize_object_type(child_def.parent_type).c_str());
        }
    }
    
    return SAI_STATUS_SUCCESS;
}

sai_status_t 
SaiObjectDB::remove(
                _In_ sai_object_type_t object_type,
                _In_ const std::string& id)
{
    //remove parent object
    auto sai_parent_type_it = m_sai_parent_objs.find(object_type);
    if (sai_parent_type_it != m_sai_parent_objs.end()) {
        auto sai_parent_it = sai_parent_type_it->second.find(id);
        if (sai_parent_it != sai_parent_type_it->second.end()) {
            sai_parent_type_it->second.erase(sai_parent_it);
        } else {
            SWSS_LOG_WARN("The object to be removed is not found in SaiObjectDB %s", id.c_str());
        }
        
        return SAI_STATUS_SUCCESS;
    }
    //The object can be a child of a parent object
    auto child_def_it = sai_child_relation_defs.find(object_type);
    if (child_def_it == sai_child_relation_defs.end()) {
        // Not an interesting type
        return SAI_STATUS_SUCCESS;
    }

    //Get Parent object type and OID
    auto& child_defs = child_def_it->second;
    for (auto child_def: child_defs) {
        std::vector<std::string> parent_ids = get_parent_oids(m_switch_db, object_type, id, child_def);
        auto sai_parents = m_sai_parent_objs[child_def.parent_type];
        for (auto parent_id: parent_ids) {
            auto parent_it = sai_parents.find(parent_id);
            if (parent_it == sai_parents.end()) {
                SWSS_LOG_WARN("Parent object is not found in SaiObjectDB %s", parent_id.c_str());
                return SAI_STATUS_SUCCESS;
            } else {
                //remove the child from sai_parent
                parent_it->second->remove_child(object_type, id);
                SWSS_LOG_DEBUG("Remove child %s from parent %s of type %s", id.c_str(), parent_id.c_str(), 
                        sai_serialize_object_type(child_def.parent_type).c_str());
            }
        }
    }
    return SAI_STATUS_SUCCESS;
}

std::shared_ptr<SaiObject> 
SaiObjectDB::get(
                _In_ sai_object_type_t object_type,
                _In_ const std::string& id)
{
    sai_status_t status;
    sai_attribute_t attr;
    //first check if the object is a stored SaiDBObject
    auto sai_parent_type_it = m_sai_parent_objs.find(object_type);
    if (sai_parent_type_it != m_sai_parent_objs.end()) {
        auto sai_parent_it = sai_parent_type_it->second.find(id);
        if (sai_parent_it != sai_parent_type_it->second.end()) {
            return sai_parent_it->second;
        } else {
            SWSS_LOG_WARN("Parent is not found in SaiObjectDB %s", id.c_str());
            return std::shared_ptr<SaiObject>();
        }
    }
    // check if the object exists
    status = m_switch_db->get(object_type, id, 0, &attr);
    if (status != SAI_STATUS_SUCCESS) {
        SWSS_LOG_WARN("Parent object is not found in SaiObjectDB %s", id.c_str());
        return std::shared_ptr<SaiObject>();
    }
    /*
     return a SaiObject as a wrapper to underlaying object is switch_db.
     the object may exists in a sai_parent as an entry but we don't care if 
     returning a new copy since it is just a wrapper today */
    return std::make_shared<SaiDBObject>(m_switch_db, object_type, id);
}

std::shared_ptr<SaiObject>
SaiObject::get_linked_object(
            _In_ sai_object_type_t linked_object_type,
            _In_ sai_attr_id_t link_attr_id) const 
{
    sai_status_t status;
    sai_attribute_t attr;
    std::string linked_obj_id;

    attr.id = link_attr_id;
    status = get_attr(attr);
    if (status != SAI_STATUS_SUCCESS) {
        SWSS_LOG_ERROR("Failed to get attribute %d from object %s", link_attr_id, m_id.c_str());
        return std::shared_ptr<SaiObject>();
    }
    linked_obj_id = sai_serialize_object_id(attr.value.oid);

    return m_switch_db->get_sai_object(linked_object_type, linked_obj_id);
}

std::vector<std::shared_ptr<SaiObject>>
SaiObject::get_linked_objects(
            _In_ sai_object_type_t linked_object_type,
            _In_ sai_attr_id_t link_attr_id) const 
{
    sai_status_t status;
    sai_attribute_t attr;
    std::string linked_obj_id;
    std::vector<std::shared_ptr<SaiObject>> linked_objs;
    sai_object_id_t obj_list[MAX_OBJLIST_LEN];
    attr.id = link_attr_id;
    attr.value.objlist.count = MAX_OBJLIST_LEN;
    attr.value.objlist.list = obj_list;
    status = get_attr(attr);
    if (status != SAI_STATUS_SUCCESS) {
        SWSS_LOG_ERROR("Failed to get attribute %d from object %s", link_attr_id, m_id.c_str());
        return linked_objs;
    }
    sai_object_list_t& linked_obj_list = attr.value.objlist;
    for (uint32_t i = 0; i < linked_obj_list.count; i++) {
        linked_obj_id = sai_serialize_object_id(linked_obj_list.list[i]);
        auto linked_obj = m_switch_db->get_sai_object(linked_object_type, linked_obj_id);
        if (linked_obj) {
            linked_objs.push_back(linked_obj);
        }
    }
    return linked_objs;
}

sai_status_t 
SaiCachedObject::get_attr(sai_attribute_t &attr) const 
{
    for (uint32_t ii = 0; ii < m_attr_count; ii++) {
        if (m_attr_list[ii].id == attr.id) {
            return transfer_attributes(m_type, 1, &(m_attr_list[ii]), &attr, false);
        }
    }
    return SAI_STATUS_ITEM_NOT_FOUND;
}

sai_status_t
SaiDBObject::get_attr(sai_attribute_t &attr) const 
{
    /* we could make a copy of all the attributes and cache in this object*/
    return m_switch_db->get(m_type, m_id, 1, &attr);
}