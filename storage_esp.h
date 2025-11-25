#pragma once

#include "interface/storage_interface.h"
#include "storage_config.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_littlefs.h"
#include <string>
#include <vector>
#include <memory>
#include <sys/stat.h>
#include <dirent.h>

#if STORAGE_ENABLE_MUTEX_PROTECTION
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#if STORAGE_ENABLE_VERSIONING
#include "file_versioning.h"
#endif

/**
 * @brief ESP32 Storage Driver Implementation
 * 
 * Native ESP-IDF storage driver supporting both SPIFFS and LittleFS
 * with configurable options through storage_config.h
 */
class storage_esp : public storage_interface 
{   
    public:
        // Constructors
        storage_esp();
        storage_esp(storage_type_t type, const std::string& partition = STORAGE_DEFAULT_PARTITION_LABEL);
        storage_esp(storage_type_t type, const std::string& partition, const std::string& mount_point);
        ~storage_esp();

        // ===== storage_interface implementation =====
        bool begin() override;
        bool read_file(const std::string& key, void* data, size_t data_size) override;
        bool write_file(const std::string& key, const void* data, size_t data_size) override;
        bool erase_file(const std::string& key) override;
        size_t file_size(const std::string& key) override;
        bool exists(const std::string& key) override;
        size_t total_size() override;
        size_t used_size() override;
        bool mount(bool format_on_fail = STORAGE_FORMAT_IF_MOUNT_FAILS) override;
        bool unmount() override;
        bool format() override;
        bool list_all_files(std::vector<file_info_t>& files) override;
        bool get_is_mounted() const override { return _is_mounted; }

        // ===== Advanced file operations =====
        bool read_file_alloc(const std::string& key, uint8_t** data, size_t* size);
        bool rename_file(const std::string& old_key, const std::string& new_key);

        // ===== Directory operations =====
        bool create_directory(const std::string& path);
        bool list_directory(const std::string& path, std::vector<file_info_t>& files);

        // ===== Utility functions =====
        bool verify_file_integrity(const std::string& key, size_t expected_size, uint32_t* checksum = nullptr);

        // ===== Getters =====
        storage_type_t get_storage_type() const { return _storage_type; }
        std::string get_base_path() const { return _base_path; }
        std::string get_partition_label() const { return _partition_label; }

    #if STORAGE_ENABLE_VERSIONING
        // ===== Versioning access =====
        file_versioning* get_versioning() { 
            _init_versioning();
            return _versioning.get(); 
        }
        const file_versioning* get_versioning() const { 
            return _versioning.get(); 
        }
    #endif

    private:
        storage_type_t _storage_type;
        std::string _base_path;
        std::string _partition_label;
        bool _is_mounted;

    #if STORAGE_ENABLE_VERSIONING
        std::unique_ptr<file_versioning> _versioning;
        void _init_versioning();
    #endif

    #if STORAGE_ENABLE_MUTEX_PROTECTION
        SemaphoreHandle_t _storage_mutex;

        class mutex_guard {
        public:
            explicit mutex_guard(SemaphoreHandle_t& mutex) : m_mutex(mutex) {
                xSemaphoreTake(m_mutex, STORAGE_MUTEX_TIMEOUT_MS);
            }
            ~mutex_guard() {
                xSemaphoreGive(m_mutex);
            }
        private:
            SemaphoreHandle_t& m_mutex;
        };
    #endif

        // ===== Internal helpers =====
        const char* _get_storage_type_name() const {
            return _storage_type == STORAGE_TYPE_SPIFFS ? "SPIFFS" : "LittleFS";
        }
        std::string _get_full_path(const std::string& relative_path) const;
        bool _create_directory_recursive(const std::string& path);
        void _init_default_config();

        // Internal raw file operations (used by versioning callbacks)
        bool _read_file_internal(const std::string& key, void* data, size_t data_size);
        bool _write_file_internal(const std::string& key, const void* data, size_t data_size);
        bool _write_file_no_mutex(const std::string& key, const void* data, size_t data_size);
        bool _read_file_no_mutex(const std::string& key, void* data, size_t data_size);
};