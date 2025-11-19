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
#include <sys/stat.h>
#include <dirent.h>

#if STORAGE_ENABLE_MUTEX_PROTECTION
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#if STORAGE_ENABLE_VERSIONING
struct file_version_info {
    uint32_t version;
    size_t size;
    bool is_current;
};
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
        // Default constructor using configuration defaults
        storage_esp();
        
        // Constructor with specific filesystem type and partition
        storage_esp(storage_type_t type, const std::string& partition = STORAGE_DEFAULT_PARTITION_LABEL);
        
        // Constructor with specific filesystem type, partition, and mount point
        storage_esp(storage_type_t type, const std::string& partition, const std::string& mount_point);
        
        ~storage_esp();

        // Implement storage_interface methods
        bool begin() override;
        bool read_file(const std::string& key, void* data, size_t data_size) override;
        bool write_file(const std::string& key, const void* data, size_t data_size) override;
        bool erase_file(const std::string& key) override;
        size_t file_size(const std::string& key) override;
        bool exists(const std::string& key) override;
        size_t total_size() override;
        size_t used_size() override;

        // Extended storage operations
        bool mount(bool format_on_fail = STORAGE_FORMAT_IF_MOUNT_FAILS) override;
        bool unmount() override;
        bool format() override;
        bool list_all_files(std::vector<file_info_t>& files) override;
        bool get_is_mounted() const override { return is_mounted; }

        // Advanced file operations
        bool read_file_alloc(const std::string& key, uint8_t** data, size_t* size);
        bool rename_file(const std::string& old_key, const std::string& new_key);

        // Directory operations
        bool create_directory(const std::string& path);
        bool list_directory(const std::string& path, std::vector<file_info_t>& files);
        
        // Utility functions
        bool verify_file_integrity(const std::string& key, size_t expected_size, uint32_t* checksum = nullptr);
        
        // Getters
        storage_type_t get_storage_type() const { return storage_type; }
        std::string get_base_path() const { return base_path; }
        std::string get_partition_label() const { return partition_label; }

# if STORAGE_ENABLE_VERSIONING
        // File versioning methods
        uint32_t get_file_version(const std::string& key);
        bool get_file_version_info(const std::string& key, file_version_info& info);
        std::vector<file_version_info> list_file_versions(const std::string& key);
        bool read_file_version(const std::string& key, uint32_t version, void* data, size_t data_size);
        bool file_has_changed(const std::string& key, uint32_t last_known_version);
        bool restore_file_version(const std::string& key, uint32_t version);
        uint32_t cleanup_old_versions(const std::string& key = "");
#endif
        
    private:
        storage_type_t storage_type;
        std::string base_path;
        std::string partition_label;
        bool is_mounted;
        
#if STORAGE_ENABLE_MUTEX_PROTECTION
        SemaphoreHandle_t storage_mutex;
        
        // RAII mutex guard for thread safety
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

#if STORAGE_ENABLE_VERSIONING
        struct file_version_metadata {
            uint32_t current_version;
            uint32_t file_size;
            uint32_t checksum;
            uint32_t version_count;
            uint32_t versions[STORAGE_MAX_VERSION_HISTORY];

        file_version_metadata() : current_version(0), file_size(0), checksum(0), version_count(0) {
            for (size_t i = 0; i < STORAGE_MAX_VERSION_HISTORY; i++) {
                versions[i] = 0;
            }
        }
    };

    std::string _get_metadata_path(const std::string& key) const;
    std::string _get_version_path(const std::string& key, uint32_t version) const;
    bool _load_metadata(const std::string& key, file_version_metadata& metadata);
    bool _save_metadata(const std::string& key, const file_version_metadata& metadata);
    uint32_t _calculate_crc32(const void* data, size_t length) const;
    bool _archive_current_version(const std::string& key, file_version_metadata& metadata);
    bool _cleanup_oldest_version(const std::string& key, file_version_metadata& metadata);
#endif
        
        // Internal helpers
        const char* get_storage_type_name() const {
            return storage_type == STORAGE_TYPE_SPIFFS ? "SPIFFS" : "LittleFS";
        }
        std::string get_full_path(const std::string& relative_path) const;
        bool create_directory_recursive(const std::string& path);
        void init_default_config();
};