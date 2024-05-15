#include <memory>
#include <map>

#include "swss/logger.h"
#include "meta/sai_serialize.h"
#include "SwitchStateBase.h"
#include "SwitchStateBaseUtils.h"
#include "SaiObjectDB.h"

using namespace saivpp;
typedef struct _SaiTableRelation
{
    sai_object_type_t table_type;
    // the attribute id in entry object pointing to table
    sai_attr_id_t table_link_attr;
} SaiTableRelation;

std::map<sai_object_type_t, SaiTableRelation> sai_entry_to_table_defs = {
    {SAI_OBJECT_TYPE_TUNNEL_MAP_ENTRY, {SAI_OBJECT_TYPE_TUNNEL_MAP, SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP}}
};

sai_status_t 
SaiObjectDB::add(
                _In_ sai_object_type_t object_type,
                _In_ const std::string& id,
                _In_ sai_object_id_t switch_id,
                _In_ uint32_t attr_count,
                _In_ const sai_attribute_t *attr_list)
{
    sai_status_t status;
    auto table_def_it = sai_entry_to_table_defs.find(object_type);
    if (table_def_it == sai_entry_to_table_defs.end()) {
        // Not an interesting type
        return SAI_STATUS_SUCCESS;
    }
    //Get table type and OID
    const sai_attribute_value_t *attr_val;
    uint32_t attr_index;
    status = find_attrib_in_list(attr_count, attr_list, table_def_it->second.table_link_attr,
                                &attr_val, &attr_index);
    if (status != SAI_STATUS_SUCCESS) {
        // attribute not found
        return SAI_STATUS_SUCCESS;
    }        
    auto sai_tables = m_sai_tables[table_def_it->second.table_type];
    std::shared_ptr<SaiTable> sai_table;
    auto serializedObjectId = sai_serialize_object_id(attr_val->oid);
    auto table_it = sai_tables.find(serializedObjectId);
    if (table_it == sai_tables.end()) {
        //The table hasn't been created. create the table
        sai_table = std::make_shared<SaiTable>(m_switch_db, table_def_it->second.table_type, serializedObjectId, object_type);
        sai_tables[serializedObjectId] = sai_table; 
    } else {
        sai_table = table_it->second;
    }
    m_sai_tables[table_def_it->second.table_type] = sai_tables;
    auto sai_entry = std::make_shared<SaiDBObject>(m_switch_db, object_type, id);
    sai_table->add_entry(sai_entry);
    return SAI_STATUS_SUCCESS;
}

sai_status_t 
SaiObjectDB::remove(
                _In_ sai_object_type_t object_type,
                _In_ const std::string& id)
{
    sai_status_t status;
    sai_attribute_t attr;
    auto sai_table_types_it = m_sai_tables.find(object_type);
    if (sai_table_types_it != m_sai_tables.end()) {
        auto sai_table_it = sai_table_types_it->second.find(id);
        if (sai_table_it != sai_table_types_it->second.end()) {
            sai_table_types_it->second.erase(sai_table_it);
        } else {
            SWSS_LOG_WARN("The object to be removed is not found in SaiObjectDB %s", id.c_str());
        }
        
        return SAI_STATUS_SUCCESS;
    }
    //The object can be a entry in a table
    auto table_def_it = sai_entry_to_table_defs.find(object_type);
    if (table_def_it == sai_entry_to_table_defs.end()) {
        // Not an interesting type
        return SAI_STATUS_SUCCESS;
    }

    //Get table type and OID
    attr.id = table_def_it->second.table_link_attr;
    //read the table_link_attr in entry object from switch_db
    status = m_switch_db->get(table_def_it->second.table_type, id, 1, &attr);
    if (status != SAI_STATUS_SUCCESS) {
        SWSS_LOG_WARN("The object to be removed is not found in switch_db %s", id.c_str());
        return SAI_STATUS_SUCCESS;
    }
     
    auto sai_tables = m_sai_tables[table_def_it->second.table_type];
    std::shared_ptr<SaiTable> sai_table;
    auto table_id = sai_serialize_object_id(attr.value.oid);
    auto table_it = sai_tables.find(table_id);
    if (table_it == sai_tables.end()) {
        SWSS_LOG_WARN("Table is not found in SaiObjectDB %s", table_id.c_str());
        return SAI_STATUS_SUCCESS;
    } else {
        sai_table = table_it->second;
    }
    //remove the entry from sai_table
    sai_table->remove_entry(id);
    return SAI_STATUS_SUCCESS;
}

std::shared_ptr<SaiObject> 
SaiObjectDB::get(
                _In_ sai_object_type_t object_type,
                _In_ const std::string& id)
{
    sai_status_t status;
    sai_attribute_t attr;
    //first check if the object is a SaiTable
    auto sai_table_types_it = m_sai_tables.find(object_type);
    if (sai_table_types_it != m_sai_tables.end()) {
        auto sai_table_it = sai_table_types_it->second.find(id);
        if (sai_table_it != sai_table_types_it->second.end()) {
            return sai_table_it->second;
        } else {
            SWSS_LOG_WARN("Table is not found in SaiObjectDB %s", id.c_str());
            return std::shared_ptr<SaiObject>();
        }
    }
    // check if the object exists
    status = m_switch_db->get(object_type, id, 0, &attr);
    if (status != SAI_STATUS_SUCCESS) {
        SWSS_LOG_WARN("Table is not found in SaiObjectDB %s", id.c_str());
        return std::shared_ptr<SaiObject>();
    }
    /*
     return a SaiObject as a wrapper to underlaying object is switch_db.
     the object may exists in a sai_table as an entry but we don't care if 
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