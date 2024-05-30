#pragma once

#include <cstdint>

extern "C" {
#include "sai.h"
}
#include <unordered_map>
#include <vector>
namespace saivpp
{
    /*
      There is circular dependency between SaiObjectDB and SwitchStateBase. To remove it we need major surgery. For example, 
      move SaiObjectDB to SwitchState. When we need to read an object, such as get_linked_object, get it from m_objectHash in SwitchState
    */
    class SwitchStateBase;

    class SaiObject {
    public:
        SaiObject(SwitchStateBase* switch_db, sai_object_type_t type, const std::string& id) : m_switch_db(switch_db), m_type(type), m_id(id){}

        virtual ~SaiObject() = default;

        virtual sai_status_t get_attr(_Out_ sai_attribute_t &attr) const = 0;

        const std::string& get_id() const { return m_id; }

        sai_object_type_t get_type() const { return m_type; }

        std::shared_ptr<SaiObject> get_linked_object(_In_ sai_object_type_t linked_object_type, _In_ sai_attr_id_t link_attr_id) const;
        std::vector<std::shared_ptr<SaiObject>> get_linked_objects(_In_ sai_object_type_t linked_object_type, _In_ sai_attr_id_t link_attr_id) const;
    protected:
        SwitchStateBase* m_switch_db;
        sai_object_type_t m_type;    
        std::string m_id;
    };
    
    class SaiCachedObject : public SaiObject {
    public:
        SaiCachedObject(SwitchStateBase* switch_db, sai_object_type_t type, const std::string& id, uint32_t attr_count, const sai_attribute_t *attr_list) : 
            SaiObject(switch_db, type, id), m_attr_count(attr_count), m_attr_list(attr_list) {}
        ~SaiCachedObject() = default;

        sai_status_t get_attr(_Out_ sai_attribute_t &attr) const override;

    private:
        uint32_t m_attr_count;
        const sai_attribute_t *m_attr_list;
    };
    
    class SaiDBObject : public SaiObject {
    public:
        SaiDBObject(SwitchStateBase* switch_db, sai_object_type_t type, const std::string& id) : SaiObject(switch_db, type, id) {}
        ~SaiDBObject() = default;

        sai_status_t get_attr(_Out_ sai_attribute_t &attr) const override;
    };

    class SaiTable : public SaiDBObject {
    public:
        SaiTable(SwitchStateBase* switch_db, sai_object_type_t type, const std::string& id, sai_object_type_t entry_type)
            : SaiDBObject(switch_db, type, id), m_entry_type(entry_type) {}
        ~SaiTable() = default;
        
        const std::unordered_map<std::string, std::shared_ptr<SaiObject>>& get_entries() const {
            return m_entries;
        }

        void add_entry(const std::shared_ptr<SaiObject> entry) {
            if (entry) {
                m_entries[entry->get_id()] = entry;
            }
        }

        void remove_entry(const std::string& id) {
            m_entries.erase(id);
        }
    private:
        sai_object_type_t m_entry_type;
        std::unordered_map<std::string, std::shared_ptr<SaiObject>> m_entries;
    };
    
    class SaiObjectDB {
    public:
        SaiObjectDB(SwitchStateBase* switch_db) : m_switch_db(switch_db) {};
        ~SaiObjectDB() = default;
        sai_status_t add(
                    _In_ sai_object_type_t object_type,
                    _In_ const std::string& id,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list);
        
        sai_status_t remove(
                    _In_ sai_object_type_t object_type,
                    _In_ const std::string& id);
        
        std::shared_ptr<SaiObject> get(
                    _In_ sai_object_type_t object_type,
                    _In_ const std::string& id);
    private:
        SwitchStateBase* m_switch_db;
        std::unordered_map<sai_object_type_t, std::unordered_map<std::string, std::shared_ptr<SaiTable>>> m_sai_tables;
    };
}